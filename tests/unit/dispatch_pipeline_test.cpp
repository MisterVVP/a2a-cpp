// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "server/engine/dispatch_pipeline.h"

#include <gtest/gtest.h>

#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "a2a/server/tasks/in_memory_task_store.h"

namespace {
using namespace a2a;
using namespace a2a::server;
using namespace a2a::server::engine;
constexpr auto kAlice = "session-alice";
constexpr auto kBob = "session-bob";
constexpr auto kSpoofed = "session-spoofed";
constexpr auto kAfterFailure = "session audit hook failed";

class SessionPolicy final : public SecurityPolicy {
 public:
  [[nodiscard]] core::Result<std::string> Authenticate(const RequestContext& context) const override {
    return context.request_id.value_or(kAlice);
  }
  [[nodiscard]] bool AuthorizeOperation(const DispatchRequest& /*request*/,
                                        const RequestContext& /*context*/) const override {
    return true;
  }
  [[nodiscard]] bool CanAccessTask(const lf::a2a::v1::Task& /*task*/,
                                   const RequestContext& /*context*/) const override {
    return true;
  }
};

class SessionInterceptor final : public ServerInterceptor {
 public:
  explicit SessionInterceptor(bool throws = false) : throws_(throws) {}
  core::Result<void> BeforeDispatch(const DispatchRequest& /*request*/, RequestContext& context) override {
    context.principal = kSpoofed;
    return {};
  }
  void AfterDispatch(const DispatchRequest& /*request*/, RequestContext& context,
                     const core::Result<DispatchResponse>& /*result*/) override {
    observed.push_back(context.principal);
    context.principal = kSpoofed;
    if (throws_) {
      throw std::runtime_error(kAfterFailure);
    }
  }
  std::vector<std::string> observed;

 private:
  const bool throws_;
};

class DispatchSessionTest : public testing::Test {
 protected:
  DispatchSessionTest() {
    pipeline.security = std::make_shared<DispatchSecurity>(std::make_shared<SessionPolicy>(), &store);
    pipeline.interceptors = {audit, throwing};
    alice.request_id = kAlice;
    bob.request_id = kBob;
  }
  InMemoryTaskStore store;
  const std::shared_ptr<SessionInterceptor> audit = std::make_shared<SessionInterceptor>();
  const std::shared_ptr<SessionInterceptor> throwing = std::make_shared<SessionInterceptor>(true);
  DispatchPipeline pipeline;
  const DispatchRequest request{.operation = DispatcherOperation::kGetExtendedAgentCard,
                                .payload = lf::a2a::v1::GetExtendedAgentCardRequest{}};
  RequestContext alice;
  RequestContext bob;
};

TEST_F(DispatchSessionTest, SharedConfigurationKeepsAuthenticationSeparateForEachRequest) {
  DispatchSession first(pipeline, request, alice);
  DispatchSession second(pipeline, request, bob);
  ASSERT_TRUE(first.Before().ok());
  ASSERT_TRUE(second.Before().ok());
  EXPECT_EQ(alice.principal, kAlice);
  EXPECT_EQ(bob.principal, kBob);
  alice.principal = kSpoofed;
  bob.principal = kSpoofed;
  core::Result<DispatchResponse> response = DispatchResponse{};
  first.Project(response);
  second.Project(response);
  EXPECT_EQ(alice.principal, kAlice);
  EXPECT_EQ(bob.principal, kBob);
}

TEST_F(DispatchSessionTest, ContainedAuditExceptionRestoresPrincipalAndRunsRemainingHooks) {
  DispatchSession session(pipeline, request, alice);
  ASSERT_TRUE(session.Before().ok());
  const core::Result<DispatchResponse> response = DispatchResponse{};
  EXPECT_NO_THROW(session.After(response, InterceptorExceptionPolicy::kContain));
  EXPECT_EQ(throwing->observed, std::vector<std::string>{kAlice});
  EXPECT_EQ(audit->observed, std::vector<std::string>{kAlice});
  EXPECT_EQ(alice.principal, kAlice);
}

TEST_F(DispatchSessionTest, DefaultAuditExceptionPolicyPreservesSynchronousPropagation) {
  DispatchSession session(pipeline, request, bob);
  ASSERT_TRUE(session.Before().ok());
  const core::Result<DispatchResponse> response = DispatchResponse{};
  EXPECT_THROW(session.After(response), std::runtime_error);
  EXPECT_EQ(throwing->observed, std::vector<std::string>{kBob});
  EXPECT_TRUE(audit->observed.empty());
}
}  // namespace
