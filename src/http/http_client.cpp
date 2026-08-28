// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "a2a/http/http_client.h"

#include <string>
#include <string_view>

#include "a2a/core/error.h"

#if defined(A2A_HAS_LIBCURL)
#include <curl/curl.h>

#if defined(__linux__)
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include <cerrno>
#endif

#include <array>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <utility>
#endif

namespace a2a::http {

#if defined(A2A_HAS_LIBCURL)

namespace detail {
class CurlStreamReactor;

struct ClientGlobalState final {
  ClientGlobalState() : code(curl_global_init(CURL_GLOBAL_DEFAULT)) {}

  CURLcode code = CURLE_OK;
};

struct StreamSlot final {
  ~StreamSlot() {
    if (easy_handle != nullptr) {
      curl_easy_cleanup(easy_handle);
    }
  }

  CURL* easy_handle = nullptr;
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
  std::mutex stream_reactor_mutex;
  std::shared_ptr<CurlStreamReactor> stream_reactor;
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
constexpr char kHeaderSeparator = ':';
constexpr long kHttpResponseCodeUnset = 0;
constexpr long kHttpInformationalStatusMin = 100;
constexpr long kHttpInformationalStatusMax = 199;
constexpr std::size_t kMaxIdleStreamSlots = 64U;
constexpr std::size_t kMaximumReactorEvents = 64U;

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

class CurlStreamReactor final : public std::enable_shared_from_this<CurlStreamReactor> {
 public:
  struct Transfer final {
    CURL* easy_handle = nullptr;
    std::mutex mutex;
    std::condition_variable completed;
    CURLcode result = CURLE_ABORTED_BY_CALLBACK;
    bool done = false;
  };

  static std::shared_ptr<CurlStreamReactor> Create() {
    auto reactor = std::shared_ptr<CurlStreamReactor>(new CurlStreamReactor());
    if (!reactor->valid_) {
      return {};
    }
    reactor->thread_ = std::thread([reactor_pointer = reactor.get()] { reactor_pointer->Run(); });
    return reactor;
  }

  ~CurlStreamReactor() {
    Enqueue(Command{.type = CommandType::kShutdown, .transfer = {}});
    if (thread_.joinable()) {
      thread_.join();
    }
#if defined(__linux__)
    CloseDescriptor(timer_descriptor_);
    CloseDescriptor(wakeup_descriptor_);
    CloseDescriptor(epoll_descriptor_);
#endif
    if (multi_handle_ != nullptr) {
      curl_multi_cleanup(multi_handle_);
    }
  }

  CurlStreamReactor(const CurlStreamReactor&) = delete;
  CurlStreamReactor& operator=(const CurlStreamReactor&) = delete;

  void Add(const std::shared_ptr<Transfer>& transfer) {
    Enqueue(Command{.type = CommandType::kAdd, .transfer = transfer});
  }

  void Cancel(const std::shared_ptr<Transfer>& transfer) {
    Enqueue(Command{.type = CommandType::kCancel, .transfer = transfer});
  }

 private:
  enum class CommandType : std::uint8_t { kAdd, kCancel, kShutdown };
  struct Command final {
    CommandType type = CommandType::kShutdown;
    std::shared_ptr<Transfer> transfer;
  };

  CurlStreamReactor() : multi_handle_(curl_multi_init()) {
    if (multi_handle_ == nullptr) {
      return;
    }
#if defined(__linux__)
    epoll_descriptor_ = epoll_create1(EPOLL_CLOEXEC);
    wakeup_descriptor_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    timer_descriptor_ = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    valid_ = epoll_descriptor_ >= 0 && wakeup_descriptor_ >= 0 && timer_descriptor_ >= 0 &&
             AddInternalDescriptor(wakeup_descriptor_) && AddInternalDescriptor(timer_descriptor_);
#else
    valid_ = true;
#endif
    valid_ = valid_ && curl_multi_setopt(multi_handle_, CURLMOPT_SOCKETFUNCTION, &HandleSocket) == CURLM_OK &&
             curl_multi_setopt(multi_handle_, CURLMOPT_SOCKETDATA, this) == CURLM_OK &&
             curl_multi_setopt(multi_handle_, CURLMOPT_TIMERFUNCTION, &HandleTimer) == CURLM_OK &&
             curl_multi_setopt(multi_handle_, CURLMOPT_TIMERDATA, this) == CURLM_OK;
  }

