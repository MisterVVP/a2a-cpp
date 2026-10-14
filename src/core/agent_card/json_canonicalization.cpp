// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "json_canonicalization.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <vector>

#include "a2a/core/error.h"
#include "a2a/core/json_canonicalization.h"
#include "a2a/core/json_value.h"
#include "a2a/core/protojson.h"

namespace a2a::core::internal {
namespace {
constexpr std::size_t kMaximumJsonSize = std::size_t{1024} * 1024U;
constexpr int kSmallestDecimalExponent = -6;
constexpr int kLargestDecimalExponent = 21;
constexpr std::size_t kNumberBufferSize = 64;
constexpr std::string_view kCanonicalZero = "0";
constexpr std::string_view kDecimalFractionPrefix = "0.";
constexpr std::string_view kCanonicalizationFailure = "invalid JSON for RFC 8785 canonicalization";
constexpr std::string_view kInvalidSigningJson = "invalid signing JSON";

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

std::vector<std::uint16_t> Utf16(std::string_view text) {
  std::vector<std::uint16_t> result;
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
      result.push_back(static_cast<std::uint16_t>(kHighSurrogateStart + (code >> kSurrogateBits)));
      result.push_back(static_cast<std::uint16_t>(kLowSurrogateStart + (code & kSurrogateMask)));
    } else {
      result.push_back(static_cast<std::uint16_t>(code));
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

constexpr std::string_view kHexDigits = "0123456789abcdef";
constexpr unsigned char kFirstPrintableCharacter = 0x20U;
constexpr unsigned kHexDigitBits = 4U;
constexpr unsigned char kHexDigitMask = 0x0FU;
constexpr std::string_view kCanonicalNull = "null";
constexpr std::string_view kCanonicalTrue = "true";
constexpr std::string_view kCanonicalFalse = "false";

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

void AppendCanonical(const google::protobuf::Value& value, std::string& output);

void AppendObject(const google::protobuf::Struct& object, std::string& output) {
  std::vector<std::pair<std::vector<std::uint16_t>, std::string>> keys;
  keys.reserve(object.fields().size());
  for (const auto& [key, value] : object.fields()) {
    (void)value;
    keys.emplace_back(Utf16(key), key);
  }
  std::ranges::sort(keys);
  output.push_back('{');
  bool first = true;
  for (const auto& [sort_key, key] : keys) {
    (void)sort_key;
    if (!first) {
      output.push_back(',');
    }
    first = false;
    AppendString(key, output);
    output.push_back(':');
    AppendCanonical(object.fields().at(key), output);
  }
  output.push_back('}');
}

void AppendCanonical(const google::protobuf::Value& value, std::string& output) {
  using Value = google::protobuf::Value;
  switch (value.kind_case()) {
    case Value::kNumberValue:
      output.append(Number(value.number_value()));
      break;
    case Value::kStringValue:
      AppendString(value.string_value(), output);
      break;
    case Value::kBoolValue:
      output.append(value.bool_value() ? kCanonicalTrue : kCanonicalFalse);
      break;
    case Value::kNullValue:
      output.append(kCanonicalNull);
      break;
    case Value::kStructValue:
      AppendObject(value.struct_value(), output);
      break;
    case Value::kListValue: {
      output.push_back('[');
      bool first = true;
      for (const auto& item : value.list_value().values()) {
        if (!first) {
          output.push_back(',');
        }
        first = false;
        AppendCanonical(item, output);
      }
      output.push_back(']');
      break;
    }
    default:
      throw std::invalid_argument(std::string(kInvalidSigningJson));
  }
}
}  // namespace

google::protobuf::Value ParseSigningJson(std::string_view input) {
  if (input.size() > kMaximumJsonSize || !json::HasStrictJsonSyntax(input)) {
    throw std::invalid_argument(std::string(kInvalidSigningJson));
  }
  google::protobuf::Value value;
  // Protobuf's generic JSON parser retains unknown fields and rejects duplicate
  // decoded member names and malformed Unicode. It does not parse an AgentCard.
  if (!JsonToMessage(input, &value).ok()) {
    throw std::invalid_argument(std::string(kInvalidSigningJson));
  }
  return value;
}
std::string Canonicalize(const google::protobuf::Value& value) {
  std::string result;
  AppendCanonical(value, result);
  return result;
}
}  // namespace a2a::core::internal

namespace a2a::core {
Result<std::string> CanonicalizeJson(std::string_view json) {
  try {
    return internal::Canonicalize(internal::ParseSigningJson(json));
  } catch (const std::exception&) {
    return Error::Validation(std::string(internal::kCanonicalizationFailure));
  }
}
}  // namespace a2a::core
