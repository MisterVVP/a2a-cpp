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

  [[nodiscard]] bool ParseString(std::string_view* unescaped_contents = nullptr) noexcept {
    if (!Consume('"')) {
      return false;
    }
    const std::size_t contents_begin = position_;
    bool has_escape = false;
    while (position_ < input_.size()) {
      const unsigned char character = static_cast<unsigned char>(input_[position_++]);
      if (character == '"') {
        if (unescaped_contents != nullptr) {
          *unescaped_contents =
              has_escape ? std::string_view{} : input_.substr(contents_begin, position_ - contents_begin - 1U);
        }
        return true;
      }
      if (character < 0x20U) {
        return false;
      }
      if (character != '\\') {
        continue;
      }
      has_escape = true;
      if (position_ >= input_.size()) {
        return false;
      }
      const char escape = input_[position_++];
      if (escape == 'u') {
        for (int digit = 0; digit < 4; ++digit) {
          if (position_ >= input_.size() || std::isxdigit(static_cast<unsigned char>(input_[position_++])) == 0) {
            return false;
          }
        }
      } else if (std::string_view(R"("\/bfnrt)").find(escape) == std::string_view::npos) {
        return false;
      }
    }
    return false;
  }

  [[nodiscard]] bool ParseNumber() noexcept {
    const std::size_t begin = position_;
    if (position_ < input_.size() && input_[position_] == '-') {
      ++position_;
    }
    if (position_ >= input_.size()) {
      return false;
    }
    if (input_[position_] == '0') {
      ++position_;
    } else if (input_[position_] >= '1' && input_[position_] <= '9') {
      while (position_ < input_.size() && std::isdigit(static_cast<unsigned char>(input_[position_])) != 0) {
        ++position_;
      }
    } else {
      return false;
    }
    if (position_ < input_.size() && input_[position_] == '.') {
      ++position_;
      if (position_ >= input_.size() || std::isdigit(static_cast<unsigned char>(input_[position_])) == 0) {
        return false;
      }
      while (position_ < input_.size() && std::isdigit(static_cast<unsigned char>(input_[position_])) != 0) {
        ++position_;
      }
    }
    if (position_ < input_.size() && (input_[position_] == 'e' || input_[position_] == 'E')) {
      ++position_;
      if (position_ < input_.size() && (input_[position_] == '+' || input_[position_] == '-')) {
        ++position_;
      }
      if (position_ >= input_.size() || std::isdigit(static_cast<unsigned char>(input_[position_])) == 0) {
        return false;
      }
      while (position_ < input_.size() && std::isdigit(static_cast<unsigned char>(input_[position_])) != 0) {
        ++position_;
      }
    }
    return position_ > begin;
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
