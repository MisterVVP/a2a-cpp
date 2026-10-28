// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once
#include <grpcpp/completion_queue.h>
#include <grpcpp/create_channel.h>
#include <grpcpp/security/server_credentials.h>
#include <grpcpp/server.h>
#include <grpcpp/server_builder.h>
#include <grpcpp/support/channel_arguments.h>

#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "a2a/core/protocol_errors.h"
#include "a2a/core/version.h"
#include "a2a/server/async_grpc_server_transport.h"

namespace a2a::test {
inline constexpr auto kAsyncTaskId = "async-task";
inline constexpr auto kMissingTaskId = "missing-task";
inline constexpr auto kAsyncRequestId = "async-request";
inline constexpr auto kAsyncTenant = "async-tenant";
inline constexpr auto kAsyncAuth = "Bearer async-token";
inline constexpr auto kAsyncExtension = "urn:a2a:async-test";
inline constexpr auto kLoopbackBind = "127.0.0.1:0";
inline constexpr auto kLoopbackPrefix = "127.0.0.1:";
inline constexpr auto kAsyncWait = std::chrono::seconds(5);
inline constexpr int kAsyncHistory = 3;
inline constexpr auto kUnaryCompletionFailed = "Unary client completion queue event failed";

// One explicitly polled queue supports concurrent test calls on older gRPC
// clients too. This thread is client infrastructure, independent of SDK workers.
class UnaryGrpcClient final : private core::NonCopyableOrMovable {
 public:
  UnaryGrpcClient() : poller_([this] { Poll(); }) {}
  ~UnaryGrpcClient() {
    queue_.Shutdown();
    poller_.join();
  }
  void GetTask(lf::a2a::v1::A2AService::Stub& stub, ::grpc::ClientContext& context,
               const lf::a2a::v1::GetTaskRequest& request, lf::a2a::v1::Task& response,
               std::function<void(::grpc::Status)> complete) {
    auto call = std::make_unique<Call>();
    call->complete = std::move(complete);
    call->reader = stub.AsyncGetTask(&context, request, &queue_);
    auto* tag = call.release();
    tag->reader->Finish(&response, &tag->status, tag);
  }

