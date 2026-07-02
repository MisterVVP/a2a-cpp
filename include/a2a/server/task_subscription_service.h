// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
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
    std::string task_id;
    lf::a2a::v1::Task current_task;
    std::deque<lf::a2a::v1::StreamResponse> events;
    std::atomic_bool closed = false;
    std::atomic_size_t queued_event_count = 0;
    std::optional<std::chrono::milliseconds> wait_timeout;
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
    [[nodiscard]] bool IsLive() const noexcept override;
    void Cancel() noexcept override;

   private:
    std::shared_ptr<ServiceState> service_state_;
    std::shared_ptr<SubscriberState> state_;
    std::atomic_bool cancelled_ = false;
    StreamResponseCoroutine coroutine_;
  };

  static void RemoveSubscriber(const std::shared_ptr<ServiceState>& service_state,
                               const std::shared_ptr<SubscriberState>& state);
  static std::optional<lf::a2a::v1::StreamResponse> WaitForPublishedEvent(
      const std::shared_ptr<SubscriberState>& state);
  static StreamResponseCoroutine RunSubscription(std::shared_ptr<SubscriberState> state);
  static lf::a2a::v1::StreamResponse BuildCurrentTaskEvent(const lf::a2a::v1::Task& task);
  static lf::a2a::v1::StreamResponse BuildStatusUpdateEvent(const lf::a2a::v1::Task& task);

  std::shared_ptr<ServiceState> state_ = std::make_shared<ServiceState>();
};

}  // namespace a2a::server
