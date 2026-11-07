// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <set>
#include <string_view>
#include <vector>

#include "a2a/core/protojson.h"
#include "a2a/server/dispatcher.h"
#include "google/protobuf/struct.pb.h"
#include "support/async_stream_grpc_support.h"

namespace {
using namespace a2a;
using namespace a2a::test;
using Clock = std::chrono::steady_clock;
constexpr std::size_t kMaximumStreams = 128;
#if defined(__linux__)
constexpr auto kThreadStatus = "/proc/self/status";
constexpr auto kThreadField = "Threads:";
#endif
constexpr auto kMode = "mode";
constexpr auto kSync = "sync";
constexpr auto kAsync = "async";
constexpr auto kCount = "streams";
constexpr auto kEvents = "events_per_stream";
constexpr auto kAppThreads = "application_threads";
constexpr auto kThreadsBefore = "process_threads_before";
constexpr auto kThreadsPending = "process_threads_pending";
constexpr auto kFirstP95 = "first_event_p95_us";
constexpr auto kCompletionP95 = "completion_p95_us";
constexpr auto kErrors = "errors";
constexpr auto kEvidenceFailed = "Stream evidence failed\n";
constexpr double kTail = 0.95;
constexpr std::size_t kEventsPerStream = 2;

std::optional<std::size_t> ProcessThreads() {
#if defined(__linux__)
  std::ifstream status(kThreadStatus);
  std::string field;
  while (status >> field) {
    if (field == kThreadField) {
      std::size_t count = 0;
      status >> count;
      return count;
    }
    std::getline(status, field);
  }
#endif
  return std::nullopt;
}
class SyncBackend final : public server::AgentExecutor {
 public:
  core::Result<lf::a2a::v1::SendMessageResponse> SendMessage(const lf::a2a::v1::SendMessageRequest& /*request*/,
                                                             server::RequestContext& /*context*/) override {
    return core::protocol_errors::UnsupportedOperation();
  }
  core::Result<std::unique_ptr<server::ServerStreamSession>> SendStreamingMessage(
      const lf::a2a::v1::SendMessageRequest& /*request*/, server::RequestContext& /*context*/) override {
    return core::protocol_errors::UnsupportedOperation();
  }
  core::Result<lf::a2a::v1::Task> GetTask(const lf::a2a::v1::GetTaskRequest& /*request*/,
                                          server::RequestContext& /*context*/) override {
    return core::protocol_errors::UnsupportedOperation();
  }
  core::Result<server::ListTasksResponse> ListTasks(const server::ListTasksRequest& /*request*/,
                                                    server::RequestContext& /*context*/) override {
    return core::protocol_errors::UnsupportedOperation();
  }
  core::Result<lf::a2a::v1::Task> CancelTask(const lf::a2a::v1::CancelTaskRequest& /*request*/,
                                             server::RequestContext& /*context*/) override {
    return core::protocol_errors::UnsupportedOperation();
  }
  core::Result<std::unique_ptr<server::ServerStreamSession>> SubscribeTask(
      const lf::a2a::v1::GetTaskRequest& /*request*/, server::RequestContext& /*context*/) override {
    {
      std::lock_guard lock(mutex);
      threads.insert(std::this_thread::get_id());
    }
    return publications.Subscribe(StreamTask());
  }
  server::TaskSubscriptionService publications;
  std::mutex mutex;
  std::set<std::thread::id> threads;
};
class SyncServer final {
 public:
  SyncServer() : dispatcher_(&application), service_(&dispatcher_, {.required_extensions = {kStreamExtension}}) {
    ::grpc::ServerBuilder builder;
    builder.AddListeningPort(kStreamBind, ::grpc::InsecureServerCredentials(), &port_);
    builder.RegisterService(&service_);
    server_ = builder.BuildAndStart();
    std::string endpoint(kStreamEndpoint);
    endpoint.append(std::to_string(port_));
    stub_ = lf::a2a::v1::A2AService::NewStub(::grpc::CreateChannel(endpoint, ::grpc::InsecureChannelCredentials()));
  }
  ~SyncServer() {
    application.publications.Shutdown();
    if (server_) {
      server_->Shutdown();
      server_->Wait();
    }
  }
  std::shared_ptr<NativeStream> Start() {
    auto call = std::make_shared<NativeStream>();
    PrepareStreamClient(call->context);
    lf::a2a::v1::SubscribeToTaskRequest request;
    request.set_id(kStreamTask);
    call->reader = stub_->SubscribeToTask(&call->context, request);
    return call;
  }
  SyncBackend application;

