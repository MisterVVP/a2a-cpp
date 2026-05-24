// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "a2a/core/result.h"
#include "a2a/server/json_rpc_server_transport.h"
#include "a2a/server/rest_server_transport.h"

namespace a2a::server {

class TransportMux final {
 public:
  static constexpr int kHttpNotFound = 404;
  static constexpr int kHttpMethodNotAllowed = 405;
  static constexpr std::string_view kRouteNotFoundCode = "ROUTE_NOT_FOUND";
  static constexpr std::string_view kRouteMethodNotAllowedCode = "ROUTE_METHOD_NOT_ALLOWED";

  enum class PathNormalizationPolicy {
    kNone,
    kTrimTrailingSlash,
    kRootToDefaultPath,
  };

  struct RouteMiss final {
    enum class Reason {
      kNoRoute,
      kMethodNotAllowed,
    };

    std::string method;
    std::string normalized_path;
    Reason reason = Reason::kNoRoute;
  };

  using Matcher = std::function<bool(std::string_view, std::string_view)>;
  using Handler = std::function<core::Result<HttpServerResponse>(const HttpServerRequest&)>;

  struct Route final {
    std::string name;
    Matcher matcher;
    Handler handler;
    int priority = 0;
  };

  struct JsonRpcRouteOptions final {
    std::string route_name = "jsonrpc";
    std::string rpc_path = "/rpc";
    std::string method = "POST";
    int priority = 100;
  };

  struct RestRouteOptions final {
    std::string route_name = "rest";
    std::string rest_api_prefix = "/a2a";
    std::string well_known_prefix = "/.well-known/";
    int priority = 10;
  };

  struct Options final {
    PathNormalizationPolicy normalization_policy = PathNormalizationPolicy::kTrimTrailingSlash;
    std::string default_path;
  };

  TransportMux();
  explicit TransportMux(Options options);

  void RegisterRoute(Route route);
  void RegisterJsonRpcRoute(JsonRpcServerTransport& transport);
  void RegisterJsonRpcRoute(JsonRpcServerTransport& transport, JsonRpcRouteOptions options);
  void RegisterRestRoute(RestServerTransport& transport);
  void RegisterRestRoute(RestServerTransport& transport, RestRouteOptions options);
  void SetNotFoundHandler(Handler handler);

  [[nodiscard]] core::Result<HttpServerResponse> RouteRequest(const HttpServerRequest& request) const;
  [[nodiscard]] const RouteMiss& last_route_miss() const noexcept;

 private:
  [[nodiscard]] std::string NormalizePath(std::string_view path) const;
  [[nodiscard]] static HttpServerResponse BuildDefaultNotFound(const RouteMiss& miss);

  Options options_;
  std::vector<Route> routes_;
  Handler not_found_handler_;
  mutable RouteMiss last_route_miss_{};
};

}  // namespace a2a::server
