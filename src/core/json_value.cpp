// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "a2a/core/json_value.h"

#include <array>
#include <cstddef>
#include <cstring>
#include <string_view>

namespace a2a::core::json {
namespace {

constexpr std::size_t kMaximumNestingDepth = 128U;

struct ScannedMember final {
  const char* value_begin;
  const char* value_end;
  bool name_matches;
};

[[nodiscard]] bool IsJsonWhitespace(char value) noexcept {
  return value == ' ' || value == '\t' || value == '\n' || value == '\r';
}

void SkipWhitespace(const char*& current, const char* end) noexcept {
  while (current != end && IsJsonWhitespace(*current)) {
    ++current;
  }
}

// String contents are deliberately not decoded here.
//
// memchr() is normally implemented using optimized libc routines and can skip
// ordinary string contents substantially faster than examining every byte.
// We only need to locate an unescaped closing quote. Full JSON escape
// validation is performed by the caller's JSON parser.
[[nodiscard]] bool SkipQuotedString(const char*& current, const char* end) noexcept {
  if (current == end || *current != '"') {
    return false;
  }

  const char* const contents_begin = ++current;

  while (current != end) {
    const auto remaining = static_cast<std::size_t>(end - current);
    const auto* quote = static_cast<const char*>(std::memchr(current, '"', remaining));

    if (quote == nullptr) {
      return false;
    }

    // A quote closes the string when it is preceded by an even number of
    // consecutive backslashes.
    const char* escape_begin = quote;
    while (escape_begin != contents_begin && escape_begin[-1] == '\\') {
      --escape_begin;
    }

    current = quote + 1;

    const auto escape_count = static_cast<std::size_t>(quote - escape_begin);
    if ((escape_count & 1U) == 0U) {
      return true;
    }
  }

  return false;
}

[[nodiscard]] char ClosingDelimiter(char opening) noexcept { return opening == '{' ? '}' : ']'; }

// Scans an object or array structurally.
//
// Parsing scalar grammar inside a composite value is intentionally avoided.
// The caller parses the extracted JSON value afterward, so doing that work here
// would duplicate the hot-path JSON parse.
[[nodiscard]] bool ScanCompositeValue(const char*& current, const char* end) noexcept {
  if (current == end || (*current != '{' && *current != '[')) {
    return false;
  }

  // Default initialization deliberately avoids clearing the array. Every entry
  // is written before it is read.
  std::array<char, kMaximumNestingDepth> closing_delimiters;
  std::size_t depth = 0U;

  closing_delimiters[depth++] = ClosingDelimiter(*current++);

  while (current != end) {
    const char value = *current++;

    if (value == '"') {
      --current;
      if (!SkipQuotedString(current, end)) {
        return false;
      }
      continue;
    }

    if (value == '{' || value == '[') {
      if (depth == closing_delimiters.size()) {
        return false;
      }

      closing_delimiters[depth++] = ClosingDelimiter(value);
      continue;
    }

    if (value != '}' && value != ']') {
      continue;
    }

    if (depth == 0U || value != closing_delimiters[depth - 1U]) {
      return false;
    }

    --depth;
    if (depth == 0U) {
      return true;
    }
  }

  return false;
}

[[nodiscard]] bool IsDigit(char value) noexcept { return value >= '0' && value <= '9'; }

[[nodiscard]] bool IsNonZeroDigit(char value) noexcept { return value >= '1' && value <= '9'; }

void SkipDigits(const char*& current, const char* end) noexcept {
  while (current != end && IsDigit(*current)) {
    ++current;
  }
}

// Outer-level scalars are inexpensive to validate exactly. Doing so also keeps
// malformed values such as `tru` and `01` from being mistaken for a complete
// top-level member.
[[nodiscard]] bool ScanNumber(const char*& current, const char* end) noexcept {
  if (current != end && *current == '-') {
    ++current;
  }

  if (current == end) {
    return false;
  }

  if (*current == '0') {
    ++current;
  } else if (IsNonZeroDigit(*current)) {
    SkipDigits(current, end);
  } else {
    return false;
  }

  if (current != end && *current == '.') {
    ++current;

    if (current == end || !IsDigit(*current)) {
      return false;
    }

    SkipDigits(current, end);
  }

  if (current == end || (*current != 'e' && *current != 'E')) {
    return true;
  }

  ++current;

  if (current != end && (*current == '+' || *current == '-')) {
    ++current;
  }

  if (current == end || !IsDigit(*current)) {
    return false;
  }

  SkipDigits(current, end);
  return true;
}

[[nodiscard]] bool ScanLiteral(const char*& current, const char* end, std::string_view literal) noexcept {
  const auto remaining = static_cast<std::size_t>(end - current);

  if (remaining < literal.size() || std::memcmp(current, literal.data(), literal.size()) != 0) {
    return false;
  }

  current += literal.size();
  return true;
}

[[nodiscard]] bool ScanValue(const char*& current, const char* end) noexcept {
  if (current == end) {
    return false;
  }

  switch (*current) {
    case '{':
    case '[':
      return ScanCompositeValue(current, end);

    case '"':
      return SkipQuotedString(current, end);

    case 't':
      return ScanLiteral(current, end, "true");

    case 'f':
      return ScanLiteral(current, end, "false");

    case 'n':
      return ScanLiteral(current, end, "null");

    default:
      return ScanNumber(current, end);
  }
}

[[nodiscard]] bool MemberNameMatches(const char* begin, const char* end, std::string_view member_name) noexcept {
  const auto length = static_cast<std::size_t>(end - begin);

  if (length != member_name.size()) {
    return false;
  }

  // Preserve the existing behavior: escaped member spellings such as
  // "re\u0073ult" are not considered a direct match for "result".
  if (std::memchr(begin, '\\', length) != nullptr) {
    return false;
  }

  return length == 0U || std::memcmp(begin, member_name.data(), length) == 0;
}

[[nodiscard]] bool ScanMember(const char*& current, const char* end, std::string_view member_name,
                              ScannedMember* member) noexcept {
  if (current == end || *current != '"') {
    return false;
  }

  const char* const name_begin = current + 1;

  if (!SkipQuotedString(current, end)) {
    return false;
  }

  const char* const name_end = current - 1;

  SkipWhitespace(current, end);

  if (current == end || *current != ':') {
    return false;
  }

  ++current;
  SkipWhitespace(current, end);

  const char* const value_begin = current;

  if (!ScanValue(current, end)) {
    return false;
  }

  *member = ScannedMember{
      .value_begin = value_begin,
      .value_end = current,
      .name_matches = MemberNameMatches(name_begin, name_end, member_name),
  };

  return true;
}

[[nodiscard]] std::optional<ValueRange> FinishDocument(const char*& current, const char* end, bool found,
                                                       ValueRange result) noexcept {
  ++current;
  SkipWhitespace(current, end);

  if (current != end || !found) {
    return std::nullopt;
  }

  return result;
}

}  // namespace

std::optional<ValueRange> FindTopLevelObjectMemberValue(std::string_view json, std::string_view member_name) noexcept {
  const char* const begin = json.data();
  const char* const end = begin + json.size();
  const char* current = begin;

  SkipWhitespace(current, end);

  if (current == end || *current != '{') {
    return std::nullopt;
  }

  ++current;
  SkipWhitespace(current, end);

  bool found = false;
  ValueRange result{};

  if (current != end && *current == '}') {
    return FinishDocument(current, end, found, result);
  }

  while (current != end) {
    ScannedMember member{};

    if (!ScanMember(current, end, member_name, &member)) {
      return std::nullopt;
    }

    if (member.name_matches) {
      if (found) {
        return std::nullopt;
      }

      found = true;
      result = ValueRange{
          .begin = static_cast<std::size_t>(member.value_begin - begin),
          .end = static_cast<std::size_t>(member.value_end - begin),
      };
    }

    SkipWhitespace(current, end);

    if (current == end) {
      return std::nullopt;
    }

    if (*current == '}') {
      return FinishDocument(current, end, found, result);
    }

    if (*current != ',') {
      return std::nullopt;
    }

    ++current;
    SkipWhitespace(current, end);
  }

  return std::nullopt;
}

}  // namespace a2a::core::json