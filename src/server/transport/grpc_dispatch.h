// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once
#include <grpcpp/server_context.h>
#include <grpcpp/support/status.h>

#include <optional>
#include <string>
#include <vector>

#include "a2a/core/result.h"
#include "a2a/server/request_context.h"
#include "a2a/server/required_extensions_validator.h"
#include "a2a/server/tasks/list_tasks.h"
#include "a2a/v1/a2a.pb.h"
namespace a2a::server {
struct ValidatedGrpcRequest final {
  RequestContext request_context;
  std::vector<std::string> activated_extensions;
};
core::Result<ValidatedGrpcRequest> BuildGrpcRequestContext(const ::grpc::ServerContextBase& context,
                                                           const RequiredExtensionsValidator& extensions_validator);
::grpc::Status GrpcErrorStatus(const core::Error& error, ::grpc::ServerContextBase* context,
                               const std::vector<std::string>& activated_extensions = {},
                               std::optional<::grpc::StatusCode> status_override = {});
void AddGrpcActivatedExtensions(::grpc::ServerContextBase* context, const std::vector<std::string>& extensions);
core::Result<ListTasksRequest> ParseGrpcListTasksRequest(const lf::a2a::v1::ListTasksRequest& request);
void WriteGrpcListTasksResponse(const ListTasksResponse& payload, lf::a2a::v1::ListTasksResponse& response);
}  // namespace a2a::server
