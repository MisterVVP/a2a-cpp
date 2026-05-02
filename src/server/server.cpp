#include "a2a/server/server.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <limits>
#include <ranges>
#include <string>
#include <utility>

#include "a2a/core/error.h"

namespace a2a::server {

namespace {

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
         lowered_name.find("auth") != std::string_view::npos ||
         lowered_name.find("token") != std::string_view::npos ||
         lowered_name.find("api-key") != std::string_view::npos ||
         lowered_name.find("apikey") != std::string_view::npos;
}

core::Result<DispatchResponse> DispatchToExecutor(AgentExecutor& executor,
                                                  const DispatchRequest& request,
                                                  RequestContext& context) {
  switch (request.operation) {
    case DispatcherOperation::kSendMessage: {
      const auto* payload = std::get_if<lf::a2a::v1::SendMessageRequest>(&request.payload);
      if (payload == nullptr) {
        return core::Error::Validation("Dispatch payload type mismatch for SendMessage");
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
        return core::Error::Validation("Dispatch payload type mismatch for SendStreamingMessage");
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
        return core::Error::Validation("Dispatch payload type mismatch for GetTask");
      }
      const auto response = executor.GetTask(*payload, context);
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
      const auto response = executor.ListTasks(*payload, context);
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
      const auto response = executor.CancelTask(*payload, context);
      if (!response.ok()) {
        return response.error();
      }
      return DispatchResponse(response.value());
    }
  }

  return core::Error::Validation("Unsupported dispatcher operation");
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
        auth_metadata.insert_or_assign("bearer_token",
                                       Trim(trimmed_value.substr(kBearerPrefix.size())));
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

Dispatcher::Dispatcher(AgentExecutor* executor,
                       std::vector<std::shared_ptr<ServerInterceptor>> interceptors)
    : executor_(executor), interceptors_(std::move(interceptors)) {}

core::Result<DispatchResponse> Dispatcher::Dispatch(const DispatchRequest& request,
                                                    RequestContext& context) const {
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
