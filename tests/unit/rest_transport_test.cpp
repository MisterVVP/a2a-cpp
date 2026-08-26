// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "a2a/server/rest_transport.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "a2a/core/http_constants.h"
#include "a2a/server/http_adapter.h"

namespace {

constexpr std::string_view kSubscribedTaskId = "task-77";
constexpr std::string_view kSubscribeTaskPath = "/tasks/task-77:subscribe";
constexpr int kRequestedHistoryLength = 20;
constexpr std::string_view kContentTypeHeaderName = "Content-Type";
constexpr std::string_view kApplicationJsonWithCharsetContentType = "Application/JSON; charset=utf-8";
constexpr std::string_view kTextPlainContentType = "text/plain";
constexpr std::string_view kContentTypeNotSupportedReason = "CONTENT_TYPE_NOT_SUPPORTED";
constexpr std::string_view kContentTypeNotSupportedProtocolCode = "-32005";

class RecordingHttpTransport final : public a2a::server::HttpByteTransport {
 public:
  [[nodiscard]] a2a::core::Result<std::size_t> Read(char* buffer, std::size_t size) override {
    (void)buffer;
    (void)size;
    return a2a::core::Error::Internal("read is not used by this test transport");
  }

  [[nodiscard]] a2a::core::Result<std::size_t> Write(const char* buffer, std::size_t size) override {
    if (fail_heartbeat && std::string_view(buffer, size) == a2a::core::http::kSseHeartbeat) {
      return a2a::core::Error::Network("client disconnected");
    }
    body.append(buffer, size);
    return size;
  }

  bool fail_heartbeat = false;
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

  [[nodiscard]] bool IsLive() const noexcept override { return !consumed_; }

 private:
  lf::a2a::v1::StreamResponse event_;
  bool consumed_ = false;
};

class HeartbeatEventSession final : public a2a::server::ServerStreamSession {
 public:
  HeartbeatEventSession(lf::a2a::v1::StreamResponse event, std::shared_ptr<std::atomic_bool> cancelled)
      : event_(std::move(event)), cancelled_(std::move(cancelled)) {}

  [[nodiscard]] a2a::core::Result<std::optional<lf::a2a::v1::StreamResponse>> Next() override {
    return NextFor(std::chrono::milliseconds::zero());
  }

  [[nodiscard]] a2a::core::Result<std::optional<lf::a2a::v1::StreamResponse>> NextFor(
      std::chrono::milliseconds timeout) override {
    (void)timeout;
    if (!consumed_) {
      consumed_ = true;
      return std::optional<lf::a2a::v1::StreamResponse>{event_};
    }
    return std::optional<lf::a2a::v1::StreamResponse>{};
  }

  [[nodiscard]] bool IsLive() const noexcept override { return !cancelled_->load(); }

  void Cancel() noexcept override { cancelled_->store(true); }

 private:
  lf::a2a::v1::StreamResponse event_;
  std::shared_ptr<std::atomic_bool> cancelled_;
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
    if (streaming_session != nullptr) {
      return std::move(streaming_session);
    }
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
    if (heartbeat_cancellation != nullptr) {
      return std::unique_ptr<a2a::server::ServerStreamSession>(
          std::make_unique<HeartbeatEventSession>(event, heartbeat_cancellation));
    }
    return std::unique_ptr<a2a::server::ServerStreamSession>(std::make_unique<SingleLiveEventSession>(event));
  }

  std::string observed_request_id;
  int observed_history_length = -1;
  std::size_t observed_page_size = 0;
  std::string observed_page_token;
  std::shared_ptr<std::atomic_bool> heartbeat_cancellation;
  std::unique_ptr<a2a::server::ServerStreamSession> streaming_session;
};

class OrderedFiniteSession final : public a2a::server::ServerStreamSession {
 public:
  OrderedFiniteSession(std::vector<std::string> task_ids, std::shared_ptr<std::size_t> writes, bool fail_at_end)
      : task_ids_(std::move(task_ids)), writes_(std::move(writes)), fail_at_end_(fail_at_end) {}

  [[nodiscard]] a2a::core::Result<std::optional<lf::a2a::v1::StreamResponse>> Next() override {
    if (next_index_ > 0U && *writes_ < next_index_) {
      return a2a::core::Error::Internal("next event requested before previous event was written");
    }
    if (next_index_ == task_ids_.size()) {
      return fail_at_end_ ? a2a::core::Result<std::optional<lf::a2a::v1::StreamResponse>>(
                                a2a::core::Error::Internal("finite producer failed"))
                          : a2a::core::Result<std::optional<lf::a2a::v1::StreamResponse>>(
                                std::optional<lf::a2a::v1::StreamResponse>{});
    }
    lf::a2a::v1::StreamResponse event;
    event.mutable_task()->set_id(task_ids_[next_index_++]);
    return std::optional<lf::a2a::v1::StreamResponse>{std::move(event)};
  }

