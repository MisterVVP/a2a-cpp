// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "a2a/http/http_client.h"

#include <string>
#include <string_view>

#include "a2a/core/error.h"
#include "http_client_internal.h"

#if defined(A2A_HAS_LIBCURL)
#include <curl/curl.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>
#endif

namespace a2a::http {
namespace {
constexpr char kHttpTransportName[] = "http";
constexpr std::string_view kStreamCompletionPendingMessage = "HTTP stream completion is pending";
}  // namespace

#if defined(A2A_HAS_LIBCURL)

namespace detail {
struct ClientGlobalState final {
  ClientGlobalState();

  CURLcode code = CURLE_OK;
  std::shared_ptr<CurlStreamReactorPool> stream_reactor_pool;
};

struct RequestSlot final {
  ~RequestSlot() {
    if (easy_handle != nullptr) {
      curl_easy_cleanup(easy_handle);
    }
  }

  CURL* easy_handle = nullptr;
  std::shared_ptr<CurlStreamReactor> reactor;
};

struct StreamSlot final {
  ~StreamSlot() {
    if (easy_handle != nullptr) {
      curl_easy_cleanup(easy_handle);
    }
  }

  CURL* easy_handle = nullptr;
  std::shared_ptr<CurlStreamReactor> reactor;
};

struct ClientState final {
  std::shared_ptr<const ClientGlobalState> global_state;
  std::atomic_size_t client_owners{1U};
  // The first unary and streaming handles share one reactor shard so common
  // request -> stream sequences can reuse the same CURLM connection cache.
  std::mutex reactor_mutex;
  std::weak_ptr<CurlStreamReactor> primary_reactor;
  // Blocking unary requests borrow independent easy handles. This mutex
  // protects only the idle pool and is never held across network I/O.
  std::mutex request_mutex;
  std::vector<std::unique_ptr<RequestSlot>> idle_request_slots;
  // Guards shutdown, stream accounting, and reusable stream easy handles.
  std::mutex stream_mutex;
  std::vector<std::unique_ptr<StreamSlot>> idle_stream_slots;
  bool primary_stream_reactor_assigned = false;
  std::condition_variable streams_finished;
  std::size_t active_streams = 0;
  bool shutting_down = false;
  std::atomic<bool> suppress_stream_callbacks{false};
};
}  // namespace detail

namespace {

constexpr std::string_view kCurlInitFailureMessage = "failed to initialize HTTP client";
constexpr std::string_view kCurlHeaderFailureMessage = "failed to build HTTP request headers";
constexpr std::string_view kConfigureRequestFailureMessage = "failed to configure HTTP request";
constexpr std::string_view kErrorBufferFailureMessage = "failed to configure HTTP client error buffer";
constexpr std::string_view kRequestFailureMessage = "failed to execute HTTP request";
constexpr std::string_view kCurlMultiInitFailureMessage = "failed to initialize HTTP stream poller";
constexpr std::string_view kReadStatusFailureMessage = "failed to read HTTP response status";
constexpr std::string_view kUnsupportedHttpVersionMessage = "HTTP client supports only HTTP/1.1, HTTP/2.0, or HTTP/3.0";
constexpr std::string_view kMalformedStatusMessage = "HTTP server did not return a response status";
constexpr std::string_view kHttpStatusLinePrefix = "HTTP/";
constexpr std::string_view kMissingStreamMetadataCallbackMessage = "HTTP stream metadata callback is required";
constexpr std::string_view kMissingStreamChunkCallbackMessage = "HTTP stream chunk callback is required";
constexpr std::string_view kMissingStreamCancellationCallbackMessage = "HTTP stream cancellation callback is required";
constexpr std::string_view kStreamCancelledMessage = "HTTP stream was cancelled";
constexpr std::string_view kAsyncStreamCallbacksRequiredMessage = "HTTP asynchronous stream callbacks are required";
constexpr std::string_view kClientShuttingDownMessage = "HTTP client is shutting down";
constexpr char kHeaderSeparator = ':';
constexpr long kHttpResponseCodeUnset = 0;
constexpr long kHttpInformationalStatusMin = 100;
constexpr long kHttpInformationalStatusMax = 199;
constexpr std::size_t kMaxIdleRequestSlots = 64U;
constexpr std::size_t kMaxIdleStreamSlots = 64U;
constexpr std::size_t kMaximumPendingDispatchTasks = 256U;
constexpr std::size_t kMaximumPendingDispatchBytes = std::size_t{4U} * 1024U * 1024U;
constexpr std::string_view kDispatchBacklogExceededMessage = "HTTP stream callback backlog limit exceeded";
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

std::shared_ptr<const detail::ClientGlobalState> EnsureCurlGlobalInit() {
  static const auto init = std::make_shared<detail::ClientGlobalState>();
  return init;
}

struct CurlSlistDeleter final {
  void operator()(curl_slist* list) const noexcept {
    if (list != nullptr) {
      curl_slist_free_all(list);
    }
  }
};

using CurlHeaderList = std::unique_ptr<curl_slist, CurlSlistDeleter>;

}  // namespace

namespace detail {

ClientGlobalState::ClientGlobalState()
    : code(curl_global_init(CURL_GLOBAL_DEFAULT)), stream_reactor_pool(std::make_shared<CurlStreamReactorPool>()) {}

}  // namespace detail

namespace {

std::shared_ptr<detail::CurlStreamReactor> GetOrCreatePrimaryReactor(detail::ClientState& state) {
  std::lock_guard lock(state.reactor_mutex);
  auto reactor = state.primary_reactor.lock();
  if (reactor == nullptr) {
    reactor = state.global_state->stream_reactor_pool->Acquire();
    state.primary_reactor = reactor;
  }
  return reactor;
}

std::string BuildCurlErrorMessage(std::string_view prefix, CURLcode code, std::string_view detail) {
  std::ostringstream message;
  message << prefix << ": " << curl_easy_strerror(code);
  if (!detail.empty()) {
    message << " (" << detail << ')';
  }
  return message.str();
}

core::Result<void> ValidateStreamCallbacks(const detail::StreamCallbackContext& context) {
  if (context.on_metadata == nullptr || !*context.on_metadata) {
    return core::Error::Validation(std::string(kMissingStreamMetadataCallbackMessage)).WithTransport("http");
  }
  if (context.on_chunk == nullptr || !*context.on_chunk) {
    return core::Error::Validation(std::string(kMissingStreamChunkCallbackMessage)).WithTransport("http");
  }
  if (context.is_cancelled == nullptr || !*context.is_cancelled) {
    return core::Error::Validation(std::string(kMissingStreamCancellationCallbackMessage)).WithTransport("http");
  }
  return {};
}

std::string BuildHeaderValue(const Header& header) {
  std::string value;
  value.reserve(header.name.size() + core::http::kHeaderNameValueSeparator.size() + header.value.size());
  value.append(header.name);
  value.append(core::http::kHeaderNameValueSeparator);
  value.append(header.value);
  return value;
}

core::Result<void> AppendHeader(CurlHeaderList* headers, const std::string& header) {
  curl_slist* const current = headers->get();
  curl_slist* const updated = curl_slist_append(current, header.c_str());
  if (updated == nullptr) {
    return core::Error::Internal(std::string(kCurlHeaderFailureMessage));
  }
  if (current == nullptr) {
    headers->reset(updated);
  } else if (updated != current) {
    curl_slist* const released = headers->release();
    (void)released;
    headers->reset(updated);
  }
  return {};
}

core::Result<CurlHeaderList> BuildHeaders(const std::vector<Header>& headers) {
  CurlHeaderList list;
  for (const auto& header : headers) {
    const auto appended = AppendHeader(&list, BuildHeaderValue(header));
    if (!appended.ok()) {
      return appended.error();
    }
  }
  return list;
}

std::string_view TrimHeaderValue(std::string_view value) {
  while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
    value.remove_prefix(1);
  }
  while (!value.empty() &&
         (value.back() == '\r' || value.back() == '\n' || value.back() == ' ' || value.back() == '\t')) {
    value.remove_suffix(1);
  }
  return value;
}

std::optional<long> ParseHttpStatusCode(std::string_view line) {
  if (!line.starts_with(kHttpStatusLinePrefix)) {
    return std::nullopt;
  }
  const auto status_start = line.find(' ');
  if (status_start == std::string_view::npos || status_start + 1 >= line.size()) {
    return std::nullopt;
  }
  long status = kHttpResponseCodeUnset;
  const char* const first = line.data() + status_start + 1;
  const char* const last = line.data() + line.size();
  const auto parsed = std::from_chars(first, last, status);
  if (parsed.ec != std::errc{}) {
    return std::nullopt;
  }
  return status;
}

std::optional<Header> ParseHeaderLine(std::string_view line) {
  const auto separator = line.find(kHeaderSeparator);
  if (separator == std::string_view::npos) {
    return std::nullopt;
  }

  std::string name(line.substr(0, separator));
  const std::string_view value = TrimHeaderValue(line.substr(separator + 1));
  return Header{.name = std::move(name), .value = std::string(value)};
}

struct StreamHeaderContext final {
  detail::HeaderCapture* header_capture = nullptr;
  detail::StreamCallbackContext* stream_context = nullptr;
};

size_t WriteResponseHeader(char* contents, size_t size, size_t nmemb, void* user_data) {
  auto* capture = static_cast<detail::HeaderCapture*>(user_data);
  const std::size_t byte_count = size * nmemb;
  const std::string_view line(contents, byte_count);
  if (line.starts_with(kHttpStatusLinePrefix)) {
    capture->response_headers->clear();
    capture->response_code = ParseHttpStatusCode(line).value_or(kHttpResponseCodeUnset);
    return byte_count;
  }
  const auto header = ParseHeaderLine(line);
  if (!header.has_value()) {
    return byte_count;
  }
  capture->response_headers->push_back(header.value());
  return byte_count;
}

core::Result<void> ValidateStreamMetadata(detail::StreamCallbackContext* context) {
  const detail::HeaderCapture& capture = *context->header_capture;
  if (capture.response_code == kHttpResponseCodeUnset) {
    return core::Error::RemoteProtocol(std::string(kMalformedStatusMessage));
  }
  if (context->on_metadata != nullptr) {
    return (*context->on_metadata)(Response{
        .status_code = static_cast<int>(capture.response_code), .headers = *capture.response_headers, .body = {}});
  }
  return {};
}

size_t WriteStreamResponseHeader(char* contents, size_t size, size_t nmemb, void* user_data) {
  auto* context = static_cast<StreamHeaderContext*>(user_data);
  detail::HeaderCapture& capture = *context->header_capture;
  const std::size_t byte_count = size * nmemb;
  const std::string_view line(contents, byte_count);
  if (line.starts_with(kHttpStatusLinePrefix)) {
    capture.response_headers->clear();
    capture.response_code = ParseHttpStatusCode(line).value_or(kHttpResponseCodeUnset);
    return byte_count;
  }
  const auto header = ParseHeaderLine(line);
  if (header.has_value()) {
    capture.response_headers->push_back(header.value());
    return byte_count;
  }
  if (context->stream_context->metadata_checked || !TrimHeaderValue(line).empty()) {
    return byte_count;
  }
  if (capture.response_code >= kHttpInformationalStatusMin && capture.response_code <= kHttpInformationalStatusMax) {
    return byte_count;
  }
  const auto metadata = ValidateStreamMetadata(context->stream_context);
  if (!metadata.ok()) {
    context->stream_context->error = metadata.error();
    return 0;
  }
  context->stream_context->metadata_checked = true;
  return byte_count;
}

size_t WriteResponseBody(char* contents, size_t size, size_t nmemb, void* user_data) {
  auto* body = static_cast<std::string*>(user_data);
  const std::size_t byte_count = size * nmemb;
  body->append(contents, byte_count);
  return byte_count;
}

size_t WriteStreamBody(char* contents, size_t size, size_t nmemb, void* user_data) {
  auto* context = static_cast<detail::StreamCallbackContext*>(user_data);
  const std::size_t byte_count = size * nmemb;
  if (context->is_cancelled != nullptr && (*context->is_cancelled)()) {
    return 0;
  }
  if (!context->metadata_checked) {
    const auto metadata = ValidateStreamMetadata(context);
    if (!metadata.ok()) {
      context->error = metadata.error();
      return 0;
    }
    context->metadata_checked = true;
  }
  const auto result = (*context->on_chunk)(std::string_view(contents, byte_count));
  if (!result.ok()) {
    context->error = result.error();
    return 0;
  }
  return byte_count;
}

int CheckStreamProgress(void* clientp, curl_off_t download_total, curl_off_t downloaded, curl_off_t upload_total,
                        curl_off_t uploaded) {
  (void)download_total;
  (void)downloaded;
  (void)upload_total;
  (void)uploaded;
  auto* context = static_cast<detail::StreamCallbackContext*>(clientp);
  if (context->is_cancelled != nullptr && (*context->is_cancelled)()) {
    return 1;
  }
  return 0;
}

core::Result<long> MapHttpVersion(std::string_view http_version) {
  if (http_version == core::http::kHttpVersion11) {
    return CURL_HTTP_VERSION_1_1;
  }
  if (http_version == core::http::kHttpVersion20) {
    return CURL_HTTP_VERSION_2TLS;
  }
  if (http_version == core::http::kHttpVersion30) {
    return CURL_HTTP_VERSION_3;
  }
  return core::Error::Validation(std::string(kUnsupportedHttpVersionMessage));
}

core::Result<void> ConfigureCurlMethodAndBody(CURL* handle, const Request& request) {
  if (request.method == core::http::kMethodGet && request.body.empty()) {
    if (curl_easy_setopt(handle, CURLOPT_HTTPGET, 1L) != CURLE_OK) {
      return core::Error::Internal(std::string(kConfigureRequestFailureMessage));
    }
    return {};
  }

  const auto set_method = curl_easy_setopt(handle, CURLOPT_CUSTOMREQUEST, request.method.c_str());
  const auto set_body = curl_easy_setopt(handle, CURLOPT_POSTFIELDS, request.body.c_str());
  const auto set_body_size =
      curl_easy_setopt(handle, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(request.body.size()));
  if (set_method != CURLE_OK || set_body != CURLE_OK || set_body_size != CURLE_OK) {
    return core::Error::Internal(std::string(kConfigureRequestFailureMessage));
  }
  return {};
}

core::Result<void> ConfigureCurl(CURL* handle, const Request& request, const CurlHeaderList& headers,
                                 std::string* response_body, detail::HeaderCapture* response_headers) {
  const auto http_version = MapHttpVersion(request.http_version);
  if (!http_version.ok()) {
    return http_version.error();
  }
  const auto method_and_body = ConfigureCurlMethodAndBody(handle, request);
  if (!method_and_body.ok()) {
    return method_and_body.error();
  }

  const auto set_url = curl_easy_setopt(handle, CURLOPT_URL, request.url.c_str());
  const auto set_headers = curl_easy_setopt(handle, CURLOPT_HTTPHEADER, headers.get());
  const auto set_timeout = curl_easy_setopt(handle, CURLOPT_TIMEOUT_MS, static_cast<long>(request.timeout.count()));
  const auto set_connect_timeout =
      curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(request.timeout.count()));
  const auto set_no_signal = curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1L);
  const auto set_write = curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, WriteResponseBody);
  const auto set_write_data = curl_easy_setopt(handle, CURLOPT_WRITEDATA, response_body);
  const auto set_header = curl_easy_setopt(handle, CURLOPT_HEADERFUNCTION, WriteResponseHeader);
  const auto set_header_data = curl_easy_setopt(handle, CURLOPT_HEADERDATA, response_headers);
  const auto set_http_version = curl_easy_setopt(handle, CURLOPT_HTTP_VERSION, http_version.value());
  const auto set_tls_minimum = curl_easy_setopt(handle, CURLOPT_SSLVERSION, CURL_SSLVERSION_TLSv1_2);
  const auto set_suppress_connect_headers = curl_easy_setopt(handle, CURLOPT_SUPPRESS_CONNECT_HEADERS, 1L);
  if (set_url != CURLE_OK || set_headers != CURLE_OK || set_timeout != CURLE_OK || set_connect_timeout != CURLE_OK ||
      set_no_signal != CURLE_OK || set_write != CURLE_OK || set_write_data != CURLE_OK || set_header != CURLE_OK ||
      set_header_data != CURLE_OK || set_http_version != CURLE_OK || set_tls_minimum != CURLE_OK ||
      set_suppress_connect_headers != CURLE_OK) {
    return core::Error::Internal(std::string(kConfigureRequestFailureMessage));
  }
  return {};
}

