// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

#include <string>
#include <string_view>

#include <google/protobuf/message.h>
#include <google/protobuf/struct.pb.h>

#include "a2a/core/error.h"
#include "a2a/client/json_rpc_transport.h"
#include "a2a/http/http_client.h"
#include "a2a/core/protojson.h"

namespace a2a::client::json_rpc_internal {

inline constexpr char kTransportName[] = "jsonrpc";

[[nodiscard]] a2a::http::Request ToSharedHttpRequest(const HttpRequest& request);
[[nodiscard]] HttpClientResponse ToClientHttpResponse(a2a::http::Response response);
[[nodiscard]] std::string JoinUrl(std::string_view interface_base_url);
[[nodiscard]] core::Result<void> ValidateResponseVersion(const HttpClientResponse& response);
[[nodiscard]] core::Error BuildJsonRpcEnvelopeError(std::string_view message, const HttpClientResponse& response);
[[nodiscard]] core::Result<google::protobuf::Value> ParseResponseResult(const HttpClientResponse& response,
                                                                        std::string_view expected_id);
[[nodiscard]] core::Result<std::string> BuildJsonRpcEnvelope(std::string_view method_name,
                                                              const google::protobuf::Message& request,
                                                              std::string_view request_id);

template <typename T>
[[nodiscard]] core::Result<T> ParseResultMessage(const google::protobuf::Value& result_value,
                                                 int response_status_code,
                                                 bool ignore_unknown_fields = false) {
  T message;
  const auto json = core::MessageToJson(result_value);
  if (!json.ok()) {
    return json.error().WithTransport(kTransportName).WithHttpStatus(response_status_code);
  }
  const auto parse = core::JsonToMessage(json.value(), &message, {.ignore_unknown_fields = ignore_unknown_fields});
  if (!parse.ok()) {
    return parse.error().WithTransport(kTransportName).WithHttpStatus(response_status_code);
  }
  return message;
}

}  // namespace a2a::client::json_rpc_internal
