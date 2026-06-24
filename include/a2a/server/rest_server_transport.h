// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "a2a/core/result.h"
#include "a2a/server/required_extensions_validator.h"
#include "a2a/server/rest_transport.h"
#include "a2a/v1/a2a.pb.h"

namespace a2a::server {

class HttpByteTransport;

struct HttpServerRequest final {
  std::string method;
  std::string target;
  std::unordered_map<std::string, std::string> headers;
  std::string body;
  std::string remote_address;
};

struct HttpServerResponse final {
  static constexpr int kDefaultStatusCode = 500;
  int status_code = kDefaultStatusCode;
  std::unordered_map<std::string, std::string> headers;
  std::string body;
  std::function<core::Result<void>(HttpByteTransport&)> stream_writer;
};

struct RestServerTransportOptions final {
  struct AgentCardCacheSettings final {
    std::optional<std::string> cache_control;
    std::optional<std::chrono::system_clock::time_point> last_modified;
  };

  std::string rest_api_base_path = "/";
  bool require_version_header = true;
  bool include_legacy_transport_fields = true;
  std::optional<AgentCardCacheSettings> agent_card_cache_settings = std::nullopt;
  std::vector<std::string> required_extensions = {};
};

class RestServerTransport final {
 public:
  static constexpr std::string_view kAgentCardPath = "/.well-known/agent-card.json";
  static constexpr std::string_view kLegacyAgentCardPath = "/.well-known/agent.json";

  RestServerTransport(Dispatcher* dispatcher, lf::a2a::v1::AgentCard agent_card,
                      RestServerTransportOptions options = {});

  [[nodiscard]] core::Result<HttpServerResponse> Handle(const HttpServerRequest& request) const;

 private:
  [[nodiscard]] core::Result<RestRequest> BuildRestRequest(const HttpServerRequest& request) const;
  [[nodiscard]] core::Result<void> ValidateVersionHeader(const HttpServerRequest& request) const;
  [[nodiscard]] core::Result<void> ValidateRequiredExtensions(const HttpServerRequest& request) const;
  [[nodiscard]] core::Result<HttpServerResponse> HandleAgentCard(const HttpServerRequest& request) const;
  [[nodiscard]] static HttpServerResponse ToHttpResponse(const RestResponse& response);

  static std::string NormalizeBasePath(std::string_view path);

  RestTransport transport_;
  lf::a2a::v1::AgentCard agent_card_;
  RestServerTransportOptions options_;
  RequiredExtensionsValidator required_extensions_validator_;
};

}  // namespace a2a::server
