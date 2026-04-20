#include "a2a/client/sse_parser.h"

#include <string>
#include <string_view>

#include "a2a/core/error.h"

namespace a2a::client {
namespace {

std::string TrimSingleLeadingSpace(std::string_view value) {
  if (!value.empty() && value.front() == ' ') {
    value.remove_prefix(1);
  }
  return std::string(value);
}

}  // namespace

core::Result<void> SseParser::Feed(std::string_view chunk, const EventCallback& on_event) {
  line_buffer_.append(chunk);

  std::size_t line_end = line_buffer_.find('\n');
  while (line_end != std::string::npos) {
    std::string line = line_buffer_.substr(0, line_end);
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }

    const auto consume = ConsumeLine(line, on_event);
    if (!consume.ok()) {
      return consume.error();
    }

    line_buffer_.erase(0, line_end + 1U);
    line_end = line_buffer_.find('\n');
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
    return core::Error::Serialization("SSE stream ended with unterminated event frame")
        .WithTransport("http");
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
    return core::Error::Serialization("Malformed SSE line: missing ':' separator")
        .WithTransport("http");
  }

  const std::string_view field = line.substr(0, separator);
  const std::string value = TrimSingleLeadingSpace(line.substr(separator + 1U));

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

  return core::Error::Serialization("Malformed SSE line: unsupported field '" + std::string(field) +
                                    "'")
      .WithTransport("http");
}

core::Result<void> SseParser::DispatchEvent(const EventCallback& on_event) {
  if (current_data_.empty() && current_event_.empty()) {
    return {};
  }

  if (!current_data_.empty() && current_data_.back() == '\n') {
    current_data_.pop_back();
  }

  SseEvent event{.event = current_event_, .data = current_data_};
  current_event_.clear();
  current_data_.clear();

  return on_event(event);
}

}  // namespace a2a::client