  void Enqueue(Command command) {
    {
      std::lock_guard lock(command_mutex_);
      if (shutdown_enqueued_ && command.type != CommandType::kShutdown) {
        Complete(command.transfer, CURLE_ABORTED_BY_CALLBACK);
        return;
      }
      shutdown_enqueued_ = shutdown_enqueued_ || command.type == CommandType::kShutdown;
      commands_.push_back(std::move(command));
    }
    Wake();
  }

  void Wake() const noexcept {
#if defined(__linux__)
    constexpr std::uint64_t kWakeValue = 1;
    ssize_t result = 0;
    do {
      result = write(wakeup_descriptor_, &kWakeValue, sizeof(kWakeValue));
    } while (result < 0 && errno == EINTR);
#else
    (void)curl_multi_wakeup(multi_handle_);
#endif
  }

  void Run() {
    while (!shutting_down_) {
      ProcessCommands();
      if (shutting_down_) {
        break;
      }
#if defined(__linux__)
      WaitForLinuxEvents();
#else
      WaitForPortableEvents();
#endif
      ReadCompletions();
    }
    for (auto& [easy_handle, transfer] : transfers_) {
      (void)curl_multi_remove_handle(multi_handle_, easy_handle);
      Complete(transfer, CURLE_ABORTED_BY_CALLBACK);
    }
    transfers_.clear();
  }

  void ProcessCommands() {
    std::deque<Command> commands;
    {
      std::lock_guard lock(command_mutex_);
      commands.swap(commands_);
    }
    for (auto& command : commands) {
      if (command.type == CommandType::kShutdown) {
        shutting_down_ = true;
      } else if (command.type == CommandType::kCancel) {
        Remove(command.transfer, CURLE_ABORTED_BY_CALLBACK);
      } else {
        AddOnReactor(std::move(command.transfer));
      }
    }
  }

  void AddOnReactor(std::shared_ptr<Transfer> transfer) {
    if (shutting_down_) {
      Complete(transfer, CURLE_ABORTED_BY_CALLBACK);
      return;
    }
    const CURLMcode code = curl_multi_add_handle(multi_handle_, transfer->easy_handle);
    if (code != CURLM_OK) {
      Complete(transfer, CURLE_FAILED_INIT);
      return;
    }
    transfers_.emplace(transfer->easy_handle, std::move(transfer));
    int running_handles = 0;
    const CURLMcode action_code = curl_multi_socket_action(multi_handle_, CURL_SOCKET_TIMEOUT, 0, &running_handles);
    if (action_code != CURLM_OK) {
      FailAll(CURLE_RECV_ERROR);
    }
    ReadCompletions();
  }

  void Remove(const std::shared_ptr<Transfer>& transfer, CURLcode result) {
    if (transfer == nullptr) {
      return;
    }
    const auto found = transfers_.find(transfer->easy_handle);
    if (found != transfers_.end() && found->second == transfer) {
      (void)curl_multi_remove_handle(multi_handle_, transfer->easy_handle);
      transfers_.erase(found);
    }
    Complete(transfer, result);
  }

  static void Complete(const std::shared_ptr<Transfer>& transfer, CURLcode result) {
    if (transfer == nullptr) {
      return;
    }
    {
      std::lock_guard lock(transfer->mutex);
      if (transfer->done) {
        return;
      }
      transfer->result = result;
      transfer->done = true;
    }
    transfer->completed.notify_all();
  }

  void ReadCompletions() {
    int pending = 0;
    while (CURLMsg* message = curl_multi_info_read(multi_handle_, &pending)) {
      if (message->msg != CURLMSG_DONE) {
        continue;
      }
      const auto found = transfers_.find(message->easy_handle);
      if (found == transfers_.end()) {
        continue;
      }
      auto transfer = std::move(found->second);
      transfers_.erase(found);
      (void)curl_multi_remove_handle(multi_handle_, message->easy_handle);
      Complete(transfer, message->data.result);
    }
  }