core::Result<void> ConfigureCurlStream(CURL* handle, const Request& request, const CurlHeaderList& headers,
                                       detail::StreamCallbackContext* stream_context,
                                       detail::HeaderCapture* response_headers) {
  const auto callbacks = ValidateStreamCallbacks(*stream_context);
  if (!callbacks.ok()) {
    return callbacks.error();
  }
  std::string unused_body;
  const auto configured = ConfigureCurl(handle, request, headers, &unused_body, response_headers);
  if (!configured.ok()) {
    return configured.error();
  }
  const auto set_write = curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, WriteStreamBody);
  const auto set_write_data = curl_easy_setopt(handle, CURLOPT_WRITEDATA, stream_context);
  const auto set_progress_data = curl_easy_setopt(handle, CURLOPT_XFERINFODATA, stream_context);
  const auto set_progress = curl_easy_setopt(handle, CURLOPT_XFERINFOFUNCTION, CheckStreamProgress);
  const auto set_no_progress = curl_easy_setopt(handle, CURLOPT_NOPROGRESS, 0L);
  if (set_write != CURLE_OK || set_write_data != CURLE_OK || set_progress_data != CURLE_OK ||
      set_progress != CURLE_OK || set_no_progress != CURLE_OK) {
    return core::Error::Internal(std::string(kConfigureRequestFailureMessage));
  }
  return {};
}

