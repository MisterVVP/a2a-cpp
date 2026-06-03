// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "a2a/server/server.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <limits>
#include <ranges>
#include <string>
#include <utility>

#include "a2a/core/error.h"
#include "a2a/core/protocol_error_messages.h"
#include "a2a/core/protocol_errors.h"
#include "a2a/core/task_states.h"

namespace a2a::server {

namespace {

template <std::size_t MessageSize>
[[nodiscard]] core::Error DispatchPayloadTypeMismatchError(const std::array<char, MessageSize>& message) {
  return core::Error::Validation(core::protocol_error_messages::ToString(message));
}

std::string ToLower(std::string_view value) {
  std::string lowered(value);
  std::ranges::transform(lowered, lowered.begin(),
                         [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  return lowered;
}

std::string Trim(std::string_view value) {
  std::size_t begin = 0;
  while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin])) != 0) {
    ++begin;
  }
  std::size_t end = value.size();
  while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
    --end;
  }
  return std::string(value.substr(begin, end - begin));
}

bool IsAuthSignalHeader(std::string_view lowered_name) {
  return lowered_name == "authorization" || lowered_name == "proxy-authorization" ||
         lowered_name.find("auth") != std::string_view::npos || lowered_name.find("token") != std::string_view::npos ||
         lowered_name.find("api-key") != std::string_view::npos ||
         lowered_name.find("apikey") != std::string_view::npos;
}

bool HasStatusAfterCutoff(const lf::a2a::v1::Task& task, const google::protobuf::Timestamp& cutoff) {
  if (!task.status().has_timestamp()) {
    return false;
  }
  const auto& ts = task.status().timestamp();
  return ts.seconds() > cutoff.seconds() || (ts.seconds() == cutoff.seconds() && ts.nanos() >= cutoff.nanos());
}

bool MatchesListFilters(const lf::a2a::v1::Task& task, const ListTasksRequest& request) {
  if (!request.context_id.empty() && task.context_id() != request.context_id) {
    return false;
  }
  if (request.status_filter.has_value() && task.status().state() != *request.status_filter) {
    return false;
  }
  if (request.status_timestamp_after.has_value() && !HasStatusAfterCutoff(task, *request.status_timestamp_after)) {
    return false;
  }
  return true;
}

void ApplyHistoryLimit(lf::a2a::v1::Task* task, std::size_t keep) {
  if (keep == 0) {
    task->clear_history();
    return;
  }
  if (std::cmp_less_equal(task->history_size(), keep)) {
    return;
  }
  const auto history_size = static_cast<std::size_t>(task->history_size());
  const int remove_count = static_cast<int>(history_size - keep);
  task->mutable_history()->DeleteSubrange(0, remove_count);
}

bool IsPushNotificationOperation(DispatcherOperation operation) {
  return operation == DispatcherOperation::kCreateTaskPushNotificationConfig ||
         operation == DispatcherOperation::kGetTaskPushNotificationConfig ||
         operation == DispatcherOperation::kListTaskPushNotificationConfigs ||
         operation == DispatcherOperation::kDeleteTaskPushNotificationConfig;
}

