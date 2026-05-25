// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "a2a/core/agent_card_builder.h"
#include "a2a/core/error.h"
#include "a2a/core/protocol_errors.h"
#include "a2a/core/task_states.h"
#include "a2a/server/server.h"
#include "a2a/v1/a2a.pb.h"
#include "example_constants.h"

namespace a2a::examples {

inline std::string UrlToTarget(std::string_view url) {
  const std::size_t scheme = url.find("://");
  if (scheme == std::string_view::npos) {
    return std::string(url);
  }

  const std::size_t path_start = url.find('/', scheme + 3);
  if (path_start == std::string_view::npos) {
    return "/";
  }
  return std::string(url.substr(path_start));
}

class SequenceStreamSession final : public server::ServerStreamSession {
 public:
  explicit SequenceStreamSession(std::vector<lf::a2a::v1::StreamResponse> events) : events_(std::move(events)) {}

  [[nodiscard]] core::Result<std::optional<lf::a2a::v1::StreamResponse>> Next() override {
    if (index_ >= events_.size()) {
      return std::optional<lf::a2a::v1::StreamResponse>{};
    }
    return std::optional<lf::a2a::v1::StreamResponse>{events_[index_++]};
  }

 private:
  std::vector<lf::a2a::v1::StreamResponse> events_;
  std::size_t index_ = 0;
};

class ExampleExecutor final : public server::AgentExecutor {
 public:
  core::Result<lf::a2a::v1::SendMessageResponse> SendMessage(const lf::a2a::v1::SendMessageRequest& request,
                                                             server::RequestContext& context) override {
    (void)context;
    if (!request.has_message() || request.message().parts_size() == 0) {
      return core::Error::Validation("message with at least one part is required");
    }
    const bool has_explicit_task_id = !request.message().task_id().empty();
    std::string task_id = request.message().task_id();
    if (task_id.empty()) {
      if (!request.message().message_id().empty()) {
        task_id = "task-" + request.message().message_id();
      } else {
        task_id = "example-task-auto-" + std::to_string(status_timestamp_counter_ + 1);
      }
    }
    if (has_explicit_task_id) {
      const auto existing_task = store_.Get(task_id);
      if (!existing_task.ok()) {
        return core::protocol_errors::TaskNotFound();
      }
      if (!request.message().context_id().empty() && !existing_task.value().context_id().empty() &&
          request.message().context_id() != existing_task.value().context_id()) {
        return core::protocol_errors::UnsupportedOperation("contextId does not match task");
      }
      if (core::IsTerminalTaskState(existing_task.value().status().state())) {
        return core::protocol_errors::UnsupportedOperation("task is already terminal");
      }
    }

    auto existing = store_.Get(task_id);
    lf::a2a::v1::Task task = existing.ok() ? existing.value() : lf::a2a::v1::Task{};
    if (!existing.ok()) {
      ordered_ids_.push_back(task_id);
    }
    task.set_id(task_id);
    if (task.context_id().empty()) {
      if (!request.message().context_id().empty()) {
        task.set_context_id(request.message().context_id());
      } else {
        task.set_context_id("ctx-" + task_id);
      }
    }
    task.mutable_status()->set_state(lf::a2a::v1::TASK_STATE_WORKING);
    task.mutable_status()->mutable_message()->set_role(lf::a2a::v1::ROLE_AGENT);
    task.mutable_status()->mutable_message()->set_message_id("status-" + task_id);
    task.mutable_status()->mutable_message()->add_parts()->set_text("ack");
    ++status_timestamp_counter_;
    task.mutable_status()->mutable_timestamp()->set_seconds(static_cast<int64_t>(status_timestamp_counter_));

    task.clear_artifacts();
    const std::string request_text = request.message().parts(0).text();
    std::string normalized_request_text;
    normalized_request_text.reserve(request_text.size());
    for (const char ch : request_text) {
      normalized_request_text.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    const std::string request_message_id = request.message().message_id();
    std::string normalized_message_id;
    normalized_message_id.reserve(request_message_id.size());
    for (const char ch : request_message_id) {
      normalized_message_id.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    std::string normalized_task_id;
    normalized_task_id.reserve(task_id.size());
    for (const char ch : task_id) {
      normalized_task_id.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }

    const bool wants_file_url_artifact = normalized_request_text.find("file_url_artifact") != std::string::npos ||
                                         normalized_request_text.find("file-url-artifact") != std::string::npos ||
                                         normalized_request_text.find("file url artifact") != std::string::npos ||
                                         normalized_request_text.find("file_url") != std::string::npos ||
                                         normalized_request_text.find("file-url") != std::string::npos ||
                                         normalized_message_id.find("file_url") != std::string::npos ||
                                         normalized_message_id.find("file-url") != std::string::npos ||
                                         normalized_task_id.find("file_url") != std::string::npos ||
                                         normalized_task_id.find("file-url") != std::string::npos ||
                                         (normalized_request_text.find("file") != std::string::npos &&
                                          normalized_request_text.find("url") != std::string::npos);
    const bool wants_file_artifact = normalized_request_text.find("file_artifact") != std::string::npos ||
                                     normalized_request_text.find("file-artifact") != std::string::npos ||
                                     normalized_request_text.find("file artifact") != std::string::npos ||
                                     normalized_message_id.find("file") != std::string::npos ||
                                     normalized_task_id.find("file") != std::string::npos ||
                                     normalized_request_text.find("output.txt") != std::string::npos;
    const bool wants_data_artifact = normalized_request_text.find("data_artifact") != std::string::npos ||
                                     normalized_request_text.find("data-artifact") != std::string::npos ||
                                     normalized_request_text.find("data artifact") != std::string::npos ||
                                     normalized_message_id.find("data") != std::string::npos ||
                                     normalized_task_id.find("data") != std::string::npos ||
                                     normalized_request_text.find("json") != std::string::npos ||
                                     normalized_request_text.find("structured data") != std::string::npos;
    const bool wants_message_response = normalized_request_text.find("message response") != std::string::npos ||
                                        normalized_request_text.find("return a message") != std::string::npos ||
                                        normalized_request_text.find("respond with message") != std::string::npos ||
                                        normalized_message_id.find("message-response") != std::string::npos ||
                                        normalized_message_id.find("message_response") != std::string::npos ||
                                        normalized_message_id.find("message") != std::string::npos ||
                                        normalized_task_id.find("message-response") != std::string::npos ||
                                        normalized_task_id.find("message_response") != std::string::npos ||
                                        normalized_task_id.find("message") != std::string::npos ||
                                        normalized_request_text.find("message with text") != std::string::npos;
    const bool wants_completed_task = normalized_message_id.find("complete-task") != std::string::npos ||
                                      normalized_message_id.find("complete_task") != std::string::npos ||
                                      normalized_task_id.find("complete-task") != std::string::npos ||
                                      normalized_task_id.find("complete_task") != std::string::npos ||
                                      normalized_request_text.find("complete task") != std::string::npos ||
                                      normalized_request_text.find("complete after history") != std::string::npos;
    const bool wants_input_required_task = normalized_message_id.find("input-required") != std::string::npos ||
                                           normalized_message_id.find("input_required") != std::string::npos ||
                                           normalized_task_id.find("input-required") != std::string::npos ||
                                           normalized_task_id.find("input_required") != std::string::npos ||
                                           normalized_request_text.find("input required") != std::string::npos;

    if (wants_completed_task) {
      task.mutable_status()->set_state(lf::a2a::v1::TASK_STATE_COMPLETED);
    } else if (wants_input_required_task) {
      task.mutable_status()->set_state(lf::a2a::v1::TASK_STATE_INPUT_REQUIRED);
    }

    auto* text_artifact = task.add_artifacts();
    text_artifact->set_artifact_id("artifact-text-" + task_id);
    text_artifact->set_name("text-artifact");
    text_artifact->add_parts()->set_text(std::string(constants::kGeneratedTextContent));

    auto* file_artifact = task.add_artifacts();
    file_artifact->set_artifact_id("artifact-file-" + task_id);
    file_artifact->set_name("file-artifact");
    auto* file_part = file_artifact->add_parts();
    file_part->set_raw("generated file content");
    file_part->set_filename(std::string(constants::kOutputFilename));
    file_part->set_media_type(std::string(constants::kTextPlainMediaType));

    auto* file_url_artifact = task.add_artifacts();
    file_url_artifact->set_artifact_id("artifact-file-url-" + task_id);
    file_url_artifact->set_name("file-url-artifact");
    auto* file_url_part = file_url_artifact->add_parts();
    file_url_part->set_url("https://example.test/output.txt");
    file_url_part->set_filename(std::string(constants::kOutputFilename));
    file_url_part->set_media_type(std::string(constants::kTextPlainMediaType));

    auto* data_artifact = task.add_artifacts();
    data_artifact->set_artifact_id("artifact-data-" + task_id);
    data_artifact->set_name("data-artifact");
    auto* data_part = data_artifact->add_parts();
    auto* data_fields = data_part->mutable_data()->mutable_struct_value()->mutable_fields();
    (*data_fields)["key"].set_string_value("value");
    (*data_fields)["count"].set_number_value(42);
    if (wants_file_url_artifact) {
      std::swap((*task.mutable_artifacts())[0], (*task.mutable_artifacts())[2]);
    } else if (wants_file_artifact) {
      std::swap((*task.mutable_artifacts())[0], (*task.mutable_artifacts())[1]);
    } else if (wants_data_artifact) {
      std::swap((*task.mutable_artifacts())[0], (*task.mutable_artifacts())[3]);
    }

    const auto stored = lifecycle_.CreateOrUpdateTask(task);
    if (!stored.ok()) {
      return stored.error();
    }
    const auto append =
        lifecycle_.AppendHistory(task_id, request.message(), server::TaskStore::HistoryAppendPolicy::kDedupByMessageId);
    if (!append.ok()) {
      return append.error();
    }
    task = append.value();

    lf::a2a::v1::SendMessageResponse response;
    response.mutable_message()->set_role(lf::a2a::v1::ROLE_AGENT);
    response.mutable_message()->set_message_id("response-" + task_id);
    response.mutable_message()->set_task_id(task_id);
    response.mutable_message()->set_context_id(task.context_id());
    response.mutable_message()->add_parts()->set_text(wants_message_response ? "Direct message response" : "ack");
    if (wants_message_response) {
      // Keep message payload set.
    } else {
      if (request.has_configuration() && request.configuration().has_history_length()) {
        const int keep = request.configuration().history_length();
        server::ApplyHistoryRetention(&task, keep <= 0 ? std::optional<std::size_t>{0}
                                                       : std::optional<std::size_t>{static_cast<std::size_t>(keep)});
      }
      *response.mutable_task() = task;
    }
    return response;
  }

  core::Result<std::unique_ptr<server::ServerStreamSession>> SendStreamingMessage(
      const lf::a2a::v1::SendMessageRequest& request, server::RequestContext& context) override {
    (void)context;
    std::string task_id = request.message().task_id();
    if (task_id.empty()) {
      if (!request.message().message_id().empty()) {
        task_id = "task-" + request.message().message_id();
      } else {
        task_id = "example-stream-default";
      }
    }

    if (!store_.Get(task_id).ok()) {
      lf::a2a::v1::Task task;
      task.set_id(task_id);
      task.set_context_id("ctx-" + task_id);
      task.mutable_status()->set_state(lf::a2a::v1::TASK_STATE_WORKING);
      (void)lifecycle_.CreateOrUpdateTask(task);
      ordered_ids_.push_back(task_id);
    }

    lf::a2a::v1::StreamResponse working;
    working.mutable_status_update()->set_task_id(task_id);
    working.mutable_status_update()->set_context_id(store_.Get(task_id).value().context_id());
    working.mutable_status_update()->mutable_status()->set_state(lf::a2a::v1::TASK_STATE_WORKING);

    lf::a2a::v1::StreamResponse completed;
    completed.mutable_status_update()->set_task_id(task_id);
    completed.mutable_status_update()->set_context_id(store_.Get(task_id).value().context_id());
    completed.mutable_status_update()->mutable_status()->set_state(lf::a2a::v1::TASK_STATE_COMPLETED);

    std::vector<lf::a2a::v1::StreamResponse> events;
    events.push_back(working);
    events.push_back(completed);

    std::unique_ptr<server::ServerStreamSession> stream = std::make_unique<SequenceStreamSession>(std::move(events));
    return stream;
  }

  core::Result<lf::a2a::v1::Task> GetTask(const lf::a2a::v1::GetTaskRequest& request,
                                          server::RequestContext& context) override {
    (void)context;
    auto task_result = store_.Get(request.id());
    if (!task_result.ok()) return task_result.error();
    lf::a2a::v1::Task task = task_result.value();
    if (request.has_history_length())
      server::ApplyHistoryRetention(
          &task, request.history_length() <= 0
                     ? std::optional<std::size_t>{0}
                     : std::optional<std::size_t>{static_cast<std::size_t>(request.history_length())});
    return task;
  }

  core::Result<server::ListTasksResponse> ListTasks(const server::ListTasksRequest& request,
                                                    server::RequestContext& context) override {
    (void)context;
    return store_.List(request);
  }

  core::Result<lf::a2a::v1::Task> CancelTask(const lf::a2a::v1::CancelTaskRequest& request,
                                             server::RequestContext& context) override {
    (void)context;
    return lifecycle_.TransitionTaskStatus(request.id(), lf::a2a::v1::TASK_STATE_CANCELED);
  }

 private:
  std::vector<std::string> ordered_ids_;
  server::InMemoryTaskStore store_;
  server::TaskLifecycleService lifecycle_{&store_};
  std::uint64_t status_timestamp_counter_ = 0;
};

inline lf::a2a::v1::AgentCard BuildRestAgentCard(std::string_view name, std::string_view url) {
  return a2a::core::AgentCardBuilder::RestPreset(name, url).Build();
}

inline lf::a2a::v1::AgentCard BuildJsonRpcAgentCard(std::string_view name, std::string_view url) {
  return a2a::core::AgentCardBuilder::JsonRpcPreset(name, url).Build();
}

}  // namespace a2a::examples
