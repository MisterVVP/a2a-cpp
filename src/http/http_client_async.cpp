// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "a2a/http/http_client.h"

#if defined(A2A_HAS_LIBCURL)
#include <array>
#include <cstddef>
#include <memory>
#include <utility>

#include "a2a/core/non_copyable.h"
#include "http_client_internal.h"

namespace a2a::http {
namespace {
constexpr std::chrono::seconds kDefaultAsyncTimeout{30};
constexpr std::size_t kMaximumNativeRequests = 4096;
constexpr const char* kRequestCapacity = "HTTP async request capacity exceeded";
constexpr const char* kClientShutdown = "HTTP async client is shut down";
constexpr const char* kRequestFailure = "failed to execute asynchronous HTTP request";
constexpr const char* kConfigureFailure = "failed to configure asynchronous HTTP request";
constexpr const char* kResponseLimit = "asynchronous HTTP response size limit exceeded";
constexpr const char* kMalformedStatus = "HTTP server did not return a response status";
constexpr std::size_t kMaximumHeaderBytes = std::size_t{64U} * 1024U;
constexpr const char* kHeaderLimit = "asynchronous HTTP response header limit exceeded";
constexpr const char* kResponseStorageFailure = "failed to store asynchronous HTTP response";
constexpr std::size_t kMaximumResponseBytes = std::size_t{16U} * 1024U * 1024U;

struct AsyncRequest final : private core::NonCopyableOrMovable {
  AsyncRequest(std::shared_ptr<detail::ClientState> client, Request input, core::AsyncCompletion<Response> output)
      : client_state(std::move(client)), request(std::move(input)), completion(std::move(output)) {}
  ~AsyncRequest() {
    if (registered) {
      {
        std::lock_guard lock(client_state->stream_mutex);
        --client_state->active_unary_requests;
      }
      client_state->streams_finished.notify_all();
    }
  }
  core::Result<void> Register() {
    std::lock_guard lock(client_state->stream_mutex);
    if (client_state->shutting_down) {
      return core::Error::Network(kClientShutdown).WithProtocolCode(core::AsyncHandle::kCancelled);
    }
    if (client_state->active_unary_requests >= kMaximumNativeRequests) {
      return core::Error::Network(kRequestCapacity);
    }
    ++client_state->active_unary_requests;
    registered = true;
    return {};
  }
  bool registered = false;
  std::shared_ptr<detail::ClientState> client_state;
  Request request;
  core::AsyncCompletion<Response> completion;
  std::unique_ptr<detail::RequestSlot> slot;
  detail::CurlHeaderList headers;
  Response response;
  detail::HeaderCapture capture{.response_headers = &response.headers};
  std::array<char, CURL_ERROR_SIZE> error_buffer{};
  bool response_limit_exceeded = false;
  bool header_limit_exceeded = false;
  bool storage_failure = false;
  std::size_t header_bytes = 0;

