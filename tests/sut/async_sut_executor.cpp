// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "sut/async_sut_executor.h"

#include <utility>

#include "sut/sut_awaitable.h"

namespace a2a::tests::sut {
template <typename Request, typename Response>
boost::asio::awaitable<core::Result<Response>> AsyncSutExecutor::Submit(
    Request request, server::engine::RequestExecutionContext context,
    core::Result<Response> (server::AgentExecutor::*operation)(const Request&, server::RequestContext&)) {
  co_return co_await AwaitSutWork<Response>(
      work_,
      [application = application_, request = std::move(request), context = std::move(context),
       operation]() mutable -> core::Result<Response> {
        if (context.StopToken().stop_requested()) {
          return core::Error::Network(kSutOperationStopped);
        }
        auto metadata = context.Metadata();
        return (application->executor().*operation)(request, metadata);
      });
}

AsyncSutExecutor::AsyncSutExecutor(std::shared_ptr<SutApplication> application)
    : application_(std::move(application)) {}
boost::asio::awaitable<core::Result<lf::a2a::v1::SendMessageResponse>> AsyncSutExecutor::SendMessage(
    lf::a2a::v1::SendMessageRequest request, server::engine::RequestExecutionContext context) {
  co_return co_await Submit(std::move(request), std::move(context), &server::AgentExecutor::SendMessage);
}
boost::asio::awaitable<core::Result<lf::a2a::v1::Task>> AsyncSutExecutor::GetTask(
    lf::a2a::v1::GetTaskRequest request, server::engine::RequestExecutionContext context) {
  co_return co_await Submit(std::move(request), std::move(context), &server::AgentExecutor::GetTask);
}
boost::asio::awaitable<core::Result<server::ListTasksResponse>> AsyncSutExecutor::ListTasks(
    server::ListTasksRequest request, server::engine::RequestExecutionContext context) {
  co_return co_await Submit(std::move(request), std::move(context), &server::AgentExecutor::ListTasks);
}
boost::asio::awaitable<core::Result<lf::a2a::v1::Task>> AsyncSutExecutor::CancelTask(
    lf::a2a::v1::CancelTaskRequest request, server::engine::RequestExecutionContext context) {
  co_return co_await Submit(std::move(request), std::move(context), &server::AgentExecutor::CancelTask);
}
boost::asio::awaitable<core::Result<lf::a2a::v1::TaskPushNotificationConfig>>
AsyncSutExecutor::CreateTaskPushNotificationConfig(lf::a2a::v1::TaskPushNotificationConfig request,
                                                   server::engine::RequestExecutionContext context) {
  co_return co_await Submit(std::move(request), std::move(context),
                            &server::AgentExecutor::CreateTaskPushNotificationConfig);
}
boost::asio::awaitable<core::Result<lf::a2a::v1::TaskPushNotificationConfig>>
AsyncSutExecutor::GetTaskPushNotificationConfig(lf::a2a::v1::GetTaskPushNotificationConfigRequest request,
                                                server::engine::RequestExecutionContext context) {
  co_return co_await Submit(std::move(request), std::move(context),
                            &server::AgentExecutor::GetTaskPushNotificationConfig);
}
boost::asio::awaitable<core::Result<lf::a2a::v1::ListTaskPushNotificationConfigsResponse>>
AsyncSutExecutor::ListTaskPushNotificationConfigs(lf::a2a::v1::ListTaskPushNotificationConfigsRequest request,
                                                  server::engine::RequestExecutionContext context) {
  co_return co_await Submit(std::move(request), std::move(context),
                            &server::AgentExecutor::ListTaskPushNotificationConfigs);
}
boost::asio::awaitable<core::Result<void>> AsyncSutExecutor::DeleteTaskPushNotificationConfig(
    lf::a2a::v1::DeleteTaskPushNotificationConfigRequest request, server::engine::RequestExecutionContext context) {
  co_return co_await Submit(std::move(request), std::move(context),
                            &server::AgentExecutor::DeleteTaskPushNotificationConfig);
}
}  // namespace a2a::tests::sut
