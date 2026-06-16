// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

#include "a2a/core/agent_card_builder.h"
#include "a2a/core/error.h"
#include "a2a/server/agent_executor.h"
#include "a2a/server/request_context.h"
#include "a2a/server/rest_transport.h"
#include "a2a/server/server_stream_session.h"
#include "a2a/server/tasks/list_tasks.h"
#include "a2a/server/tasks/task_store.h"
#include "a2a/v1/a2a.pb.h"

namespace a2a::tests::support {

class StoreExecutor final : public server::AgentExecutor {
 public:
  explicit StoreExecutor(server::TaskStore* store) : store_(store) {}

  core::Result<lf::a2a::v1::SendMessageResponse> SendMessage(const lf::a2a::v1::SendMessageRequest& request,
                                                             server::RequestContext& context) override {
    (void)context;
    if (request.message().task_id().empty()) {
      return core::Error::Validation("message.task_id is required");
    }

    lf::a2a::v1::Task task;
    task.set_id(request.message().task_id());
    task.mutable_status()->set_state(lf::a2a::v1::TASK_STATE_WORKING);
    const auto saved = store_->CreateOrUpdate(task);
    if (!saved.ok()) {
      return saved.error();
    }

    lf::a2a::v1::SendMessageResponse response;
    *response.mutable_task() = task;
    return response;
  }

  core::Result<std::unique_ptr<server::ServerStreamSession>> SendStreamingMessage(
      const lf::a2a::v1::SendMessageRequest& request, server::RequestContext& context) override {
    (void)request;
    (void)context;
    return core::Error::Validation("streaming not implemented");
  }

  core::Result<lf::a2a::v1::Task> GetTask(const lf::a2a::v1::GetTaskRequest& request,
                                          server::RequestContext& context) override {
    (void)context;
    return store_->Get(request.id());
  }

  core::Result<server::ListTasksResponse> ListTasks(const server::ListTasksRequest& request,
                                                    server::RequestContext& context) override {
    (void)context;
    return store_->List(request);
  }

  core::Result<lf::a2a::v1::Task> CancelTask(const lf::a2a::v1::CancelTaskRequest& request,
                                             server::RequestContext& context) override {
    (void)context;
    return store_->Cancel(request.id());
  }

 private:
  server::TaskStore* store_;
};

inline lf::a2a::v1::AgentCard BuildRestAgentCard(std::string_view name, std::string_view url) {
  return a2a::core::AgentCardBuilder::RestPreset(name, url).Build();
}

inline lf::a2a::v1::AgentCard BuildJsonRpcAgentCard(std::string_view name, std::string_view url) {
  return a2a::core::AgentCardBuilder::JsonRpcPreset(name, url).Build();
}

inline server::HttpServerRequest MakeHttpRequest(std::string method, std::string target,
                                                 std::unordered_map<std::string, std::string> headers = {},
                                                 std::string body = {}, std::string remote = {}) {
  return server::HttpServerRequest{.method = std::move(method),
                                   .target = std::move(target),
                                   .headers = std::move(headers),
                                   .body = std::move(body),
                                   .remote_address = std::move(remote)};
}

}  // namespace a2a::tests::support
