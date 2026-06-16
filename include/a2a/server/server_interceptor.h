// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

#include "a2a/core/result.h"
#include "a2a/server/dispatch_types.h"
#include "a2a/server/request_context.h"

namespace a2a::server {

class ServerInterceptor {
 public:
  virtual ~ServerInterceptor() = default;

  virtual core::Result<void> BeforeDispatch(const DispatchRequest& request, RequestContext& context) {
    (void)request;
    (void)context;
    return {};
  }

  virtual void AfterDispatch(const DispatchRequest& request, RequestContext& context,
                             const core::Result<DispatchResponse>& result) {
    (void)request;
    (void)context;
    (void)result;
  }
};

}  // namespace a2a::server
