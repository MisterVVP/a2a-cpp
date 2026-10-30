// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "a2a/server/engine/blocking_executor_adapter.h"

#include <boost/asio/associated_executor.hpp>
#include <boost/asio/async_result.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/thread_pool.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace a2a::server::engine {
namespace {
constexpr auto kInvalidOptions = "Blocking executor requires an application and positive worker/pending limits";
constexpr auto kUnavailable = "Blocking executor is closed or at capacity";
constexpr auto kStopped = "Blocking executor operation cancelled before backend execution";
constexpr auto kFailed = "Blocking executor operation threw an exception";
constexpr auto kJoinFromWorker = "Join must run outside blocking backend operations";
bool CanStart(const RequestExecutionContext& context) {
  const auto deadline = context.Deadline();
  return !context.StopToken().stop_requested() && (!deadline || RequestExecutionContext::Clock::now() < *deadline);
}
std::size_t Validate(const std::shared_ptr<AgentExecutor>& executor, const BlockingExecutorOptions& options) {
  if (!executor || options.workers == 0 || options.max_pending == 0) {
    throw std::invalid_argument(kInvalidOptions);
  }
  return options.workers;
}
}  // namespace
struct BlockingExecutorAdapter::State final {
  State(std::shared_ptr<AgentExecutor> application, const BlockingExecutorOptions& options)
      : executor(std::move(application)), pool(Validate(executor, options)), capacity(options.max_pending) {}
  template <typename Work>
  bool Schedule(Work work) {
    std::lock_guard lock(mutex);
    if (closed || pending == capacity) {
      return false;
    }
    ++pending;
    try {
      boost::asio::post(pool, std::move(work));
    } catch (...) {
      --pending;
      throw;
    }
    return true;
  }
  void Release() {
    std::lock_guard lock(mutex);
    --pending;
  }
  void Close() {
    std::lock_guard lock(mutex);
    closed = true;
  }
  const std::shared_ptr<AgentExecutor> executor;
  boost::asio::thread_pool pool;
  const std::size_t capacity;
  std::mutex mutex;
  std::mutex join_mutex;
  std::size_t pending = 0;
  bool closed = false;
};
BlockingExecutorAdapter::BlockingExecutorAdapter(std::shared_ptr<AgentExecutor> executor,
                                                 BlockingExecutorOptions options)
    : state_(std::make_shared<State>(std::move(executor), options)) {}
BlockingExecutorAdapter::~BlockingExecutorAdapter() {
  try {
    Join();
  } catch (...) {
    std::terminate();
  }
}
void BlockingExecutorAdapter::Shutdown() { state_->Close(); }
void BlockingExecutorAdapter::Join() {
  if (state_->pool.get_executor().running_in_this_thread()) {
    throw std::logic_error(kJoinFromWorker);
  }
  Shutdown();
  std::lock_guard lock(state_->join_mutex);
  state_->pool.join();
}

template <typename Request, typename Response>
boost::asio::awaitable<core::Result<Response>> BlockingExecutorAdapter::Submit(
    std::shared_ptr<State> state, Request request, RequestExecutionContext context,
    core::Result<Response> (AgentExecutor::*operation)(const Request&, RequestContext&)) {
  auto initiate = [state, request = std::move(request), context = std::move(context), operation](auto handler) mutable {
    const auto completion = std::make_shared<decltype(handler)>(std::move(handler));
    const auto scheduler = boost::asio::get_associated_executor(*completion);
    const auto work =
        std::make_shared<decltype(boost::asio::make_work_guard(scheduler))>(boost::asio::make_work_guard(scheduler));
    auto deliver = [completion, scheduler, work](core::Result<Response> result) {
      boost::asio::post(scheduler,
                        [completion, work, result = std::move(result)]() mutable { (*completion)(std::move(result)); });
    };
    const bool accepted = state->Schedule(
        [state, request = std::move(request), context = std::move(context), operation, deliver]() mutable {
          core::Result<Response> result = core::Error::Network(kStopped);
          try {
            if (CanStart(context)) {
              auto metadata = context.Metadata();
              result = (state->executor.get()->*operation)(request, metadata);
            }
          } catch (...) {
            result = core::Error::Internal(kFailed);
          }
          state->Release();
          deliver(std::move(result));
        });
    if (!accepted) {
      deliver(core::Error::Network(kUnavailable));
    }
  };
  co_return co_await boost::asio::async_initiate<decltype(boost::asio::use_awaitable), void(core::Result<Response>)>(
      std::move(initiate), boost::asio::use_awaitable);
}

boost::asio::awaitable<core::Result<lf::a2a::v1::SendMessageResponse>> BlockingExecutorAdapter::SendMessage(
    lf::a2a::v1::SendMessageRequest request, RequestExecutionContext context) {
  return Submit(state_, std::move(request), std::move(context), &AgentExecutor::SendMessage);
}
boost::asio::awaitable<core::Result<lf::a2a::v1::Task>> BlockingExecutorAdapter::GetTask(
    lf::a2a::v1::GetTaskRequest request, RequestExecutionContext context) {
  return Submit(state_, std::move(request), std::move(context), &AgentExecutor::GetTask);
}
boost::asio::awaitable<core::Result<ListTasksResponse>> BlockingExecutorAdapter::ListTasks(
    ListTasksRequest request, RequestExecutionContext context) {
  return Submit(state_, std::move(request), std::move(context), &AgentExecutor::ListTasks);
}
boost::asio::awaitable<core::Result<lf::a2a::v1::Task>> BlockingExecutorAdapter::CancelTask(
    lf::a2a::v1::CancelTaskRequest request, RequestExecutionContext context) {
  return Submit(state_, std::move(request), std::move(context), &AgentExecutor::CancelTask);
}
boost::asio::awaitable<core::Result<lf::a2a::v1::TaskPushNotificationConfig>>
BlockingExecutorAdapter::CreateTaskPushNotificationConfig(lf::a2a::v1::TaskPushNotificationConfig request,
                                                          RequestExecutionContext context) {
  return Submit(state_, std::move(request), std::move(context), &AgentExecutor::CreateTaskPushNotificationConfig);
}
boost::asio::awaitable<core::Result<lf::a2a::v1::TaskPushNotificationConfig>>
BlockingExecutorAdapter::GetTaskPushNotificationConfig(lf::a2a::v1::GetTaskPushNotificationConfigRequest request,
                                                       RequestExecutionContext context) {
  return Submit(state_, std::move(request), std::move(context), &AgentExecutor::GetTaskPushNotificationConfig);
}
boost::asio::awaitable<core::Result<lf::a2a::v1::ListTaskPushNotificationConfigsResponse>>
BlockingExecutorAdapter::ListTaskPushNotificationConfigs(lf::a2a::v1::ListTaskPushNotificationConfigsRequest request,
                                                         RequestExecutionContext context) {
  return Submit(state_, std::move(request), std::move(context), &AgentExecutor::ListTaskPushNotificationConfigs);
}
boost::asio::awaitable<core::Result<void>> BlockingExecutorAdapter::DeleteTaskPushNotificationConfig(
    lf::a2a::v1::DeleteTaskPushNotificationConfigRequest request, RequestExecutionContext context) {
  return Submit(state_, std::move(request), std::move(context), &AgentExecutor::DeleteTaskPushNotificationConfig);
}
}  // namespace a2a::server::engine
