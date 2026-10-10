// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

#include <cstddef>
#include <memory>
#include <string_view>

namespace a2a::server {
class TransportMux;
}

namespace a2a::tests::sut {
class SutRuntimeObserver;

// One Asio I/O loop owns all sockets. A fixed pool runs serialized per-connection
// routing/stream jobs; no worker waits for network readiness or an idle stream.
class PerformanceHttpServer final {
 public:
  struct Options final {
    std::size_t application_workers = 4;
    std::size_t max_connections = 256;
    std::size_t max_input_bytes = std::size_t{1024} * 1024U;
    std::size_t max_output_bytes = std::size_t{4} * 1024U * 1024U;
    std::size_t write_slice_bytes = std::size_t{64} * 1024U;
  };

  PerformanceHttpServer(std::string_view host, int port, const server::TransportMux& mux, SutRuntimeObserver* observer,
                        Options options);
  ~PerformanceHttpServer();
  PerformanceHttpServer(const PerformanceHttpServer&) = delete;
  PerformanceHttpServer& operator=(const PerformanceHttpServer&) = delete;
  PerformanceHttpServer(PerformanceHttpServer&&) = delete;
  PerformanceHttpServer& operator=(PerformanceHttpServer&&) = delete;

  [[nodiscard]] bool Start();
  void Run();
  // Thread-safe; posting wakes an otherwise idle event loop immediately.
  void Stop();
  void Join();
  [[nodiscard]] static Options OptionsFromEnvironment();

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};
}  // namespace a2a::tests::sut