  static std::size_t WriteBody(char* contents, std::size_t size, std::size_t count, void* data) noexcept {
    auto& state = *static_cast<AsyncRequest*>(data);
    const std::size_t bytes = size * count;
    if (bytes > kMaximumResponseBytes - state.response.body.size()) {
      state.response_limit_exceeded = true;
      return 0;
    }
    try {
      state.response.body.append(contents, bytes);
    } catch (...) {
      state.storage_failure = true;
      return 0;
    }
    return bytes;
  }
  static std::size_t WriteHeader(char* contents, std::size_t size, std::size_t count, void* data) noexcept {
    auto& state = *static_cast<AsyncRequest*>(data);
    const auto bytes = size * count;
    if (bytes > kMaximumHeaderBytes - state.header_bytes) {
      state.header_limit_exceeded = true;
      return 0;
    }
    state.header_bytes += bytes;
    try {
      return detail::CaptureResponseHeader(contents, size, count, &state.capture);
    } catch (...) {
      state.storage_failure = true;
      return 0;
    }
  }
  core::Result<void> Configure() {
    auto built_headers = detail::BuildHeaders(request.headers);
    if (!built_headers.ok()) {
      return built_headers.error();
    }
    headers = std::move(built_headers.value());
    auto acquired = detail::AcquireRequestSlot(*client_state);
    if (!acquired.ok()) {
      return acquired.error();
    }
    slot = std::move(acquired.value());
    auto* const easy = slot->easy_handle;
    curl_easy_reset(easy);
    const auto configured = detail::ConfigureCurl(easy, request, headers, &response.body, &capture);
    if (!configured.ok()) {
      return configured.error();
    }
    if (curl_easy_setopt(easy, CURLOPT_ERRORBUFFER, error_buffer.data()) != CURLE_OK ||
        curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, WriteBody) != CURLE_OK ||
        curl_easy_setopt(easy, CURLOPT_WRITEDATA, this) != CURLE_OK ||
        curl_easy_setopt(easy, CURLOPT_HEADERFUNCTION, WriteHeader) != CURLE_OK ||
        curl_easy_setopt(easy, CURLOPT_HEADERDATA, this) != CURLE_OK) {
      return core::Error::Internal(kConfigureFailure);
    }
    return {};
  }
  core::Result<Response> Result(CURLcode code) {
    if (storage_failure) {
      return core::Error::Internal(kResponseStorageFailure);
    }
    if (header_limit_exceeded) {
      return core::Error::Network(kHeaderLimit);
    }
    if (response_limit_exceeded) {
      return core::Error::Network(kResponseLimit);
    }
    if (code == CURLE_OPERATION_TIMEDOUT) {
      return core::Error::Network(kRequestFailure).WithProtocolCode(core::AsyncHandle::kDeadlineExceeded);
    }
    if (code != CURLE_OK) {
      return core::Error::Network(detail::BuildCurlErrorMessage(kRequestFailure, code, error_buffer.data()));
    }
    if (capture.response_code == 0) {
      return core::Error::RemoteProtocol(kMalformedStatus);
    }
    response.status_code = static_cast<int>(capture.response_code);
    return std::move(response);
  }
};
}  // namespace

core::Result<core::AsyncHandle> Client::SendRequestAsync(Request request, core::Completion<Response> completion) const {
  const auto client_state = state_;
  if (completion) {
    // Keep client state alive through callback destruction. A canceled transfer
    // can otherwise release the last state on a reactor while a callback still
    // runs, making that reactor wait for application code during scope teardown.
    completion = [lifetime = client_state, callback = std::move(completion)](core::Result<Response> response) mutable {
      (void)lifetime;
      callback(std::move(response));
    };
  }
  auto started = client_state->unary_scope.Start<Response>(
      std::move(completion), request.timeout.count() != 0 ? request.timeout : kDefaultAsyncTimeout);
  if (!started.ok()) {
    return started.error();
  }
  auto token = std::move(started.value());
  if (!token.IsActive()) {
    return token.handle();
  }
  auto operation = std::make_shared<AsyncRequest>(client_state, std::move(request), token);
  const auto registered = operation->Register();
  if (!registered.ok()) {
    token.Complete(registered.error());
    return token.handle();
  }
  const auto configured = operation->Configure();
  if (!configured.ok()) {
    token.Complete(configured.error());
    return token.handle();
  }
  auto transfer = std::make_shared<detail::CurlStreamReactor::Transfer>();
  transfer->easy_handle = operation->slot->easy_handle;
  transfer->owner = client_state.get();
  transfer->lifetime = client_state;
  transfer->on_complete = [operation](CURLcode code) {
    auto result = operation->Result(code);
    detail::ReleaseRequestSlot(*operation->client_state, std::move(operation->slot));
    operation->completion.Complete(std::move(result));
  };
  auto reactor = operation->slot->reactor;
  // Add precedes cancellation registration, including cancellation before start.
  reactor->Add(transfer);
  token.RegisterCancellation([reactor, weak_transfer = std::weak_ptr(transfer)] {
    if (auto active = weak_transfer.lock()) {
      reactor->Cancel(active);
    }
  });
  return token.handle();
}
}  // namespace a2a::http
#else
namespace a2a::http {
namespace {
constexpr const char* kDisabled = "asynchronous HTTP requires libcurl support";
}
core::Result<core::AsyncHandle> Client::SendRequestAsync(Request request, core::Completion<Response> completion) const {
  (void)request;
  (void)completion;
  return core::Error::Validation(kDisabled);
}
}  // namespace a2a::http
#endif
