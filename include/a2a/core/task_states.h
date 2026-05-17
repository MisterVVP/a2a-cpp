#pragma once

#include "a2a/v1/a2a.pb.h"

namespace a2a::core {

[[nodiscard]] inline bool IsTerminalTaskState(lf::a2a::v1::TaskState state) {
  switch (state) {
    case lf::a2a::v1::TASK_STATE_COMPLETED:
    case lf::a2a::v1::TASK_STATE_FAILED:
    case lf::a2a::v1::TASK_STATE_CANCELED:
    case lf::a2a::v1::TASK_STATE_REJECTED:
      return true;
    case lf::a2a::v1::TASK_STATE_UNSPECIFIED:
    case lf::a2a::v1::TASK_STATE_SUBMITTED:
    case lf::a2a::v1::TASK_STATE_WORKING:
    case lf::a2a::v1::TASK_STATE_INPUT_REQUIRED:
    case lf::a2a::v1::TASK_STATE_AUTH_REQUIRED:
      return false;
  }
  return false;
}

}  // namespace a2a::core
