// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "a2a/core/agent_card/agent_card_builder.h"
#include "a2a/core/error.h"
#include "a2a/core/protocol_codes.h"
#include "a2a/core/protocol_errors.h"
#include "a2a/core/response_builders.h"
#include "a2a/core/task_states.h"
#include "a2a/core/url_utils.h"
#include "a2a/server/agent_executor.h"
#include "a2a/server/push_notification_delivery.h"
#include "a2a/server/push_notification_service.h"
#include "a2a/server/push_notification_store.h"
#include "a2a/server/request_context.h"
#include "a2a/server/server_stream_session.h"
#include "a2a/server/task_id_generator.h"
#include "a2a/server/task_subscription_service.h"
#include "a2a/server/tasks/in_memory_task_store.h"
#include "a2a/server/tasks/list_tasks.h"
#include "a2a/server/tasks/task_history.h"
#include "a2a/server/tasks/task_lifecycle_service.h"
#include "a2a/server/tasks/task_store.h"
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
constexpr std::string_view kStreamingTaskIdPrefix = "task-stream-";
constexpr std::size_t kTaskLockStripeCount = 64U;
constexpr std::size_t kConditionalWriteRetryLimit = 8U;
constexpr std::string_view kConditionalWriteRetryExhaustedMessage = "conditional task persistence retry limit exceeded";
constexpr std::string_view kExampleTaskNotFoundMessage = "Task not found";

[[nodiscard]] bool IsTaskNotFoundError(const core::Error& error) {
  return error.code() == core::ErrorCode::kRemoteProtocol && error.protocol_code().has_value() &&
         *error.protocol_code() == std::string(core::protocol_codes::kTaskNotFound);
}

}  // namespace

