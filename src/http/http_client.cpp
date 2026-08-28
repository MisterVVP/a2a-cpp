// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "a2a/http/http_client.h"

#include <string>
#include <string_view>

#include "a2a/core/error.h"

#if defined(A2A_HAS_LIBCURL)
#include <curl/curl.h>

#ifndef _WIN32
#include <poll.h>
#include <unistd.h>
#endif

#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <system_error>
#include <unordered_map>
#include <utility>
#endif

namespace a2a::http {

#if defined(A2A_HAS_LIBCURL)

namespace detail {
struct ClientGlobalState final {
  ClientGlobalState() : code(curl_global_init(CURL_GLOBAL_DEFAULT)) {}

  CURLcode code = CURLE_OK;
};

struct StreamSlot final {
  ~StreamSlot() {
    if (multi_handle != nullptr) {
      curl_multi_cleanup(multi_handle);
    }
    if (easy_handle != nullptr) {
      curl_easy_cleanup(easy_handle);
    }
  }

  CURL* easy_handle = nullptr;
  CURLM* multi_handle = nullptr;
};

struct ClientState final {
  ~ClientState() {
    if (handle != nullptr) {
      curl_easy_cleanup(handle);
    }
  }

  std::shared_ptr<const ClientGlobalState> global_state;
  CURL* handle = nullptr;
  std::mutex mutex;
  std::mutex stream_mutex;
  std::vector<std::unique_ptr<StreamSlot>> idle_stream_slots;
};
}  // namespace detail

namespace {

constexpr std::string_view kCurlInitFailureMessage = "failed to initialize HTTP client";
constexpr std::string_view kCurlHeaderFailureMessage = "failed to build HTTP request headers";
constexpr std::string_view kConfigureRequestFailureMessage = "failed to configure HTTP request";
constexpr std::string_view kErrorBufferFailureMessage = "failed to configure HTTP client error buffer";
constexpr std::string_view kRequestFailureMessage = "failed to execute HTTP request";
constexpr std::string_view kCurlMultiInitFailureMessage = "failed to initialize HTTP stream poller";
constexpr std::string_view kCurlMultiFailureMessage = "failed to execute polled HTTP stream";
constexpr std::string_view kCurlMultiCompletionFailureMessage = "HTTP stream ended without a libcurl completion";
constexpr std::string_view kReadStatusFailureMessage = "failed to read HTTP response status";
constexpr std::string_view kUnsupportedHttpVersionMessage = "HTTP client supports only HTTP/1.1, HTTP/2.0, or HTTP/3.0";
constexpr std::string_view kMalformedStatusMessage = "HTTP server did not return a response status";
constexpr std::string_view kHttpStatusLinePrefix = "HTTP/";
constexpr std::string_view kMissingStreamMetadataCallbackMessage = "HTTP stream metadata callback is required";
constexpr std::string_view kMissingStreamChunkCallbackMessage = "HTTP stream chunk callback is required";
constexpr std::string_view kMissingStreamCancellationCallbackMessage = "HTTP stream cancellation callback is required";
constexpr char kHeaderSeparator = ':';
constexpr long kHttpResponseCodeUnset = 0;
constexpr long kHttpInformationalStatusMin = 100;
constexpr long kHttpInformationalStatusMax = 199;
constexpr int kLegacyStreamPollTimeoutMs = 100;
constexpr std::size_t kMaxIdleStreamSlots = 64U;

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

class CurlMultiAttachment final {
 public:
  CurlMultiAttachment(CURLM* multi_handle, CURL* easy_handle)
      : multi_handle_(multi_handle), easy_handle_(easy_handle) {}

  ~CurlMultiAttachment() {
    if (multi_handle_ != nullptr && easy_handle_ != nullptr) {
      (void)curl_multi_remove_handle(multi_handle_, easy_handle_);
    }
  }

  CurlMultiAttachment(const CurlMultiAttachment&) = delete;
  CurlMultiAttachment& operator=(const CurlMultiAttachment&) = delete;
  CurlMultiAttachment(CurlMultiAttachment&&) = delete;
  CurlMultiAttachment& operator=(CurlMultiAttachment&&) = delete;

