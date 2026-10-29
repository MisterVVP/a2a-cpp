// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

#include <memory>

#include "a2a/core/non_copyable.h"
#include "a2a/core/result.h"
#include "a2a/server/rest_server_transport.h"
#include "sut/sut_runtime.h"
#include "sut/tck_sut.h"

namespace grpc {
class Service;
}
namespace a2a::server {
class TransportMux;
}
namespace a2a::tests::sut {
struct SutServerConfig final {
  SutConfig listener;
  SutEndpoints endpoints;
};
[[nodiscard]] core::Result<SutServerConfig> ReadSutServerConfig(int argc, char** argv);
[[nodiscard]] server::RestServerTransportOptions SutRestOptions();
class SutServerRuntime final : private core::NonCopyableOrMovable {
 public:
  SutServerRuntime(SutServerConfig config, SutRuntimeOptions options);
  ~SutServerRuntime();
  [[nodiscard]] bool Start(const server::TransportMux& mux, ::grpc::Service& service);
  void Run();
  void ShutdownHttp();
  void ShutdownGrpc();

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};
}  // namespace a2a::tests::sut
