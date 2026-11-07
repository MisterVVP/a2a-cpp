// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "server/transport/grpc_task_conversion.h"

#include <cstdint>
#include <optional>

namespace a2a::server {
namespace {
constexpr auto kInvalidListHistory = "ListTasksRequest.history_length must be non-negative";
}
core::Result<ListTasksRequest> ParseGrpcListTasksRequest(const lf::a2a::v1::ListTasksRequest& request) {
  ListTasksRequest list_request;
  const auto page_size = NormalizeListTasksPageSize(
      request.has_page_size() ? std::optional<std::int64_t>{request.page_size()} : std::nullopt);
  if (!page_size.ok()) {
    return page_size.error();
  }
  list_request.page_size = page_size.value();
  list_request.page_token = request.page_token();
  list_request.context_id = request.context_id();
  if (request.status() != lf::a2a::v1::TASK_STATE_UNSPECIFIED) {
    list_request.status_filter = request.status();
  }
  if (request.has_history_length()) {
    if (request.history_length() < 0) {
      return core::Error::Validation(kInvalidListHistory);
    }
    list_request.history_length = static_cast<std::size_t>(request.history_length());
  }
  if (request.has_include_artifacts()) {
    list_request.include_artifacts = request.include_artifacts();
  }
  if (request.has_status_timestamp_after()) {
    list_request.status_timestamp_after = request.status_timestamp_after();
  }

  return list_request;
}
void WriteGrpcListTasksResponse(const ListTasksResponse& payload, lf::a2a::v1::ListTasksResponse& response) {
  for (const auto& task : payload.tasks) {
    *response.add_tasks() = task;
  }
  response.set_page_size(static_cast<int32_t>(payload.page_size));
  response.set_total_size(static_cast<int32_t>(payload.total_size));
  response.set_next_page_token(payload.next_page_token);
}
}  // namespace a2a::server
