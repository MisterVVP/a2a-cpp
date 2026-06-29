// SPDX-License-Identifier: Apache-2.0

#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "a2a/client/client.h"
#include "a2a/client/http_json_transport.h"
#include "a2a/core/agent_card/agent_card_builder.h"
#include "a2a/core/url_utils.h"
#include "a2a/server/agent_executor.h"
#include "a2a/server/dispatcher.h"
#include "a2a/server/rest_server_transport.h"
#include "a2a/server/server_stream_session.h"
namespace {

constexpr char kTaskId[] = "example-task-1";
constexpr char kContextId[] = "example-context-1";
constexpr char kMessageId[] = "example-message-1";
constexpr char kAgentReply[] = "ack";

class OneShotStreamSession final : public a2a::server::ServerStreamSession {
 public:
  explicit OneShotStreamSession(lf::a2a::v1::StreamResponse event) : event_(std::move(event)) {}

  a2a::core::Result<std::optional<lf::a2a::v1::StreamResponse>> Next() override {
    if (sent_) {
      return std::optional<lf::a2a::v1::StreamResponse>{};
    }
    sent_ = true;
    return std::optional<lf::a2a::v1::StreamResponse>{event_};
  }

 private:
  lf::a2a::v1::StreamResponse event_;
  bool sent_ = false;
};

class ExampleExecutor final : public a2a::server::AgentExecutor {
 public:
  a2a::core::Result<lf::a2a::v1::SendMessageResponse> SendMessage(const lf::a2a::v1::SendMessageRequest& request,
                                                                  a2a::server::RequestContext& context) override {
    (void)context;
    if (!request.has_message()) {
      return a2a::core::Error::Validation("message is required");
    }
    task_.set_id(request.message().task_id().empty() ? kTaskId : request.message().task_id());
    task_.set_context_id(request.message().context_id().empty() ? kContextId : request.message().context_id());
    task_.mutable_status()->set_state(lf::a2a::v1::TASK_STATE_COMPLETED);
    task_.mutable_status()->mutable_message()->set_role(lf::a2a::v1::ROLE_AGENT);
    task_.mutable_status()->mutable_message()->set_message_id("status-message-1");
    task_.mutable_status()->mutable_message()->add_parts()->set_text(kAgentReply);

    lf::a2a::v1::SendMessageResponse response;
    *response.mutable_task() = task_;
    return response;
  }

  a2a::core::Result<std::unique_ptr<a2a::server::ServerStreamSession>> SendStreamingMessage(
      const lf::a2a::v1::SendMessageRequest& request, a2a::server::RequestContext& context) override {
    auto sent = SendMessage(request, context);
    if (!sent.ok()) {
      return sent.error();
    }
    lf::a2a::v1::StreamResponse event;
    event.mutable_status_update()->set_task_id(task_.id());
    event.mutable_status_update()->set_context_id(task_.context_id());
    event.mutable_status_update()->mutable_status()->set_state(lf::a2a::v1::TASK_STATE_COMPLETED);
    std::unique_ptr<a2a::server::ServerStreamSession> stream = std::make_unique<OneShotStreamSession>(std::move(event));
    return stream;
  }

  a2a::core::Result<lf::a2a::v1::Task> GetTask(const lf::a2a::v1::GetTaskRequest& request,
                                               a2a::server::RequestContext& context) override {
    (void)context;
    if (!task_.id().empty() && request.id() == task_.id()) {
      return task_;
    }
    return a2a::core::Error::Validation("task not found");
  }

  a2a::core::Result<a2a::server::ListTasksResponse> ListTasks(const a2a::server::ListTasksRequest& request,
                                                              a2a::server::RequestContext& context) override {
    (void)request;
    (void)context;
    a2a::server::ListTasksResponse response;
    if (!task_.id().empty()) {
      response.tasks.push_back(task_);
    }
    return response;
  }

  a2a::core::Result<lf::a2a::v1::Task> CancelTask(const lf::a2a::v1::CancelTaskRequest& request,
                                                  a2a::server::RequestContext& context) override {
    (void)context;
    task_.set_id(request.id());
    task_.set_context_id(kContextId);
    task_.mutable_status()->set_state(lf::a2a::v1::TASK_STATE_CANCELED);
    return task_;
  }

 private:
  lf::a2a::v1::Task task_;
};

lf::a2a::v1::SendMessageRequest BuildMessageRequest() {
  lf::a2a::v1::SendMessageRequest request;
  request.mutable_message()->set_role(lf::a2a::v1::ROLE_USER);
  request.mutable_message()->set_message_id(kMessageId);
  request.mutable_message()->add_parts()->set_text("hello from example");
  return request;
}

}  // namespace
int main() {
  ExampleExecutor executor;
  a2a::server::Dispatcher dispatcher(&executor);
  a2a::server::RequestContext context;
  auto stream = executor.SendStreamingMessage(BuildMessageRequest(), context);
  if (!stream.ok()) {
    std::cerr << "streaming_server failed: " << stream.error().message() << '\n';
    return 1;
  }
  auto event = stream.value()->Next();
  if (!event.ok() || !event.value().has_value() || !event.value()->has_status_update()) {
    std::cerr << "streaming_server did not publish an event\n";
    return 1;
  }
  std::cout << "streaming_server event task id: " << event.value()->status_update().task_id() << '\n';
  std::cout << "streaming_server event state: " << event.value()->status_update().status().state() << '\n';
  return 0;
}
