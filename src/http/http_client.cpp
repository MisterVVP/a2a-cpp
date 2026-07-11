// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "a2a/http/http_client.h"

#include <string>
#include <string_view>

#include "a2a/core/error.h"

#if defined(A2A_HAS_LIBCURL)
#include <curl/curl.h>

#include <array>
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <utility>
#endif

namespace a2a::http {

#if defined(A2A_HAS_LIBCURL)

namespace detail {
struct ClientGlobalState final {
  ClientGlobalState() : code(curl_global_init(CURL_GLOBAL_DEFAULT)) {}

  CURLcode code = CURLE_OK;
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
};
}  // namespace detail

namespace {

constexpr std::string_view kCurlInitFailureMessage = "failed to initialize HTTP client";
constexpr std::string_view kCurlHeaderFailureMessage = "failed to build HTTP request headers";
constexpr std::string_view kConfigureRequestFailureMessage = "failed to configure HTTP request";
constexpr std::string_view kErrorBufferFailureMessage = "failed to configure HTTP client error buffer";
constexpr std::string_view kRequestFailureMessage = "failed to execute HTTP request";
constexpr std::string_view kReadStatusFailureMessage = "failed to read HTTP response status";
constexpr std::string_view kUnsupportedHttpVersionMessage = "HTTP client supports only HTTP/1.1, HTTP/2.0, or HTTP/3.0";
constexpr std::string_view kMalformedStatusMessage = "HTTP server did not return a response status";
constexpr std::string_view kHttpStatusLinePrefix = "HTTP/";
constexpr char kHeaderSeparator = ':';
constexpr long kHttpResponseCodeUnset = 0;

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

std::string BuildCurlErrorMessage(std::string_view prefix, CURLcode code, std::string_view detail) {
  std::ostringstream message;
  message << prefix << ": " << curl_easy_strerror(code);
  if (!detail.empty()) {
    message << " (" << detail << ')';
  }
  return message.str();
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

std::optional<Header> ParseHeaderLine(std::string_view line) {
  if (line.starts_with(kHttpStatusLinePrefix)) {
    return Header{};
  }

  const auto separator = line.find(kHeaderSeparator);
  if (separator == std::string_view::npos) {
    return std::nullopt;
  }

  std::string name(line.substr(0, separator));
  const std::string_view value = TrimHeaderValue(line.substr(separator + 1));
  return Header{.name = std::move(name), .value = std::string(value)};
}

size_t WriteResponseHeader(char* contents, size_t size, size_t nmemb, void* user_data) {
  auto* headers = static_cast<std::vector<Header>*>(user_data);
  const std::size_t byte_count = size * nmemb;
  const std::string_view line(contents, byte_count);
  const auto header = ParseHeaderLine(line);
  if (!header.has_value()) {
    return byte_count;
  }
  if (header->name.empty() && header->value.empty()) {
    headers->clear();
    return byte_count;
  }
  headers->push_back(header.value());
  return byte_count;
}

size_t WriteResponseBody(char* contents, size_t size, size_t nmemb, void* user_data) {
  auto* body = static_cast<std::string*>(user_data);
  const std::size_t byte_count = size * nmemb;
  body->append(contents, byte_count);
  return byte_count;
}

struct StreamCallbackContext final {
  const std::function<core::Result<void>(std::string_view)>* on_chunk = nullptr;
  const std::function<bool()>* is_cancelled = nullptr;
  std::optional<core::Error> error;
};

size_t WriteStreamBody(char* contents, size_t size, size_t nmemb, void* user_data) {
  auto* context = static_cast<StreamCallbackContext*>(user_data);
  const std::size_t byte_count = size * nmemb;
  if (context->is_cancelled != nullptr && (*context->is_cancelled)()) {
    return 0;
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

core::Result<void> ConfigureCurl(CURL* handle, const Request& request, const CurlHeaderList& headers,
                                 std::string* response_body, std::vector<Header>* response_headers) {
  const auto http_version = MapHttpVersion(request.http_version);
  if (!http_version.ok()) {
    return http_version.error();
  }

  const auto set_url = curl_easy_setopt(handle, CURLOPT_URL, request.url.c_str());
  const auto set_method = curl_easy_setopt(handle, CURLOPT_CUSTOMREQUEST, request.method.c_str());
  const auto set_headers = curl_easy_setopt(handle, CURLOPT_HTTPHEADER, headers.get());
  const auto set_body = curl_easy_setopt(handle, CURLOPT_POSTFIELDS, request.body.c_str());
  const auto set_body_size =
      curl_easy_setopt(handle, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(request.body.size()));
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
  if (set_url != CURLE_OK || set_method != CURLE_OK || set_headers != CURLE_OK || set_body != CURLE_OK ||
      set_body_size != CURLE_OK || set_timeout != CURLE_OK || set_connect_timeout != CURLE_OK ||
      set_no_signal != CURLE_OK || set_write != CURLE_OK || set_write_data != CURLE_OK || set_header != CURLE_OK ||
      set_header_data != CURLE_OK || set_http_version != CURLE_OK || set_tls_minimum != CURLE_OK) {
    return core::Error::Internal(std::string(kConfigureRequestFailureMessage));
  }
  return {};
}

core::Result<void> ConfigureCurlStream(CURL* handle, const Request& request, const CurlHeaderList& headers,
                                       StreamCallbackContext* stream_context, std::vector<Header>* response_headers) {
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
  const auto configured = ConfigureCurl(handle, request, headers.value(), &response_body, &response_headers);
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
                                             const std::function<core::Result<void>(std::string_view)>& on_chunk,
                                             const std::function<bool()>& is_cancelled) const {
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
  std::vector<Header> response_headers;
  StreamCallbackContext stream_context{.on_chunk = &on_chunk, .is_cancelled = &is_cancelled, .error = std::nullopt};
  const auto configured = ConfigureCurlStream(handle, request, headers.value(), &stream_context, &response_headers);
  if (!configured.ok()) {
    return configured.error();
  }
  const CURLcode code = curl_easy_perform(handle);
  if (code != CURLE_OK) {
    if (stream_context.error.has_value()) {
      return stream_context.error.value();
    }
    if (is_cancelled()) {
      return core::Error::Network("HTTP stream was cancelled").WithTransport("http");
    }
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
                                             const std::function<core::Result<void>(std::string_view)>& on_chunk,
                                             const std::function<bool()>& is_cancelled) const {
  (void)request;
  (void)on_chunk;
  (void)is_cancelled;
  return core::Error::Internal(std::string(kLibcurlDisabledMessage)).WithTransport("http");
}

bool IsSupportedHttpVersion(std::string_view http_version) noexcept {
  return http_version == core::http::kHttpVersion11 || http_version == core::http::kHttpVersion20 ||
         http_version == core::http::kHttpVersion30;
}

#endif

}  // namespace a2a::http