 private:
  std::vector<std::string> task_ids_;
  std::shared_ptr<std::size_t> writes_;
  bool fail_at_end_;
  std::size_t next_index_ = 0U;
};

class CountingHttpTransport final : public a2a::server::HttpByteTransport {
 public:
  explicit CountingHttpTransport(std::shared_ptr<std::size_t> writes) : writes_(std::move(writes)) {}
  a2a::core::Result<std::size_t> Read(char* buffer, std::size_t size) override {
    (void)buffer;
    (void)size;
    return a2a::core::Error::Internal("read is not used by this test transport");
  }
  a2a::core::Result<std::size_t> Write(const char* buffer, std::size_t size) override {
    body.append(buffer, size);
    ++*writes_;
    return size;
  }
  std::string body;

 private:
  std::shared_ptr<std::size_t> writes_;
};

TEST(RestTransportTest, ExposesCentralRouteTable) {
  const auto& routes = a2a::server::RestTransport::Routes();

  ASSERT_EQ(routes.size(), 11U);
  EXPECT_EQ(routes[0].method, "POST");
  EXPECT_EQ(routes[0].path_pattern, "/message:send");
  EXPECT_EQ(routes[1].path_pattern, "/message:stream");
  EXPECT_EQ(routes[2].path_pattern, "/tasks/{id}");
  EXPECT_EQ(routes[3].path_pattern, "/tasks");
  EXPECT_EQ(routes[4].path_pattern, "/tasks/{id}:cancel");
  EXPECT_EQ(routes[5].method, a2a::core::http::kMethodGet);
  EXPECT_EQ(routes[5].path_pattern, "/tasks/{id}:subscribe");
  EXPECT_EQ(routes[6].method, a2a::core::http::kMethodPost);
  EXPECT_EQ(routes[6].path_pattern, "/tasks/{id}:subscribe");
  EXPECT_EQ(routes[7].path_pattern, "/tasks/{task_id}/pushNotificationConfigs");
  EXPECT_EQ(routes[8].path_pattern, "/tasks/{task_id}/pushNotificationConfigs/{id}");
}

TEST(RestTransportTest, FiniteStreamWritesEachEventBeforeRequestingNext) {
  auto writes = std::make_shared<std::size_t>(0U);
  FakeExecutor executor;
  executor.streaming_session =
      std::make_unique<OrderedFiniteSession>(std::vector<std::string>{"first-task", "second-task"}, writes, false);
  a2a::server::Dispatcher dispatcher(&executor);
  a2a::server::RestTransport transport(&dispatcher);
  a2a::server::RestRequest request;
  request.method = a2a::core::http::kMethodPost;
  request.path = a2a::server::RestEndpointPaths::kSendStreamingMessage;
  request.headers[std::string(kContentTypeHeaderName)] = std::string(a2a::core::http::kContentTypeApplicationJson);
  request.body = R"({"message":{"role":"ROLE_USER","taskId":"request-task"}})";

  const auto response = transport.Handle(request);
  ASSERT_TRUE(response.ok()) << response.error().message();
  EXPECT_TRUE(response.value().body.empty());
  ASSERT_TRUE(response.value().stream_writer);
  CountingHttpTransport output(writes);
  const auto streamed = response.value().stream_writer(output);

  ASSERT_TRUE(streamed.ok()) << streamed.error().message();
  const auto first = output.body.find("first-task");
  const auto second = output.body.find("second-task");
  ASSERT_NE(first, std::string::npos);
  ASSERT_NE(second, std::string::npos);
  EXPECT_LT(first, second);
}

TEST(RestTransportTest, FiniteStreamPropagatesProducerFailure) {
  auto writes = std::make_shared<std::size_t>(0U);
  FakeExecutor executor;
  executor.streaming_session =
      std::make_unique<OrderedFiniteSession>(std::vector<std::string>{"only-task"}, writes, true);
  a2a::server::Dispatcher dispatcher(&executor);
  a2a::server::RestTransport transport(&dispatcher);
  a2a::server::RestRequest request;
  request.method = a2a::core::http::kMethodPost;
  request.path = a2a::server::RestEndpointPaths::kSendStreamingMessage;
  request.headers[std::string(kContentTypeHeaderName)] = std::string(a2a::core::http::kContentTypeApplicationJson);
  request.body = R"({"message":{"role":"ROLE_USER","taskId":"request-task"}})";

  const auto response = transport.Handle(request);
  ASSERT_TRUE(response.ok()) << response.error().message();
  CountingHttpTransport output(writes);
  const auto streamed = response.value().stream_writer(output);

  ASSERT_FALSE(streamed.ok());
  EXPECT_NE(output.body.find("only-task"), std::string::npos);
}

