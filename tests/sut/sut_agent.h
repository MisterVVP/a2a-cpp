// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

#include <memory>

#include "a2a/server/dispatcher.h"
#include "a2a/server/stores/store_factory.h"
#include "a2a/v1/a2a.pb.h"
#include "sut/sut_agent_options.h"

namespace a2a::tests::sut {

class SutAgent final {
 public:
  SutAgent(server::stores::StoreBundle stores, const SutEndpoints& endpoints, SutAgentOptions options = {});
  ~SutAgent();
  SutAgent(const SutAgent&) = delete;
  SutAgent& operator=(const SutAgent&) = delete;
  SutAgent(SutAgent&&) = delete;
  SutAgent& operator=(SutAgent&&) = delete;

  [[nodiscard]] server::Dispatcher& dispatcher() noexcept;
  [[nodiscard]] const lf::a2a::v1::AgentCard& agent_card() const noexcept;
  void ShutdownSubscriptions();

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace a2a::tests::sut
