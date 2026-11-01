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

constexpr auto kMissingStream = "Streaming dispatch requires a stream operation and sink";
constexpr auto kConcurrentWrite = "Only one stream write may be outstanding";

class CheckedStream final : public AsyncServerStreamSession {
 public:
  CheckedStream(std::shared_ptr<AsyncServerStreamSession> sink, std::shared_ptr<RequestExecution> source,
                std::shared_ptr<const DispatchSecurity> security, RequestContext context,
                const DispatchRequest& request)
      : sink_(std::move(sink)),
        source_(std::move(source)),
        security_(std::move(security)),
        context_(std::move(context)) {
    if (const auto* get = std::get_if<lf::a2a::v1::GetTaskRequest>(&request.payload)) {
      task_id_ = get->id();
    } else if (const auto* send = std::get_if<lf::a2a::v1::SendMessageRequest>(&request.payload)) {
      task_id_ = send->message().task_id();
    }
  }
  boost::asio::awaitable<core::Result<void>> Write(lf::a2a::v1::StreamResponse event) override {
    if (writing_.exchange(true)) {
      co_return Reject(core::Error::Validation(kConcurrentWrite));
    }
    struct Reservation final {
      std::atomic_bool& writing;
      ~Reservation() { writing.store(false); }
    } reservation{.writing = writing_};
    if (auto failed = Failure(); failed.has_value()) {
      co_return failed.value();
    }
    (void)source_->ExpireDeadline();
    if (source_->Context().Outcome() != RequestOutcome::kPending) {
      co_return Reject(CancellationError(source_->Context().Outcome()));
    }
    if (security_ != nullptr) {
      auto authorized = security_->AuthorizeStreamEvent(event, context_, task_id_);
      if (!authorized.ok()) {
        co_return Reject(authorized.error());
      }
    }
    auto result = co_await sink_->Write(std::move(event));
    co_return result.ok() ? result : Reject(result.error());
  }
  std::optional<core::Error> Failure() {
    std::lock_guard lock(mutex_);
    return failure_;
  }

 private:
  core::Result<void> Reject(const core::Error& error) {
    std::lock_guard lock(mutex_);
    if (failure_.has_value()) {
      return failure_.value();
    }
    failure_ = error;
    return error;
  }
  std::mutex mutex_;
  std::optional<core::Error> failure_;
  const std::shared_ptr<AsyncServerStreamSession> sink_;
  const std::shared_ptr<RequestExecution> source_;
  const std::shared_ptr<const DispatchSecurity> security_;
  const RequestContext context_;
  std::string task_id_;
  std::atomic_bool writing_ = false;
};

struct AdmissionCounts final {
  std::atomic_size_t active = 0;
  std::atomic_size_t pending = 0;
};

// Active admission ends at transport completion; pending admission belongs to
// the coroutine frame until cleanup. Construction failures release both slots.
class AdmissionReservation final : private core::NonCopyableOrMovable {
 public:
  explicit AdmissionReservation(std::shared_ptr<AdmissionCounts> counts) noexcept : counts_(std::move(counts)) {
    counts_->active.fetch_add(1);
    counts_->pending.fetch_add(1);
  }
  ~AdmissionReservation() {
    ReleaseActive();
    counts_->pending.fetch_sub(1);
  }
  void ReleaseActive() noexcept {
    if (active_) {
      active_ = false;
      counts_->active.fetch_sub(1);
    }
  }

 private:
  const std::shared_ptr<AdmissionCounts> counts_;
  bool active_ = true;
};
}  // namespace

