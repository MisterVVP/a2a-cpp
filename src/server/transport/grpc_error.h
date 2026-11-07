// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

#include <grpcpp/support/status.h>

#include <optional>
#include <string>
#include <vector>

#include "a2a/core/error.h"

namespace grpc {
class ServerContextBase;
}

namespace a2a::server {
[[nodiscard]] ::grpc::StatusCode GrpcStatusCode(const core::Error& error);
[[nodiscard]] std::string GrpcErrorCodeName(core::ErrorCode code);
[[nodiscard]] std::string GrpcErrorInfoReason(const core::Error& error);
[[nodiscard]] ::grpc::Status GrpcErrorStatus(const core::Error& error, ::grpc::ServerContextBase* context,
                                             const std::vector<std::string>& activated_extensions = {},
                                             std::optional<::grpc::StatusCode> status_override = {});
}  // namespace a2a::server
