// SPDX-License-Identifier: Apache-2.0

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "a2a/client/client.h"
#include "a2a/client/http_json_transport.h"
#include "a2a/client/json_rpc_transport.h"

namespace {

constexpr std::chrono::seconds kWaitTimeout{10};
constexpr std::chrono::milliseconds kCallbackStabilization{50};
constexpr std::string_view kHttpJson = "http_json";
constexpr std::string_view kJsonRpc = "jsonrpc";
constexpr std::string_view kSend = "send";
constexpr std::string_view kSubscribe = "subscribe";
constexpr std::string_view kCancel = "cancel";
constexpr std::string_view kTaskId = "fixture-task";
constexpr std::string_view kMessageId = "http-sse-interop-message";
constexpr std::string_view kSubscribeSeedMessageId = "subscribe-seed-message";
constexpr std::string_view kSubscribeTerminalMessageId = "complete-task-subscribe-terminal-message";
constexpr std::string_view kRequiredExtension = "urn:a2a:tck:required-extension";
constexpr std::string_view kRealSubscribeEnv = "A2A_INTEROP_REAL_SUBSCRIBE";
constexpr std::string_view kFixtureModeHeader = "X-Fixture-Mode";
constexpr std::string_view kFixtureCancelMode = "cancel";

class RecordingObserver final : public a2a::client::StreamObserver {
 public:
  void OnEvent(const lf::a2a::v1::StreamResponse& response) override {
    {
      std::lock_guard lock(mutex_);
      if (response.has_status_update()) {
        states_.push_back(response.status_update().status().state());
      } else if (response.has_task()) {
        states_.push_back(response.task().status().state());
      }
    }
    condition_.notify_all();
  }

  void OnError(const a2a::core::Error& error) override {
    {
      std::lock_guard lock(mutex_);
      error_ = error.message();
    }
    condition_.notify_all();
  }

  void OnCompleted() override {
    {
      std::lock_guard lock(mutex_);
      ++completion_count_;
    }
    condition_.notify_all();
  }

  [[nodiscard]] bool WaitForTerminal() {
    std::unique_lock lock(mutex_);
    return condition_.wait_for(lock, kWaitTimeout, [this] { return completion_count_ > 0 || !error_.empty(); });
  }

  [[nodiscard]] bool WaitForEventCount(std::size_t expected) {
    std::unique_lock lock(mutex_);
    return condition_.wait_for(lock, kWaitTimeout,
                               [this, expected] { return states_.size() >= expected || !error_.empty(); });
  }

  [[nodiscard]] bool IsSuccessful() const {
    std::lock_guard lock(mutex_);
    return error_.empty() && completion_count_ == 1 && states_.size() >= 2U &&
           states_.front() == lf::a2a::v1::TASK_STATE_WORKING && states_.back() == lf::a2a::v1::TASK_STATE_COMPLETED;
  }

  [[nodiscard]] bool IsCancelledCleanly() const {
    std::lock_guard lock(mutex_);
    return error_.empty() && completion_count_ == 0 && states_.size() == 1U &&
           states_.front() == lf::a2a::v1::TASK_STATE_WORKING;
  }

  [[nodiscard]] std::string error() const {
    std::lock_guard lock(mutex_);
    std::string summary = error_;
    summary.append(" states=");
    summary.append(std::to_string(states_.size()));
    summary.append(" completions=");
    summary.append(std::to_string(completion_count_));
    if (!states_.empty()) {
      summary.append(" first=");
      summary.append(std::to_string(states_.front()));
      summary.append(" last=");
      summary.append(std::to_string(states_.back()));
    }
    return summary;
  }

