// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "a2a/core/error.h"
#include "a2a/core/http_constants.h"
#include "a2a/core/result.h"

namespace a2a::http {

struct Header final {
  std::string name;
  std::string value;
};

struct Request final {
  std::string method;
  std::string url;
  std::vector<Header> headers;
  std::string body;
  std::chrono::milliseconds timeout{0};
  std::string http_version = std::string(core::http::kHttpVersion11);
};

struct Response final {
  int status_code = 0;
  std::vector<Header> headers;
  std::string body;
};

namespace detail {
struct ClientState;

struct HeaderCapture final {
  std::vector<Header>* response_headers = nullptr;
  long response_code = 0;
};

struct StreamCallbackContext final {
  const std::function<core::Result<void>(std::string_view)>* on_chunk = nullptr;
  const std::function<bool()>* is_cancelled = nullptr;
  HeaderCapture* header_capture = nullptr;
  std::optional<core::Error> error;
  bool metadata_checked = false;
};
}  // namespace detail

class Client final {
 public:
  Client();

  [[nodiscard]] core::Result<Response> SendRequest(const Request& request) const;
  [[nodiscard]] core::Result<Response> StreamRequest(
      const Request& request, const std::function<core::Result<void>(std::string_view)>& on_chunk,
      const std::function<bool()>& is_cancelled) const;

 private:
  std::shared_ptr<detail::ClientState> state_;
};

[[nodiscard]] bool IsSupportedHttpVersion(std::string_view http_version) noexcept;

}  // namespace a2a::http
