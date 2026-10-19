// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "agent_card_json.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace a2a::core::internal {
namespace {
constexpr std::size_t kMaximumJsonSize = std::size_t{1024} * 1024U;
constexpr std::size_t kMaximumNestingDepth = 128;
constexpr int kSmallestDecimalExponent = -6;
constexpr int kLargestDecimalExponent = 21;
constexpr std::size_t kNumberBufferSize = 64;
constexpr std::string_view kCanonicalZero = "0";
constexpr std::string_view kDecimalFractionPrefix = "0.";

constexpr std::uint32_t kUtf8FourBytePrefix = 0xF0U;
constexpr std::uint32_t kUtf8FourByteMask = 0x07U;
constexpr std::uint32_t kUtf8ThreeBytePrefix = 0xE0U;
constexpr std::uint32_t kUtf8ThreeByteMask = 0x0FU;
constexpr std::uint32_t kUtf8TwoBytePrefix = 0xC0U;
constexpr std::uint32_t kUtf8TwoByteMask = 0x1FU;
constexpr unsigned kUtf8ContinuationBits = 6U;
constexpr std::uint32_t kUtf8ContinuationMask = 0x3FU;
constexpr std::uint32_t kMaximumBmpCodePoint = 0xFFFFU;
constexpr std::uint32_t kSupplementaryCodePointOffset = 0x10000U;
constexpr std::uint32_t kHighSurrogateStart = 0xD800U;
constexpr unsigned kSurrogateBits = 10U;
constexpr std::uint32_t kLowSurrogateStart = 0xDC00U;
constexpr std::uint32_t kSurrogateMask = 0x3FFU;

std::u16string Utf16(std::string_view text) {
  std::u16string result;
  for (std::size_t index = 0; index < text.size();) {
    auto code = static_cast<std::uint32_t>(static_cast<unsigned char>(text[index++]));
    unsigned remaining = 0;
    if (code >= kUtf8FourBytePrefix) {
      remaining = 3;
      code &= kUtf8FourByteMask;
    } else if (code >= kUtf8ThreeBytePrefix) {
      remaining = 2;
      code &= kUtf8ThreeByteMask;
    } else if (code >= kUtf8TwoBytePrefix) {
      remaining = 1;
      code &= kUtf8TwoByteMask;
    }
    for (unsigned continuation = 0; continuation < remaining; ++continuation) {
      code = (code << kUtf8ContinuationBits) | (static_cast<unsigned char>(text.at(index++)) & kUtf8ContinuationMask);
    }
    if (code > kMaximumBmpCodePoint) {
      code -= kSupplementaryCodePointOffset;
      result.push_back(static_cast<char16_t>(kHighSurrogateStart + (code >> kSurrogateBits)));
      result.push_back(static_cast<char16_t>(kLowSurrogateStart + (code & kSurrogateMask)));
    } else {
      result.push_back(static_cast<char16_t>(code));
    }
  }
  return result;
}

std::string Number(double value) {
  if (!std::isfinite(value)) {
    throw std::invalid_argument(std::string(kInvalidSigningJson));
  }
  if (value == 0) {
    return std::string(kCanonicalZero);
  }
  std::array<char, kNumberBufferSize> buffer{};
  auto converted = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value, std::chars_format::scientific);
  if (converted.ec != std::errc()) {
    throw std::invalid_argument(std::string(kInvalidSigningJson));
  }
  const std::string_view scientific(buffer.data(), static_cast<std::size_t>(converted.ptr - buffer.data()));
  const auto exponent_at = scientific.find('e');
  auto exponent_text = scientific.substr(exponent_at + 1);
  if (exponent_text.starts_with('+')) {
    exponent_text.remove_prefix(1);
  }
  int exponent = 0;
  const auto parsed = std::from_chars(exponent_text.data(), exponent_text.data() + exponent_text.size(), exponent);
  if (parsed.ec != std::errc()) {
    throw std::invalid_argument(std::string(kInvalidSigningJson));
  }
  std::string digits(scientific.substr(0, exponent_at));
  std::string result;
  if (digits.starts_with('-')) {
    result.push_back('-');
    digits.erase(0, 1);
  }
  std::erase(digits, '.');
  if (exponent < kSmallestDecimalExponent || exponent >= kLargestDecimalExponent) {
    result.push_back(digits.front());
    if (digits.size() > 1) {
      result.push_back('.');
      result.append(digits.substr(1));
    }
    result.push_back('e');
    if (exponent >= 0) {
      result.push_back('+');
    }
    result.append(std::to_string(exponent));
  } else if (exponent < 0) {
    result.append(kDecimalFractionPrefix);
    result.append(static_cast<std::size_t>(-exponent) - 1, '0');
    result.append(digits);
  } else {
    const auto decimal_at = static_cast<std::size_t>(exponent) + 1;
    if (decimal_at >= digits.size()) {
      result.append(digits);
      result.append(decimal_at - digits.size(), '0');
    } else {
      result.append(digits.substr(0, decimal_at));
      result.push_back('.');
      result.append(digits.substr(decimal_at));
    }
  }
  return result;
}

