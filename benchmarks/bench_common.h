// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <algorithm>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

#include "a2a/core/agent_card_builder.h"
#include "a2a/core/protocol_bindings.h"
#include "a2a/core/protojson.h"
#include "a2a/core/result.h"
#include "a2a/core/version.h"
#include "a2a/server/rest_server_transport.h"
#include "a2a/server/server.h"
#include "a2a/v1/a2a.pb.h"

namespace a2a::bench {

inline constexpr std::string_view kVersion = "1.0";
inline constexpr std::string_view kTextPlain = "text/plain";
inline constexpr std::string_view kTaskId = "task-bench-1";
inline constexpr std::string_view kContextId = "context-bench-1";
inline constexpr std::string_view kMessageId = "message-bench-1";
inline constexpr std::string_view kBenchText = "hello benchmark";
inline constexpr std::size_t kTaskCount = 1000;
inline constexpr std::size_t kLargeHistoryCount = 100;

inline lf::a2a::v1::Message BuildMessage(std::string_view message_id = kMessageId, std::string_view task_id = kTaskId,
                                         std::string_view context_id = kContextId) {
  lf::a2a::v1::Message message;
  message.set_message_id(std::string(message_id));
  message.set_task_id(std::string(task_id));
  message.set_context_id(std::string(context_id));
  message.set_role(lf::a2a::v1::ROLE_USER);
  auto* part = message.add_parts();
  part->set_text(std::string(kBenchText));
  part->set_media_type(std::string(kTextPlain));
  return message;
}

inline lf::a2a::v1::SendMessageRequest BuildSendMessageRequest(std::string_view task_id = kTaskId) {
  lf::a2a::v1::SendMessageRequest request;
  *request.mutable_message() = BuildMessage(kMessageId, task_id, kContextId);
  request.mutable_configuration()->set_return_immediately(false);
  return request;
}

inline lf::a2a::v1::Task BuildTask(std::string_view id = kTaskId, std::size_t history_count = 1) {
  lf::a2a::v1::Task task;
  task.set_id(std::string(id));
  task.set_context_id(std::string(kContextId));
  task.mutable_status()->set_state(lf::a2a::v1::TASK_STATE_SUBMITTED);
  task.mutable_status()->mutable_timestamp()->set_seconds(1'704'067'200);
  task.mutable_status()->mutable_timestamp()->set_nanos(123'000'000);
  for (std::size_t index = 0; index < history_count; ++index) {
    *task.add_history() = BuildMessage("message-history-" + std::to_string(index), id, kContextId);
  }
  return task;
}

inline lf::a2a::v1::StreamResponse BuildStreamResponse() {
  lf::a2a::v1::StreamResponse response;
  *response.mutable_task() = BuildTask();
  return response;
}

inline lf::a2a::v1::AgentCard BuildAgentCard(std::size_t interface_count = 1, std::size_t skill_count = 1) {
  lf::a2a::v1::AgentCard card;
  card.set_name("Benchmark Agent");
  card.set_description("Benchmark agent card");
  card.set_version(std::string(core::Version::kAgentCardVersion));
  card.add_default_input_modes(std::string(kTextPlain));
  card.add_default_output_modes(std::string(kTextPlain));
  card.mutable_capabilities()->set_streaming(true);
  for (std::size_t index = 0; index < interface_count; ++index) {
    auto* iface = card.add_supported_interfaces();
    iface->set_protocol_binding(index == 0 ? std::string(core::protocol_bindings::kHttpJson)
                                           : std::string(core::protocol_bindings::kJsonRpc));
    iface->set_protocol_version(std::string(kVersion));
    iface->set_url("http://localhost:8080/a2a/" + std::to_string(index));
  }
  for (std::size_t index = 0; index < skill_count; ++index) {
    auto* skill = card.add_skills();
    skill->set_id("skill-" + std::to_string(index));
    skill->set_name("Skill " + std::to_string(index));
    skill->set_description("Benchmark skill");
    skill->add_tags("benchmark");
  }
  return card;
}

class StaticExecutor final : public server::AgentExecutor {
 public:
  core::Result<lf::a2a::v1::SendMessageResponse> SendMessage(const lf::a2a::v1::SendMessageRequest& request,
                                                             server::RequestContext& context) override {
    (void)context;
    lf::a2a::v1::SendMessageResponse response;
    lf::a2a::v1::Task task = BuildTask(request.message().task_id());
    *response.mutable_task() = std::move(task);
    return response;
  }

  core::Result<std::unique_ptr<server::ServerStreamSession>> SendStreamingMessage(
      const lf::a2a::v1::SendMessageRequest& request, server::RequestContext& context) override {
    (void)request;
    (void)context;
    return core::Error::Validation("streaming is not used by benchmarks");
  }

  core::Result<lf::a2a::v1::Task> GetTask(const lf::a2a::v1::GetTaskRequest& request,
                                          server::RequestContext& context) override {
    (void)context;
    const std::size_t history_count =
        request.has_history_length() ? static_cast<std::size_t>(std::max(request.history_length(), 0)) : std::size_t{1};
    return BuildTask(request.id(), history_count);
  }

  core::Result<server::ListTasksResponse> ListTasks(const server::ListTasksRequest& request,
                                                    server::RequestContext& context) override {
    (void)request;
    (void)context;
    server::ListTasksResponse response;
    response.tasks.push_back(BuildTask());
    response.page_size = response.tasks.size();
    response.total_size = response.tasks.size();
    return response;
  }

  core::Result<lf::a2a::v1::Task> CancelTask(const lf::a2a::v1::CancelTaskRequest& request,
                                             server::RequestContext& context) override {
    (void)context;
    auto task = BuildTask(request.id());
    task.mutable_status()->set_state(lf::a2a::v1::TASK_STATE_CANCELED);
    return task;
  }
};

inline server::HttpServerRequest BuildHttpRequest(std::string method, std::string target, std::string body = {}) {
  return {.method = std::move(method),
          .target = std::move(target),
          .headers = {{"A2A-Version", std::string(kVersion)}, {"Content-Type", "application/json"}},
          .body = std::move(body),
          .remote_address = "127.0.0.1"};
}

inline std::string JsonOrDie(const google::protobuf::Message& message) {
  auto json = core::MessageToJson(message);
  if (!json.ok()) {
    return "{}";
  }
  return json.value();
}

}  // namespace a2a::bench
