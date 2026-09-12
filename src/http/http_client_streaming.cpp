// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "a2a/http/http_client.h"

#if defined(A2A_HAS_LIBCURL)

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "a2a/core/error.h"
#include "http_client_internal.h"

namespace a2a::http {
namespace {

constexpr char kHttpTransportName[] = "http";
constexpr std::string_view kStreamCompletionPendingMessage = "HTTP stream completion is pending";
constexpr std::string_view kCurlInitFailureMessage = "failed to initialize HTTP client";
constexpr std::string_view kCurlMultiInitFailureMessage = "failed to initialize HTTP stream poller";
constexpr std::string_view kErrorBufferFailureMessage = "failed to configure HTTP client error buffer";
constexpr std::string_view kConfigureRequestFailureMessage = "failed to configure HTTP request";
constexpr std::string_view kRequestFailureMessage = "failed to execute HTTP request";
constexpr std::string_view kReadStatusFailureMessage = "failed to read HTTP response status";
constexpr std::string_view kMalformedStatusMessage = "HTTP server did not return a response status";
constexpr long kHttpResponseCodeUnset = 0;
constexpr std::string_view kStreamCancelledMessage = "HTTP stream was cancelled";
constexpr std::string_view kAsyncStreamCallbacksRequiredMessage = "HTTP asynchronous stream callbacks are required";
constexpr std::string_view kClientShuttingDownMessage = "HTTP client is shutting down";
constexpr std::size_t kMaximumPendingDispatchTasks = 256U;
constexpr std::size_t kMaximumPendingDispatchBytes = std::size_t{4U} * 1024U * 1024U;
constexpr std::size_t kStreamDispatchWorkerCount = 4U;
constexpr std::string_view kDispatchBacklogExceededMessage = "HTTP stream callback backlog limit exceeded";
constexpr std::string_view kDispatchInitializationFailureMessage = "failed to initialize HTTP stream callback executor";
constexpr std::string_view kSynchronousStreamFromCallbackMessage =
    "synchronous HTTP streaming cannot run from a stream callback";
constexpr std::chrono::milliseconds kSynchronousCancellationCheckInterval{1};

thread_local detail::ClientState* g_dispatch_client_state = nullptr;

class DispatchCallbackScope final {
 public:
  explicit DispatchCallbackScope(detail::ClientState* state) : previous_(g_dispatch_client_state) {
    g_dispatch_client_state = state;
  }
  ~DispatchCallbackScope() { g_dispatch_client_state = previous_; }

 private:
  detail::ClientState* previous_;
};

void StopCancellationWatcher(std::atomic<bool>& stop_requested, std::thread& watcher) {
  stop_requested.store(true);
  if (watcher.joinable()) {
    watcher.join();
  }
}

class StreamDispatchExecutor final {
 public:
  using Task = std::function<void()>;
  using WorkerFactory = std::function<std::thread(Task)>;

  StreamDispatchExecutor() : StreamDispatchExecutor([](Task task) { return std::thread(std::move(task)); }) {}

  explicit StreamDispatchExecutor(const WorkerFactory& worker_factory) {
    workers_.reserve(kStreamDispatchWorkerCount);
    try {
      for (std::size_t index = 0; index < kStreamDispatchWorkerCount; ++index) {
        workers_.emplace_back(worker_factory([this] { Run(); }));
      }
    } catch (...) {
      StopAndJoinWorkers();
      throw;
    }
  }

  ~StreamDispatchExecutor() { StopAndJoinWorkers(); }

  void Submit(Task task) {
    {
      std::lock_guard lock(mutex_);
      tasks_.push_back(std::move(task));
    }
    available_.notify_one();
  }

 private:
  void StopAndJoinWorkers() {
    {
      std::lock_guard lock(mutex_);
      stopping_ = true;
    }
    available_.notify_all();
    for (auto& worker : workers_) {
      if (worker.joinable()) {
        worker.join();
      }
    }
  }

  void Run() {
    while (true) {
      Task task;
      {
        std::unique_lock lock(mutex_);
        available_.wait(lock, [this] { return stopping_ || !tasks_.empty(); });
        if (stopping_ && tasks_.empty()) {
          return;
        }
        task = std::move(tasks_.front());
        tasks_.pop_front();
      }
      task();
    }
  }

