// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "a2a/server/engine/asio_agent_executor.h"

#include <atomic>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/strand.hpp>
#include <boost/system/system_error.hpp>
#include <exception>
#include <functional>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <utility>

namespace a2a::server::engine {
namespace {
constexpr auto kInvalidOptions = "Asio executor requires an application, scheduler, and nonzero capacity";
constexpr auto kCapacityExceeded = "Asio coroutine capacity exceeded";
constexpr auto kCancelled = "Asio coroutine cancelled";
constexpr auto kFailed = "Asio coroutine failed";

core::Error CoroutineError(const std::exception_ptr& exception) {
  try {
    std::rethrow_exception(exception);
  } catch (const boost::system::system_error& error) {
    if (error.code() == boost::asio::error::operation_aborted) {
      return core::Error::Network(kCancelled);
    }
    return core::Error::Internal(error.what());
  } catch (const std::exception& error) {
    return core::Error::Internal(error.what());
  } catch (...) {
    return core::Error::Internal(kFailed);
  }
}
}  // namespace

struct AsioAgentExecutor::State final {
  State(std::shared_ptr<CoroutineAgentExecutor> application, boost::asio::any_io_executor scheduler,
        std::size_t capacity)
      : application(std::move(application)), scheduler(std::move(scheduler)), capacity(capacity) {}

  template <typename Request, typename Response>
  struct Operation final : std::enable_shared_from_this<Operation<Request, Response>> {
    using Method = boost::asio::awaitable<core::Result<Response>> (CoroutineAgentExecutor::*)(Request,
                                                                                              RequestExecutionContext);
    Operation(std::shared_ptr<State> state, Request request, RequestExecutionContext context,
              Completion<Response> completion, Method method)
        : state(std::move(state)),
          strand(boost::asio::make_strand(this->state->scheduler)),
          request(std::move(request)),
          context(std::move(context)),
          completion(std::move(completion)),
          method(method) {}
    ~Operation() { state->pending.fetch_sub(1); }

    // This wrapper keeps the application alive and supplies a default-constructible
    // completion value for co_spawn's exception path without changing core::Result.
    static boost::asio::awaitable<std::optional<core::Result<Response>>> Invoke(
        std::shared_ptr<CoroutineAgentExecutor> application, Method method, Request request,
        RequestExecutionContext context) {
      auto work = std::invoke(method, application.get(), std::move(request), std::move(context));
      auto response = co_await std::move(work);
      co_return std::move(response);
    }

    void Start() {
      try {
        if (context.StopToken().stop_requested()) {
          Finish(core::Error::Network(kCancelled));
          return;
        }
        const auto weak = this->weak_from_this();
        observer = std::make_unique<std::stop_callback<std::function<void()>>>(context.StopToken(), [weak] {
          if (const auto operation = weak.lock()) {
            boost::asio::post(operation->strand, [operation] {
              if (!operation->done) {
                operation->cancellation.emit(boost::asio::cancellation_type::terminal);
              }
            });
          }
        });
        boost::asio::co_spawn(
            strand, Invoke(state->application, method, std::move(request), context),
            boost::asio::bind_cancellation_slot(
                cancellation.slot(), [self = this->shared_from_this()](const std::exception_ptr& exception,
                                                                       std::optional<core::Result<Response>> response) {
                  self->Finish(exception ? core::Result<Response>(CoroutineError(exception)) : std::move(*response));
                }));
      } catch (...) {
        Finish(CoroutineError(std::current_exception()));
      }
    }

    void Finish(core::Result<Response> response) {
      done = true;
      observer.reset();
      try {
        completion(std::move(response));
      } catch (...) {
        // Completion belongs to application infrastructure; preserve the event loop.
        return;
      }
    }

    const std::shared_ptr<State> state;
    boost::asio::strand<boost::asio::any_io_executor> strand;
    Request request;
    RequestExecutionContext context;
    Completion<Response> completion;
    Method method;
    boost::asio::cancellation_signal cancellation;
    std::unique_ptr<std::stop_callback<std::function<void()>>> observer;
    bool done = false;
  };

  template <typename Request, typename Response>
  static void Submit(const std::shared_ptr<State>& state, Request request, RequestExecutionContext context,
                     Completion<Response> completion, typename Operation<Request, Response>::Method method) {
    auto pending = state->pending.load();
    while (pending < state->capacity) {
      if (state->pending.compare_exchange_weak(pending, pending + 1)) {
        std::shared_ptr<Operation<Request, Response>> operation;
        try {
          operation = std::make_shared<Operation<Request, Response>>(state, std::move(request), std::move(context),
                                                                     std::move(completion), method);
        } catch (...) {
          state->pending.fetch_sub(1);
          throw;
        }
        boost::asio::post(operation->strand, [operation] { operation->Start(); });
        return;
      }
    }
    completion(core::Error::Network(kCapacityExceeded));
  }

