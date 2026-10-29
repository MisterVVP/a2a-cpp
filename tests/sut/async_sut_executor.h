// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

#include <memory>
#include <utility>

#include "a2a/server/engine/async_agent_executor.h"
#include "sut/sut_application.h"
#include "sut/sut_work_queue.h"

namespace a2a::tests::sut {
class AsyncSutExecutor final : public server::engine::AsyncAgentExecutor {
 public:
  explicit AsyncSutExecutor(std::shared_ptr<SutApplication> application);
  void SendMessage(lf::a2a::v1::SendMessageRequest request, server::engine::RequestExecutionContext context,
                   Completion<lf::a2a::v1::SendMessageResponse> complete) override;
  void GetTask(lf::a2a::v1::GetTaskRequest request, server::engine::RequestExecutionContext context,
               Completion<lf::a2a::v1::Task> complete) override;
  void ListTasks(server::ListTasksRequest request, server::engine::RequestExecutionContext context,
                 Completion<server::ListTasksResponse> complete) override;
  void CancelTask(lf::a2a::v1::CancelTaskRequest request, server::engine::RequestExecutionContext context,
                  Completion<lf::a2a::v1::Task> complete) override;
  void CreateTaskPushNotificationConfig(lf::a2a::v1::TaskPushNotificationConfig request,
                                        server::engine::RequestExecutionContext context,
                                        Completion<lf::a2a::v1::TaskPushNotificationConfig> complete) override;
  void GetTaskPushNotificationConfig(lf::a2a::v1::GetTaskPushNotificationConfigRequest request,
                                     server::engine::RequestExecutionContext context,
                                     Completion<lf::a2a::v1::TaskPushNotificationConfig> complete) override;
  void ListTaskPushNotificationConfigs(
      lf::a2a::v1::ListTaskPushNotificationConfigsRequest request, server::engine::RequestExecutionContext context,
      Completion<lf::a2a::v1::ListTaskPushNotificationConfigsResponse> complete) override;
  void DeleteTaskPushNotificationConfig(lf::a2a::v1::DeleteTaskPushNotificationConfigRequest request,
                                        server::engine::RequestExecutionContext context,
                                        Completion<void> complete) override;

 private:
  template <typename Request, typename Response>
  void Submit(Request request, server::engine::RequestExecutionContext context, Completion<Response> complete,
              core::Result<Response> (server::AgentExecutor::*operation)(const Request&, server::RequestContext&)) {
    auto rejected = complete;
    const bool accepted =
        work_.Post([application = application_, request = std::move(request), context = std::move(context),
                    complete = std::move(complete), operation]() mutable {
          if (context.StopToken().stop_requested()) {
            complete(core::Error::Network(kStopped));
            return;
          }
          auto metadata = context.Metadata();
          core::Result<Response> result = core::Error::Internal(kOperationFailed);
          try {
            result = (application->executor().*operation)(request, metadata);
          } catch (...) {
            result = core::Error::Internal(kOperationFailed);
          }
          complete(std::move(result));
        });
    if (!accepted) {
      rejected(core::Error::Network(kStopped));
    }
  }
  static constexpr auto kOperationFailed = "SUT application operation threw an exception";
  static constexpr auto kStopped = "SUT operation cancelled or work queue unavailable";
  std::shared_ptr<SutApplication> application_;
  SutWorkQueue work_;
};
}  // namespace a2a::tests::sut
