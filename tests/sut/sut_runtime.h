// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

#include <chrono>
#include <cstddef>
#include <memory>
#include <string_view>

namespace a2a::server {
struct HttpServerRequest;
struct HttpServerResponse;
}  // namespace a2a::server

namespace a2a::tests::sut {

class SutHttpConnectionObserver {
 public:
  virtual ~SutHttpConnectionObserver() = default;

  SutHttpConnectionObserver(const SutHttpConnectionObserver&) = delete;
  SutHttpConnectionObserver& operator=(const SutHttpConnectionObserver&) = delete;

  [[nodiscard]] virtual bool BeginRequest(const server::HttpServerRequest& request,
                                          server::HttpServerResponse& response) = 0;
  virtual void FinishRequest(const server::HttpServerResponse& response, bool close_connection,
                             bool handled_by_observer) = 0;

 protected:
  SutHttpConnectionObserver() = default;
};

class SutRuntimeObserver {
 public:
  virtual ~SutRuntimeObserver() = default;

  SutRuntimeObserver(const SutRuntimeObserver&) = delete;
  SutRuntimeObserver& operator=(const SutRuntimeObserver&) = delete;

  [[nodiscard]] virtual std::unique_ptr<SutHttpConnectionObserver> ObserveHttpConnection() = 0;
  virtual void OnShutdown() = 0;

 protected:
  SutRuntimeObserver() = default;
};

enum class SutReadFixture { kDisabled, kListener, kDispatcher, kPipeline };
enum class SutIngress { kShared, kSeparate };

struct SutRuntimeOptions final {
  std::string_view display_name;
  SutRuntimeObserver* observer = nullptr;
  bool asynchronous = false;
  bool allow_opaque_input = false;
  std::chrono::milliseconds get_task_delay{};
  std::size_t http_pending_limit = 0;
  SutReadFixture read_fixture = SutReadFixture::kDisabled;
  std::size_t dispatcher_threads = 0;
  SutIngress ingress = SutIngress::kShared;
  bool seed_pipeline_task = false;
};

int RunSutRuntime(int argc, char** argv, const SutRuntimeOptions& options);

}  // namespace a2a::tests::sut
