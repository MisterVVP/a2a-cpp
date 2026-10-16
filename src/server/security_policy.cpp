// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "a2a/server/security_policy.h"

#include <algorithm>
#include <optional>
#include <string_view>
#include <type_traits>
#include <utility>

#include "a2a/core/protocol_errors.h"

namespace a2a::server {
namespace {

constexpr std::string_view kMissingSecurityConfiguration = "security policy and task store are required";
constexpr std::string_view kInvalidPageSize = "page size exceeds maximum";
constexpr std::string_view kEmptyIdentity = "authentication returned an empty identity";

core::Result<lf::a2a::v1::Task> GetAuthorizedTask(TaskStore& store, const SecurityPolicy& policy,
                                                  const RequestContext& context, std::string_view id) {
  auto task = store.Get(id);
  if (!task.ok()) {
    const auto& error = task.error();
    return error.code() == core::ErrorCode::kRemoteProtocol &&
                   error.protocol_code() == core::protocol_codes::kTaskNotFound
               ? core::protocol_errors::TaskNotFound()
               : error;
  }
  if (!policy.CanAccessTask(task.value(), context)) {
    return core::protocol_errors::TaskNotFound();
  }
  return task;
}

struct ResponseAuthorizer final {
  const SecurityPolicy& policy;
  TaskStore& store;
  const RequestContext& context;

  [[nodiscard]] core::Result<void> AuthorizeId(std::string_view id) const {
    if (id.empty()) {
      return core::protocol_errors::TaskNotFound();
    }
    auto task = GetAuthorizedTask(store, policy, context, id);
    return task.ok() ? core::Result<void>{} : task.error();
  }
  core::Result<void> operator()(const lf::a2a::v1::Task& task) const {
    return policy.CanAccessTask(task, context) ? core::Result<void>{} : core::protocol_errors::TaskNotFound();
  }
  core::Result<void> operator()(const lf::a2a::v1::SendMessageResponse& send) const {
    if (send.has_task()) {
      return (*this)(send.task());
    }
    return send.has_message() && !send.message().task_id().empty() ? AuthorizeId(send.message().task_id())
                                                                   : core::Result<void>{};
  }
  core::Result<void> operator()(const lf::a2a::v1::TaskPushNotificationConfig& config) const {
    return AuthorizeId(config.task_id());
  }
  core::Result<void> operator()(const lf::a2a::v1::ListTaskPushNotificationConfigsResponse& response) const {
    for (const auto& config : response.configs()) {
      auto authorized = (*this)(config);
      if (!authorized.ok()) {
        return authorized.error();
      }
    }
    return {};
  }
  template <typename Payload>
  core::Result<void> operator()(const Payload& /*payload*/) const {
    // Lists are authorized before projection; streams are checked on read.
    static_assert(std::is_same_v<Payload, ListTasksResponse> || std::is_same_v<Payload, lf::a2a::v1::AgentCard> ||
                  std::is_same_v<Payload, std::unique_ptr<ServerStreamSession>> ||
                  std::is_same_v<Payload, std::monostate>);
    return {};
  }
};

std::string TaskId(const DispatchRequest& request) {
  return std::visit(
      [](const auto& payload) -> std::string {
        using Payload = std::decay_t<decltype(payload)>;
        if constexpr (std::is_same_v<Payload, lf::a2a::v1::SendMessageRequest>) {
          return payload.message().task_id();
        } else if constexpr (std::is_same_v<Payload, lf::a2a::v1::GetTaskRequest> ||
                             std::is_same_v<Payload, lf::a2a::v1::CancelTaskRequest>) {
          return payload.id();
        } else if constexpr (std::is_same_v<Payload, ListTasksRequest> ||
                             std::is_same_v<Payload, lf::a2a::v1::GetExtendedAgentCardRequest>) {
          return {};
        } else {
          return payload.task_id();
        }
      },
      request.payload);
}

std::string_view EventTaskId(const lf::a2a::v1::StreamResponse& event) {
  if (event.has_task()) {
    return event.task().id();
  }
  if (event.has_status_update()) {
    return event.status_update().task_id();
  }
  if (event.has_artifact_update()) {
    return event.artifact_update().task_id();
  }
  return event.message().task_id();
}

class AuthorizedStream final : public ServerStreamSession {
 public:
  AuthorizedStream(std::unique_ptr<ServerStreamSession> stream, std::shared_ptr<const SecurityPolicy> policy,
                   TaskStore* store, RequestContext context, std::string task_id)
      : stream_(std::move(stream)),
        policy_(std::move(policy)),
        store_(store),
        context_(std::move(context)),
        task_id_(std::move(task_id)) {}

  core::Result<std::optional<lf::a2a::v1::StreamResponse>> Next() override { return Read(std::nullopt); }
  core::Result<std::optional<lf::a2a::v1::StreamResponse>> NextFor(std::chrono::milliseconds timeout) override {
    return Read(timeout);
  }
  bool SupportsTimedNext() const noexcept override { return stream_->SupportsTimedNext(); }
  bool IsLive() const noexcept override { return stream_->IsLive(); }
  void Cancel() noexcept override { stream_->Cancel(); }

