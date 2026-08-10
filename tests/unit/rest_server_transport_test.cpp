// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "a2a/server/rest_server_transport.h"

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>

#include "a2a/core/agent_card/agent_card_provider.h"
#include "a2a/core/protocol_bindings.h"
#include "a2a/core/protocol_errors.h"
#include "a2a/core/protojson.h"
#include "a2a/core/version.h"
#include "server/transport/transport_components.h"

namespace {

constexpr std::time_t kAgentCardLastModifiedUnix = 1704067200;
constexpr std::string_view kRequiredExtension = "urn:a2a:tck:required-extension";
constexpr std::string_view kTenantId = "tenant-1";
constexpr std::string_view kPageSizeQueryKey = "pageSize";
constexpr std::string_view kPageTokenQueryKey = "pageToken";
constexpr std::string_view kHistoryLengthQueryKey = "historyLength";

class EchoExecutor final : public a2a::server::AgentExecutor {
 public:
  a2a::core::Result<lf::a2a::v1::SendMessageResponse> SendMessage(const lf::a2a::v1::SendMessageRequest& request,
                                                                  a2a::server::RequestContext& context) override {
    observed_request_header = context.client_headers["A2A-Version"];
    observed_bearer_token = context.auth_metadata["bearer_token"];
    observed_api_key = context.auth_metadata["api_key"];
    lf::a2a::v1::SendMessageResponse response;
    response.mutable_message()->set_task_id(request.message().task_id());
    response.mutable_message()->set_role(lf::a2a::v1::ROLE_AGENT);
    return response;
  }

  a2a::core::Result<std::unique_ptr<a2a::server::ServerStreamSession>> SendStreamingMessage(
      const lf::a2a::v1::SendMessageRequest& request, a2a::server::RequestContext& context) override {
    (void)request;
    (void)context;
    return a2a::core::Error::Validation("not implemented");
  }

  a2a::core::Result<lf::a2a::v1::Task> GetTask(const lf::a2a::v1::GetTaskRequest& request,
                                               a2a::server::RequestContext& context) override {
    (void)context;
    observed_history_length = request.history_length();
    lf::a2a::v1::Task task;
    task.set_id(request.id());
    return task;
  }

  a2a::core::Result<a2a::server::ListTasksResponse> ListTasks(const a2a::server::ListTasksRequest& request,
                                                              a2a::server::RequestContext& context) override {
    (void)request;
    (void)context;
    return a2a::server::ListTasksResponse{};
  }

  a2a::core::Result<lf::a2a::v1::Task> CancelTask(const lf::a2a::v1::CancelTaskRequest& request,
                                                  a2a::server::RequestContext& context) override {
    (void)context;
    lf::a2a::v1::Task task;
    task.set_id(request.id());
    task.mutable_status()->set_state(lf::a2a::v1::TASK_STATE_CANCELED);
    return task;
  }

  std::string observed_request_header;
  int observed_history_length = -1;
  std::string observed_bearer_token;
  std::string observed_api_key;
};

class RecordingAgentCardProvider final : public a2a::core::AgentCardProvider {
 public:
  explicit RecordingAgentCardProvider(lf::a2a::v1::AgentCard extended_agent_card)
      : extended_agent_card_(std::move(extended_agent_card)) {}

  [[nodiscard]] a2a::core::Result<lf::a2a::v1::AgentCard> GetExtendedAgentCard(
      const a2a::core::AgentCardRequestContext& context) const override {
    observed_tenant = context.tenant;
    return extended_agent_card_;
  }

  mutable std::optional<std::string> observed_tenant;