core::Result<std::unique_ptr<detail::RequestSlot>> AcquireRequestSlot(detail::ClientState& state) {
  std::unique_ptr<detail::RequestSlot> slot;
  bool use_primary_reactor = false;
  {
    std::lock_guard lock(state.request_mutex);
    if (!state.idle_request_slots.empty()) {
      slot = std::move(state.idle_request_slots.back());
      state.idle_request_slots.pop_back();
      use_primary_reactor = slot->reactor == nullptr;
    }
  }
  if (slot == nullptr) {
    slot = std::make_unique<detail::RequestSlot>();
    slot->easy_handle = curl_easy_init();
    if (slot->easy_handle == nullptr) {
      return core::Error::Internal(std::string(kCurlInitFailureMessage));
    }
  }
  if (slot->reactor == nullptr) {
    slot->reactor =
        use_primary_reactor ? GetOrCreatePrimaryReactor(state) : state.global_state->stream_reactor_pool->Acquire();
    if (slot->reactor == nullptr) {
      return core::Error::Internal(std::string(kCurlMultiInitFailureMessage));
    }
  }
  return slot;
}

void ReleaseRequestSlot(detail::ClientState& state, std::unique_ptr<detail::RequestSlot> slot) {
  std::lock_guard lock(state.request_mutex);
  if (state.idle_request_slots.size() < kMaxIdleRequestSlots) {
    state.idle_request_slots.push_back(std::move(slot));
  }
}

