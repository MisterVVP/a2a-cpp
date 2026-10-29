// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "a2a/server/engine/async_dispatcher.h"

#include <atomic>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/strand.hpp>
#include <boost/system/system_error.hpp>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <stop_token>
#include <unordered_map>
#include <utility>

#include "a2a/server/required_extensions_validator.h"
#include "server/engine/dispatch_pipeline.h"

namespace a2a::server::engine {
namespace {
constexpr auto kInvalidOptions = "Async dispatcher requires an executor, Asio scheduler, and nonzero capacity";
constexpr auto kAdmissionClosed = "Async dispatcher is shut down";
constexpr auto kAdmissionFull = "Async dispatcher capacity exceeded";
constexpr auto kPendingFull = "Async dispatcher coroutine capacity exceeded";
constexpr auto kMissingCompletion = "Transport completion is required";
constexpr auto kClientCancelled = "Request cancelled by client";
constexpr auto kDeadlineExceeded = "Request deadline exceeded";
constexpr auto kServerShutdown = "Request cancelled by server shutdown";

core::Error CancellationError(RequestOutcome outcome) {
  switch (outcome) {
    case RequestOutcome::kDeadlineExceeded:
      return core::Error::Network(kDeadlineExceeded);
    case RequestOutcome::kServerShutdown:
      return core::Error::Network(kServerShutdown);
    default:
      return core::Error::Network(kClientCancelled);
  }
}

core::Error CoroutineError(const std::exception_ptr& exception) {
  try {
    std::rethrow_exception(exception);
  } catch (const boost::system::system_error& error) {
    return error.code() == boost::asio::error::operation_aborted ? core::Error::Network(kClientCancelled)
                                                                 : core::Error::Internal(kDispatchException);
  } catch (...) {
    return core::Error::Internal(kDispatchException);
  }
}

struct CoroutineDispatchResult final {
  using ReturnType = boost::asio::awaitable<core::Result<DispatchResponse>>;
  template <typename T>
  ReturnType operator()(boost::asio::awaitable<core::Result<T>> operation) const {
    auto result = co_await std::move(operation);
    co_return ToDispatchResponse(std::move(result));
  }
  ReturnType operator()(core::Result<DispatchResponse> result) const { co_return std::move(result); }
};
}  // namespace

struct AsyncDispatcher::State final {
  struct Call final : std::enable_shared_from_this<Call> {
    Call(const boost::asio::any_io_executor& scheduler, std::shared_ptr<std::atomic_size_t> pending,
         DispatchRequest request, RequestContext metadata, TransportCompletion completion,
         std::optional<RequestExecution::Clock::time_point> deadline)
        : strand(boost::asio::make_strand(scheduler)),
          pending(std::move(pending)),
          request(std::move(request)),
          context(metadata),
          source(std::make_shared<RequestExecution>(std::move(metadata), deadline)),
          completion(std::move(completion)) {}
    ~Call() { pending->fetch_sub(1); }

    void ObserveStop() {
      const auto weak = weak_from_this();
      observer = std::make_unique<std::stop_callback<std::function<void()>>>(source->Context().StopToken(), [weak] {
        if (const auto call = weak.lock()) {
          boost::asio::post(call->strand, [call] { call->Stopped(); });
        }
      });
    }

    void Stopped() {
      if (finished) {
        return;
      }
      cancellation.emit(boost::asio::cancellation_type::terminal);
      // Before/After hooks are serialized on this strand. Run observes cancellation
      // before entering application code even if this notification precedes startup.
      if (started) {
        Finish(CancellationError(source->Context().Outcome()));
      }
    }

    static boost::asio::awaitable<void> Run(std::shared_ptr<State> state, std::shared_ptr<Call> call) {
      call->started = true;
      core::Result<DispatchResponse> response = core::Error::Internal(kDispatchException);
      try {
        const auto valid = state->extensions.Validate(call->context.client_headers);
        if (!valid.ok()) {
          response = valid.error();
        } else {
          const auto before = call->pipeline.Before(call->request, call->context, call->principal);
          (void)call->source->ExpireDeadline();
          call->ObserveShutdown(*state);
          if (!before.ok()) {
            response = before.error();
          } else if (call->source->Context().StopToken().stop_requested()) {
            response = CancellationError(call->source->Context().Outcome());
          } else if (auto handled = call->pipeline.Handle(call->request, call->context)) {
            response = std::move(*handled);
          } else {
            auto view = call->source->Context().WithMetadata(call->context);
            const auto prepared = call->pipeline.Prepare(call->request);
            response = co_await RouteDispatch(*state->executor, prepared.has_value() ? *prepared : call->request, view,
                                              CoroutineDispatchResult{});
          }
        }
      } catch (...) {
        response = CoroutineError(std::current_exception());
      }
      call->Finish(std::move(response));
    }

    void ObserveShutdown(State& state) {
      bool closed = false;
      {
        std::lock_guard lock(state.mutex);
        closed = state.closed;
      }
      if (closed) {
        (void)source->Cancel(RequestCancellationReason::kServerShutdown);
      }
    }

