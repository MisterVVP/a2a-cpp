// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once
#include <boost/asio/any_io_executor.hpp>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "a2a/core/agent_card/agent_card_provider.h"
#include "a2a/core/non_copyable.h"
#include "a2a/server/engine/coroutine_agent_executor.h"
#include "a2a/server/security_policy.h"
#include "a2a/server/server_interceptor.h"

namespace a2a::server::engine {
struct AsyncDispatcherOptions final {
  static constexpr std::size_t kDefaultMaxInFlight = 1024;
  std::size_t max_in_flight = kDefaultMaxInFlight;
  // Includes coroutines still cleaning up after logical request cancellation.
  std::size_t max_pending = kDefaultMaxInFlight;
  std::vector<std::shared_ptr<ServerInterceptor>> interceptors;
  std::shared_ptr<core::AgentCardProvider> agent_card_provider;
  std::shared_ptr<const SecurityPolicy> security_policy;
  // Same blocking security/store facility as synchronous dispatch. Must outlive
  // dispatch completion. It always runs on Asio workers, never a reactor.
  TaskStore* task_store = nullptr;
  std::vector<std::string> required_extensions;
};

// Coroutine execution pipeline; the completion sink below is a transport boundary.
class AsyncDispatcher final : private core::NonCopyableOrMovable {
 public:
  using TransportCompletion = std::function<void(core::Result<DispatchResponse>, RequestExecutionContext)>;
  AsyncDispatcher(std::shared_ptr<CoroutineAgentExecutor> executor, boost::asio::any_io_executor scheduler,
                  AsyncDispatcherOptions options = {});
  ~AsyncDispatcher();

  // On admission failure no callback runs. Accepted requests complete exactly
  // once on its Asio strand, after AfterDispatch. Never runs user code inline.
  // Completion receives an owned lifecycle view, even if it runs before Dispatch
  // returns the cancellation handle. Its metadata is the ingress snapshot.
  // Transport owns deadline notification via the returned source; completion and
  // start also check the deadline. A transport must not call TryComplete itself.
  [[nodiscard]] core::Result<std::shared_ptr<RequestExecution>> Dispatch(
      DispatchRequest request, RequestContext metadata, TransportCompletion completion,
      std::optional<RequestExecution::Clock::time_point> deadline = {});

  // Streams retain admission and run AfterDispatch only at terminal completion.
  // Each awaited write consumes one unit of transport demand.
  [[nodiscard]] core::Result<std::shared_ptr<RequestExecution>> DispatchStream(
      DispatchRequest request, RequestContext metadata, std::shared_ptr<AsyncServerStreamSession> stream,
      TransportCompletion completion, std::optional<RequestExecution::Clock::time_point> deadline = {});

  // Queue one cancellation notification per transport call. Stop observers run
  // on Asio workers, never inline on a network reactor. The source may already be
  // terminal; first-terminal-transition precedence remains unchanged.
  void Cancel(std::shared_ptr<RequestExecution> execution,
              RequestCancellationReason reason = RequestCancellationReason::kClientCancelled);

  // Closes admission and schedules cancellation. The caller owns the Asio context
  // and must drain operations before destroying it. Destruction never waits for I/O;
  // suspended frames retain their application and pipeline until cleanup finishes.
  void Shutdown();

 private:
  [[nodiscard]] core::Result<std::shared_ptr<RequestExecution>> DispatchImpl(
      DispatchRequest request, RequestContext metadata, TransportCompletion completion,
      std::optional<RequestExecution::Clock::time_point> deadline, std::shared_ptr<AsyncServerStreamSession> stream);
  struct State;
  std::shared_ptr<State> state_;
};
}  // namespace a2a::server::engine