core::Result<std::unique_ptr<detail::StreamSlot>> AcquireStreamSlot(detail::ClientState& state) {
  {
    std::lock_guard lock(state.stream_mutex);
    if (state.shutting_down) {
      return core::Error::Network(std::string(kClientShuttingDownMessage)).WithTransport(kHttpTransportName);
    }
    if (!state.idle_stream_slots.empty()) {
      auto slot = std::move(state.idle_stream_slots.back());
      state.idle_stream_slots.pop_back();
      return slot;
    }
  }
  auto slot = std::make_unique<detail::StreamSlot>();
  slot->easy_handle = curl_easy_init();
  if (slot->easy_handle == nullptr) {
    return core::Error::Internal(std::string(kCurlMultiInitFailureMessage));
  }
  bool use_primary_reactor = false;
  {
    std::lock_guard lock(state.stream_mutex);
    if (state.shutting_down) {
      return core::Error::Network(std::string(kClientShuttingDownMessage)).WithTransport(kHttpTransportName);
    }
    use_primary_reactor = !state.primary_stream_reactor_assigned;
    state.primary_stream_reactor_assigned = true;
  }
  slot->reactor =
      use_primary_reactor ? GetOrCreatePrimaryReactor(state) : state.global_state->stream_reactor_pool->Acquire();
  if (slot->reactor == nullptr) {
    return core::Error::Internal(std::string(kCurlMultiInitFailureMessage));
  }
  return slot;
}

