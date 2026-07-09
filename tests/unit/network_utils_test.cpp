// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "a2a/server/network_utils.h"

#include <gtest/gtest.h>

#include <string>
#include <string_view>

namespace {

constexpr int kInvalidMaxPort = 0;
constexpr int kDefaultTestPort = 50061;
constexpr int kMaxTestPort = 65534;
constexpr std::string_view kLocalhost = "127.0.0.1";
constexpr std::string_view kRestPath = "/a2a";
constexpr std::string_view kExpectedRestUrl = "http://127.0.0.1:50061/a2a";
constexpr std::string_view kValidEndpoint = "127.0.0.1:50061";
constexpr std::string_view kMissingPortEndpoint = "127.0.0.1";
constexpr std::string_view kInvalidPortEndpoint = "127.0.0.1:not-a-port";
constexpr std::string_view kTooLargePortEndpoint = "127.0.0.1:65535";
constexpr std::string_view kExpectedEndpointFormatText = "Expected <host>:<port>";
constexpr std::string_view kExpectedTestPortRangeText = "port must be between 1 and 65534";
constexpr std::string_view kExpectedMaxPortRangeText = "max_port must be between 1 and 65535";

TEST(NetworkUtilsTest, BuildHttpUrlUsesHostPortAndPath) {
  EXPECT_EQ(a2a::server::BuildHttpUrl(kLocalhost, kDefaultTestPort, kRestPath), kExpectedRestUrl);
}

TEST(NetworkUtilsTest, ParseHostPortEndpointAcceptsValidEndpoint) {
  auto parsed = a2a::server::ParseHostPortEndpoint(kValidEndpoint, kMaxTestPort);
  ASSERT_TRUE(parsed.ok()) << parsed.error().message();
  EXPECT_EQ(parsed.value().host, kLocalhost);
  EXPECT_EQ(parsed.value().port, kDefaultTestPort);
}

TEST(NetworkUtilsTest, ParseHostPortEndpointRejectsMissingPort) {
  auto parsed = a2a::server::ParseHostPortEndpoint(kMissingPortEndpoint, kMaxTestPort);
  ASSERT_FALSE(parsed.ok());
  EXPECT_NE(parsed.error().message().find(kExpectedEndpointFormatText), std::string::npos);
}

TEST(NetworkUtilsTest, ParseHostPortEndpointRejectsInvalidPort) {
  auto parsed = a2a::server::ParseHostPortEndpoint(kInvalidPortEndpoint, kMaxTestPort);
  ASSERT_FALSE(parsed.ok());
  EXPECT_NE(parsed.error().message().find(kExpectedTestPortRangeText), std::string::npos);
}

TEST(NetworkUtilsTest, ParseHostPortEndpointRejectsPortAboveConfiguredMaximum) {
  auto parsed = a2a::server::ParseHostPortEndpoint(kTooLargePortEndpoint, kMaxTestPort);
  ASSERT_FALSE(parsed.ok());
  EXPECT_NE(parsed.error().message().find(kExpectedTestPortRangeText), std::string::npos);
}

TEST(NetworkUtilsTest, ParseHostPortEndpointRejectsInvalidConfiguredMaximum) {
  auto parsed = a2a::server::ParseHostPortEndpoint(kValidEndpoint, kInvalidMaxPort);
  ASSERT_FALSE(parsed.ok());
  EXPECT_NE(parsed.error().message().find(kExpectedMaxPortRangeText), std::string::npos);
}

}  // namespace
