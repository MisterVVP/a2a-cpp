// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "a2a/core/json_value.h"

#include <gtest/gtest.h>

#include <array>
#include <string>
#include <string_view>

namespace {

constexpr std::string_view kTarget = "result";

struct RangeCase final {
  std::string_view document;
  std::string_view expected;
};

constexpr std::array<RangeCase, 16> kValidCases{{
    {.document = R"({"result":{},"other":1})", .expected = "{}"},
    {.document = R"({"first":1,"result":[1,2],"last":3})", .expected = "[1,2]"},
    {.document = R"({"first":1,"result":null})", .expected = "null"},
    {.document = " { \"result\" \t : \n true } ", .expected = "true"},
    {.document = R"({"nested":{"result":"wrong"},"result":{"value":1}})", .expected = R"({"value":1})"},
    {.document = R"({"items":[{"result":"wrong"}],"result":[]})", .expected = "[]"},
    {.document = R"({"text":"result: [{}],{}","result":"value,:[{}]"})", .expected = R"("value,:[{}]")"},
    {.document = R"({"result":"escaped \" quote"})", .expected = R"("escaped \" quote")"},
    {.document = R"({"result":"escaped \\ backslash"})", .expected = R"("escaped \\ backslash")"},
    {.document = R"({"result":"text"})", .expected = R"("text")"},
    {.document = R"({"result":-12.5e+2})", .expected = "-12.5e+2"},
    {.document = R"({"result":false})", .expected = "false"},
    {.document = R"({"result":[]})", .expected = "[]"},
    {.document = R"({"result":{}})", .expected = "{}"},
    {.document = R"({"results":1,"result":2})", .expected = "2"},
    {.document = R"({"result":"ends with \\\\"})", .expected = R"("ends with \\\\")"},
}};

constexpr std::array<std::string_view, 11> kInvalidOrMissingCases{{
    R"({})",
    R"([])",
    R"({"other":1})",
    R"({"result":)",
    R"({"result":[1,2})",
    R"({"result":"unterminated})",
    R"({"result":tru})",
    R"({"result":01})",
    R"({"result":{},})",
    R"({"result":1,"result":2})",
    R"({"re\u0073ult":1})",
}};

constexpr std::array<RangeCase, 3> kStructurallyBalancedInvalidNestedCases{{
    {.document = R"({"result":{"items":[,]}})", .expected = R"({"items":[,]})"},
    {.document = R"({"result":{"value":tru}})", .expected = R"({"value":tru})"},
    {.document = R"({"result":{"value":01}})", .expected = R"({"value":01})"},
}};

std::string BuildNestedResult(std::size_t depth) {
  std::string document = R"({"result":)";
  document.append(depth, '[');
  document.append(depth, ']');
  document.push_back('}');
  return document;
}

TEST(JsonValueTest, FindsTopLevelMemberValues) {
  for (const auto& test_case : kValidCases) {
    const auto range = a2a::core::json::FindTopLevelObjectMemberValue(test_case.document, kTarget);
    if (!range.has_value()) {
      ADD_FAILURE() << test_case.document;
      continue;
    }
    const auto value_range = *range;
    EXPECT_EQ(test_case.document.substr(value_range.begin, value_range.end - value_range.begin), test_case.expected);
  }
}

TEST(JsonValueTest, RejectsMalformedMissingDuplicateAndEscapedMemberNames) {
  for (const auto document : kInvalidOrMissingCases) {
    EXPECT_FALSE(a2a::core::json::FindTopLevelObjectMemberValue(document, kTarget).has_value()) << document;
  }
}

TEST(JsonValueTest, ReturnsRangeWithoutValidatingNestedCompositeGrammar) {
  for (const auto& test_case : kStructurallyBalancedInvalidNestedCases) {
    const auto range = a2a::core::json::FindTopLevelObjectMemberValue(test_case.document, kTarget);
    if (!range.has_value()) {
      ADD_FAILURE() << test_case.document;
      continue;
    }
    const auto value_range = *range;
    EXPECT_EQ(test_case.document.substr(value_range.begin, value_range.end - value_range.begin), test_case.expected);
  }
}

TEST(JsonValueTest, AcceptsConfiguredMaximumNestingDepth) {
  constexpr std::size_t kMaximumSupportedDepth = 128U;
  const std::string document = BuildNestedResult(kMaximumSupportedDepth);
  EXPECT_TRUE(a2a::core::json::FindTopLevelObjectMemberValue(document, kTarget).has_value());
}

TEST(JsonValueTest, EnforcesNestingLimit) {
  constexpr std::size_t kExcessiveDepth = 129U;
  const std::string document = BuildNestedResult(kExcessiveDepth);
  EXPECT_FALSE(a2a::core::json::FindTopLevelObjectMemberValue(document, kTarget).has_value());
}

}  // namespace