  std::mutex mutex_;
  std::condition_variable available_;
  std::deque<Task> tasks_;
  std::vector<std::thread> workers_;
  bool stopping_ = false;
};

struct HttpProcessState final {
  HttpProcessState() : global_state(std::make_shared<detail::ClientGlobalState>()) {}
  ~HttpProcessState() {
    // Drop the process-global curl owner while callback workers can still
    // accept terminal work emitted by reactor teardown.
    global_state.reset();
    stream_dispatch_executor.reset();
  }

  HttpProcessState(const HttpProcessState&) = delete;
  HttpProcessState& operator=(const HttpProcessState&) = delete;
  HttpProcessState(HttpProcessState&&) = delete;
  HttpProcessState& operator=(HttpProcessState&&) = delete;

  std::unique_ptr<StreamDispatchExecutor> stream_dispatch_executor;
  std::shared_ptr<const detail::ClientGlobalState> global_state;
};

HttpProcessState& GetHttpProcessState() {
  static HttpProcessState state;
  return state;
}

void DeferUnusedReactorRelease(std::shared_ptr<detail::CurlStreamReactorPool> reactor_pool) noexcept {
  try {
    auto* const executor = GetHttpProcessState().stream_dispatch_executor.get();
    if (executor == nullptr) {
      return;
    }
    executor->Submit([reactor_pool = std::move(reactor_pool)] { (void)reactor_pool->ReleaseUnused(); });
  } catch (...) {
    // The process-global pool remains an owner and releases the shard during
    // process teardown if deferred cleanup cannot be queued.
    return;
  }
}

std::shared_ptr<const detail::ClientGlobalState> GetCurlGlobalState() { return GetHttpProcessState().global_state; }

StreamDispatchExecutor& GetStreamDispatchExecutor() {
  static StreamDispatchExecutor& executor = []() -> StreamDispatchExecutor& {
    auto& process_state = GetHttpProcessState();
    process_state.stream_dispatch_executor = std::make_unique<StreamDispatchExecutor>();
    return *process_state.stream_dispatch_executor;
  }();
  return executor;
}

core::Result<void> EnsureStreamDispatchExecutor() {
  try {
    (void)GetStreamDispatchExecutor();
  } catch (const std::system_error&) {
    return core::Error::Internal(std::string(kDispatchInitializationFailureMessage)).WithTransport(kHttpTransportName);
  }
  return {};
}

class SerialStreamDispatch final : public std::enable_shared_from_this<SerialStreamDispatch> {
 public:
  using Task = std::function<void()>;

  bool Enqueue(Task task, std::size_t pending_bytes = 0U, bool terminal = false) {
    bool schedule = false;
    {
      std::lock_guard lock(mutex_);
      const std::size_t bounded_pending_bytes =
          pending_bytes < kMaximumPendingDispatchBytes ? pending_bytes : kMaximumPendingDispatchBytes;
      if (!terminal && (tasks_.size() >= kMaximumPendingDispatchTasks ||
                        pending_bytes_ > kMaximumPendingDispatchBytes - bounded_pending_bytes)) {
        return false;
      }
      tasks_.push_back(QueuedTask{.task = std::move(task), .pending_bytes = pending_bytes});
      pending_bytes_ += pending_bytes;
      schedule = !scheduled_;
      scheduled_ = true;
    }
    if (schedule) {
      GetStreamDispatchExecutor().Submit([self = shared_from_this()] { self->Drain(); });
    }
    return true;
  }

 private:
  void Drain() {
    while (true) {
      QueuedTask queued;
      {
        std::lock_guard lock(mutex_);
        if (tasks_.empty()) {
          scheduled_ = false;
          return;
        }
        queued = std::move(tasks_.front());
        tasks_.pop_front();
        pending_bytes_ -= queued.pending_bytes;
      }
      queued.task();
    }
  }

  struct QueuedTask final {
    Task task;
    std::size_t pending_bytes = 0U;
  };

