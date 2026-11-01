// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "a2a/server/engine/coroutine_agent_executor.h"

#include "a2a/core/protocol_errors.h"

namespace a2a::server::engine {
boost::asio::awaitable<core::Result<void>> CoroutineAgentExecutor::SendStreamingMessage(
    lf::a2a::v1::SendMessageRequest /*request*/, RequestExecutionContext /*context*/,
    std::shared_ptr<AsyncServerStreamSession> /*stream*/) {
  co_return core::protocol_errors::UnsupportedOperation();
}
boost::asio::awaitable<core::Result<void>> CoroutineAgentExecutor::SubscribeTask(
    lf::a2a::v1::GetTaskRequest /*request*/, RequestExecutionContext /*context*/,
    std::shared_ptr<AsyncServerStreamSession> /*stream*/) {
  co_return core::protocol_errors::UnsupportedOperation();
}
boost::asio::awaitable<core::Result<lf::a2a::v1::SendMessageResponse>> CoroutineAgentExecutor::SendMessage(
    lf::a2a::v1::SendMessageRequest /*request*/, RequestExecutionContext /*context*/) {
  co_return core::protocol_errors::UnsupportedOperation();
}
boost::asio::awaitable<core::Result<lf::a2a::v1::Task>> CoroutineAgentExecutor::GetTask(
    lf::a2a::v1::GetTaskRequest /*request*/, RequestExecutionContext /*context*/) {
  co_return core::protocol_errors::UnsupportedOperation();
}
boost::asio::awaitable<core::Result<ListTasksResponse>> CoroutineAgentExecutor::ListTasks(
    ListTasksRequest /*request*/, RequestExecutionContext /*context*/) {
  co_return core::protocol_errors::UnsupportedOperation();
}
boost::asio::awaitable<core::Result<lf::a2a::v1::Task>> CoroutineAgentExecutor::CancelTask(
    lf::a2a::v1::CancelTaskRequest /*request*/, RequestExecutionContext /*context*/) {
  co_return core::protocol_errors::UnsupportedOperation();
}
boost::asio::awaitable<core::Result<lf::a2a::v1::TaskPushNotificationConfig>>
CoroutineAgentExecutor::CreateTaskPushNotificationConfig(lf::a2a::v1::TaskPushNotificationConfig /*request*/,
                                                         RequestExecutionContext /*context*/) {
  co_return core::protocol_errors::UnsupportedOperation();
}
boost::asio::awaitable<core::Result<lf::a2a::v1::TaskPushNotificationConfig>>
CoroutineAgentExecutor::GetTaskPushNotificationConfig(lf::a2a::v1::GetTaskPushNotificationConfigRequest /*request*/,
                                                      RequestExecutionContext /*context*/) {
  co_return core::protocol_errors::UnsupportedOperation();
}
boost::asio::awaitable<core::Result<lf::a2a::v1::ListTaskPushNotificationConfigsResponse>>
CoroutineAgentExecutor::ListTaskPushNotificationConfigs(lf::a2a::v1::ListTaskPushNotificationConfigsRequest /*request*/,
                                                        RequestExecutionContext /*context*/) {
  co_return core::protocol_errors::UnsupportedOperation();
}
boost::asio::awaitable<core::Result<void>> CoroutineAgentExecutor::DeleteTaskPushNotificationConfig(
    lf::a2a::v1::DeleteTaskPushNotificationConfigRequest /*request*/, RequestExecutionContext /*context*/) {
  co_return core::protocol_errors::UnsupportedOperation();
}
}  // namespace a2a::server::engine