core::Result<DispatchResponse> DispatchPushToExecutor(AgentExecutor& executor, const DispatchRequest& request,
                                                      RequestContext& context) {
  switch (request.operation) {
    case DispatcherOperation::kCreateTaskPushNotificationConfig: {
      const auto* payload = std::get_if<lf::a2a::v1::TaskPushNotificationConfig>(&request.payload);
      if (payload == nullptr) {
        return DispatchPayloadTypeMismatchError(
            core::protocol_error_messages::kDispatchPayloadTypeMismatchForCreateTaskPushNotificationConfig);
      }
      const auto response = executor.CreateTaskPushNotificationConfig(*payload, context);
      if (!response.ok()) {
        return response.error();
      }
      return DispatchResponse(response.value());
    }
    case DispatcherOperation::kGetTaskPushNotificationConfig: {
      const auto* payload = std::get_if<lf::a2a::v1::GetTaskPushNotificationConfigRequest>(&request.payload);
      if (payload == nullptr) {
        return DispatchPayloadTypeMismatchError(
            core::protocol_error_messages::kDispatchPayloadTypeMismatchForGetTaskPushNotificationConfig);
      }
      const auto response = executor.GetTaskPushNotificationConfig(*payload, context);
      if (!response.ok()) {
        return response.error();
      }
      return DispatchResponse(response.value());
    }
    case DispatcherOperation::kListTaskPushNotificationConfigs: {
      const auto* payload = std::get_if<lf::a2a::v1::ListTaskPushNotificationConfigsRequest>(&request.payload);
      if (payload == nullptr) {
        return DispatchPayloadTypeMismatchError(
            core::protocol_error_messages::kDispatchPayloadTypeMismatchForListTaskPushNotificationConfigs);
      }
      const auto response = executor.ListTaskPushNotificationConfigs(*payload, context);
      if (!response.ok()) {
        return response.error();
      }
      return DispatchResponse(response.value());
    }
    case DispatcherOperation::kDeleteTaskPushNotificationConfig: {
      const auto* payload = std::get_if<lf::a2a::v1::DeleteTaskPushNotificationConfigRequest>(&request.payload);
      if (payload == nullptr) {
        return DispatchPayloadTypeMismatchError(
            core::protocol_error_messages::kDispatchPayloadTypeMismatchForDeleteTaskPushNotificationConfig);
      }
      const auto response = executor.DeleteTaskPushNotificationConfig(*payload, context);
      if (!response.ok()) {
        return response.error();
      }
      return DispatchResponse();
    }
    case DispatcherOperation::kSendMessage:
    case DispatcherOperation::kSendStreamingMessage:
    case DispatcherOperation::kGetTask:
    case DispatcherOperation::kListTasks:
    case DispatcherOperation::kCancelTask:
      return core::Error::Validation("Dispatch operation is not a push notification operation");
  }
  return core::Error::Validation("Unsupported push notification dispatcher operation");
}

core::Result<DispatchResponse> DispatchToExecutor(AgentExecutor& executor, const DispatchRequest& request,
                                                  RequestContext& context) {
  if (IsPushNotificationOperation(request.operation)) {
    return DispatchPushToExecutor(executor, request, context);
  }

  switch (request.operation) {
    case DispatcherOperation::kSendMessage: {
      const auto* payload = std::get_if<lf::a2a::v1::SendMessageRequest>(&request.payload);
      if (payload == nullptr) {
        return DispatchPayloadTypeMismatchError(
            core::protocol_error_messages::kDispatchPayloadTypeMismatchForSendMessage);
      }
      const auto response = executor.SendMessage(*payload, context);
      if (!response.ok()) {
        return response.error();
      }
      return DispatchResponse(response.value());
    }
    case DispatcherOperation::kSendStreamingMessage: {
      const auto* payload = std::get_if<lf::a2a::v1::SendMessageRequest>(&request.payload);
      if (payload == nullptr) {
        return DispatchPayloadTypeMismatchError(
            core::protocol_error_messages::kDispatchPayloadTypeMismatchForSendStreamingMessage);
      }
      auto response = executor.SendStreamingMessage(*payload, context);
      if (!response.ok()) {
        return response.error();
      }
      return DispatchResponse(std::move(response.value()));
    }
    case DispatcherOperation::kGetTask: {
      const auto* payload = std::get_if<lf::a2a::v1::GetTaskRequest>(&request.payload);
      if (payload == nullptr) {
        return DispatchPayloadTypeMismatchError(core::protocol_error_messages::kDispatchPayloadTypeMismatchForGetTask);
      }
      auto response = executor.GetTask(*payload, context);
      if (!response.ok()) {
        return response.error();
      }
      lf::a2a::v1::Task task = std::move(response.value());
      if (payload->has_history_length()) {
        ApplyHistoryLimit(&task, static_cast<std::size_t>(payload->history_length()));
      }
      return DispatchResponse(std::move(task));
    }
    case DispatcherOperation::kListTasks: {
      const auto* payload = std::get_if<ListTasksRequest>(&request.payload);
      if (payload == nullptr) {
        return DispatchPayloadTypeMismatchError(
            core::protocol_error_messages::kDispatchPayloadTypeMismatchForListTasks);
      }
      const auto response = executor.ListTasks(*payload, context);
      if (!response.ok()) {
        return response.error();
      }
      return DispatchResponse(response.value());
    }
    case DispatcherOperation::kCancelTask: {
      const auto* payload = std::get_if<lf::a2a::v1::CancelTaskRequest>(&request.payload);
      if (payload == nullptr) {
        return DispatchPayloadTypeMismatchError(
            core::protocol_error_messages::kDispatchPayloadTypeMismatchForCancelTask);
      }
      const auto response = executor.CancelTask(*payload, context);
      if (!response.ok()) {
        return response.error();
      }
      return DispatchResponse(response.value());
    }
    case DispatcherOperation::kCreateTaskPushNotificationConfig:
    case DispatcherOperation::kGetTaskPushNotificationConfig:
    case DispatcherOperation::kListTaskPushNotificationConfigs:
    case DispatcherOperation::kDeleteTaskPushNotificationConfig:
      return core::Error::Validation("Push notification dispatch was not handled by push dispatcher");
  }

  return core::Error::Validation("Unsupported dispatcher operation");
}