 private:
  CURLM* multi_handle_;
  CURL* easy_handle_;
};

#ifndef _WIN32
class CurlMultiSocketLoop final {
 public:
  explicit CurlMultiSocketLoop(CURLM* multi_handle) : multi_handle_(multi_handle) {
    if (::pipe(wakeup_pipe_.data()) != 0) {
      wakeup_pipe_ = {-1, -1};
    }
  }

  ~CurlMultiSocketLoop() {
    for (const int descriptor : wakeup_pipe_) {
      if (descriptor >= 0) {
        (void)::close(descriptor);
      }
    }
  }

  [[nodiscard]] core::Result<void> Configure() {
    const CURLMcode socket_code =
        curl_multi_setopt(multi_handle_, CURLMOPT_SOCKETFUNCTION, &CurlMultiSocketLoop::HandleSocket);
    const CURLMcode socket_data_code = curl_multi_setopt(multi_handle_, CURLMOPT_SOCKETDATA, this);
    const CURLMcode timer_code =
        curl_multi_setopt(multi_handle_, CURLMOPT_TIMERFUNCTION, &CurlMultiSocketLoop::HandleTimer);
    const CURLMcode timer_data_code = curl_multi_setopt(multi_handle_, CURLMOPT_TIMERDATA, this);
    if (socket_code != CURLM_OK || socket_data_code != CURLM_OK || timer_code != CURLM_OK ||
        timer_data_code != CURLM_OK || wakeup_pipe_[0] < 0) {
      return core::Error::Internal(std::string(kCurlMultiInitFailureMessage));
    }
    return {};
  }

  void Wake() noexcept {
    constexpr char kWakeByte = 1;
    if (wakeup_pipe_[1] >= 0) {
      (void)::write(wakeup_pipe_[1], &kWakeByte, sizeof(kWakeByte));
    }
  }

  [[nodiscard]] CURLMcode Run(int* running_handles, const std::function<bool()>& is_cancelled) {
    CURLMcode code = curl_multi_socket_action(multi_handle_, CURL_SOCKET_TIMEOUT, 0, running_handles);
    while (code == CURLM_OK && *running_handles > 0 && !is_cancelled()) {
      std::vector<pollfd> descriptors;
      descriptors.reserve(sockets_.size() + 1U);
      descriptors.push_back({.fd = wakeup_pipe_[0], .events = POLLIN, .revents = 0});
      for (const auto& [socket, events] : sockets_) {
        descriptors.push_back({.fd = socket, .events = events, .revents = 0});
      }
      const int ready = ::poll(descriptors.data(), descriptors.size(), TimerWaitMilliseconds());
      if (ready < 0) {
        return CURLM_INTERNAL_ERROR;
      }
      if (ready == 0) {
        timer_deadline_.reset();
        code = curl_multi_socket_action(multi_handle_, CURL_SOCKET_TIMEOUT, 0, running_handles);
        continue;
      }
      if ((descriptors.front().revents & POLLIN) != 0) {
        char wake_byte = 0;
        (void)::read(wakeup_pipe_[0], &wake_byte, sizeof(wake_byte));
      }
      for (std::size_t index = 1; code == CURLM_OK && index < descriptors.size(); ++index) {
        const short returned_events = descriptors[index].revents;
        if (returned_events == 0) {
          continue;
        }
        int action = 0;
        if ((returned_events & POLLIN) != 0) {
          action |= CURL_CSELECT_IN;
        }
        if ((returned_events & POLLOUT) != 0) {
          action |= CURL_CSELECT_OUT;
        }
        if ((returned_events & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
          action |= CURL_CSELECT_ERR;
        }
        code = curl_multi_socket_action(multi_handle_, descriptors[index].fd, action, running_handles);
      }
    }
    return code;
  }

 private:
  static int HandleSocket(CURL* easy_handle, curl_socket_t socket, int action, void* user_data, void* socket_data) {
    (void)easy_handle;
    (void)socket_data;
    auto* loop = static_cast<CurlMultiSocketLoop*>(user_data);
    if (action == CURL_POLL_REMOVE) {
      loop->sockets_.erase(socket);
      return 0;
    }
    short events = 0;
    if (action == CURL_POLL_IN || action == CURL_POLL_INOUT) {
      events |= POLLIN;
    }
    if (action == CURL_POLL_OUT || action == CURL_POLL_INOUT) {
      events |= POLLOUT;
    }
    loop->sockets_[socket] = events;
    return 0;
  }

  static int HandleTimer(CURLM* multi_handle, long timeout_ms, void* user_data) {
    (void)multi_handle;
    auto* loop = static_cast<CurlMultiSocketLoop*>(user_data);
    if (timeout_ms < 0) {
      loop->timer_deadline_.reset();
    } else {
      loop->timer_deadline_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    }
    return 0;
  }

  [[nodiscard]] int TimerWaitMilliseconds() const {
    if (!timer_deadline_.has_value()) {
      return -1;
    }
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(timer_deadline_.value() -
                                                                                 std::chrono::steady_clock::now());
    if (remaining.count() <= 0) {
      return 0;
    }
    return static_cast<int>(std::min<long long>(remaining.count(), std::numeric_limits<int>::max()));
  }

  CURLM* multi_handle_;
  std::array<int, 2> wakeup_pipe_{};
  std::unordered_map<curl_socket_t, short> sockets_;
  std::optional<std::chrono::steady_clock::time_point> timer_deadline_;
};

class CurlMultiSocketRegistration final {
 public:
  explicit CurlMultiSocketRegistration(std::shared_ptr<CurlMultiSocketLoop> loop,
                                       const std::function<void(const std::function<void()>&)>& register_cancellation)
      : loop_(std::move(loop)) {
    register_cancellation([weak_loop = std::weak_ptr<CurlMultiSocketLoop>(loop_)] {
      if (const auto loop = weak_loop.lock()) {
        loop->Wake();
      }
    });
  }

