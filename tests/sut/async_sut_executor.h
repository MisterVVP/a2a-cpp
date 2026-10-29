// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

#include <boost/asio/associated_executor.hpp>
#include <boost/asio/async_result.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <memory>
#include <utility>

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
      core::Result<Response> (server::AgentExecutor::*operation)(const Request&, server::RequestContext&)) {
    auto initiate = [this, request = std::move(request), context = std::move(context),
                     operation](auto handler) mutable {
      using Handler = decltype(handler);
      const auto completion = std::make_shared<Handler>(std::move(handler));
      const auto scheduler = boost::asio::get_associated_executor(*completion);
      const auto work =
          std::make_shared<decltype(boost::asio::make_work_guard(scheduler))>(boost::asio::make_work_guard(scheduler));
      auto deliver = [completion, scheduler, work](core::Result<Response> result) {
        boost::asio::post(
            scheduler, [completion, work, result = std::move(result)]() mutable { (*completion)(std::move(result)); });
      };
      const bool accepted = work_.Post([application = application_, request = std::move(request),
                                        context = std::move(context), operation, deliver]() mutable {
        if (context.StopToken().stop_requested()) {
          deliver(core::Error::Network(kStopped));
          return;
        }
        auto metadata = context.Metadata();
        core::Result<Response> result = core::Error::Internal(kOperationFailed);
        try {
          result = (application->executor().*operation)(request, metadata);
        } catch (...) {
          result = core::Error::Internal(kOperationFailed);
        }
        deliver(std::move(result));
      });
      if (!accepted) {
        deliver(core::Error::Network(kStopped));
      }
    };
    co_return co_await boost::asio::async_initiate<decltype(boost::asio::use_awaitable), void(core::Result<Response>)>(
        std::move(initiate), boost::asio::use_awaitable);
  }
  static constexpr auto kOperationFailed = "SUT application operation threw an exception";
  static constexpr auto kStopped = "SUT operation cancelled or work queue unavailable";
  std::shared_ptr<SutApplication> application_;
  SutWorkQueue work_;
};
}  // namespace a2a::tests::sut
