// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "a2a/server/rest_transport.h"

#include <gtest/gtest.h>

#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "a2a/server/http_adapter.h"

namespace {

class RecordingHttpTransport final : public a2a::server::HttpByteTransport {
 public:
  [[nodiscard]] a2a::core::Result<std::size_t> Read(char* buffer, std::size_t size) override {
    (void)buffer;
    (void)size;
    return a2a::core::Error::Internal("read is not used by this test transport");
  }

  [[nodiscard]] a2a::core::Result<std::size_t> Write(const char* buffer, std::size_t size) override {
    body.append(buffer, size);
    return size;
  }

  std::string body;
};

class SingleLiveEventSession final : public a2a::server::ServerStreamSession {
 public:
  explicit SingleLiveEventSession(lf::a2a::v1::StreamResponse event) : event_(std::move(event)) {}

  [[nodiscard]] a2a::core::Result<std::optional<lf::a2a::v1::StreamResponse>> Next() override {
    if (consumed_) {
      return std::optional<lf::a2a::v1::StreamResponse>{};
    }
    consumed_ = true;
    return std::optional<lf::a2a::v1::StreamResponse>{event_};
  }

  [[nodiscard]] bool IsLive() const noexcept override { return true; }

 private:
  lf::a2a::v1::StreamResponse event_;
  bool consumed_ = false;
};

class FakeExecutor final : public a2a::server::AgentExecutor {
 public:
  a2a::core::Result<lf::a2a::v1::SendMessageResponse> SendMessage(const lf::a2a::v1::SendMessageRequest& request,
                                                                  a2a::server::RequestContext& context) override {
    observed_request_id = context.request_id.value_or("missing");
    lf::a2a::v1::SendMessageResponse response;
    auto* message = response.mutable_message();
    message->set_role(lf::a2a::v1::ROLE_AGENT);
    message->set_task_id(request.message().task_id());
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
    if (request.id() == "missing") {
      return a2a::core::Error::RemoteProtocol("task not found").WithProtocolCode("TASK_NOT_FOUND");
    }

    observed_history_length = request.history_length();
    lf::a2a::v1::Task task;
    task.set_id(request.id());
    return task;
  }

  a2a::core::Result<a2a::server::ListTasksResponse> ListTasks(const a2a::server::ListTasksRequest& request,
                                                              a2a::server::RequestContext& context) override {
    (void)context;
    observed_page_size = request.page_size;
    observed_page_token = request.page_token;

    a2a::server::ListTasksResponse response;
    lf::a2a::v1::Task task;
    task.set_id("task-1");
    response.tasks.push_back(task);
    response.next_page_token = "next-token";
    return response;
  }

  a2a::core::Result<lf::a2a::v1::Task> CancelTask(const lf::a2a::v1::CancelTaskRequest& request,
                                                  a2a::server::RequestContext& context) override {
    (void)context;
    lf::a2a::v1::Task task;
    task.set_id(request.id());
    task.mutable_status()->set_state(lf::a2a::v1::TASK_STATE_CANCELED);
    return task;
  }

  a2a::core::Result<std::unique_ptr<a2a::server::ServerStreamSession>> SubscribeTask(
      const lf::a2a::v1::GetTaskRequest& request, a2a::server::RequestContext& context) override {
    auto task = GetTask(request, context);
    if (!task.ok()) {
      return task.error();
    }
    lf::a2a::v1::StreamResponse event;
    *event.mutable_task() = task.value();
    return std::unique_ptr<a2a::server::ServerStreamSession>(std::make_unique<SingleLiveEventSession>(event));
  }

