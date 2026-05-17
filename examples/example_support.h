#pragma once

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "a2a/core/error.h"
#include "a2a/core/protocol_codes.h"
#include "a2a/server/server.h"
#include "a2a/v1/a2a.pb.h"

namespace a2a::examples {
namespace {
constexpr std::string_view kGeneratedTextContent = "Generated text content";
constexpr std::string_view kOutputFilename = "output.txt";
constexpr std::string_view kTextPlainMediaType = "text/plain";
}  // namespace

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
    if (!request.has_message() || request.message().parts_size() == 0) {
      return core::Error::Validation("message with at least one part is required");
    }
    const bool has_explicit_task_id = !request.message().task_id().empty();
    std::string task_id = request.message().task_id();
    if (task_id.empty()) {
      if (!request.message().message_id().empty()) {
        task_id = "task-" + request.message().message_id();
      } else {
        ++generated_task_counter_;
        task_id = "example-task-" + std::to_string(generated_task_counter_);
      }
    }
    if (has_explicit_task_id && !tasks_.contains(task_id)) {
      return core::Error::RemoteProtocol("task not found")
          .WithHttpStatus(404)
          .WithProtocolCode(std::string(core::protocol_codes::kTaskNotFound));
    }

    lf::a2a::v1::Task task = tasks_.contains(task_id) ? tasks_.at(task_id) : lf::a2a::v1::Task{};
    if (!tasks_.contains(task_id)) {
      ordered_ids_.push_back(task_id);
    }
    task.set_id(task_id);
    if (task.context_id().empty()) {
      if (!request.message().context_id().empty()) {
        task.set_context_id(request.message().context_id());
      } else {
        task.set_context_id("ctx-" + task_id);
      }
    }
    *task.add_history() = request.message();
    task.mutable_status()->set_state(lf::a2a::v1::TASK_STATE_WORKING);
    task.mutable_status()->mutable_message()->set_role(lf::a2a::v1::ROLE_AGENT);
    task.mutable_status()->mutable_message()->set_message_id("status-" + task_id);
    task.mutable_status()->mutable_message()->add_parts()->set_text("ack");
    ++status_timestamp_counter_;
    task.mutable_status()->mutable_timestamp()->set_seconds(
        static_cast<int64_t>(status_timestamp_counter_));

    task.clear_artifacts();
    auto* text_artifact = task.add_artifacts();
    text_artifact->set_artifact_id("artifact-text-" + task_id);
    text_artifact->set_name("text-artifact");
    text_artifact->add_parts()->set_text(std::string(kGeneratedTextContent));

    auto* file_artifact = task.add_artifacts();
    file_artifact->set_artifact_id("artifact-file-" + task_id);
    file_artifact->set_name("file-artifact");
    auto* file_part = file_artifact->add_parts();
    file_part->set_raw("generated file content");
    file_part->set_filename(std::string(kOutputFilename));
    file_part->set_media_type(std::string(kTextPlainMediaType));

    auto* file_url_artifact = task.add_artifacts();
    file_url_artifact->set_artifact_id("artifact-file-url-" + task_id);
    file_url_artifact->set_name("file-url-artifact");
    auto* file_url_part = file_url_artifact->add_parts();
    file_url_part->set_url("https://example.test/output.txt");
    file_url_part->set_filename(std::string(kOutputFilename));
    file_url_part->set_media_type(std::string(kTextPlainMediaType));

    auto* data_artifact = task.add_artifacts();
    data_artifact->set_artifact_id("artifact-data-" + task_id);
    data_artifact->set_name("data-artifact");
    auto* data_part = data_artifact->add_parts();
    auto* data_fields = data_part->mutable_data()->mutable_struct_value()->mutable_fields();
    (*data_fields)["key"].set_string_value("value");
    (*data_fields)["count"].set_number_value(42);

    tasks_[task_id] = task;