 private:
  server::Dispatcher dispatcher_;
  server::GrpcServerTransport service_;
  std::unique_ptr<::grpc::Server> server_;
  std::unique_ptr<lf::a2a::v1::A2AService::Stub> stub_;
  int port_ = 0;
};

void Number(google::protobuf::Struct& row, const char* key, double value) {
  (*row.mutable_fields())[key].set_number_value(value);
}
void Threads(google::protobuf::Struct& row, const char* key, std::optional<std::size_t> count) {
  if (count.has_value()) {
    Number(row, key, static_cast<double>(*count));
  } else {
    (*row.mutable_fields())[key].set_null_value(google::protobuf::NULL_VALUE);
  }
}
double P95(std::vector<double> values) {
  std::ranges::sort(values);
  return values.at(static_cast<std::size_t>(static_cast<double>(values.size() - 1) * kTail));
}
double Elapsed(Clock::time_point start) {
  return std::chrono::duration<double, std::micro>(Clock::now() - start).count();
}
template <typename Start>
std::optional<google::protobuf::Struct> Measure(const char* mode, std::size_t count, Start start,
                                                server::TaskSubscriptionService& publications) {
  const auto before = ProcessThreads();
  std::vector<std::shared_ptr<NativeStream>> calls;
  std::vector<double> first;
  calls.reserve(count);
  first.reserve(count);
  for (std::size_t index = 0; index < count; ++index) {
    const auto began = Clock::now();
    auto call = start();
    if (!call->Read() || !call->event.has_task()) {
      return std::nullopt;
    }
    first.push_back(Elapsed(began));
    calls.push_back(std::move(call));
  }
  const auto pending = ProcessThreads();
  const auto released = Clock::now();
  publications.PublishTaskUpdated(StreamTask(lf::a2a::v1::TASK_STATE_COMPLETED));
  std::vector<double> completion;
  completion.reserve(count);
  for (const auto& call : calls) {
    if (!call->Read() || !call->event.has_status_update() || call->Read() || !call->Finish().ok()) {
      return std::nullopt;
    }
    completion.push_back(Elapsed(released));
  }
  google::protobuf::Struct row;
  (*row.mutable_fields())[kMode].set_string_value(mode);
  Number(row, kCount, static_cast<double>(count));
  Number(row, kEvents, static_cast<double>(kEventsPerStream));
  Number(row, kErrors, 0);
  Number(row, kFirstP95, P95(std::move(first)));
  Number(row, kCompletionP95, P95(std::move(completion)));
  Threads(row, kThreadsBefore, before);
  Threads(row, kThreadsPending, pending);
  return row;
}
}  // namespace
int main(int argc, char** argv) {
  std::size_t count = kStreamConcurrency;
  if (argc > 1) {
    const std::string_view argument(argv[1]);
    const auto parsed = std::from_chars(argument.data(), argument.data() + argument.size(), count);
    if (parsed.ec != std::errc{} || parsed.ptr != argument.data() + argument.size() || count == 0 ||
        count > kMaximumStreams) {
      return EXIT_FAILURE;
    }
  }
  google::protobuf::ListValue rows;
  {
    SyncServer server;
    auto row = Measure(kSync, count, [&server] { return server.Start(); }, server.application.publications);
    if (!row.has_value()) {
      std::cerr << kEvidenceFailed;
      return EXIT_FAILURE;
    }
    {
      std::lock_guard lock(server.application.mutex);
      Number(*row, kAppThreads, static_cast<double>(server.application.threads.size()));
    }
    *rows.add_values()->mutable_struct_value() = std::move(*row);
  }
  {
    NativeStreamServer server;
    auto row = Measure(kAsync, count, [&server] { return server.Start(true); }, server.application->publications);
    if (!row.has_value() || server.application->ThreadCount() > kCoroutineWorkers) {
      std::cerr << kEvidenceFailed;
      return EXIT_FAILURE;
    }
    Number(*row, kAppThreads, static_cast<double>(server.application->ThreadCount()));
    *rows.add_values()->mutable_struct_value() = std::move(*row);
  }
  auto json = core::MessageToJson(rows);
  if (!json.ok()) {
    return EXIT_FAILURE;
  }
  std::cout << json.value() << '\n';
  return EXIT_SUCCESS;
}