 private:
  struct Call final {
    // Keep the completion's owned request/context alive until reader destruction.
    std::function<void(::grpc::Status)> complete;
    ::grpc::Status status;
    std::unique_ptr<::grpc::ClientAsyncResponseReader<lf::a2a::v1::Task>> reader;
  };
  void Poll() {
    void* tag = nullptr;
    bool ok = false;
    while (queue_.Next(&tag, &ok)) {
      std::unique_ptr<Call> call(static_cast<Call*>(tag));
      call->complete(ok ? std::move(call->status)
                        : ::grpc::Status(::grpc::StatusCode::UNKNOWN, kUnaryCompletionFailed));
    }
  }
  ::grpc::CompletionQueue queue_;
  std::thread poller_;
};

inline lf::a2a::v1::Task AsyncTask() {
  lf::a2a::v1::Task task;
  task.set_id(kAsyncTaskId);
  task.mutable_status()->set_state(lf::a2a::v1::TASK_STATE_WORKING);
  for (int index = 0; index < kAsyncHistory; ++index) {
    task.add_history()->set_task_id(kAsyncTaskId);
  }
  return task;
}

class UnaryBackend final : public std::enable_shared_from_this<UnaryBackend> {
 public:
  using GetCompletion = server::engine::AsyncAgentExecutor::Completion<lf::a2a::v1::Task>;
  explicit UnaryBackend(bool pending = false) : pending_(pending) {}
  void Record(const server::engine::RequestExecutionContext& context) {
    auto observer = Observe(context);
    {
      std::lock_guard lock(mutex_);
      contexts_.push_back(context);
      workers_.insert(std::this_thread::get_id());
      observers_.push_back(std::move(observer));
    }
    changed_.notify_all();
  }
  void Get(const lf::a2a::v1::GetTaskRequest& request, server::engine::RequestExecutionContext context,
           GetCompletion complete) {
    if (request.id() == kMissingTaskId) {
      Record(context);
      complete(core::protocol_errors::TaskNotFound());
      return;
    }
    auto observer = Observe(context);
    GetCompletion immediate;
    {
      std::lock_guard lock(mutex_);
      contexts_.push_back(context);
      workers_.insert(std::this_thread::get_id());
      observers_.push_back(std::move(observer));
      if (pending_ && !released_) {
        replies_.push_back(std::move(complete));
      } else {
        immediate = std::move(complete);
      }
    }
    changed_.notify_all();
    if (immediate) {
      immediate(AsyncTask());
    }
  }
  core::Result<lf::a2a::v1::Task> GetSync(const lf::a2a::v1::GetTaskRequest& request) {
    if (request.id() == kMissingTaskId) {
      return core::protocol_errors::TaskNotFound();
    }
    std::unique_lock lock(mutex_);
    workers_.insert(std::this_thread::get_id());
    ++sync_calls_;
    changed_.notify_all();
    if (pending_) {
      changed_.wait(lock, [&] { return released_; });
    }
    lock.unlock();
    return AsyncTask();
  }
  [[nodiscard]] bool WaitForCalls(std::size_t count) {
    std::unique_lock lock(mutex_);
    return changed_.wait_for(lock, kAsyncWait, [&] { return contexts_.size() + sync_calls_ >= count; });
  }
  [[nodiscard]] bool WaitForStops(std::size_t count) {
    std::unique_lock lock(mutex_);
    return changed_.wait_for(lock, kAsyncWait, [&] { return stopped_.size() >= count; });
  }
  [[nodiscard]] std::vector<server::engine::RequestOutcome> Stopped() const {
    std::lock_guard lock(mutex_);
    return stopped_;
  }
  [[nodiscard]] server::engine::RequestExecutionContext Context(std::size_t index = 0) const {
    std::lock_guard lock(mutex_);
    return contexts_.at(index);
  }
  [[nodiscard]] std::size_t WorkerCount() const {
    std::lock_guard lock(mutex_);
    return workers_.size();
  }
  [[nodiscard]] GetCompletion PendingReply(std::size_t index = 0) const {
    std::lock_guard lock(mutex_);
    return replies_.at(index);
  }
  void Release() {
    std::vector<GetCompletion> replies;
    {
      std::lock_guard lock(mutex_);
      released_ = true;
      replies.swap(replies_);
    }
    changed_.notify_all();
    for (const auto& complete : replies) {
      complete(AsyncTask());
    }
  }

