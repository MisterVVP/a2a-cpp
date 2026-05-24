// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "a2a/server/transport_mux.h"

#include <algorithm>

namespace a2a::server {

TransportMux::TransportMux() : TransportMux(Options{}) {}

TransportMux::TransportMux(Options options) : options_(std::move(options)) {
  not_found_handler_ = [this](const HttpServerRequest&) { return BuildDefaultNotFound(last_route_miss_); };
}

void TransportMux::RegisterRoute(Route route) {
  routes_.push_back(std::move(route));
  std::stable_sort(routes_.begin(), routes_.end(),
                   [](const Route& lhs, const Route& rhs) { return lhs.priority > rhs.priority; });
}

void TransportMux::SetNotFoundHandler(Handler handler) { not_found_handler_ = std::move(handler); }

void TransportMux::RegisterJsonRpcRoute(JsonRpcServerTransport& transport) {
  RegisterJsonRpcRoute(transport, JsonRpcRouteOptions{});
}

void TransportMux::RegisterJsonRpcRoute(JsonRpcServerTransport& transport, JsonRpcRouteOptions options) {
  RegisterRoute({
      .name = std::move(options.route_name),
      .matcher = [method = std::move(options.method), rpc_path = std::move(options.rpc_path)](
                     std::string_view request_method,
                     std::string_view path) { return request_method == method && path == rpc_path; },
      .handler = [&transport](const HttpServerRequest& routed_request) { return transport.Handle(routed_request); },
      .priority = options.priority,
  });
}

void TransportMux::RegisterRestRoute(RestServerTransport& transport) {
  RegisterRestRoute(transport, RestRouteOptions{});
}

void TransportMux::RegisterRestRoute(RestServerTransport& transport, RestRouteOptions options) {
  RegisterRoute({
      .name = std::move(options.route_name),
      .matcher =
          [rest_api_prefix = std::move(options.rest_api_prefix),
           well_known_prefix = std::move(options.well_known_prefix)](std::string_view method, std::string_view path) {
            (void)method;
            return path.starts_with(rest_api_prefix) || path.starts_with(well_known_prefix);
          },
      .handler = [&transport](const HttpServerRequest& routed_request) { return transport.Handle(routed_request); },
      .priority = options.priority,
  });
}

core::Result<HttpServerResponse> TransportMux::RouteRequest(const HttpServerRequest& request) const {
  const std::string normalized_path = NormalizePath(request.target);
  bool path_matched = false;

  for (const auto& route : routes_) {
    if (route.matcher(request.method, normalized_path)) {
      HttpServerRequest normalized_request = request;
      normalized_request.target = NormalizeTargetForHandler(request.target);
      return route.handler(normalized_request);
    }
    if (route.matcher("*", normalized_path)) {
      path_matched = true;
    }
  }

  last_route_miss_ =
      RouteMiss{.method = request.method,
                .normalized_path = normalized_path,
                .reason = path_matched ? RouteMiss::Reason::kMethodNotAllowed : RouteMiss::Reason::kNoRoute};
  return not_found_handler_(request);
}

const TransportMux::RouteMiss& TransportMux::last_route_miss() const noexcept { return last_route_miss_; }

std::string TransportMux::NormalizePath(std::string_view path) const {
  std::string normalized(path);
  const auto query_start = normalized.find('?');
  if (query_start != std::string::npos) {
    normalized = normalized.substr(0, query_start);
  }
  if (normalized.empty()) {
    normalized = "/";
  }
  if (normalized.front() != '/') {
    normalized.insert(normalized.begin(), '/');
  }
  if (options_.normalization_policy == PathNormalizationPolicy::kTrimTrailingSlash ||
      options_.normalization_policy == PathNormalizationPolicy::kRootToDefaultPath) {
    while (normalized.size() > 1 && normalized.back() == '/') {
      normalized.pop_back();
    }
  }
  if (options_.normalization_policy == PathNormalizationPolicy::kRootToDefaultPath && normalized == "/" &&
      !options_.default_path.empty()) {
    normalized = options_.default_path;
  }
  return normalized;
}

std::string TransportMux::NormalizeTargetForHandler(std::string_view target) const {
  const auto query_start = target.find('?');
  const std::string normalized_path = NormalizePath(target);
  if (query_start == std::string_view::npos) {
    return normalized_path;
  }
  std::string normalized_target = normalized_path;
  normalized_target.append(target.substr(query_start));
  return normalized_target;
}

HttpServerResponse TransportMux::BuildDefaultNotFound(const RouteMiss& miss) {
  const bool method_not_allowed = miss.reason == RouteMiss::Reason::kMethodNotAllowed;
  HttpServerResponse response;
  response.status_code = method_not_allowed ? kHttpMethodNotAllowed : kHttpNotFound;
  response.headers["content-type"] = "application/json";
  const std::string_view message =
      method_not_allowed ? "No route matched HTTP method for normalized path" : "No route matched normalized path";
  const std::string_view code = method_not_allowed ? kRouteMethodNotAllowedCode : kRouteNotFoundCode;
  response.body = Concat(kRouteMissErrorPrefix, message, kRouteMissCodePrefix, code, kRouteMissPathPrefix,
                         miss.normalized_path, kRouteMissMethodPrefix, miss.method, kRouteMissJsonSuffix);
  return response;
}

}  // namespace a2a::server