void ExpectUnsupportedContentType(std::string_view path, std::string_view body) {
  FakeExecutor executor;
  a2a::server::Dispatcher dispatcher(&executor);
  a2a::server::RestTransport transport(&dispatcher);

  a2a::server::RestRequest request;
  request.method = a2a::core::http::kMethodPost;
  request.path = path;
  request.headers[std::string(kContentTypeHeaderName)] = std::string(kTextPlainContentType);
  request.body = body;

  const auto response = transport.Handle(request);
  ASSERT_TRUE(response.ok());
  EXPECT_EQ(response.value().http_status, a2a::core::http::kStatusUnsupportedMediaType);
  EXPECT_NE(response.value().body.find(kContentTypeNotSupportedReason), std::string::npos);
  EXPECT_NE(response.value().body.find(kContentTypeNotSupportedProtocolCode), std::string::npos);
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

TEST(RestTransportTest, RejectsTextPlainContentTypeForSendMessage) {
  ExpectUnsupportedContentType(a2a::server::RestEndpointPaths::kSendMessage,
                               R"({"message":{"messageId":"msg-1","role":"ROLE_USER","parts":[{"text":"hello"}]}})");
}

TEST(RestTransportTest, RejectsTextPlainContentTypeForStreamingSendMessage) {
  ExpectUnsupportedContentType(a2a::server::RestEndpointPaths::kSendStreamingMessage,
                               R"({"message":{"messageId":"msg-1","role":"ROLE_USER","parts":[{"text":"hello"}]}})");
}

TEST(RestTransportTest, RejectsTextPlainContentTypeForPushConfigCreate) {
  ExpectUnsupportedContentType("/tasks/task-1/pushNotificationConfigs",
                               R"({"id":"push-1","url":"http://127.0.0.1/webhook"})");
}

TEST(RestTransportTest, AcceptsJsonContentTypeWithParametersForSendMessage) {
  FakeExecutor executor;
  a2a::server::Dispatcher dispatcher(&executor);
  a2a::server::RestTransport transport(&dispatcher);

  a2a::server::RestRequest request;
  request.method = a2a::core::http::kMethodPost;
  request.path = a2a::server::RestEndpointPaths::kSendMessage;
  request.headers[std::string(kContentTypeHeaderName)] = std::string(kApplicationJsonWithCharsetContentType);
  request.body = R"({"message":{"messageId":"msg-1","role":"ROLE_USER","parts":[{"text":"hello"}],"taskId":"t-42"}})";

  const auto response = transport.Handle(request);
  ASSERT_TRUE(response.ok());
  EXPECT_EQ(response.value().http_status, 200);
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

TEST(RestTransportTest, ListTasksUsesProtocolDefaultWhenPageSizeIsOmitted) {
  FakeExecutor executor;
  a2a::server::Dispatcher dispatcher(&executor);
  a2a::server::RestTransport transport(&dispatcher);

  a2a::server::RestRequest request;
  request.method = "GET";
  request.path = "/tasks";

  const auto response = transport.Handle(request);
  ASSERT_TRUE(response.ok());
  EXPECT_EQ(response.value().http_status, 200);
  EXPECT_EQ(executor.observed_page_size, a2a::server::kDefaultListTasksPageSize);
}

TEST(RestTransportTest, ListTasksResponseIncludesRequiredTopLevelFields) {
  FakeExecutor executor;
  a2a::server::Dispatcher dispatcher(&executor);
  a2a::server::RestTransport transport(&dispatcher);
  a2a::server::RestRequest request;
  request.method = "GET";
  request.path = "/tasks";
  const auto response = transport.Handle(request);
  ASSERT_TRUE(response.ok());
  EXPECT_NE(response.value().body.find(R"("tasks":)"), std::string::npos);
  EXPECT_NE(response.value().body.find(R"("pageSize":0)"), std::string::npos);
  EXPECT_NE(response.value().body.find(R"("totalSize":0)"), std::string::npos);
  EXPECT_NE(response.value().body.find(R"("nextPageToken":)"), std::string::npos);
}

TEST(RestTransportTest, ListTasksRejectsPageSizesOutsideProtocolRange) {
  FakeExecutor executor;
  a2a::server::Dispatcher dispatcher(&executor);
  a2a::server::RestTransport transport(&dispatcher);

  for (const std::string_view page_size : {"0", "101", "-1"}) {
    a2a::server::RestRequest request;
    request.method = "GET";
    request.path = "/tasks";
    request.query_params["pageSize"] = page_size;

    const auto response = transport.Handle(request);
    ASSERT_TRUE(response.ok());
    EXPECT_EQ(response.value().http_status, 400);
  }
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

TEST(RestTransportTest, RejectsMalformedListQueryWithBadRequest) {
  FakeExecutor executor;
  a2a::server::Dispatcher dispatcher(&executor);
  a2a::server::RestTransport transport(&dispatcher);

  a2a::server::RestRequest request;
  request.method = "GET";
  request.path = "/tasks";
  request.query_params["pageSize"] = "abc";

  const auto response = transport.Handle(request);
  ASSERT_TRUE(response.ok());
  EXPECT_EQ(response.value().http_status, 400);
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

void ExpectSubscribeEndpoint(std::string_view method) {
  FakeExecutor executor;
  a2a::server::Dispatcher dispatcher(&executor);
  a2a::server::RestTransport transport(&dispatcher);

  a2a::server::RestRequest request;
  request.method = method;
  request.path = kSubscribeTaskPath;

  const auto response = transport.Handle(request);
  ASSERT_TRUE(response.ok());
  EXPECT_EQ(response.value().http_status, 200);
  EXPECT_EQ(response.value().headers.at("Content-Type"), "text/event-stream");
  ASSERT_TRUE(response.value().stream_writer);
  RecordingHttpTransport output;
  const auto write = response.value().stream_writer(output);
  ASSERT_TRUE(write.ok()) << write.error().message();
  EXPECT_NE(output.body.find(kSubscribedTaskId), std::string::npos);
}

TEST(RestTransportTest, SupportsGetSubscribeEndpointForNonTerminalTask) {
  ExpectSubscribeEndpoint(a2a::core::http::kMethodGet);
}

TEST(RestTransportTest, SupportsPostSubscribeEndpointForNonTerminalTask) {
  ExpectSubscribeEndpoint(a2a::core::http::kMethodPost);
}

TEST(RestTransportTest, CancelsSubscriptionWhenHeartbeatDetectsDisconnect) {
  FakeExecutor executor;
  executor.heartbeat_cancellation = std::make_shared<std::atomic_bool>(false);
  a2a::server::Dispatcher dispatcher(&executor);
  a2a::server::RestTransport transport(&dispatcher);

  a2a::server::RestRequest request;
  request.method = a2a::core::http::kMethodGet;
  request.path = kSubscribeTaskPath;
  const auto response = transport.Handle(request);
  ASSERT_TRUE(response.ok());
  ASSERT_TRUE(response.value().stream_writer);

  RecordingHttpTransport output;
  output.fail_heartbeat = true;
  const auto write = response.value().stream_writer(output);

  ASSERT_FALSE(write.ok());
  EXPECT_TRUE(executor.heartbeat_cancellation->load());
  EXPECT_NE(output.body.find(kSubscribedTaskId), std::string::npos);
}

TEST(RestTransportTest, ForwardsSubscribeHistoryLength) {
  FakeExecutor executor;
  a2a::server::Dispatcher dispatcher(&executor);
  a2a::server::RestTransport transport(&dispatcher);

  a2a::server::RestRequest request;
  request.method = a2a::core::http::kMethodGet;
  request.path = kSubscribeTaskPath;
  request.query_params["historyLength"] = std::to_string(kRequestedHistoryLength);

  const auto response = transport.Handle(request);
  ASSERT_TRUE(response.ok());
  EXPECT_EQ(response.value().http_status, 200);
  EXPECT_EQ(executor.observed_history_length, kRequestedHistoryLength);
}

TEST(RestTransportTest, RejectsMalformedSubscribeHistoryLength) {
  FakeExecutor executor;
  a2a::server::Dispatcher dispatcher(&executor);
  a2a::server::RestTransport transport(&dispatcher);

  a2a::server::RestRequest request;
  request.method = a2a::core::http::kMethodGet;
  request.path = kSubscribeTaskPath;
  request.query_params["historyLength"] = "abc";

  const auto response = transport.Handle(request);
  ASSERT_TRUE(response.ok());
  EXPECT_EQ(response.value().http_status, 404);
  EXPECT_EQ(executor.observed_history_length, -1);
}

}  // namespace
