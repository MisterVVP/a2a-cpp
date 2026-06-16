// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "a2a/server/tasks/task_ordering.h"

#include <algorithm>
#include <cstdint>
#include <ranges>

namespace a2a::server {

void TimestampDescTaskOrdering::Sort(std::vector<const lf::a2a::v1::Task*>* tasks) {
  std::ranges::stable_sort(*tasks, [](const lf::a2a::v1::Task* lhs, const lf::a2a::v1::Task* rhs) {
    const int64_t lhs_seconds = lhs->status().has_timestamp() ? lhs->status().timestamp().seconds() : 0;
    const int64_t rhs_seconds = rhs->status().has_timestamp() ? rhs->status().timestamp().seconds() : 0;
    if (lhs_seconds != rhs_seconds) {
      return lhs_seconds > rhs_seconds;
    }
    const int32_t lhs_nanos = lhs->status().has_timestamp() ? lhs->status().timestamp().nanos() : 0;
    const int32_t rhs_nanos = rhs->status().has_timestamp() ? rhs->status().timestamp().nanos() : 0;
    return lhs_nanos > rhs_nanos;
  });
}

}  // namespace a2a::server
