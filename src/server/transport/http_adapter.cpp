// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "a2a/server/http_adapter.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "a2a/core/http_constants.h"
#include "a2a/core/string_utils.h"

namespace a2a::server {
namespace {
constexpr std::size_t kResponsePayloadReserveSlackBytes = 32;
constexpr std::size_t kResponsePayloadReserveSlackLineCount = 3;
constexpr std::size_t kChunkSizeBufferBytes = (sizeof(std::size_t) * 2U) + 1U;
constexpr std::string_view kFinalChunk = "0\r\n\r\n";

constexpr std::string_view kHeaderCountLimitExceeded = "HTTP header count limit exceeded";
constexpr std::string_view kRequestEof = "Unexpected end of stream while reading HTTP request";
constexpr std::string_view kInputLimitExceeded = "HTTP input buffer exceeds max_request_size";
constexpr std::string_view kHeaderSizeLimitExceeded = "HTTP header size limit exceeded";

struct RequestLine final {
  std::string method;
  std::string target;
};

std::string Trim(std::string_view value) {
  std::size_t start = 0;
  while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start])) != 0) {
    ++start;
  }
  std::size_t end = value.size();
  while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
    --end;
  }
  return std::string(value.substr(start, end - start));
}

[[nodiscard]] std::size_t ResponsePayloadReserveSize(const HttpServerResponse& response,
                                                     std::string_view reason_phrase) {
  std::size_t size = core::http::kHttpVersion11.size() + reason_phrase.size() + core::http::kLineTerminator.size() +
                     response.body.size() + core::http::kConnectionHeaderName.size() +
                     core::http::kConnectionCloseHeaderValue.size() + core::http::kContentLengthHeaderName.size() +
                     (core::http::kLineTerminator.size() * kResponsePayloadReserveSlackLineCount) +
                     kResponsePayloadReserveSlackBytes;
  for (const auto& [name, value] : response.headers) {
    size +=
        name.size() + value.size() + core::http::kLineTerminator.size() + core::http::kHeaderNameValueSeparator.size();
  }
  return size;
}

std::string NormalizeRequestTarget(std::string_view target) {
  if (target.empty()) {
    return "/";
  }
  const std::size_t scheme_separator = target.find("://");
  if (scheme_separator == std::string_view::npos) {
    return std::string(target);
  }
  const std::size_t authority_start = scheme_separator + 3;
  const std::size_t path_start = target.find('/', authority_start);
  if (path_start == std::string_view::npos) {
    return "/";
  }
  return std::string(target.substr(path_start));
}

core::Result<std::size_t> ParseContentLength(std::string_view value) {
  std::uint64_t parsed = 0;
  const std::string trimmed = Trim(value);
  if (trimmed.empty()) {
    return core::Error::Validation("Content-Length header is empty");
  }
  const auto [ptr, ec] = std::from_chars(trimmed.data(), trimmed.data() + trimmed.size(), parsed);
  if (ec != std::errc() || ptr != trimmed.data() + trimmed.size()) {
    return core::Error::Validation("Content-Length header is not a valid unsigned integer");
  }
  if (parsed > (std::numeric_limits<std::size_t>::max)()) {
    return core::Error::Validation("Content-Length value overflows platform size_t");
  }
  return static_cast<std::size_t>(parsed);
}

core::Result<RequestLine> ParseRequestLine(std::string_view header_block) {
  const std::size_t first_line_end = header_block.find(core::http::kLineTerminator);
  if (first_line_end == std::string_view::npos) {
    return core::Error::Validation("HTTP request line is missing");
  }

  std::istringstream request_line_parser(std::string(header_block.substr(0, first_line_end)));
  std::string method;
  std::string target;
  std::string version;
  request_line_parser >> method >> target >> version;
  if (method.empty() || target.empty() || version.empty()) {
    return core::Error::Validation("Malformed HTTP request line");
  }
  if (version != core::http::kHttpVersion11) {
    return core::Error::Validation("Unsupported HTTP version in request line");
  }

  return RequestLine{.method = std::move(method), .target = NormalizeRequestTarget(target)};
}