void ReleaseStreamSlot(detail::ClientState& state, std::unique_ptr<detail::StreamSlot> slot) {
  std::lock_guard lock(state.stream_mutex);
  if (!state.shutting_down && state.idle_stream_slots.size() < kMaxIdleStreamSlots) {
    state.idle_stream_slots.push_back(std::move(slot));
  }
}

CURLcode PerformCurlTransfer(CURL* handle, const std::shared_ptr<detail::CurlStreamReactor>& reactor) {
  auto transfer = std::make_shared<detail::CurlStreamReactor::Transfer>();
  transfer->easy_handle = handle;
  reactor->Add(transfer);

  std::unique_lock lock(transfer->mutex);
  transfer->completed.wait(lock, [&transfer] { return transfer->done; });
  return transfer->result;
}

class StreamDispatchExecutor final {
 public:
  using Task = std::function<void()>;

  StreamDispatchExecutor() {
    constexpr std::size_t kWorkerCount = 4U;
    workers_.reserve(kWorkerCount);
    for (std::size_t index = 0; index < kWorkerCount; ++index) {
      workers_.emplace_back([this] { Run(); });
    }
  }

  ~StreamDispatchExecutor() {
    {
      std::lock_guard lock(mutex_);
      stopping_ = true;
    }
    available_.notify_all();
    for (auto& worker : workers_) {
      worker.join();
    }
  }