 private:
  using Observer = std::stop_callback<std::function<void()>>;
  std::unique_ptr<Observer> Observe(server::engine::RequestExecutionContext context) {
    const auto weak = weak_from_this();
    return std::make_unique<Observer>(context.StopToken(), [weak, context] {
      if (const auto backend = weak.lock()) {
        {
          std::lock_guard lock(backend->mutex_);
          backend->stopped_.push_back(context.Outcome());
        }
        backend->changed_.notify_all();
      }
    });
  }
  const bool pending_;
  mutable std::mutex mutex_;
  std::condition_variable changed_;
  bool released_ = false;
  std::size_t sync_calls_ = 0;
  std::vector<server::engine::RequestExecutionContext> contexts_;
  std::set<std::thread::id> workers_;
  std::vector<std::unique_ptr<Observer>> observers_;
  std::vector<server::engine::RequestOutcome> stopped_;
  std::vector<GetCompletion> replies_;
};

class UnaryAsyncExecutor final : public server::engine::AsyncAgentExecutor {
 public:
  explicit UnaryAsyncExecutor(std::shared_ptr<UnaryBackend> backend) : backend_(std::move(backend)) {}
  void SendMessage(lf::a2a::v1::SendMessageRequest /*request*/, server::engine::RequestExecutionContext context,
                   Completion<lf::a2a::v1::SendMessageResponse> complete) override {
    backend_->Record(context);
    lf::a2a::v1::SendMessageResponse reply;
    *reply.mutable_task() = AsyncTask();
    complete(std::move(reply));
  }
  void GetTask(lf::a2a::v1::GetTaskRequest request, server::engine::RequestExecutionContext context,
               Completion<lf::a2a::v1::Task> complete) override {
    backend_->Get(request, std::move(context), std::move(complete));
  }
  void ListTasks(server::ListTasksRequest /*request*/, server::engine::RequestExecutionContext context,
                 Completion<server::ListTasksResponse> complete) override {
    backend_->Record(context);
    server::ListTasksResponse reply;
    reply.tasks.push_back(AsyncTask());
    reply.page_size = 1;
    reply.total_size = 1;
    complete(std::move(reply));
  }
  void CancelTask(lf::a2a::v1::CancelTaskRequest /*request*/, server::engine::RequestExecutionContext context,
                  Completion<lf::a2a::v1::Task> complete) override {
    backend_->Record(context);
    auto reply = AsyncTask();
    reply.mutable_status()->set_state(lf::a2a::v1::TASK_STATE_CANCELED);
    complete(std::move(reply));
  }
  void CreateTaskPushNotificationConfig(lf::a2a::v1::TaskPushNotificationConfig /*request*/,
                                        server::engine::RequestExecutionContext context,
                                        Completion<lf::a2a::v1::TaskPushNotificationConfig> complete) override {
    backend_->Record(context);
    lf::a2a::v1::TaskPushNotificationConfig reply;
    reply.set_task_id(kAsyncTaskId);
    complete(std::move(reply));
  }
  void GetTaskPushNotificationConfig(lf::a2a::v1::GetTaskPushNotificationConfigRequest /*request*/,
                                     server::engine::RequestExecutionContext context,
                                     Completion<lf::a2a::v1::TaskPushNotificationConfig> complete) override {
    backend_->Record(context);
    lf::a2a::v1::TaskPushNotificationConfig reply;
    reply.set_task_id(kAsyncTaskId);
    complete(std::move(reply));
  }
  void ListTaskPushNotificationConfigs(
      lf::a2a::v1::ListTaskPushNotificationConfigsRequest /*request*/, server::engine::RequestExecutionContext context,
      Completion<lf::a2a::v1::ListTaskPushNotificationConfigsResponse> complete) override {
    backend_->Record(context);
    lf::a2a::v1::ListTaskPushNotificationConfigsResponse reply;
    reply.add_configs()->set_task_id(kAsyncTaskId);
    complete(std::move(reply));
  }
  void DeleteTaskPushNotificationConfig(lf::a2a::v1::DeleteTaskPushNotificationConfigRequest /*request*/,
                                        server::engine::RequestExecutionContext context,
                                        Completion<void> complete) override {
    backend_->Record(context);
    core::Result<void> reply;
    complete(std::move(reply));
  }

 private:
  const std::shared_ptr<UnaryBackend> backend_;
};

class UnarySyncExecutor final : public server::AgentExecutor {
 public:
  explicit UnarySyncExecutor(std::shared_ptr<UnaryBackend> backend) : backend_(std::move(backend)) {}
  core::Result<lf::a2a::v1::SendMessageResponse> SendMessage(const lf::a2a::v1::SendMessageRequest& /*request*/,
                                                             server::RequestContext& /*context*/) override {
    lf::a2a::v1::SendMessageResponse response;
    *response.mutable_task() = AsyncTask();
    return response;
  }
  core::Result<std::unique_ptr<server::ServerStreamSession>> SendStreamingMessage(
      const lf::a2a::v1::SendMessageRequest& /*request*/, server::RequestContext& /*context*/) override {
    return core::protocol_errors::UnsupportedOperation();
  }
  core::Result<lf::a2a::v1::Task> GetTask(const lf::a2a::v1::GetTaskRequest& request,
                                          server::RequestContext& /*context*/) override {
    return backend_->GetSync(request);
  }
  core::Result<server::ListTasksResponse> ListTasks(const server::ListTasksRequest& /*request*/,
                                                    server::RequestContext& /*context*/) override {
    server::ListTasksResponse response;
    response.tasks.push_back(AsyncTask());
    response.page_size = 1;
    response.total_size = 1;
    return response;
  }
  core::Result<lf::a2a::v1::Task> CancelTask(const lf::a2a::v1::CancelTaskRequest& /*request*/,
                                             server::RequestContext& /*context*/) override {
    return AsyncTask();
  }