  void FailAll(CURLcode result) {
    for (auto& [easy_handle, transfer] : transfers_) {
      (void)curl_multi_remove_handle(multi_handle_, easy_handle);
      Complete(transfer, result);
    }
    transfers_.clear();
  }

#if defined(__linux__)
  static void CloseDescriptor(int descriptor) noexcept {
    if (descriptor >= 0) {
      (void)close(descriptor);
    }
  }

  bool AddInternalDescriptor(int descriptor) const {
    epoll_event event{.events = EPOLLIN, .data = {.fd = descriptor }};
    return epoll_ctl(epoll_descriptor_, EPOLL_CTL_ADD, descriptor, &event) == 0;
  }

  void WaitForLinuxEvents() {
    std::array<epoll_event, kMaximumReactorEvents> events{};
    int count = 0;
    do {
      count = epoll_wait(epoll_descriptor_, events.data(), static_cast<int>(events.size()), -1);
    } while (count < 0 && errno == EINTR);
    if (count < 0) {
      FailAll(CURLE_RECV_ERROR);
      shutting_down_ = true;
      return;
    }
    for (int index = 0; index < count; ++index) {
      const int descriptor = events[static_cast<std::size_t>(index)].data.fd;
      if (descriptor == wakeup_descriptor_) {
        DrainEventDescriptor(wakeup_descriptor_);
        ProcessCommands();
      } else if (descriptor == timer_descriptor_) {
        DrainEventDescriptor(timer_descriptor_);
        int running_handles = 0;
        const CURLMcode code = curl_multi_socket_action(multi_handle_, CURL_SOCKET_TIMEOUT, 0, &running_handles);
        if (code != CURLM_OK) {
          FailAll(CURLE_RECV_ERROR);
        }
      } else {
        ProcessSocket(descriptor, events[static_cast<std::size_t>(index)].events);
      }
    }
  }

  static void DrainEventDescriptor(int descriptor) noexcept {
    std::uint64_t value = 0;
    while (read(descriptor, &value, sizeof(value)) > 0) {
    }
  }

  void ProcessSocket(int descriptor, std::uint32_t events) {
    int action = 0;
    action |= (events & EPOLLIN) != 0U ? CURL_CSELECT_IN : 0;
    action |= (events & EPOLLOUT) != 0U ? CURL_CSELECT_OUT : 0;
    action |= (events & (EPOLLERR | EPOLLHUP)) != 0U ? CURL_CSELECT_ERR : 0;
    int running_handles = 0;
    const CURLMcode code = curl_multi_socket_action(multi_handle_, descriptor, action, &running_handles);
    if (code != CURLM_OK) {
      FailAll(CURLE_RECV_ERROR);
    }
  }

  static int HandleSocket(CURL* easy_handle, curl_socket_t socket, int action, void* user_data, void* socket_data) {
    (void)easy_handle;
    (void)socket_data;
    auto* reactor = static_cast<CurlStreamReactor*>(user_data);
    if (action == CURL_POLL_REMOVE) {
      if (epoll_ctl(reactor->epoll_descriptor_, EPOLL_CTL_DEL, socket, nullptr) != 0 && errno != ENOENT &&
          errno != EBADF) {
        reactor->FailAll(CURLE_RECV_ERROR);
      }
      return 0;
    }
    std::uint32_t events = EPOLLERR | EPOLLHUP;
    events |= action == CURL_POLL_IN || action == CURL_POLL_INOUT ? EPOLLIN : 0U;
    events |= action == CURL_POLL_OUT || action == CURL_POLL_INOUT ? EPOLLOUT : 0U;
    epoll_event event{.events = events, .data = {.fd = socket }};
    if (epoll_ctl(reactor->epoll_descriptor_, EPOLL_CTL_MOD, socket, &event) != 0 &&
        (errno != ENOENT || epoll_ctl(reactor->epoll_descriptor_, EPOLL_CTL_ADD, socket, &event) != 0)) {
      reactor->FailAll(CURLE_RECV_ERROR);
    }
    return 0;
  }