    void Finish(core::Result<DispatchResponse> response) {
      if (finished) {
        return;
      }
      finished = true;
      try {
        if (!source->Context().StopToken().stop_requested()) {
          pipeline.Project(request, context, principal, response);
        }
      } catch (...) {
        response = CoroutineError(std::current_exception());
      }
      if (!source->TryComplete(response.ok() ? RequestCompletion::kSucceeded : RequestCompletion::kFailed)) {
        response = CancellationError(source->Context().Outcome());
      }
      pipeline.After(request, context, principal, response, /*contain_exceptions=*/true);
      if (const auto state = owner.lock()) {
        std::lock_guard lock(state->mutex);
        state->calls.erase(this);
      }
      auto deliver = std::move(completion);
      try {
        deliver(std::move(response), source->Context());
      } catch (...) {
        return;
      }
    }

    boost::asio::strand<boost::asio::any_io_executor> strand;
    const std::shared_ptr<std::atomic_size_t> pending;
    DispatchRequest request;
    RequestContext context;
    std::shared_ptr<RequestExecution> source;
    TransportCompletion completion;
    DispatchPipeline pipeline;
    std::optional<std::string> principal;
    std::weak_ptr<State> owner;
    boost::asio::cancellation_signal cancellation;
    std::unique_ptr<std::stop_callback<std::function<void()>>> observer;
    bool started = false;
    bool finished = false;
  };

  State(std::shared_ptr<CoroutineAgentExecutor> application, boost::asio::any_io_executor scheduler,
        AsyncDispatcherOptions options)
      : executor(std::move(application)),
        scheduler(std::move(scheduler)),
        capacity(options.max_in_flight),
        max_pending(options.max_pending),
        pipeline{
            .security = options.security_policy != nullptr
                            ? std::make_shared<DispatchSecurity>(std::move(options.security_policy), options.task_store)
                            : nullptr,
            .interceptors = std::move(options.interceptors),
            .agent_card_provider = std::move(options.agent_card_provider)},
        extensions(std::move(options.required_extensions)) {}

  void Shutdown() {
    std::vector<std::shared_ptr<Call>> active;
    {
      std::lock_guard lock(mutex);
      if (closed) {
        return;
      }
      closed = true;
      active.reserve(calls.size());
      for (const auto& [key, call] : calls) {
        active.push_back(call);
      }
    }
    for (const auto& call : active) {
      boost::asio::post(call->strand,
                        [call] { (void)call->source->Cancel(RequestCancellationReason::kServerShutdown); });
    }
  }

  const std::shared_ptr<CoroutineAgentExecutor> executor;
  const boost::asio::any_io_executor scheduler;
  const std::size_t capacity;
  const std::size_t max_pending;
  const std::shared_ptr<std::atomic_size_t> pending = std::make_shared<std::atomic_size_t>(0);
  DispatchPipeline pipeline;
  RequiredExtensionsValidator extensions;
  std::mutex mutex;
  std::unordered_map<Call*, std::shared_ptr<Call>> calls;
  bool closed = false;
};

AsyncDispatcher::AsyncDispatcher(std::shared_ptr<CoroutineAgentExecutor> executor,
                                 boost::asio::any_io_executor scheduler, AsyncDispatcherOptions options) {
  if (executor == nullptr || !scheduler || options.max_in_flight == 0 || options.max_pending == 0) {
    throw std::invalid_argument(kInvalidOptions);
  }
  state_ = std::make_shared<State>(std::move(executor), std::move(scheduler), std::move(options));
}
AsyncDispatcher::~AsyncDispatcher() {
  try {
    Shutdown();
  } catch (...) {
    std::terminate();
  }
}
void AsyncDispatcher::Shutdown() { state_->Shutdown(); }
void AsyncDispatcher::Cancel(std::shared_ptr<RequestExecution> execution, RequestCancellationReason reason) {
  if (execution != nullptr) {
    boost::asio::post(state_->scheduler,
                      [execution = std::move(execution), reason] { (void)execution->Cancel(reason); });
  }
}

core::Result<std::shared_ptr<RequestExecution>> AsyncDispatcher::Dispatch(
    DispatchRequest request, RequestContext metadata, TransportCompletion completion,
    std::optional<RequestExecution::Clock::time_point> deadline) {
  if (!completion) {
    return core::Error::Validation(kMissingCompletion);
  }
  const auto state = state_;
  std::lock_guard lock(state->mutex);
  if (state->closed) {
    return core::Error::Network(kAdmissionClosed);
  }
  if (state->calls.size() >= state->capacity) {
    return core::Error::Network(kAdmissionFull);
  }
  if (state->pending->load() >= state->max_pending) {
    return core::Error::Network(kPendingFull);
  }
  state->pending->fetch_add(1);
  std::shared_ptr<State::Call> call;
  try {
    call = std::make_shared<State::Call>(state->scheduler, state->pending, std::move(request), std::move(metadata),
                                         std::move(completion), deadline);
  } catch (...) {
    state->pending->fetch_sub(1);
    throw;
  }
  call->pipeline = state->pipeline;
  call->owner = state;
  call->ObserveStop();
  state->calls.emplace(call.get(), call);
  boost::asio::post(call->strand, [state, call] {
    boost::asio::co_spawn(
        call->strand, State::Call::Run(state, call),
        boost::asio::bind_cancellation_slot(call->cancellation.slot(), [call](const std::exception_ptr& exception) {
          if (exception) {
            call->Finish(CoroutineError(exception));
          }
          call->observer.reset();
        }));
  });
  return call->source;
}
}  // namespace a2a::server::engine