struct AsyncDispatcher::State final {
  struct Call final : std::enable_shared_from_this<Call> {
    Call(const boost::asio::any_io_executor& scheduler, std::shared_ptr<AdmissionCounts> counts,
         DispatchRequest request, RequestContext metadata, TransportCompletion completion,
         std::optional<RequestExecution::Clock::time_point> deadline, DispatchPipeline pipeline,
         std::shared_ptr<AsyncServerStreamSession> stream)
        : strand(boost::asio::make_strand(scheduler)),
          admission(std::move(counts)),
          request(std::move(request)),
          context(metadata),
          source(std::make_shared<RequestExecution>(std::move(metadata), deadline)),
          completion(std::move(completion)),
          pipeline(std::move(pipeline)),
          session(this->pipeline, this->request, context),
          stream(std::move(stream)) {}

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
          const auto before = call->session.Before();
          (void)call->source->ExpireDeadline();
          call->ObserveShutdown(*state);
          if (!before.ok()) {
            response = before.error();
          } else if (call->source->Context().StopToken().stop_requested()) {
            response = CancellationError(call->source->Context().Outcome());
          } else if (auto handled = call->session.Handle()) {
            response = std::move(*handled);
          } else {
            auto view = call->source->Context().WithMetadata(call->context);
            const auto prepared = call->session.Prepare();
            if (call->stream != nullptr) {
              response = co_await call->RunStream(*state->executor, view);
            } else {
              response = co_await RouteDispatch(*state->executor, prepared.has_value() ? *prepared : call->request,
                                                view, CoroutineDispatchResult{});
            }
          }
        }
      } catch (...) {
        response = CoroutineError(std::current_exception());
      }
      call->Finish(std::move(response));
    }

    boost::asio::awaitable<core::Result<DispatchResponse>> RunStream(CoroutineAgentExecutor& application,
                                                                     RequestExecutionContext view) {
      auto checked = std::make_shared<CheckedStream>(stream, source, pipeline.security, context, request);
      core::Result<DispatchResponse> result = core::Error::Validation(kMissingStream);
      if (request.operation == DispatcherOperation::kSendStreamingMessage) {
        result = co_await RoutePayload<lf::a2a::v1::SendMessageRequest>(
            request, core::protocol_error_messages::kDispatchPayloadTypeMismatchForSendStreamingMessage.data(),
            CoroutineDispatchResult{}, [&](const auto& payload) {
              return CoroutineDispatchResult{}(application.SendStreamingMessage(payload, view, checked));
            });
      } else {
        result = co_await RoutePayload<lf::a2a::v1::GetTaskRequest>(
            request, core::protocol_error_messages::kDispatchPayloadTypeMismatchForGetTask.data(),
            CoroutineDispatchResult{}, [&](const auto& payload) {
              return CoroutineDispatchResult{}(application.SubscribeTask(payload, view, checked));
            });
      }
      if (auto failed = checked->Failure(); failed.has_value()) {
        result = failed.value();
      }
      co_return result;
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
          session.Project(response);
        }
      } catch (...) {
        response = CoroutineError(std::current_exception());
      }
      if (!source->TryComplete(response.ok() ? RequestCompletion::kSucceeded : RequestCompletion::kFailed)) {
        response = CancellationError(source->Context().Outcome());
      }
      session.After(response, InterceptorExceptionPolicy::kContain);
      if (const auto state = owner.lock()) {
        std::lock_guard lock(state->mutex);
        state->calls.erase(this);
      }
      admission.ReleaseActive();
      auto deliver = std::move(completion);
      try {
        deliver(std::move(response), source->Context());
      } catch (...) {
        return;
      }
    }

    boost::asio::strand<boost::asio::any_io_executor> strand;
    AdmissionReservation admission;
    DispatchRequest request;
    RequestContext context;
    std::shared_ptr<RequestExecution> source;
    TransportCompletion completion;
    DispatchPipeline pipeline;
    DispatchSession session;
    const std::shared_ptr<AsyncServerStreamSession> stream;
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
  const std::shared_ptr<AdmissionCounts> admission = std::make_shared<AdmissionCounts>();
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
  return DispatchImpl(std::move(request), std::move(metadata), std::move(completion), deadline, nullptr);
}

core::Result<std::shared_ptr<RequestExecution>> AsyncDispatcher::DispatchStream(
    DispatchRequest request, RequestContext metadata, std::shared_ptr<AsyncServerStreamSession> stream,
    TransportCompletion completion, std::optional<RequestExecution::Clock::time_point> deadline) {
  if (stream == nullptr || (request.operation != DispatcherOperation::kSendStreamingMessage &&
                            request.operation != DispatcherOperation::kSubscribeTask)) {
    return core::Error::Validation(kMissingStream);
  }
  return DispatchImpl(std::move(request), std::move(metadata), std::move(completion), deadline, std::move(stream));
}

core::Result<std::shared_ptr<RequestExecution>> AsyncDispatcher::DispatchImpl(
    DispatchRequest request, RequestContext metadata, TransportCompletion completion,
    std::optional<RequestExecution::Clock::time_point> deadline, std::shared_ptr<AsyncServerStreamSession> stream) {
  if (!completion) {
    return core::Error::Validation(kMissingCompletion);
  }
  const auto state = state_;
  std::lock_guard lock(state->mutex);
  if (state->closed) {
    return core::Error::Network(kAdmissionClosed);
  }
  if (state->admission->active.load() >= state->capacity) {
    return core::Error::Network(kAdmissionFull);
  }
  if (state->admission->pending.load() >= state->max_pending) {
    return core::Error::Network(kPendingFull);
  }
  auto call = std::make_shared<State::Call>(state->scheduler, state->admission, std::move(request), std::move(metadata),
                                            std::move(completion), deadline, state->pipeline, std::move(stream));
  call->owner = state;
  call->ObserveStop();
  state->calls.emplace(call.get(), call);
  try {
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
  } catch (...) {
    state->calls.erase(call.get());
    throw;
  }
  return call->source;
}
}  // namespace a2a::server::engine