  std::mutex mutex_;
  std::deque<QueuedTask> tasks_;
  std::size_t pending_bytes_ = 0U;
  bool scheduled_ = false;
};

struct AsyncStreamState final : public std::enable_shared_from_this<AsyncStreamState> {
  std::shared_ptr<detail::ClientState> client_state;
  std::shared_ptr<detail::CurlStreamReactor> reactor;
  std::shared_ptr<detail::CurlStreamReactor::Transfer> transfer;
  std::unique_ptr<detail::StreamSlot> slot;
  Request request;
  detail::CurlHeaderList headers;
  std::array<char, CURL_ERROR_SIZE> error_buffer{};
  std::vector<Header> response_headers;
  detail::HeaderCapture header_capture;
  detail::StreamCallbackContext callback_context;
  detail::StreamHeaderContext stream_header_context;
  std::function<core::Result<void>(const Response&)> metadata_handler;
  std::function<core::Result<void>(std::string_view)> chunk_handler;
  std::function<bool()> is_cancelled;
  Client::StreamCompletion completion;
  std::shared_ptr<SerialStreamDispatch> dispatch = std::make_shared<SerialStreamDispatch>();
  std::mutex error_mutex;
  std::optional<core::Error> dispatch_error;
  bool registered = false;
  std::atomic<bool> finalized{false};
  std::atomic<bool> suppress_callbacks{false};

  void Finalize() {
    if (!registered || finalized.exchange(true)) {
      return;
    }
    {
      std::lock_guard lock(client_state->stream_mutex);
      --client_state->active_streams;
    }
    client_state->streams_finished.notify_all();
  }

  void RecordError(const core::Error& error) {
    {
      std::lock_guard lock(error_mutex);
      if (dispatch_error.has_value()) {
        return;
      }
      dispatch_error = error;
    }
    suppress_callbacks.store(true);
    reactor->Cancel(transfer);
  }

  void QueueMetadata(Response response) {
    if (!dispatch->Enqueue([self = shared_from_this(), response = std::move(response)] {
          DispatchCallbackScope callback_scope(self->client_state.get());
          if (self->client_state->suppress_stream_callbacks.load() || self->suppress_callbacks.load()) {
            return;
          }
          const auto handled = self->metadata_handler(response);
          if (!handled.ok()) {
            self->RecordError(handled.error());
          }
        })) {
      RecordError(core::Error::Network(std::string(kDispatchBacklogExceededMessage)).WithTransport(kHttpTransportName));
    }
  }

  void QueueChunk(std::string chunk) {
    const std::size_t chunk_size = chunk.size();
    if (!dispatch->Enqueue(
            [self = shared_from_this(), chunk = std::move(chunk)] {
              DispatchCallbackScope callback_scope(self->client_state.get());
              if (self->client_state->suppress_stream_callbacks.load() || self->suppress_callbacks.load()) {
                return;
              }
              const auto handled = self->chunk_handler(chunk);
              if (!handled.ok()) {
                self->RecordError(handled.error());
              }
            },
            chunk_size)) {
      RecordError(core::Error::Network(std::string(kDispatchBacklogExceededMessage)).WithTransport(kHttpTransportName));
    }
  }

  void Finish(CURLcode code) {
    Finalize();
    (void)dispatch->Enqueue(
        [self = shared_from_this(), code] {
          DispatchCallbackScope callback_scope(self->client_state.get());
          core::Result<Response> result = self->BuildResult(code);
          detail::ReleaseStreamSlot(*self->client_state, std::move(self->slot));
          self->completion(std::move(result));
          self->transfer->lifetime.reset();
        },
        0U, true);
  }

