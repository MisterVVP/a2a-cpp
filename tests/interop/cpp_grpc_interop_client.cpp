#include <google/api/annotations.pb.h>
#include <google/api/client.pb.h>
#include <google/api/field_behavior.pb.h>
#include <google/api/http.pb.h>
#include <grpcpp/create_channel.h>
#include <grpcpp/security/credentials.h>

#include <chrono>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "a2a/client/client.h"
#include "a2a/client/discovery.h"
#include "a2a/client/grpc_transport.h"

namespace {
constexpr int kPollIntervalMs = 5;
class RecordingObserver final : public a2a::client::StreamObserver {
 public:
  void OnEvent(const lf::a2a::v1::StreamResponse& response) override { events.push_back(response); }
  void OnError(const a2a::core::Error& error) override { errors.emplace_back(error.message()); }
  void OnCompleted() override { completed = true; }
  std::vector<lf::a2a::v1::StreamResponse> events;
  std::vector<std::string> errors;
  bool completed = false;
};
}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    return 2;
  }
  const std::string endpoint = argv[1];
  a2a::client::ResolvedInterface iface{.transport = a2a::client::PreferredTransport::kGrpc,
                                       .url = endpoint,
                                       .security_requirements = {},
                                       .security_schemes = {}};
  auto channel = grpc::CreateChannel(endpoint, grpc::InsecureChannelCredentials());
  a2a::client::A2AClient client(std::make_unique<a2a::client::GrpcTransport>(iface, channel));

  lf::a2a::v1::SendMessageRequest send;
  send.mutable_message()->set_role(lf::a2a::v1::ROLE_USER);
  send.mutable_message()->set_task_id("interop-task-1");
  if (auto r = client.SendMessage(send); !r.ok()) {
    return 1;
  }

  lf::a2a::v1::GetTaskRequest get;
  get.set_id("interop-task-1");
  if (auto r = client.GetTask(get); !r.ok() || r.value().id() != "interop-task-1") {
    return 1;
  }

  RecordingObserver observer;
  auto stream_result = client.SubscribeTask(get, observer);
  if (!stream_result.ok()) {
    return 1;
  }
  while (stream_result.value()->IsActive()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(kPollIntervalMs));
  }
  if (observer.events.empty() || !observer.completed || !observer.errors.empty()) {
    return 1;
  }

  lf::a2a::v1::CancelTaskRequest cancel;
  cancel.set_id("interop-task-1");
  if (auto r = client.CancelTask(cancel);
      !r.ok() || r.value().status().state() != lf::a2a::v1::TASK_STATE_CANCELED) {
    return 1;
  }

  lf::a2a::v1::GetTaskRequest missing;
  missing.set_id("missing");
  if (client.GetTask(missing).ok()) {
    return 1;
  }
  return 0;
}