    lf::a2a::v1::SendMessageResponse response;
    response.mutable_message()->set_role(lf::a2a::v1::ROLE_AGENT);
    response.mutable_message()->set_message_id("response-" + task_id);
    response.mutable_message()->set_task_id(task_id);
    response.mutable_message()->set_context_id(task.context_id());
    response.mutable_message()->add_parts()->set_text("ack");
    std::string request_text;
    if (request.message().parts_size() > 0 && request.message().parts(0).has_text()) {
      request_text = request.message().parts(0).text();
    }
    if (request_text.find("message response") != std::string::npos) {
      // Keep message payload set.
    } else {
      *response.mutable_task() = task;
    }
    return response;
  }

  core::Result<std::unique_ptr<server::ServerStreamSession>> SendStreamingMessage(
      const lf::a2a::v1::SendMessageRequest& request, server::RequestContext& context) override {
    (void)context;
    std::string task_id = request.message().task_id();
    if (task_id.empty()) {
      if (!request.message().message_id().empty()) {
        task_id = "task-" + request.message().message_id();
      } else {
        ++generated_task_counter_;
        task_id = "example-task-" + std::to_string(generated_task_counter_);
      }
    }

    if (!tasks_.contains(task_id)) {
      lf::a2a::v1::Task task;
      task.set_id(task_id);
      task.set_context_id("ctx-" + task_id);
      task.mutable_status()->set_state(lf::a2a::v1::TASK_STATE_WORKING);
      tasks_[task_id] = task;
      ordered_ids_.push_back(task_id);
    }

    lf::a2a::v1::StreamResponse working;
    working.mutable_status_update()->set_task_id(task_id);
    working.mutable_status_update()->mutable_status()->set_state(lf::a2a::v1::TASK_STATE_WORKING);

    lf::a2a::v1::StreamResponse completed;
    completed.mutable_status_update()->set_task_id(task_id);
    completed.mutable_status_update()->mutable_status()->set_state(
        lf::a2a::v1::TASK_STATE_COMPLETED);

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
    if (!tasks_.contains(request.id())) {
      return core::Error::RemoteProtocol("task not found")
          .WithHttpStatus(404)
          .WithProtocolCode(std::string(core::protocol_codes::kTaskNotFound));
    }
    lf::a2a::v1::Task task = tasks_.at(request.id());
    if (request.has_history_length()) {
      const int keep = request.history_length();
      if (keep <= 0) {
        task.clear_history();
      } else if (task.history_size() > keep) {
        task.mutable_history()->DeleteSubrange(0, task.history_size() - keep);
      }
    }
    return task;
  }

  core::Result<server::ListTasksResponse> ListTasks(const server::ListTasksRequest& request,
                                                    server::RequestContext& context) override {
    (void)context;
    std::vector<lf::a2a::v1::Task> filtered;
    for (const auto& id : ordered_ids_) {
      const auto it = tasks_.find(id);
      if (it == tasks_.end()) continue;
      const auto& task = it->second;
      if (!request.context_id.empty() && task.context_id() != request.context_id) continue;
      if (request.status_filter.has_value() && task.status().state() != *request.status_filter)
        continue;
      filtered.push_back(task);
    }

    std::stable_sort(
        filtered.begin(), filtered.end(),
        [](const lf::a2a::v1::Task& lhs, const lf::a2a::v1::Task& rhs) {
          const auto lhs_s = lhs.status().has_timestamp() ? lhs.status().timestamp().seconds() : 0;
          const auto rhs_s = rhs.status().has_timestamp() ? rhs.status().timestamp().seconds() : 0;
          return lhs_s > rhs_s;
        });

    std::size_t offset = 0;
    if (!request.page_token.empty()) {
      const auto* b = request.page_token.data();
      const auto* e = b + request.page_token.size();
      const auto p = std::from_chars(b, e, offset);
      if (p.ec != std::errc() || p.ptr != e) {
        return core::Error::Validation(
            "ListTasksRequest.page_token must be a non-negative integer");
      }
    }
    if (offset > filtered.size()) {
      return core::Error::Validation("ListTasksRequest.page_token exceeds available task count");
    }

    const std::size_t page_size = request.page_size == 0 ? 50U : request.page_size;
    server::ListTasksResponse response;
    response.total_size = filtered.size();

    for (std::size_t i = offset; i < filtered.size(); ++i) {
      if (response.tasks.size() >= page_size) {
        response.next_page_token = std::to_string(i);
        break;
      }
      auto task = filtered[i];
      if (!request.include_artifacts) {
        task.clear_artifacts();
      }
      if (request.history_length.has_value()) {
        const auto keep = *request.history_length;
        if (keep == 0) {
          task.clear_history();
        } else if (static_cast<std::size_t>(task.history_size()) > keep) {
          task.mutable_history()->DeleteSubrange(
              0, static_cast<int>(static_cast<std::size_t>(task.history_size()) - keep));
        }
      }
      response.tasks.push_back(std::move(task));
    }
    response.page_size = response.tasks.size();
    return response;
  }

  core::Result<lf::a2a::v1::Task> CancelTask(const lf::a2a::v1::CancelTaskRequest& request,
                                             server::RequestContext& context) override {
    (void)context;
    if (!tasks_.contains(request.id())) {
      return core::Error::RemoteProtocol("task not found")
          .WithHttpStatus(404)
          .WithProtocolCode(std::string(core::protocol_codes::kTaskNotFound));
    }
    auto task = tasks_.at(request.id());
    task.mutable_status()->set_state(lf::a2a::v1::TASK_STATE_CANCELED);
    tasks_[request.id()] = task;
    return task;
  }

 private:
  std::unordered_map<std::string, lf::a2a::v1::Task> tasks_;
  std::vector<std::string> ordered_ids_;
  std::uint64_t generated_task_counter_ = 0;
  std::uint64_t status_timestamp_counter_ = 0;
};

inline lf::a2a::v1::AgentCard BuildRestAgentCard(std::string_view name, std::string_view url) {
  lf::a2a::v1::AgentCard card;
  card.set_name(std::string(name));
  card.set_description("example rest agent");
  card.set_version("1.0.0");
  card.add_default_input_modes("text/plain");
  card.add_default_output_modes("text/plain");
  auto* iface = card.add_supported_interfaces();
  iface->set_url(std::string(url));
  iface->set_protocol_binding("HTTP+JSON");
  iface->set_protocol_version("1.0");
  return card;
}

inline lf::a2a::v1::AgentCard BuildJsonRpcAgentCard(std::string_view name, std::string_view url) {
  lf::a2a::v1::AgentCard card;
  card.set_name(std::string(name));
  card.set_description("example json-rpc agent");
  card.set_version("1.0.0");
  card.add_default_input_modes("text/plain");
  card.add_default_output_modes("text/plain");
  auto* iface = card.add_supported_interfaces();
  iface->set_url(std::string(url));
  iface->set_protocol_binding("JSONRPC");
  iface->set_protocol_version("1.0");
  return card;
}

}  // namespace a2a::examples