  core::Result<Response> BuildResult(CURLcode code) {
    {
      std::lock_guard lock(error_mutex);
      if (dispatch_error.has_value()) {
        return dispatch_error.value();
      }
    }
    if (code != CURLE_OK) {
      if (is_cancelled()) {
        return core::Error::Network(std::string(kStreamCancelledMessage)).WithTransport(kHttpTransportName);
      }
      return core::Error::Network(detail::BuildCurlErrorMessage(kRequestFailureMessage, code, error_buffer.data()));
    }
    if (!callback_context.metadata_checked) {
      const auto metadata = detail::ValidateStreamMetadata(&callback_context);
      if (!metadata.ok()) {
        return metadata.error();
      }
    }
    long response_code = kHttpResponseCodeUnset;
    const CURLcode info_code = curl_easy_getinfo(slot->easy_handle, CURLINFO_RESPONSE_CODE, &response_code);
    if (info_code != CURLE_OK) {
      return core::Error::RemoteProtocol(detail::BuildCurlErrorMessage(kReadStatusFailureMessage, info_code, {}));
    }
    if (response_code == kHttpResponseCodeUnset) {
      return core::Error::RemoteProtocol(std::string(kMalformedStatusMessage));
    }
    return Response{.status_code = static_cast<int>(response_code), .headers = response_headers, .body = {}};
  }
};

}  // namespace

namespace detail {

std::shared_ptr<const ClientGlobalState> EnsureCurlGlobalInit() { return GetCurlGlobalState(); }

bool IsDispatchingStreamCallback(const ClientState* state) noexcept {
  return g_dispatch_client_state != nullptr && (state == nullptr || g_dispatch_client_state == state);
}

ClientState::~ClientState() {
  idle_request_slots.clear();
  idle_stream_slots.clear();
  if (global_state == nullptr || global_state->stream_reactor_pool == nullptr) {
    return;
  }
  auto reactor_pool = global_state->stream_reactor_pool;
  if (!reactor_pool->ReleaseUnused()) {
    DeferUnusedReactorRelease(std::move(reactor_pool));
  }
}

}  // namespace detail

core::Result<Response> Client::StreamRequest(const Request& request,
                                             const std::function<core::Result<void>(const Response&)>& on_metadata,
                                             const std::function<core::Result<void>(std::string_view)>& on_chunk,
                                             const std::function<bool()>& is_cancelled) const {
  return StreamRequest(request, on_metadata, on_chunk, is_cancelled, {});
}

core::Result<Response> Client::StreamRequest(
    const Request& request, const std::function<core::Result<void>(const Response&)>& on_metadata,
    const std::function<core::Result<void>(std::string_view)>& on_chunk, const std::function<bool()>& is_cancelled,
    const std::function<void(const std::function<void()>&)>& register_cancellation) const {
  if (detail::IsDispatchingStreamCallback(nullptr)) {
    return core::Error::Validation(std::string(kSynchronousStreamFromCallbackMessage))
        .WithTransport(kHttpTransportName);
  }
  std::mutex completion_mutex;
  std::condition_variable completion_condition;
  core::Result<Response> response = core::Error::Internal(std::string(kStreamCompletionPendingMessage));
  bool completed = false;
  std::mutex cancellation_mutex;
  std::function<void()> cancel_transfer;
  const bool needs_cancellation_watcher = !register_cancellation && static_cast<bool>(is_cancelled);
  std::atomic<bool> cancellation_requested{false};
  const auto effective_cancellation_registrar =
      needs_cancellation_watcher
          ? std::function<void(const std::function<void()>&)>{[&cancellation_mutex, &cancel_transfer](
                                                                  const std::function<void()>& callback) {
              std::lock_guard lock(cancellation_mutex);
              cancel_transfer = callback;
            }}
          : register_cancellation;
  const auto effective_is_cancelled =
      needs_cancellation_watcher
          ? std::function<bool()>{[&cancellation_requested] { return cancellation_requested.load(); }}
          : is_cancelled;
  std::atomic<bool> stop_cancellation_watcher{false};
  std::thread cancellation_watcher;
  if (needs_cancellation_watcher) {
    cancellation_watcher = std::thread([&] {
      while (!stop_cancellation_watcher.load()) {
        if (!cancellation_requested.load() && is_cancelled()) {
          cancellation_requested.store(true);
        }
        if (cancellation_requested.load()) {
          std::function<void()> cancel;
          {
            std::lock_guard lock(cancellation_mutex);
            cancel = cancel_transfer;
          }
          if (cancel) {
            cancel();
            return;
          }
        }
        std::this_thread::sleep_for(kSynchronousCancellationCheckInterval);
      }
    });
  }
  const auto started = StartStreamRequest(
      request, on_metadata, on_chunk, effective_is_cancelled, effective_cancellation_registrar,
      [&completion_mutex, &completion_condition, &response, &completed](core::Result<Response> result) {
        {
          std::lock_guard lock(completion_mutex);
          response = std::move(result);
          completed = true;
        }
        completion_condition.notify_one();
      });
  if (!started.ok()) {
    StopCancellationWatcher(stop_cancellation_watcher, cancellation_watcher);
    return started.error();
  }
  std::unique_lock lock(completion_mutex);
  completion_condition.wait(lock, [&completed] { return completed; });
  lock.unlock();
  StopCancellationWatcher(stop_cancellation_watcher, cancellation_watcher);
  return response;
}

core::Result<void> Client::StartStreamRequest(
    Request request, std::function<core::Result<void>(const Response&)> on_metadata,
    std::function<core::Result<void>(std::string_view)> on_chunk, std::function<bool()> is_cancelled,
    const std::function<void(const std::function<void()>&)>& register_cancellation,
    StreamCompletion on_complete) const {
  if (!on_metadata || !on_chunk || !is_cancelled || !on_complete) {
    return core::Error::Validation(std::string(kAsyncStreamCallbacksRequiredMessage)).WithTransport(kHttpTransportName);
  }
  if (state_->global_state->code != CURLE_OK) {
    return core::Error::Internal(
        detail::BuildCurlErrorMessage(kCurlInitFailureMessage, state_->global_state->code, {}));
  }
  auto headers = detail::BuildHeaders(request.headers);
  if (!headers.ok()) {
    return headers.error();
  }
  auto acquired_slot = detail::AcquireStreamSlot(*state_);
  if (!acquired_slot.ok()) {
    return acquired_slot.error();
  }
  const auto dispatch_ready = EnsureStreamDispatchExecutor();
  if (!dispatch_ready.ok()) {
    return dispatch_ready.error();
  }
  auto async_state = std::make_shared<AsyncStreamState>();
  async_state->client_state = state_;
  async_state->slot = std::move(acquired_slot.value());
  async_state->reactor = async_state->slot->reactor;
  if (async_state->reactor == nullptr) {
    return core::Error::Internal(std::string(kCurlMultiInitFailureMessage));
  }
  async_state->request = std::move(request);
  async_state->headers = std::move(headers.value());
  async_state->metadata_handler = std::move(on_metadata);
  async_state->chunk_handler = std::move(on_chunk);
  async_state->is_cancelled = std::move(is_cancelled);
  async_state->completion = std::move(on_complete);
  async_state->header_capture.response_headers = &async_state->response_headers;
  std::weak_ptr<AsyncStreamState> weak_state = async_state;
  const auto queued_metadata = [weak_state](const Response& response) -> core::Result<void> {
    if (const auto locked = weak_state.lock()) {
      locked->QueueMetadata(response);
    }
    return {};
  };
  const auto queued_chunk = [weak_state](std::string_view chunk) -> core::Result<void> {
    if (const auto locked = weak_state.lock()) {
      locked->QueueChunk(std::string(chunk));
    }
    return {};
  };
  async_state->callback_context = {.on_metadata = nullptr,
                                   .on_chunk = nullptr,
                                   .is_cancelled = &async_state->is_cancelled,
                                   .header_capture = &async_state->header_capture,
                                   .error = std::nullopt,
                                   .metadata_checked = false};
  // Callback function objects must be owned by the transfer state because
  // libcurl stores only their addresses in StreamCallbackContext.
  async_state->metadata_handler = [handler = std::move(async_state->metadata_handler)](const Response& response) {
    return handler(response);
  };
  async_state->chunk_handler = [handler = std::move(async_state->chunk_handler)](std::string_view chunk) {
    return handler(chunk);
  };
  // Store the bounded capture wrappers separately from user handlers.
  auto capture_metadata = std::make_shared<std::function<core::Result<void>(const Response&)>>(queued_metadata);
  auto capture_chunk = std::make_shared<std::function<core::Result<void>(std::string_view)>>(queued_chunk);
  async_state->callback_context.on_metadata = capture_metadata.get();
  async_state->callback_context.on_chunk = capture_chunk.get();
  async_state->stream_header_context = {.header_capture = &async_state->header_capture,
                                        .stream_context = &async_state->callback_context};
  CURL* const handle = async_state->slot->easy_handle;
  curl_easy_reset(handle);
  const auto set_error_buffer = curl_easy_setopt(handle, CURLOPT_ERRORBUFFER, async_state->error_buffer.data());
  if (set_error_buffer != CURLE_OK) {
    return core::Error::Internal(detail::BuildCurlErrorMessage(kErrorBufferFailureMessage, set_error_buffer, {}));
  }
  const auto configured = detail::ConfigureCurlStream(handle, async_state->request, async_state->headers,
                                                      &async_state->callback_context, &async_state->header_capture);
  if (!configured.ok()) {
    return configured.error();
  }
  if (curl_easy_setopt(handle, CURLOPT_HEADERFUNCTION, detail::WriteStreamResponseHeader) != CURLE_OK ||
      curl_easy_setopt(handle, CURLOPT_HEADERDATA, &async_state->stream_header_context) != CURLE_OK) {
    return core::Error::Internal(std::string(kConfigureRequestFailureMessage));
  }
  async_state->transfer = std::make_shared<detail::CurlStreamReactor::Transfer>();
  async_state->transfer->easy_handle = handle;
  async_state->transfer->owner = state_.get();
  async_state->transfer->lifetime = async_state;
  // Keep capture wrappers alive alongside the transfer without adding raw
  // callback addresses to any caller stack.
  struct CaptureLifetime final {
    std::shared_ptr<AsyncStreamState> state;
    std::shared_ptr<std::function<core::Result<void>(const Response&)>> metadata;
    std::shared_ptr<std::function<core::Result<void>(std::string_view)>> chunk;
  };
  async_state->transfer->lifetime = std::make_shared<CaptureLifetime>(CaptureLifetime{
      .state = async_state, .metadata = std::move(capture_metadata), .chunk = std::move(capture_chunk)});
  async_state->transfer->on_complete = [weak_state](CURLcode code) {
    if (const auto locked = weak_state.lock()) {
      locked->Finish(code);
    }
  };
  {
    std::lock_guard lock(state_->stream_mutex);
    if (state_->shutting_down) {
      async_state->transfer->lifetime.reset();
      async_state->transfer->on_complete = {};
      return core::Error::Network(std::string(kClientShuttingDownMessage)).WithTransport(kHttpTransportName);
    }
    ++state_->active_streams;
    async_state->registered = true;
    // Publish the add command before shutdown can enqueue owner cancellation.
    async_state->reactor->Add(async_state->transfer);
  }
  if (register_cancellation) {
    register_cancellation([weak_state] {
      if (const auto locked = weak_state.lock()) {
        locked->reactor->Cancel(locked->transfer);
      }
    });
  }
  return {};
}

namespace testing {

bool StreamDispatchExecutorHandlesPartialConstructionFailure() {
  constexpr std::size_t kSuccessfulWorkersBeforeFailure = 1U;
  std::size_t started_workers = 0U;
  try {
    StreamDispatchExecutor executor([&started_workers](StreamDispatchExecutor::Task task) -> std::thread {
      if (started_workers == kSuccessfulWorkersBeforeFailure) {
        throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again));
      }
      std::thread worker(std::move(task));
      ++started_workers;
      return worker;
    });
  } catch (const std::system_error&) {
    return started_workers == kSuccessfulWorkersBeforeFailure;
  }
  return false;
}