bool HasSameMessageFingerprint(const lf::a2a::v1::Message& existing, const lf::a2a::v1::Message& message) {
  if (existing.task_id() != message.task_id() || existing.context_id() != message.context_id() ||
      existing.role() != message.role() || existing.parts_size() != message.parts_size()) {
    return false;
  }

  for (int index = 0; index < existing.parts_size(); ++index) {
    if (existing.parts(index).SerializeAsString() != message.parts(index).SerializeAsString()) {
      return false;
    }
  }
  return true;
}

bool HasSameMessageIdAndFingerprint(const lf::a2a::v1::Message& existing, const lf::a2a::v1::Message& message) {
  return !existing.message_id().empty() && existing.message_id() == message.message_id() &&
         HasSameMessageFingerprint(existing, message);
}

std::optional<TaskStore::HistoryDedupeEvent::Reason> FindMessageIdDedupeReason(
    const google::protobuf::RepeatedPtrField<lf::a2a::v1::Message>& history, const lf::a2a::v1::Message& message) {
  for (const auto& existing : history) {
    if (HasSameMessageIdAndFingerprint(existing, message)) {
      return TaskStore::HistoryDedupeEvent::Reason::kDuplicateMessageIdAndFingerprint;
    }
  }
  return std::nullopt;
}

std::optional<TaskStore::HistoryDedupeEvent::Reason> FindIdOrFingerprintDedupeReason(
    const google::protobuf::RepeatedPtrField<lf::a2a::v1::Message>& history, const lf::a2a::v1::Message& message) {
  const bool has_message_id = !message.message_id().empty();
  for (const auto& existing : history) {
    if (has_message_id && HasSameMessageIdAndFingerprint(existing, message)) {
      return TaskStore::HistoryDedupeEvent::Reason::kDuplicateMessageIdAndFingerprint;
    }
    if (!has_message_id && HasSameMessageFingerprint(existing, message)) {
      return TaskStore::HistoryDedupeEvent::Reason::kDuplicateFingerprintWithoutMessageId;
    }
  }
  return std::nullopt;
}

std::optional<TaskStore::HistoryDedupeEvent::Reason> FindHistoryDedupeReason(
    const google::protobuf::RepeatedPtrField<lf::a2a::v1::Message>& history, const lf::a2a::v1::Message& message,
    TaskStore::HistoryAppendPolicy policy) {
  if (policy == TaskStore::HistoryAppendPolicy::kDedupByMessageId && !message.message_id().empty()) {
    return FindMessageIdDedupeReason(history, message);
  }
  if (policy == TaskStore::HistoryAppendPolicy::kDedupByIdOrFingerprint) {
    return FindIdOrFingerprintDedupeReason(history, message);
  }
  return std::nullopt;
}

void UpdateDedupeSnapshot(TaskStore::HistoryTelemetrySnapshot* snapshot, TaskStore::HistoryDedupeEvent::Reason reason) {
  snapshot->dedupe_dropped_total += 1;
  if (reason == TaskStore::HistoryDedupeEvent::Reason::kDuplicateMessageIdAndFingerprint) {
    snapshot->dedupe_dropped_by_message_id_and_fingerprint += 1;
    return;
  }
  snapshot->dedupe_dropped_by_fingerprint_without_message_id += 1;
}

}  // namespace

