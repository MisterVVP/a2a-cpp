#pragma once

#include <functional>
#include <string>
#include <string_view>

#include "a2a/core/result.h"

namespace a2a::client {

struct SseEvent final {
  std::string event;
  std::string data;
};

class SseParser final {
 public:
  using EventCallback = std::function<core::Result<void>(const SseEvent&)>;

  core::Result<void> Feed(std::string_view chunk, const EventCallback& on_event);
  core::Result<void> Finish(const EventCallback& on_event);

 private:
  core::Result<void> ConsumeLine(std::string_view line, const EventCallback& on_event);
  core::Result<void> DispatchEvent(const EventCallback& on_event);

  std::string line_buffer_;
  std::string current_event_;
  std::string current_data_;
};

}  // namespace a2a::client
