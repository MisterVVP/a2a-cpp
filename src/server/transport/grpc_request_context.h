// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

#include <grpcpp/server_context.h>

#include <string>
#include <vector>

#include "a2a/core/result.h"
#include "a2a/server/request_context.h"
#include "a2a/server/required_extensions_validator.h"

namespace a2a::server {
struct ValidatedGrpcRequest final {
  RequestContext request_context;
  std::vector<std::string> activated_extensions;
};
[[nodiscard]] core::Result<ValidatedGrpcRequest> BuildGrpcRequestContext(
    const ::grpc::ServerContextBase& context, const RequiredExtensionsValidator& extensions_validator);
void AddGrpcActivatedExtensions(::grpc::ServerContextBase* context, const std::vector<std::string>& extensions);
}  // namespace a2a::server
