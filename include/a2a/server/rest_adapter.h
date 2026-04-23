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

struct RestResponse final {
  int status_code = 200;
  std::unordered_map<std::string, std::string> headers;
  std::string body;
};

using AgentCardProvider = std::function<core::Result<lf::a2a::v1::AgentCard>(const RequestContext&)>;

struct RestAdapterConfig final {
  std::string rest_base_url;
  AgentCardProvider agent_card_provider;
};

class RestAdapter final {
 public:
  explicit RestAdapter(RestAdapterConfig config);

  [[nodiscard]] RestResponse Handle(const RestRequest& request,
                                    const RequestContext& context) const;

 private:
  [[nodiscard]] RestResponse HandleAgentCard(const RequestContext& context) const;

  RestAdapterConfig config_;
};

}  // namespace a2a::server
