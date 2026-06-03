// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <string_view>

#include "a2a/core/protocol_bindings.h"
#include "a2a/core/version.h"
#include "a2a/server/rest_server_transport.h"

namespace {

constexpr int kHttpOk = 200;
constexpr int kPageSize = 25;
constexpr std::string_view kTaskId = "task-1";
constexpr std::string_view kConfigId = "push-1";
constexpr std::string_view kWebhookUrl = "https://example.invalid/push";

class PushExecutor final : public a2a::server::AgentExecutor {
 public:
  a2a::core::Result<lf::a2a::v1::SendMessageResponse> SendMessage(const lf::a2a::v1::SendMessageRequest& request,
                                                                  a2a::server::RequestContext& context) override {
    (void)request;
    (void)context;
    return a2a::core::Error::Validation("not implemented");
  }

  a2a::core::Result<std::unique_ptr<a2a::server::ServerStreamSession>> SendStreamingMessage(
      const lf::a2a::v1::SendMessageRequest& request, a2a::server::RequestContext& context) override {
    (void)request;
    (void)context;
    return a2a::core::Error::Validation("not implemented");
  }

  a2a::core::Result<lf::a2a::v1::Task> GetTask(const lf::a2a::v1::GetTaskRequest& request,
                                               a2a::server::RequestContext& context) override {
    (void)request;
    (void)context;
    return a2a::core::Error::Validation("not implemented");
  }

  a2a::core::Result<a2a::server::ListTasksResponse> ListTasks(const a2a::server::ListTasksRequest& request,
                                                              a2a::server::RequestContext& context) override {
    (void)request;
    (void)context;
    return a2a::core::Error::Validation("not implemented");
  }

  a2a::core::Result<lf::a2a::v1::Task> CancelTask(const lf::a2a::v1::CancelTaskRequest& request,
                                                  a2a::server::RequestContext& context) override {
    (void)request;
    (void)context;
    return a2a::core::Error::Validation("not implemented");
  }

  a2a::core::Result<lf::a2a::v1::TaskPushNotificationConfig> CreateTaskPushNotificationConfig(
      const lf::a2a::v1::TaskPushNotificationConfig& request, a2a::server::RequestContext& context) override {
    (void)context;
    task_id = request.task_id();
    config_id = request.id();
    url = request.url();
    return request;
  }

  a2a::core::Result<lf::a2a::v1::TaskPushNotificationConfig> GetTaskPushNotificationConfig(
      const lf::a2a::v1::GetTaskPushNotificationConfigRequest& request, a2a::server::RequestContext& context) override {
    (void)context;
    task_id = request.task_id();
    config_id = request.id();
    lf::a2a::v1::TaskPushNotificationConfig config;
    config.set_task_id(request.task_id());
    config.set_id(request.id());
    config.set_url(std::string(kWebhookUrl));
    return config;
  }

  a2a::core::Result<lf::a2a::v1::ListTaskPushNotificationConfigsResponse> ListTaskPushNotificationConfigs(
      const lf::a2a::v1::ListTaskPushNotificationConfigsRequest& request,
      a2a::server::RequestContext& context) override {
    (void)context;
    task_id = request.task_id();
    page_size = request.page_size();
    page_token = request.page_token();
    lf::a2a::v1::ListTaskPushNotificationConfigsResponse response;
    auto* config = response.add_configs();
    config->set_task_id(request.task_id());
    config->set_id(std::string(kConfigId));
    config->set_url(std::string(kWebhookUrl));
    return response;
  }

  a2a::core::Result<void> DeleteTaskPushNotificationConfig(
      const lf::a2a::v1::DeleteTaskPushNotificationConfigRequest& request,
      a2a::server::RequestContext& context) override {
    (void)context;
    task_id = request.task_id();
    config_id = request.id();
    deleted = true;
    return {};
  }

