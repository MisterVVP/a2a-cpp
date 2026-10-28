// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "sut/sut_agent.h"
#if A2A_HAS_REQUEST_EXECUTION
#include <chrono>
#include <future>

#include "sut/blocking_sut_executor.h"
#endif

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <string_view>

#include "a2a/core/http_constants.h"
#include "a2a/core/protocol_codes.h"
#include "a2a/core/version.h"
#include "a2a/server/dispatch_types.h"
#include "a2a/server/request_context.h"
#include "a2a/server/stores/store_factory.h"

namespace {

constexpr std::string_view kRestUrl = "http://agent.local/a2a";
constexpr std::string_view kRpcUrl = "http://agent.local/rpc";
constexpr std::string_view kGrpcUrl = "agent.local:50062";
constexpr std::string_view kAuthToken = "unit-sut-token";
constexpr std::string_view kWrongToken = "other-unit-token";
constexpr std::string_view kBearerKey = "bearer_token";
constexpr std::size_t kBindingCount = 3;

std::unique_ptr<a2a::tests::sut::SutAgent> MakeAgent(a2a::tests::sut::SutAgentOptions options = {}) {
  const a2a::server::stores::InMemoryStoreFactory factory;
  auto stores = factory.CreateStoreBundle();
  EXPECT_TRUE(stores.ok());
  return std::make_unique<a2a::tests::sut::SutAgent>(
      std::move(stores.value()),
      a2a::tests::sut::SutEndpoints{
          .rest_url = std::string(kRestUrl), .json_rpc_url = std::string(kRpcUrl), .grpc_url = std::string(kGrpcUrl)},
      std::move(options));
}

TEST(SutAgentTest, AllInterfacesAdvertiseTheSameProtocolAndCapabilities) {
  const auto agent = MakeAgent();
  const auto& card = agent->agent_card();
  ASSERT_EQ(card.supported_interfaces_size(), kBindingCount);
  EXPECT_TRUE(card.capabilities().streaming());
  EXPECT_TRUE(card.capabilities().push_notifications());
  for (const auto& iface : card.supported_interfaces()) {
    EXPECT_EQ(iface.protocol_version(), a2a::core::Version::kProtocolVersion);
  }
}

TEST(SutAgentTest, NegativeCapabilitiesAreEnforcedBeforeExecutorDispatch) {
  const auto agent = MakeAgent({.streaming = false, .push_notifications = false, .auth_token = {}});
  EXPECT_FALSE(agent->agent_card().capabilities().streaming());
  EXPECT_FALSE(agent->agent_card().capabilities().push_notifications());
  a2a::server::RequestContext context;
  const auto streaming =
      agent->dispatcher().Dispatch({.operation = a2a::server::DispatcherOperation::kSendStreamingMessage,
                                    .payload = lf::a2a::v1::SendMessageRequest{}},
                                   context);
  ASSERT_FALSE(streaming.ok());
  EXPECT_EQ(streaming.error().protocol_code(), a2a::core::protocol_codes::kUnsupportedOperation);
  const auto push =
      agent->dispatcher().Dispatch({.operation = a2a::server::DispatcherOperation::kCreateTaskPushNotificationConfig,
                                    .payload = lf::a2a::v1::TaskPushNotificationConfig{}},
                                   context);
  ASSERT_FALSE(push.ok());
  EXPECT_EQ(push.error().protocol_code(), a2a::core::protocol_codes::kPushNotificationNotSupported);
}

TEST(SutAgentTest, SharedAuthorizationPolicyRejectsMissingAndIncorrectCredentials) {
  a2a::tests::sut::SutAgentOptions options;
  options.auth_token = kAuthToken;
  const auto agent = MakeAgent(options);
  EXPECT_FALSE(agent->agent_card().security_schemes().empty());
  const a2a::server::DispatchRequest request{.operation = a2a::server::DispatcherOperation::kGetExtendedAgentCard,
                                             .payload = lf::a2a::v1::GetExtendedAgentCardRequest{}};
  a2a::server::RequestContext context;
  const auto missing = agent->dispatcher().Dispatch(request, context);
  ASSERT_FALSE(missing.ok());
  EXPECT_EQ(missing.error().http_status(), a2a::core::http::kStatusUnauthorized);
  context.auth_metadata[std::string(kBearerKey)] = kWrongToken;
  const auto wrong = agent->dispatcher().Dispatch(request, context);
  ASSERT_FALSE(wrong.ok());
  EXPECT_EQ(wrong.error().http_status(), a2a::core::http::kStatusForbidden);
  context.auth_metadata[std::string(kBearerKey)] = kAuthToken;
  EXPECT_TRUE(agent->dispatcher().Dispatch(request, context).ok());
}

void CheckExtendedCardMode(std::string_view mode) {
  a2a::tests::sut::SutAgentOptions options;
  options.extended_card_mode = mode;
  const auto agent = MakeAgent(options);
  a2a::server::RequestContext context;
  const auto card = agent->dispatcher().Dispatch({.operation = a2a::server::DispatcherOperation::kGetExtendedAgentCard,
                                                  .payload = lf::a2a::v1::GetExtendedAgentCardRequest{}},
                                                 context);
  if (mode == a2a::tests::sut::kExtendedCardModeConfigured) {
    EXPECT_TRUE(card.ok());
  } else {
    ASSERT_FALSE(card.ok());
    EXPECT_EQ(card.error().protocol_code(), a2a::core::protocol_codes::kExtendedAgentCardNotConfigured);
  }
  EXPECT_EQ(agent->agent_card().capabilities().extended_agent_card(),
            mode != a2a::tests::sut::kExtendedCardModeDisabled);
}

TEST(SutAgentTest, ExistingExtendedCardProfilesShareOneProvider) {
  for (const auto mode : {a2a::tests::sut::kExtendedCardModeConfigured, a2a::tests::sut::kExtendedCardModeDeclaredOnly,
                          a2a::tests::sut::kExtendedCardModeDisabled}) {
    SCOPED_TRACE(mode);
    CheckExtendedCardMode(mode);
  }
}

}  // namespace

#if A2A_HAS_REQUEST_EXECUTION
namespace {
constexpr auto kBlockingAdapterWait = std::chrono::seconds(5);
TEST(SutAgentTest, BlockingAsyncFixturePreservesUnaryErrors) {
  auto agent = MakeAgent();
  a2a::tests::sut::BlockingSutExecutor executor(agent->dispatcher());
  a2a::server::engine::RequestExecution execution;
  std::promise<a2a::core::Result<lf::a2a::v1::Task>> reply;
  auto result = reply.get_future();
  executor.GetTask({}, execution.Context(), [&](auto response) { reply.set_value(std::move(response)); });
  ASSERT_EQ(result.wait_for(kBlockingAdapterWait), std::future_status::ready);
  auto response = result.get();
  EXPECT_FALSE(response.ok());
}
TEST(SutAgentTest, BlockingAsyncFixtureUsesExistingExtendedCardProvider) {
  auto agent = MakeAgent();
  a2a::tests::sut::BlockingSutExecutor executor(agent->dispatcher());
  const auto card = executor.GetExtendedAgentCard({});
  ASSERT_TRUE(card.ok());
  EXPECT_FALSE(card.value().name().empty());
}
}  // namespace
#endif
