// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include <grpcpp/create_channel.h>
#include <grpcpp/server.h>
#include <grpcpp/server_builder.h>
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "a2a/client/grpc_transport.h"
#include "a2a/core/extensions.h"
#include "a2a/core/http_constants.h"
#include "a2a/core/version.h"
#include "a2a/v1/a2a.grpc.pb.h"

namespace {
using namespace a2a;
using client::ListTasksRequest;
using client::ListTasksResponse;
constexpr auto kCancellationWait = std::chrono::seconds(1);
constexpr auto kGrpcTransport = "grpc";
constexpr auto kVersionMetadata = "a2a-version";
constexpr auto kWait = std::chrono::seconds(5);
constexpr auto kPoll = std::chrono::milliseconds(1);
constexpr auto kShortTimeout = std::chrono::milliseconds(30);
constexpr auto kBind = "127.0.0.1:0";
constexpr auto kPrefix = "127.0.0.1:";
constexpr auto kTaskId = "native-async-task";
constexpr int kPageSize = 2;
constexpr auto kPageToken = "native-page-token";
constexpr auto kConfigId = "native-config";
constexpr auto kRequestHeader = "x-async-metadata";
constexpr auto kHeaderValue = "metadata-value";
constexpr auto kAuthMetadata = "authorization";
constexpr auto kExtensionsMetadata = "a2a-extensions";
constexpr auto kAuth = "Bearer async-credential";
constexpr auto kExtension = "urn:a2a:async-test";
constexpr auto kCancelled = "CANCELLED";
constexpr auto kDeadline = "DEADLINE_EXCEEDED";
constexpr auto kRemoteError = "remote native async error";
constexpr std::size_t kConcurrentCalls = 32;

void ExpectMetadata(const ::grpc::ServerContext& context, std::string_view name, std::string_view value) {
  const auto& metadata = context.client_metadata();
  const auto found = metadata.find(::grpc::string_ref(name.data(), name.size()));
  ASSERT_NE(found, metadata.end());
  EXPECT_EQ(std::string_view(found->second.data(), found->second.size()), value);
}

template <typename T>
void ExpectProtocolError(const core::Result<T>& response, const char* code) {
  ASSERT_FALSE(response.ok());
  EXPECT_EQ(response.error().protocol_code(), code);
}

void ExpectInactive(const core::AsyncHandle& handle) { EXPECT_FALSE(handle.IsActive()); }

void ExpectPagination(const lf::a2a::v1::ListTasksRequest& request, int page_size) {
  EXPECT_TRUE(request.has_page_size());
  EXPECT_EQ(request.page_size(), page_size);
  EXPECT_EQ(request.page_token(), kPageToken);
}

class UnaryService final : public lf::a2a::v1::A2AService::Service {
 public:
  std::atomic<int> expected_page_size{0};
  std::atomic<bool> hold{false};
  std::atomic<bool> fail{false};
  std::atomic<bool> cancelled{false};
  std::mutex mutex;
  std::condition_variable changed;
  std::size_t received = 0;
  bool WaitForRequest() {
    std::unique_lock lock(mutex);
    return changed.wait_for(lock, kWait, [this] { return received != 0U; });
  }
  bool WaitForCancellation() {
    std::unique_lock lock(mutex);
    return changed.wait_for(lock, kCancellationWait, [this] { return cancelled.load(); });
  }
  ::grpc::Status SendMessage(::grpc::ServerContext* context, const lf::a2a::v1::SendMessageRequest* request,
                             lf::a2a::v1::SendMessageResponse* response) override {
    (void)request;
    response->mutable_task()->set_id(kTaskId);
    return Handle(context);
  }
  ::grpc::Status GetTask(::grpc::ServerContext* context, const lf::a2a::v1::GetTaskRequest* request,
                         lf::a2a::v1::Task* response) override {
    (void)request;
    response->set_id(kTaskId);
    return Handle(context);
  }
  ::grpc::Status ListTasks(::grpc::ServerContext* context, const lf::a2a::v1::ListTasksRequest* request,
                           lf::a2a::v1::ListTasksResponse* response) override {
    const auto page_size = expected_page_size.load();
    if (page_size == 0) {
      EXPECT_FALSE(request->has_page_size());
    } else {
      ExpectPagination(*request, page_size);
    }
    response->add_tasks()->set_id(kTaskId);
    return Handle(context);
  }
  ::grpc::Status CancelTask(::grpc::ServerContext* context, const lf::a2a::v1::CancelTaskRequest* request,
                            lf::a2a::v1::Task* response) override {
    (void)request;
    response->set_id(kTaskId);
    return Handle(context);
  }
  ::grpc::Status CreateTaskPushNotificationConfig(::grpc::ServerContext* context,
                                                  const lf::a2a::v1::TaskPushNotificationConfig* request,
                                                  lf::a2a::v1::TaskPushNotificationConfig* response) override {
    (void)request;
    response->set_id(kConfigId);
    response->set_task_id(kTaskId);
    return Handle(context);
  }
  ::grpc::Status GetTaskPushNotificationConfig(::grpc::ServerContext* context,
                                               const lf::a2a::v1::GetTaskPushNotificationConfigRequest* request,
                                               lf::a2a::v1::TaskPushNotificationConfig* response) override {
    (void)request;
    response->set_id(kConfigId);
    response->set_task_id(kTaskId);
    return Handle(context);
  }
  ::grpc::Status ListTaskPushNotificationConfigs(
      ::grpc::ServerContext* context, const lf::a2a::v1::ListTaskPushNotificationConfigsRequest* request,
      lf::a2a::v1::ListTaskPushNotificationConfigsResponse* response) override {
    (void)request;
    response->add_configs()->set_id(kConfigId);
    return Handle(context);
  }
  ::grpc::Status DeleteTaskPushNotificationConfig(::grpc::ServerContext* context,
                                                  const lf::a2a::v1::DeleteTaskPushNotificationConfigRequest* request,
                                                  google::protobuf::Empty* response) override {
    (void)request;
    (void)response;
    return Handle(context);
  }

