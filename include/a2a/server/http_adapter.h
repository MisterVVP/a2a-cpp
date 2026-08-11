// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

#include <cstddef>
#include <string>
#include <string_view>

#include "a2a/core/result.h"
#include "a2a/server/rest_server_transport.h"

namespace a2a::server {

class HttpByteTransport {
 public:
  virtual ~HttpByteTransport() = default;
  [[nodiscard]] virtual core::Result<std::size_t> Read(char* buffer, std::size_t size) = 0;
  [[nodiscard]] virtual core::Result<std::size_t> Write(const char* buffer, std::size_t size) = 0;
};

// Retains bytes read beyond one request for the lifetime of an HTTP connection.
class HttpConnectionState final {
 public:
  HttpConnectionState() = default;

 private:
  friend class HttpAdapter;
  std::string buffered_bytes_;
};

class HttpAdapter final {
 public:
  struct Options final {
    std::size_t max_request_size = 1024U * 1024U;
    std::size_t read_buffer_size = 4096;
  };

  HttpAdapter();
  explicit HttpAdapter(Options options);

  [[nodiscard]] core::Result<HttpServerRequest> ReadRequest(HttpByteTransport& transport,
                                                            std::string remote_address) const;
  [[nodiscard]] core::Result<HttpServerRequest> ReadRequest(HttpByteTransport& transport, HttpConnectionState& state,
                                                            std::string remote_address) const;
  [[nodiscard]] static bool IsConnectionReusable(const HttpServerRequest& request);
  [[nodiscard]] static bool ShouldCloseConnection(const HttpServerRequest& request, const HttpServerResponse& response);
  [[nodiscard]] static core::Result<void> WriteResponse(HttpByteTransport& transport,
                                                        const HttpServerResponse& response);
  [[nodiscard]] static core::Result<void> WriteResponse(HttpByteTransport& transport,
                                                        const HttpServerResponse& response, bool close_connection);

  [[nodiscard]] static std::string ReasonPhrase(int status_code);

 private:
  Options options_;
};

}  // namespace a2a::server