std::unordered_map<std::string, std::string> ExtractAuthMetadata(
    const std::unordered_map<std::string, std::string>& headers) {
  std::unordered_map<std::string, std::string> auth_metadata;

  for (const auto& [name, value] : headers) {
    const std::string lowered_name = ToLower(name);
    if (lowered_name == "authorization" || lowered_name == "proxy-authorization") {
      const std::string trimmed_value = Trim(value);
      auth_metadata.insert_or_assign("authorization", trimmed_value);

      const std::string lowered_value = ToLower(trimmed_value);
      constexpr std::string_view kBearerPrefix = "bearer ";
      if (lowered_value.starts_with(kBearerPrefix) && trimmed_value.size() > kBearerPrefix.size()) {
        auth_metadata.insert_or_assign("bearer_token", Trim(trimmed_value.substr(kBearerPrefix.size())));
      }
    }

    if (lowered_name == "x-api-key") {
      auth_metadata.insert_or_assign("api_key", value);
    }

    if (lowered_name == "x-forwarded-client-cert") {
      auth_metadata.insert_or_assign("mtls_client_cert", value);
    }

    if (IsAuthSignalHeader(lowered_name)) {
      auth_metadata.insert_or_assign("header." + lowered_name, value);
    }
  }

  return auth_metadata;
}

Dispatcher::Dispatcher(AgentExecutor* executor) : executor_(executor) {}

Dispatcher::Dispatcher(AgentExecutor* executor, std::vector<std::shared_ptr<ServerInterceptor>> interceptors)
    : executor_(executor), interceptors_(std::move(interceptors)) {}

core::Result<DispatchResponse> Dispatcher::Dispatch(const DispatchRequest& request, RequestContext& context) const {
  if (executor_ == nullptr) {
    return core::Error::Internal("Server dispatcher executor is not configured");
  }

  std::shared_lock<std::shared_mutex> read_lock(interceptor_mutex_);
  for (const auto& interceptor : interceptors_) {
    if (interceptor == nullptr) {
      continue;
    }
    const auto before_result = interceptor->BeforeDispatch(request, context);
    if (!before_result.ok()) {
      core::Result<DispatchResponse> failure = before_result.error();
      read_lock.unlock();
      RunAfterInterceptors(request, context, failure);
      return before_result.error();
    }
  }
  read_lock.unlock();

  auto dispatch_result = DispatchToExecutor(*executor_, request, context);
  RunAfterInterceptors(request, context, dispatch_result);
  return dispatch_result;
}

void Dispatcher::AddInterceptor(std::shared_ptr<ServerInterceptor> interceptor) {
  if (interceptor == nullptr) {
    return;
  }
  std::unique_lock<std::shared_mutex> lock(interceptor_mutex_);
  interceptors_.push_back(std::move(interceptor));
}

void Dispatcher::RunAfterInterceptors(const DispatchRequest& request, RequestContext& context,
                                      const core::Result<DispatchResponse>& result) const {
  std::shared_lock<std::shared_mutex> read_lock(interceptor_mutex_);
  for (const auto& interceptor : std::ranges::reverse_view(interceptors_)) {
    if (interceptor == nullptr) {
      continue;
    }
    interceptor->AfterDispatch(request, context, result);
  }
}

InMemoryTaskStore::InMemoryTaskStore(std::shared_ptr<HistoryTelemetrySink> telemetry_sink)
    : telemetry_sink_(std::move(telemetry_sink)) {}

core::Result<void> InMemoryTaskStore::CreateOrUpdate(const lf::a2a::v1::Task& task) {
  if (task.id().empty()) {
    return core::Error::Validation("Task.id is required");
  }

  std::unique_lock<std::shared_mutex> lock(mutex_);
  auto [it, inserted] = tasks_.try_emplace(task.id(), task);
  if (inserted) {
    ordered_ids_.push_back(it->first);
  } else {
    it->second = task;
  }
  return {};
}

core::Result<lf::a2a::v1::Task> InMemoryTaskStore::Get(std::string_view id) const {
  if (id.empty()) {
    return core::Error::Validation("Task id is required");
  }

  std::shared_lock<std::shared_mutex> lock(mutex_);
  const auto it = tasks_.find(id);
  if (it == tasks_.end()) {
    return core::protocol_errors::TaskNotFound("Task not found");
  }
  return it->second;
}

