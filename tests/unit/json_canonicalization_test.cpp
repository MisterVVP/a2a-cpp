// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "a2a/core/json_canonicalization.h"

#include <google/protobuf/struct.pb.h>
#include <gtest/gtest.h>

#include <array>
#include <fstream>
#include <iterator>
#include <string>

#include "a2a/core/protojson.h"

namespace {
constexpr std::string_view kCanonicalInput =
    R"({"numbers":[333333333.33333329,1E30,4.50,2e-3,1e-27],"string":"€$\u000f\nA'B\"\\\"/"})";
constexpr std::string_view kCanonicalOutput =
    R"({"numbers":[333333333.3333333,1e+30,4.5,0.002,1e-27],"string":"€$\u000f\nA'B\"\\\"/"})";
constexpr std::string_view kThresholdInput = R"([-0.0,1e-6,1e-7,1e20,1e21,9007199254740992])";
constexpr std::string_view kThresholdOutput = R"([0,0.000001,1e-7,100000000000000000000,1e+21,9007199254740992])";
constexpr std::string_view kUnicodeInput = R"({"דּ":1,"😀":2,"€":3,"\r":4,"1":5,"ö":6,"\u0080":7})";
constexpr std::string_view kEscapedControl = R"(\u0080)";
constexpr std::string_view kUtf8Control = "\xc2\x80";
constexpr std::string_view kUnicodeOutput = R"({"\r":4,"1":5,"\u0080":7,"ö":6,"€":3,"😀":2,"דּ":1})";
constexpr std::string_view kDuplicateKeys = R"({"key":1,"\u006bey":2})";
constexpr std::string_view kLoneSurrogate = R"({"key":"\ud800"})";
TEST(JsonCanonicalizationTest, Rfc8785NumberAndStringVectors) {
  auto canonical = a2a::core::CanonicalizeJson(kCanonicalInput);
  ASSERT_TRUE(canonical.ok());
  EXPECT_EQ(canonical.value(), kCanonicalOutput);
  auto thresholds = a2a::core::CanonicalizeJson(kThresholdInput);
  ASSERT_TRUE(thresholds.ok());
  EXPECT_EQ(thresholds.value(), kThresholdOutput);
}
TEST(JsonCanonicalizationTest, SortsUtf16AndRejectsDuplicatesAndInvalidUnicode) {
  auto canonical = a2a::core::CanonicalizeJson(kUnicodeInput);
  ASSERT_TRUE(canonical.ok());
  // JCS emits U+0080 as UTF-8, not an escaped sequence.
  std::string expected(kUnicodeOutput);
  const auto escape = expected.find(kEscapedControl);
  expected.replace(escape, kEscapedControl.size(), kUtf8Control);
  EXPECT_EQ(canonical.value(), expected);
  EXPECT_FALSE(a2a::core::CanonicalizeJson(kDuplicateKeys).ok());
  EXPECT_FALSE(a2a::core::CanonicalizeJson(kLoneSurrogate).ok());
}

constexpr std::string_view kVectors = "vectors";
constexpr std::string_view kVectorId = "id";
constexpr std::string_view kInput = "input";
constexpr std::string_view kRawInput = "input_raw";
constexpr std::string_view kDisposition = "disposition";
constexpr std::string_view kMustReject = "MUST-REJECT";
constexpr std::string_view kCanonicalHex = "canonical_utf8_hex";
constexpr std::string_view kSignatures = "signatures";
constexpr std::string_view kHexDigits = "0123456789abcdef";
constexpr unsigned kHexDigitBits = 4;
constexpr unsigned char kHexDigitMask = 0x0fU;
constexpr int kCorpusSize = 57;
constexpr std::array<std::string_view, 6> kNonstandardJson{R"([1,])",    R"({"a":1,})", R"({"a":"\v"})",
                                                           R"({'a':1})", R"([01])",     R"([1.] )"};

std::string Hex(std::string_view value) {
  std::string result;
  result.reserve(value.size() * 2U);
  for (const char character : value) {
    const auto byte = static_cast<unsigned char>(character);
    result.push_back(kHexDigits[byte >> kHexDigitBits]);
    result.push_back(kHexDigits[byte & kHexDigitMask]);
  }
  return result;
}

void CheckRejectedVector(std::string_view raw) {
  google::protobuf::Value input;
  if (a2a::core::JsonToMessage(raw, &input).ok() && input.kind_case() == google::protobuf::Value::kStructValue &&
      input.struct_value().fields().contains(std::string(kSignatures))) {
    // Two corpus cases reject claimed AgentCard canonical output containing
    // signatures. They are valid generic JSON, so check exclusion separately.
    input.mutable_struct_value()->mutable_fields()->erase(std::string(kSignatures));
    auto unsigned_json = a2a::core::MessageToJson(input);
    ASSERT_TRUE(unsigned_json.ok());
    auto canonical = a2a::core::CanonicalizeJson(unsigned_json.value());
    ASSERT_TRUE(canonical.ok());
    EXPECT_NE(canonical.value(), raw);
    return;
  }
  EXPECT_FALSE(a2a::core::CanonicalizeJson(raw).ok());
}

void CheckVector(const google::protobuf::Struct& vector) {
  const auto& fields = vector.fields();
  SCOPED_TRACE(fields.at(std::string(kVectorId)).string_value());
  if (fields.at(std::string(kDisposition)).string_value() == kMustReject) {
    const auto raw = fields.find(std::string(kRawInput));
    ASSERT_NE(raw, fields.end());
    CheckRejectedVector(raw->second.string_value());
    return;
  }
  auto input = fields.at(std::string(kInput));
  if (input.kind_case() == google::protobuf::Value::kStructValue) {
    input.mutable_struct_value()->mutable_fields()->erase(std::string(kSignatures));
  }
  auto json = a2a::core::MessageToJson(input);
  ASSERT_TRUE(json.ok());
  auto canonical = a2a::core::CanonicalizeJson(json.value());
  ASSERT_TRUE(canonical.ok());
  EXPECT_EQ(Hex(canonical.value()), fields.at(std::string(kCanonicalHex)).string_value());
}

TEST(JsonCanonicalizationTest, IndependentRfc8785Corpus) {
  std::ifstream file(A2A_JCS_VECTOR_FILE);
  ASSERT_TRUE(file.is_open());
  const std::string json{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
  google::protobuf::Struct corpus;
  ASSERT_TRUE(a2a::core::JsonToMessage(json, &corpus).ok());
  const auto& vectors = corpus.fields().at(std::string(kVectors)).list_value().values();
  ASSERT_EQ(vectors.size(), kCorpusSize);
  for (const auto& vector : vectors) {
    CheckVector(vector.struct_value());
  }
}

TEST(JsonCanonicalizationTest, RejectsPermissiveParserExtensions) {
  for (const auto input : kNonstandardJson) {
    SCOPED_TRACE(input);
    EXPECT_FALSE(a2a::core::CanonicalizeJson(input).ok());
  }
}

}  // namespace
