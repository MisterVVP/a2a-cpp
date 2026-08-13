// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "a2a/core/json_value.h"

#include <cctype>
#include <cstddef>
#include <optional>
#include <string_view>

namespace a2a::core::json {
namespace {

constexpr std::size_t kMaximumNestingDepth = 128U;
constexpr unsigned char kFirstNonControlCharacter = 0x20U;
constexpr std::size_t kUnicodeEscapeHexDigitCount = 4U;
constexpr std::string_view kSimpleEscapeCharacters = R"("\/bfnrt)";

class Parser final {
 public:
  Parser(std::string_view input, std::string_view target) noexcept : input_(input), target_(target) {}

  [[nodiscard]] std::optional<ValueRange> Find() noexcept {
    SkipWhitespace();
    if (!ParseObject(0U, true)) {
      return std::nullopt;
    }
    SkipWhitespace();
    return position_ == input_.size() && !duplicate_ ? found_ : std::nullopt;
  }

 private:
  void SkipWhitespace() noexcept {
    while (position_ < input_.size()) {
      const char value = input_[position_];
      if (value != ' ' && value != '\t' && value != '\n' && value != '\r') {
        break;
      }
      ++position_;
    }
  }

  [[nodiscard]] bool Consume(char expected) noexcept {
    if (position_ >= input_.size() || input_[position_] != expected) {
      return false;
    }
    ++position_;
    return true;
  }

  [[nodiscard]] bool IsAtEnd() const noexcept { return position_ >= input_.size(); }

  [[nodiscard]] static bool IsDigit(char value) noexcept {
    return std::isdigit(static_cast<unsigned char>(value)) != 0;
  }

  [[nodiscard]] static bool IsNonZeroDigit(char value) noexcept { return value >= '1' && value <= '9'; }

  void ConsumeDigits() noexcept {
    while (!IsAtEnd() && IsDigit(input_[position_])) {
      ++position_;
    }
  }

  [[nodiscard]] bool ConsumeRequiredDigits() noexcept {
    if (IsAtEnd() || !IsDigit(input_[position_])) {
      return false;
    }
    ConsumeDigits();
    return true;
  }

  [[nodiscard]] bool ConsumeUnicodeEscape() noexcept {
    for (std::size_t digit = 0U; digit < kUnicodeEscapeHexDigitCount; ++digit) {
      if (IsAtEnd()) {
        return false;
      }
      const auto character = static_cast<unsigned char>(input_[position_]);
      ++position_;
      if (std::isxdigit(character) == 0) {
        return false;
      }
    }
    return true;
  }

  [[nodiscard]] bool ConsumeEscapeSequence() noexcept {
    if (IsAtEnd()) {
      return false;
    }
    const char escape = input_[position_++];
    if (escape == 'u') {
      return ConsumeUnicodeEscape();
    }
    return kSimpleEscapeCharacters.find(escape) != std::string_view::npos;
  }

  void SetUnescapedContents(std::size_t contents_begin, bool has_escape,
                            std::string_view* unescaped_contents) const noexcept {
    if (unescaped_contents == nullptr) {
      return;
    }
    if (has_escape) {
      *unescaped_contents = {};
      return;
    }
    *unescaped_contents = input_.substr(contents_begin, position_ - contents_begin - 1U);
  }

  [[nodiscard]] bool ParseString(std::string_view* unescaped_contents = nullptr) noexcept {
    if (!Consume('"')) {
      return false;
    }
    const std::size_t contents_begin = position_;
    bool has_escape = false;
    while (!IsAtEnd()) {
      const auto character = static_cast<unsigned char>(input_[position_]);
      ++position_;
      if (character == '"') {
        SetUnescapedContents(contents_begin, has_escape, unescaped_contents);
        return true;
      }
      if (character < kFirstNonControlCharacter) {
        return false;
      }
      if (character != '\\') {
        continue;
      }
      has_escape = true;
      if (!ConsumeEscapeSequence()) {
        return false;
      }
    }
    return false;
  }

  [[nodiscard]] bool ParseIntegerPart() noexcept {
    if (IsAtEnd()) {
      return false;
    }
    if (Consume('0')) {
      return true;
    }
    if (!IsNonZeroDigit(input_[position_])) {
      return false;
    }
    ConsumeDigits();
    return true;
  }

  [[nodiscard]] bool ParseFractionPart() noexcept {
    if (!Consume('.')) {
      return true;
    }
    return ConsumeRequiredDigits();
  }

  [[nodiscard]] bool ParseExponentPart() noexcept {
    if (IsAtEnd() || (input_[position_] != 'e' && input_[position_] != 'E')) {
      return true;
    }
    ++position_;
    if (!IsAtEnd() && (input_[position_] == '+' || input_[position_] == '-')) {
      ++position_;
    }
    return ConsumeRequiredDigits();
  }

  [[nodiscard]] bool ParseNumber() noexcept {
    (void)Consume('-');
    return ParseIntegerPart() && ParseFractionPart() && ParseExponentPart();
  }

  [[nodiscard]] bool ParseLiteral(std::string_view literal) noexcept {
    if (input_.substr(position_, literal.size()) != literal) {
      return false;
    }
    position_ += literal.size();
    return true;
  }

  [[nodiscard]] bool ParseValue(std::size_t depth) noexcept {
    if (depth > kMaximumNestingDepth) {
      return false;
    }
    SkipWhitespace();
    if (position_ >= input_.size()) {
      return false;
    }
    switch (input_[position_]) {
      case '{':
        return ParseObject(depth, false);
      case '[':
        return ParseArray(depth);
      case '"':
        return ParseString();
      case 't':
        return ParseLiteral("true");
      case 'f':
        return ParseLiteral("false");
      case 'n':
        return ParseLiteral("null");
      default:
        return ParseNumber();
    }
  }

  [[nodiscard]] bool ParseArray(std::size_t depth) noexcept {
    if (!Consume('[')) {
      return false;
    }
    SkipWhitespace();
    if (Consume(']')) {
      return true;
    }
    do {
      if (!ParseValue(depth + 1U)) {
        return false;
      }
      SkipWhitespace();
      if (Consume(']')) {
        return true;
      }
    } while (Consume(',') && (SkipWhitespace(), true));
    return false;
  }

  [[nodiscard]] bool ParseObject(std::size_t depth, bool top_level) noexcept {
    if (!Consume('{')) {
      return false;
    }
    SkipWhitespace();
    if (Consume('}')) {
      return true;
    }
    do {
      std::string_view name;
      if (!ParseString(&name)) {
        return false;
      }
      SkipWhitespace();
      if (!Consume(':')) {
        return false;
      }
      SkipWhitespace();
      const std::size_t value_begin = position_;
      if (!ParseValue(depth + 1U)) {
        return false;
      }
      if (top_level && name == target_) {
        if (found_.has_value()) {
          duplicate_ = true;
        }
        found_ = ValueRange{value_begin, position_};
      }
      SkipWhitespace();
      if (Consume('}')) {
        return true;
      }
    } while (Consume(',') && (SkipWhitespace(), true));
    return false;
  }

  std::string_view input_;
  std::string_view target_;
  std::size_t position_ = 0U;
  std::optional<ValueRange> found_;
  bool duplicate_ = false;
};

}  // namespace

std::optional<ValueRange> FindTopLevelObjectMemberValue(std::string_view json, std::string_view member_name) noexcept {
  return Parser(json, member_name).Find();
}

}  // namespace a2a::core::json
