// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "http_client_internal.h"

#if defined(A2A_HAS_LIBCURL)

#include <charconv>
#include <limits>
#include <sstream>
#include <system_error>

#include "a2a/core/http_constants.h"

namespace a2a::http::detail {
namespace {

constexpr char kHttpTransportName[] = "http";
constexpr std::string_view kCurlHeaderFailureMessage = "failed to build HTTP request headers";
constexpr std::string_view kCurlInitFailureMessage = "failed to initialize HTTP client";
constexpr std::string_view kCurlMultiInitFailureMessage = "failed to initialize HTTP stream poller";
constexpr std::string_view kConfigureRequestFailureMessage = "failed to configure HTTP request";
constexpr std::string_view kMissingStreamMetadataCallbackMessage = "HTTP stream metadata callback is required";
constexpr std::string_view kMissingStreamChunkCallbackMessage = "HTTP stream chunk callback is required";
constexpr std::string_view kMissingStreamCancellationCallbackMessage = "HTTP stream cancellation callback is required";
constexpr std::string_view kUnsupportedHttpVersionMessage = "HTTP client supports only HTTP/1.1, HTTP/2.0, or HTTP/3.0";
constexpr std::string_view kMalformedStatusMessage = "HTTP server did not return a response status";
constexpr std::string_view kClientShuttingDownMessage = "HTTP client is shutting down";
constexpr std::string_view kHttpStatusLinePrefix = "HTTP/";
constexpr char kHeaderSeparator = ':';
constexpr long kHttpResponseCodeUnset = 0;
constexpr long kHttpInformationalStatusMin = 100;
constexpr long kHttpInformationalStatusMax = 199;
constexpr std::size_t kMaxIdleRequestSlots = 64U;
constexpr std::size_t kMaxIdleStreamSlots = 64U;

std::shared_ptr<CurlStreamReactor> GetOrCreatePrimaryReactor(ClientState& state) {
  std::lock_guard lock(state.reactor_mutex);
  auto reactor = state.primary_reactor.lock();
  if (reactor == nullptr) {
    reactor = state.global_state->stream_reactor_pool->Acquire();
    state.primary_reactor = reactor;
  }
  return reactor;
}

}  // namespace

std::string BuildCurlErrorMessage(std::string_view prefix, CURLcode code, std::string_view detail) {
  std::ostringstream message;
  message << prefix << ": " << curl_easy_strerror(code);
  if (!detail.empty()) {
    message << " (" << detail << ')';
  }
  return message.str();
}

namespace {

core::Result<void> ValidateStreamCallbacks(const StreamCallbackContext& context) {
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

}  // namespace

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

namespace {

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

size_t WriteResponseHeader(char* contents, size_t size, size_t nmemb, void* user_data) {
  auto* capture = static_cast<HeaderCapture*>(user_data);
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

}  // namespace

core::Result<void> ValidateStreamMetadata(StreamCallbackContext* context) {
  const HeaderCapture& capture = *context->header_capture;
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
  HeaderCapture& capture = *context->header_capture;
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

namespace {

size_t WriteResponseBody(char* contents, size_t size, size_t nmemb, void* user_data) {
  auto* body = static_cast<std::string*>(user_data);
  const std::size_t byte_count = size * nmemb;
  body->append(contents, byte_count);
  return byte_count;
}

size_t WriteStreamBody(char* contents, size_t size, size_t nmemb, void* user_data) {
  auto* context = static_cast<StreamCallbackContext*>(user_data);
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
  auto* context = static_cast<StreamCallbackContext*>(clientp);
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

}  // namespace

core::Result<void> ConfigureCurl(CURL* handle, const Request& request, const CurlHeaderList& headers,
                                 std::string* response_body, HeaderCapture* response_headers) {
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
                                       StreamCallbackContext* stream_context, HeaderCapture* response_headers) {
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

core::Result<std::unique_ptr<RequestSlot>> AcquireRequestSlot(ClientState& state) {
  std::unique_ptr<RequestSlot> slot;
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
    slot = std::make_unique<RequestSlot>();
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

void ReleaseRequestSlot(ClientState& state, std::unique_ptr<RequestSlot> slot) {
  std::lock_guard lock(state.request_mutex);
  if (state.idle_request_slots.size() < kMaxIdleRequestSlots) {
    state.idle_request_slots.push_back(std::move(slot));
  }
}

core::Result<std::unique_ptr<StreamSlot>> AcquireStreamSlot(ClientState& state) {
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
  auto slot = std::make_unique<StreamSlot>();
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

void ReleaseStreamSlot(ClientState& state, std::unique_ptr<StreamSlot> slot) {
  std::lock_guard lock(state.stream_mutex);
  if (!state.shutting_down && state.idle_stream_slots.size() < kMaxIdleStreamSlots) {
    state.idle_stream_slots.push_back(std::move(slot));
  }
}

CURLcode PerformCurlTransfer(CURL* handle, const std::shared_ptr<CurlStreamReactor>& reactor) {
  auto transfer = std::make_shared<CurlStreamReactor::Transfer>();
  transfer->easy_handle = handle;
  reactor->Add(transfer);

  std::unique_lock lock(transfer->mutex);
  transfer->completed.wait(lock, [&transfer] { return transfer->done; });
  return transfer->result;
}

void CurlSlistDeleter::operator()(curl_slist* list) const noexcept {
  if (list != nullptr) {
    curl_slist_free_all(list);
  }
}

ClientGlobalState::ClientGlobalState()
    : code(curl_global_init(CURL_GLOBAL_DEFAULT)), stream_reactor_pool(std::make_shared<CurlStreamReactorPool>()) {}

RequestSlot::~RequestSlot() {
  if (easy_handle != nullptr) {
    curl_easy_cleanup(easy_handle);
  }
}

StreamSlot::~StreamSlot() {
  if (easy_handle != nullptr) {
    curl_easy_cleanup(easy_handle);
  }
}

}  // namespace a2a::http::detail

#endif
