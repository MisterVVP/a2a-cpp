// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

#include <cstddef>
#include <limits>
#include <optional>
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
  [[nodiscard]] std::size_t BufferedSize() const noexcept { return buffered_bytes_.size(); }

 private:
  friend class HttpAdapter;
  std::string buffered_bytes_;
  std::optional<HttpServerRequest> pending_request_;
  std::size_t body_start_ = 0;
  std::size_t body_size_ = 0;
};

class HttpAdapter final {
 public:
  struct Options final {
    std::size_t max_request_size = 1024U * 1024U;
    std::size_t read_buffer_size = 4096;
    std::size_t max_header_size = (std::numeric_limits<std::size_t>::max)();
    std::size_t max_header_count = (std::numeric_limits<std::size_t>::max)();
  };

  HttpAdapter();
  explicit HttpAdapter(Options options);

  [[nodiscard]] core::Result<HttpServerRequest> ReadRequest(HttpByteTransport& transport,
                                                            const std::string& remote_address) const;
  [[nodiscard]] core::Result<HttpServerRequest> ReadRequest(HttpByteTransport& transport, HttpConnectionState& state,
                                                            const std::string& remote_address) const;
  // Append is bounded; TryReadRequest consumes at most one complete request and
  // retains pipelined bytes. An empty optional means more input is needed.
  [[nodiscard]] core::Result<void> AppendInput(HttpConnectionState& state, std::string_view bytes) const;
  [[nodiscard]] core::Result<std::optional<HttpServerRequest>> TryReadRequest(HttpConnectionState& state,
                                                                              std::string remote_address) const;
  [[nodiscard]] static core::Result<std::string> EncodeResponse(const HttpServerResponse& response,
                                                                bool close_connection);
  [[nodiscard]] static bool IsConnectionReusable(const HttpServerRequest& request);
  [[nodiscard]] static bool ShouldCloseConnection(const HttpServerRequest& request, const HttpServerResponse& response);
  [[nodiscard]] static core::Result<void> WriteResponse(HttpByteTransport& transport,
                                                        const HttpServerResponse& response);
  [[nodiscard]] static core::Result<void> WriteResponse(HttpByteTransport& transport,
                                                        const HttpServerResponse& response, bool close_connection);

  [[nodiscard]] static std::string ReasonPhrase(int status_code);

 private:
  [[nodiscard]] core::Result<bool> ParseRequestHeaders(HttpConnectionState& state, std::string remote_address) const;
  Options options_;
};

}  // namespace a2a::server
