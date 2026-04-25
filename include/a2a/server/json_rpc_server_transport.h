#pragma once

#include <google/protobuf/struct.pb.h>

#include <optional>
#include <string>
#include <unordered_map>

#include "a2a/core/result.h"
#include "a2a/server/rest_server_transport.h"
#include "a2a/server/server.h"

namespace a2a::server {

struct JsonRpcServerTransportOptions final {
  std::string rpc_path = "/";
  bool require_version_header = true;
};

class JsonRpcServerTransport final {
 public:
  JsonRpcServerTransport(Dispatcher* dispatcher, JsonRpcServerTransportOptions options = {});

  [[nodiscard]] core::Result<HttpServerResponse> Handle(const HttpServerRequest& request) const;

 private:
  class ResponseId final {
   public:
    ResponseId() = default;
    explicit ResponseId(google::protobuf::Value value) : value_(std::move(value)) {}

    [[nodiscard]] const google::protobuf::Value& value() const noexcept { return value_; }

   private:
    google::protobuf::Value value_;
  };

  struct JsonRpcRequest final {
    ResponseId id;
    DispatchRequest dispatch;
  };

  [[nodiscard]] core::Result<void> ValidateVersionHeader(const HttpServerRequest& request) const;
  [[nodiscard]] static core::Result<JsonRpcRequest> ParseRequest(std::string_view body);
  [[nodiscard]] static core::Result<google::protobuf::Value> SerializeDispatchResult(
      const DispatchRequest& request, const DispatchResponse& response);
  [[nodiscard]] static HttpServerResponse BuildSuccessResponse(
      const ResponseId& id, const google::protobuf::Value& result);
  [[nodiscard]] static HttpServerResponse BuildErrorResponse(
      int json_rpc_code, std::string_view message, const ResponseId& id,
      const std::optional<core::Error>& error, int http_status);
  [[nodiscard]] static std::string NormalizePath(std::string path);

  Dispatcher* dispatcher_ = nullptr;
  JsonRpcServerTransportOptions options_;
};

}  // namespace a2a::server
