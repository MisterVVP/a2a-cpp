#pragma once

#include <optional>
#include <string>
#include <unordered_map>

#include "a2a/core/error.h"
#include "a2a/server/server.h"

namespace a2a::server {

struct RestPipelineResponse final {
  int status_code = 0;
  std::unordered_map<std::string, std::string> headers;
  std::optional<DispatchResponse> payload;
  std::optional<core::Error> error;
};

class RestAdapter final {
 public:
  explicit RestAdapter(const Dispatcher* dispatcher);

  [[nodiscard]] RestPipelineResponse Handle(
      const DispatchRequest& request,
      const std::unordered_map<std::string, std::string>& request_headers,
      RequestContext& context) const;

 private:
  const Dispatcher* dispatcher_ = nullptr;
};

}  // namespace a2a::server