  std::string observed_request_id;
  int observed_history_length = -1;
  std::size_t observed_page_size = 0;
  std::string observed_page_token;
};

TEST(RestTransportTest, ExposesCentralRouteTable) {
  const auto& routes = a2a::server::RestTransport::Routes();

  ASSERT_EQ(routes.size(), 10U);
  EXPECT_EQ(routes[0].method, "POST");
  EXPECT_EQ(routes[0].path_pattern, "/message:send");
  EXPECT_EQ(routes[1].path_pattern, "/message:stream");
  EXPECT_EQ(routes[2].path_pattern, "/tasks/{id}");
  EXPECT_EQ(routes[3].path_pattern, "/tasks");
  EXPECT_EQ(routes[4].path_pattern, "/tasks/{id}:cancel");
  EXPECT_EQ(routes[6].path_pattern, "/tasks/{task_id}/pushNotificationConfigs");
  EXPECT_EQ(routes[7].path_pattern, "/tasks/{task_id}/pushNotificationConfigs/{id}");
}

TEST(RestTransportTest, DispatchesSendMessageFromJsonBody) {
  FakeExecutor executor;
  a2a::server::Dispatcher dispatcher(&executor);
  a2a::server::RestTransport transport(&dispatcher);

  a2a::server::RestRequest request;
  request.method = "POST";
  request.path = "/message:send";
  request.body = R"({"message":{"messageId":"msg-1","role":"ROLE_USER","parts":[{"text":"hello"}],"taskId":"t-42"}})";
  request.context.request_id = "req-9";

  const auto response = transport.Handle(request);
  ASSERT_TRUE(response.ok());
  EXPECT_EQ(response.value().http_status, 200);
  EXPECT_NE(response.value().body.find("ROLE_AGENT"), std::string::npos);
  EXPECT_EQ(executor.observed_request_id, "req-9");
}

TEST(RestTransportTest, DispatchesGetTaskUsingPathAndQuery) {
  FakeExecutor executor;
  a2a::server::Dispatcher dispatcher(&executor);
  a2a::server::RestTransport transport(&dispatcher);

  a2a::server::RestRequest request;
  request.method = "GET";
  request.path = "/tasks/task-99";
  request.query_params["historyLength"] = "20";

  const auto response = transport.Handle(request);
  ASSERT_TRUE(response.ok());
  EXPECT_EQ(response.value().http_status, 200);
  EXPECT_NE(response.value().body.find("task-99"), std::string::npos);
  EXPECT_EQ(executor.observed_history_length, 20);
}

TEST(RestTransportTest, DispatchesListTasksUsingQuery) {
  FakeExecutor executor;
  a2a::server::Dispatcher dispatcher(&executor);
  a2a::server::RestTransport transport(&dispatcher);

  a2a::server::RestRequest request;
  request.method = "GET";
  request.path = "/tasks";
  request.query_params["pageSize"] = "15";
  request.query_params["pageToken"] = "page-2";

  const auto response = transport.Handle(request);
  ASSERT_TRUE(response.ok());
  EXPECT_EQ(response.value().http_status, 200);
  EXPECT_NE(response.value().body.find("nextPageToken"), std::string::npos);
  EXPECT_EQ(executor.observed_page_size, 15U);
  EXPECT_EQ(executor.observed_page_token, "page-2");
}

TEST(RestTransportTest, DispatchesCancelTaskFromPath) {
  FakeExecutor executor;
  a2a::server::Dispatcher dispatcher(&executor);
  a2a::server::RestTransport transport(&dispatcher);

  a2a::server::RestRequest request;
  request.method = "POST";
  request.path = "/tasks/task-55:cancel";

  const auto response = transport.Handle(request);
  ASSERT_TRUE(response.ok());
  EXPECT_EQ(response.value().http_status, 200);
  EXPECT_NE(response.value().body.find("task-55"), std::string::npos);
}

TEST(RestTransportTest, MapsDispatcherErrorsToStructuredHttpErrorBody) {
  FakeExecutor executor;
  a2a::server::Dispatcher dispatcher(&executor);
  a2a::server::RestTransport transport(&dispatcher);

  a2a::server::RestRequest request;
  request.method = "GET";
  request.path = "/tasks/missing";

  const auto response = transport.Handle(request);
  ASSERT_TRUE(response.ok());
  EXPECT_EQ(response.value().http_status, 502);
  EXPECT_NE(response.value().body.find("TASK_NOT_FOUND"), std::string::npos);
  EXPECT_NE(response.value().body.find("TASK_NOT_FOUND"), std::string::npos);
}

TEST(RestTransportTest, ReturnsNotFoundForUnknownRoute) {
  FakeExecutor executor;
  a2a::server::Dispatcher dispatcher(&executor);
  a2a::server::RestTransport transport(&dispatcher);

  a2a::server::RestRequest request;
  request.method = "DELETE";
  request.path = "/tasks/task-1";

  const auto response = transport.Handle(request);
  ASSERT_TRUE(response.ok());
  EXPECT_EQ(response.value().http_status, 404);
}

TEST(RestTransportTest, RejectsMalformedQueryParameters) {
  FakeExecutor executor;
  a2a::server::Dispatcher dispatcher(&executor);
  a2a::server::RestTransport transport(&dispatcher);

  a2a::server::RestRequest request;
  request.method = "GET";
  request.path = "/tasks";
  request.query_params["pageSize"] = "abc";

  const auto response = transport.Handle(request);
  ASSERT_TRUE(response.ok());
  EXPECT_EQ(response.value().http_status, 404);
}

TEST(RestTransportTest, RejectsUnsupportedPushNotificationEndpoints) {
  FakeExecutor executor;
  a2a::server::Dispatcher dispatcher(&executor);
  a2a::server::RestTransport transport(&dispatcher);

  a2a::server::RestRequest request;
  request.method = "POST";
  request.path = "/tasks/task-1/pushNotificationConfigs";
  request.body = R"({"id":"push-1","url":"http://127.0.0.1/webhook"})";

  const auto response = transport.Handle(request);
  ASSERT_TRUE(response.ok());
  EXPECT_EQ(response.value().http_status, 400);
  EXPECT_NE(response.value().body.find("PUSH_NOTIFICATION_NOT_SUPPORTED"), std::string::npos);
}

TEST(RestTransportTest, SupportsSubscribeEndpointForNonTerminalTask) {
  FakeExecutor executor;
  a2a::server::Dispatcher dispatcher(&executor);
  a2a::server::RestTransport transport(&dispatcher);

  a2a::server::RestRequest request;
  request.method = "POST";
  request.path = "/tasks/task-77:subscribe";

  const auto response = transport.Handle(request);
  ASSERT_TRUE(response.ok());
  EXPECT_EQ(response.value().http_status, 200);
  EXPECT_EQ(response.value().headers.at("Content-Type"), "text/event-stream");
  ASSERT_TRUE(response.value().stream_writer);
  RecordingHttpTransport output;
  const auto write = response.value().stream_writer(output);
  ASSERT_TRUE(write.ok()) << write.error().message();
  EXPECT_NE(output.body.find("task-77"), std::string::npos);
}

}  // namespace