  static int HandleTimer(CURLM* multi_handle, long timeout_ms, void* user_data) {
    (void)multi_handle;
    auto* reactor = static_cast<CurlStreamReactor*>(user_data);
    itimerspec timer{};
    if (timeout_ms >= 0) {
      constexpr long kMillisecondsPerSecond = 1000L;
      constexpr long kNanosecondsPerMillisecond = 1000000L;
      timer.it_value.tv_sec = timeout_ms / kMillisecondsPerSecond;
      timer.it_value.tv_nsec = (timeout_ms % kMillisecondsPerSecond) * kNanosecondsPerMillisecond;
      if (timeout_ms == 0) {
        timer.it_value.tv_nsec = 1;
      }
    }
    if (timerfd_settime(reactor->timer_descriptor_, 0, &timer, nullptr) != 0) {
      reactor->FailAll(CURLE_RECV_ERROR);
    }
    return 0;
  }
#else
  void WaitForPortableEvents() {
    int ready = 0;
    const CURLMcode poll_code = curl_multi_poll(multi_handle_, nullptr, 0, -1, &ready);
    if (poll_code != CURLM_OK) {
      FailAll(CURLE_RECV_ERROR);
      return;
    }
    ProcessCommands();
    int running_handles = 0;
    if (curl_multi_perform(multi_handle_, &running_handles) != CURLM_OK) {
      FailAll(CURLE_RECV_ERROR);
    }
  }

  static int HandleSocket(CURL*, curl_socket_t, int, void*, void*) { return 0; }
  static int HandleTimer(CURLM*, long, void*) { return 0; }
#endif

  CURLM* multi_handle_ = nullptr;
  std::thread thread_;
  std::mutex command_mutex_;
  std::deque<Command> commands_;
  std::unordered_map<CURL*, std::shared_ptr<Transfer>> transfers_;
  bool valid_ = false;
  bool shutdown_enqueued_ = false;
  bool shutting_down_ = false;
#if defined(__linux__)
  int epoll_descriptor_ = -1;
  int wakeup_descriptor_ = -1;
  int timer_descriptor_ = -1;
#endif
};

}  // namespace detail

namespace {

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
  if (slot->easy_handle == nullptr) {
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

core::Result<CURLcode> PerformStreamingTransfer(
    const std::shared_ptr<detail::CurlStreamReactor>& reactor, CURL* easy_handle,
    const std::function<bool()>& is_cancelled,
    const std::function<void(const std::function<void()>&)>& register_cancellation) {
  if (reactor == nullptr) {
    return core::Error::Internal(std::string(kCurlMultiInitFailureMessage));
  }
  if (is_cancelled()) {
    return CURLE_ABORTED_BY_CALLBACK;
  }
  auto transfer = std::make_shared<detail::CurlStreamReactor::Transfer>();
  transfer->easy_handle = easy_handle;
  reactor->Add(transfer);
  if (register_cancellation) {
    register_cancellation([weak_reactor = std::weak_ptr<detail::CurlStreamReactor>(reactor),
                           weak_transfer = std::weak_ptr<detail::CurlStreamReactor::Transfer>(transfer)] {
      const auto locked_reactor = weak_reactor.lock();
      const auto locked_transfer = weak_transfer.lock();
      if (locked_reactor != nullptr && locked_transfer != nullptr) {
        locked_reactor->Cancel(locked_transfer);
      }
    });
  }
  std::unique_lock lock(transfer->mutex);
  transfer->completed.wait(lock, [&transfer] { return transfer->done; });
  return transfer->result;
}

std::shared_ptr<detail::CurlStreamReactor> GetStreamReactor(detail::ClientState& state) {
  std::lock_guard lock(state.stream_reactor_mutex);
  if (state.stream_reactor == nullptr) {
    state.stream_reactor = detail::CurlStreamReactor::Create();
  }
  return state.stream_reactor;
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
      PerformStreamingTransfer(GetStreamReactor(*state_), handle, is_cancelled, register_cancellation);
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
