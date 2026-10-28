// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "a2a/core/agent_card/agent_card_provider.h"
#include "a2a/core/non_copyable.h"
#include "a2a/server/engine/async_agent_executor.h"
#include "a2a/server/security_policy.h"
#include "a2a/server/server_interceptor.h"

namespace a2a::server::engine {
struct AsyncDispatcherOptions final {
  static constexpr std::size_t kDefaultWorkerCount = 2;
  static constexpr std::size_t kDefaultMaxInFlight = 1024;
  std::size_t worker_count = kDefaultWorkerCount;
  std::size_t max_in_flight = kDefaultMaxInFlight;
  std::vector<std::shared_ptr<ServerInterceptor>> interceptors;
  std::shared_ptr<core::AgentCardProvider> agent_card_provider;
  std::shared_ptr<const SecurityPolicy> security_policy;
  // Same blocking security/store facility as synchronous dispatch. Must outlive
  // dispatch completion. It always runs on dispatcher workers, never a reactor.
  TaskStore* task_store = nullptr;
  std::vector<std::string> required_extensions;
};

class AsyncDispatcher final : private core::NonCopyableOrMovable {
 public:
  using Completion = std::function<void(core::Result<DispatchResponse>, RequestExecutionContext)>;
  explicit AsyncDispatcher(std::shared_ptr<AsyncAgentExecutor> executor, AsyncDispatcherOptions options = {});
  ~AsyncDispatcher();

  // On admission failure no callback runs. Accepted requests complete exactly
  // once on a dispatcher worker, after AfterDispatch. Never runs user code inline.
  // Completion receives an owned lifecycle view, even if it runs before Dispatch
  // returns the cancellation handle. Its metadata is the ingress snapshot.
  // Transport owns deadline notification via the returned source; completion and
  // start also check the deadline. A transport must not call TryComplete itself.
  [[nodiscard]] core::Result<std::shared_ptr<RequestExecution>> Dispatch(
      DispatchRequest request, RequestContext metadata, Completion completion,
      std::optional<RequestExecution::Clock::time_point> deadline = {});

  // Stops admission and requests cancellation without waiting for application I/O.
  // Destruction drains SDK work and joins workers (except the current worker when
  // destroyed from a callback). Late executor completions are harmless.
  void Shutdown();

 private:
  struct State;
  std::shared_ptr<State> state_;
};
}  // namespace a2a::server::engine
