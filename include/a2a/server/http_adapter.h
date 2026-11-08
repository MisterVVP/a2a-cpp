// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

#include <cstddef>
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

 private:
  friend class HttpAdapter;
  std::string buffered_bytes_;
  std::optional<HttpServerRequest> pending_request_;
  std::size_t header_search_offset_ = 0;
  std::size_t body_offset_ = 0;
  std::size_t request_bytes_ = 0;
  std::size_t previous_input_bytes_ = 0;
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
  // Byte-only codec. Incomplete input returns an empty optional and consumes
  // zero bytes; success consumes exactly one request, including its body.
  [[nodiscard]] core::Result<std::optional<HttpServerRequest>> ParseRequest(std::string_view bytes,
                                                                            std::size_t& consumed,
                                                                            std::string remote_address = {}) const;
  // Stateful incremental decoding. Append to the same input until success;
  // then remove exactly consumed bytes before decoding the next request. Header
  // searches and parsing are retained in state, which belongs to one connection.
  [[nodiscard]] core::Result<std::optional<HttpServerRequest>> ParseRequest(std::string_view bytes,
                                                                            HttpConnectionState& state,
                                                                            std::size_t& consumed,
                                                                            std::string remote_address = {}) const;
  // Streaming heads use chunked framing on reusable connections. The caller
  // owns chunk framing and the final chunk when using this byte-only API.
  // chunked_stream forces chunking even on a closing connection.
  [[nodiscard]] static core::Result<std::string> EncodeResponseHead(const HttpServerResponse& response,
                                                                    bool close_connection, bool chunked_stream = false);
  [[nodiscard]] static core::Result<std::string> EncodeResponse(const HttpServerResponse& response,
                                                                bool close_connection = false);
  [[nodiscard]] static bool IsConnectionReusable(const HttpServerRequest& request);
  [[nodiscard]] static bool ShouldCloseConnection(const HttpServerRequest& request, const HttpServerResponse& response);
  [[nodiscard]] static core::Result<void> WriteResponse(HttpByteTransport& transport,
                                                        const HttpServerResponse& response);
  [[nodiscard]] static core::Result<void> WriteResponse(HttpByteTransport& transport,
                                                        const HttpServerResponse& response, bool close_connection);

  [[nodiscard]] static std::string ReasonPhrase(int status_code);

 private:
  [[nodiscard]] core::Result<bool> ParseRequestHeaders(std::string_view bytes, HttpConnectionState& state) const;
  Options options_;
};

}  // namespace a2a::server
