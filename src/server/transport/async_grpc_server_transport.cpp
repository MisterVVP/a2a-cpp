// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "a2a/server/async_grpc_server_transport.h"

#include <grpc/support/time.h>

#include <atomic>
#include <boost/asio/experimental/concurrent_channel.hpp>
#include <boost/asio/recycling_allocator.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <chrono>
#include <cstddef>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <utility>
#include <vector>

#include "server/transport/grpc_error.h"
#include "server/transport/grpc_request_context.h"
#include "server/transport/grpc_unary_conversion.h"

namespace a2a::server {
namespace {
constexpr auto kMissingDispatcher = "Async gRPC dispatcher is required";
constexpr auto kServiceClosed = "Async gRPC service is shut down";
constexpr auto kMissingArguments = "Request and response are required";
constexpr auto kUnexpectedPayload = "Async gRPC dispatch returned an unexpected payload";
using engine::RequestCancellationReason;
using engine::RequestExecution;
using engine::RequestExecutionContext;
using engine::RequestOutcome;

std::optional<RequestExecution::Clock::time_point> SteadyDeadline(std::chrono::system_clock::time_point deadline) {
  if (deadline == std::chrono::system_clock::time_point::max()) {
    return std::nullopt;
  }
  const auto now = RequestExecution::Clock::now();
  const auto remaining = deadline - std::chrono::system_clock::now();
  if (remaining <= decltype(remaining)::zero()) {
    return now;
  }
  const auto maximum = RequestExecution::Clock::time_point::max() - now;
  if (std::chrono::duration<long double>(remaining) >= std::chrono::duration<long double>(maximum)) {
    return RequestExecution::Clock::time_point::max();
  }
  return now + std::chrono::duration_cast<RequestExecution::Clock::duration>(remaining);
}

std::optional<::grpc::StatusCode> CancellationStatus(RequestOutcome outcome) {
  switch (outcome) {
    case RequestOutcome::kClientCancelled:
      return ::grpc::StatusCode::CANCELLED;
    case RequestOutcome::kDeadlineExceeded:
      return ::grpc::StatusCode::DEADLINE_EXCEEDED;
    case RequestOutcome::kServerShutdown:
      return ::grpc::StatusCode::UNAVAILABLE;
    default:
      return std::nullopt;
  }
}

::grpc::ServerUnaryReactor* Reject(::grpc::CallbackServerContext& context, const ::grpc::Status& status) {
  auto* reactor = context.DefaultReactor();
  reactor->Finish(status);
  return reactor;
}
}  // namespace

struct AsyncGrpcServerTransport::State final {
  struct Call final {
    struct Shutdown final {
      Call* call;
      void operator()() const { call->Cancel(RequestCancellationReason::kServerShutdown); }
    };

    explicit Call(std::shared_ptr<engine::AsyncDispatcher> target) : dispatcher(std::move(target)) {}

    void Bind(const std::shared_ptr<RequestExecution>& execution, std::stop_token token) {
      source = execution;
      shutdown.emplace(std::move(token), Shutdown{this});
    }

    void Cancel(RequestCancellationReason reason) {
      if (source && !cancelled.test_and_set()) {
        dispatcher->Cancel(source, reason);
      }
    }

    const std::shared_ptr<engine::AsyncDispatcher> dispatcher;
    std::vector<std::string> extensions;
    std::shared_ptr<RequestExecution> source;
    std::atomic_flag cancelled{};
    std::optional<std::stop_callback<Shutdown>> shutdown;
  };

  template <typename Response>
  class Reactor final : public ::grpc::ServerUnaryReactor {
   public:
    Reactor(std::shared_ptr<engine::AsyncDispatcher> dispatcher, ::grpc::CallbackServerContext& context,
            Response& response)
        : call(std::move(dispatcher)), context_(context), response_(response) {}
    Call call;
    void Complete(core::Result<DispatchResponse> result, const RequestExecutionContext& execution) {
      ::grpc::Status status;
      if (!result.ok()) {
        status = GrpcErrorStatus(result.error(), &context_, call.extensions, CancellationStatus(execution.Outcome()));
      } else {
        status = WriteGrpcUnaryResponse(result.value(), response_, kUnexpectedPayload);
        AddGrpcActivatedExtensions(&context_, call.extensions);
      }
      // OnDone can delete this immediately after Finish. No member access follows.
      Finish(status);
    }
    void OnCancel() override {
      const auto deadline = context_.raw_deadline();
      const auto reason = gpr_time_cmp(deadline, gpr_now(deadline.clock_type)) <= 0
                              ? RequestCancellationReason::kDeadlineExceeded
                              : RequestCancellationReason::kClientCancelled;
      call.Cancel(reason);
    }
    void OnDone() override { delete this; }

