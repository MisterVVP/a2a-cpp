// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

#include <atomic>
#include <filesystem>
#include <string>
#include <thread>

#include "a2a/server/tasks/task_authorization_service.h"
#include "example_support.h"

namespace a2a::tests::sut {

inline constexpr std::string_view kAuthorizationTrigger = "request-authorization";
inline constexpr std::string_view kAuthorizationExplanation =
    "Supply the task credential using the out-of-band fixture directory.";
inline constexpr std::string_view kAuthorizationStatusId = "authorization-request";
inline constexpr char kCredentialDirectoryEnv[] = "A2A_TCK_CREDENTIAL_DIRECTORY";
inline constexpr char kTaskCredentialEnv[] = "A2A_TCK_TASK_CREDENTIAL";

class SecurityExecutor final : public server::AgentExecutor {
 public:
  SecurityExecutor(server::TaskStore* store, server::PushNotificationStore* push_store,
                   std::filesystem::path credential_directory, std::string credential);
  ~SecurityExecutor() override;
  core::Result<lf::a2a::v1::SendMessageResponse> SendMessage(const lf::a2a::v1::SendMessageRequest& request,
                                                             server::RequestContext& context) override;
  core::Result<std::unique_ptr<server::ServerStreamSession>> SendStreamingMessage(
      const lf::a2a::v1::SendMessageRequest& request, server::RequestContext& context) override;
  core::Result<lf::a2a::v1::Task> GetTask(const lf::a2a::v1::GetTaskRequest& request,
                                          server::RequestContext& context) override;
  core::Result<std::unique_ptr<server::ServerStreamSession>> SubscribeTask(const lf::a2a::v1::GetTaskRequest& request,
                                                                           server::RequestContext& context) override;
  core::Result<server::ListTasksResponse> ListTasks(const server::ListTasksRequest& request,
                                                    server::RequestContext& context) override;
  core::Result<lf::a2a::v1::Task> CancelTask(const lf::a2a::v1::CancelTaskRequest& request,
                                             server::RequestContext& context) override;
  core::Result<lf::a2a::v1::TaskPushNotificationConfig> CreateTaskPushNotificationConfig(
      const lf::a2a::v1::TaskPushNotificationConfig& request, server::RequestContext& context) override;
  core::Result<lf::a2a::v1::TaskPushNotificationConfig> GetTaskPushNotificationConfig(
      const lf::a2a::v1::GetTaskPushNotificationConfigRequest& request, server::RequestContext& context) override;
  core::Result<lf::a2a::v1::ListTaskPushNotificationConfigsResponse> ListTaskPushNotificationConfigs(
      const lf::a2a::v1::ListTaskPushNotificationConfigsRequest& request, server::RequestContext& context) override;
  core::Result<void> DeleteTaskPushNotificationConfig(
      const lf::a2a::v1::DeleteTaskPushNotificationConfigRequest& request, server::RequestContext& context) override;
  void ShutdownSubscriptions();

 private:
  [[nodiscard]] bool IsAuthorizationTask(std::string_view id) const;
  void PollCredentials();
  server::TaskStore* store_;
  examples::ExampleExecutor executor_;
  server::TaskAuthorizationService authorization_;
  std::filesystem::path credential_directory_;
  std::atomic_bool stopped_ = false;
  std::thread worker_;
};

}  // namespace a2a::tests::sut
