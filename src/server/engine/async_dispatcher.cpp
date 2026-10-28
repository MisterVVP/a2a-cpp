// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "a2a/server/engine/async_dispatcher.h"

#include <condition_variable>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <stop_token>
#include <thread>
#include <unordered_map>
#include <utility>

#include "a2a/server/required_extensions_validator.h"
#include "server/engine/dispatch_pipeline.h"

namespace a2a::server::engine {
namespace {
constexpr auto kInvalidOptions = "Async dispatcher requires an executor, workers, and nonzero capacity";
constexpr auto kAdmissionClosed = "Async dispatcher is shut down";
constexpr auto kAdmissionFull = "Async dispatcher capacity exceeded";
constexpr auto kMissingCompletion = "Async dispatcher completion is required";
constexpr auto kClientCancelled = "Request cancelled by client";
constexpr auto kDeadlineExceeded = "Request deadline exceeded";
constexpr auto kServerShutdown = "Request cancelled by server shutdown";

// Queue ownership is separate from thread ownership so destroying the dispatcher
// in its own completion can detach that worker and safely drain its remaining work.
struct WorkQueue final {
  void Post(std::function<void()> work) {
    {
      std::lock_guard lock(mutex);
      pending.push_back(std::move(work));
    }
    ready.notify_one();
  }
  void Run() {
    for (;;) {
      std::function<void()> work;
      {
        std::unique_lock lock(mutex);
        ready.wait(lock, [&] { return stopping || !pending.empty(); });
        if (pending.empty()) {
          return;
        }
        work = std::move(pending.front());
        pending.pop_front();
      }
      work();
    }
  }
  std::mutex mutex;
  std::condition_variable ready;
  std::deque<std::function<void()>> pending;
  bool stopping = false;
};

class Workers final : private core::NonCopyableOrMovable {
 public:
  explicit Workers(std::size_t count) : queue(std::make_shared<WorkQueue>()) {
    threads_.reserve(count);
    try {
      for (std::size_t index = 0; index < count; ++index) {
        threads_.emplace_back([work = queue] { work->Run(); });
      }
    } catch (...) {
      Stop();
      throw;
    }
  }
  ~Workers() { Stop(); }
  void Drain() { Stop(); }
  const std::shared_ptr<WorkQueue> queue;

 private:
  void Stop() {
    {
      std::lock_guard lock(queue->mutex);
      queue->stopping = true;
    }
    queue->ready.notify_all();
    for (auto& thread : threads_) {
      if (!thread.joinable()) {
        continue;
      }
      if (thread.get_id() == std::this_thread::get_id()) {
        thread.detach();
      } else {
        thread.join();
      }
    }
  }
  std::vector<std::thread> threads_;
};

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

}  // namespace

struct AsyncDispatcher::State final {
  struct Call final : std::enable_shared_from_this<Call> {
    void ScheduleFinish() {
      std::lock_guard lock(mutex);
      if (!started || finish_queued || (!result.has_value() && !source->Context().StopToken().stop_requested())) {
        return;
      }
      finish_queued = true;
      queue->Post([self = shared_from_this()] { self->Finish(); });
    }
    void Submit(core::Result<DispatchResponse> response) {
      {
        std::lock_guard lock(mutex);
        if (result.has_value() || finish_queued) {
          return;
        }
        result = std::move(response);
      }
      ScheduleFinish();
    }
    void Execute() {
      const auto before = pipeline.Before(request, context, principal);
      if (!before.ok()) {
        Submit(before.error());
        return;
      }
      (void)source->ExpireDeadline();
      if (source->Context().Outcome() != RequestOutcome::kPending) {
        return;
      }
      if (auto handled = pipeline.Handle(request, context)) {
        Submit(std::move(*handled));
        return;
      }
      auto view = source->Context().WithMetadata(context);
      const auto prepared = pipeline.Prepare(request);
      const auto weak = weak_from_this();
      RouteDispatch(*executor, prepared.has_value() ? *prepared : request, view, [weak](auto response) {
        if (const auto call = weak.lock()) {
          call->Submit(std::move(response));
        }
      });
    }
    void Start() {
      try {
        Execute();
      } catch (...) {
        Submit(core::Error::Internal(kDispatchCallbackException));
      }
      executor.reset();
      {
        std::lock_guard lock(mutex);
        started = true;
      }
      ScheduleFinish();
    }
    void Finish() {
      core::Result<DispatchResponse> response = CancellationError(source->Context().Outcome());
      {
        std::lock_guard lock(mutex);
        if (result.has_value()) {
          response = std::move(*result);
        }
      }
      try {
        if (!source->Context().StopToken().stop_requested()) {
          pipeline.Project(request, context, principal, response);
        }
      } catch (...) {
        response = core::Error::Internal(kDispatchCallbackException);
      }
      const auto completed =
          source->TryComplete(response.ok() ? RequestCompletion::kSucceeded : RequestCompletion::kFailed);
      if (!completed) {
        response = CancellationError(source->Context().Outcome());
      }
      pipeline.After(request, context, principal, response, /*contain_exceptions=*/true);
      // Release admission before invoking user completion, allowing reentrant dispatch.
      if (const auto state = owner.lock()) {
        std::lock_guard lock(state->mutex);
        state->calls.erase(this);
      }
      try {
        completion(std::move(response), source->Context());
      } catch (...) {
        // The result is already terminal; preserve the worker for other requests.
        return;
      }
    }
    DispatchRequest request;
    RequestContext context;
    DispatchPipeline pipeline;
    std::optional<std::string> principal;
    std::shared_ptr<RequestExecution> source;
    std::shared_ptr<AsyncAgentExecutor> executor;
    Completion completion;
    std::shared_ptr<WorkQueue> queue;
    std::weak_ptr<State> owner;
    std::unique_ptr<std::stop_callback<std::function<void()>>> stop_callback;
    std::mutex mutex;
    std::optional<core::Result<DispatchResponse>> result;
    bool started = false;
    bool finish_queued = false;
  };