  void Submit(Task task) {
    {
      std::lock_guard lock(mutex_);
      tasks_.push_back(std::move(task));
    }
    available_.notify_one();
  }

 private:
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

StreamDispatchExecutor& GetStreamDispatchExecutor() {
  static StreamDispatchExecutor executor;
  return executor;
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
  CurlHeaderList headers;
  std::array<char, CURL_ERROR_SIZE> error_buffer{};
  std::vector<Header> response_headers;
  detail::HeaderCapture header_capture;
  detail::StreamCallbackContext callback_context;
  StreamHeaderContext stream_header_context;
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
    (void)dispatch->Enqueue(
        [self = shared_from_this(), code] {
          DispatchCallbackScope callback_scope(self->client_state.get());
          core::Result<Response> result = self->BuildResult(code);
          ReleaseStreamSlot(*self->client_state, std::move(self->slot));
          self->completion(std::move(result));
          self->transfer->lifetime.reset();
          self->Finalize();
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
      return core::Error::Network(BuildCurlErrorMessage(kRequestFailureMessage, code, error_buffer.data()));
    }
    if (!callback_context.metadata_checked) {
      const auto metadata = ValidateStreamMetadata(&callback_context);
      if (!metadata.ok()) {
        return metadata.error();
      }
    }
    long response_code = kHttpResponseCodeUnset;
    const CURLcode info_code = curl_easy_getinfo(slot->easy_handle, CURLINFO_RESPONSE_CODE, &response_code);
    if (info_code != CURLE_OK) {
      return core::Error::RemoteProtocol(BuildCurlErrorMessage(kReadStatusFailureMessage, info_code, {}));
    }
    if (response_code == kHttpResponseCodeUnset) {
      return core::Error::RemoteProtocol(std::string(kMalformedStatusMessage));
    }
    return Response{.status_code = static_cast<int>(response_code), .headers = response_headers, .body = {}};
  }
};

}  // namespace

bool IsSupportedHttpVersion(std::string_view http_version) noexcept {
  return http_version == core::http::kHttpVersion11 || http_version == core::http::kHttpVersion20 ||
         http_version == core::http::kHttpVersion30;
}

Client::Client() : state_(std::make_shared<detail::ClientState>()) {
  state_->global_state = EnsureCurlGlobalInit();
  auto slot = std::make_unique<detail::RequestSlot>();
  slot->easy_handle = curl_easy_init();
  if (slot->easy_handle != nullptr) {
    state_->idle_request_slots.push_back(std::move(slot));
  }
}

Client::Client(const Client& other) : state_(other.state_) {
  if (state_ != nullptr) {
    state_->client_owners.fetch_add(1U, std::memory_order_relaxed);
  }
}

Client& Client::operator=(const Client& other) {
  if (this == &other) {
    return *this;
  }
  ReleaseOwner();
  state_ = other.state_;
  if (state_ != nullptr) {
    state_->client_owners.fetch_add(1U, std::memory_order_relaxed);
  }
  return *this;
}

Client::Client(Client&& other) noexcept : state_(std::move(other.state_)) {}

Client& Client::operator=(Client&& other) noexcept {
  if (this == &other) {
    return *this;
  }
  ReleaseOwner();
  state_ = std::move(other.state_);
  return *this;
}

Client::~Client() { ReleaseOwner(); }

void Client::ReleaseOwner() {
  if (state_ == nullptr) {
    return;
  }
  if (state_->client_owners.fetch_sub(1U, std::memory_order_acq_rel) == 1U) {
    Shutdown();
  }
  state_.reset();
}

void Client::Shutdown() const {
  if (state_ == nullptr) {
    return;
  }
  std::vector<std::unique_ptr<detail::StreamSlot>> released_stream_slots;
  bool has_active_streams = false;
  {
    std::lock_guard lock(state_->stream_mutex);
    state_->shutting_down = true;
    state_->suppress_stream_callbacks.store(true);
    has_active_streams = state_->active_streams != 0U;
    released_stream_slots.swap(state_->idle_stream_slots);
  }
  if (has_active_streams) {
    state_->global_state->stream_reactor_pool->CancelOwner(state_.get());
  }
  if (g_dispatch_client_state == state_.get()) {
    return;
  }
  std::unique_lock lock(state_->stream_mutex);
  state_->streams_finished.wait(lock, [this] { return state_->active_streams == 0U; });
}

