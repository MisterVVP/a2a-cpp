// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "sut/sut_agent.h"

#include <utility>

#include "sut/sut_application.h"

namespace a2a::tests::sut {
class SutAgent::Impl final {
 public:
  Impl(server::stores::StoreBundle stores, const SutEndpoints& endpoints, SutAgentOptions options)
      : application_(std::move(stores), endpoints, std::move(options)),
        dispatcher_(&application_.executor(), application_.card_provider()) {
    dispatcher_.AddInterceptor(application_.interceptor());
    if (const auto security = application_.security_policy()) {
      dispatcher_.SetSecurityPolicy(security, application_.task_store());
    }
  }
  SutApplication application_;
  server::Dispatcher dispatcher_;
};

SutAgent::SutAgent(server::stores::StoreBundle stores, const SutEndpoints& endpoints, SutAgentOptions options)
    : impl_(std::make_unique<Impl>(std::move(stores), endpoints, std::move(options))) {}
SutAgent::~SutAgent() = default;
server::Dispatcher& SutAgent::dispatcher() noexcept { return impl_->dispatcher_; }
const lf::a2a::v1::AgentCard& SutAgent::agent_card() const noexcept { return impl_->application_.agent_card(); }
void SutAgent::ShutdownSubscriptions() { impl_->application_.Shutdown(); }
}  // namespace a2a::tests::sut
