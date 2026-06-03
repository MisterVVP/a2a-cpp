// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

#include <chrono>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "a2a/core/http_constants.h"
#include "a2a/core/result.h"

namespace a2a::server {

namespace detail {
struct HttpClientGlobalState;
}

struct HttpClientHeader final {
  std::string name;
  std::string value;
};

struct HttpClientRequest final {
  std::string method;
  std::string url;
  std::vector<HttpClientHeader> headers;
  std::string body;
  std::chrono::milliseconds timeout{0};
  std::string http_version = std::string(core::http::kHttpVersion11);
};

struct HttpClientResponse final {
  int status_code = 0;
  std::string body;
};

class HttpClient final {
 public:
  HttpClient();

  [[nodiscard]] core::Result<HttpClientResponse> SendRequest(const HttpClientRequest& request) const;

 private:
  std::shared_ptr<const detail::HttpClientGlobalState> global_state_;
};

[[nodiscard]] bool IsSupportedHttpVersion(std::string_view http_version) noexcept;

}  // namespace a2a::server