 private:
  const std::shared_ptr<UnaryBackend> backend_;
};

class UnaryGrpcServer final : private core::NonCopyableOrMovable {
 public:
  explicit UnaryGrpcServer(std::shared_ptr<UnaryBackend> backend, bool asynchronous = true,
                           server::GrpcServerTransportOptions options = {},
                           server::engine::AsyncDispatcherOptions execution_options = {},
                           const ::grpc::ChannelArguments& client_options = {}) {
    backend_ = backend;
    if (execution_options.agent_card_provider == nullptr) {
      lf::a2a::v1::AgentCard card;
      card.set_name(kAsyncTaskId);
      execution_options.agent_card_provider = std::make_shared<core::StaticAgentCardProvider>(card);
    }
    ::grpc::Service* service = nullptr;
    if (asynchronous) {
      dispatcher_ = std::make_shared<server::engine::AsyncDispatcher>(std::make_shared<UnaryAsyncExecutor>(backend),
                                                                      execution_options);
      async_service_ = std::make_unique<server::AsyncGrpcServerTransport>(dispatcher_, options);
      service = async_service_.get();
    } else {
      sync_executor_ = std::make_unique<UnarySyncExecutor>(backend);
      sync_dispatcher_ =
          std::make_unique<server::Dispatcher>(sync_executor_.get(), execution_options.agent_card_provider);
      sync_service_ = std::make_unique<server::GrpcServerTransport>(sync_dispatcher_.get(), options);
      service = sync_service_.get();
    }
    ::grpc::ServerBuilder builder;
    builder.AddListeningPort(kLoopbackBind, ::grpc::InsecureServerCredentials(), &port_);
    builder.RegisterService(service);
    server_ = builder.BuildAndStart();
    std::string endpoint(kLoopbackPrefix);
    endpoint.append(std::to_string(port_));
    stub_ = lf::a2a::v1::A2AService::NewStub(
        ::grpc::CreateCustomChannel(endpoint, ::grpc::InsecureChannelCredentials(), client_options));
  }
  ~UnaryGrpcServer() {
    CloseAdmission();
    backend_->Release();
    if (server_ != nullptr) {
      server_->Shutdown();
      server_->Wait();
    }
  }
  void CloseAdmission() {
    if (async_service_ != nullptr) {
      async_service_->Shutdown();
    }
  }
  void Shutdown() {
    CloseAdmission();
    server_->Shutdown();
    server_->Wait();
  }
  [[nodiscard]] lf::a2a::v1::A2AService::Stub& Stub() const { return *stub_; }
  [[nodiscard]] UnaryGrpcClient& Client() { return client_; }
  [[nodiscard]] bool Ready() const { return server_ != nullptr && port_ != 0; }

 private:
  std::shared_ptr<UnaryBackend> backend_;
  int port_ = 0;
  std::shared_ptr<server::engine::AsyncDispatcher> dispatcher_;
  std::unique_ptr<server::AsyncGrpcServerTransport> async_service_;
  std::unique_ptr<UnarySyncExecutor> sync_executor_;
  std::unique_ptr<server::Dispatcher> sync_dispatcher_;
  std::unique_ptr<server::GrpcServerTransport> sync_service_;
  std::unique_ptr<::grpc::Server> server_;
  std::unique_ptr<lf::a2a::v1::A2AService::Stub> stub_;
  UnaryGrpcClient client_;
};

inline void PrepareGrpcClient(::grpc::ClientContext& context) {
  context.AddMetadata(std::string(server::GrpcServerTransport::kVersionMetadataKey),
                      std::string(core::Version::kProtocolVersion));
  context.set_deadline(std::chrono::system_clock::now() + kAsyncWait);
}
}  // namespace a2a::test