 private:
  core::Result<std::optional<lf::a2a::v1::StreamResponse>> Read(std::optional<std::chrono::milliseconds> timeout) {
    if (!task_id_.empty()) {
      auto task = GetAuthorizedTask(*store_, *policy_, context_, task_id_);
      if (!task.ok()) {
        return task.error();
      }
    }
    auto event = timeout.has_value() ? stream_->NextFor(*timeout) : stream_->Next();
    if (!event.ok()) {
      return event.error();
    }
    const auto& item = event.value();
    if (!item.has_value()) {
      return event;
    }
    auto id = EventTaskId(*item);
    if (id.empty()) {
      if (!item->has_message()) {
        return core::protocol_errors::TaskNotFound();
      }
      // A taskless message still belongs to an already associated stream.
      if (task_id_.empty()) {
        return event;
      }
      id = task_id_;
    }
    auto task = GetAuthorizedTask(*store_, *policy_, context_, id);
    if (!task.ok()) {
      return task.error();
    }
    task_id_ = id;
    return event;
  }
  std::unique_ptr<ServerStreamSession> stream_;
  std::shared_ptr<const SecurityPolicy> policy_;
  TaskStore* store_;
  RequestContext context_;
  std::string task_id_;
};

}  // namespace

DispatchSecurity::DispatchSecurity(std::shared_ptr<const SecurityPolicy> policy, TaskStore* task_store)
    : policy_(std::move(policy)), task_store_(task_store) {}

core::Result<void> DispatchSecurity::Authorize(const DispatchRequest& request, RequestContext& context) const {
  context.principal.clear();
  if (policy_ == nullptr || task_store_ == nullptr) {
    return core::Error::Internal(std::string(kMissingSecurityConfiguration));
  }
  auto identity = policy_->Authenticate(context);
  if (!identity.ok()) {
    return identity.error();
  }
  if (identity.value().empty()) {
    return core::protocol_errors::Unauthenticated(std::string(kEmptyIdentity));
  }
  context.principal = std::move(identity.value());
  const auto task_id = TaskId(request);
  if (!task_id.empty()) {
    auto task = GetAuthorizedTask(*task_store_, *policy_, context, task_id);
    if (!task.ok()) {
      return task.error();
    }
  }
  if (!policy_->AuthorizeOperation(request, context)) {
    return core::protocol_errors::PermissionDenied();
  }
  return {};
}

core::Result<void> DispatchSecurity::AuthorizeResponse(const DispatchResponse& response,
                                                       const RequestContext& context) const {
  if (policy_ == nullptr || task_store_ == nullptr) {
    return core::Error::Internal(std::string(kMissingSecurityConfiguration));
  }
  return std::visit(ResponseAuthorizer{.policy = *policy_, .store = *task_store_, .context = context},
                    response.payload());
}

core::Result<DispatchResponse> DispatchSecurity::ListTasks(const ListTasksRequest& request,
                                                           const RequestContext& context) const {
  if (policy_ == nullptr || task_store_ == nullptr) {
    return core::Error::Internal(std::string(kMissingSecurityConfiguration));
  }
  if (request.page_size > kMaxListTasksPageSize) {
    return core::Error::Validation(std::string(kInvalidPageSize));
  }
  auto offset = ParseListPageToken(request.page_token);
  if (!offset.ok()) {
    return offset.error();
  }
  const auto size = request.page_size == 0 ? kDefaultListTasksPageSize : request.page_size;
  ListTasksResponse response;
  response.page_size = size;
  response.tasks.reserve(size);
  auto query = request;
  query.history_length.reset();
  query.include_artifacts = true;
  auto scoped = policy_->ScopeTaskList(query, context);
  std::vector<std::string> selected_ids;
  if (scoped.has_value()) {
    auto selected = SelectTaskIds(std::move(*scoped), context, offset.value(), size, response.total_size);
    if (!selected.ok()) {
      return selected.error();
    }
    selected_ids = std::move(selected.value());
  }
  auto valid_offset = ValidateListPageOffset(offset.value(), response.total_size);
  if (!valid_offset.ok()) {
    return valid_offset.error();
  }
  for (const auto& id : selected_ids) {
    auto task = GetAuthorizedTask(*task_store_, *policy_, context, id);
    if (!task.ok()) {
      return task.error();
    }
    response.tasks.push_back(ProjectTaskForList(task.value(), request.include_artifacts, request.history_length));
  }
  if (offset.value() + response.tasks.size() < response.total_size) {
    response.next_page_token = std::to_string(offset.value() + response.tasks.size());
  }
  return DispatchResponse(std::move(response));
}

core::Result<std::vector<std::string>> DispatchSecurity::SelectTaskIds(ListTasksRequest query,
                                                                       const RequestContext& context,
                                                                       std::size_t offset, std::size_t page_size,
                                                                       std::size_t& total_size) const {
  std::vector<std::string> selected;
  selected.reserve(page_size);
  query.page_size = kMaxListTasksPageSize;
  query.page_token.clear();
  do {
    auto listed = task_store_->List(query);
    if (!listed.ok()) {
      return listed.error();
    }
    for (const auto& task : listed.value().tasks) {
      if (!policy_->CanAccessTask(task, context)) {
        continue;
      }
      if (total_size >= offset && selected.size() < page_size) {
        selected.push_back(task.id());
      }
      ++total_size;
    }
    query.page_token = std::move(listed.value().next_page_token);
  } while (!query.page_token.empty());
  return selected;
}

core::Result<void> DispatchSecurity::ProtectStream(const DispatchRequest& request, const RequestContext& context,
                                                   DispatchResponse& response) const {
  if (policy_ == nullptr || task_store_ == nullptr) {
    return core::Error::Internal(std::string(kMissingSecurityConfiguration));
  }
  auto* stream = std::get_if<std::unique_ptr<ServerStreamSession>>(&response.payload());
  if (stream == nullptr || *stream == nullptr) {
    return {};
  }
  *stream = std::make_unique<AuthorizedStream>(std::move(*stream), policy_, task_store_, context, TaskId(request));
  return {};
}

}  // namespace a2a::server
