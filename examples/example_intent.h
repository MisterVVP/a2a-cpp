// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

#include <cstdint>
#include <string_view>

#include "a2a/v1/a2a.pb.h"

namespace a2a::examples {

enum class ExamplePrimaryArtifactType : std::uint8_t { kText = 0, kFile = 1, kFileUrl = 2, kData = 3 };
enum class ExampleResponseMode : std::uint8_t { kTask = 0, kMessage = 1 };

struct ExampleIntent final {
  ExamplePrimaryArtifactType primary_artifact = ExamplePrimaryArtifactType::kText;
  ExampleResponseMode response_mode = ExampleResponseMode::kTask;
  lf::a2a::v1::TaskState terminal_state = lf::a2a::v1::TASK_STATE_WORKING;
};

inline ExampleIntent ExtractExampleIntent(const lf::a2a::v1::SendMessageRequest& request, std::string_view task_id) {
  static constexpr std::string_view kEmpty = "";
  static constexpr std::string_view kPrefixArtifactFileUrl = "artifact-file-url";
  static constexpr std::string_view kPrefixArtifactFile = "artifact-file";
  static constexpr std::string_view kPrefixArtifactData = "artifact-data";
  static constexpr std::string_view kPrefixMessageResponse = "message-response";
  static constexpr std::string_view kPrefixCompleteTask = "complete-task";
  static constexpr std::string_view kPrefixInputRequired = "input-required";

  const std::string_view message_id = request.has_message() ? request.message().message_id() : kEmpty;

  ExampleIntent intent;
  if (message_id.find(kPrefixArtifactFileUrl) != std::string_view::npos) {
    intent.primary_artifact = ExamplePrimaryArtifactType::kFileUrl;
  } else if (message_id.find(kPrefixArtifactFile) != std::string_view::npos) {
    intent.primary_artifact = ExamplePrimaryArtifactType::kFile;
  } else if (message_id.find(kPrefixArtifactData) != std::string_view::npos) {
    intent.primary_artifact = ExamplePrimaryArtifactType::kData;
  }

  if (message_id.find(kPrefixMessageResponse) != std::string_view::npos) {
    intent.response_mode = ExampleResponseMode::kMessage;
  }

  if (message_id.find(kPrefixCompleteTask) != std::string_view::npos ||
      task_id.find(kPrefixCompleteTask) != std::string_view::npos) {
    intent.terminal_state = lf::a2a::v1::TASK_STATE_COMPLETED;
  } else if (message_id.find(kPrefixInputRequired) != std::string_view::npos ||
             task_id.find(kPrefixInputRequired) != std::string_view::npos) {
    intent.terminal_state = lf::a2a::v1::TASK_STATE_INPUT_REQUIRED;
  }

  return intent;
}

}  // namespace a2a::examples
