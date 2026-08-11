// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "a2a/server/http_adapter.h"

#include <algorithm>
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

core::Result<void> ReadUntilHeadersComplete(HttpByteTransport& transport, std::size_t max_request_size,
                                            std::vector<char>& buffer, std::string& raw) {
  while (raw.find(core::http::kHeaderDelimiter) == std::string::npos) {
    const auto read = transport.Read(buffer.data(), buffer.size());
    if (!read.ok()) {
      return read.error();
    }
    if (read.value() == 0) {
      return core::Error::Internal("Unexpected end of stream while reading HTTP headers");
    }
    raw.append(buffer.data(), read.value());
    if (raw.find(core::http::kHeaderDelimiter) == std::string::npos && raw.size() > max_request_size) {
      return core::Error::Validation("HTTP request exceeds max_request_size before headers complete");
    }
  }
  return {};
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

  headers->insert_or_assign(name, value);
  return {};
}

core::Result<std::optional<std::size_t>> ParseHeaders(std::string_view header_block,
                                                      std::unordered_map<std::string, std::string>* headers) {
  const std::size_t first_line_end = header_block.find(core::http::kLineTerminator);
  std::size_t offset = first_line_end + core::http::kLineTerminator.size();
  std::optional<std::size_t> content_length;

  while (offset < header_block.size()) {
    const std::size_t line_end = header_block.find(core::http::kLineTerminator, offset);
    const std::size_t next = (line_end == std::string::npos) ? header_block.size() : line_end;
    const std::string_view line = header_block.substr(offset, next - offset);
    if (!line.empty()) {
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

struct BodyReadLimits final {
  std::size_t expected_body_size;
  std::size_t body_start;
  std::size_t max_request_size;
};

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

core::Result<void> ReadRemainingBody(HttpByteTransport& transport, const BodyReadLimits& limits,
                                     std::vector<char>& buffer, std::string& raw) {
  while (raw.size() - limits.body_start < limits.expected_body_size) {
    const auto read = transport.Read(buffer.data(), buffer.size());
    if (!read.ok()) {
      return read.error();
    }
    if (read.value() == 0) {
      return core::Error::Internal("Unexpected end of stream while reading HTTP body");
    }
    raw.append(buffer.data(), read.value());
    if (limits.body_start + limits.expected_body_size > limits.max_request_size) {
      return core::Error::Validation("HTTP request exceeds max_request_size while reading body");
    }
  }
  return {};
}

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
                                                         std::string remote_address) const {
  HttpConnectionState state;
  return ReadRequest(transport, state, std::move(remote_address));
}

core::Result<HttpServerRequest> HttpAdapter::ReadRequest(HttpByteTransport& transport, HttpConnectionState& state,
                                                         std::string remote_address) const {
  if (options_.read_buffer_size == 0) {
    return core::Error::Internal("HTTP adapter read_buffer_size must be greater than zero");
  }

  std::string& raw = state.buffered_bytes_;
  if (raw.capacity() < options_.read_buffer_size * 2U) {
    raw.reserve(options_.read_buffer_size * 2U);
  }
  std::vector<char> buffer(options_.read_buffer_size);

  const auto headers_read = ReadUntilHeadersComplete(transport, options_.max_request_size, buffer, raw);
  if (!headers_read.ok()) {
    return headers_read.error();
  }

  const std::size_t header_end = raw.find(core::http::kHeaderDelimiter);
  const std::string_view header_block(raw.data(), header_end);

  const auto request_line = ParseRequestLine(header_block);
  if (!request_line.ok()) {
    return request_line.error();
  }

  std::unordered_map<std::string, std::string> headers;
  const auto content_length = ParseHeaders(header_block, &headers);
  if (!content_length.ok()) {
    return content_length.error();
  }

  const std::size_t body_start = header_end + core::http::kHeaderDelimiter.size();
  const std::size_t expected_body_size = content_length.value().value_or(0);
  if (body_start > options_.max_request_size || expected_body_size > options_.max_request_size - body_start) {
    return core::Error::Validation("Content-Length exceeds max_request_size");
  }

  const BodyReadLimits limits{
      .expected_body_size = expected_body_size,
      .body_start = body_start,
      .max_request_size = options_.max_request_size,
  };
  const auto body_read = ReadRemainingBody(transport, limits, buffer, raw);
  if (!body_read.ok()) {
    return body_read.error();
  }

  HttpServerRequest request;
  request.method = request_line.value().method;
  request.target = request_line.value().target;
  request.headers = std::move(headers);
  request.body = raw.substr(body_start, expected_body_size);
  request.remote_address = std::move(remote_address);
  raw.erase(0, body_start + expected_body_size);
  return request;
}

bool HttpAdapter::IsConnectionReusable(const HttpServerRequest& request) {
  return !HeaderContainsToken(request.headers, core::http::kConnectionHeader, core::http::kConnectionCloseHeaderValue);
}

bool HttpAdapter::ShouldCloseConnection(const HttpServerRequest& request, const HttpServerResponse& response) {
  return !IsConnectionReusable(request) || static_cast<bool>(response.stream_writer) ||
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

core::Result<void> HttpAdapter::WriteResponse(HttpByteTransport& transport, const HttpServerResponse& response,
                                              bool close_connection) {
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
  const bool must_close_connection = close_connection || is_streaming || response_requests_close;
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

  const auto headers_written = WriteAll(transport, payload);
  if (!headers_written.ok()) {
    return headers_written.error();
  }
  if (is_streaming) {
    return response.stream_writer(transport);
  }
  return {};
}

}  // namespace a2a::server