core::Result<void> ParseHeaderLine(std::string_view line, std::unordered_map<std::string, std::string>* headers,
                                   std::optional<std::size_t>* content_length) {
  const std::size_t colon = line.find(':');
  if (colon == std::string::npos || colon == 0) {
    return core::Error::Validation("Malformed HTTP header line");
  }

  const std::string name = Trim(line.substr(0, colon));
  const std::string value = Trim(line.substr(colon + 1));
  if (name.empty()) {
    return core::Error::Validation("HTTP header name cannot be empty");
  }

  if (core::strings::EqualsAsciiCaseInsensitive(name, core::http::kTransferEncodingHeader)) {
    return core::Error::Validation("Transfer-Encoding is not supported");
  }

  if (core::strings::EqualsAsciiCaseInsensitive(name, core::http::kContentLengthHeader)) {
    const auto parsed_length = ParseContentLength(value);
    if (!parsed_length.ok()) {
      return parsed_length.error();
    }
    if (content_length->has_value() && content_length->value() != parsed_length.value()) {
      return core::Error::Validation("Conflicting Content-Length header values");
    }
    *content_length = parsed_length.value();
  }

  if (core::strings::EqualsAsciiCaseInsensitive(name, core::http::kConnectionHeader)) {
    for (auto& [existing_name, existing_value] : *headers) {
      if (core::strings::EqualsAsciiCaseInsensitive(existing_name, core::http::kConnectionHeader)) {
        existing_value.append(", ");
        existing_value.append(value);
        return {};
      }
    }
  }

  headers->insert_or_assign(name, value);
  return {};
}

core::Result<std::optional<std::size_t>> ParseHeaders(std::string_view header_block,
                                                      std::unordered_map<std::string, std::string>* headers,
                                                      std::size_t max_header_count) {
  const std::size_t first_line_end = header_block.find(core::http::kLineTerminator);
  std::size_t offset = first_line_end + core::http::kLineTerminator.size();
  std::optional<std::size_t> content_length;
  std::size_t header_count = 0;

  while (offset < header_block.size()) {
    const std::size_t line_end = header_block.find(core::http::kLineTerminator, offset);
    const std::size_t next = (line_end == std::string::npos) ? header_block.size() : line_end;
    const std::string_view line = header_block.substr(offset, next - offset);
    if (!line.empty()) {
      if (++header_count > max_header_count) {
        return core::Error::Validation(std::string(kHeaderCountLimitExceeded));
      }
      const auto parsed = ParseHeaderLine(line, headers, &content_length);
      if (!parsed.ok()) {
        return parsed.error();
      }
    }
    if (line_end == std::string::npos) {
      break;
    }
    offset = line_end + core::http::kLineTerminator.size();
  }

  return content_length;
}

core::Result<void> WriteAll(HttpByteTransport& transport, std::string_view payload) {
  std::size_t sent = 0;
  while (sent < payload.size()) {
    const auto written = transport.Write(payload.data() + sent, payload.size() - sent);
    if (!written.ok()) {
      return written.error();
    }
    if (written.value() == 0) {
      return core::Error::Internal("Transport write returned zero bytes");
    }
    sent += written.value();
  }
  return {};
}

class ChunkedByteTransport final : public HttpByteTransport {
 public:
  explicit ChunkedByteTransport(HttpByteTransport& transport) : transport_(transport) {
    scratch_.reserve(kChunkSizeBufferBytes + (core::http::kLineTerminator.size() * 2U));
  }

  core::Result<std::size_t> Read(char* buffer, std::size_t size) override { return transport_.Read(buffer, size); }

  core::Result<std::size_t> Write(const char* buffer, std::size_t size) override {
    if (size == 0U) {
      return 0U;
    }
    std::array<char, kChunkSizeBufferBytes> chunk_size{};
    const auto converted = std::to_chars(chunk_size.data(), chunk_size.data() + chunk_size.size(), size, 16);
    if (converted.ec != std::errc{}) {
      return core::Error::Internal("Failed to encode HTTP chunk size");
    }
    const auto size_length = static_cast<std::size_t>(converted.ptr - chunk_size.data());
    scratch_.clear();
    scratch_.reserve(size_length + size + (core::http::kLineTerminator.size() * 2U));
    scratch_.append(chunk_size.data(), size_length);
    scratch_.append(core::http::kLineTerminator);
    scratch_.append(buffer, size);
    scratch_.append(core::http::kLineTerminator);
    const auto chunk_written = WriteAll(transport_, scratch_);
    if (!chunk_written.ok()) {
      return chunk_written.error();
    }
    return size;
  }

