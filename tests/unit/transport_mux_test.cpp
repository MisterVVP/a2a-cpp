// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "a2a/server/transport_mux.h"

#include <gtest/gtest.h>

namespace {

using a2a::server::HttpServerRequest;
using a2a::server::HttpServerResponse;
using a2a::server::TransportMux;

constexpr int kHttpOk = 200;

TEST(TransportMuxTest, RoutesByPriorityAndNormalizesRootToDefaultPath) {
  TransportMux mux(
      {.normalization_policy = TransportMux::PathNormalizationPolicy::kRootToDefaultPath, .default_path = "/rpc"});
  mux.RegisterRoute(
      {.name = "low",
       .matcher = [](std::string_view method,
                     std::string_view path) { return (method == "POST" || method == "*") && path == "/rpc"; },
       .handler =
           [](const HttpServerRequest&) {
             HttpServerResponse response;
             response.status_code = 201;
             return response;
           },
       .priority = 1});
  mux.RegisterRoute(
      {.name = "high",
       .matcher = [](std::string_view method,
                     std::string_view path) { return (method == "POST" || method == "*") && path == "/rpc"; },
       .handler =
           [](const HttpServerRequest&) {
             HttpServerResponse response;
             response.status_code = kHttpOk;
             return response;
           },
       .priority = 10});

  HttpServerRequest request{.method = "POST", .target = "/", .headers = {}, .body = "", .remote_address = ""};
  auto result = mux.RouteRequest(request);
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result.value().status_code, kHttpOk);
}

TEST(TransportMuxTest, PreservesQueryStringWhenForwardingNormalizedTarget) {
  constexpr std::string_view kExpectedTarget = "/a2a/tasks/task-1?historyLength=0";
  TransportMux mux;
  mux.RegisterRoute(
      {.name = "rest",
       .matcher = [](std::string_view method,
                     std::string_view path) { return (method == "GET" || method == "*") && path == "/a2a/tasks/task-1"; },
       .handler =
           [](const HttpServerRequest& routed_request) {
             HttpServerResponse response;
             response.status_code = routed_request.target == kExpectedTarget ? kHttpOk : 500;
             return response;
           },
       .priority = 1});

  HttpServerRequest request{
      .method = "GET", .target = "/a2a/tasks/task-1/?historyLength=0", .headers = {}, .body = "", .remote_address = ""};
  const auto result = mux.RouteRequest(request);
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result.value().status_code, kHttpOk);
}

TEST(TransportMuxTest, ReturnsStructuredRouteMissForMethodMismatch) {
  TransportMux mux;
  mux.RegisterRoute(
      {.name = "rpc",
       .matcher = [](std::string_view method,
                     std::string_view path) { return (method == "POST" || method == "*") && path == "/rpc"; },
       .handler =
           [](const HttpServerRequest&) {
             HttpServerResponse response;
             response.status_code = kHttpOk;
             return response;
           },
       .priority = 1});

  HttpServerRequest request{.method = "GET", .target = "/rpc", .headers = {}, .body = "", .remote_address = ""};
  auto result = mux.RouteRequest(request);
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result.value().status_code, 405);
  EXPECT_NE(result.value().body.find("ROUTE_METHOD_NOT_ALLOWED"), std::string::npos);
}

}  // namespace
