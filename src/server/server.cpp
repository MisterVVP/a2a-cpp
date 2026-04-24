#include "a2a/server/server.h"

#include <algorithm>
#include <charconv>
#include <limits>
#include <string>
#include <utility>

#include "a2a/core/error.h"

namespace a2a::server {

Dispatcher::Dispatcher(AgentExecutor* executor) : executor_(executor) {}

core::Result<DispatchResponse> Dispatcher::Dispatch(const DispatchRequest& request,
                                                    RequestContext& context) const {
  if (executor_ == nullptr) {
    return core::Error::Internal("Server dispatcher executor is not configured");
  }

  switch (request.operation) {
    case DispatcherOperation::kSendMessage: {
      const auto* payload = std::get_if<lf::a2a::v1::SendMessageRequest>(&request.payload);
      if (payload == nullptr) {
        return core::Error::Validation("Dispatch payload type mismatch for SendMessage");
      }
      const auto response = executor_->SendMessage(*payload, context);
      if (!response.ok()) {
        return response.error();
      }
      return DispatchResponse(response.value());
    }
    case DispatcherOperation::kSendStreamingMessage: {
      const auto* payload = std::get_if<lf::a2a::v1::SendMessageRequest>(&request.payload);
      if (payload == nullptr) {
        return core::Error::Validation("Dispatch payload type mismatch for SendStreamingMessage");
      }
      auto response = executor_->SendStreamingMessage(*payload, context);
      if (!response.ok()) {
        return response.error();
      }
      return DispatchResponse(std::move(response.value()));
    }
    case DispatcherOperation::kGetTask: {
      const auto* payload = std::get_if<lf::a2a::v1::GetTaskRequest>(&request.payload);
      if (payload == nullptr) {
        return core::Error::Validation("Dispatch payload type mismatch for GetTask");
      }
      const auto response = executor_->GetTask(*payload, context);
      if (!response.ok()) {
        return response.error();
      }
      return DispatchResponse(response.value());
    }
    case DispatcherOperation::kListTasks: {
      const auto* payload = std::get_if<ListTasksRequest>(&request.payload);
      if (payload == nullptr) {
        return core::Error::Validation("Dispatch payload type mismatch for ListTasks");
      }
      const auto response = executor_->ListTasks(*payload, context);
      if (!response.ok()) {
        return response.error();
      }
      return DispatchResponse(response.value());
    }
    case DispatcherOperation::kCancelTask: {
      const auto* payload = std::get_if<lf::a2a::v1::CancelTaskRequest>(&request.payload);
      if (payload == nullptr) {
        return core::Error::Validation("Dispatch payload type mismatch for CancelTask");
      }
      const auto response = executor_->CancelTask(*payload, context);
      if (!response.ok()) {
        return response.error();
      }
      return DispatchResponse(response.value());
    }
  }

  return core::Error::Validation("Unsupported dispatcher operation");
}

core::Result<void> InMemoryTaskStore::CreateOrUpdate(const lf::a2a::v1::Task& task) {
  if (task.id().empty()) {
    return core::Error::Validation("Task.id is required");
  }

  std::lock_guard<std::mutex> lock(mutex_);
  const auto existing = tasks_.find(task.id());
  if (existing == tasks_.end()) {
    ordered_ids_.push_back(task.id());
  }
  tasks_[task.id()] = task;
  return {};
}

core::Result<lf::a2a::v1::Task> InMemoryTaskStore::Get(std::string_view id) const {
  if (id.empty()) {
    return core::Error::Validation("Task id is required");
  }

  std::lock_guard<std::mutex> lock(mutex_);
  const auto it = tasks_.find(std::string(id));
  if (it == tasks_.end()) {
    return core::Error::Validation("Task not found");
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
  const auto offset = ParsePageToken(request.page_token);
  if (!offset.has_value()) {
    return core::Error::Validation("ListTasksRequest.page_token must be a non-negative integer");
  }

  std::lock_guard<std::mutex> lock(mutex_);

  if (offset.value() > ordered_ids_.size()) {
    return core::Error::Validation("ListTasksRequest.page_token exceeds available task count");
  }

  const std::size_t max_items = request.page_size == 0
                                    ? ordered_ids_.size()
                                    : std::min(request.page_size, ordered_ids_.size());

  ListTasksResponse response;
  for (std::size_t idx = offset.value(); idx < ordered_ids_.size(); ++idx) {
    if (response.tasks.size() >= max_items) {
      response.next_page_token = std::to_string(idx);
      break;
    }
    const auto task_it = tasks_.find(ordered_ids_[idx]);
    if (task_it != tasks_.end()) {
      response.tasks.push_back(task_it->second);
    }
  }

  return response;
}

core::Result<lf::a2a::v1::Task> InMemoryTaskStore::Cancel(std::string_view id) {
  if (id.empty()) {
    return core::Error::Validation("Task id is required");
  }

  std::lock_guard<std::mutex> lock(mutex_);
  const auto it = tasks_.find(std::string(id));
  if (it == tasks_.end()) {
    return core::Error::Validation("Task not found");
  }

  auto* mutable_status = it->second.mutable_status();
  mutable_status->set_state(lf::a2a::v1::TASK_STATE_CANCELED);
  return it->second;
}

}  // namespace a2a::server
