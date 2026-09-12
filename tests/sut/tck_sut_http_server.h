// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

#include <csignal>
#include <memory>
#include <string_view>

namespace a2a::server {
class TransportMux;
}

namespace a2a::tests::sut {

class TckHttpServer final {
 public:
  TckHttpServer(std::string_view host, int port, const server::TransportMux& mux);
  ~TckHttpServer();

  TckHttpServer(const TckHttpServer&) = delete;
  TckHttpServer& operator=(const TckHttpServer&) = delete;
  TckHttpServer(TckHttpServer&&) = delete;
  TckHttpServer& operator=(TckHttpServer&&) = delete;

  [[nodiscard]] bool Start();
  void AcceptConnections(const volatile std::sig_atomic_t& keep_running);
  void ShutdownActiveSockets();
  void JoinConnections();
  void EmitDiagnostics() const;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace a2a::tests::sut
