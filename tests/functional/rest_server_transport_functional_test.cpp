// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <string_view>

#include "../support/rest_server_test_utils.h"
#include "a2a/core/protojson.h"
#include "a2a/server/dispatcher.h"
#include "a2a/server/rest_server_transport.h"
#include "a2a/server/tasks/in_memory_task_store.h"

namespace {

constexpr std::string_view kRequiredExtension = "urn:a2a:tck:required-extension";

a2a::server::RestServerTransportOptions RestOptions(std::string rest_api_base_path) {
  return {.rest_api_base_path = std::move(rest_api_base_path),
          .require_version_header = true,
          .include_legacy_transport_fields = true,
          .agent_card_cache_settings = std::nullopt};
}

a2a::server::RestServerTransportOptions RestOptionsWithRequiredExtension(std::string rest_api_base_path) {
  return {.rest_api_base_path = std::move(rest_api_base_path),
          .require_version_header = true,
          .include_legacy_transport_fields = true,
          .agent_card_cache_settings = std::nullopt,
          .required_extensions = {std::string(kRequiredExtension)}};
}

TEST(RestServerTransportFunctionalTest, SupportsCoreTaskLifecycleOverHttpTargetMapping) {
  a2a::server::InMemoryTaskStore store;
  a2a::tests::support::StoreExecutor executor(&store);
  a2a::server::Dispatcher dispatcher(&executor);
  a2a::server::RestServerTransport server(
      &dispatcher, a2a::tests::support::BuildRestAgentCard("Functional REST Agent", "http://localhost:9090/api"),
      RestOptions("/api"));

  const auto send_response = server.Handle(a2a::tests::support::MakeHttpRequest(
      "POST", "/api/message:send", {{"A2A-Version", "1.0"}},
      R"({"message":{"role":"ROLE_USER","messageId":"task-functional-1","parts":[{"text":"hello"}]}})"));
  ASSERT_TRUE(send_response.ok());
  EXPECT_EQ(send_response.value().status_code, 400);

  const auto list_response =
      server.Handle(a2a::tests::support::MakeHttpRequest("GET", "/api/tasks?pageSize=10", {{"A2A-Version", "1.0"}}));
  ASSERT_TRUE(list_response.ok());
  EXPECT_EQ(list_response.value().status_code, 200);
  EXPECT_EQ(list_response.value().body.find("task-functional-1"), std::string::npos);

  const auto cancel_response = server.Handle(a2a::tests::support::MakeHttpRequest(
      "POST", "/api/tasks/task-functional-1:cancel", {{"A2A-Version", "1.0"}}, "{}"));
  ASSERT_TRUE(cancel_response.ok());
  EXPECT_EQ(cancel_response.value().status_code, 404);
}

TEST(RestServerTransportFunctionalTest, ReturnsStructuredNotFoundForMalformedInput) {
  a2a::server::InMemoryTaskStore store;
  a2a::tests::support::StoreExecutor executor(&store);
  a2a::server::Dispatcher dispatcher(&executor);
  a2a::server::RestServerTransport server(
      &dispatcher, a2a::tests::support::BuildRestAgentCard("Functional REST Agent", "http://localhost:9090/api"),
      RestOptions("/api"));

  const auto response =
      server.Handle(a2a::tests::support::MakeHttpRequest("GET", "/api/tasks?pageSize=abc", {{"A2A-Version", "1.0"}}));

  ASSERT_TRUE(response.ok());
  EXPECT_EQ(response.value().status_code, 404);
  EXPECT_FALSE(response.value().body.empty());
}

TEST(RestServerTransportFunctionalTest, RequiresDeclaredExtensionForHttpJsonRequests) {
  a2a::server::InMemoryTaskStore store;
  a2a::tests::support::StoreExecutor executor(&store);
  a2a::server::Dispatcher dispatcher(&executor);
  a2a::server::RestServerTransport server(
      &dispatcher, a2a::tests::support::BuildRestAgentCard("Functional REST Agent", "http://localhost:9090/api"),
      RestOptionsWithRequiredExtension("/api"));

  const auto missing_extension =
      server.Handle(a2a::tests::support::MakeHttpRequest("GET", "/api/tasks", {{"A2A-Version", "1.0"}}));
  ASSERT_TRUE(missing_extension.ok());
  EXPECT_EQ(missing_extension.value().status_code, 400);
  EXPECT_NE(missing_extension.value().body.find("EXTENSION_SUPPORT_REQUIRED"), std::string::npos);

  const auto with_extension = server.Handle(a2a::tests::support::MakeHttpRequest(
      "GET", "/api/tasks", {{"A2A-Version", "1.0"}, {"A2A-Extensions", std::string(kRequiredExtension)}}));
  ASSERT_TRUE(with_extension.ok());
  EXPECT_EQ(with_extension.value().status_code, 200);
}

}  // namespace
