// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "a2a/core/json_value.h"

#include <gtest/gtest.h>

#include <array>
#include <string_view>

namespace {

constexpr std::string_view kTarget = "result";

struct RangeCase final {
  std::string_view document;
  std::string_view expected;
};

constexpr std::array<RangeCase, 14> kValidCases{{
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

TEST(JsonValueTest, EnforcesNestingLimit) {
  constexpr std::size_t kExcessiveDepth = 130U;
  std::string document = R"({"result":)";
  document.append(kExcessiveDepth, '[');
  document.append(kExcessiveDepth, ']');
  document.push_back('}');
  EXPECT_FALSE(a2a::core::json::FindTopLevelObjectMemberValue(document, kTarget).has_value());
}

}  // namespace
