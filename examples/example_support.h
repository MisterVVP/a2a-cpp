#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "a2a/core/error.h"
#include "a2a/server/server.h"
#include "a2a/v1/a2a.pb.h"

namespace a2a::examples {

inline std::string UrlToTarget(std::string_view url) {
  const std::size_t scheme = url.find("://");
  if (scheme == std::string_view::npos) {
    return std::string(url);
  }

  const std::size_t path_start = url.find('/', scheme + 3);
  if (path_start == std::string_view::npos) {
    return "/";
  }
  return std::string(url.substr(path_start));
}

class SequenceStreamSession final : public server::ServerStreamSession {
 public:
  explicit SequenceStreamSession(std::vector<lf::a2a::v1::StreamResponse> events)
      : events_(std::move(events)) {}

  [[nodiscard]] core::Result<std::optional<lf::a2a::v1::StreamResponse>> Next() override {
    if (index_ >= events_.size()) {
      return std::optional<lf::a2a::v1::StreamResponse>{};
    }
    return std::optional<lf::a2a::v1::StreamResponse>{events_[index_++]};
  }

 private:
  std::vector<lf::a2a::v1::StreamResponse> events_;
  std::size_t index_ = 0;
};

class ExampleExecutor final : public server::AgentExecutor {
 public:
  core::Result<lf::a2a::v1::SendMessageResponse> SendMessage(
      const lf::a2a::v1::SendMessageRequest& request, server::RequestContext& context) override {
    (void)context;
    const std::string task_id = request.message().task_id().empty() ? "example-task" : request.message().task_id();

    lf::a2a::v1::Task task;
    task.set_id(task_id);
    task.mutable_status()->set_state(lf::a2a::v1::TASK_STATE_WORKING);
    task.mutable_status()->mutable_message()->set_role("agent");
    task.mutable_status()->mutable_message()->add_parts()->mutable_text()->set_text("ack");
    task_ = task;

    lf::a2a::v1::SendMessageResponse response;
    *response.mutable_task() = task;
    return response;
  }

  core::Result<std::unique_ptr<server::ServerStreamSession>> SendStreamingMessage(
      const lf::a2a::v1::SendMessageRequest& request, server::RequestContext& context) override {
    (void)context;
    if (request.message().task_id().empty()) {
      return core::Error::Validation("message.task_id is required");
    }

    lf::a2a::v1::StreamResponse working;
    working.mutable_status_update()->set_task_id(request.message().task_id());
    working.mutable_status_update()->mutable_status()->set_state(lf::a2a::v1::TASK_STATE_WORKING);

    lf::a2a::v1::StreamResponse completed;
    completed.mutable_status_update()->set_task_id(request.message().task_id());
    completed.mutable_status_update()->mutable_status()->set_state(lf::a2a::v1::TASK_STATE_COMPLETED);
    completed.mutable_status_update()->set_final(true);

    std::vector<lf::a2a::v1::StreamResponse> events;
    events.push_back(working);
    events.push_back(completed);

    std::unique_ptr<server::ServerStreamSession> stream =
        std::make_unique<SequenceStreamSession>(std::move(events));
    return stream;
  }

  core::Result<lf::a2a::v1::Task> GetTask(const lf::a2a::v1::GetTaskRequest& request,
                                          server::RequestContext& context) override {
    (void)context;
    if (!task_.has_value() || request.id() != task_->id()) {
      return core::Error::RemoteProtocol("task not found").WithHttpStatus(404);
    }
    return *task_;
  }

  core::Result<server::ListTasksResponse> ListTasks(const server::ListTasksRequest& request,
                                                    server::RequestContext& context) override {
    (void)context;
    (void)request;
    server::ListTasksResponse response;
    if (task_.has_value()) {
      response.tasks.push_back(*task_);
    }
    return response;
  }

  core::Result<lf::a2a::v1::Task> CancelTask(const lf::a2a::v1::CancelTaskRequest& request,
                                             server::RequestContext& context) override {
    (void)context;
    if (!task_.has_value() || request.id() != task_->id()) {
      return core::Error::RemoteProtocol("task not found").WithHttpStatus(404);
    }
    task_->mutable_status()->set_state(lf::a2a::v1::TASK_STATE_CANCELED);
    return *task_;
  }

 private:
  std::optional<lf::a2a::v1::Task> task_;
};

inline lf::a2a::v1::AgentCard BuildRestAgentCard(std::string_view name, std::string_view url) {
  lf::a2a::v1::AgentCard card;
  card.set_protocol_version("1.0");
  card.set_name(std::string(name));
  auto* iface = card.add_supported_interfaces();
  iface->set_transport(lf::a2a::v1::TRANSPORT_PROTOCOL_REST);
  iface->set_url(std::string(url));
  return card;
}

inline lf::a2a::v1::AgentCard BuildJsonRpcAgentCard(std::string_view name, std::string_view url) {
  lf::a2a::v1::AgentCard card;
  card.set_protocol_version("1.0");
  card.set_name(std::string(name));
  auto* iface = card.add_supported_interfaces();
  iface->set_transport(lf::a2a::v1::TRANSPORT_PROTOCOL_JSON_RPC);
  iface->set_url(std::string(url));
  return card;
}

}  // namespace a2a::examples
