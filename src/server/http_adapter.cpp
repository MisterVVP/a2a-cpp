// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "a2a/server/http_adapter.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <cstdint>
#include <limits>
#include <vector>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>

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
  std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return out;
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
  if (parsed > std::numeric_limits<std::size_t>::max()) {
    return core::Error::Validation("Content-Length value overflows platform size_t");
  }
  return static_cast<std::size_t>(parsed);
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

  while (raw.find(kHeaderDelimiter) == std::string::npos) {
    const auto read = transport.Read(buffer.data(), buffer.size());
    if (!read.ok()) {
      return read.error();
    }
    if (read.value() == 0) {
      return core::Error::Internal("Unexpected end of stream while reading HTTP headers");
    }
    raw.append(buffer.data(), read.value());
    if (raw.size() > options_.max_request_size) {
      return core::Error::Validation("HTTP request exceeds max_request_size before headers complete");
    }
  }

  const std::size_t header_end = raw.find(kHeaderDelimiter);
  const std::string_view header_block(raw.data(), header_end);
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

  std::unordered_map<std::string, std::string> headers;
  std::optional<std::size_t> content_length;

  std::size_t offset = first_line_end + kLineDelimiter.size();
  while (offset < header_block.size()) {
    const std::size_t line_end = header_block.find(kLineDelimiter, offset);
    const std::size_t next = line_end == std::string::npos ? header_block.size() : line_end;
    const std::string_view line = header_block.substr(offset, next - offset);
    if (!line.empty()) {
      const std::size_t colon = line.find(':');
      if (colon == std::string::npos || colon == 0 || colon == line.size() - 1) {
        return core::Error::Validation("Malformed HTTP header line");
      }
      const std::string name = Trim(line.substr(0, colon));
      const std::string value = Trim(line.substr(colon + 1));
      if (name.empty()) {
        return core::Error::Validation("HTTP header name cannot be empty");
      }
      if (value.empty()) {
        return core::Error::Validation("HTTP header value cannot be empty");
      }
      const std::string lowered_name = ToLower(name);
      if (lowered_name == kContentLengthHeader) {
        auto parsed_length = ParseContentLength(value);
        if (!parsed_length.ok()) {
          return parsed_length.error();
        }
        if (content_length.has_value() && content_length.value() != parsed_length.value()) {
          return core::Error::Validation("Conflicting Content-Length header values");
        }
        content_length = parsed_length.value();
      }
      headers.insert_or_assign(name, value);
    }
    if (line_end == std::string::npos) {
      break;
    }
    offset = line_end + kLineDelimiter.size();
  }

  const std::size_t body_start = header_end + kHeaderDelimiter.size();
  const std::size_t expected_body_size = content_length.value_or(0);
  if (expected_body_size > options_.max_request_size) {
    return core::Error::Validation("Content-Length exceeds max_request_size");
  }
  while (raw.size() - body_start < expected_body_size) {
    const auto read = transport.Read(buffer.data(), buffer.size());
    if (!read.ok()) {
      return read.error();
    }
    if (read.value() == 0) {
      return core::Error::Internal("Unexpected end of stream while reading HTTP body");
    }
    raw.append(buffer.data(), read.value());
    if (raw.size() > options_.max_request_size) {
      return core::Error::Validation("HTTP request exceeds max_request_size while reading body");
    }
  }

  HttpServerRequest request;
  request.method = std::move(method);
  request.target = std::move(target);
  request.headers = std::move(headers);
  request.body = raw.substr(body_start, expected_body_size);
  request.remote_address = std::move(remote_address);
  return request;
}

std::string HttpAdapter::ReasonPhrase(int status_code) {
  switch (status_code) {
    case 200:
      return "OK";
    case 201:
      return "Created";
    case 202:
      return "Accepted";
    case 204:
      return "No Content";
    case 400:
      return "Bad Request";
    case 401:
      return "Unauthorized";
    case 403:
      return "Forbidden";
    case 404:
      return "Not Found";
    case 405:
      return "Method Not Allowed";
    case 409:
      return "Conflict";
    case 413:
      return "Payload Too Large";
    case 415:
      return "Unsupported Media Type";
    case 422:
      return "Unprocessable Entity";
    case 429:
      return "Too Many Requests";
    case 500:
      return "Internal Server Error";
    case 501:
      return "Not Implemented";
    case 502:
      return "Bad Gateway";
    case 503:
      return "Service Unavailable";
    default:
      return "Unknown";
  }
}

core::Result<void> HttpAdapter::WriteResponse(HttpByteTransport& transport,
                                              const HttpServerResponse& response) const {
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