   private:
    ::grpc::CallbackServerContext& context_;
    Response& response_;
  };

  class StreamingReactor final : public ::grpc::ServerWriteReactor<lf::a2a::v1::StreamResponse> {
   public:
    using Acknowledgement = boost::asio::experimental::concurrent_channel<void(boost::system::error_code, bool)>;
    struct Session final : engine::AsyncServerStreamSession, std::enable_shared_from_this<Session> {
      boost::asio::awaitable<core::Result<void>> Write(lf::a2a::v1::StreamResponse event) override {
        return WriteOwned(shared_from_this(), std::move(event), {});
      }
      boost::asio::awaitable<core::Result<void>> WriteBatch(std::vector<lf::a2a::v1::StreamResponse> events) override {
        if (events.empty() || events.size() > kMaxBatchEvents) {
          return AsyncServerStreamSession::WriteBatch(std::move(events));
        }
        return WriteOwned(shared_from_this(), {}, std::move(events));
      }
      static boost::asio::awaitable<core::Result<void>> WriteOwned(std::shared_ptr<Session> session,
                                                                   lf::a2a::v1::StreamResponse event,
                                                                   std::vector<lf::a2a::v1::StreamResponse> events) {
        const auto scheduler = co_await boost::asio::this_coro::executor;
        std::shared_ptr<Acknowledgement> ack;
        {
          std::lock_guard lock(session->mutex);
          if (session->reactor == nullptr || session->terminal.has_value() || session->writing) {
            co_return core::Error::Network(kStreamClosed);
          }
          if (session->acknowledgement == nullptr) {
            session->acknowledgement = std::allocate_shared<Acknowledgement>(
                boost::asio::recycling_allocator<Acknowledgement>{}, scheduler, 1);
          }
          ack = session->acknowledgement;
          session->writing = true;
          session->StartWrite(std::move(event), std::move(events));
        }
        const bool ok = co_await ack->async_receive(boost::asio::use_awaitable);
        co_return ok ? core::Result<void>{} : core::Error::Network(kStreamWriteFailed);
      }
      void StartWrite(lf::a2a::v1::StreamResponse event, std::vector<lf::a2a::v1::StreamResponse> events) {
        if (events.empty()) {
          current = std::move(event);
          reactor->StartWrite(&current);
          return;
        }
        batch = std::move(events);
        next = 1;
        reactor->StartWrite(&batch.front());
      }
      std::mutex mutex;
      StreamingReactor* reactor = nullptr;
      lf::a2a::v1::StreamResponse current;
      std::vector<lf::a2a::v1::StreamResponse> batch;
      std::size_t next = 0;
      std::shared_ptr<Acknowledgement> acknowledgement;
      std::optional<::grpc::Status> terminal;
      bool writing = false;
    };

    StreamingReactor(std::shared_ptr<engine::AsyncDispatcher> dispatcher, ::grpc::CallbackServerContext& context)
        : call(std::move(dispatcher)), context_(context), session_(std::make_shared<Session>()) {
      session_->reactor = this;
    }
    Call call;
    [[nodiscard]] std::shared_ptr<Session> session() const { return session_; }
    void Complete(core::Result<DispatchResponse> result, const RequestExecutionContext& execution) {
      auto status = result.ok()
                        ? ::grpc::Status::OK
                        : GrpcErrorStatus(result.error(), &context_, {}, CancellationStatus(execution.Outcome()));
      FinishOnce(status);
    }
    void FinishOnce(const ::grpc::Status& status) {
      const auto session = session_;
      std::lock_guard lock(session->mutex);
      if (session->terminal.has_value()) {
        return;
      }
      AddGrpcActivatedExtensions(&context_, call.extensions);
      session->terminal = status;
      if (session->writing) {
        // A stalled HTTP/2 write must not hold host shutdown waiting for demand.
        // gRPC cancellation releases its write callback; OnDone still owns cleanup.
        session->terminal = ::grpc::Status(::grpc::StatusCode::CANCELLED, status.error_message());
        context_.TryCancel();
      } else {
        session->acknowledgement.reset();
        Finish(status);
      }
    }
    void OnWriteDone(bool ok) override {
      const auto session = session_;
      std::lock_guard lock(session->mutex);
      const bool more = !session->batch.empty() && session->next < session->batch.size();
      if (more && ok && !session->terminal.has_value() && !context_.IsCancelled()) {
        StartWrite(&session->batch[session->next++]);
        return;
      }
      if (!session->batch.empty()) {
        session->batch = decltype(session->batch){};
      }
      session->writing = false;
      (void)session->acknowledgement->try_send(boost::system::error_code{}, ok && !more);
      const auto terminal = session->terminal;
      if (terminal.has_value()) {
        session->acknowledgement.reset();
        Finish(terminal.value());
      }
    }
    void OnCancel() override {
      const auto deadline = context_.raw_deadline();
      call.Cancel(gpr_time_cmp(deadline, gpr_now(deadline.clock_type)) <= 0
                      ? RequestCancellationReason::kDeadlineExceeded
                      : RequestCancellationReason::kClientCancelled);
    }
    void OnDone() override {
      {
        std::lock_guard lock(session_->mutex);
        session_->reactor = nullptr;
      }
      delete this;
    }

