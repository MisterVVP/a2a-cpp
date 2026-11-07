// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

#include <grpcpp/support/status.h>

#include <string>
#include <string_view>

namespace a2a::server {
[[nodiscard]] std::string SerializeGrpcStatusDetails(::grpc::StatusCode code, std::string_view error_message,
                                                     std::string_view reason);
}  // namespace a2a::server
