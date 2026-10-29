// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include <gtest/gtest.h>

#include <barrier>
#include <boost/asio/executor_work_guard.hpp>
#include <chrono>
#include <exception>
#include <future>
#include <memory>
#include <thread>
#include <vector>

#include "support/asio_test_support.h"
#include "support/async_unary_test_support.h"

namespace {
using namespace a2a;
using namespace a2a::server;
using namespace a2a::server::engine;
using namespace a2a::test;
constexpr auto kCoroutineDeadline = std::chrono::seconds(1);
constexpr auto kRaceParticipants = 2;
constexpr auto kRequestIdHeader = "x-request-id";

struct GrpcReply final {
  ::grpc::ClientContext context;
  lf::a2a::v1::GetTaskRequest request;
  lf::a2a::v1::Task response;
  std::promise<::grpc::Status> completion;
  std::future<::grpc::Status> result = completion.get_future();
};

class AsioGrpcServer final {
  asio::io_context loop_;

 public:
  explicit AsioGrpcServer(CoroutineBehavior behavior = CoroutineBehavior::kImmediate)
      : application(std::make_shared<CoroutineBackend>(behavior)),
        guard_(asio::make_work_guard(loop_)),
        dispatcher_(std::make_shared<AsyncDispatcher>(application, loop_.get_executor())),
        service_(dispatcher_) {
    ::grpc::ServerBuilder builder;
    builder.AddListeningPort(kLoopbackBind, ::grpc::InsecureServerCredentials(), &port_);
    builder.RegisterService(&service_);
    server_ = builder.BuildAndStart();
    std::string endpoint(kLoopbackPrefix);
    endpoint.append(std::to_string(port_));
    stub_ = lf::a2a::v1::A2AService::NewStub(::grpc::CreateChannel(endpoint, ::grpc::InsecureChannelCredentials()));
    workers_.reserve(kCoroutineWorkers);
    for (std::size_t index = 0; index < kCoroutineWorkers; ++index) {
      workers_.emplace_back([this] { loop_.run(); });
    }
  }
  ~AsioGrpcServer() {
    try {
      service_.Shutdown();
      application->Release();
      if (server_ != nullptr) {
        server_->Shutdown();
        server_->Wait();
      }
      guard_.reset();
      for (auto& worker : workers_) {
        worker.join();
      }
    } catch (...) {
      std::terminate();
    }
  }
  [[nodiscard]] bool Ready() const { return server_ != nullptr && port_ != 0; }
  void Shutdown() { service_.Shutdown(); }
  std::shared_ptr<GrpcReply> GetTask(bool deadline = false) {
    const auto reply = std::make_shared<GrpcReply>();
    reply->request.set_id(kCoroutineTaskId);
    reply->context.AddMetadata(kRequestIdHeader, kCoroutineRequestId);
    reply->context.AddMetadata(std::string(GrpcServerTransport::kVersionMetadataKey),
                               std::string(core::Version::kProtocolVersion));
    if (deadline) {
      reply->context.set_deadline(std::chrono::system_clock::now() + kCoroutineDeadline);
    }
    client_.GetTask(*stub_, reply->context, reply->request, reply->response,
                    [reply](auto status) { reply->completion.set_value(std::move(status)); });
    return reply;
  }
  static ::grpc::Status Receive(const std::shared_ptr<GrpcReply>& reply) {
    if (reply->result.wait_for(kCoroutineWait) != std::future_status::ready) {
      ADD_FAILURE() << kCoroutineFailure;
      reply->context.TryCancel();
      return {::grpc::StatusCode::DEADLINE_EXCEEDED, kCoroutineFailure};
    }
    return reply->result.get();
  }
  const std::shared_ptr<CoroutineBackend> application;

