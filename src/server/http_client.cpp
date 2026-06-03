// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "a2a/server/http_client.h"

#include <curl/curl.h>

#include <array>
#include <cstddef>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

#include "a2a/core/error.h"

namespace a2a::server {

namespace detail {
struct HttpClientGlobalState final {
  HttpClientGlobalState() : code(curl_global_init(CURL_GLOBAL_DEFAULT)) {}

  CURLcode code = CURLE_OK;
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
constexpr long kHttpResponseCodeUnset = 0;

std::shared_ptr<const detail::HttpClientGlobalState> EnsureCurlGlobalInit() {
  static const auto init = std::make_shared<detail::HttpClientGlobalState>();
  return init;
}

struct CurlEasyDeleter final {
  void operator()(CURL* handle) const noexcept {
    if (handle != nullptr) {
      curl_easy_cleanup(handle);
    }
  }
};

struct CurlSlistDeleter final {
  void operator()(curl_slist* list) const noexcept {
    if (list != nullptr) {
      curl_slist_free_all(list);
    }
  }
};

using CurlEasyHandle = std::unique_ptr<CURL, CurlEasyDeleter>;
using CurlHeaderList = std::unique_ptr<curl_slist, CurlSlistDeleter>;

std::string BuildCurlErrorMessage(std::string_view prefix, CURLcode code, std::string_view detail) {
  std::ostringstream message;
  message << prefix << ": " << curl_easy_strerror(code);
  if (!detail.empty()) {
    message << " (" << detail << ')';
  }
  return message.str();
}

std::string BuildHeaderValue(const HttpClientHeader& header) {
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

core::Result<CurlHeaderList> BuildHeaders(const std::vector<HttpClientHeader>& headers) {
  CurlHeaderList list;
  for (const auto& header : headers) {
    const auto appended = AppendHeader(&list, BuildHeaderValue(header));
    if (!appended.ok()) {
      return appended.error();
    }
  }
  return list;
}

size_t WriteResponseBody(char* contents, size_t size, size_t nmemb, void* user_data) {
  auto* body = static_cast<std::string*>(user_data);
  const std::size_t byte_count = size * nmemb;
  body->append(contents, byte_count);
  return byte_count;
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

core::Result<void> ConfigureCurl(CURL* handle, const HttpClientRequest& request, const CurlHeaderList& headers,
                                 std::string* response_body) {
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
  const auto set_http_version = curl_easy_setopt(handle, CURLOPT_HTTP_VERSION, http_version.value());
  const auto set_tls_minimum = curl_easy_setopt(handle, CURLOPT_SSLVERSION, CURL_SSLVERSION_TLSv1_2);
  if (set_url != CURLE_OK || set_method != CURLE_OK || set_headers != CURLE_OK || set_body != CURLE_OK ||
      set_body_size != CURLE_OK || set_timeout != CURLE_OK || set_connect_timeout != CURLE_OK ||
      set_no_signal != CURLE_OK || set_write != CURLE_OK || set_write_data != CURLE_OK ||
      set_http_version != CURLE_OK || set_tls_minimum != CURLE_OK) {
    return core::Error::Internal(std::string(kConfigureRequestFailureMessage));
  }
  return {};
}

}  // namespace

bool IsSupportedHttpVersion(std::string_view http_version) noexcept {
  return http_version == core::http::kHttpVersion11 || http_version == core::http::kHttpVersion20 ||
         http_version == core::http::kHttpVersion30;
}

HttpClient::HttpClient() : global_state_(EnsureCurlGlobalInit()) {}

core::Result<HttpClientResponse> HttpClient::SendRequest(const HttpClientRequest& request) const {
  if (global_state_->code != CURLE_OK) {
    return core::Error::Internal(BuildCurlErrorMessage(kCurlInitFailureMessage, global_state_->code, {}));
  }

  CurlEasyHandle handle(curl_easy_init());
  if (handle == nullptr) {
    return core::Error::Internal(std::string(kCurlInitFailureMessage));
  }

  auto headers = BuildHeaders(request.headers);
  if (!headers.ok()) {
    return headers.error();
  }

  std::array<char, CURL_ERROR_SIZE> error_buffer{};
  const auto set_error_buffer = curl_easy_setopt(handle.get(), CURLOPT_ERRORBUFFER, error_buffer.data());
  if (set_error_buffer != CURLE_OK) {
    return core::Error::Internal(BuildCurlErrorMessage(kErrorBufferFailureMessage, set_error_buffer, {}));
  }

  std::string response_body;
  const auto configured = ConfigureCurl(handle.get(), request, headers.value(), &response_body);
  if (!configured.ok()) {
    return configured.error();
  }

  const CURLcode code = curl_easy_perform(handle.get());
  if (code != CURLE_OK) {
    return core::Error::Network(BuildCurlErrorMessage(kRequestFailureMessage, code, error_buffer.data()));
  }

  long response_code = kHttpResponseCodeUnset;
  const CURLcode info_code = curl_easy_getinfo(handle.get(), CURLINFO_RESPONSE_CODE, &response_code);
  if (info_code != CURLE_OK) {
    return core::Error::RemoteProtocol(BuildCurlErrorMessage(kReadStatusFailureMessage, info_code, {}));
  }
  if (response_code == kHttpResponseCodeUnset) {
    return core::Error::RemoteProtocol(std::string(kMalformedStatusMessage));
  }
  return HttpClientResponse{.status_code = static_cast<int>(response_code), .body = std::move(response_body)};
}

}  // namespace a2a::server
