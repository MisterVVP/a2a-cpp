#include <grpcpp/security/server_credentials.h>
#include <grpcpp/server.h>
#include <grpcpp/server_builder.h>

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "a2a/server/grpc_server_transport.h"
#include "a2a/server/server.h"

namespace {
class StreamSession final : public a2a::server::ServerStreamSession {
 public:
  explicit StreamSession(std::vector<lf::a2a::v1::StreamResponse> events) : events_(std::move(events)) {}
  a2a::core::Result<std::optional<lf::a2a::v1::StreamResponse>> Next() override {
    if (index_ >= events_.size()) {
      return std::optional<lf::a2a::v1::StreamResponse>{};
    }
    return std::optional<lf::a2a::v1::StreamResponse>(events_[index_++]);
  }

 private:
  std::vector<lf::a2a::v1::StreamResponse> events_;
  std::size_t index_ = 0;
};

class Executor final : public a2a::server::AgentExecutor {
 public:
  explicit Executor(a2a::server::TaskStore* store) : store_(store) {}
  a2a::core::Result<lf::a2a::v1::SendMessageResponse> SendMessage(const lf::a2a::v1::SendMessageRequest& request,
                                                                  a2a::server::RequestContext& context) override {
    (void)context;
    lf::a2a::v1::Task task;
    task.set_id(request.message().task_id());
    task.mutable_status()->set_state(lf::a2a::v1::TASK_STATE_WORKING);
    if (auto s = store_->CreateOrUpdate(task); !s.ok()) {
      return s.error();
    }
    lf::a2a::v1::SendMessageResponse response;
    *response.mutable_task() = task;
    return response;
  }
  a2a::core::Result<std::unique_ptr<a2a::server::ServerStreamSession>> SendStreamingMessage(
      const lf::a2a::v1::SendMessageRequest& request, a2a::server::RequestContext& context) override {
    (void)context;
    lf::a2a::v1::StreamResponse event;
    event.mutable_task()->set_id(request.message().task_id());
    event.mutable_task()->mutable_status()->set_state(lf::a2a::v1::TASK_STATE_WORKING);
    return std::unique_ptr<a2a::server::ServerStreamSession>(std::make_unique<StreamSession>(std::vector{event}));
  }
  a2a::core::Result<lf::a2a::v1::Task> GetTask(const lf::a2a::v1::GetTaskRequest& request,
                                               a2a::server::RequestContext& context) override {
    (void)context;
    return store_->Get(request.id());
  }
  a2a::core::Result<a2a::server::ListTasksResponse> ListTasks(const a2a::server::ListTasksRequest& request,
                                                              a2a::server::RequestContext& context) override {
    (void)context;
    return store_->List(request);
  }
  a2a::core::Result<lf::a2a::v1::Task> CancelTask(const lf::a2a::v1::CancelTaskRequest& request,
                                                  a2a::server::RequestContext& context) override {
    (void)context;
    return store_->Cancel(request.id());
  }

 private:
  a2a::server::TaskStore* store_;
};
}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    return 2;
  }
  a2a::server::InMemoryTaskStore store;
  Executor executor(&store);
  a2a::server::Dispatcher dispatcher(&executor);
  a2a::server::GrpcServerTransport service(&dispatcher);

  grpc::ServerBuilder builder;
  builder.AddListeningPort(argv[1], grpc::InsecureServerCredentials());
  builder.RegisterService(&service);
  std::unique_ptr<grpc::Server> server = builder.BuildAndStart();
  if (!server) {
    return 1;
  }
  server->Wait();
  return 0;
}
