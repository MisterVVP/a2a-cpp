#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "a2a/core/result.h"
#include "a2a/server/server.h"

namespace a2a::server {

struct RestEndpointPaths final {
  static constexpr std::string_view kSendMessage = "/messages:send";
  static constexpr std::string_view kTaskCollection = "/tasks";
  static constexpr std::string_view kTaskResourcePrefix = "/tasks/";
  static constexpr std::string_view kTaskCancelSuffix = ":cancel";
};

struct RestRequest final {
  std::string method;
  std::string path;
  std::unordered_map<std::string, std::string> query_params;
  std::string body;
  RequestContext context;
};

struct RestResponse final {
  static constexpr int kDefaultHttpStatus = 500;
  int http_status = kDefaultHttpStatus;
  std::unordered_map<std::string, std::string> headers;
  std::string body;
};

struct RestRoute final {
  std::string_view method;
  std::string_view path_pattern;
  DispatcherOperation operation;
};

class RestTransport final {
 public:
  explicit RestTransport(Dispatcher* dispatcher);

  [[nodiscard]] static const std::vector<RestRoute>& Routes();
  [[nodiscard]] core::Result<RestResponse> Handle(const RestRequest& request) const;

 private:
  [[nodiscard]] static std::optional<DispatchRequest> BuildDispatchRequest(
      const RestRequest& request);
  [[nodiscard]] static core::Result<std::string> SerializeDispatchResponse(
      DispatcherOperation operation, const DispatchResponse& response);
  [[nodiscard]] static RestResponse BuildErrorResponse(const core::Error& error);

  Dispatcher* dispatcher_ = nullptr;
};

}  // namespace a2a::server
