#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

#include "a2a/core/result.h"
#include "a2a/server/server.h"
#include "a2a/v1/a2a.pb.h"

namespace a2a::server {

struct RestRequest final {
  std::string method;
  std::string path;
  std::unordered_map<std::string, std::string> headers;
};

inline constexpr int kDefaultHttpStatusCode = 200;

struct RestResponse final {
  int status_code = kDefaultHttpStatusCode;
  std::unordered_map<std::string, std::string> headers;
  std::string body;
};

using AgentCardProvider =
    std::function<core::Result<lf::a2a::v1::AgentCard>(const RequestContext&)>;

struct RestTransportConfig final {
  std::string rest_base_url;
  AgentCardProvider agent_card_provider;
};

class RestTransport final {
 public:
  explicit RestTransport(RestTransportConfig config);

  [[nodiscard]] RestResponse Handle(const RestRequest& request,
                                    const RequestContext& context) const;

 private:
  [[nodiscard]] RestResponse HandleAgentCard(const RequestContext& context) const;

  RestTransportConfig config_;
};

}  // namespace a2a::server