std::optional<std::size_t> InMemoryTaskStore::ParsePageToken(std::string_view token) {
  if (token.empty()) {
    return std::size_t{0};
  }

  std::size_t parsed = 0;
  const auto* begin = token.data();
  const auto* end = token.data() + token.size();
  const auto result = std::from_chars(begin, end, parsed);
  if (result.ec != std::errc() || result.ptr != end) {
    return std::nullopt;
  }
  return parsed;
}

core::Result<ListTasksResponse> InMemoryTaskStore::List(const ListTasksRequest& request) const {
  const auto offset = ParseListPageToken(request.page_token);
  if (!offset.ok()) {
    return offset.error();
  }

  std::shared_lock<std::shared_mutex> lock(mutex_);

  const std::size_t start = offset.value();
  const std::size_t effective_page_size = request.page_size;
  ListTasksResponse response;
  if (effective_page_size == 0) {
    response.tasks.reserve(start < ordered_ids_.size() ? ordered_ids_.size() - start : 0);
  } else {
    response.tasks.reserve(effective_page_size);
  }

  std::size_t matched_count = 0;
  for (const auto& id : ordered_ids_) {
    const auto it = tasks_.find(id);
    if (it != tasks_.end() && MatchesListFilters(it->second, request)) {
      if (matched_count >= start && (effective_page_size == 0 || response.tasks.size() < effective_page_size)) {
        lf::a2a::v1::Task projected = it->second;
        ApplyArtifactProjection(&projected, request.include_artifacts);
        ApplyHistoryRetention(&projected, request.history_length);
        response.tasks.push_back(std::move(projected));
      }
      ++matched_count;
    }
  }

  const auto valid_offset = ValidateListPageOffset(start, matched_count);
  if (!valid_offset.ok()) {
    return valid_offset.error();
  }

  response.page_size = response.tasks.size();
  response.total_size = matched_count;
  if (effective_page_size != 0 && response.tasks.size() == effective_page_size) {
    const std::size_t next_offset = start + response.tasks.size();
    if (next_offset < matched_count) {
      response.next_page_token = std::to_string(next_offset);
    }
  }

  return response;
}

core::Result<lf::a2a::v1::Task> TaskLifecycleService::CreateOrUpdateTask(const lf::a2a::v1::Task& task) const {
  if (store_ == nullptr) {
    return core::Error::Internal("TaskLifecycleService store is not configured");
  }
  const auto upsert = store_->CreateOrUpdate(task);
  if (!upsert.ok()) {
    return upsert.error();
  }
  return store_->Get(task.id());
}

TaskLifecycleService::TaskLifecycleService(TaskStore* store, std::shared_ptr<TaskIdGenerator> task_id_generator)
    : store_(store), task_id_generator_(std::move(task_id_generator)) {
  if (task_id_generator_ == nullptr) {
    task_id_generator_ = std::make_shared<UuidV7TaskIdGenerator>();
  }
}

core::Result<std::string> TaskLifecycleService::ResolveTaskIdForSendRequest(
    const lf::a2a::v1::SendMessageRequest& request, const RequestContext& context) const {
  if (!request.has_message()) {
    return core::Error::Validation("message is required");
  }
  const auto& message = request.message();
  if (!message.task_id().empty()) {
    const auto existing = store_->Get(message.task_id());
    if (!existing.ok()) {
      return core::protocol_errors::TaskNotFound();
    }
    if (!message.context_id().empty() && !existing.value().context_id().empty() &&
        message.context_id() != existing.value().context_id()) {
      return core::protocol_errors::UnsupportedOperation("contextId does not match task");
    }
    if (core::IsTerminalTaskState(existing.value().status().state())) {
      return core::protocol_errors::UnsupportedOperation("task is already terminal");
    }
    return message.task_id();
  }
  if (message.message_id().empty()) {
    return core::Error::Validation("message.messageId is required when message.taskId is absent");
  }
  return task_id_generator_->GenerateTaskId(request, context);
}

core::Result<lf::a2a::v1::Task> TaskLifecycleService::TransitionTaskStatus(std::string_view task_id,
                                                                           lf::a2a::v1::TaskState next_state) const {
  if (store_ == nullptr) {
    return core::Error::Internal("TaskLifecycleService store is not configured");
  }
  auto current = store_->Get(task_id);
  if (!current.ok()) {
    return current.error();
  }
  if (core::IsTerminalTaskState(current.value().status().state())) {
    return core::protocol_errors::UnsupportedOperation("task is already terminal");
  }
  current.value().mutable_status()->set_state(next_state);
  const auto upsert = store_->CreateOrUpdate(current.value());
  if (!upsert.ok()) {
    return upsert.error();
  }
  return current.value();
}

