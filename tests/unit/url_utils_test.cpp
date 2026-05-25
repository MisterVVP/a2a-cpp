// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "a2a/core/url_utils.h"

#include <gtest/gtest.h>

namespace {

using a2a::core::ExtractTargetPath;
using a2a::core::UrlSuffixPolicy;

TEST(UrlUtilsTest, ExtractsPathFromAbsoluteUrls) {
  EXPECT_EQ(ExtractTargetPath("https://agent.local/a2a/tasks"), "/a2a/tasks");
  EXPECT_EQ(ExtractTargetPath("http://agent.local"), "/");
}

TEST(UrlUtilsTest, HandlesNoSchemeInputsAndNormalizesLeadingSlash) {
  EXPECT_EQ(ExtractTargetPath("already/path"), "/already/path");
  EXPECT_EQ(ExtractTargetPath("/already/path"), "/already/path");
  EXPECT_EQ(ExtractTargetPath("?q=1"), "/");
  EXPECT_EQ(ExtractTargetPath("#fragment"), "/");
}

TEST(UrlUtilsTest, AppliesSuffixPolicyExplicitly) {
  constexpr std::string_view kUrl = "https://agent.local/a2a/tasks?limit=10#section";
  EXPECT_EQ(ExtractTargetPath(kUrl, UrlSuffixPolicy::kPathOnly), "/a2a/tasks");
  EXPECT_EQ(ExtractTargetPath(kUrl, UrlSuffixPolicy::kPathAndQuery), "/a2a/tasks?limit=10");
  EXPECT_EQ(ExtractTargetPath(kUrl, UrlSuffixPolicy::kPathQueryAndFragment), "/a2a/tasks?limit=10#section");
}

TEST(UrlUtilsTest, ReturnsDefaultSlashWhenPathIsAbsentForAnyPolicy) {
  constexpr std::string_view kUrl = "https://agent.local?limit=10#section";
  EXPECT_EQ(ExtractTargetPath(kUrl, UrlSuffixPolicy::kPathOnly), "/");
  EXPECT_EQ(ExtractTargetPath(kUrl, UrlSuffixPolicy::kPathAndQuery), "/?limit=10");
  EXPECT_EQ(ExtractTargetPath(kUrl, UrlSuffixPolicy::kPathQueryAndFragment), "/?limit=10#section");
}

}  // namespace