  CurlMultiSocketRegistration(const CurlMultiSocketRegistration&) = delete;
  CurlMultiSocketRegistration& operator=(const CurlMultiSocketRegistration&) = delete;

 private:
  std::shared_ptr<CurlMultiSocketLoop> loop_;
};
#endif

std::string BuildCurlErrorMessage(std::string_view prefix, CURLcode code, std::string_view detail) {
  std::ostringstream message;
  message << prefix << ": " << curl_easy_strerror(code);
  if (!detail.empty()) {
    message << " (" << detail << ')';
  }
  return message.str();
}

std::string BuildCurlMultiErrorMessage(std::string_view prefix, CURLMcode code) {
  std::ostringstream message;
  message << prefix << ": " << curl_multi_strerror(code);
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

core::Result<std::unique_ptr<detail::StreamSlot>> AcquireStreamSlot(detail::ClientState& state) {
  {
    std::lock_guard lock(state.stream_mutex);
    if (!state.idle_stream_slots.empty()) {
      auto slot = std::move(state.idle_stream_slots.back());
      state.idle_stream_slots.pop_back();
      return slot;
    }
  }
  auto slot = std::make_unique<detail::StreamSlot>();
  slot->easy_handle = curl_easy_init();
  slot->multi_handle = curl_multi_init();
  if (slot->easy_handle == nullptr || slot->multi_handle == nullptr) {
    return core::Error::Internal(std::string(kCurlMultiInitFailureMessage));
  }
  return slot;
}

void ReleaseStreamSlot(detail::ClientState& state, std::unique_ptr<detail::StreamSlot> slot) {
  std::lock_guard lock(state.stream_mutex);
  if (state.idle_stream_slots.size() < kMaxIdleStreamSlots) {
    state.idle_stream_slots.push_back(std::move(slot));
  }
}

core::Result<void> PerformLegacyStreamingTransfer(CURLM* multi_handle, const std::function<bool()>& is_cancelled) {
  int running_handles = 0;
  CURLMcode multi_code = curl_multi_perform(multi_handle, &running_handles);
  while (multi_code == CURLM_OK && running_handles > 0) {
    if (is_cancelled()) {
      return {};
    }
    int ready_descriptors = 0;
    multi_code = curl_multi_poll(multi_handle, nullptr, 0, kLegacyStreamPollTimeoutMs, &ready_descriptors);
    if (multi_code == CURLM_OK) {
      multi_code = curl_multi_perform(multi_handle, &running_handles);
    }
  }
  if (multi_code != CURLM_OK) {
    return core::Error::Network(BuildCurlMultiErrorMessage(kCurlMultiFailureMessage, multi_code));
  }
  return {};
}

core::Result<CURLcode> PerformStreamingTransfer(
    CURL* easy_handle, CURLM* multi_handle, const std::function<bool()>& is_cancelled,
    const std::function<void(const std::function<void()>&)>& register_cancellation) {
  if (is_cancelled()) {
    return CURLE_ABORTED_BY_CALLBACK;
  }

#ifndef _WIN32
  std::shared_ptr<CurlMultiSocketLoop> socket_loop;
  if (register_cancellation) {
    socket_loop = std::make_shared<CurlMultiSocketLoop>(multi_handle);
    const auto configured = socket_loop->Configure();
    if (!configured.ok()) {
      return configured.error();
    }
  }
#endif

  const CURLMcode add_code = curl_multi_add_handle(multi_handle, easy_handle);
  if (add_code != CURLM_OK) {
    return core::Error::Internal(BuildCurlMultiErrorMessage(kCurlMultiFailureMessage, add_code));
  }
  [[maybe_unused]] const CurlMultiAttachment attachment(multi_handle, easy_handle);
#ifndef _WIN32
  std::optional<CurlMultiSocketRegistration> socket_registration;
  if (socket_loop != nullptr) {
    socket_registration.emplace(socket_loop, register_cancellation);
    int running_handles = 0;
    const CURLMcode socket_code = socket_loop->Run(&running_handles, is_cancelled);
    if (is_cancelled()) {
      return CURLE_ABORTED_BY_CALLBACK;
    }
    if (socket_code != CURLM_OK) {
      return core::Error::Network(BuildCurlMultiErrorMessage(kCurlMultiFailureMessage, socket_code));
    }
  } else {
#endif
    const auto legacy_transfer = PerformLegacyStreamingTransfer(multi_handle, is_cancelled);
    if (!legacy_transfer.ok()) {
      return legacy_transfer.error();
    }
    if (is_cancelled()) {
      return CURLE_ABORTED_BY_CALLBACK;
    }
#ifndef _WIN32
  }
#endif

  int pending_messages = 0;
  while (CURLMsg* message = curl_multi_info_read(multi_handle, &pending_messages)) {
    if (message->msg == CURLMSG_DONE && message->easy_handle == easy_handle) {
      return message->data.result;
    }
  }
  return core::Error::Internal(std::string(kCurlMultiCompletionFailureMessage));
}

}  // namespace

bool IsSupportedHttpVersion(std::string_view http_version) noexcept {
  return http_version == core::http::kHttpVersion11 || http_version == core::http::kHttpVersion20 ||
         http_version == core::http::kHttpVersion30;
}

Client::Client() : state_(std::make_shared<detail::ClientState>()) {
  state_->global_state = EnsureCurlGlobalInit();
  state_->handle = curl_easy_init();
}

core::Result<Response> Client::SendRequest(const Request& request) const {
  if (state_->global_state->code != CURLE_OK) {
    return core::Error::Internal(BuildCurlErrorMessage(kCurlInitFailureMessage, state_->global_state->code, {}));
  }

  auto headers = BuildHeaders(request.headers);
  if (!headers.ok()) {
    return headers.error();
  }

  std::lock_guard<std::mutex> lock(state_->mutex);
  if (state_->handle == nullptr) {
    return core::Error::Internal(std::string(kCurlInitFailureMessage));
  }
  CURL* const handle = state_->handle;
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

  const CURLcode code = curl_easy_perform(handle);
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
  return Response{.status_code = static_cast<int>(response_code),
                  .headers = std::move(response_headers),
                  .body = std::move(response_body)};
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
  auto stream_slot = std::move(acquired_slot.value());
  CURL* const handle = stream_slot->easy_handle;
  curl_easy_reset(handle);
  std::array<char, CURL_ERROR_SIZE> error_buffer{};
  const auto set_error_buffer = curl_easy_setopt(handle, CURLOPT_ERRORBUFFER, error_buffer.data());
  if (set_error_buffer != CURLE_OK) {
    return core::Error::Internal(BuildCurlErrorMessage(kErrorBufferFailureMessage, set_error_buffer, {}));
  }
  std::vector<Header> response_headers;
  detail::HeaderCapture header_capture{.response_headers = &response_headers};
  detail::StreamCallbackContext stream_context{.on_metadata = &on_metadata,
                                               .on_chunk = &on_chunk,
                                               .is_cancelled = &is_cancelled,
                                               .header_capture = &header_capture,
                                               .error = std::nullopt,
                                               .metadata_checked = false};
  const auto configured = ConfigureCurlStream(handle, request, headers.value(), &stream_context, &header_capture);
  if (!configured.ok()) {
    return configured.error();
  }
  StreamHeaderContext stream_header_context{.header_capture = &header_capture, .stream_context = &stream_context};
  const auto set_stream_header = curl_easy_setopt(handle, CURLOPT_HEADERFUNCTION, WriteStreamResponseHeader);
  const auto set_stream_header_data = curl_easy_setopt(handle, CURLOPT_HEADERDATA, &stream_header_context);
  if (set_stream_header != CURLE_OK || set_stream_header_data != CURLE_OK) {
    return core::Error::Internal(std::string(kConfigureRequestFailureMessage));
  }
  const auto performed =
      PerformStreamingTransfer(handle, stream_slot->multi_handle, is_cancelled, register_cancellation);
  if (!performed.ok()) {
    return performed.error();
  }
  const CURLcode code = performed.value();
  if (code != CURLE_OK) {
    if (stream_context.error.has_value()) {
      return stream_context.error.value();
    }
    if (is_cancelled()) {
      return core::Error::Network("HTTP stream was cancelled").WithTransport("http");
    }
    return core::Error::Network(BuildCurlErrorMessage(kRequestFailureMessage, code, error_buffer.data()));
  }
  if (!stream_context.metadata_checked) {
    const auto metadata = ValidateStreamMetadata(&stream_context);
    if (!metadata.ok()) {
      return metadata.error();
    }
    stream_context.metadata_checked = true;
  }
  long response_code = kHttpResponseCodeUnset;
  const CURLcode info_code = curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &response_code);
  if (info_code != CURLE_OK) {
    return core::Error::RemoteProtocol(BuildCurlErrorMessage(kReadStatusFailureMessage, info_code, {}));
  }
  if (response_code == kHttpResponseCodeUnset) {
    return core::Error::RemoteProtocol(std::string(kMalformedStatusMessage));
  }
  ReleaseStreamSlot(*state_, std::move(stream_slot));
  return Response{.status_code = static_cast<int>(response_code), .headers = std::move(response_headers), .body = {}};
}

#else

namespace {

constexpr std::string_view kLibcurlDisabledMessage =
    "default libcurl-backed HTTP support is disabled; rebuild with A2A_ENABLE_LIBCURL=ON and libcurl available or "
    "inject a custom requester/fetcher";

}  // namespace

Client::Client() = default;

core::Result<Response> Client::SendRequest(const Request& request) const {
  (void)request;
  return core::Error::Internal(std::string(kLibcurlDisabledMessage)).WithTransport("http");
}

core::Result<Response> Client::StreamRequest(const Request& request,
                                             const std::function<core::Result<void>(const Response&)>& on_metadata,
                                             const std::function<core::Result<void>(std::string_view)>& on_chunk,
                                             const std::function<bool()>& is_cancelled) const {
  (void)request;
  (void)on_metadata;
  (void)on_chunk;
  (void)is_cancelled;
  return core::Error::Internal(std::string(kLibcurlDisabledMessage)).WithTransport("http");
}

core::Result<Response> Client::StreamRequest(
    const Request& request, const std::function<core::Result<void>(const Response&)>& on_metadata,
    const std::function<core::Result<void>(std::string_view)>& on_chunk, const std::function<bool()>& is_cancelled,
    const std::function<void(const std::function<void()>&)>& register_cancellation) const {
  (void)register_cancellation;
  return StreamRequest(request, on_metadata, on_chunk, is_cancelled);
}

bool IsSupportedHttpVersion(std::string_view http_version) noexcept {
  return http_version == core::http::kHttpVersion11 || http_version == core::http::kHttpVersion20 ||
         http_version == core::http::kHttpVersion30;
}

#endif

}  // namespace a2a::http