  State(std::shared_ptr<AsyncAgentExecutor> application, AsyncDispatcherOptions options)
      : executor(std::move(application)),
        capacity(options.max_in_flight),
        pipeline{
            .security = options.security_policy != nullptr
                            ? std::make_shared<DispatchSecurity>(std::move(options.security_policy), options.task_store)
                            : nullptr,
            .interceptors = std::move(options.interceptors),
            .agent_card_provider = std::move(options.agent_card_provider)},
        extensions(std::move(options.required_extensions)),
        workers(options.worker_count) {}

  void Shutdown() {
    std::vector<std::shared_ptr<Call>> pending;
    {
      std::lock_guard lock(mutex);
      if (closed) {
        return;
      }
      closed = true;
      pending.reserve(calls.size());
      for (const auto& [key, call] : calls) {
        pending.push_back(call);
      }
    }
    for (const auto& call : pending) {
      (void)call->source->Cancel(RequestCancellationReason::kServerShutdown);
      // A cancellation observer can reenter shutdown before our stop callback
      // has run. Queue finalization even when another terminal signal won.
      call->ScheduleFinish();
    }
  }
  std::shared_ptr<AsyncAgentExecutor> executor;
  const std::size_t capacity;
  // Workers are declared last so they drain before other state is destroyed.
  std::mutex mutex;
  std::unordered_map<Call*, std::shared_ptr<Call>> calls;
  DispatchPipeline pipeline;
  RequiredExtensionsValidator extensions;
  bool closed = false;
  Workers workers;
};

AsyncDispatcher::AsyncDispatcher(std::shared_ptr<AsyncAgentExecutor> executor, AsyncDispatcherOptions options) {
  if (executor == nullptr || options.worker_count == 0 || options.max_in_flight == 0) {
    throw std::invalid_argument(kInvalidOptions);
  }
  state_ = std::make_shared<State>(std::move(executor), std::move(options));
}
AsyncDispatcher::~AsyncDispatcher() {
  Shutdown();
  state_->workers.Drain();
}
void AsyncDispatcher::Shutdown() { state_->Shutdown(); }

core::Result<std::shared_ptr<RequestExecution>> AsyncDispatcher::Dispatch(
    DispatchRequest request, RequestContext metadata, Completion completion,
    std::optional<RequestExecution::Clock::time_point> deadline) {
  if (!completion) {
    return core::Error::Validation(kMissingCompletion);
  }
  const auto state = state_;
  auto call = std::make_shared<State::Call>();
  call->request = std::move(request);
  call->context = metadata;
  call->source = std::make_shared<RequestExecution>(std::move(metadata), deadline);
  call->pipeline = state->pipeline;
  call->executor = state->executor;
  call->completion = std::move(completion);
  call->owner = state;
  call->queue = state->workers.queue;
  const auto weak = std::weak_ptr(call);
  call->stop_callback =
      std::make_unique<std::stop_callback<std::function<void()>>>(call->source->Context().StopToken(), [weak] {
        if (const auto active = weak.lock()) {
          active->ScheduleFinish();
        }
      });
  {
    std::lock_guard lock(state->mutex);
    if (state->closed) {
      return core::Error::Network(kAdmissionClosed);
    }
    if (state->calls.size() >= state->capacity) {
      return core::Error::Network(kAdmissionFull);
    }
    state->calls.emplace(call.get(), call);
    call->queue->Post([call, extensions = state->extensions] {
      const auto valid = extensions.Validate(call->context.client_headers);
      if (!valid.ok()) {
        call->Submit(valid.error());
      }
      // Extension rejection precedes application hooks, matching transport validation.
      if (valid.ok()) {
        call->Start();
      } else {
        {
          std::lock_guard lock(call->mutex);
          call->started = true;
        }
        call->ScheduleFinish();
      }
    });
  }
  return call->source;
}
}  // namespace a2a::server::engine