  core::Result<void> Finish() { return WriteAll(transport_, kFinalChunk); }

 private:
  HttpByteTransport& transport_;
  std::string scratch_;
};

bool HeaderContainsToken(const std::unordered_map<std::string, std::string>& headers, std::string_view header_name,
                         std::string_view expected_token) {
  for (const auto& [name, value] : headers) {
    if (!core::strings::EqualsAsciiCaseInsensitive(name, header_name)) {
      continue;
    }
    std::size_t offset = 0;
    while (offset <= value.size()) {
      const std::size_t separator = value.find(',', offset);
      const std::size_t end = separator == std::string::npos ? value.size() : separator;
      if (core::strings::EqualsAsciiCaseInsensitive(Trim(std::string_view(value).substr(offset, end - offset)),
                                                    expected_token)) {
        return true;
      }
      if (separator == std::string::npos) {
        break;
      }
      offset = separator + 1;
    }
  }
  return false;
}

core::Result<void> ValidateResponseContentLength(const HttpServerResponse& response, std::string_view value,
                                                 bool is_streaming) {
  if (is_streaming) {
    return core::Error::Validation("Streaming responses cannot set Content-Length");
  }
  const auto parsed_length = ParseContentLength(value);
  if (!parsed_length.ok()) {
    return core::Error::Validation("Response Content-Length header is invalid");
  }
  if (parsed_length.value() != response.body.size()) {
    return core::Error::Validation("Response Content-Length header does not match body size");
  }
  return {};
}

core::Result<bool> AppendResponseHeaders(const HttpServerResponse& response, bool is_streaming,
                                         bool must_close_connection, bool response_requests_close,
                                         std::string* payload) {
  bool has_content_length = false;
  for (const auto& [name, value] : response.headers) {
    if (core::strings::EqualsAsciiCaseInsensitive(name, core::http::kTransferEncodingHeader)) {
      return core::Error::Validation("Response Transfer-Encoding is owned by the HTTP adapter");
    }
    if (core::strings::EqualsAsciiCaseInsensitive(name, core::http::kContentLengthHeader)) {
      const auto validated = ValidateResponseContentLength(response, value, is_streaming);
      if (!validated.ok()) {
        return validated.error();
      }
      has_content_length = true;
    }
    if (core::strings::EqualsAsciiCaseInsensitive(name, core::http::kConnectionHeader) && must_close_connection &&
        !response_requests_close) {
      continue;
    }
    *payload += name;
    *payload += core::http::kHeaderNameValueSeparator;
    *payload += value;
    *payload += core::http::kLineTerminator;
  }
  return has_content_length;
}

}  // namespace

HttpAdapter::HttpAdapter() = default;
HttpAdapter::HttpAdapter(Options options) : options_(options) {}

core::Result<HttpServerRequest> HttpAdapter::ReadRequest(HttpByteTransport& transport,
                                                         const std::string& remote_address) const {
  HttpConnectionState state;
  return ReadRequest(transport, state, remote_address);
}

core::Result<HttpServerRequest> HttpAdapter::ReadRequest(HttpByteTransport& transport, HttpConnectionState& state,
                                                         const std::string& remote_address) const {
  if (options_.read_buffer_size == 0) {
    return core::Error::Internal("HTTP adapter read_buffer_size must be greater than zero");
  }

  std::vector<char> buffer(options_.read_buffer_size);
  while (true) {
    auto parsed = TryReadRequest(state, remote_address);
    if (!parsed.ok()) {
      return parsed.error();
    }
    auto& request = parsed.value();
    if (request.has_value()) {
      return std::move(request.value());
    }
    const auto read = transport.Read(buffer.data(), buffer.size());
    if (!read.ok()) {
      return read.error();
    }
    if (read.value() == 0) {
      return core::Error::Internal(std::string(kRequestEof));
    }
    // Blocking callers can over-read one buffer beyond a full request.
    state.buffered_bytes_.append(buffer.data(), read.value());
  }
}

