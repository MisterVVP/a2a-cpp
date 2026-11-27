// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

#include <grpcpp/grpcpp.h>

#include <chrono>
#include <memory>
#include <string>
#include <utility>

#include "a2a/core/version.h"
#include "a2a/server/async_grpc_server_transport.h"
#include "a2a/server/engine/async_runtime.h"
#include "support/async_stream_test_support.h"

namespace a2a::test {
inline constexpr auto kStreamBind = "127.0.0.1:0";
inline constexpr auto kStreamEndpoint = "127.0.0.1:";
inline constexpr auto kStreamExtensionKey = "a2a-extensions";
inline constexpr auto kStreamExtension = "urn:a2a:stream-test";
inline constexpr auto kStreamClientTimeout = std::chrono::seconds(10);

inline void PrepareStreamClient(::grpc::ClientContext& context) {
  context.AddMetadata(std::string(server::GrpcServerTransport::kVersionMetadataKey),
                      std::string(core::Version::kProtocolVersion));
  context.AddMetadata(kStreamHeader, kStreamMetadata);
  context.AddMetadata(kStreamExtensionKey, kStreamExtension);
  context.set_deadline(std::chrono::system_clock::now() + kStreamClientTimeout);
}

struct NativeStream final {
  ::grpc::ClientContext context;
  std::unique_ptr<::grpc::ClientReader<lf::a2a::v1::StreamResponse>> reader;
  lf::a2a::v1::StreamResponse event;
  bool Read() { return reader->Read(&event); }
  ::grpc::Status Finish() { return reader->Finish(); }
};

class NativeStreamServer final {
 public:
  explicit NativeStreamServer(std::vector<std::shared_ptr<server::ServerInterceptor>> interceptors = {})
      : application(std::make_shared<StreamBackend>()),
        runtime_(application, Execution(std::move(interceptors))),
        service_(runtime_.dispatcher(), {.required_extensions = {kStreamExtension}}) {
    ::grpc::ServerBuilder builder;
    builder.AddListeningPort(kStreamBind, ::grpc::InsecureServerCredentials(), &port_);
    builder.RegisterService(&service_);
    server_ = builder.BuildAndStart();
    std::string endpoint(kStreamEndpoint);
    endpoint.append(std::to_string(port_));
    channel_ = ::grpc::CreateChannel(endpoint, ::grpc::InsecureChannelCredentials());
    stub_ = lf::a2a::v1::A2AService::NewStub(channel_);
  }
  ~NativeStreamServer() {
    service_.Shutdown();
    application->publications.Shutdown();
    if (server_ != nullptr) {
      server_->Shutdown();
      server_->Wait();
    }
    runtime_.Join();
  }
  bool Ready() const { return server_ != nullptr && port_ != 0; }
  void Shutdown() { service_.Shutdown(); }
  std::shared_ptr<::grpc::Channel> Channel() const { return channel_; }
  std::string Endpoint() const {
    std::string endpoint(kStreamEndpoint);
    endpoint.append(std::to_string(port_));
    return endpoint;
  }
  std::shared_ptr<NativeStream> Start(
      bool subscribe = false, bool metadata = true,
      std::chrono::milliseconds timeout = std::chrono::duration_cast<std::chrono::milliseconds>(kStreamClientTimeout)) {
    auto call = std::make_shared<NativeStream>();
    if (metadata) {
      PrepareStreamClient(call->context);
    }
    call->context.set_deadline(std::chrono::system_clock::now() + timeout);
    if (subscribe) {
      lf::a2a::v1::SubscribeToTaskRequest request;
      request.set_id(kStreamTask);
      request.set_tenant(kStreamTenant);
      call->reader = stub_->SubscribeToTask(&call->context, request);
    } else {
      lf::a2a::v1::SendMessageRequest request;
      call->reader = stub_->SendStreamingMessage(&call->context, request);
    }
    return call;
  }
  const std::shared_ptr<StreamBackend> application;

 private:
  static server::engine::AsyncDispatcherOptions Execution(
      std::vector<std::shared_ptr<server::ServerInterceptor>> interceptors) {
    server::engine::AsyncDispatcherOptions options;
    options.interceptors = std::move(interceptors);
    return options;
  }
  server::engine::AsyncRuntime runtime_;
  server::AsyncGrpcServerTransport service_;
  std::unique_ptr<::grpc::Server> server_;
  std::shared_ptr<::grpc::Channel> channel_;
  std::unique_ptr<lf::a2a::v1::A2AService::Stub> stub_;
  int port_ = 0;
};
}  // namespace a2a::test
