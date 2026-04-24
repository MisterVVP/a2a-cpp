#pragma once

#include <string>
#include <string_view>
#include <unordered_map>

#include "a2a/core/result.h"
#include "a2a/server/rest_transport.h"
#include "a2a/v1/a2a.pb.h"

namespace a2a::server {

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
};

struct RestServerTransportOptions final {
  std::string rest_api_base_path = "/";
  bool require_version_header = true;
};

class RestServerTransport final {
 public:
  static constexpr std::string_view kAgentCardPath = "/.well-known/agent-card.json";

  RestServerTransport(Dispatcher* dispatcher, lf::a2a::v1::AgentCard agent_card,
                      RestServerTransportOptions options = {});

  [[nodiscard]] core::Result<HttpServerResponse> Handle(const HttpServerRequest& request) const;

 private:
  [[nodiscard]] core::Result<RestRequest> BuildRestRequest(const HttpServerRequest& request) const;
  [[nodiscard]] core::Result<void> ValidateVersionHeader(const HttpServerRequest& request) const;
  [[nodiscard]] core::Result<HttpServerResponse> HandleAgentCard(
      const HttpServerRequest& request) const;
  [[nodiscard]] static HttpServerResponse ToHttpResponse(const RestResponse& response);

  static std::string NormalizeBasePath(std::string_view path);

  RestTransport transport_;
  lf::a2a::v1::AgentCard agent_card_;
  RestServerTransportOptions options_;
};

}  // namespace a2a::server
