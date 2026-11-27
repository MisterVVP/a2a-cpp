// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "a2a/server/engine/async_dispatcher.h"

#include <atomic>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/strand.hpp>
#include <boost/system/system_error.hpp>
#include <deque>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <stop_token>
#include <unordered_map>
#include <utility>
#include <vector>

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

bool HasDispatchHooks(const DispatchPipeline& pipeline) noexcept {
  return pipeline.security != nullptr || !pipeline.interceptors.empty();
}

struct CoroutineDispatchResult final {
  using ReturnType = boost::asio::awaitable<core::Result<DispatchResponse>>;
  template <typename T>
  ReturnType operator()(boost::asio::awaitable<core::Result<T>> operation) const {
#if defined(__clang_analyzer__)
    (void)operation;
    co_return core::Error::Internal(kDispatchException);
#else
    auto result = co_await std::move(operation);
    co_return ToDispatchResponse(std::move(result));
#endif
  }
  ReturnType operator()(core::Result<DispatchResponse> result) const { co_return std::move(result); }
};

constexpr auto kMissingStream = "Streaming dispatch requires a stream operation and sink";
constexpr auto kConcurrentWrite = "Only one stream write may be outstanding";

struct AdmissionCounts final {
  std::atomic_size_t active = 0;
  std::atomic_size_t pending = 0;
};

class CheckedStream final : public AsyncServerStreamSession {
 public:
  CheckedStream(std::shared_ptr<AsyncServerStreamSession> sink, std::shared_ptr<RequestExecution> source,
                std::shared_ptr<const DispatchSecurity> security, std::shared_ptr<const AdmissionCounts> admission,
                const RequestContext& context, const DispatchRequest& request)
      : sink_(std::move(sink)),
        source_(std::move(source)),
        security_(std::move(security)),
        admission_(std::move(admission)),
        context_(security_ != nullptr ? context : RequestContext{}) {
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
    const Reservation reservation{.writing = writing_};
    if (auto ready = CheckEvent(event); !ready.ok()) {
      co_return ready;
    }
    auto result = co_await sink_->Write(std::move(event));
    co_return MarkWrite(std::move(result));
  }
  boost::asio::awaitable<core::Result<void>> WriteBatch(std::vector<lf::a2a::v1::StreamResponse> events) override {
    if (events.empty()) {
      co_return core::Result<void>{};
    }
    if (events.size() > kMaxBatchEvents) {
      co_return Reject(core::Error::Validation(kBatchTooLarge));
    }
    if (writing_.exchange(true)) {
      co_return Reject(core::Error::Validation(kConcurrentWrite));
    }
    const Reservation reservation{.writing = writing_};
    for (auto event = events.begin(); event != events.end(); ++event) {
      if (auto ready = CheckEvent(*event); !ready.ok()) {
        co_return ready;
      }
      if (wrote_event_ && security_ == nullptr && events.end() - event > 1 &&
          admission_->active.load(std::memory_order_relaxed) == 1) {
        events.erase(events.begin(), event);
        co_return MarkWrite(co_await sink_->WriteBatch(std::move(events)));
      }
      auto result = MarkWrite(co_await sink_->Write(std::move(*event)));
      if (!result.ok()) {
        co_return result;
      }
    }
    co_return core::Result<void>{};
  }
  std::optional<core::Error> Failure() {
    std::lock_guard lock(mutex_);
    return failure_;
  }

 private:
  struct Reservation final {
    std::atomic_bool& writing;
    ~Reservation() { writing.store(false); }
  };

  core::Result<void> MarkWrite(core::Result<void> result) {
    if (!result.ok()) {
      return Reject(result.error());
    }
    wrote_event_ = true;
    return result;
  }

  core::Result<void> CheckWrite() {
    if (auto failed = Failure(); failed.has_value()) {
      return failed.value();
    }
    const auto view = source_->Context();
    if (view.Deadline().has_value()) {
      (void)source_->ExpireDeadline();
    }
    const auto outcome = view.Outcome();
    return outcome == RequestOutcome::kPending ? core::Result<void>{} : Reject(CancellationError(outcome));
  }

  core::Result<void> CheckEvent(const lf::a2a::v1::StreamResponse& event) {
    auto ready = CheckWrite();
    if (!ready.ok() || security_ == nullptr) {
      return ready;
    }
    auto authorized = security_->AuthorizeStreamEvent(event, context_, task_id_);
    return authorized.ok() ? authorized : Reject(authorized.error());
  }

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
  const std::shared_ptr<const AdmissionCounts> admission_;
  const RequestContext context_;
  std::string task_id_;
  std::atomic_bool writing_ = false;
  bool wrote_event_ = false;
};

struct RollbackAdmission final {
  void operator()(AdmissionCounts* counts) const noexcept {
    counts->active.fetch_sub(1);
    counts->pending.fetch_sub(1);
  }
};

using PendingAdmission = std::unique_ptr<AdmissionCounts, RollbackAdmission>;

