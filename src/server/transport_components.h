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

struct JsonRpcEnvelope final {
  google::protobuf::Value id;
  std::string method;
  google::protobuf::Struct params;
};

[[nodiscard]] core::Result<JsonRpcEnvelope> ParseJsonRpcEnvelope(std::string_view body);
[[nodiscard]] core::Result<std::string> SerializeJsonRpcSuccessEnvelope(const google::protobuf::Value& id,
                                                                        const google::protobuf::Value& result);

[[nodiscard]] core::Result<void> ParseRestQueryString(
    std::string_view query, std::unordered_map<std::string, std::string>* query_params);
[[nodiscard]] HttpServerResponse BuildRestHttpResponse(
    const RestResponse& response, const std::vector<std::string>& activated_extensions);

}  // namespace a2a::server::internal
