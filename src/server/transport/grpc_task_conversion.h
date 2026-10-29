// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

#include "a2a/core/result.h"
#include "a2a/server/tasks/list_tasks.h"
#include "a2a/v1/a2a.pb.h"

namespace a2a::server {
[[nodiscard]] core::Result<ListTasksRequest> ParseGrpcListTasksRequest(const lf::a2a::v1::ListTasksRequest& request);
void WriteGrpcListTasksResponse(const ListTasksResponse& payload, lf::a2a::v1::ListTasksResponse& response);
}  // namespace a2a::server