  const std::shared_ptr<CoroutineAgentExecutor> application;
  const boost::asio::any_io_executor scheduler;
  const std::size_t capacity;
  std::atomic_size_t pending = 0;
};

AsioAgentExecutor::AsioAgentExecutor(std::shared_ptr<CoroutineAgentExecutor> executor,
                                     boost::asio::any_io_executor scheduler, std::size_t max_pending) {
  if (executor == nullptr || !scheduler || max_pending == 0) {
    throw std::invalid_argument(kInvalidOptions);
  }
  state_ = std::make_shared<State>(std::move(executor), std::move(scheduler), max_pending);
}
void AsioAgentExecutor::SendMessage(lf::a2a::v1::SendMessageRequest request, RequestExecutionContext context,
                                    Completion<lf::a2a::v1::SendMessageResponse> completion) {
  State::Submit<lf::a2a::v1::SendMessageRequest, lf::a2a::v1::SendMessageResponse>(
      state_, std::move(request), std::move(context), std::move(completion), &CoroutineAgentExecutor::SendMessage);
}
void AsioAgentExecutor::GetTask(lf::a2a::v1::GetTaskRequest request, RequestExecutionContext context,
                                Completion<lf::a2a::v1::Task> completion) {
  State::Submit<lf::a2a::v1::GetTaskRequest, lf::a2a::v1::Task>(
      state_, std::move(request), std::move(context), std::move(completion), &CoroutineAgentExecutor::GetTask);
}
void AsioAgentExecutor::ListTasks(ListTasksRequest request, RequestExecutionContext context,
                                  Completion<ListTasksResponse> completion) {
  State::Submit<ListTasksRequest, ListTasksResponse>(state_, std::move(request), std::move(context),
                                                     std::move(completion), &CoroutineAgentExecutor::ListTasks);
}
void AsioAgentExecutor::CancelTask(lf::a2a::v1::CancelTaskRequest request, RequestExecutionContext context,
                                   Completion<lf::a2a::v1::Task> completion) {
  State::Submit<lf::a2a::v1::CancelTaskRequest, lf::a2a::v1::Task>(
      state_, std::move(request), std::move(context), std::move(completion), &CoroutineAgentExecutor::CancelTask);
}
void AsioAgentExecutor::CreateTaskPushNotificationConfig(
    lf::a2a::v1::TaskPushNotificationConfig request, RequestExecutionContext context,
    Completion<lf::a2a::v1::TaskPushNotificationConfig> completion) {
  State::Submit<lf::a2a::v1::TaskPushNotificationConfig, lf::a2a::v1::TaskPushNotificationConfig>(
      state_, std::move(request), std::move(context), std::move(completion),
      &CoroutineAgentExecutor::CreateTaskPushNotificationConfig);
}
void AsioAgentExecutor::GetTaskPushNotificationConfig(lf::a2a::v1::GetTaskPushNotificationConfigRequest request,
                                                      RequestExecutionContext context,
                                                      Completion<lf::a2a::v1::TaskPushNotificationConfig> completion) {
  State::Submit<lf::a2a::v1::GetTaskPushNotificationConfigRequest, lf::a2a::v1::TaskPushNotificationConfig>(
      state_, std::move(request), std::move(context), std::move(completion),
      &CoroutineAgentExecutor::GetTaskPushNotificationConfig);
}
void AsioAgentExecutor::ListTaskPushNotificationConfigs(
    lf::a2a::v1::ListTaskPushNotificationConfigsRequest request, RequestExecutionContext context,
    Completion<lf::a2a::v1::ListTaskPushNotificationConfigsResponse> completion) {
  State::Submit<lf::a2a::v1::ListTaskPushNotificationConfigsRequest,
                lf::a2a::v1::ListTaskPushNotificationConfigsResponse>(
      state_, std::move(request), std::move(context), std::move(completion),
      &CoroutineAgentExecutor::ListTaskPushNotificationConfigs);
}
void AsioAgentExecutor::DeleteTaskPushNotificationConfig(lf::a2a::v1::DeleteTaskPushNotificationConfigRequest request,
                                                         RequestExecutionContext context, Completion<void> completion) {
  State::Submit<lf::a2a::v1::DeleteTaskPushNotificationConfigRequest, void>(
      state_, std::move(request), std::move(context), std::move(completion),
      &CoroutineAgentExecutor::DeleteTaskPushNotificationConfig);
}
}  // namespace a2a::server::engine
