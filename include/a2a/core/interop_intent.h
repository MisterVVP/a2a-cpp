// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>

#include "a2a/v1/a2a.pb.h"

namespace a2a::core {

enum class InteropPrimaryArtifactType : std::uint8_t { kText = 0, kFile = 1, kFileUrl = 2, kData = 3 };
enum class InteropResponseMode : std::uint8_t { kTask = 0, kMessage = 1 };

struct InteropIntent final {
  InteropPrimaryArtifactType primary_artifact = InteropPrimaryArtifactType::kText;
  InteropResponseMode response_mode = InteropResponseMode::kTask;
  lf::a2a::v1::TaskState terminal_state = lf::a2a::v1::TASK_STATE_WORKING;
};

namespace detail {

inline std::string ToLower(std::string_view source) {
  std::string lowered;
  lowered.reserve(source.size());
  for (const char ch : source) {
    lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
  }
  return lowered;
}

inline bool ContainsAny(std::string_view source, std::initializer_list<std::string_view> needles) {
  return std::any_of(needles.begin(), needles.end(), [&source](const std::string_view needle) {
    return source.find(needle) != std::string_view::npos;
  });
}

}  // namespace detail

inline InteropIntent ExtractInteropIntent(const lf::a2a::v1::SendMessageRequest& request, std::string_view task_id) {
  static constexpr std::string_view kEmpty = "";
  static constexpr std::string_view kRequestPartContainsFile = "file";
  static constexpr std::string_view kRequestPartContainsUrl = "url";
  static constexpr std::array<std::string_view, 8> kFileUrlNeedles = {
      "file_url_artifact", "file-url-artifact", "file url artifact", "file_url", "file-url", "file_url",
      "file-url", "output.txt"};
  static constexpr std::array<std::string_view, 6> kFileNeedles = {
      "file_artifact", "file-artifact", "file artifact", "file", "file", "output.txt"};
  static constexpr std::array<std::string_view, 7> kDataNeedles = {
      "data_artifact", "data-artifact", "data artifact", "data", "data", "json", "structured data"};
  static constexpr std::array<std::string_view, 10> kMessageResponseNeedles = {
      "message response", "return a message", "respond with message", "message-response", "message_response",
      "message",          "message-response",   "message_response",     "message",          "message with text"};
  static constexpr std::array<std::string_view, 6> kCompletedNeedles = {
      "complete-task", "complete_task", "complete-task", "complete_task", "complete task", "complete after history"};
  static constexpr std::array<std::string_view, 5> kInputRequiredNeedles = {
      "input-required", "input_required", "input-required", "input_required", "input required"};

  const std::string request_text =
      (request.has_message() && request.message().parts_size() > 0) ? request.message().parts(0).text() : std::string{kEmpty};
  const std::string message_id = request.has_message() ? request.message().message_id() : std::string{kEmpty};

  const std::string normalized_request_text = detail::ToLower(request_text);
  const std::string normalized_message_id = detail::ToLower(message_id);
  const std::string normalized_task_id = detail::ToLower(task_id);

  const bool wants_file_url_artifact =
      detail::ContainsAny(normalized_request_text, {kFileUrlNeedles[0], kFileUrlNeedles[1], kFileUrlNeedles[2], kFileUrlNeedles[3],
                                                    kFileUrlNeedles[4]}) ||
      detail::ContainsAny(normalized_message_id, {kFileUrlNeedles[5], kFileUrlNeedles[6]}) ||
      detail::ContainsAny(normalized_task_id, {kFileUrlNeedles[5], kFileUrlNeedles[6]}) ||
      (normalized_request_text.find(kRequestPartContainsFile) != std::string::npos &&
       normalized_request_text.find(kRequestPartContainsUrl) != std::string::npos);
  const bool wants_file_artifact = detail::ContainsAny(normalized_request_text,
                                                       {kFileNeedles[0], kFileNeedles[1], kFileNeedles[2], kFileNeedles[5]}) ||
                                   detail::ContainsAny(normalized_message_id, {kFileNeedles[3]}) ||
                                   detail::ContainsAny(normalized_task_id, {kFileNeedles[4]});
  const bool wants_data_artifact = detail::ContainsAny(normalized_request_text,
                                                       {kDataNeedles[0], kDataNeedles[1], kDataNeedles[2], kDataNeedles[5],
                                                        kDataNeedles[6]}) ||
                                   detail::ContainsAny(normalized_message_id, {kDataNeedles[3]}) ||
                                   detail::ContainsAny(normalized_task_id, {kDataNeedles[4]});

  InteropIntent intent;
  if (wants_file_url_artifact) {
    intent.primary_artifact = InteropPrimaryArtifactType::kFileUrl;
  } else if (wants_file_artifact) {
    intent.primary_artifact = InteropPrimaryArtifactType::kFile;
  } else if (wants_data_artifact) {
    intent.primary_artifact = InteropPrimaryArtifactType::kData;
  }

  const bool wants_message_response = detail::ContainsAny(
                                          normalized_request_text,
                                          {kMessageResponseNeedles[0], kMessageResponseNeedles[1], kMessageResponseNeedles[2],
                                           kMessageResponseNeedles[9]}) ||
                                      detail::ContainsAny(normalized_message_id,
                                                          {kMessageResponseNeedles[3], kMessageResponseNeedles[4],
                                                           kMessageResponseNeedles[5]}) ||
                                      detail::ContainsAny(normalized_task_id,
                                                          {kMessageResponseNeedles[6], kMessageResponseNeedles[7],
                                                           kMessageResponseNeedles[8]});
  if (wants_message_response) {
    intent.response_mode = InteropResponseMode::kMessage;
  }

  if (detail::ContainsAny(normalized_message_id,
                          {kCompletedNeedles[0], kCompletedNeedles[1]}) ||
      detail::ContainsAny(normalized_task_id, {kCompletedNeedles[2], kCompletedNeedles[3]}) ||
      detail::ContainsAny(normalized_request_text, {kCompletedNeedles[4], kCompletedNeedles[5]})) {
    intent.terminal_state = lf::a2a::v1::TASK_STATE_COMPLETED;
  } else if (detail::ContainsAny(normalized_message_id,
                                 {kInputRequiredNeedles[0], kInputRequiredNeedles[1]}) ||
             detail::ContainsAny(normalized_task_id, {kInputRequiredNeedles[2], kInputRequiredNeedles[3]}) ||
             detail::ContainsAny(normalized_request_text, {kInputRequiredNeedles[4]})) {
    intent.terminal_state = lf::a2a::v1::TASK_STATE_INPUT_REQUIRED;
  }

  return intent;
}

}  // namespace a2a::core
