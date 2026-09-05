// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "a2a/client/sse_parser.h"

#include <string>
#include <string_view>

#include "a2a/core/error.h"

namespace a2a::client {
namespace {

std::string_view TrimSingleLeadingSpace(std::string_view value) {
  if (!value.empty() && value.front() == ' ') {
    value.remove_prefix(1);
  }
  return value;
}

}  // namespace

core::Result<void> SseParser::Feed(std::string_view chunk, const EventCallback& on_event) {
  line_buffer_.append(chunk);

  std::size_t line_start = 0;
  std::size_t line_end = line_buffer_.find('\n', line_start);
  while (line_end != std::string::npos) {
    std::string_view line(line_buffer_.data() + line_start, line_end - line_start);
    if (!line.empty() && line.back() == '\r') {
      line.remove_suffix(1);
    }

    const auto consume = ConsumeLine(line, on_event);
    if (!consume.ok()) {
      return consume.error();
    }

    line_start = line_end + 1U;
    line_end = line_buffer_.find('\n', line_start);
  }
  if (line_start != 0U) {
    line_buffer_.erase(0, line_start);
  }

  return {};
}

core::Result<void> SseParser::Finish(const EventCallback& on_event) {
  if (!line_buffer_.empty()) {
    const auto consume = ConsumeLine(line_buffer_, on_event);
    if (!consume.ok()) {
      return consume.error();
    }
    line_buffer_.clear();
  }

  if (!current_data_.empty() || !current_event_.empty()) {
    return core::Error::Serialization("SSE stream ended with unterminated event frame").WithTransport("http");
  }
  return {};
}

core::Result<void> SseParser::ConsumeLine(std::string_view line, const EventCallback& on_event) {
  if (line.empty()) {
    return DispatchEvent(on_event);
  }
  if (line.starts_with(':')) {
    return {};
  }

  const std::size_t separator = line.find(':');
  if (separator == std::string_view::npos) {
    return core::Error::Serialization("Malformed SSE line: missing ':' separator").WithTransport("http");
  }

  const std::string_view field = line.substr(0, separator);
  const std::string_view value = TrimSingleLeadingSpace(line.substr(separator + 1U));

  if (field == "event") {
    current_event_ = value;
    return {};
  }
  if (field == "data") {
    current_data_.append(value);
    current_data_.push_back('\n');
    return {};
  }

  if (field == "id" || field == "retry") {
    return {};
  }

  return core::Error::Serialization("Malformed SSE line: unsupported field '" + std::string(field) + "'")
      .WithTransport("http");
}

core::Result<void> SseParser::DispatchEvent(const EventCallback& on_event) {
  if (current_data_.empty() && current_event_.empty()) {
    return {};
  }

  if (!current_data_.empty() && current_data_.back() == '\n') {
    current_data_.pop_back();
  }

  SseEvent event{.event = std::exchange(current_event_, {}), .data = std::exchange(current_data_, {})};

  return on_event(event);
}

}  // namespace a2a::client
