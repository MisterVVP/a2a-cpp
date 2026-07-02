// SPDX-License-Identifier: Apache-2.0

#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "a2a/client/client.h"
#include "a2a/client/json_rpc_transport.h"
#include "a2a/core/agent_card/agent_card_builder.h"
#include "a2a/core/url_utils.h"
#include "a2a/server/agent_executor.h"
#include "a2a/server/dispatcher.h"
#include "a2a/server/json_rpc_server_transport.h"
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
  a2a::server::JsonRpcServerTransport server(&dispatcher, {.rpc_path = "/rpc"});

  auto transport = std::make_unique<a2a::client::JsonRpcTransport>(
      a2a::client::ResolvedInterface{.transport = a2a::client::PreferredTransport::kJsonRpc,
                                     .url = "http://agent.local/rpc",
                                     .security_requirements = {},
                                     .security_schemes = {}},
      [&server](const a2a::client::HttpRequest& request) -> a2a::core::Result<a2a::client::HttpClientResponse> {
        const auto response = server.Handle({.method = request.method,
                                             .target = a2a::core::ExtractTargetPath(request.url),
                                             .headers = request.headers,
                                             .body = request.body,
                                             .remote_address = {}});
        if (!response.ok()) return response.error();
        return a2a::client::HttpClientResponse{.status_code = response.value().status_code,
                                               .headers = response.value().headers,
                                               .body = response.value().body};
      });
  a2a::client::A2AClient client(std::move(transport));
  const auto sent = client.SendMessage(BuildMessageRequest());
  if (!sent.ok()) {
    std::cerr << "json_rpc_server failed: " << sent.error().message() << '\n';
    return 1;
  }
  std::cout << "json_rpc_server task id: " << sent.value().task().id() << '\n';
  std::cout << "json_rpc_server state: " << sent.value().task().status().state() << '\n';
  return 0;
}
