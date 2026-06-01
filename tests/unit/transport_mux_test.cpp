// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "a2a/server/transport_mux.h"

#include <gtest/gtest.h>

namespace {

using a2a::server::HttpServerRequest;
using a2a::server::HttpServerResponse;
using a2a::server::TransportMux;

constexpr int kHttpOk = 200;
constexpr int kHttpCreated = 201;
constexpr int kHttpInternalServerError = 500;
constexpr std::string_view kRpcPath = "/rpc";
constexpr std::string_view kNoRouteCode = "ROUTE_NOT_FOUND";
constexpr std::string_view kCustomNotFoundBody = "custom not found";
constexpr int kLowPriority = 1;
constexpr int kHighPriority = 10;
constexpr std::string_view kHistoryQueryTarget = "/a2a/tasks/task-1?historyLength=0";

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
             response.status_code = kHttpCreated;
             return response;
           },
       .priority = kLowPriority});
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
       .priority = kHighPriority});

  HttpServerRequest request{.method = "POST", .target = "/", .headers = {}, .body = "", .remote_address = ""};
  auto result = mux.RouteRequest(request);
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result.value().status_code, kHttpOk);
}

TEST(TransportMuxTest, PreservesQueryStringWhenForwardingNormalizedTarget) {
  TransportMux mux;
  mux.RegisterRoute({.name = "rest",
                     .matcher =
                         [](std::string_view method, std::string_view path) {
                           return (method == "GET" || method == "*") && path == "/a2a/tasks/task-1";
                         },
                     .handler =
                         [](const HttpServerRequest& routed_request) {
                           HttpServerResponse response;
                           response.status_code =
                               routed_request.target == kHistoryQueryTarget ? kHttpOk : kHttpInternalServerError;
                           return response;
                         },
                     .priority = kLowPriority});

  HttpServerRequest request{
      .method = "GET", .target = "/a2a/tasks/task-1/?historyLength=0", .headers = {}, .body = "", .remote_address = ""};
  const auto result = mux.RouteRequest(request);
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result.value().status_code, kHttpOk);
}

TEST(TransportMuxTest, NormalizesRelativeTargetsAndTrimsTrailingSlash) {
  TransportMux mux;
  mux.RegisterRoute(
      {.name = "rpc",
       .matcher = [](std::string_view method,
                     std::string_view path) { return (method == "POST" || method == "*") && path == kRpcPath; },
       .handler =
           [](const HttpServerRequest& routed_request) {
             HttpServerResponse response;
             response.status_code = routed_request.target == kRpcPath ? kHttpOk : kHttpInternalServerError;
             return response;
           },
       .priority = kLowPriority});

  const HttpServerRequest request{.method = "POST", .target = "rpc/", .headers = {}, .body = "", .remote_address = ""};
  const auto result = mux.RouteRequest(request);

  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result.value().status_code, kHttpOk);
}

TEST(TransportMuxTest, ReturnsStructuredRouteMissForMissingPath) {
  TransportMux mux;

  const HttpServerRequest request{.method = "GET", .target = "", .headers = {}, .body = "", .remote_address = ""};
  const auto result = mux.RouteRequest(request);

  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result.value().status_code, TransportMux::kHttpNotFound);
  EXPECT_NE(result.value().body.find(std::string(kNoRouteCode)), std::string::npos);
  EXPECT_EQ(mux.last_route_miss().normalized_path, "/");
}

TEST(TransportMuxTest, UsesCustomNotFoundHandler) {
  TransportMux mux;
  mux.SetNotFoundHandler([](const HttpServerRequest&) {
    HttpServerResponse response;
    response.status_code = kHttpInternalServerError;
    response.body = std::string(kCustomNotFoundBody);
    return response;
  });

  const HttpServerRequest request{
      .method = "GET", .target = "/missing", .headers = {}, .body = "", .remote_address = ""};
  const auto result = mux.RouteRequest(request);

  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result.value().status_code, kHttpInternalServerError);
  EXPECT_EQ(result.value().body, kCustomNotFoundBody);
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
       .priority = kLowPriority});

  HttpServerRequest request{.method = "GET", .target = "/rpc", .headers = {}, .body = "", .remote_address = ""};
  auto result = mux.RouteRequest(request);
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result.value().status_code, TransportMux::kHttpMethodNotAllowed);
  EXPECT_NE(result.value().body.find("ROUTE_METHOD_NOT_ALLOWED"), std::string::npos);
}

}  // namespace
