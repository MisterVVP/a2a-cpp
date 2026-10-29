// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

#include <memory>

#include "a2a/core/agent_card/agent_card_provider.h"
#include "a2a/core/non_copyable.h"
#include "a2a/server/agent_executor.h"
#include "a2a/server/security_policy.h"
#include "a2a/server/server_interceptor.h"
#include "a2a/server/stores/store_factory.h"
#include "sut/sut_agent_options.h"

namespace a2a::tests::sut {
class SutApplication final : private core::NonCopyableOrMovable {
 public:
  SutApplication(server::stores::StoreBundle stores, const SutEndpoints& endpoints, SutAgentOptions options = {});
  ~SutApplication();
  [[nodiscard]] server::AgentExecutor& executor() noexcept;
  [[nodiscard]] const lf::a2a::v1::AgentCard& agent_card() const noexcept;
  [[nodiscard]] std::shared_ptr<core::AgentCardProvider> card_provider() const;
  [[nodiscard]] std::shared_ptr<server::ServerInterceptor> interceptor() const;
  [[nodiscard]] std::shared_ptr<const server::SecurityPolicy> security_policy() const;
  [[nodiscard]] server::TaskStore* task_store() const;
  void Shutdown();

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};
}  // namespace a2a::tests::sut
