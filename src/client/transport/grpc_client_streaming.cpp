// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include <grpcpp/support/client_callback.h>

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

#include "a2a/core/async_operation.h"
#include "a2a/v1/a2a.grpc.pb.h"
#include "grpc_transport_internal.h"

namespace a2a::client::grpc_internal {

struct NativeStreams::State final {
  struct Call final {
    std::shared_ptr<StreamHandle::State> stream;
    std::shared_ptr<::grpc::ClientContext> context;
  };
  std::shared_ptr<lf::a2a::v1::A2AService::StubInterface> stub;
  const std::shared_ptr<std::atomic_bool> closed = std::make_shared<std::atomic_bool>(false);
  std::mutex mutex;
  std::condition_variable finished;
  std::unordered_map<StreamHandle::State*, Call> calls;
};

namespace {
constexpr auto kStreamsClosed = "gRPC streaming transport is shut down";

template <typename Request>
class ReadReactor final : public ::grpc::ClientReadReactor<lf::a2a::v1::StreamResponse>,
                          private core::NonCopyableOrMovable {
 public:
  ReadReactor(std::shared_ptr<NativeStreams::State> owner, std::shared_ptr<StreamHandle::State> stream,
              std::shared_ptr<::grpc::ClientContext> context, Request request, StreamObserver& observer)
      : owner_(std::move(owner)),
        stream_(std::move(stream)),
        context_(std::move(context)),
        request_(std::move(request)),
        observer_(observer) {}

  ~ReadReactor() override {
    if (registered_) {
      // Release native resources before allowing an external shutdown to return.
      context_.reset();
      {
        std::lock_guard lock(owner_->mutex);
        owner_->calls.erase(stream_.get());
      }
      owner_->finished.notify_all();
    }
  }

  bool Register() {
    std::lock_guard lock(owner_->mutex);
    if (owner_->closed->load()) {
      return false;
    }
    owner_->calls.emplace(stream_.get(), NativeStreams::State::Call{.stream = stream_, .context = context_});
    registered_ = true;
    return true;
  }

  template <typename Invoke>
  void Prepare(const Invoke& invoke) {
    invoke(owner_->stub->async(), context_.get(), &request_, this);
    this->StartRead(&event_);
  }

  void OnReadDone(bool ok) override {
    if (!ok) {
      return;
    }
    {
      StreamHandle::State::CallbackExecutionScope callback_scope(*stream_);
      if (Suppressed()) {
        return;
      }
      try {
        observer_.OnEvent(event_);
      } catch (...) {
        stream_->cancel_requested.store(true);
        stream_->active.store(false);
        context_->TryCancel();
        return;
      }
    }
    // End the observer scope before a new read can invoke the next reaction.
    // One read at a time preserves ordering and backpressure without a frame queue.
    if (!Suppressed()) {
      this->StartRead(&event_);
    }
  }

  void OnDone(const ::grpc::Status& status) override {
    {
      StreamHandle::State::CallbackExecutionScope callback_scope(*stream_);
      if (!Suppressed()) {
        try {
          if (status.ok()) {
            observer_.OnCompleted();
          } else {
            observer_.OnError(BuildGrpcError(status));
          }
        } catch (...) {
          // Application exceptions must not escape gRPC or prevent native cleanup.
          stream_->cancel_requested.store(true);
        }
      }
      stream_->active.store(false);
    }
    // gRPC guarantees OnDone runs after all other reactions have returned.
    delete this;
  }

 private:
  [[nodiscard]] bool Suppressed() const { return stream_->cancel_requested.load() || owner_->closed->load(); }
  const std::shared_ptr<NativeStreams::State> owner_;
  const std::shared_ptr<StreamHandle::State> stream_;
  std::shared_ptr<::grpc::ClientContext> context_;
  Request request_;
  StreamObserver& observer_;
  lf::a2a::v1::StreamResponse event_;
  bool registered_ = false;
};

template <typename Request, typename Invoke>
core::Result<std::shared_ptr<StreamHandle::State>> StartStream(const std::shared_ptr<NativeStreams::State>& owner,
                                                               const Request& request, StreamObserver& observer,
                                                               std::unique_ptr<::grpc::ClientContext> context,
                                                               const Invoke& invoke) {
  auto stream = std::make_shared<StreamHandle::State>();
  stream->transport_shutdown = owner->closed;
  auto shared_context = std::shared_ptr<::grpc::ClientContext>(std::move(context));
  stream->RegisterCancelCallback([weak_context = std::weak_ptr(shared_context)] {
    if (const auto active = weak_context.lock()) {
      active->TryCancel();
    }
  });
  auto call = std::make_unique<ReadReactor<Request>>(owner, stream, shared_context, request, observer);
  if (!call->Register()) {
    return core::Error::Network(kStreamsClosed);
  }
  call->Prepare(invoke);
  // Transfer ownership before StartCall: OnDone may delete the reactor immediately.
  auto* active = call.release();
  active->StartCall();
  return stream;
}
}  // namespace

NativeStreams::NativeStreams(const std::shared_ptr<::grpc::Channel>& channel) : state_(std::make_shared<State>()) {
  state_->stub = lf::a2a::v1::A2AService::NewStub(channel);
}

NativeStreams::~NativeStreams() { Shutdown(); }

core::Result<std::shared_ptr<StreamHandle::State>> NativeStreams::Start(
    const lf::a2a::v1::SendMessageRequest& request, StreamObserver& observer,
    std::unique_ptr<::grpc::ClientContext> context) {
  return StartStream(state_, request, observer, std::move(context),
                     [](auto* stub, auto* rpc_context, const auto* input, auto* reactor) {
                       stub->SendStreamingMessage(rpc_context, input, reactor);
                     });
}

core::Result<std::shared_ptr<StreamHandle::State>> NativeStreams::Start(
    const lf::a2a::v1::SubscribeToTaskRequest& request, StreamObserver& observer,
    std::unique_ptr<::grpc::ClientContext> context) {
  return StartStream(state_, request, observer, std::move(context),
                     [](auto* stub, auto* rpc_context, const auto* input, auto* reactor) {
                       stub->SubscribeToTask(rpc_context, input, reactor);
                     });
}

void NativeStreams::Shutdown() {
  std::vector<State::Call> calls;
  {
    std::lock_guard lock(state_->mutex);
    state_->closed->store(true);
    calls.reserve(state_->calls.size());
    for (const auto& [key, call] : state_->calls) {
      (void)key;
      calls.push_back(call);
    }
  }
  for (const auto& call : calls) {
    call.stream->cancel_requested.store(true);
    call.stream->active.store(false);
    call.context->TryCancel();
  }
  if (!core::async_detail::IsCallbackThread()) {
    for (const auto& call : calls) {
      call.stream->WaitForCallbackIdle();
    }
    std::unique_lock lock(state_->mutex);
    state_->finished.wait(lock, [this] { return state_->calls.empty(); });
  }
}

}  // namespace a2a::client::grpc_internal
