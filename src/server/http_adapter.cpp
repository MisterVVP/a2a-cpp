// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "a2a/server/http_adapter.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <limits>
#include <optional>
#include <ranges>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <unistd.h>
#endif

namespace a2a::server {
namespace {
constexpr std::string_view kHeaderDelimiter = "\r\n\r\n";
constexpr std::string_view kLineDelimiter = "\r\n";
constexpr std::string_view kHttpVersion = "HTTP/1.1";
constexpr std::string_view kContentLengthHeader = "content-length";
constexpr std::string_view kConnectionClose = "Connection: close\r\n";

constexpr int kStatusOk = 200;
constexpr int kStatusCreated = 201;
constexpr int kStatusAccepted = 202;
constexpr int kStatusNoContent = 204;
constexpr int kStatusBadRequest = 400;
constexpr int kStatusUnauthorized = 401;
constexpr int kStatusForbidden = 403;
constexpr int kStatusNotFound = 404;
constexpr int kStatusMethodNotAllowed = 405;
constexpr int kStatusConflict = 409;
constexpr int kStatusPayloadTooLarge = 413;
constexpr int kStatusUnsupportedMediaType = 415;
constexpr int kStatusUnprocessableEntity = 422;
constexpr int kStatusTooManyRequests = 429;
constexpr int kStatusInternalServerError = 500;
constexpr int kStatusNotImplemented = 501;
constexpr int kStatusBadGateway = 502;
constexpr int kStatusServiceUnavailable = 503;

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

std::string ToLower(std::string_view value) {
  std::string out(value);
  std::ranges::transform(out, out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return out;
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
  while (raw.find(kHeaderDelimiter) == std::string::npos) {
    const auto read = transport.Read(buffer.data(), buffer.size());
    if (!read.ok()) {
      return read.error();
    }
    if (read.value() == 0) {
      return core::Error::Internal("Unexpected end of stream while reading HTTP headers");
    }
    raw.append(buffer.data(), read.value());
    if (raw.size() > max_request_size) {
      return core::Error::Validation("HTTP request exceeds max_request_size before headers complete");
    }
  }
  return {};
}

core::Result<RequestLine> ParseRequestLine(std::string_view header_block) {
  const std::size_t first_line_end = header_block.find(kLineDelimiter);
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
  if (version != kHttpVersion) {
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

  if (ToLower(name) == kContentLengthHeader) {
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
  const std::size_t first_line_end = header_block.find(kLineDelimiter);
  std::size_t offset = first_line_end + kLineDelimiter.size();
  std::optional<std::size_t> content_length;

  while (offset < header_block.size()) {
    const std::size_t line_end = header_block.find(kLineDelimiter, offset);
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
    offset = line_end + kLineDelimiter.size();
  }

  return content_length;
}

struct BodyReadLimits final {
  std::size_t expected_body_size;
  std::size_t body_start;
  std::size_t max_request_size;
};

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
    if (raw.size() > limits.max_request_size) {
      return core::Error::Validation("HTTP request exceeds max_request_size while reading body");
    }
  }
  return {};
}

}  // namespace

HttpAdapter::HttpAdapter() = default;
HttpAdapter::HttpAdapter(Options options) : options_(options) {}

core::Result<HttpServerRequest> HttpAdapter::ReadRequest(HttpByteTransport& transport,
                                                         std::string remote_address) const {
  if (options_.read_buffer_size == 0) {
    return core::Error::Internal("HTTP adapter read_buffer_size must be greater than zero");
  }

  std::string raw;
  raw.reserve(options_.read_buffer_size * 2U);
  std::vector<char> buffer(options_.read_buffer_size);

  const auto headers_read = ReadUntilHeadersComplete(transport, options_.max_request_size, buffer, raw);
  if (!headers_read.ok()) {
    return headers_read.error();
  }

  const std::size_t header_end = raw.find(kHeaderDelimiter);
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

  const std::size_t body_start = header_end + kHeaderDelimiter.size();
  const std::size_t expected_body_size = content_length.value().value_or(0);
  if (expected_body_size > options_.max_request_size) {
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
  return request;
}

std::string HttpAdapter::ReasonPhrase(int status_code) {
  switch (status_code) {
    case kStatusOk:
      return "OK";
    case kStatusCreated:
      return "Created";
    case kStatusAccepted:
      return "Accepted";
    case kStatusNoContent:
      return "No Content";
    case kStatusBadRequest:
      return "Bad Request";
    case kStatusUnauthorized:
      return "Unauthorized";
    case kStatusForbidden:
      return "Forbidden";
    case kStatusNotFound:
      return "Not Found";
    case kStatusMethodNotAllowed:
      return "Method Not Allowed";
    case kStatusConflict:
      return "Conflict";
    case kStatusPayloadTooLarge:
      return "Payload Too Large";
    case kStatusUnsupportedMediaType:
      return "Unsupported Media Type";
    case kStatusUnprocessableEntity:
      return "Unprocessable Entity";
    case kStatusTooManyRequests:
      return "Too Many Requests";
    case kStatusInternalServerError:
      return "Internal Server Error";
    case kStatusNotImplemented:
      return "Not Implemented";
    case kStatusBadGateway:
      return "Bad Gateway";
    case kStatusServiceUnavailable:
      return "Service Unavailable";
    default:
      return "Unknown";
  }
}

core::Result<void> HttpAdapter::WriteResponse(HttpByteTransport& transport, const HttpServerResponse& response) {
  std::ostringstream stream;
  stream << kHttpVersion << ' ' << response.status_code << ' ' << ReasonPhrase(response.status_code) << "\r\n";

  bool has_content_length = false;
  for (const auto& [name, value] : response.headers) {
    if (ToLower(name) == kContentLengthHeader) {
      has_content_length = true;
      const auto parsed_length = ParseContentLength(value);
      if (!parsed_length.ok()) {
        return core::Error::Validation("Response Content-Length header is invalid");
      }
      if (parsed_length.value() != response.body.size()) {
        return core::Error::Validation("Response Content-Length header does not match body size");
      }
    }
    stream << name << ": " << value << "\r\n";
  }
  if (!has_content_length) {
    stream << "Content-Length: " << response.body.size() << "\r\n";
  }
  stream << kConnectionClose << "\r\n";
  stream << response.body;

  const std::string payload = stream.str();
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

void CloseSocketCrossPlatform(int fd) noexcept {
#ifdef _WIN32
  closesocket(fd);
#else
  close(fd);
#endif
}

}  // namespace a2a::server