core::Result<void> HttpAdapter::AppendInput(HttpConnectionState& state, std::string_view bytes) const {
  if (bytes.size() > options_.max_request_size ||
      state.buffered_bytes_.size() > options_.max_request_size - bytes.size()) {
    return core::Error::Validation(std::string(kInputLimitExceeded));
  }
  state.buffered_bytes_.append(bytes);
  return {};
}

core::Result<std::optional<HttpServerRequest>> HttpAdapter::TryReadRequest(HttpConnectionState& state,
                                                                           std::string remote_address) const {
  auto& raw = state.buffered_bytes_;
  if (!state.pending_request_.has_value()) {
    auto ready = ParseRequestHeaders(state, std::move(remote_address));
    if (!ready.ok()) {
      return ready.error();
    }
    if (!ready.value()) {
      return std::optional<HttpServerRequest>{};
    }
  }
  if (!state.pending_request_.has_value() || raw.size() - state.body_start_ < state.body_size_) {
    return std::optional<HttpServerRequest>{};
  }
  auto request = std::move(state.pending_request_.value());
  state.pending_request_.reset();
  request.body.assign(raw, state.body_start_, state.body_size_);
  raw.erase(0, state.body_start_ + state.body_size_);
  return std::optional<HttpServerRequest>(std::move(request));
}

core::Result<bool> HttpAdapter::ParseRequestHeaders(HttpConnectionState& state, std::string remote_address) const {
  const auto& raw = state.buffered_bytes_;
  const std::size_t header_end = raw.find(core::http::kHeaderDelimiter);
  if (header_end == std::string::npos) {
    if (raw.size() > std::min(options_.max_request_size, options_.max_header_size)) {
      return core::Error::Validation("HTTP request exceeds max_request_size before headers complete");
    }
    return false;
  }
  if (header_end > options_.max_header_size) {
    return core::Error::Validation(std::string(kHeaderSizeLimitExceeded));
  }
  const std::string_view header_block(raw.data(), header_end);
  auto request_line = ParseRequestLine(header_block);
  if (!request_line.ok()) {
    return request_line.error();
  }
  HttpServerRequest request;
  auto content_length = ParseHeaders(header_block, &request.headers, options_.max_header_count);
  if (!content_length.ok()) {
    return content_length.error();
  }
  state.body_start_ = header_end + core::http::kHeaderDelimiter.size();
  state.body_size_ = content_length.value().value_or(0);
  if (state.body_start_ > options_.max_request_size ||
      state.body_size_ > options_.max_request_size - state.body_start_) {
    return core::Error::Validation("Content-Length exceeds max_request_size");
  }
  request.method = std::move(request_line.value().method);
  request.target = std::move(request_line.value().target);
  request.remote_address = std::move(remote_address);
  state.pending_request_ = std::move(request);
  return true;
}

bool HttpAdapter::IsConnectionReusable(const HttpServerRequest& request) {
  return !HeaderContainsToken(request.headers, core::http::kConnectionHeader, core::http::kConnectionCloseHeaderValue);
}

bool HttpAdapter::ShouldCloseConnection(const HttpServerRequest& request, const HttpServerResponse& response) {
  return !IsConnectionReusable(request) ||
         HeaderContainsToken(response.headers, core::http::kConnectionHeader, core::http::kConnectionCloseHeaderValue);
}