 private:
  mutable std::mutex mutex_;
  std::condition_variable condition_;
  std::vector<lf::a2a::v1::TaskState> states_;
  std::string error_;
  int completion_count_ = 0;
};

std::unique_ptr<a2a::client::ClientTransport> MakeTransport(std::string_view transport, std::string endpoint) {
  if (transport == kJsonRpc) {
    return a2a::client::JsonRpcTransport::CreateDefault({.transport = a2a::client::PreferredTransport::kJsonRpc,
                                                         .url = std::move(endpoint),
                                                         .security_requirements = {},
                                                         .security_schemes = {}});
  }
  return a2a::client::HttpJsonTransport::CreateDefault({.transport = a2a::client::PreferredTransport::kRest,
                                                        .url = std::move(endpoint),
                                                        .security_requirements = {},
                                                        .security_schemes = {}});
}

lf::a2a::v1::SendMessageRequest MakeSendRequest() {
  lf::a2a::v1::SendMessageRequest request;
  request.mutable_message()->set_role(lf::a2a::v1::ROLE_USER);
  request.mutable_message()->set_message_id(std::string(kMessageId));
  request.mutable_message()->add_parts()->set_text("HTTP SSE interoperability fixture");
  return request;
}

lf::a2a::v1::SendMessageRequest MakeTaskUpdateRequest(std::string_view task_id, std::string_view message_id) {
  lf::a2a::v1::SendMessageRequest request;
  request.mutable_message()->set_role(lf::a2a::v1::ROLE_USER);
  request.mutable_message()->set_task_id(std::string(task_id));
  request.mutable_message()->set_message_id(std::string(message_id));
  request.mutable_message()->add_parts()->set_text("HTTP SSE subscribe fixture");
  return request;
}

lf::a2a::v1::GetTaskRequest MakeSubscribeRequest(std::string_view task_id) {
  lf::a2a::v1::GetTaskRequest request;
  request.set_id(std::string(task_id));
  return request;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 4) {
    std::cerr << "usage: http_sse_interop_client http_json|jsonrpc send|subscribe|cancel <endpoint>\n";
    return EXIT_FAILURE;
  }
  const std::string_view transport(argv[1]);
  const std::string_view operation(argv[2]);
  if ((transport != kHttpJson && transport != kJsonRpc) ||
      (operation != kSend && operation != kSubscribe && operation != kCancel)) {
    std::cerr << "unsupported transport or operation\n";
    return EXIT_FAILURE;
  }

  a2a::client::A2AClient client(MakeTransport(transport, argv[3]));
  const bool use_real_subscribe_flow = std::getenv(std::string(kRealSubscribeEnv).c_str()) != nullptr;
  std::string task_id(kTaskId);
  if (use_real_subscribe_flow) {
    task_id.append("-");
    task_id.append(transport);
    task_id.append("-");
    task_id.append(operation);
  }
  RecordingObserver observer;
  a2a::client::CallOptions options;
  options.extensions = {std::string(kRequiredExtension)};
  if (!use_real_subscribe_flow && operation == kCancel) {
    options.headers[std::string(kFixtureModeHeader)] = std::string(kFixtureCancelMode);
  }
  if (use_real_subscribe_flow && (operation == kSubscribe || operation == kCancel)) {
    RecordingObserver seed_observer;
    auto seed =
        client.SendStreamingMessage(MakeTaskUpdateRequest(task_id, kSubscribeSeedMessageId), seed_observer, options);
    if (!seed.ok()) {
      std::cerr << seed.error().message() << '\n';
      return EXIT_FAILURE;
    }
    if (!seed_observer.WaitForTerminal()) {
      seed.value()->Cancel();
      std::cerr << "seed stream timed out\n";
      return EXIT_FAILURE;
    }
  }
  a2a::core::Result<std::unique_ptr<a2a::client::StreamHandle>> handle =
      a2a::core::Error::Internal("stream not started");
  if (operation == kSubscribe || operation == kCancel) {
    handle = client.SubscribeTask(MakeSubscribeRequest(task_id), observer, options);
  } else {
    handle = client.SendStreamingMessage(MakeSendRequest(), observer, options);
  }
  if (!handle.ok()) {
    std::cerr << handle.error().message() << '\n';
    return EXIT_FAILURE;
  }

  if (operation == kCancel) {
    if (!observer.WaitForEventCount(1U)) {
      handle.value()->Cancel();
      std::cerr << "stream did not produce the first event\n";
      return EXIT_FAILURE;
    }
    handle.value()->Cancel();
    std::this_thread::sleep_for(kCallbackStabilization);
    if (!observer.IsCancelledCleanly()) {
      std::cerr << "unexpected cancellation result: " << observer.error() << '\n';
      return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
  }

  if (use_real_subscribe_flow && operation == kSubscribe) {
    if (!observer.WaitForEventCount(1U)) {
      handle.value()->Cancel();
      std::cerr << "subscribe stream did not start\n";
      return EXIT_FAILURE;
    }
    const auto terminal = client.SendMessage(MakeTaskUpdateRequest(task_id, kSubscribeTerminalMessageId), options);
    if (!terminal.ok()) {
      handle.value()->Cancel();
      std::cerr << terminal.error().message() << '\n';
      return EXIT_FAILURE;
    }
  }

  if (!observer.WaitForTerminal()) {
    handle.value()->Cancel();
    std::cerr << "stream timed out\n";
    return EXIT_FAILURE;
  }
  if (!observer.IsSuccessful()) {
    handle.value()->Cancel();
    std::cerr << "unexpected stream result: " << observer.error() << '\n';
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
