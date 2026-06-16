// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

#include <cstddef>
#include <optional>

#include "a2a/server/tasks/task_store.h"
#include "a2a/v1/a2a.pb.h"

namespace a2a::server {

void ApplyHistoryRetention(lf::a2a::v1::Task* task, std::optional<std::size_t> history_length);

[[nodiscard]] std::optional<TaskStore::HistoryDedupeEvent::Reason> FindHistoryDedupeReason(
    const google::protobuf::RepeatedPtrField<lf::a2a::v1::Message>& history, const lf::a2a::v1::Message& message,
    TaskStore::HistoryAppendPolicy policy);

void UpdateDedupeSnapshot(TaskStore::HistoryTelemetrySnapshot* snapshot, TaskStore::HistoryDedupeEvent::Reason reason);

}  // namespace a2a::server
