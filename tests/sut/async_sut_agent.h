// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

#include <boost/asio/thread_pool.hpp>
#include <memory>

#include "a2a/core/non_copyable.h"
#include "a2a/server/engine/async_dispatcher.h"
#include "sut/async_sut_executor.h"
#include "sut/sut_application.h"

namespace a2a::tests::sut {
class AsyncSutAgent final : private core::NonCopyableOrMovable {
 public:
  AsyncSutAgent(server::stores::StoreBundle stores, const SutEndpoints& endpoints, SutAgentOptions options);
  ~AsyncSutAgent();
  [[nodiscard]] std::shared_ptr<server::engine::AsyncDispatcher> dispatcher() const;
  [[nodiscard]] const lf::a2a::v1::AgentCard& agent_card() const;
  void Shutdown();

 private:
  static constexpr std::size_t kIoWorkers = 2;
  boost::asio::thread_pool runtime_{kIoWorkers};
  std::shared_ptr<SutApplication> application_;
  std::shared_ptr<AsyncSutExecutor> executor_;
  std::shared_ptr<server::engine::AsyncDispatcher> dispatcher_;
};
}  // namespace a2a::tests::sut
