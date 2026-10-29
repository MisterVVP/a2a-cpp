// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

#include <memory>

#include "a2a/server/engine/coroutine_agent_executor.h"
#include "sut/sut_application.h"
#include "sut/sut_work_queue.h"

namespace a2a::tests::sut {
class AsyncSutExecutor final : public server::engine::CoroutineAgentExecutor {
 public:
  explicit AsyncSutExecutor(std::shared_ptr<SutApplication> application);
  boost::asio::awaitable<core::Result<lf::a2a::v1::SendMessageResponse>> SendMessage(
      lf::a2a::v1::SendMessageRequest request, server::engine::RequestExecutionContext context) override;
  boost::asio::awaitable<core::Result<lf::a2a::v1::Task>> GetTask(
      lf::a2a::v1::GetTaskRequest request, server::engine::RequestExecutionContext context) override;
  boost::asio::awaitable<core::Result<server::ListTasksResponse>> ListTasks(
      server::ListTasksRequest request, server::engine::RequestExecutionContext context) override;
  boost::asio::awaitable<core::Result<lf::a2a::v1::Task>> CancelTask(
      lf::a2a::v1::CancelTaskRequest request, server::engine::RequestExecutionContext context) override;
  boost::asio::awaitable<core::Result<lf::a2a::v1::TaskPushNotificationConfig>> CreateTaskPushNotificationConfig(
      lf::a2a::v1::TaskPushNotificationConfig request, server::engine::RequestExecutionContext context) override;
  boost::asio::awaitable<core::Result<lf::a2a::v1::TaskPushNotificationConfig>> GetTaskPushNotificationConfig(
      lf::a2a::v1::GetTaskPushNotificationConfigRequest request,
      server::engine::RequestExecutionContext context) override;
  boost::asio::awaitable<core::Result<lf::a2a::v1::ListTaskPushNotificationConfigsResponse>>
  ListTaskPushNotificationConfigs(lf::a2a::v1::ListTaskPushNotificationConfigsRequest request,
                                  server::engine::RequestExecutionContext context) override;
  boost::asio::awaitable<core::Result<void>> DeleteTaskPushNotificationConfig(
      lf::a2a::v1::DeleteTaskPushNotificationConfigRequest request,
      server::engine::RequestExecutionContext context) override;

 private:
  template <typename Request, typename Response>
  boost::asio::awaitable<core::Result<Response>> Submit(
      Request request, server::engine::RequestExecutionContext context,
      core::Result<Response> (server::AgentExecutor::*operation)(const Request&, server::RequestContext&));
  std::shared_ptr<SutApplication> application_;
  SutWorkQueue work_;
};
}  // namespace a2a::tests::sut
