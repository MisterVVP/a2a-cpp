// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

#include <google/protobuf/empty.pb.h>
#include <grpcpp/support/status.h>

#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

#include "a2a/core/result.h"
#include "a2a/server/dispatch_types.h"
#include "server/transport/grpc_task_conversion.h"

namespace a2a::server {
template <typename Request>
[[nodiscard]] core::Result<DispatchRequest> ParseGrpcUnaryRequest(DispatcherOperation operation,
                                                                  const Request& request) {
  DispatchRequest dispatch{.operation = operation};
  if constexpr (std::is_same_v<Request, lf::a2a::v1::ListTasksRequest>) {
    auto parsed = ParseGrpcListTasksRequest(request);
    if (!parsed.ok()) {
      return parsed.error();
    }
    dispatch.payload = std::move(parsed.value());
  } else if constexpr (std::is_same_v<Request, lf::a2a::v1::SubscribeToTaskRequest>) {
    lf::a2a::v1::GetTaskRequest get;
    get.set_id(request.id());
    get.set_tenant(request.tenant());
    dispatch.payload = std::move(get);
  } else {
    dispatch.payload = request;
  }
  return dispatch;
}

template <typename Response>
[[nodiscard]] ::grpc::Status WriteGrpcUnaryResponse(DispatchResponse& dispatch, Response& response,
                                                    std::string_view mismatch) {
  if constexpr (std::is_same_v<Response, lf::a2a::v1::ListTasksResponse>) {
    if (const auto* list = std::get_if<ListTasksResponse>(&dispatch.payload())) {
      WriteGrpcListTasksResponse(*list, response);
      return ::grpc::Status::OK;
    }
  } else if constexpr (std::is_same_v<Response, google::protobuf::Empty>) {
    if (std::holds_alternative<std::monostate>(dispatch.payload())) {
      return ::grpc::Status::OK;
    }
  } else {
    if (auto* payload = std::get_if<Response>(&dispatch.payload())) {
      response = std::move(*payload);
      return ::grpc::Status::OK;
    }
  }
  return {::grpc::StatusCode::INTERNAL, std::string(mismatch)};
}
}  // namespace a2a::server
