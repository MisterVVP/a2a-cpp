// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

#include <vector>

#include "a2a/v1/a2a.pb.h"

namespace a2a::server {

class TimestampDescTaskOrdering final {
 public:
  static void Sort(std::vector<const lf::a2a::v1::Task*>* tasks);
};

}  // namespace a2a::server