core::Result<Response> Client::SendRequest(const Request& request) const {
  if (state_->global_state->code != CURLE_OK) {
    return core::Error::Internal(BuildCurlErrorMessage(kCurlInitFailureMessage, state_->global_state->code, {}));
  }

  auto headers = BuildHeaders(request.headers);
  if (!headers.ok()) {
    return headers.error();
  }

  auto acquired_slot = AcquireRequestSlot(*state_);
  if (!acquired_slot.ok()) {
    return acquired_slot.error();
  }
  auto slot = std::move(acquired_slot.value());
  CURL* const handle = slot->easy_handle;
  curl_easy_reset(handle);

  std::array<char, CURL_ERROR_SIZE> error_buffer{};
  const auto set_error_buffer = curl_easy_setopt(handle, CURLOPT_ERRORBUFFER, error_buffer.data());
  if (set_error_buffer != CURLE_OK) {
    return core::Error::Internal(BuildCurlErrorMessage(kErrorBufferFailureMessage, set_error_buffer, {}));
  }

  std::string response_body;
  std::vector<Header> response_headers;
  detail::HeaderCapture header_capture{.response_headers = &response_headers};
  const auto configured = ConfigureCurl(handle, request, headers.value(), &response_body, &header_capture);
  if (!configured.ok()) {
    return configured.error();
  }

  const CURLcode code = PerformCurlTransfer(handle, slot->reactor);
  if (code != CURLE_OK) {
    return core::Error::Network(BuildCurlErrorMessage(kRequestFailureMessage, code, error_buffer.data()));
  }

  long response_code = kHttpResponseCodeUnset;
  const CURLcode info_code = curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &response_code);
  if (info_code != CURLE_OK) {
    return core::Error::RemoteProtocol(BuildCurlErrorMessage(kReadStatusFailureMessage, info_code, {}));
  }
  if (response_code == kHttpResponseCodeUnset) {
    return core::Error::RemoteProtocol(std::string(kMalformedStatusMessage));
  }
  Response response{.status_code = static_cast<int>(response_code),
                    .headers = std::move(response_headers),
                    .body = std::move(response_body)};
  ReleaseRequestSlot(*state_, std::move(slot));
  return response;
}

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
  std::mutex completion_mutex;
  std::condition_variable completion_condition;
  core::Result<Response> response = core::Error::Internal(std::string(kStreamCompletionPendingMessage));
  bool completed = false;
  std::mutex cancellation_mutex;
  std::function<void()> cancel_transfer;
  const bool needs_cancellation_watcher = !register_cancellation;
  const auto effective_cancellation_registrar =
      needs_cancellation_watcher
          ? std::function<void(const std::function<void()>&)>{[&cancellation_mutex, &cancel_transfer](
                                                                  const std::function<void()>& callback) {
              std::lock_guard lock(cancellation_mutex);
              cancel_transfer = callback;
            }}
          : register_cancellation;
  const auto started = StartStreamRequest(
      request, on_metadata, on_chunk, is_cancelled, effective_cancellation_registrar,
      [&completion_mutex, &completion_condition, &response, &completed](core::Result<Response> result) {
        {
          std::lock_guard lock(completion_mutex);
          response = std::move(result);
          completed = true;
        }
        completion_condition.notify_one();
      });
  if (!started.ok()) {
    return started.error();
  }
  std::atomic<bool> stop_cancellation_watcher{false};
  std::thread cancellation_watcher;
  if (needs_cancellation_watcher) {
    cancellation_watcher = std::thread([&] {
      while (!stop_cancellation_watcher.load()) {
        if (is_cancelled()) {
          std::function<void()> cancel;
          {
            std::lock_guard lock(cancellation_mutex);
            cancel = cancel_transfer;
          }
          if (cancel) {
            cancel();
          }
          return;
        }
        std::this_thread::sleep_for(kSynchronousCancellationCheckInterval);
      }
    });
  }
  std::unique_lock lock(completion_mutex);
  completion_condition.wait(lock, [&completed] { return completed; });
  lock.unlock();
  stop_cancellation_watcher.store(true);
  if (cancellation_watcher.joinable()) {
    cancellation_watcher.join();
  }
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
    return core::Error::Internal(BuildCurlErrorMessage(kCurlInitFailureMessage, state_->global_state->code, {}));
  }
  auto headers = BuildHeaders(request.headers);
  if (!headers.ok()) {
    return headers.error();
  }
  auto acquired_slot = AcquireStreamSlot(*state_);
  if (!acquired_slot.ok()) {
    return acquired_slot.error();
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
    return core::Error::Internal(BuildCurlErrorMessage(kErrorBufferFailureMessage, set_error_buffer, {}));
  }
  const auto configured = ConfigureCurlStream(handle, async_state->request, async_state->headers,
                                              &async_state->callback_context, &async_state->header_capture);
  if (!configured.ok()) {
    return configured.error();
  }
  if (curl_easy_setopt(handle, CURLOPT_HEADERFUNCTION, WriteStreamResponseHeader) != CURLE_OK ||
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

std::pair<std::size_t, std::size_t> CurlStreamReactorLifecycleCounts() noexcept {
  return detail::GetCurlStreamReactorLifecycleCounts();
}

std::size_t CurlStreamReactorPoolSize() { return EnsureCurlGlobalInit()->stream_reactor_pool->size(); }

}  // namespace testing

