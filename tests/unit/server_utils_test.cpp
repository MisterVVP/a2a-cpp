// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "a2a/server/server_utils.h"

#include <gtest/gtest.h>

namespace {

TEST(ServerUtilsTest, ConcatBuildsExpectedString) {
  const std::string result = a2a::server::Concat("{\"k\":\"", "v", "\"}");
  EXPECT_EQ(result, "{\"k\":\"v\"}");
}

TEST(ServerUtilsTest, ConcatAcceptsStringViewsAndStrings) {
  const std::string left = "left";
  constexpr std::string_view middle = "-mid-";
  const std::string result = a2a::server::Concat(left, middle, "right");
  EXPECT_EQ(result, "left-mid-right");
}

}  // namespace
