// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

#include <google/protobuf/timestamp.pb.h>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "a2a/core/result.h"
#include "a2a/v1/a2a.pb.h"

namespace a2a::server {

struct ListTasksRequest final {
  std::size_t page_size = 0;
  std::string page_token;
  std::string context_id;
  std::optional<lf::a2a::v1::TaskState> status_filter;
  std::optional<google::protobuf::Timestamp> status_timestamp_after;
  std::optional<std::size_t> history_length;
  bool include_artifacts = false;

  ListTasksRequest() noexcept = default;

  ListTasksRequest(std::size_t page_size_value, std::string page_token_value)
      : page_size(page_size_value), page_token(std::move(page_token_value)) {}

  ListTasksRequest(const ListTasksRequest& other)
      : page_size(other.page_size),
        page_token(other.page_token),
        context_id(other.context_id),
        status_filter(other.status_filter),
        status_timestamp_after(other.status_timestamp_after),
        history_length(other.history_length),
        include_artifacts(other.include_artifacts) {}

  ListTasksRequest& operator=(const ListTasksRequest& other) {
    if (this != &other) {
      page_size = other.page_size;
      page_token = other.page_token;
      context_id = other.context_id;
      status_filter = other.status_filter;
      status_timestamp_after = other.status_timestamp_after;
      history_length = other.history_length;
      include_artifacts = other.include_artifacts;
    }
    return *this;
  }

  ListTasksRequest(ListTasksRequest&& other) noexcept
      : page_size(other.page_size),
        page_token(std::move(other.page_token)),
        context_id(std::move(other.context_id)),
        status_filter(other.status_filter),
        status_timestamp_after(std::move(other.status_timestamp_after)),
        history_length(other.history_length),
        include_artifacts(other.include_artifacts) {
    other.page_size = 0;
    other.include_artifacts = false;
  }

  ListTasksRequest& operator=(ListTasksRequest&& other) noexcept {
    if (this != &other) {
      page_size = other.page_size;
      page_token = std::move(other.page_token);
      context_id = std::move(other.context_id);
      status_filter = other.status_filter;
      status_timestamp_after = std::move(other.status_timestamp_after);
      history_length = other.history_length;
      include_artifacts = other.include_artifacts;
      other.page_size = 0;
      other.include_artifacts = false;
    }
    return *this;
  }
};

struct ListTasksResponse final {
  std::vector<lf::a2a::v1::Task> tasks;
  std::size_t page_size = 0;
  std::size_t total_size = 0;
  std::string next_page_token;
};

[[nodiscard]] bool MatchesListFilters(const lf::a2a::v1::Task& task, const ListTasksRequest& request);
[[nodiscard]] core::Result<std::size_t> ParseListPageToken(std::string_view page_token);
[[nodiscard]] core::Result<void> ValidateListPageOffset(std::size_t offset, std::size_t size);
void ApplyArtifactProjection(lf::a2a::v1::Task* task, bool include_artifacts);

}  // namespace a2a::server
