// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "a2a/http/http_client.h"

#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "a2a/core/error.h"

#if defined(A2A_HAS_LIBCURL)
#include <curl/curl.h>

#include <array>
#include <atomic>

#include "http_client_internal.h"
#endif

namespace a2a::http {
#if defined(A2A_HAS_LIBCURL)

namespace {
constexpr std::string_view kCurlInitFailureMessage = "failed to initialize HTTP client";
constexpr std::string_view kErrorBufferFailureMessage = "failed to configure HTTP client error buffer";
constexpr std::string_view kRequestFailureMessage = "failed to execute HTTP request";
constexpr std::string_view kReadStatusFailureMessage = "failed to read HTTP response status";
constexpr std::string_view kMalformedStatusMessage = "HTTP server did not return a response status";
constexpr long kHttpResponseCodeUnset = 0;
}  // namespace

bool IsSupportedHttpVersion(std::string_view http_version) noexcept {
  return http_version == core::http::kHttpVersion11 || http_version == core::http::kHttpVersion20 ||
         http_version == core::http::kHttpVersion30;
}

Client::Client() : state_(std::make_shared<detail::ClientState>()) {
  state_->global_state = detail::EnsureCurlGlobalInit();
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
  if (detail::IsDispatchingStreamCallback(state_.get())) {
    return;
  }
  std::unique_lock lock(state_->stream_mutex);
  state_->streams_finished.wait(lock, [this] { return state_->active_streams == 0U; });
}

core::Result<Response> Client::SendRequest(const Request& request) const {
  if (state_->global_state->code != CURLE_OK) {
    return core::Error::Internal(
        detail::BuildCurlErrorMessage(kCurlInitFailureMessage, state_->global_state->code, {}));
  }

  auto headers = detail::BuildHeaders(request.headers);
  if (!headers.ok()) {
    return headers.error();
  }

  auto acquired_slot = detail::AcquireRequestSlot(*state_);
  if (!acquired_slot.ok()) {
    return acquired_slot.error();
  }
  auto slot = std::move(acquired_slot.value());
  CURL* const handle = slot->easy_handle;
  curl_easy_reset(handle);

  std::array<char, CURL_ERROR_SIZE> error_buffer{};
  const auto set_error_buffer = curl_easy_setopt(handle, CURLOPT_ERRORBUFFER, error_buffer.data());
  if (set_error_buffer != CURLE_OK) {
    return core::Error::Internal(detail::BuildCurlErrorMessage(kErrorBufferFailureMessage, set_error_buffer, {}));
  }

  std::string response_body;
  std::vector<Header> response_headers;
  detail::HeaderCapture header_capture{.response_headers = &response_headers};
  const auto configured = detail::ConfigureCurl(handle, request, headers.value(), &response_body, &header_capture);
  if (!configured.ok()) {
    return configured.error();
  }

  const CURLcode code = detail::PerformCurlTransfer(handle, slot->reactor);
  if (code != CURLE_OK) {
    return core::Error::Network(detail::BuildCurlErrorMessage(kRequestFailureMessage, code, error_buffer.data()));
  }

  long response_code = kHttpResponseCodeUnset;
  const CURLcode info_code = curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &response_code);
  if (info_code != CURLE_OK) {
    return core::Error::RemoteProtocol(detail::BuildCurlErrorMessage(kReadStatusFailureMessage, info_code, {}));
  }
  if (response_code == kHttpResponseCodeUnset) {
    return core::Error::RemoteProtocol(std::string(kMalformedStatusMessage));
  }
  Response response{.status_code = static_cast<int>(response_code),
                    .headers = std::move(response_headers),
                    .body = std::move(response_body)};
  detail::ReleaseRequestSlot(*state_, std::move(slot));
  return response;
}

#else

namespace {

constexpr char kHttpTransportName[] = "http";
constexpr std::string_view kStreamCompletionPendingMessage = "HTTP stream completion is pending";
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
