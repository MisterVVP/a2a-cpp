// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <coroutine>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "a2a/core/non_copyable.h"
#include "a2a/core/protocol_errors.h"
#include "a2a/core/result.h"
#include "a2a/core/task_states.h"
#include "a2a/server/server_stream_session.h"
#include "a2a/server/stream_response_coroutine.h"
#include "a2a/v1/a2a.pb.h"

namespace a2a::server {

class TaskSubscriptionService final : private core::NonCopyableOrMovable {
 public:
  TaskSubscriptionService() = default;
  ~TaskSubscriptionService();

  [[nodiscard]] core::Result<std::unique_ptr<ServerStreamSession>> Subscribe(const lf::a2a::v1::Task& task);
  void PublishTaskUpdated(const lf::a2a::v1::Task& task);
  void Shutdown();

 private:
  struct LatestTaskState final {
    std::string context_id;
    lf::a2a::v1::TaskStatus status;
  };

  struct SubscriberState final {
    // The subscriber mutex protects both queued events and the suspended
    // continuation. A signaler removes the continuation and accounts for an
    // active resume while holding the mutex, then always resumes without any
    // service, publication, or subscriber lock held. Session destruction
    // waits for that accounting to reach zero before destroying the frame.
    // A resumer releases the frame's resume mutex before dropping its count.
    std::string task_id;
    lf::a2a::v1::Task current_task;
    std::deque<std::shared_ptr<const lf::a2a::v1::StreamResponse>> events;
    std::atomic_bool closed = false;
    std::size_t queued_bytes = 0;
    std::size_t max_queued_bytes = (std::numeric_limits<std::size_t>::max)();
    std::atomic_bool overflowed = false;
    // Counts published events until Next()/NextFor() hands them to the caller,
    // including an event already staged at a coroutine yield point.
    std::atomic_size_t pending_delivery_count = 0;
    std::coroutine_handle<StreamResponseCoroutine::promise_type> continuation;
    std::size_t active_resumes = 0;
    std::mutex mutex;
    std::condition_variable ready;
  };

  struct ServiceState final {
    std::mutex publication_mutex;
    std::mutex mutex;
    std::unordered_map<std::string, std::vector<std::weak_ptr<SubscriberState>>> subscribers_by_task_id;
    std::unordered_map<std::string, LatestTaskState> latest_task_states_by_id;
    bool shutdown = false;
  };

  class SubscriptionSession final : public ServerStreamSession {
   public:
    SubscriptionSession(std::shared_ptr<ServiceState> service_state, std::shared_ptr<SubscriberState> state);
    ~SubscriptionSession() override;

    [[nodiscard]] core::Result<std::optional<lf::a2a::v1::StreamResponse>> Next() override;
    [[nodiscard]] core::Result<std::optional<lf::a2a::v1::StreamResponse>> NextFor(
        std::chrono::milliseconds timeout) override;
    [[nodiscard]] bool SupportsTimedNext() const noexcept override { return true; }
    [[nodiscard]] bool IsLive() const noexcept override;
    [[nodiscard]] bool SetReadyCallback(std::function<void()> callback) override {
      coroutine_.SetReadyCallback(std::move(callback));
      return true;
    }
    void SetPendingEventByteLimit(std::size_t limit) override {
      std::lock_guard lock(state_->mutex);
      state_->max_queued_bytes = limit;
      if (state_->queued_bytes > limit) {
        state_->overflowed.store(true);
      }
    }
    void Cancel() noexcept override;

   private:
    void RecordDeliveredEvent(const std::optional<lf::a2a::v1::StreamResponse>& event) noexcept;
    std::shared_ptr<ServiceState> service_state_;
    std::shared_ptr<SubscriberState> state_;
    std::atomic_bool cancelled_ = false;
    StreamResponseCoroutine coroutine_;
  };

  static void RemoveSubscriber(const std::shared_ptr<ServiceState>& service_state,
                               const std::shared_ptr<SubscriberState>& state);
  class SubscriberEventAwaitable;
  static void SignalSubscriber(const std::shared_ptr<SubscriberState>& state);
  static void WaitForResumes(const std::shared_ptr<SubscriberState>& state);
  static StreamResponseCoroutine RunSubscription(std::shared_ptr<SubscriberState> state);
  static lf::a2a::v1::StreamResponse BuildCurrentTaskEvent(const lf::a2a::v1::Task& task);
  static lf::a2a::v1::StreamResponse BuildStatusUpdateEvent(const lf::a2a::v1::Task& task);

  std::shared_ptr<ServiceState> state_ = std::make_shared<ServiceState>();
};

}  // namespace a2a::server