  std::string task_id;
  std::string config_id;
  std::string url;
  int page_size = 0;
  std::string page_token;
  bool deleted = false;
};

lf::a2a::v1::AgentCard BuildCard() {
  lf::a2a::v1::AgentCard card;
  card.set_name("Push Unit Agent");
  card.set_description("Push unit test agent");
  card.set_version(std::string(a2a::core::Version::kAgentCardVersion));
  card.add_default_input_modes("text/plain");
  card.add_default_output_modes("text/plain");
  auto* iface = card.add_supported_interfaces();
  iface->set_protocol_binding(std::string(a2a::core::protocol_bindings::kHttpJson));
  iface->set_protocol_version("1.0");
  iface->set_url("https://example.invalid/a2a");
  return card;
}

a2a::server::RestServerTransportOptions RestOptions() {
  a2a::server::RestServerTransportOptions options;
  options.rest_api_base_path = "/a2a";
  options.require_version_header = true;
  return options;
}

TEST(RestPushNotificationTransportTest, CreatesPushNotificationConfig) {
  PushExecutor executor;
  a2a::server::Dispatcher dispatcher(&executor);
  a2a::server::RestServerTransport server(&dispatcher, BuildCard(), RestOptions());

  const auto response = server.Handle({.method = "POST",
                                       .target = "/a2a/tasks/task-1/pushNotificationConfigs",
                                       .headers = {{"A2A-Version", "1.0"}},
                                       .body = R"({"id":"push-1","url":"https://example.invalid/push"})",
                                       .remote_address = {}});

  ASSERT_TRUE(response.ok());
  EXPECT_EQ(response.value().status_code, kHttpOk);
  EXPECT_EQ(executor.task_id, kTaskId);
  EXPECT_EQ(executor.config_id, kConfigId);
  EXPECT_EQ(executor.url, kWebhookUrl);
}

TEST(RestPushNotificationTransportTest, GetsPushNotificationConfig) {
  PushExecutor executor;
  a2a::server::Dispatcher dispatcher(&executor);
  a2a::server::RestServerTransport server(&dispatcher, BuildCard(), RestOptions());

  const auto response = server.Handle({.method = "GET",
                                       .target = "/a2a/tasks/task-1/pushNotificationConfigs/push-1",
                                       .headers = {{"A2A-Version", "1.0"}},
                                       .body = {},
                                       .remote_address = {}});

  ASSERT_TRUE(response.ok());
  EXPECT_EQ(response.value().status_code, kHttpOk);
  EXPECT_EQ(executor.task_id, kTaskId);
  EXPECT_EQ(executor.config_id, kConfigId);
}

TEST(RestPushNotificationTransportTest, ListsPushNotificationConfigsWithPagingQuery) {
  PushExecutor executor;
  a2a::server::Dispatcher dispatcher(&executor);
  a2a::server::RestServerTransport server(&dispatcher, BuildCard(), RestOptions());

  const auto response = server.Handle({.method = "GET",
                                       .target = "/a2a/tasks/task-1/pushNotificationConfigs?pageSize=25&pageToken=abc",
                                       .headers = {{"A2A-Version", "1.0"}},
                                       .body = {},
                                       .remote_address = {}});

  ASSERT_TRUE(response.ok());
  EXPECT_EQ(response.value().status_code, kHttpOk);
  EXPECT_EQ(executor.task_id, kTaskId);
  EXPECT_EQ(executor.page_size, kPageSize);
  EXPECT_EQ(executor.page_token, "abc");
}

TEST(RestPushNotificationTransportTest, DeletesPushNotificationConfig) {
  PushExecutor executor;
  a2a::server::Dispatcher dispatcher(&executor);
  a2a::server::RestServerTransport server(&dispatcher, BuildCard(), RestOptions());

  const auto response = server.Handle({.method = "DELETE",
                                       .target = "/a2a/tasks/task-1/pushNotificationConfigs/push-1",
                                       .headers = {{"A2A-Version", "1.0"}},
                                       .body = {},
                                       .remote_address = {}});

  ASSERT_TRUE(response.ok());
  EXPECT_EQ(response.value().status_code, kHttpOk);
  EXPECT_TRUE(executor.deleted);
  EXPECT_EQ(executor.task_id, kTaskId);
  EXPECT_EQ(executor.config_id, kConfigId);
}

}  // namespace
