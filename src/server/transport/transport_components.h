// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <google/protobuf/struct.pb.h>

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "a2a/core/result.h"
#include "a2a/server/rest_server_transport.h"

namespace a2a::server::internal {

inline constexpr std::string_view kPreconditionReasons[]{
    "TASK_NOT_CANCELABLE",        "PUSH_NOTIFICATION_NOT_SUPPORTED",
    "UNSUPPORTED_OPERATION",      "EXTENDED_AGENT_CARD_NOT_CONFIGURED",
    "EXTENSION_SUPPORT_REQUIRED", "VERSION_NOT_SUPPORTED"};
inline constexpr std::string_view kFailedPrecondition = "FAILED_PRECONDITION";

[[nodiscard]] inline std::string_view A2aErrorStatus(std::string_view reason, std::string_view fallback) {
  for (const auto expected : kPreconditionReasons) {
    if (reason == expected) {
      return kFailedPrecondition;
    }
  }
  return fallback;
}

struct JsonRpcEnvelope final {
  google::protobuf::Value id;
  std::string method;
  std::string params;
};

[[nodiscard]] core::Result<JsonRpcEnvelope> ParseJsonRpcEnvelope(std::string_view body);
[[nodiscard]] core::Result<std::string> SerializeJsonRpcSuccessEnvelope(const google::protobuf::Value& id,
                                                                        const google::protobuf::Value& result);

[[nodiscard]] core::Result<void> ParseRestQueryString(std::string_view query,
                                                      std::unordered_map<std::string, std::string>* query_params);
[[nodiscard]] HttpServerResponse BuildRestHttpResponse(const RestResponse& response,
                                                       const std::vector<std::string>& activated_extensions);

}  // namespace a2a::server::internal
