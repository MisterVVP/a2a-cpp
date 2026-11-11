// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

#include <boost/asio/associated_executor.hpp>
#include <boost/asio/async_result.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/thread_pool.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <memory>
#include <utility>

#include "a2a/server/engine/async_task_subscription.h"
#include "a2a/server/engine/blocking_executor_adapter.h"
#include "a2a/server/tasks/in_memory_task_store.h"
#include "sut/sut_agent.h"

namespace a2a::tests::sut {
// Ready in-memory work runs on the coroutine executor. Blocking stores retain
// bounded adaptation; live subscriptions await the shared publication core.
class SutCoroutineExecutor final : public server::engine::CoroutineAgentExecutor {
 public:
  explicit SutCoroutineExecutor(SutAgent& agent)
      : agent_(agent),
        executor_(agent.executor()),
        in_memory_(dynamic_cast<server::InMemoryTaskStore*>(agent.task_store()) != nullptr),
        unary_(executor_) {}
  boost::asio::awaitable<core::Result<void>> SendStreamingMessage(
      lf::a2a::v1::SendMessageRequest request, server::engine::RequestExecutionContext context,
      std::shared_ptr<server::engine::AsyncServerStreamSession> stream) override {
    auto events =
        co_await Backend<std::vector<lf::a2a::v1::StreamResponse>>([this, request = std::move(request), context] {
          if (context.StopToken().stop_requested()) {
            return core::Result<std::vector<lf::a2a::v1::StreamResponse>>(core::Error::Network(kFixtureCancelled));
          }
          return agent_.BuildStreamingEvents(request, context.Metadata());
        });
    if (!events.ok()) {
      co_return events.error();
    }
    for (auto& event : events.value()) {
      auto written = co_await stream->Write(std::move(event));
      if (!written.ok()) {
        co_return written.error();
      }
    }
    co_return core::Result<void>{};
  }
  boost::asio::awaitable<core::Result<void>> SubscribeTask(
      lf::a2a::v1::GetTaskRequest request, server::engine::RequestExecutionContext context,
      std::shared_ptr<server::engine::AsyncServerStreamSession> stream) override {
    auto task = co_await GetTask(std::move(request), context);
    if (!task.ok()) {
      co_return task.error();
    }
    co_return co_await server::engine::AsyncTaskSubscription::Run(agent_.TaskSubscriptions(), std::move(task.value()),
                                                                  context, std::move(stream));
  }
  boost::asio::awaitable<core::Result<lf::a2a::v1::SendMessageResponse>> SendMessage(
      lf::a2a::v1::SendMessageRequest request, server::engine::RequestExecutionContext context) override {
    return Invoke(std::move(request), std::move(context), &server::AgentExecutor::SendMessage,
                  &server::engine::BlockingExecutorAdapter::SendMessage);
  }
  boost::asio::awaitable<core::Result<lf::a2a::v1::Task>> GetTask(
      lf::a2a::v1::GetTaskRequest request, server::engine::RequestExecutionContext context) override {
    return Invoke(std::move(request), std::move(context), &server::AgentExecutor::GetTask,
                  &server::engine::BlockingExecutorAdapter::GetTask);
  }
  boost::asio::awaitable<core::Result<server::ListTasksResponse>> ListTasks(
      server::ListTasksRequest request, server::engine::RequestExecutionContext context) override {
    return Invoke(std::move(request), std::move(context), &server::AgentExecutor::ListTasks,
                  &server::engine::BlockingExecutorAdapter::ListTasks);
  }
  boost::asio::awaitable<core::Result<lf::a2a::v1::Task>> CancelTask(
      lf::a2a::v1::CancelTaskRequest request, server::engine::RequestExecutionContext context) override {
    return Invoke(std::move(request), std::move(context), &server::AgentExecutor::CancelTask,
                  &server::engine::BlockingExecutorAdapter::CancelTask);
  }
  boost::asio::awaitable<core::Result<lf::a2a::v1::TaskPushNotificationConfig>> CreateTaskPushNotificationConfig(
      lf::a2a::v1::TaskPushNotificationConfig request, server::engine::RequestExecutionContext context) override {
    return Invoke(std::move(request), std::move(context), &server::AgentExecutor::CreateTaskPushNotificationConfig,
                  &server::engine::BlockingExecutorAdapter::CreateTaskPushNotificationConfig);
  }
  boost::asio::awaitable<core::Result<lf::a2a::v1::TaskPushNotificationConfig>> GetTaskPushNotificationConfig(
      lf::a2a::v1::GetTaskPushNotificationConfigRequest request,
      server::engine::RequestExecutionContext context) override {
    return Invoke(std::move(request), std::move(context), &server::AgentExecutor::GetTaskPushNotificationConfig,
                  &server::engine::BlockingExecutorAdapter::GetTaskPushNotificationConfig);
  }
  boost::asio::awaitable<core::Result<lf::a2a::v1::ListTaskPushNotificationConfigsResponse>>
  ListTaskPushNotificationConfigs(lf::a2a::v1::ListTaskPushNotificationConfigsRequest request,
                                  server::engine::RequestExecutionContext context) override {
    return Invoke(std::move(request), std::move(context), &server::AgentExecutor::ListTaskPushNotificationConfigs,
                  &server::engine::BlockingExecutorAdapter::ListTaskPushNotificationConfigs);
  }
  boost::asio::awaitable<core::Result<void>> DeleteTaskPushNotificationConfig(
      lf::a2a::v1::DeleteTaskPushNotificationConfigRequest request,
      server::engine::RequestExecutionContext context) override {
    return Invoke(std::move(request), std::move(context), &server::AgentExecutor::DeleteTaskPushNotificationConfig,
                  &server::engine::BlockingExecutorAdapter::DeleteTaskPushNotificationConfig);
  }

