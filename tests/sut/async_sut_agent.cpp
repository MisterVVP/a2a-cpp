// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "sut/async_sut_agent.h"

#include <utility>

namespace a2a::tests::sut {
AsyncSutAgent::AsyncSutAgent(server::stores::StoreBundle stores, const SutEndpoints& endpoints, SutAgentOptions options)
    : application_(std::make_shared<SutApplication>(std::move(stores), endpoints, std::move(options))),
      executor_(std::make_shared<AsyncSutExecutor>(application_)) {
  server::engine::AsyncDispatcherOptions execution;
  execution.agent_card_provider = application_->card_provider();
  execution.interceptors = {application_->interceptor()};
  execution.security_policy = application_->security_policy();
  execution.task_store = application_->task_store();
  dispatcher_ = std::make_shared<server::engine::AsyncDispatcher>(executor_, std::move(execution));
}
AsyncSutAgent::~AsyncSutAgent() { Shutdown(); }
std::shared_ptr<server::engine::AsyncDispatcher> AsyncSutAgent::dispatcher() const { return dispatcher_; }
const lf::a2a::v1::AgentCard& AsyncSutAgent::agent_card() const { return application_->agent_card(); }
void AsyncSutAgent::Shutdown() { dispatcher_->Shutdown(); }
}  // namespace a2a::tests::sut
