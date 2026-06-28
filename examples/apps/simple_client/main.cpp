// SPDX-License-Identifier: Apache-2.0

#include <iostream>
#include <memory>
#include <string>
#include <utility>

#include "a2a/client/client.h"
#include "a2a/core/error.h"

namespace {

constexpr char kTaskId[] = "simple-client-task";
constexpr char kMessageId[] = "simple-client-message";

class DeterministicTransport final : public a2a::client::ClientTransport {
 public:
  a2a::core::Result<lf::a2a::v1::SendMessageResponse> SendMessage(const lf::a2a::v1::SendMessageRequest& request,
                                                                  const a2a::client::CallOptions& options) override {
    (void)options;
    if (!request.has_message()) {
      return a2a::core::Error::Validation("message is required");
    }
    task_.set_id(kTaskId);
    task_.set_context_id("simple-client-context");
    task_.mutable_status()->set_state(lf::a2a::v1::TASK_STATE_COMPLETED);
    lf::a2a::v1::SendMessageResponse response;
    *response.mutable_task() = task_;
    return response;
  }

  a2a::core::Result<lf::a2a::v1::Task> GetTask(const lf::a2a::v1::GetTaskRequest& request,
                                               const a2a::client::CallOptions& options) override {
    (void)options;
    if (request.id() != task_.id()) {
      return a2a::core::Error::Validation("unknown task");
    }
    return task_;
  }

  a2a::core::Result<a2a::client::ListTasksResponse> ListTasks(const a2a::client::ListTasksRequest& request,
                                                              const a2a::client::CallOptions& options) override {
    (void)request;
    (void)options;
    a2a::client::ListTasksResponse response;
    if (!task_.id().empty()) {
      response.tasks.push_back(task_);
    }
    return response;
  }

  a2a::core::Result<lf::a2a::v1::Task> CancelTask(const lf::a2a::v1::CancelTaskRequest& request,
                                                  const a2a::client::CallOptions& options) override {
    (void)options;
    task_.set_id(request.id());
    task_.mutable_status()->set_state(lf::a2a::v1::TASK_STATE_CANCELED);
    return task_;
  }

  a2a::core::Result<lf::a2a::v1::TaskPushNotificationConfig> CreateTaskPushNotificationConfig(
      const lf::a2a::v1::TaskPushNotificationConfig& request, const a2a::client::CallOptions& options) override {
    (void)options;
    return request;
  }

  a2a::core::Result<lf::a2a::v1::TaskPushNotificationConfig> GetTaskPushNotificationConfig(
      const lf::a2a::v1::GetTaskPushNotificationConfigRequest& request,
      const a2a::client::CallOptions& options) override {
    (void)request;
    (void)options;
    return a2a::core::Error::Validation("no push config");
  }

  a2a::core::Result<lf::a2a::v1::ListTaskPushNotificationConfigsResponse> ListTaskPushNotificationConfigs(
      const lf::a2a::v1::ListTaskPushNotificationConfigsRequest& request,
      const a2a::client::CallOptions& options) override {
    (void)request;
    (void)options;
    return lf::a2a::v1::ListTaskPushNotificationConfigsResponse{};
  }

  a2a::core::Result<void> DeleteTaskPushNotificationConfig(
      const lf::a2a::v1::DeleteTaskPushNotificationConfigRequest& request,
      const a2a::client::CallOptions& options) override {
    (void)request;
    (void)options;
    return {};
  }

  a2a::core::Result<std::unique_ptr<a2a::client::StreamHandle>> SendStreamingMessage(
      const lf::a2a::v1::SendMessageRequest& request, a2a::client::StreamObserver& observer,
      const a2a::client::CallOptions& options) override {
    (void)request;
    (void)observer;
    (void)options;
    return a2a::core::Error::Validation("streaming not used");
  }

  a2a::core::Result<std::unique_ptr<a2a::client::StreamHandle>> SubscribeTask(
      const lf::a2a::v1::GetTaskRequest& request, a2a::client::StreamObserver& observer,
      const a2a::client::CallOptions& options) override {
    (void)request;
    (void)observer;
    (void)options;
    return a2a::core::Error::Validation("streaming not used");
  }

 private:
  lf::a2a::v1::Task task_;
};

}  // namespace

int main() {
  a2a::client::A2AClient client(std::make_unique<DeterministicTransport>());
  lf::a2a::v1::SendMessageRequest request;
  request.mutable_message()->set_role(lf::a2a::v1::ROLE_USER);
  request.mutable_message()->set_message_id(kMessageId);
  request.mutable_message()->add_parts()->set_text("hello");

  const auto sent = client.SendMessage(request);
  if (!sent.ok()) {
    std::cerr << "simple_client send failed: " << sent.error().message() << '\n';
    return 1;
  }
  lf::a2a::v1::GetTaskRequest get;
  get.set_id(sent.value().task().id());
  const auto fetched = client.GetTask(get);
  if (!fetched.ok()) {
    std::cerr << "simple_client get failed: " << fetched.error().message() << '\n';
    return 1;
  }
  std::cout << "simple_client task id: " << fetched.value().id() << '\n';
  std::cout << "simple_client state: " << fetched.value().status().state() << '\n';
  return 0;
}