 private:
  lf::a2a::v1::AgentCard extended_agent_card_;
};

class FailingAgentCardProvider final : public a2a::core::AgentCardProvider {
 public:
  [[nodiscard]] a2a::core::Result<lf::a2a::v1::AgentCard> GetExtendedAgentCard(
      const a2a::core::AgentCardRequestContext& context) const override {
    (void)context;
    return a2a::core::protocol_errors::InvalidAgentResponse("extended card provider failed");
  }
};

lf::a2a::v1::AgentCard BuildCard() {
  lf::a2a::v1::AgentCard card;
  card.set_name("Unit Agent");
  card.set_description("Unit test agent");
  card.set_version(std::string(a2a::core::Version::kAgentCardVersion));
  card.add_default_input_modes("text/plain");
  card.add_default_output_modes("text/plain");
  auto* iface = card.add_supported_interfaces();
  iface->set_protocol_binding(std::string(a2a::core::protocol_bindings::kHttpJson));
  iface->set_protocol_version("1.0");
  iface->set_url("http://localhost:8080/a2a");
  return card;
}

a2a::server::RestServerTransportOptions RestOptions(
    std::string rest_api_base_path,
    std::optional<a2a::server::RestServerTransportOptions::AgentCardCacheSettings> cache_settings = std::nullopt) {
  return {.rest_api_base_path = std::move(rest_api_base_path),
          .require_version_header = true,
          .include_legacy_transport_fields = true,
          .agent_card_cache_settings = std::move(cache_settings),
          .required_extensions = {}};
}

a2a::server::RestServerTransportOptions RestOptionsWithRequiredExtension(std::string rest_api_base_path) {
  auto options = RestOptions(std::move(rest_api_base_path));
  options.required_extensions = {std::string(kRequiredExtension)};
  return options;
}

TEST(RestServerTransportTest, ServesAgentCardFromWellKnownEndpoint) {
  EchoExecutor executor;
  a2a::server::Dispatcher dispatcher(&executor);
  a2a::server::RestServerTransport server(&dispatcher, BuildCard(), RestOptions("/a2a"));

  const auto response = server.Handle(
      {.method = "GET", .target = "/.well-known/agent-card.json", .headers = {}, .body = {}, .remote_address = {}});

  ASSERT_TRUE(response.ok());
  EXPECT_EQ(response.value().status_code, 200);
  EXPECT_EQ(response.value().headers.at("A2A-Version"), "1.0");
  EXPECT_FALSE(response.value().headers.contains("Cache-Control"));
  EXPECT_FALSE(response.value().headers.contains("Last-Modified"));
  EXPECT_TRUE(response.value().headers.contains("ETag"));

  lf::a2a::v1::AgentCard parsed;
  ASSERT_TRUE(a2a::core::JsonToMessage(response.value().body, &parsed, {.ignore_unknown_fields = true}).ok());
  ASSERT_FALSE(parsed.supported_interfaces().empty());
  EXPECT_EQ(parsed.supported_interfaces(0).protocol_version(), "1.0");
  ASSERT_EQ(parsed.supported_interfaces_size(), 1);
  EXPECT_EQ(parsed.supported_interfaces(0).url(), "http://localhost:8080/a2a");
}

TEST(RestServerTransportTest, UsesConfigurableAgentCardCacheHeaders) {
  EchoExecutor executor;
  a2a::server::Dispatcher dispatcher(&executor);
  a2a::server::RestServerTransport server(
      &dispatcher, BuildCard(),
      RestOptions("/a2a", a2a::server::RestServerTransportOptions::AgentCardCacheSettings{
                              .cache_control = "public, max-age=60, stale-while-revalidate=30",
                              .last_modified = std::chrono::system_clock::from_time_t(kAgentCardLastModifiedUnix)}));

  const auto response = server.Handle(
      {.method = "GET", .target = "/.well-known/agent-card.json", .headers = {}, .body = {}, .remote_address = {}});

  ASSERT_TRUE(response.ok());
  EXPECT_EQ(response.value().status_code, 200);
  EXPECT_EQ(response.value().headers.at("Cache-Control"), "public, max-age=60, stale-while-revalidate=30");
  EXPECT_EQ(response.value().headers.at("Last-Modified"), "Mon, 01 Jan 2024 00:00:00 GMT");
  EXPECT_TRUE(response.value().headers.contains("ETag"));
}

TEST(RestServerTransportTest, ServesAgentCardFromLegacyWellKnownEndpoint) {
  EchoExecutor executor;
  a2a::server::Dispatcher dispatcher(&executor);
  a2a::server::RestServerTransport server(&dispatcher, BuildCard(), RestOptions("/a2a"));

  const auto response = server.Handle(
      {.method = "GET", .target = "/.well-known/agent.json", .headers = {}, .body = {}, .remote_address = {}});

  ASSERT_TRUE(response.ok());
  EXPECT_EQ(response.value().status_code, 200);
}

TEST(RestServerTransportTest, AddsBackwardCompatibleTransportFieldsToAgentCard) {
  EchoExecutor executor;
  a2a::server::Dispatcher dispatcher(&executor);
  a2a::server::RestServerTransport server(&dispatcher, BuildCard(), RestOptions("/a2a"));

  const auto response = server.Handle(
      {.method = "GET", .target = "/.well-known/agent.json", .headers = {}, .body = {}, .remote_address = {}});

  ASSERT_TRUE(response.ok());
  google::protobuf::Struct parsed;
  ASSERT_TRUE(a2a::core::JsonToMessage(response.value().body, &parsed).ok());
  const auto& fields = parsed.fields();
  ASSERT_TRUE(fields.contains("endpoint"));
  EXPECT_EQ(fields.at("endpoint").string_value(), "http://localhost:8080/a2a");
  ASSERT_TRUE(fields.contains("preferredTransport"));
  EXPECT_EQ(fields.at("preferredTransport").string_value(), std::string(a2a::core::protocol_bindings::kHttpJson));
}

TEST(RestServerTransportTest, RoutesRequestUsingConfiguredBasePath) {
  EchoExecutor executor;
  a2a::server::Dispatcher dispatcher(&executor);
  a2a::server::RestServerTransport server(&dispatcher, BuildCard(), RestOptions("/a2a"));

  const auto response = server.Handle(
      {.method = "POST",
       .target = "/a2a/message:send",
       .headers = {{"A2A-Version", "1.0"}},
       .body = R"({"message":{"messageId":"msg-1","role":"ROLE_USER","parts":[{"text":"hello"}],"taskId":"t-1"}})",
       .remote_address = {}});

  ASSERT_TRUE(response.ok());
  EXPECT_EQ(response.value().status_code, 200);
  EXPECT_EQ(executor.observed_request_header, "1.0");
  EXPECT_NE(response.value().body.find("t-1"), std::string::npos);
}

TEST(RestServerTransportTest, RejectsMissingVersionWhenConfigured) {
  EchoExecutor executor;
  a2a::server::Dispatcher dispatcher(&executor);
  a2a::server::RestServerTransport server(&dispatcher, BuildCard(), RestOptions("/a2a"));

  const auto response =
      server.Handle({.method = "GET", .target = "/a2a/tasks/task-7", .headers = {}, .body = {}, .remote_address = {}});

  ASSERT_TRUE(response.ok());
  EXPECT_EQ(response.value().status_code, 400);
  EXPECT_NE(response.value().body.find("Missing required A2A-Version header"), std::string::npos);
}

TEST(RestServerTransportTest, ParsesAndDecodesQueryString) {
  EchoExecutor executor;
  a2a::server::Dispatcher dispatcher(&executor);
  a2a::server::RestServerTransport server(&dispatcher, BuildCard(), RestOptions("/a2a"));

  const auto response = server.Handle({.method = "GET",
                                       .target = "/a2a/tasks/task-3?historyLength=20",
                                       .headers = {{"A2A-Version", "1.0"}},
                                       .body = {},
                                       .remote_address = {}});

  ASSERT_TRUE(response.ok());
  EXPECT_EQ(response.value().status_code, 200);
  EXPECT_EQ(executor.observed_history_length, 20);
}

TEST(RestServerTransportTest, ExtractsAuthMetadataIntoRequestContext) {
  EchoExecutor executor;
  a2a::server::Dispatcher dispatcher(&executor);
  a2a::server::RestServerTransport server(&dispatcher, BuildCard(), RestOptions("/a2a"));

  const auto response = server.Handle(
      {.method = "POST",
       .target = "/a2a/message:send",
       .headers = {{"A2A-Version", "1.0"}, {"Authorization", "Bearer token-rest"}, {"X-API-Key", "rest-key"}},
       .body = R"({"message":{"messageId":"msg-2","role":"ROLE_USER","parts":[{"text":"hello"}]}})",
       .remote_address = {}});

  ASSERT_TRUE(response.ok());
  EXPECT_EQ(response.value().status_code, 200);
  EXPECT_EQ(executor.observed_bearer_token, "token-rest");
  EXPECT_EQ(executor.observed_api_key, "rest-key");
}

TEST(RestServerTransportTest, EchoesActivatedExtensionsOnPostValidationRouteErrors) {
  EchoExecutor executor;
  a2a::server::Dispatcher dispatcher(&executor);
  a2a::server::RestServerTransport server(&dispatcher, BuildCard(), RestOptionsWithRequiredExtension("/a2a"));

  const auto response =
      server.Handle({.method = "GET",
                     .target = "/wrong-base/tasks/task-7",
                     .headers = {{"A2A-Version", "1.0"}, {"A2A-Extensions", std::string(kRequiredExtension)}},
                     .body = {},
                     .remote_address = {}});

  ASSERT_TRUE(response.ok());
  EXPECT_EQ(response.value().status_code, 404);
  ASSERT_TRUE(response.value().headers.contains("A2A-Extensions"));
  EXPECT_EQ(response.value().headers.at("A2A-Extensions"), std::string(kRequiredExtension));
}

TEST(RestServerTransportTest, DoesNotEchoActivatedExtensionsWhenRequiredExtensionValidationFails) {
  EchoExecutor executor;
  a2a::server::Dispatcher dispatcher(&executor);
  a2a::server::RestServerTransport server(&dispatcher, BuildCard(), RestOptionsWithRequiredExtension("/a2a"));

  const auto response = server.Handle({.method = "GET",
                                       .target = "/a2a/tasks/task-7",
                                       .headers = {{"A2A-Version", "1.0"}},
                                       .body = {},
                                       .remote_address = {}});

  ASSERT_TRUE(response.ok());
  EXPECT_EQ(response.value().status_code, 400);
  EXPECT_FALSE(response.value().headers.contains("A2A-Extensions"));
}

TEST(RestServerTransportTest, ServesConfiguredExtendedAgentCard) {
  constexpr std::string_view kExtendedName = "Extended REST Agent";
  EchoExecutor executor;
  auto extended_card = BuildCard();
  extended_card.set_name(std::string(kExtendedName));
  auto provider = std::make_shared<a2a::core::StaticAgentCardProvider>(extended_card);
  a2a::server::Dispatcher dispatcher(&executor, provider);
  a2a::server::RestServerTransport server(&dispatcher, BuildCard(), RestOptions("/a2a"));

  const auto response = server.Handle({.method = "GET",
                                       .target = "/extendedAgentCard",
                                       .headers = {{"A2A-Version", "1.0"}},
                                       .body = {},
                                       .remote_address = {}});

  ASSERT_TRUE(response.ok());
  EXPECT_EQ(response.value().status_code, 200);
  EXPECT_NE(response.value().body.find(kExtendedName), std::string::npos);
}

TEST(RestServerTransportTest, ServesConfiguredExtendedAgentCardFromDiscoveryView) {
  constexpr std::string_view kExtendedName = "Extended Discovery Agent";
  EchoExecutor executor;
  auto extended_card = BuildCard();
  extended_card.set_name(std::string(kExtendedName));
  auto provider = std::make_shared<a2a::core::StaticAgentCardProvider>(extended_card);
  a2a::server::Dispatcher dispatcher(&executor, provider);
  a2a::server::RestServerTransport server(&dispatcher, BuildCard(), RestOptions("/a2a"));

  const auto response = server.Handle({.method = "GET",
                                       .target = "/.well-known/agent-card.json?view=extended",
                                       .headers = {{"A2A-Version", "1.0"}},
                                       .body = {},
                                       .remote_address = {}});

  ASSERT_TRUE(response.ok());
  EXPECT_EQ(response.value().status_code, 200);
  EXPECT_NE(response.value().body.find(kExtendedName), std::string::npos);
}

TEST(RestServerTransportTest, ServesConfiguredExtendedAgentCardUnderRestBasePath) {
  constexpr std::string_view kExtendedName = "Extended REST Base Agent";
  EchoExecutor executor;
  auto extended_card = BuildCard();
  extended_card.set_name(std::string(kExtendedName));
  auto provider = std::make_shared<a2a::core::StaticAgentCardProvider>(extended_card);
  a2a::server::Dispatcher dispatcher(&executor, provider);
  a2a::server::RestServerTransport server(&dispatcher, BuildCard(), RestOptions("/a2a"));

  const auto response = server.Handle({.method = "GET",
                                       .target = "/a2a/extendedAgentCard",
                                       .headers = {{"A2A-Version", "1.0"}},
                                       .body = {},
                                       .remote_address = {}});

  ASSERT_TRUE(response.ok());
  EXPECT_EQ(response.value().status_code, 200);
  EXPECT_NE(response.value().body.find(kExtendedName), std::string::npos);
}

TEST(RestServerTransportTest, PropagatesTenantFromExtendedAgentCardPath) {
  EchoExecutor executor;
  auto extended_card = BuildCard();
  auto provider = std::make_shared<RecordingAgentCardProvider>(extended_card);
  a2a::server::Dispatcher dispatcher(&executor, provider);
  a2a::server::RestServerTransport server(&dispatcher, BuildCard(), RestOptions("/a2a"));

  const auto response = server.Handle({.method = "GET",
                                       .target = "/a2a/tenant-1/extendedAgentCard",
                                       .headers = {{"A2A-Version", "1.0"}},
                                       .body = {},
                                       .remote_address = {}});

  ASSERT_TRUE(response.ok());
  EXPECT_EQ(response.value().status_code, 200);
  EXPECT_EQ(provider->observed_tenant, std::optional<std::string>(std::string(kTenantId)));
}

TEST(RestServerTransportTest, ExtendedAgentCardReturnsNotConfiguredWhenMissing) {
  constexpr std::string_view kErrorReason = "EXTENDED_AGENT_CARD_NOT_CONFIGURED";
  EchoExecutor executor;
  a2a::server::Dispatcher dispatcher(&executor);
  a2a::server::RestServerTransport server(&dispatcher, BuildCard(), RestOptions("/a2a"));

  const auto response = server.Handle({.method = "GET",
                                       .target = "/extendedAgentCard",
                                       .headers = {{"A2A-Version", "1.0"}},
                                       .body = {},
                                       .remote_address = {}});

  ASSERT_TRUE(response.ok());
  EXPECT_EQ(response.value().status_code, 400);
  EXPECT_NE(response.value().body.find(kErrorReason), std::string::npos);
}

TEST(RestServerTransportTest, ExtendedAgentCardPreservesProviderErrorReason) {
  constexpr std::string_view kExpectedReason = "INVALID_AGENT_RESPONSE";
  constexpr std::string_view kUnexpectedReason = "EXTENDED_AGENT_CARD_NOT_CONFIGURED";
  EchoExecutor executor;
  auto provider = std::make_shared<FailingAgentCardProvider>();
  a2a::server::Dispatcher dispatcher(&executor, provider);
  a2a::server::RestServerTransport server(&dispatcher, BuildCard(), RestOptions("/a2a"));

  const auto response = server.Handle({.method = "GET",
                                       .target = "/extendedAgentCard",
                                       .headers = {{"A2A-Version", "1.0"}},
                                       .body = {},
                                       .remote_address = {}});

  ASSERT_TRUE(response.ok());
  EXPECT_EQ(response.value().status_code, 502);
  EXPECT_NE(response.value().body.find(kExpectedReason), std::string::npos);
  EXPECT_EQ(response.value().body.find(kUnexpectedReason), std::string::npos);
}

TEST(RestQueryParserTest, ParsesUnescapedKnownParameters) {
  constexpr std::string_view kQuery = "pageSize=15&pageToken=page-2&historyLength=3";
  constexpr std::string_view kExpectedPageSize = "15";
  constexpr std::string_view kExpectedPageToken = "page-2";
  constexpr std::string_view kExpectedHistoryLength = "3";
  std::unordered_map<std::string, std::string> query_params;

  const auto parsed = a2a::server::internal::ParseRestQueryString(kQuery, &query_params);

  ASSERT_TRUE(parsed.ok());
  EXPECT_EQ(query_params.at(std::string(kPageSizeQueryKey)), kExpectedPageSize);
  EXPECT_EQ(query_params.at(std::string(kPageTokenQueryKey)), kExpectedPageToken);
  EXPECT_EQ(query_params.at(std::string(kHistoryLengthQueryKey)), kExpectedHistoryLength);
}

TEST(RestQueryParserTest, PreservesUrlDecodingSemantics) {
  constexpr std::string_view kQuery = "pageToken=next%2Fpage+one";
  constexpr std::string_view kExpectedPageToken = "next/page one";
  std::unordered_map<std::string, std::string> query_params;

  const auto parsed = a2a::server::internal::ParseRestQueryString(kQuery, &query_params);

  ASSERT_TRUE(parsed.ok());
  EXPECT_EQ(query_params.at(std::string(kPageTokenQueryKey)), kExpectedPageToken);
}

}  // namespace