class AdmissionReservation final : private core::NonCopyableOrMovable {
 public:
  AdmissionReservation(std::shared_ptr<AdmissionCounts> counts, PendingAdmission& pending) noexcept
      : counts_(std::move(counts), pending.release()) {}

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
  [[nodiscard]] const std::shared_ptr<AdmissionCounts>& counts() const noexcept { return counts_; }

 private:
  const std::shared_ptr<AdmissionCounts> counts_;
  bool active_ = true;
};
}  // namespace

struct AsyncDispatcher::State final {
  struct Call;
  struct Lane final {
    explicit Lane(boost::asio::any_io_executor worker) : worker(std::move(worker)) {}
    boost::asio::any_io_executor worker;
    std::mutex mutex;
    std::unordered_map<Call*, std::shared_ptr<Call>> calls;
  };
  struct Call final {
    struct NotifyStop final {
      std::weak_ptr<Call> call;
      void operator()() const {
        if (const auto locked = call.lock()) {
          boost::asio::post(locked->worker, [locked] { locked->Stopped(); });
        }
      }
    };

    Call(boost::asio::any_io_executor worker, Lane& lane, const std::shared_ptr<AdmissionCounts>& counts,
         PendingAdmission& pending, DispatchRequest request, RequestContext metadata, TransportCompletion completion,
         std::optional<RequestExecution::Clock::time_point> deadline, std::shared_ptr<const DispatchPipeline> pipeline,
         std::shared_ptr<AsyncServerStreamSession> stream)
        : worker(std::move(worker)),
          lane(lane),
          admission(counts, pending),
          request(std::move(request)),
          context(HasDispatchHooks(*pipeline) || this->request.operation == DispatcherOperation::kGetExtendedAgentCard
                      ? metadata
                      : RequestContext{}),
          source(std::make_shared<RequestExecution>(std::move(metadata), deadline)),
          completion(std::move(completion)),
          pipeline(std::move(pipeline)),
          session(*this->pipeline, this->request, context),
          stream(std::move(stream)) {}

    void Stopped() {
      if (finished) {
        return;
      }
      cancellation.emit(boost::asio::cancellation_type::terminal);
      if (started) {
        Finish(CancellationError(source->Context().Outcome()));
      }
    }

