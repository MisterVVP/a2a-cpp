// SPDX-License-Identifier: Apache-2.0

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "a2a/client/client.h"
#include "a2a/client/http_json_transport.h"
#include "a2a/client/json_rpc_transport.h"

namespace {

constexpr std::chrono::milliseconds kDefaultTimeout{30000};
constexpr char kHttpJsonTransport[] = "http_json";
constexpr char kJsonRpcTransport[] = "jsonrpc";
constexpr char kSendOperation[] = "send";
constexpr char kSubscribeOperation[] = "subscribe";
constexpr char kHelpFlag[] = "--help";
constexpr char kTransportFlag[] = "--transport";
constexpr char kEndpointFlag[] = "--endpoint";
constexpr char kOperationFlag[] = "--operation";
constexpr char kTaskIdFlag[] = "--task-id";
constexpr char kTimeoutMsFlag[] = "--timeout-ms";
constexpr char kCancelAfterFirstEventFlag[] = "--cancel-after-first-event";
constexpr char kMessageIdPrefix[] = "streaming-client-message-";
constexpr char kMessageText[] = "Hello from the production streaming_client example";
constexpr int kSuccess = 0;
constexpr int kUsageError = 2;
constexpr int kRuntimeError = 1;

std::atomic<std::uint64_t> g_message_sequence{0};

struct Options final {
  std::string transport = kHttpJsonTransport;
  std::string endpoint;
  std::string operation = kSendOperation;
  std::string task_id;
  std::chrono::milliseconds timeout = kDefaultTimeout;
  bool cancel_after_first_event = false;
};

void PrintUsage(std::ostream& output) {
  output << "Usage: streaming_client " << kTransportFlag << " http_json|jsonrpc " << kEndpointFlag << " <url> "
         << kOperationFlag << " send|subscribe [" << kTaskIdFlag << " <id>] [" << kTimeoutMsFlag << " <milliseconds>] ["
         << kCancelAfterFirstEventFlag << "]\n";
}

std::optional<std::chrono::milliseconds> ParseTimeout(std::string_view value) {
  try {
    std::size_t parsed = 0;
    const auto timeout = std::stoll(std::string(value), &parsed);
    if (parsed != value.size() || timeout <= 0) {
      return std::nullopt;
    }
    return std::chrono::milliseconds(timeout);
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

std::optional<Options> ParseOptions(int argc, char** argv) {
  Options options;
  for (int index = 1; index < argc; ++index) {
    const std::string_view argument(argv[index]);
    const auto require_value = [&](std::string* destination) -> bool {
      if (index + 1 >= argc) {
        return false;
      }
      *destination = argv[++index];
      return true;
    };
    if (argument == kHelpFlag) {
      PrintUsage(std::cout);
      std::exit(kSuccess);
    } else if (argument == kTransportFlag) {
      if (!require_value(&options.transport)) {
        return std::nullopt;
      }
    } else if (argument == kEndpointFlag) {
      if (!require_value(&options.endpoint)) {
        return std::nullopt;
      }
    } else if (argument == kOperationFlag) {
      if (!require_value(&options.operation)) {
        return std::nullopt;
      }
    } else if (argument == kTaskIdFlag) {
      if (!require_value(&options.task_id)) {
        return std::nullopt;
      }
    } else if (argument == kTimeoutMsFlag) {
      std::string raw_timeout;
      if (!require_value(&raw_timeout)) {
        return std::nullopt;
      }
      auto timeout = ParseTimeout(raw_timeout);
      if (!timeout.has_value()) {
        return std::nullopt;
      }
      options.timeout = *timeout;
    } else if (argument == kCancelAfterFirstEventFlag) {
      options.cancel_after_first_event = true;
    } else {
      return std::nullopt;
    }
  }

  if (options.endpoint.empty() || (options.transport != kHttpJsonTransport && options.transport != kJsonRpcTransport) ||
      (options.operation != kSendOperation && options.operation != kSubscribeOperation)) {
    return std::nullopt;
  }
  if (options.operation == kSubscribeOperation && options.task_id.empty()) {
    return std::nullopt;
  }
  return options;
}

class PrintingObserver final : public a2a::client::StreamObserver {
 public:
  explicit PrintingObserver(bool cancel_after_first_event) : cancel_after_first_event_(cancel_after_first_event) {}

  void SetHandle(a2a::client::StreamHandle* handle) {
    {
      std::lock_guard lock(mutex_);
      handle_ = handle;
    }
    completion_cv_.notify_all();
  }

  [[nodiscard]] bool WaitForTerminal(std::chrono::milliseconds timeout) {
    std::unique_lock lock(mutex_);
    return completion_cv_.wait_for(lock, timeout, [this] { return completed_ || failed_ || cancelled_; });
  }

  [[nodiscard]] bool failed() const {
    std::lock_guard lock(mutex_);
    return failed_;
  }

  void OnEvent(const lf::a2a::v1::StreamResponse& response) override {
    PrintEvent(response);
    a2a::client::StreamHandle* handle = nullptr;
    {
      std::unique_lock lock(mutex_);
      ++event_count_;
      if (cancel_after_first_event_ && event_count_ == 1U) {
        completion_cv_.wait(lock, [this] { return handle_ != nullptr; });
        handle = handle_;
      }
    }
    if (handle == nullptr) {
      return;
    }

    std::cout << "streaming_client cancelling after first event\n";
    handle->Cancel();
    {
      std::lock_guard lock(mutex_);
      cancelled_ = true;
    }
    completion_cv_.notify_all();
  }

  void OnError(const a2a::core::Error& error) override {
    std::cerr << "streaming_client error: " << error.message() << '\n';
    std::lock_guard lock(mutex_);
    failed_ = true;
    completion_cv_.notify_all();
  }

  void OnCompleted() override {
    std::cout << "streaming_client completed\n";
    std::lock_guard lock(mutex_);
    completed_ = true;
    completion_cv_.notify_all();
  }

 private:
  static void PrintEvent(const lf::a2a::v1::StreamResponse& response) {
    if (response.has_task()) {
      std::cout << "event task id=" << response.task().id() << " state=" << response.task().status().state() << '\n';
    } else if (response.has_status_update()) {
      std::cout << "event status task_id=" << response.status_update().task_id()
                << " state=" << response.status_update().status().state() << '\n';
    } else if (response.has_artifact_update()) {
      std::cout << "event artifact task_id=" << response.artifact_update().task_id()
                << " artifact_id=" << response.artifact_update().artifact().artifact_id() << '\n';
    } else {
      std::cout << "event unknown variant\n";
    }
  }

  mutable std::mutex mutex_;
  std::condition_variable completion_cv_;
  a2a::client::StreamHandle* handle_ = nullptr;
  bool cancel_after_first_event_ = false;
  bool completed_ = false;
  bool failed_ = false;
  bool cancelled_ = false;
  std::uint32_t event_count_ = 0;
};

std::unique_ptr<a2a::client::ClientTransport> CreateTransport(const Options& options) {
  if (options.transport == kJsonRpcTransport) {
    return a2a::client::JsonRpcTransport::CreateDefault({.transport = a2a::client::PreferredTransport::kJsonRpc,
                                                         .url = options.endpoint,
                                                         .security_requirements = {},
                                                         .security_schemes = {}},
                                                        options.timeout);
  }
  return a2a::client::HttpJsonTransport::CreateDefault({.transport = a2a::client::PreferredTransport::kRest,
                                                        .url = options.endpoint,
                                                        .security_requirements = {},
                                                        .security_schemes = {}},
                                                       options.timeout);
}

std::string MakeMessageId() {
  const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
  const auto sequence = g_message_sequence.fetch_add(1, std::memory_order_relaxed);
  std::string message_id;
  message_id.reserve(std::string_view(kMessageIdPrefix).size() + 32U);
  message_id.append(kMessageIdPrefix);
  message_id.append(std::to_string(now));
  message_id.push_back('-');
  message_id.append(std::to_string(sequence));
  return message_id;
}

lf::a2a::v1::SendMessageRequest MakeSendRequest(std::string_view task_id) {
  lf::a2a::v1::SendMessageRequest request;
  auto* message = request.mutable_message();
  message->set_role(lf::a2a::v1::ROLE_USER);
  message->set_message_id(MakeMessageId());
  if (!task_id.empty()) {
    message->set_task_id(std::string(task_id));
  }
  message->add_parts()->set_text(kMessageText);
  return request;
}

lf::a2a::v1::GetTaskRequest MakeGetTaskRequest(std::string_view task_id) {
  lf::a2a::v1::GetTaskRequest request;
  request.set_id(std::string(task_id));
  return request;
}

}  // namespace

int main(int argc, char** argv) {
  const auto options = ParseOptions(argc, argv);
  if (!options.has_value()) {
    PrintUsage(std::cerr);
    return kUsageError;
  }

  a2a::client::A2AClient client(CreateTransport(*options));
  PrintingObserver observer(options->cancel_after_first_event);
  a2a::core::Result<std::unique_ptr<a2a::client::StreamHandle>> handle =
      a2a::core::Error::Internal("stream was not started");
  if (options->operation == kSubscribeOperation) {
    handle = client.SubscribeTask(MakeGetTaskRequest(options->task_id), observer);
  } else {
    handle = client.SendStreamingMessage(MakeSendRequest(options->task_id), observer);
  }
  if (!handle.ok()) {
    std::cerr << "streaming_client failed to start: " << handle.error().message() << '\n';
    return kRuntimeError;
  }

  observer.SetHandle(handle.value().get());
  const bool finished = observer.WaitForTerminal(options->timeout);
  if (!finished) {
    std::cerr << "streaming_client timed out after " << options->timeout.count() << " ms\n";
    handle.value()->Cancel();
    return kRuntimeError;
  }
  return observer.failed() ? kRuntimeError : kSuccess;
}
