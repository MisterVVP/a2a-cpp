// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "a2a/client/call_options.h"
#include "a2a/core/result.h"
#include "a2a/v1/a2a.pb.h"

namespace a2a::client {
struct ListTasksRequest final {
  std::size_t page_size = 0;
  std::string page_token;
};

struct ListTasksResponse final {
  std::vector<lf::a2a::v1::Task> tasks;
  std::string next_page_token;
};

struct ClientCallContext final {
  std::string_view operation;
  const CallOptions* options = nullptr;
};

struct ClientCallResult final {
  bool ok = true;
  std::optional<core::Error> error = std::nullopt;
};

class ClientInterceptor {
 public:
  virtual ~ClientInterceptor() = default;
  virtual void BeforeCall(const ClientCallContext& context) { (void)context; }
  virtual void AfterCall(const ClientCallContext& context, const ClientCallResult& result) {
    (void)context;
    (void)result;
  }
};

}  // namespace a2a::client
