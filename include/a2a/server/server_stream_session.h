// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

#include <optional>

#include "a2a/core/result.h"
#include "a2a/v1/a2a.pb.h"

namespace a2a::server {

class ServerStreamSession {
 public:
  virtual ~ServerStreamSession() = default;

  [[nodiscard]] virtual core::Result<std::optional<lf::a2a::v1::StreamResponse>> Next() = 0;
};

}  // namespace a2a::server
