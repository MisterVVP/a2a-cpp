// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)
#pragma once
#include <functional>
#include <string>
#include <variant>

#include "a2a/core/result.h"
#include "a2a/server/dispatch_types.h"
#include "a2a/server/request_context.h"

namespace a2a::server {
// Separates protocol validation/serialization from application scheduling. The
// continuation owns its ingress metadata and must be consumed once.
template <typename Response>
struct PreparedDispatch final {
  DispatchRequest request;
  RequestContext context;
  std::function<core::Result<Response>(core::Result<DispatchResponse>)> complete;
  std::function<core::Result<std::string>(const lf::a2a::v1::StreamResponse&)> encode_event = {};
  std::function<core::Result<std::string>(const core::Error&)> encode_error = {};
};
template <typename Response>
using PreparedResponse = std::variant<Response, PreparedDispatch<Response>>;
}  // namespace a2a::server