 private:
  ::grpc::Status Handle(::grpc::ServerContext* context) {
    ExpectMetadata(*context, kRequestHeader, kHeaderValue);
    ExpectMetadata(*context, kAuthMetadata, kAuth);
    ExpectMetadata(*context, kExtensionsMetadata, kExtension);
    ExpectMetadata(*context, kVersionMetadata, core::Version::kProtocolVersion);
    EXPECT_GT(context->deadline(), std::chrono::system_clock::now() - kWait);
    EXPECT_LT(context->deadline(), std::chrono::system_clock::now() + kWait + kShortTimeout);
    {
      std::lock_guard lock(mutex);
      ++received;
    }
    changed.notify_all();
    while (hold.load() && !context->IsCancelled()) {
      std::this_thread::sleep_for(kPoll);
    }
    {
      std::lock_guard lock(mutex);
      cancelled.store(context->IsCancelled());
    }
    changed.notify_all();
    if (fail.load()) {
      return {::grpc::StatusCode::NOT_FOUND, kRemoteError};
    }
    return ::grpc::Status::OK;
  }
};

class NativeAsyncGrpcTest : public testing::Test {
 protected:
  void SetUp() override {
    ::grpc::ServerBuilder builder;
    int port = 0;
    builder.AddListeningPort(kBind, ::grpc::InsecureServerCredentials(), &port);
    builder.RegisterService(&service_);
    server_ = builder.BuildAndStart();
    ASSERT_NE(server_, nullptr);
    std::string target(kPrefix);
    target.append(std::to_string(port));
    client_ = std::make_unique<client::A2AClient>(std::make_unique<client::GrpcTransport>(
        client::ResolvedInterface{.transport = client::PreferredTransport::kGrpc,
                                  .url = target,
                                  .security_requirements = {},
                                  .security_schemes = {}},
        ::grpc::CreateChannel(target, ::grpc::InsecureChannelCredentials())));
  }
  void TearDown() override {
    service_.hold.store(false);
    EXPECT_TRUE(client_->Destroy().ok());
    server_->Shutdown();
  }
  static client::CallOptions Options() {
    client::CallOptions options;
    options.timeout = kWait;
    options.headers[kRequestHeader] = kHeaderValue;
    options.extensions = {kExtension};
    options.auth_hook = [](client::HeaderMap& headers) {
      headers[std::string(core::http::kAuthorizationHeaderName)] = kAuth;
    };
    return options;
  }
  template <typename T, typename Start>
  core::Result<T> Await(Start start) {
    std::promise<core::Result<T>> result;
    auto future = result.get_future();
    auto operation = start([&](core::Result<T> response) { result.set_value(std::move(response)); });
    EXPECT_TRUE(operation.ok());
    EXPECT_EQ(future.wait_for(kWait), std::future_status::ready);
    auto response = future.get();
    operation.value().Wait();
    return response;
  }
  UnaryService service_;
  std::unique_ptr<::grpc::Server> server_;
  std::unique_ptr<client::A2AClient> client_;
};

TEST_F(NativeAsyncGrpcTest, SendMessageUsesNativeCallbackApi) {
  lf::a2a::v1::SendMessageRequest request;
  auto response = Await<lf::a2a::v1::SendMessageResponse>(
      [&](auto completion) { return client_->SendMessageAsync(request, std::move(completion), Options()); });
  ASSERT_TRUE(response.ok()) << response.error().message();
  EXPECT_EQ(response.value().task().id(), kTaskId);
}

TEST_F(NativeAsyncGrpcTest, GetTaskUsesNativeCallbackApi) {
  lf::a2a::v1::GetTaskRequest request;
  request.set_id(kTaskId);
  auto response = Await<lf::a2a::v1::Task>(
      [&](auto completion) { return client_->GetTaskAsync(request, std::move(completion), Options()); });
  ASSERT_TRUE(response.ok()) << response.error().message();
  EXPECT_EQ(response.value().id(), kTaskId);
}

TEST_F(NativeAsyncGrpcTest, ListTasksUsesNativeCallbackApi) {
  ListTasksRequest request;
  auto response = Await<ListTasksResponse>(
      [&](auto completion) { return client_->ListTasksAsync(request, std::move(completion), Options()); });
  ASSERT_TRUE(response.ok()) << response.error().message();
  ASSERT_EQ(response.value().tasks.size(), 1U);
  EXPECT_EQ(response.value().tasks.front().id(), kTaskId);
}

TEST_F(NativeAsyncGrpcTest, ListTasksPreservesExplicitPagination) {
  service_.expected_page_size.store(kPageSize);
  client::ListTasksRequest request{.page_size = kPageSize, .page_token = kPageToken};
  auto response = Await<client::ListTasksResponse>(
      [&](auto completion) { return client_->ListTasksAsync(request, std::move(completion), Options()); });
  ASSERT_TRUE(response.ok()) << response.error().message();
}

TEST_F(NativeAsyncGrpcTest, CancelTaskUsesNativeCallbackApi) {
  lf::a2a::v1::CancelTaskRequest request;
  request.set_id(kTaskId);
  auto response = Await<lf::a2a::v1::Task>(
      [&](auto completion) { return client_->CancelTaskAsync(request, std::move(completion), Options()); });
  ASSERT_TRUE(response.ok()) << response.error().message();
  EXPECT_EQ(response.value().id(), kTaskId);
}

TEST_F(NativeAsyncGrpcTest, CreateTaskPushNotificationConfigUsesNativeCallbackApi) {
  lf::a2a::v1::TaskPushNotificationConfig request;
  request.set_task_id(kTaskId);
  auto response = Await<lf::a2a::v1::TaskPushNotificationConfig>([&](auto completion) {
    return client_->CreateTaskPushNotificationConfigAsync(request, std::move(completion), Options());
  });
  ASSERT_TRUE(response.ok()) << response.error().message();
  EXPECT_EQ(response.value().id(), kConfigId);
}

TEST_F(NativeAsyncGrpcTest, GetTaskPushNotificationConfigUsesNativeCallbackApi) {
  lf::a2a::v1::GetTaskPushNotificationConfigRequest request;
  request.set_task_id(kTaskId);
  request.set_id(kConfigId);
  auto response = Await<lf::a2a::v1::TaskPushNotificationConfig>([&](auto completion) {
    return client_->GetTaskPushNotificationConfigAsync(request, std::move(completion), Options());
  });
  ASSERT_TRUE(response.ok()) << response.error().message();
  EXPECT_EQ(response.value().id(), kConfigId);
}

TEST_F(NativeAsyncGrpcTest, ListTaskPushNotificationConfigsUsesNativeCallbackApi) {
  lf::a2a::v1::ListTaskPushNotificationConfigsRequest request;
  request.set_task_id(kTaskId);
  auto response = Await<lf::a2a::v1::ListTaskPushNotificationConfigsResponse>([&](auto completion) {
    return client_->ListTaskPushNotificationConfigsAsync(request, std::move(completion), Options());
  });
  ASSERT_TRUE(response.ok()) << response.error().message();
  ASSERT_EQ(response.value().configs_size(), 1);
  EXPECT_EQ(response.value().configs(0).id(), kConfigId);
}

TEST_F(NativeAsyncGrpcTest, DeleteTaskPushNotificationConfigUsesNativeCallbackApi) {
  lf::a2a::v1::DeleteTaskPushNotificationConfigRequest request;
  request.set_task_id(kTaskId);
  request.set_id(kConfigId);
  auto response = Await<void>([&](auto completion) {
    return client_->DeleteTaskPushNotificationConfigAsync(request, std::move(completion), Options());
  });
  ASSERT_TRUE(response.ok()) << response.error().message();
}

TEST_F(NativeAsyncGrpcTest, RemoteErrorsRetainGrpcProtocolCode) {
  service_.fail.store(true);
  lf::a2a::v1::GetTaskRequest request;
  request.set_id(kTaskId);
  auto result = Await<lf::a2a::v1::Task>(
      [&](auto completion) { return client_->GetTaskAsync(request, std::move(completion), Options()); });
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().transport(), kGrpcTransport);
  EXPECT_EQ(result.error().protocol_code(), std::to_string(static_cast<int>(::grpc::StatusCode::NOT_FOUND)));
}
TEST_F(NativeAsyncGrpcTest, CancelSignalsClientContextAndCompletesOnce) {
  service_.hold.store(true);
  lf::a2a::v1::GetTaskRequest request;
  request.set_id(kTaskId);
  std::promise<core::Result<lf::a2a::v1::Task>> result;
  auto future = result.get_future();
  std::atomic<int> calls{0};
  auto operation = client_->GetTaskAsync(
      request,
      [&](auto response) {
        ++calls;
        result.set_value(std::move(response));
      },
      Options());
  ASSERT_TRUE(operation.ok());
  ASSERT_TRUE(service_.WaitForRequest());
  operation.value().Cancel();
  ASSERT_EQ(future.wait_for(kWait), std::future_status::ready);
  auto response = future.get();
  ExpectProtocolError(response, kCancelled);
  operation.value().Wait();
  EXPECT_TRUE(service_.WaitForCancellation());
  EXPECT_EQ(calls.load(), 1);
}
TEST_F(NativeAsyncGrpcTest, DeadlineCompletesHeldNativeCall) {
  service_.hold.store(true);
  auto options = Options();
  options.timeout = kShortTimeout;
  lf::a2a::v1::GetTaskRequest request;
  request.set_id(kTaskId);
  auto result = Await<lf::a2a::v1::Task>(
      [&](auto completion) { return client_->GetTaskAsync(request, std::move(completion), options); });
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().protocol_code(), kDeadline);
}
TEST_F(NativeAsyncGrpcTest, ConcurrentOperationsAndDestroyDrainCallbacks) {
  std::vector<core::AsyncHandle> operations;
  std::atomic<std::size_t> calls{0};
  service_.hold.store(true);
  lf::a2a::v1::GetTaskRequest request;
  request.set_id(kTaskId);
  for (std::size_t index = 0; index < kConcurrentCalls; ++index) {
    auto operation = client_->GetTaskAsync(
        request,
        [&](auto result) {
          (void)result;
          ++calls;
        },
        Options());
    ASSERT_TRUE(operation.ok());
    operations.push_back(std::move(operation.value()));
  }
  ASSERT_TRUE(service_.WaitForRequest());
  EXPECT_TRUE(client_->Destroy().ok());
  EXPECT_EQ(calls.load(), kConcurrentCalls);
  for (const auto& operation : operations) {
    ExpectInactive(operation);
  }
}
}  // namespace