constexpr std::string_view kCanonicalNull = "null";
constexpr std::string_view kCanonicalTrue = "true";
constexpr std::string_view kCanonicalFalse = "false";
constexpr std::string_view kHexDigits = "0123456789abcdef";
constexpr unsigned char kFirstPrintableCharacter = 0x20U;
constexpr unsigned kHexDigitBits = 4U;
constexpr unsigned char kHexDigitMask = 0x0FU;

void AppendString(std::string_view text, std::string& output) {
  output.push_back('"');
  for (const char character : text) {
    switch (character) {
      case '"':
        output.append(R"(\")");
        break;
      case '\\':
        output.append(R"(\\)");
        break;
      case '\b':
        output.append(R"(\b)");
        break;
      case '\f':
        output.append(R"(\f)");
        break;
      case '\n':
        output.append(R"(\n)");
        break;
      case '\r':
        output.append(R"(\r)");
        break;
      case '\t':
        output.append(R"(\t)");
        break;
      default: {
        const auto byte = static_cast<unsigned char>(character);
        if (byte < kFirstPrintableCharacter) {
          output.append(R"(\u00)");
          output.push_back(kHexDigits[byte >> kHexDigitBits]);
          output.push_back(kHexDigits[byte & kHexDigitMask]);
        } else {
          output.push_back(character);
        }
      }
    }
  }
  output.push_back('"');
}

// simdjson validates syntax and Unicode. JCS additionally requires binary64
// number formatting, UTF-16 member ordering and duplicate-name rejection.
// On-Demand's get_double also accepts integer tokens outside the 64-bit range.
template <typename Value>
void AppendCanonical(Value& value, std::string& output, std::size_t depth, std::string_view excluded_member = {});

struct CanonicalMember {
  std::u16string sort_key;
  std::string_view name;
  std::string value;
};

void AppendObject(simdjson::ondemand::object object, std::string& output, std::size_t depth,
                  std::string_view excluded_member) {
  std::vector<CanonicalMember> members;
  for (auto field_result : object) {
    auto field = ReadJson(std::move(field_result));
    const auto name = ReadJson(field.unescaped_key());
    auto value = field.value();
    CanonicalMember member{.sort_key = Utf16(name), .name = name, .value = {}};
    // Consume and validate excluded signatures too, including duplicate names.
    AppendCanonical(value, member.value, depth + 1);
    members.push_back(std::move(member));
  }
  std::ranges::sort(members, {}, &CanonicalMember::sort_key);
  output.push_back('{');
  bool first = true;
  for (std::size_t index = 0; index < members.size(); ++index) {
    const auto& member = members[index];
    if (index != 0 && member.name == members[index - 1].name) {
      throw std::invalid_argument(std::string(kInvalidSigningJson));
    }
    if (!excluded_member.empty() && member.name == excluded_member) {
      continue;
    }
    if (!first) {
      output.push_back(',');
    }
    first = false;
    AppendString(member.name, output);
    output.push_back(':');
    output.append(member.value);
  }
  output.push_back('}');
}

template <typename Value>
void AppendCanonical(Value& value, std::string& output, std::size_t depth, std::string_view excluded_member) {
  using Type = simdjson::ondemand::json_type;
  const auto type = ReadJson(value.type());
  if ((type == Type::object || type == Type::array) && depth >= kMaximumNestingDepth) {
    throw std::invalid_argument(std::string(kInvalidSigningJson));
  }
  switch (type) {
    case Type::number:
      output.append(Number(ReadJson(value.get_double())));
      break;
    case Type::string:
      AppendString(ReadJson(value.get_string()), output);
      break;
    case Type::boolean:
      output.append(ReadJson(value.get_bool()) ? kCanonicalTrue : kCanonicalFalse);
      break;
    case Type::null:
      if (!ReadJson(value.is_null())) {
        throw std::invalid_argument(std::string(kInvalidSigningJson));
      }
      output.append(kCanonicalNull);
      break;
    case Type::object:
      AppendObject(ReadJson(value.get_object()), output, depth, excluded_member);
      break;
    case Type::array: {
      output.push_back('[');
      bool first = true;
      for (auto item_result : ReadJson(value.get_array())) {
        auto item = ReadJson(std::move(item_result));
        if (!first) {
          output.push_back(',');
        }
        first = false;
        AppendCanonical(item, output, depth + 1);
      }
      output.push_back(']');
      break;
    }
  }
}
}  // namespace

SigningJson::SigningJson(std::string_view input, std::string_view excluded_root_member) {
  if (input.size() > kMaximumJsonSize ||
      parser_.allocate(input.size(), kMaximumNestingDepth + 1) != simdjson::SUCCESS) {
    throw std::invalid_argument(std::string(kInvalidSigningJson));
  }
  input_ = simdjson::padded_string(input);
  root_ = ReadJson(parser_.iterate(input_));
  canonical_.reserve(input.size());
  AppendCanonical(root_, canonical_, 0, excluded_root_member);
  // Fully consuming the root must leave no remaining structural token.
  if (root_.current_location().error() != simdjson::OUT_OF_BOUNDS) {
    throw std::invalid_argument(std::string(kInvalidSigningJson));
  }
  root_.rewind();
}
}  // namespace a2a::core::internal