    static boost::asio::awaitable<void> Run(std::shared_ptr<State> state, std::shared_ptr<Call> call) {
      call->started = true;
      auto view = call->source->Context();
      core::Result<DispatchResponse> response = core::Error::Internal({});
      try {
        const auto valid = state->extensions.Validate(view.Metadata().client_headers);
        if (!valid.ok()) {
          response = valid.error();
        } else {
          const auto before = call->session.Before();
          if (view.Deadline().has_value()) {
            (void)call->source->ExpireDeadline();
          }
          call->ObserveShutdown(state->closed.load());
          if (!before.ok()) {
            response = before.error();
          } else if (const auto outcome = view.Outcome(); outcome != RequestOutcome::kPending) {
            response = CancellationError(outcome);
          } else if (auto handled = call->session.Handle()) {
            response = std::move(*handled);
          } else {
            if (HasDispatchHooks(*call->pipeline)) {
              view = view.WithMetadata(call->context);
            }
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
      auto checked =
          std::make_shared<CheckedStream>(stream, source, pipeline->security, admission.counts(), context, request);
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

    void ObserveShutdown(bool closed) {
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
        if (source->Context().Outcome() == RequestOutcome::kPending) {
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
        std::lock_guard lock(lane.mutex);
        lane.calls.erase(this);
      }
      admission.ReleaseActive();
      auto deliver = std::move(completion);
      try {
        deliver(std::move(response), source->Context());
      } catch (...) {
        return;
      }
    }

    const boost::asio::any_io_executor worker;
    Lane& lane;
    AdmissionReservation admission;
    DispatchRequest request;
    RequestContext context;
    std::shared_ptr<RequestExecution> source;
    TransportCompletion completion;
    const std::shared_ptr<const DispatchPipeline> pipeline;
    DispatchSession session;
    const std::shared_ptr<AsyncServerStreamSession> stream;
    std::weak_ptr<State> owner;
    boost::asio::cancellation_signal cancellation;
    std::optional<std::stop_callback<NotifyStop>> observer;
    bool started = false;
    bool finished = false;
  };

  State(std::shared_ptr<CoroutineAgentExecutor> application, boost::asio::any_io_executor scheduler,
        AsyncDispatcherOptions options)
      : executor(std::move(application)),
        capacity(options.max_in_flight),
        max_pending(options.max_pending),
        pipeline(std::make_shared<const DispatchPipeline>(DispatchPipeline{
            .security = options.security_policy != nullptr
                            ? std::make_shared<DispatchSecurity>(std::move(options.security_policy), options.task_store)
                            : nullptr,
            .interceptors = std::move(options.interceptors),
            .agent_card_provider = std::move(options.agent_card_provider)})),
        extensions(std::move(options.required_extensions)) {
    lanes.emplace_back(std::move(scheduler));
  }

  core::Result<PendingAdmission> Reserve() {
    std::lock_guard lock(admission_mutex);
    if (closed.load()) {
      return core::Error::Network(kAdmissionClosed);
    }
    if (admission->active.load() >= capacity) {
      return core::Error::Network(kAdmissionFull);
    }
    if (admission->pending.load() >= max_pending) {
      return core::Error::Network(kPendingFull);
    }
    admission->active.fetch_add(1);
    admission->pending.fetch_add(1);
    return PendingAdmission(admission.get());
  }

  static void Start(const std::shared_ptr<Call>& call, const std::shared_ptr<State>& state) {
    boost::asio::co_spawn(
        call->worker, Call::Run(state, call),
        boost::asio::bind_cancellation_slot(call->cancellation.slot(), [call](const std::exception_ptr& exception) {
          if (exception) {
            call->Finish(CoroutineError(exception));
          }
          call->observer.reset();
        }));
    if (call->finished) {
      return;
    }
    call->owner = state;
    call->observer.emplace(call->source->Context().StopToken(), Call::NotifyStop{.call = call});
    {
      std::lock_guard lock(call->lane.mutex);
      if (!state->closed.load()) {
        call->lane.calls.emplace(call.get(), call);
        return;
      }
    }
    call->ObserveShutdown(true);
  }

  void Shutdown() {
    std::vector<std::shared_ptr<Call>> active;
    if (closed.exchange(true)) {
      return;
    }
    for (auto& lane : lanes) {
      std::lock_guard lock(lane.mutex);
      for (const auto& [key, call] : lane.calls) {
        active.push_back(call);
      }
    }
    for (const auto& call : active) {
      boost::asio::post(call->worker,
                        [call] { (void)call->source->Cancel(RequestCancellationReason::kServerShutdown); });
    }
  }

  const std::shared_ptr<CoroutineAgentExecutor> executor;
  std::deque<Lane> lanes;
  std::atomic_size_t next_lane = 0;
  const std::size_t capacity;
  const std::size_t max_pending;
  const std::shared_ptr<AdmissionCounts> admission = std::make_shared<AdmissionCounts>();
  const std::shared_ptr<const DispatchPipeline> pipeline;
  RequiredExtensionsValidator extensions;
  std::mutex admission_mutex;
  std::atomic_bool closed = false;
  bool worker_affine = false;
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

void AsyncDispatcher::EnableWorkerLanes(std::vector<boost::asio::any_io_executor> workers) {
  state_->lanes.clear();
  for (auto& worker : workers) {
    state_->lanes.emplace_back(std::move(worker));
  }
  state_->worker_affine = true;
}

std::pair<boost::asio::any_io_executor, std::optional<std::size_t>> AsyncDispatcher::NativeHttpLane() const {
  if (!state_->worker_affine) {
    return {};
  }
  const auto index = state_->next_lane.fetch_add(1, std::memory_order_relaxed) % state_->lanes.size();
  return {state_->lanes[index].worker, index};
}

void AsyncDispatcher::Cancel(std::shared_ptr<RequestExecution> execution, RequestCancellationReason reason) {
  if (execution != nullptr) {
    const auto& worker = state_->lanes[execution->Lane() % state_->lanes.size()].worker;
    boost::asio::post(worker, [execution = std::move(execution), reason] { (void)execution->Cancel(reason); });
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
    std::optional<RequestExecution::Clock::time_point> deadline, std::shared_ptr<AsyncServerStreamSession> stream,
    std::optional<std::size_t> ingress_lane,
    const std::function<void(const std::shared_ptr<RequestExecution>&)>& admitted) {
  if (!completion) {
    return core::Error::Validation(kMissingCompletion);
  }
  const auto state = state_;
  auto index = ingress_lane.value_or(0);
  if (!ingress_lane && state->worker_affine) {
    index = state->next_lane.fetch_add(1, std::memory_order_relaxed) % state->lanes.size();
  }
  auto& lane = state->lanes[index];
  const auto submission_work = boost::asio::make_work_guard(lane.worker);
  auto reserved = state->Reserve();
  if (!reserved.ok()) {
    return reserved.error();
  }
  auto worker =
      state->worker_affine ? lane.worker : boost::asio::any_io_executor(boost::asio::make_strand(lane.worker));
  auto call = std::make_shared<State::Call>(std::move(worker), lane, state->admission, reserved.value(),
                                            std::move(request), std::move(metadata), std::move(completion), deadline,
                                            state->pipeline, std::move(stream));
  call->source->SetLane(index);
  if (admitted) {
    admitted(call->source);
  }
  auto start = [state, call] { State::Start(call, state); };
  if (ingress_lane) {
    start();
  } else {
    boost::asio::post(call->worker, std::move(start));
  }
  return call->source;
}
}  // namespace a2a::server::engine
