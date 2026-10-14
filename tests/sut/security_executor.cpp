// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "sut/security_executor.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <utility>

namespace a2a::tests::sut {
namespace {
constexpr std::string_view kParentDirectorySegment = "..";
constexpr auto kFixturePollInterval = std::chrono::milliseconds(20);
constexpr std::size_t kMaxCredentialSize = 4096;

bool RequestsAuthorization(const lf::a2a::v1::SendMessageRequest& request) {
  return std::ranges::any_of(request.message().parts(),
                             [](const auto& part) { return part.text() == kAuthorizationTrigger; });
}
}  // namespace

SecurityExecutor::SecurityExecutor(server::TaskStore* store, server::PushNotificationStore* push_store,
                                   std::filesystem::path credential_directory, std::string credential)
    : store_(store),
      executor_({.task_store = store, .push_store = push_store, .push_delivery = nullptr, .task_id_generator = {}}),
      authorization_(store,
                     [credential = std::move(credential)](const auto&, std::string_view supplied) {
                       return !credential.empty() && supplied == credential;
                     }),
      credential_directory_(std::move(credential_directory)) {
  if (!credential_directory_.empty()) {
    worker_ = std::thread([this] { PollCredentials(); });
  }
}
SecurityExecutor::~SecurityExecutor() { ShutdownSubscriptions(); }
void SecurityExecutor::ShutdownSubscriptions() {
  stopped_.store(true);
  if (worker_.joinable()) {
    worker_.join();
  }
  authorization_.Shutdown();
  executor_.ShutdownSubscriptions();
}

bool SecurityExecutor::IsAuthorizationTask(std::string_view id) const {
  if (id.empty()) {
    return false;
  }
  auto task = store_->Get(id);
  return task.ok() && server::TaskAuthorizationService::IsAuthorizationTask(task.value());
}

core::Result<lf::a2a::v1::SendMessageResponse> SecurityExecutor::SendMessage(
    const lf::a2a::v1::SendMessageRequest& request, server::RequestContext& context) {
  if (!RequestsAuthorization(request) && !IsAuthorizationTask(request.message().task_id())) {
    return executor_.SendMessage(request, context);
  }
  lf::a2a::v1::Message explanation;
  explanation.set_role(lf::a2a::v1::ROLE_AGENT);
  explanation.set_message_id(std::string(kAuthorizationStatusId));
  explanation.add_parts()->set_text(std::string(kAuthorizationExplanation));
  auto task = request.message().task_id().empty() ? authorization_.RequestAuthorization(request, context, explanation)
                                                  : authorization_.ReceiveMessage(request);
  if (!task.ok()) {
    return task.error();
  }
  lf::a2a::v1::SendMessageResponse response;
  *response.mutable_task() = std::move(task.value());
  return response;
}

core::Result<std::unique_ptr<server::ServerStreamSession>> SecurityExecutor::SendStreamingMessage(
    const lf::a2a::v1::SendMessageRequest& request, server::RequestContext& context) {
  if (!RequestsAuthorization(request) && !IsAuthorizationTask(request.message().task_id())) {
    return executor_.SendStreamingMessage(request, context);
  }
  lf::a2a::v1::Message explanation;
  explanation.set_role(lf::a2a::v1::ROLE_AGENT);
  explanation.set_message_id(std::string(kAuthorizationStatusId));
  explanation.add_parts()->set_text(std::string(kAuthorizationExplanation));
  return authorization_.SendStreamingMessage(request, context, explanation);
}

core::Result<lf::a2a::v1::Task> SecurityExecutor::GetTask(const lf::a2a::v1::GetTaskRequest& request,
                                                          server::RequestContext& context) {
  return executor_.GetTask(request, context);
}
core::Result<std::unique_ptr<server::ServerStreamSession>> SecurityExecutor::SubscribeTask(
    const lf::a2a::v1::GetTaskRequest& request, server::RequestContext& context) {
  return IsAuthorizationTask(request.id()) ? authorization_.Subscribe(request.id())
                                           : executor_.SubscribeTask(request, context);
}
core::Result<server::ListTasksResponse> SecurityExecutor::ListTasks(const server::ListTasksRequest& request,
                                                                    server::RequestContext& context) {
  return executor_.ListTasks(request, context);
}
core::Result<lf::a2a::v1::Task> SecurityExecutor::CancelTask(const lf::a2a::v1::CancelTaskRequest& request,
                                                             server::RequestContext& context) {
  return IsAuthorizationTask(request.id()) ? authorization_.Cancel(request.id())
                                           : executor_.CancelTask(request, context);
}
core::Result<lf::a2a::v1::TaskPushNotificationConfig> SecurityExecutor::CreateTaskPushNotificationConfig(
    const lf::a2a::v1::TaskPushNotificationConfig& request, server::RequestContext& context) {
  return executor_.CreateTaskPushNotificationConfig(request, context);
}
core::Result<lf::a2a::v1::TaskPushNotificationConfig> SecurityExecutor::GetTaskPushNotificationConfig(
    const lf::a2a::v1::GetTaskPushNotificationConfigRequest& request, server::RequestContext& context) {
  return executor_.GetTaskPushNotificationConfig(request, context);
}
core::Result<lf::a2a::v1::ListTaskPushNotificationConfigsResponse> SecurityExecutor::ListTaskPushNotificationConfigs(
    const lf::a2a::v1::ListTaskPushNotificationConfigsRequest& request, server::RequestContext& context) {
  return executor_.ListTaskPushNotificationConfigs(request, context);
}
core::Result<void> SecurityExecutor::DeleteTaskPushNotificationConfig(
    const lf::a2a::v1::DeleteTaskPushNotificationConfigRequest& request, server::RequestContext& context) {
  return executor_.DeleteTaskPushNotificationConfig(request, context);
}

void SecurityExecutor::PollCredentials() {
  while (!stopped_.load()) {
    server::ListTasksRequest query;
    query.status_filter = lf::a2a::v1::TASK_STATE_AUTH_REQUIRED;
    auto tasks = store_->List(query);
    if (tasks.ok()) {
      for (const auto& task : tasks.value().tasks) {
        const std::filesystem::path name(task.id());
        if (!IsAuthorizationTask(task.id()) || name != name.filename() ||
            name == std::filesystem::path(kParentDirectorySegment)) {
          continue;
        }
        std::ifstream fixture(credential_directory_ / name);
        std::string credential;
        for (char byte = 0; credential.size() <= kMaxCredentialSize && fixture.get(byte);) {
          credential.push_back(byte);
        }
        if (credential.size() > kMaxCredentialSize) {
          continue;
        }
        auto authorized = authorization_.Authorize(task.id(), credential);
        if (authorized.ok()) {
          (void)authorization_.Complete(task.id());
        }
      }
    }
    std::this_thread::sleep_for(kFixturePollInterval);
  }
}

}  // namespace a2a::tests::sut
