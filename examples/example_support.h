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
#include "a2a/core/response_builders.h"
#include "a2a/core/task_states.h"
#include "a2a/core/url_utils.h"
#include "a2a/server/server.h"
#include "a2a/v1/a2a.pb.h"
#include "example_constants.h"
#include "example_intent.h"

namespace a2a::examples {

inline std::string UrlToTarget(std::string_view url) { return core::ExtractTargetPath(url); }

namespace {

constexpr std::string_view kGeneratedFileContent = "generated file content";
constexpr std::string_view kOutputFileUrl = "https://example.test/output.txt";
constexpr std::string_view kStructuredDataKey = "key";
constexpr std::string_view kStructuredDataValue = "value";
constexpr std::string_view kStructuredDataCountKey = "count";
constexpr double kStructuredDataCount = 42.0;

}  // namespace

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
    if (!request.has_message() || request.message().parts_size() == 0) {
      return core::Error::Validation("message with at least one part is required");
    }
    auto task_id_result = lifecycle_.ResolveTaskIdForSendRequest(request, context);
    if (!task_id_result.ok()) {
      return task_id_result.error();
    }
    std::string task_id = task_id_result.value();

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
    const ExampleIntent interop_intent = ExtractExampleIntent(request, task_id);

    if (interop_intent.terminal_state == lf::a2a::v1::TASK_STATE_COMPLETED) {
      task.mutable_status()->set_state(lf::a2a::v1::TASK_STATE_COMPLETED);
    } else if (interop_intent.terminal_state == lf::a2a::v1::TASK_STATE_INPUT_REQUIRED) {
      task.mutable_status()->set_state(lf::a2a::v1::TASK_STATE_INPUT_REQUIRED);
    }

    google::protobuf::Value structured_data;
    auto* data_fields = structured_data.mutable_struct_value()->mutable_fields();
    (*data_fields)[std::string{kStructuredDataKey}].set_string_value(std::string{kStructuredDataValue});
    (*data_fields)[std::string{kStructuredDataCountKey}].set_number_value(kStructuredDataCount);

    const auto text_artifact = core::ResponseBuilders::TextArtifact(
        constants::kGeneratedTextContent, {.artifact_id = "artifact-text-" + task_id, .name = "text-artifact"});
    const auto file_artifact =
        core::ResponseBuilders::RawFileArtifact(kGeneratedFileContent,
                                                {.filename = std::string(constants::kOutputFilename),
                                                 .media_type = std::string(constants::kTextPlainMediaType)},
                                                {.artifact_id = "artifact-file-" + task_id, .name = "file-artifact"});
    const auto file_url_artifact = core::ResponseBuilders::FileUrlArtifact(
        kOutputFileUrl,
        {.filename = std::string(constants::kOutputFilename),
         .media_type = std::string(constants::kTextPlainMediaType)},
        {.artifact_id = "artifact-file-url-" + task_id, .name = "file-url-artifact"});
    const auto data_artifact = core::ResponseBuilders::StructuredDataArtifact(
        structured_data, {.artifact_id = "artifact-data-" + task_id, .name = "data-artifact"});

    if (interop_intent.primary_artifact == ExamplePrimaryArtifactType::kFileUrl) {
      core::ResponseBuilders::AddArtifactsWithPrimary(&task, file_url_artifact,
                                                      {text_artifact, file_artifact, data_artifact});
    } else if (interop_intent.primary_artifact == ExamplePrimaryArtifactType::kFile) {
      core::ResponseBuilders::AddArtifactsWithPrimary(&task, file_artifact,
                                                      {text_artifact, file_url_artifact, data_artifact});
    } else if (interop_intent.primary_artifact == ExamplePrimaryArtifactType::kData) {
      core::ResponseBuilders::AddArtifactsWithPrimary(&task, data_artifact,
                                                      {text_artifact, file_artifact, file_url_artifact});
    } else {
      core::ResponseBuilders::AddArtifactsWithPrimary(&task, text_artifact,
                                                      {file_artifact, file_url_artifact, data_artifact});
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
    const bool wants_message_response = interop_intent.response_mode == ExampleResponseMode::kMessage;
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
    std::string task_id = request.has_message() ? request.message().task_id() : "";
    if (task_id.empty()) {
      if (request.has_message() && !request.message().message_id().empty()) {
        auto task_id_result = lifecycle_.ResolveTaskIdForSendRequest(request, context);
        if (!task_id_result.ok()) {
          return task_id_result.error();
        }
        task_id = task_id_result.value();
      } else {
        task_id = "task-test-stream-default";
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
  std::shared_ptr<server::TaskIdGenerator> task_id_generator_{std::make_shared<server::SequentialTaskIdGenerator>()};
  server::TaskLifecycleService lifecycle_{&store_, task_id_generator_};
  std::uint64_t status_timestamp_counter_ = 0;
};

inline lf::a2a::v1::AgentCard BuildRestAgentCard(std::string_view name, std::string_view url) {
  return a2a::core::AgentCardBuilder::RestPreset(name, url).Build();
}

inline lf::a2a::v1::AgentCard BuildJsonRpcAgentCard(std::string_view name, std::string_view url) {
  return a2a::core::AgentCardBuilder::JsonRpcPreset(name, url).Build();
}

}  // namespace a2a::examples
