// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include <cstdlib>
#include <iostream>
#include <string_view>
#if A2A_HAS_ASYNC_SERVER
#include <grpcpp/create_channel.h>
#include <grpcpp/security/server_credentials.h>
#include <grpcpp/server_builder.h>

#include <chrono>
#include <memory>
#include <string>
#include <utility>

#include "a2a/core/protocol_errors.h"
#include "a2a/core/version.h"
#include "a2a/server/engine/async_runtime.h"
#include "a2a/server/server_bindings.h"

namespace {
using namespace a2a::server;
using namespace a2a::server::engine;
constexpr auto kTaskId = "example-task";
constexpr auto kBind = "127.0.0.1:50051";
constexpr auto kSmokeBind = "127.0.0.1:0";
constexpr auto kEndpointPrefix = "127.0.0.1:";
constexpr auto kSmokeArgument = "--smoke";
constexpr auto kStartFailed = "Could not start async gRPC server\n";
constexpr auto kListening = "Async unary gRPC service on 127.0.0.1:50051; press Enter to stop\n";
constexpr auto kCancelled = "Application observed cancellation";
constexpr auto kTimeout = std::chrono::seconds(5);

// A read-only coroutine application. Real backends co_await their I/O here.
class ReadOnlyExecutor final : public CoroutineAgentExecutor {
 public:
  boost::asio::awaitable<a2a::core::Result<lf::a2a::v1::Task>> GetTask(lf::a2a::v1::GetTaskRequest request,
                                                                       RequestExecutionContext context) override {
    if (context.StopToken().stop_requested()) {
      co_return a2a::core::Error::Network(kCancelled);
    }
    if (request.id() != kTaskId) {
      co_return a2a::core::protocol_errors::TaskNotFound();
    }
    lf::a2a::v1::Task task;
    task.set_id(kTaskId);
    task.mutable_status()->set_state(lf::a2a::v1::TASK_STATE_WORKING);
    co_return std::move(task);
  }
};

bool Smoke(int port) {
  std::string endpoint(kEndpointPrefix);
  endpoint.append(std::to_string(port));
  auto stub = lf::a2a::v1::A2AService::NewStub(::grpc::CreateChannel(endpoint, ::grpc::InsecureChannelCredentials()));
  ::grpc::ClientContext context;
  context.AddMetadata(std::string(GrpcServerTransport::kVersionMetadataKey),
                      std::string(a2a::core::Version::kProtocolVersion));
  context.set_deadline(std::chrono::system_clock::now() + kTimeout);
  lf::a2a::v1::GetTaskRequest request;
  request.set_id(kTaskId);
  lf::a2a::v1::Task response;
  return stub->GetTask(&context, request, &response).ok() && response.id() == kTaskId;
}
}  // namespace

int main(int argc, char** argv) try {
  const bool smoke = argc > 1 && std::string_view(argv[1]) == kSmokeArgument;
  AsyncRuntime runtime(std::make_shared<ReadOnlyExecutor>());
  ServerBindings bindings(runtime.dispatcher(), {});
  ::grpc::ServerBuilder builder;
  int port = 0;
  builder.AddListeningPort(smoke ? kSmokeBind : kBind, ::grpc::InsecureServerCredentials(), &port);
  builder.RegisterService(&bindings.grpc_service());
  auto server = builder.BuildAndStart();
  if (server == nullptr) {
    std::cerr << kStartFailed;
    return EXIT_FAILURE;
  }
  bool success = true;
  if (smoke) {
    success = Smoke(port);
  } else {
    std::cout << kListening;
    std::string line;
    std::getline(std::cin, line);
  }
  // Close service admission first so applications observe kServerShutdown.
  bindings.Shutdown();
  server->Shutdown();
  server->Wait();
  runtime.Join();
  return success ? EXIT_SUCCESS : EXIT_FAILURE;
} catch (...) {
  std::cerr << kStartFailed;
  return EXIT_FAILURE;
}
#else
namespace {
constexpr auto kUnavailable = "Async unary execution is unavailable in this SDK/standard-library configuration\n";
constexpr int kUnavailableExit = 77;
}  // namespace
int main() {
  std::cout << kUnavailable;
  return kUnavailableExit;
}
#endif
