// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <google/protobuf/struct.pb.h>

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "a2a/core/protocol_error_reasons.h"
#include "a2a/core/result.h"
#include "a2a/server/rest_server_transport.h"

namespace a2a::server::internal {

namespace error_statuses {
inline constexpr char kInvalidArgument[] = "INVALID_ARGUMENT";
inline constexpr char kNotFound[] = "NOT_FOUND";
inline constexpr char kUnauthenticated[] = "UNAUTHENTICATED";
inline constexpr char kPermissionDenied[] = "PERMISSION_DENIED";
inline constexpr char kConflict[] = "CONFLICT";
inline constexpr char kUnsupportedMediaType[] = "UNSUPPORTED_MEDIA_TYPE";
inline constexpr char kInternal[] = "INTERNAL";
inline constexpr char kBadGateway[] = "BAD_GATEWAY";
inline constexpr char kUnknown[] = "UNKNOWN";
inline constexpr char kFailedPrecondition[] = "FAILED_PRECONDITION";
inline constexpr char kUnavailable[] = "UNAVAILABLE";
}  // namespace error_statuses

namespace error_fields {
inline constexpr char kType[] = "@type";
inline constexpr char kReason[] = "reason";
inline constexpr char kDomain[] = "domain";
inline constexpr char kMetadata[] = "metadata";
inline constexpr char kTransport[] = "transport";
inline constexpr char kProtocolCode[] = "protocolCode";
inline constexpr char kCode[] = "code";
inline constexpr char kStatus[] = "status";
inline constexpr char kMessage[] = "message";
inline constexpr char kDetails[] = "details";
inline constexpr char kEnvelope[] = "error";
inline constexpr char kData[] = "data";
inline constexpr char kTypeUrl[] = "type.googleapis.com/google.rpc.ErrorInfo";
inline constexpr char kDomainValue[] = "a2a-protocol.org";
}  // namespace error_fields

namespace request_fields {
inline constexpr char kPageSize[] = "pageSize";
inline constexpr char kPageToken[] = "pageToken";
inline constexpr char kContextId[] = "contextId";
inline constexpr char kSnakeContextId[] = "context_id";
inline constexpr char kStatus[] = "status";
inline constexpr char kStatusTimestampAfter[] = "statusTimestampAfter";
inline constexpr char kHistoryLength[] = "historyLength";
inline constexpr char kIncludeArtifacts[] = "includeArtifacts";
inline constexpr char kTaskId[] = "taskId";
inline constexpr char kPushNotificationConfig[] = "pushNotificationConfig";
}  // namespace request_fields

namespace messages {
inline constexpr char kMissingRestDispatcher[] = "REST transport dispatcher is not configured";
inline constexpr char kNoMatchingRestRoute[] = "No matching route or request was malformed";
}  // namespace messages

namespace transport_names {
inline constexpr char kRest[] = "rest";
inline constexpr char kJsonRpc[] = "jsonrpc";
}  // namespace transport_names

inline constexpr std::string_view kPreconditionReasons[]{core::protocol_error_reasons::kTaskNotCancelable,
                                                         core::protocol_error_reasons::kPushNotificationNotSupported,
                                                         core::protocol_error_reasons::kUnsupportedOperation,
                                                         core::protocol_error_reasons::kExtendedAgentCardNotConfigured,
                                                         core::protocol_error_reasons::kExtensionSupportRequired,
                                                         core::protocol_error_reasons::kVersionNotSupported};

[[nodiscard]] inline std::string_view A2aErrorStatus(std::string_view reason, std::string_view fallback) {
  for (const auto expected : kPreconditionReasons) {
    if (reason == expected) {
      return error_statuses::kFailedPrecondition;
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