core::Result<lf::a2a::v1::Task> TaskLifecycleService::AppendHistory(std::string_view task_id,
                                                                    const lf::a2a::v1::Message& message,
                                                                    TaskStore::HistoryAppendPolicy policy) const {
  if (store_ == nullptr) {
    return core::Error::Internal("TaskLifecycleService store is not configured");
  }
  return store_->AppendTaskHistory(task_id, message, policy);
}

core::Result<std::size_t> ParseListPageToken(std::string_view page_token) {
  if (page_token.empty()) {
    return std::size_t{0};
  }
  std::size_t parsed = 0;
  const auto* begin = page_token.data();
  const auto* end = page_token.data() + page_token.size();
  const auto result = std::from_chars(begin, end, parsed);
  if (result.ec != std::errc() || result.ptr != end) {
    return core::Error::Validation("ListTasksRequest.page_token must be a non-negative integer");
  }
  return parsed;
}

core::Result<void> ValidateListPageOffset(std::size_t offset, std::size_t size) {
  if (offset > size) {
    return core::Error::Validation("ListTasksRequest.page_token exceeds available task count");
  }
  return {};
}

void ApplyHistoryRetention(lf::a2a::v1::Task* task, std::optional<std::size_t> history_length) {
  if (!history_length.has_value()) {
    return;
  }
  ApplyHistoryLimit(task, *history_length);
}

void ApplyArtifactProjection(lf::a2a::v1::Task* task, bool include_artifacts) {
  if (!include_artifacts) {
    task->clear_artifacts();
  }
}

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

core::Result<lf::a2a::v1::Task> InMemoryTaskStore::Cancel(std::string_view id) {
  if (id.empty()) {
    return core::Error::Validation("Task id is required");
  }

  std::unique_lock<std::shared_mutex> lock(mutex_);
  const auto it = tasks_.find(id);
  if (it == tasks_.end()) {
    return core::protocol_errors::TaskNotFound("Task not found");
  }
  if (core::IsTerminalTaskState(it->second.status().state())) {
    return core::protocol_errors::TaskNotCancelable();
  }

  auto* mutable_status = it->second.mutable_status();
  mutable_status->set_state(lf::a2a::v1::TASK_STATE_CANCELED);
  return it->second;
}

core::Result<lf::a2a::v1::Task> InMemoryTaskStore::AppendTaskHistory(std::string_view task_id,
                                                                     const lf::a2a::v1::Message& message,
                                                                     HistoryAppendPolicy policy) {
  if (task_id.empty()) {
    return core::Error::Validation("Task id is required");
  }

  std::shared_ptr<HistoryTelemetrySink> telemetry_sink;
  std::optional<HistoryDedupeEvent> dedupe_event;
  lf::a2a::v1::Task result;
  {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    const auto it = tasks_.find(task_id);
    if (it == tasks_.end()) {
      return core::protocol_errors::TaskNotFound("Task not found");
    }

    const auto dedupe_reason = FindHistoryDedupeReason(it->second.history(), message, policy);
    if (dedupe_reason.has_value()) {
      UpdateDedupeSnapshot(&telemetry_snapshot_, *dedupe_reason);
      telemetry_sink = telemetry_sink_;
      dedupe_event = TaskStore::HistoryDedupeEvent{
          .task_id = std::string(task_id),
          .message_id = message.message_id(),
          .policy = policy,
          .reason = *dedupe_reason,
      };
      result = it->second;
    } else {
      *it->second.add_history() = message;
      result = it->second;
    }
  }

  if (telemetry_sink != nullptr && dedupe_event.has_value()) {
    telemetry_sink->OnDedupedHistoryMessage(*dedupe_event);
  }
  return result;
}

TaskStore::HistoryTelemetrySnapshot InMemoryTaskStore::GetHistoryTelemetrySnapshot() const {
  std::shared_lock<std::shared_mutex> lock(mutex_);
  return telemetry_snapshot_;
}

}  // namespace a2a::server