   private:
    static constexpr auto kStreamClosed = "Async gRPC stream is closed or already writing";
    static constexpr auto kStreamWriteFailed = "Async gRPC stream write failed";
    ::grpc::CallbackServerContext& context_;
    const std::shared_ptr<Session> session_;
  };

  template <typename Request>
  ::grpc::ServerWriteReactor<lf::a2a::v1::StreamResponse>* StartStream(::grpc::CallbackServerContext& context,
                                                                       const Request* request,
                                                                       DispatcherOperation operation) {
    auto* reactor = new StreamingReactor(dispatcher, context);
    auto metadata = BuildGrpcRequestContext(context, extensions);
    if (request == nullptr || !metadata.ok()) {
      reactor->FinishOnce(request == nullptr ? ::grpc::Status(::grpc::StatusCode::INVALID_ARGUMENT, kMissingArguments)
                                             : GrpcErrorStatus(metadata.error(), &context));
      return reactor;
    }
    reactor->call.extensions = std::move(metadata.value().activated_extensions);
    auto routed = ParseGrpcUnaryRequest(operation, *request);
    if (!routed.ok()) {
      reactor->FinishOnce(GrpcErrorStatus(routed.error(), &context));
      return reactor;
    }
    if (shutdown.stop_requested()) {
      reactor->FinishOnce(GrpcErrorStatus(core::Error::Network(kServiceClosed), &context));
      return reactor;
    }
    const auto admitted = dispatcher->DispatchImpl(
        std::move(routed.value()), std::move(metadata.value().request_context),
        [reactor](auto result, const RequestExecutionContext& execution) {
          reactor->Complete(std::move(result), execution);
        },
        SteadyDeadline(context.deadline()), reactor->session(), {},
        [&](const auto& source) { reactor->call.Bind(source, shutdown.get_token()); });
    if (!admitted.ok()) {
      reactor->FinishOnce(GrpcErrorStatus(admitted.error(), &context));
    }
    return reactor;
  }

  State(std::shared_ptr<engine::AsyncDispatcher> target, GrpcServerTransportOptions options)
      : dispatcher(std::move(target)), extensions(std::move(options.required_extensions)) {}

  template <typename Request, typename Response>
  ::grpc::ServerUnaryReactor* Start(::grpc::CallbackServerContext& context, const Request* request, Response* response,
                                    DispatcherOperation operation) {
    if (request == nullptr || response == nullptr) {
      return Reject(context, {::grpc::StatusCode::INVALID_ARGUMENT, kMissingArguments});
    }
    auto metadata = BuildGrpcRequestContext(context, extensions);
    if (!metadata.ok()) {
      return Reject(context, GrpcErrorStatus(metadata.error(), &context));
    }
    auto routed = ParseGrpcUnaryRequest(operation, *request);
    if (!routed.ok()) {
      return Reject(context, GrpcErrorStatus(routed.error(), &context, metadata.value().activated_extensions));
    }
    if (shutdown.stop_requested()) {
      return Reject(context, GrpcErrorStatus(core::Error::Network(kServiceClosed), &context,
                                             metadata.value().activated_extensions));
    }
    auto* reactor = new Reactor<Response>(dispatcher, context, *response);
    reactor->call.extensions = std::move(metadata.value().activated_extensions);
    const auto admitted = dispatcher->DispatchImpl(
        std::move(routed.value()), std::move(metadata.value().request_context),
        [reactor](auto result, const RequestExecutionContext& execution) {
          reactor->Complete(std::move(result), execution);
        },
        SteadyDeadline(context.deadline()), nullptr, {},
        [&](const auto& source) { reactor->call.Bind(source, shutdown.get_token()); });
    if (!admitted.ok()) {
      reactor->Finish(GrpcErrorStatus(admitted.error(), &context, reactor->call.extensions));
    }
    return reactor;
  }