std::string HttpAdapter::ReasonPhrase(int status_code) {
  switch (status_code) {
    case core::http::kStatusOk:
      return "OK";
    case core::http::kStatusCreated:
      return "Created";
    case core::http::kStatusAccepted:
      return "Accepted";
    case core::http::kStatusNoContent:
      return "No Content";
    case core::http::kStatusBadRequest:
      return "Bad Request";
    case core::http::kStatusUnauthorized:
      return "Unauthorized";
    case core::http::kStatusForbidden:
      return "Forbidden";
    case core::http::kStatusNotFound:
      return "Not Found";
    case core::http::kStatusMethodNotAllowed:
      return "Method Not Allowed";
    case core::http::kStatusConflict:
      return "Conflict";
    case core::http::kStatusPayloadTooLarge:
      return "Payload Too Large";
    case core::http::kStatusUnsupportedMediaType:
      return "Unsupported Media Type";
    case core::http::kStatusUnprocessableEntity:
      return "Unprocessable Entity";
    case core::http::kStatusTooManyRequests:
      return "Too Many Requests";
    case core::http::kStatusInternalServerError:
      return "Internal Server Error";
    case core::http::kStatusNotImplemented:
      return "Not Implemented";
    case core::http::kStatusBadGateway:
      return "Bad Gateway";
    case core::http::kStatusServiceUnavailable:
      return "Service Unavailable";
    default:
      return "Unknown";
  }
}

core::Result<void> HttpAdapter::WriteResponse(HttpByteTransport& transport, const HttpServerResponse& response) {
  return WriteResponse(transport, response, true);
}

core::Result<void> HttpAdapter::WriteResponse(HttpByteTransport& transport, const HttpServerResponse& response,
                                              bool close_connection) {
  auto encoded = EncodeResponse(response, close_connection);
  if (!encoded.ok()) {
    return encoded.error();
  }
  const auto& payload = encoded.value();
  const bool is_streaming = static_cast<bool>(response.stream_writer);
  const bool must_close_connection =
      close_connection ||
      HeaderContainsToken(response.headers, core::http::kConnectionHeader, core::http::kConnectionCloseHeaderValue);
  if (is_streaming) {
    if (must_close_connection) {
      const auto headers_written = WriteAll(transport, payload);
      if (!headers_written.ok()) {
        return headers_written.error();
      }
      return response.stream_writer(transport);
    }
    const auto headers_written = WriteAll(transport, payload);
    if (!headers_written.ok()) {
      return headers_written.error();
    }
    ChunkedByteTransport chunked_transport(transport);
    const auto streamed = response.stream_writer(chunked_transport);
    if (!streamed.ok()) {
      return streamed.error();
    }
    return chunked_transport.Finish();
  }
  const auto response_written = WriteAll(transport, payload);
  if (!response_written.ok()) {
    return response_written.error();
  }
  return {};
}

core::Result<std::string> HttpAdapter::EncodeResponse(const HttpServerResponse& response, bool close_connection) {
  const std::string reason_phrase = ReasonPhrase(response.status_code);
  std::string payload;
  payload.reserve(ResponsePayloadReserveSize(response, reason_phrase));
  payload += core::http::kHttpVersion11;
  payload.push_back(' ');
  payload += std::to_string(response.status_code);
  payload.push_back(' ');
  payload += reason_phrase;
  payload += core::http::kLineTerminator;

  const bool is_streaming = static_cast<bool>(response.stream_writer);
  const bool response_requests_close =
      HeaderContainsToken(response.headers, core::http::kConnectionHeader, core::http::kConnectionCloseHeaderValue);
  const bool must_close_connection = close_connection || response_requests_close;
  const auto headers =
      AppendResponseHeaders(response, is_streaming, must_close_connection, response_requests_close, &payload);
  if (!headers.ok()) {
    return headers.error();
  }
  if (!headers.value() && !is_streaming) {
    payload += core::http::kContentLengthHeaderName;
    payload += core::http::kHeaderNameValueSeparator;
    payload += std::to_string(response.body.size());
    payload += core::http::kLineTerminator;
  }
  if (is_streaming && !must_close_connection) {
    payload += core::http::kTransferEncodingHeaderName;
    payload += core::http::kHeaderNameValueSeparator;
    payload += core::http::kTransferEncodingChunked;
    payload += core::http::kLineTerminator;
  }
  if (must_close_connection && !response_requests_close) {
    payload += core::http::kConnectionHeaderName;
    payload += core::http::kHeaderNameValueSeparator;
    payload += core::http::kConnectionCloseHeaderValue;
    payload += core::http::kLineTerminator;
  }
  payload += core::http::kLineTerminator;
  if (!is_streaming) {
    payload += response.body;
  }

  return payload;
}

}  // namespace a2a::server