 private:
  template <typename Request, typename Response>
  boost::asio::awaitable<core::Result<Response>> Invoke(
      Request request, server::engine::RequestExecutionContext context,
      core::Result<Response> (server::AgentExecutor::*operation)(const Request&, server::RequestContext&),
      boost::asio::awaitable<core::Result<Response>> (server::engine::BlockingExecutorAdapter::*adapted)(
          Request, server::engine::RequestExecutionContext)) {
    if (!in_memory_) {
      return (unary_.*adapted)(std::move(request), std::move(context));
    }
    return RunReady(std::move(request), std::move(context), operation);
  }
  template <typename Request, typename Response>
  boost::asio::awaitable<core::Result<Response>> RunReady(
      Request request, server::engine::RequestExecutionContext context,
      core::Result<Response> (server::AgentExecutor::*operation)(const Request&, server::RequestContext&)) {
    if (context.StopToken().stop_requested()) {
      co_return core::Error::Network(kFixtureCancelled);
    }
    auto metadata = context.Metadata();
    co_return (executor_.get()->*operation)(request, metadata);
  }
  template <typename T, typename Work>
  boost::asio::awaitable<core::Result<T>> Backend(Work work) {
    if (in_memory_) {
      try {
        co_return work();
      } catch (...) {
        co_return core::Error::Internal(kFixtureFailure);
      }
    }
    auto initiate = [this, work = std::move(work)](auto handler) mutable {
      const auto scheduler = boost::asio::get_associated_executor(handler);
      auto guard = boost::asio::make_work_guard(scheduler);
      boost::asio::post(backend_, [scheduler, handler = std::move(handler), guard = std::move(guard),
                                   work = std::move(work)]() mutable {
        core::Result<T> result = core::Error::Internal(kFixtureFailure);
        try {
          result = work();
        } catch (...) {
          result = core::Error::Internal(kFixtureFailure);
        }
        boost::asio::post(scheduler, [handler = std::move(handler), guard = std::move(guard),
                                      result = std::move(result)]() mutable { handler(std::move(result)); });
      });
    };
    co_return co_await boost::asio::async_initiate<decltype(boost::asio::use_awaitable), void(core::Result<T>)>(
        std::move(initiate), boost::asio::use_awaitable);
  }
  static constexpr auto kFixtureCancelled = "Async streaming fixture cancelled before startup";
  static constexpr auto kFixtureFailure = "Async streaming fixture startup failed";
  static constexpr std::size_t kBackendWorkers = 2;
  SutAgent& agent_;
  const std::shared_ptr<server::AgentExecutor> executor_;
  const bool in_memory_;
  server::engine::BlockingExecutorAdapter unary_;
  boost::asio::thread_pool backend_{kBackendWorkers};
};
}  // namespace a2a::tests::sut