bool StreamDispatchExecutorUsesProcessStateLifetime() {
  auto& executor = GetStreamDispatchExecutor();
  return GetHttpProcessState().stream_dispatch_executor.get() == &executor;
}

bool CurlStreamReactorHandlesThreadStartupFailure() {
  const auto reactor = detail::CurlStreamReactor::Create([](const std::function<void()>&) -> std::thread {
    throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again));
  });
  return reactor == nullptr;
}

bool CurlStreamReactorPoolDefersSelfThreadRelease() {
  const auto dispatch_ready = EnsureStreamDispatchExecutor();
  if (!dispatch_ready.ok()) {
    return false;
  }
  auto reactor_pool = std::make_shared<detail::CurlStreamReactorPool>();
  auto reactor = reactor_pool->Acquire();
  if (reactor == nullptr) {
    return false;
  }
  auto transfer = std::make_shared<detail::CurlStreamReactor::Transfer>();
  std::promise<bool> deferred_release_promise;
  auto deferred_release = deferred_release_promise.get_future();
  transfer->on_complete = [reactor_pool, &deferred_release_promise](CURLcode) {
    const bool needs_retry = !reactor_pool->ReleaseUnused();
    if (needs_retry) {
      DeferUnusedReactorRelease(reactor_pool);
    }
    deferred_release_promise.set_value(needs_retry);
  };
  reactor->Cancel(transfer);
  reactor.reset();
  const bool was_deferred = deferred_release.get();
  const bool released_off_thread = reactor_pool->ReleaseUnused();
  return was_deferred && released_off_thread && reactor_pool->size() == 0U;
}

std::pair<std::size_t, std::size_t> CurlStreamReactorLifecycleCounts() noexcept {
  return detail::GetCurlStreamReactorLifecycleCounts();
}

std::size_t CurlStreamReactorPoolSize() { return detail::EnsureCurlGlobalInit()->stream_reactor_pool->size(); }

}  // namespace testing

}  // namespace a2a::http

#endif