 private:
  asio::executor_work_guard<asio::io_context::executor_type> guard_;
  std::shared_ptr<AsyncDispatcher> dispatcher_;
  AsyncGrpcServerTransport service_;
  std::unique_ptr<::grpc::Server> server_;
  int port_ = 0;
  std::unique_ptr<lf::a2a::v1::A2AService::Stub> stub_;
  UnaryGrpcClient client_;
  std::vector<std::thread> workers_;
};

void ExpectSuccessfulReply(const std::shared_ptr<GrpcReply>& reply) {
  const auto status = AsioGrpcServer::Receive(reply);
  EXPECT_TRUE(status.ok()) << status.error_message();
  EXPECT_EQ(reply->response.id(), kCoroutineTaskId);
}

TEST(AsioGrpcServer, ReturnsCoroutineResultAndIngressMetadata) {
  AsioGrpcServer server;
  ASSERT_TRUE(server.Ready());
  const auto reply = server.GetTask();
  ExpectSuccessfulReply(reply);
  EXPECT_EQ(server.application->Metadata().client_headers.at(kRequestIdHeader), kCoroutineRequestId);
}

TEST(AsioGrpcServer, ClientCancellationAbortsAsioWait) {
  AsioGrpcServer server(CoroutineBehavior::kWait);
  ASSERT_TRUE(server.Ready());
  const auto reply = server.GetTask();
  ASSERT_TRUE(server.application->WaitStarted());
  reply->context.TryCancel();
  EXPECT_EQ(AsioGrpcServer::Receive(reply).error_code(), ::grpc::StatusCode::CANCELLED);
  EXPECT_TRUE(server.application->WaitFinished());
}

TEST(AsioGrpcServer, NativeDeadlineCancelsAsioWait) {
  AsioGrpcServer server(CoroutineBehavior::kWait);
  ASSERT_TRUE(server.Ready());
  const auto reply = server.GetTask(/*deadline=*/true);
  ASSERT_TRUE(server.application->WaitStarted());
  EXPECT_EQ(AsioGrpcServer::Receive(reply).error_code(), ::grpc::StatusCode::DEADLINE_EXCEEDED);
  EXPECT_TRUE(server.application->WaitFinished());
}

TEST(AsioGrpcServer, ShutdownCancelsAsioWait) {
  AsioGrpcServer server(CoroutineBehavior::kWait);
  ASSERT_TRUE(server.Ready());
  const auto reply = server.GetTask();
  ASSERT_TRUE(server.application->WaitStarted());
  server.Shutdown();
  EXPECT_EQ(AsioGrpcServer::Receive(reply).error_code(), ::grpc::StatusCode::UNAVAILABLE);
  EXPECT_TRUE(server.application->WaitFinished());
}

TEST(AsioGrpcServer, ConcurrentSuspendedRequestsShareFixedAsioWorkers) {
  AsioGrpcServer server(CoroutineBehavior::kWait);
  ASSERT_TRUE(server.Ready());
  std::vector<std::shared_ptr<GrpcReply>> replies;
  replies.reserve(kCoroutineRequests);
  for (std::size_t index = 0; index < kCoroutineRequests; ++index) {
    replies.push_back(server.GetTask());
  }
  ASSERT_TRUE(server.application->WaitStarted(kCoroutineRequests));
  EXPECT_EQ(server.application->completed, 0);
  server.application->Release();
  for (const auto& reply : replies) {
    ExpectSuccessfulReply(reply);
  }
}
TEST(AsioGrpcServer, ConcurrentCompletionAndCancellationKeepFramesAlive) {
  AsioGrpcServer server(CoroutineBehavior::kWait);
  ASSERT_TRUE(server.Ready());
  std::vector<std::shared_ptr<GrpcReply>> replies;
  replies.reserve(kCoroutineRequests);
  for (std::size_t index = 0; index < kCoroutineRequests; ++index) {
    replies.push_back(server.GetTask());
  }
  ASSERT_TRUE(server.application->WaitStarted(kCoroutineRequests));
  std::barrier start(kRaceParticipants);
  std::jthread cancel([&] {
    start.arrive_and_wait();
    for (const auto& reply : replies) {
      reply->context.TryCancel();
    }
  });
  start.arrive_and_wait();
  server.application->Release();
  cancel.join();
  for (const auto& reply : replies) {
    const auto status = AsioGrpcServer::Receive(reply);
    EXPECT_TRUE(status.ok() || status.error_code() == ::grpc::StatusCode::CANCELLED);
  }
  EXPECT_TRUE(server.application->WaitFinished(kCoroutineRequests));
}
}  // namespace