struct ExampleExecutorOptions final {
  server::TaskStore* task_store = nullptr;
  server::PushNotificationStore* push_store = nullptr;
  server::PushNotificationDeliveryClient* push_delivery = nullptr;
  std::shared_ptr<server::TaskIdGenerator> task_id_generator;
};

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
  explicit ExampleExecutor(ExampleExecutorOptions options = {})
      : owned_task_store_(options.task_store == nullptr ? std::make_unique<server::InMemoryTaskStore>() : nullptr),
        owned_push_store_(options.push_store == nullptr ? std::make_unique<server::InMemoryPushNotificationStore>()
                                                        : nullptr),
        owned_push_delivery_(options.push_delivery == nullptr
                                 ? std::make_unique<server::HttpPushNotificationDeliveryClient>()
                                 : nullptr),
        task_store_(options.task_store == nullptr ? owned_task_store_.get() : options.task_store),
        push_store_(options.push_store == nullptr ? owned_push_store_.get() : options.push_store),
        push_delivery_(options.push_delivery == nullptr ? owned_push_delivery_.get() : options.push_delivery),
        task_id_generator_(options.task_id_generator == nullptr ? std::make_shared<server::UuidV7TaskIdGenerator>()
                                                                : std::move(options.task_id_generator)),
        lifecycle_(task_store_, task_id_generator_),
        push_notifications_(task_store_, push_store_, push_delivery_) {}

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
    std::lock_guard task_lock(TaskMutex(task_id));

    const bool supports_conditional_writes = task_store_->SupportsConditionalWrites();
    const bool generated_task_id = request.message().task_id().empty();
    for (std::size_t attempt = 0; attempt < kConditionalWriteRetryLimit; ++attempt) {
      std::uint64_t expected_revision = 0;
      core::Result<lf::a2a::v1::Task> existing =
          core::protocol_errors::TaskNotFound(std::string(kExampleTaskNotFoundMessage));
      const bool try_new_task_without_read = supports_conditional_writes && generated_task_id && attempt == 0U;
      if (supports_conditional_writes && !try_new_task_without_read) {
        auto snapshot = task_store_->GetMutationSnapshot(task_id);
        if (snapshot.ok()) {
          expected_revision = snapshot.value().revision;
          existing = std::move(snapshot.value().task);
        } else {
          existing = snapshot.error();
        }
      } else if (!supports_conditional_writes) {
        existing = task_store_->Get(task_id);
      }
      lf::a2a::v1::Task task;
      if (existing.ok()) {
        const auto validated = lifecycle_.ValidateTaskForSendRequest(request, existing.value());
        if (!validated.ok()) {
          return validated.error();
        }
        task = existing.value();
      } else {
        if (!IsTaskNotFoundError(existing.error())) {
          return existing.error();
        }
        if (!generated_task_id) {
          return core::protocol_errors::TaskNotFound();
        }
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
      const std::uint64_t status_timestamp = status_timestamp_counter_.fetch_add(1, std::memory_order_relaxed) + 1;
      task.mutable_status()->mutable_timestamp()->set_seconds(static_cast<int64_t>(status_timestamp));

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

      constexpr auto kHistoryPolicy = server::TaskStore::HistoryAppendPolicy::kDedupByMessageId;
      const bool duplicate_history =
          server::FindHistoryDedupeReason(task.history(), request.message(), kHistoryPolicy).has_value();
      if (supports_conditional_writes && !duplicate_history) {
        *task.add_history() = request.message();
      }
      if (supports_conditional_writes) {
        const auto stored = generated_task_id && expected_revision == 0U
                                ? task_store_->CreateGeneratedTaskIfAbsent(task)
                                : task_store_->CreateOrUpdateIfRevision(task, expected_revision);
        if (!stored.ok()) {
          return stored.error();
        }
        if (stored.value() == server::TaskStore::ConditionalWriteResult::kConflict) {
          continue;
        }
      } else {
        const auto stored = task_store_->CreateOrUpdate(task);
        if (!stored.ok()) {
          return stored.error();
        }
      }
      const auto register_push = push_notifications_.RegisterInlineConfigIfPresent(request, task_id);
      if (!register_push.ok()) {
        return register_push.error();
      }
      if (duplicate_history || !supports_conditional_writes) {
        const auto append = task_store_->AppendTaskHistory(task_id, request.message(), kHistoryPolicy);
        if (!append.ok()) {
          return append.error();
        }
        task = append.value();
      }
      const auto notify = push_notifications_.NotifyTaskUpdated(task);
      if (!notify.ok()) {
        return notify.error();
      }
      subscriptions_.PublishTaskUpdated(task);

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
    return core::Error::Internal(std::string(kConditionalWriteRetryExhaustedMessage));
  }

  core::Result<std::unique_ptr<server::ServerStreamSession>> SendStreamingMessage(
      const lf::a2a::v1::SendMessageRequest& request, server::RequestContext& context) override {
    (void)context;
    std::string task_id = request.has_message() ? request.message().task_id() : "";
    if (task_id.empty()) {
      if (request.has_message() && !request.message().message_id().empty()) {
        task_id.reserve(kStreamingTaskIdPrefix.size() + request.message().message_id().size());
        task_id.append(kStreamingTaskIdPrefix);
        task_id.append(request.message().message_id());
      } else {
        task_id = "task-test-stream-default";
      }
    }

    auto existing = task_store_->Get(task_id);
    lf::a2a::v1::Task task;
    if (existing.ok()) {
      task = existing.value();
    } else {
      if (!IsTaskNotFoundError(existing.error())) {
        return existing.error();
      }
      task.set_id(task_id);
      task.set_context_id("ctx-" + task_id);
      task.mutable_status()->set_state(lf::a2a::v1::TASK_STATE_WORKING);
      const auto stored = lifecycle_.CreateOrUpdateTask(task);
      if (!stored.ok()) {
        return stored.error();
      }
    }

    lf::a2a::v1::StreamResponse working;
    working.mutable_status_update()->set_task_id(task_id);
    working.mutable_status_update()->set_context_id(task.context_id());
    working.mutable_status_update()->mutable_status()->set_state(lf::a2a::v1::TASK_STATE_WORKING);

    lf::a2a::v1::StreamResponse completed;
    completed.mutable_status_update()->set_task_id(task_id);
    completed.mutable_status_update()->set_context_id(task.context_id());
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
    auto task_result = task_store_->Get(request.id());
    if (!task_result.ok()) return task_result.error();
    lf::a2a::v1::Task task = task_result.value();
    if (request.has_history_length())
      server::ApplyHistoryRetention(
          &task, request.history_length() <= 0
                     ? std::optional<std::size_t>{0}
                     : std::optional<std::size_t>{static_cast<std::size_t>(request.history_length())});
    return task;
  }

  core::Result<std::unique_ptr<server::ServerStreamSession>> SubscribeTask(const lf::a2a::v1::GetTaskRequest& request,
                                                                           server::RequestContext& context) override {
    (void)context;
    auto task = task_store_->Get(request.id());
    if (!task.ok()) {
      return task.error();
    }
    return subscriptions_.Subscribe(task.value());
  }

  core::Result<server::ListTasksResponse> ListTasks(const server::ListTasksRequest& request,
                                                    server::RequestContext& context) override {
    (void)context;
    return task_store_->List(request);
  }

  core::Result<lf::a2a::v1::Task> CancelTask(const lf::a2a::v1::CancelTaskRequest& request,
                                             server::RequestContext& context) override {
    (void)context;
    std::lock_guard task_lock(TaskMutex(request.id()));
    auto task = lifecycle_.TransitionTaskStatus(request.id(), lf::a2a::v1::TASK_STATE_CANCELED);
    if (!task.ok()) {
      return task.error();
    }
    const auto notify = push_notifications_.NotifyTaskUpdated(task.value());
    if (!notify.ok()) {
      return notify.error();
    }
    subscriptions_.PublishTaskUpdated(task.value());
    return task.value();
  }

  core::Result<lf::a2a::v1::TaskPushNotificationConfig> CreateTaskPushNotificationConfig(
      const lf::a2a::v1::TaskPushNotificationConfig& request, server::RequestContext& context) override {
    (void)context;
    return push_notifications_.CreateConfig(request);
  }

  core::Result<lf::a2a::v1::TaskPushNotificationConfig> GetTaskPushNotificationConfig(
      const lf::a2a::v1::GetTaskPushNotificationConfigRequest& request, server::RequestContext& context) override {
    (void)context;
    return push_notifications_.GetConfig(request);
  }

  core::Result<lf::a2a::v1::ListTaskPushNotificationConfigsResponse> ListTaskPushNotificationConfigs(
      const lf::a2a::v1::ListTaskPushNotificationConfigsRequest& request, server::RequestContext& context) override {
    (void)context;
    return push_notifications_.ListConfigs(request);
  }

  core::Result<void> DeleteTaskPushNotificationConfig(
      const lf::a2a::v1::DeleteTaskPushNotificationConfigRequest& request, server::RequestContext& context) override {
    (void)context;
    return push_notifications_.DeleteConfig(request);
  }

  void ShutdownSubscriptions() { subscriptions_.Shutdown(); }

 private:
  [[nodiscard]] std::mutex& TaskMutex(std::string_view task_id) noexcept {
    return task_mutexes_[std::hash<std::string_view>{}(task_id) % task_mutexes_.size()];
  }

  std::array<std::mutex, kTaskLockStripeCount> task_mutexes_;
  std::unique_ptr<server::InMemoryTaskStore> owned_task_store_;
  std::unique_ptr<server::InMemoryPushNotificationStore> owned_push_store_;
  std::unique_ptr<server::HttpPushNotificationDeliveryClient> owned_push_delivery_;
  server::TaskStore* task_store_ = nullptr;
  server::PushNotificationStore* push_store_ = nullptr;
  server::PushNotificationDeliveryClient* push_delivery_ = nullptr;
  std::shared_ptr<server::TaskIdGenerator> task_id_generator_;
  server::TaskLifecycleService lifecycle_;
  server::PushNotificationService push_notifications_;
  server::TaskSubscriptionService subscriptions_;
  std::atomic<std::uint64_t> status_timestamp_counter_{0};
};

inline lf::a2a::v1::AgentCard BuildRestAgentCard(std::string_view name, std::string_view url) {
  return a2a::core::AgentCardBuilder::RestPreset(name, url).WithPushNotifications(true).Build();
}

inline lf::a2a::v1::AgentCard BuildJsonRpcAgentCard(std::string_view name, std::string_view url) {
  return a2a::core::AgentCardBuilder::JsonRpcPreset(name, url).WithPushNotifications(true).Build();
}

}  // namespace a2a::examples
