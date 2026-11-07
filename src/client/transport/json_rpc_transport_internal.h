// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

#include <google/protobuf/message.h>
#include <google/protobuf/struct.pb.h>

#include <string>
#include <string_view>

#include "a2a/client/json_rpc_transport.h"
#include "a2a/core/error.h"
#include "a2a/core/protojson.h"
#include "a2a/http/http_client.h"

namespace a2a::client::json_rpc_internal {

inline constexpr char kTransportName[] = "jsonrpc";

[[nodiscard]] a2a::http::Request ToSharedHttpRequest(const HttpRequest& request);
[[nodiscard]] HttpClientResponse ToClientHttpResponse(a2a::http::Response response);
[[nodiscard]] std::string JoinUrl(std::string_view interface_base_url);
[[nodiscard]] core::Result<void> ValidateResponseVersion(const HttpClientResponse& response);
[[nodiscard]] core::Error BuildJsonRpcEnvelopeError(std::string_view message, const HttpClientResponse& response);
[[nodiscard]] core::Result<std::string> ParseResponseResult(const HttpClientResponse& response,
                                                            std::string_view expected_id);
[[nodiscard]] core::Result<std::string> BuildJsonRpcEnvelope(std::string_view method_name,
                                                             const google::protobuf::Message& request,
                                                             std::string_view request_id);

template <typename T>
[[nodiscard]] core::Result<T> ParseResultMessage(std::string_view json, int response_status_code,
                                                 bool ignore_unknown_fields = false) {
  T message;
  const auto parse = core::JsonToMessage(json, &message, {.ignore_unknown_fields = ignore_unknown_fields});
  if (!parse.ok()) {
    return parse.error().WithTransport(kTransportName).WithHttpStatus(response_status_code);
  }
  return message;
}

[[nodiscard]] core::Result<HttpRequest> BuildUnaryRequest(const ResolvedInterface& resolved_interface,
                                                          std::chrono::milliseconds default_timeout,
                                                          std::string request_body, const CallOptions& options);
[[nodiscard]] std::shared_ptr<AsyncClientTransport> MakeAsyncTransport(
    ResolvedInterface resolved_interface, HttpAsyncRequester requester, std::chrono::milliseconds default_timeout,
    std::shared_ptr<RequestIdGenerator> id_generator);
[[nodiscard]] core::Result<void> ValidateJsonRpcHttpStatus(const HttpClientResponse& response);
[[nodiscard]] core::Result<lf::a2a::v1::ListTasksResponse> ParseListTasksResult(const HttpClientResponse& response,
                                                                                std::string_view expected_id);

}  // namespace a2a::client::json_rpc_internal