  const std::shared_ptr<engine::AsyncDispatcher> dispatcher;
  const RequiredExtensionsValidator extensions;
  std::stop_source shutdown;
};

AsyncGrpcServerTransport::AsyncGrpcServerTransport(std::shared_ptr<engine::AsyncDispatcher> dispatcher,
                                                   GrpcServerTransportOptions options) {
  if (dispatcher == nullptr) {
    throw std::invalid_argument(kMissingDispatcher);
  }
  state_ = std::make_shared<State>(std::move(dispatcher), std::move(options));
}
AsyncGrpcServerTransport::~AsyncGrpcServerTransport() { Shutdown(); }
void AsyncGrpcServerTransport::Shutdown() { (void)state_->shutdown.request_stop(); }

::grpc::ServerUnaryReactor* AsyncGrpcServerTransport::SendMessage(::grpc::CallbackServerContext* context,
                                                                  const lf::a2a::v1::SendMessageRequest* request,
                                                                  lf::a2a::v1::SendMessageResponse* response) {
  return state_->Start(*context, request, response, DispatcherOperation::kSendMessage);
}

::grpc::ServerUnaryReactor* AsyncGrpcServerTransport::GetTask(::grpc::CallbackServerContext* context,
                                                              const lf::a2a::v1::GetTaskRequest* request,
                                                              lf::a2a::v1::Task* response) {
  return state_->Start(*context, request, response, DispatcherOperation::kGetTask);
}

::grpc::ServerUnaryReactor* AsyncGrpcServerTransport::ListTasks(::grpc::CallbackServerContext* context,
                                                                const lf::a2a::v1::ListTasksRequest* request,
                                                                lf::a2a::v1::ListTasksResponse* response) {
  return state_->Start(*context, request, response, DispatcherOperation::kListTasks);
}

::grpc::ServerUnaryReactor* AsyncGrpcServerTransport::CancelTask(::grpc::CallbackServerContext* context,
                                                                 const lf::a2a::v1::CancelTaskRequest* request,
                                                                 lf::a2a::v1::Task* response) {
  return state_->Start(*context, request, response, DispatcherOperation::kCancelTask);
}

::grpc::ServerUnaryReactor* AsyncGrpcServerTransport::CreateTaskPushNotificationConfig(
    ::grpc::CallbackServerContext* context, const lf::a2a::v1::TaskPushNotificationConfig* request,
    lf::a2a::v1::TaskPushNotificationConfig* response) {
  return state_->Start(*context, request, response, DispatcherOperation::kCreateTaskPushNotificationConfig);
}

::grpc::ServerUnaryReactor* AsyncGrpcServerTransport::GetTaskPushNotificationConfig(
    ::grpc::CallbackServerContext* context, const lf::a2a::v1::GetTaskPushNotificationConfigRequest* request,
    lf::a2a::v1::TaskPushNotificationConfig* response) {
  return state_->Start(*context, request, response, DispatcherOperation::kGetTaskPushNotificationConfig);
}

::grpc::ServerUnaryReactor* AsyncGrpcServerTransport::ListTaskPushNotificationConfigs(
    ::grpc::CallbackServerContext* context, const lf::a2a::v1::ListTaskPushNotificationConfigsRequest* request,
    lf::a2a::v1::ListTaskPushNotificationConfigsResponse* response) {
  return state_->Start(*context, request, response, DispatcherOperation::kListTaskPushNotificationConfigs);
}

::grpc::ServerUnaryReactor* AsyncGrpcServerTransport::DeleteTaskPushNotificationConfig(
    ::grpc::CallbackServerContext* context, const lf::a2a::v1::DeleteTaskPushNotificationConfigRequest* request,
    google::protobuf::Empty* response) {
  return state_->Start(*context, request, response, DispatcherOperation::kDeleteTaskPushNotificationConfig);
}

::grpc::ServerUnaryReactor* AsyncGrpcServerTransport::GetExtendedAgentCard(
    ::grpc::CallbackServerContext* context, const lf::a2a::v1::GetExtendedAgentCardRequest* request,
    lf::a2a::v1::AgentCard* response) {
  return state_->Start(*context, request, response, DispatcherOperation::kGetExtendedAgentCard);
}

::grpc::ServerWriteReactor<lf::a2a::v1::StreamResponse>* AsyncGrpcServerTransport::SendStreamingMessage(
    ::grpc::CallbackServerContext* context, const lf::a2a::v1::SendMessageRequest* request) {
  return state_->StartStream(*context, request, DispatcherOperation::kSendStreamingMessage);
}
::grpc::ServerWriteReactor<lf::a2a::v1::StreamResponse>* AsyncGrpcServerTransport::SubscribeToTask(
    ::grpc::CallbackServerContext* context, const lf::a2a::v1::SubscribeToTaskRequest* request) {
  return state_->StartStream(*context, request, DispatcherOperation::kSubscribeTask);
}

}  // namespace a2a::server
