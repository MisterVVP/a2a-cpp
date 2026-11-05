// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

// Included only by the temporary source overlay, in one test translation unit.
#include <gtest/gtest.h>

#include <exception>
#include <iostream>

#include "curl_reactor_connect_probe.h"
#include "curl_reactor_test_support.h"

namespace a2a::http::stage_trace {
using namespace a2a::http::testing;

TEST(CurlConnectionProbeTest, RefusedConnectionRemainsAnError) {
  Client client;
  TestSocket bound(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
  ASSERT_TRUE(bound.valid());
  auto address = TestLoopbackAddress();
  auto length = static_cast<TestSocketLength>(sizeof(address));
  ASSERT_EQ(::bind(bound.get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)), 0);
  ASSERT_EQ(::getsockname(bound.get(), reinterpret_cast<sockaddr*>(&address), &length), 0);
  Request request;
  request.method = kTestGetMethod;
  request.url = kLoopbackUrlPrefix;
  request.url.append(std::to_string(ntohs(address.sin_port)));
  request.timeout = kReactorTestTimeout;
  EXPECT_FALSE(client.SendRequest(request).ok());
}

TEST(CurlConnectionProbeTest, InvalidSocketPreservesFailureRetrievalAndWinsockError) {
  const auto global = detail::EnsureCurlGlobalInit();
  ASSERT_EQ(global->code, CURLE_OK);
  if (original_socket_error == nullptr) {
    GTEST_SKIP();
  }
  const auto before = confirmed_failure_checks.load();
  int error = 0;
  int length = sizeof(error);
  EXPECT_EQ(ProbeSocketError(INVALID_SOCKET, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &length),
            SOCKET_ERROR);
  EXPECT_EQ(WSAGetLastError(), WSAENOTSOCK);
  EXPECT_GT(confirmed_failure_checks.load(), before);
}

}  // namespace a2a::http::stage_trace

int main(int argc, char** argv) {
  try {
    ::testing::InitGoogleTest(&argc, argv);
    // CMake's discovery invocation does not set the trace marker.
    if (GetEnvironmentVariableA(a2a::http::stage_trace::kOutputVariable, nullptr, 0) == 0U) {
      return RUN_ALL_TESTS();
    }
    const a2a::http::stage_trace::ConnectProbe probe;
    return RUN_ALL_TESTS();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