#else

namespace {

constexpr std::string_view kLibcurlDisabledMessage =
    "default libcurl-backed HTTP support is disabled; rebuild with A2A_ENABLE_LIBCURL=ON and libcurl available or "
    "inject a custom requester/fetcher";

}  // namespace

Client::Client() = default;
Client::Client(const Client& other) = default;
Client& Client::operator=(const Client& other) = default;
Client::Client(Client&& other) noexcept = default;
Client& Client::operator=(Client&& other) noexcept = default;
Client::~Client() = default;

void Client::ReleaseOwner() {}
void Client::Shutdown() const {}

core::Result<Response> Client::SendRequest(const Request& request) const {
  (void)request;
  return core::Error::Internal(std::string(kLibcurlDisabledMessage)).WithTransport(kHttpTransportName);
}

core::Result<Response> Client::StreamRequest(const Request& request,
                                             const std::function<core::Result<void>(const Response&)>& on_metadata,
                                             const std::function<core::Result<void>(std::string_view)>& on_chunk,
                                             const std::function<bool()>& is_cancelled) const {
  (void)request;
  (void)on_metadata;
  (void)on_chunk;
  (void)is_cancelled;
  return core::Error::Internal(std::string(kLibcurlDisabledMessage)).WithTransport(kHttpTransportName);
}

core::Result<Response> Client::StreamRequest(
    const Request& request, const std::function<core::Result<void>(const Response&)>& on_metadata,
    const std::function<core::Result<void>(std::string_view)>& on_chunk, const std::function<bool()>& is_cancelled,
    const std::function<void(const std::function<void()>&)>& register_cancellation) const {
  std::mutex completion_mutex;
  std::condition_variable completion_condition;
  core::Result<Response> response = core::Error::Internal(std::string(kStreamCompletionPendingMessage));
  bool completed = false;
  const auto started = StartStreamRequest(
      request, on_metadata, on_chunk, is_cancelled, register_cancellation,
      [&completion_mutex, &completion_condition, &response, &completed](core::Result<Response> result) {
        {
          std::lock_guard lock(completion_mutex);
          response = std::move(result);
          completed = true;
        }
        completion_condition.notify_one();
      });
  if (!started.ok()) {
    return started.error();
  }
  std::unique_lock lock(completion_mutex);
  completion_condition.wait(lock, [&completed] { return completed; });
  return response;
}

core::Result<void> Client::StartStreamRequest(
    Request request, std::function<core::Result<void>(const Response&)> on_metadata,
    std::function<core::Result<void>(std::string_view)> on_chunk, std::function<bool()> is_cancelled,
    const std::function<void(const std::function<void()>&)>& register_cancellation,
    StreamCompletion on_complete) const {
  (void)request;
  (void)on_metadata;
  (void)on_chunk;
  (void)is_cancelled;
  (void)register_cancellation;
  (void)on_complete;
  return core::Error::Internal(std::string(kLibcurlDisabledMessage)).WithTransport(kHttpTransportName);
}

bool IsSupportedHttpVersion(std::string_view http_version) noexcept {
  return http_version == core::http::kHttpVersion11 || http_version == core::http::kHttpVersion20 ||
         http_version == core::http::kHttpVersion30;
}

#endif

}  // namespace a2a::http
