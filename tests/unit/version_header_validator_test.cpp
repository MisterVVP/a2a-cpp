// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "a2a/server/version_header_validator.h"

#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <unordered_map>

#include "a2a/core/error.h"
#include "a2a/core/version.h"

namespace {

constexpr std::string_view kUnsupportedVersion = "2.0";

using HeaderMap = std::unordered_map<std::string, std::string>;

HeaderMap MakeVersionHeaders(std::string_view version) {
  return {{std::string(a2a::core::Version::kHeaderName), std::string(version)}};
}

TEST(VersionHeaderValidatorTest, RejectsMissingVersionWhenRequired) {
  const a2a::server::VersionHeaderValidator validator(/*require_version_header=*/true);

  const auto result = validator.Validate(HeaderMap{});

  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().code(), a2a::core::ErrorCode::kUnsupportedVersion);
  EXPECT_EQ(result.error().message(), a2a::server::VersionHeaderValidator::kMissingRequiredVersionMessage);
}

TEST(VersionHeaderValidatorTest, AcceptsMissingVersionWhenOptional) {
  const a2a::server::VersionHeaderValidator validator(/*require_version_header=*/false);

  const auto result = validator.Validate(HeaderMap{});

  EXPECT_TRUE(result.ok());
}

TEST(VersionHeaderValidatorTest, RejectsUnsupportedVersion) {
  const a2a::server::VersionHeaderValidator validator(/*require_version_header=*/true);

  const auto result = validator.Validate(MakeVersionHeaders(kUnsupportedVersion));

  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().code(), a2a::core::ErrorCode::kUnsupportedVersion);
  EXPECT_EQ(result.error().message(), a2a::server::VersionHeaderValidator::kUnsupportedVersionMessage);
}

TEST(VersionHeaderValidatorTest, AcceptsSupportedVersion) {
  const a2a::server::VersionHeaderValidator validator(/*require_version_header=*/true);

  const auto result = validator.Validate(MakeVersionHeaders(a2a::core::Version::HeaderValue()));

  EXPECT_TRUE(result.ok());
}

}  // namespace
