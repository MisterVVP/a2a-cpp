// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "a2a/core/protocol_errors.h"
#include "a2a/core/result.h"
#include "a2a/core/task_states.h"
#include "a2a/server/server_stream_session.h"
#include "a2a/server/stream_response_coroutine.h"
#include "a2a/v1/a2a.pb.h"

namespace a2a::server {

class TaskSubscriptionService final {
 public:
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
    bool closed = false;
    std::mutex mutex;
    std::condition_variable ready;
  };

  class SubscriptionSession final : public ServerStreamSession {
   public:
    SubscriptionSession(TaskSubscriptionService* owner, std::shared_ptr<SubscriberState> state);
    ~SubscriptionSession() override;

    [[nodiscard]] core::Result<std::optional<lf::a2a::v1::StreamResponse>> Next() override;
    [[nodiscard]] bool IsLive() const noexcept override { return true; }
    void Cancel() noexcept override;

   private:
    TaskSubscriptionService* owner_ = nullptr;
    std::shared_ptr<SubscriberState> state_;
    StreamResponseCoroutine coroutine_;
  };

  void RemoveSubscriber(const std::shared_ptr<SubscriberState>& state);
  static std::optional<lf::a2a::v1::StreamResponse> WaitForPublishedEvent(
      const std::shared_ptr<SubscriberState>& state);
  static StreamResponseCoroutine RunSubscription(std::shared_ptr<SubscriberState> state);
  static lf::a2a::v1::StreamResponse BuildCurrentTaskEvent(const lf::a2a::v1::Task& task);
  static lf::a2a::v1::StreamResponse BuildStatusUpdateEvent(const lf::a2a::v1::Task& task);

  std::mutex mutex_;
  std::unordered_map<std::string, std::vector<std::weak_ptr<SubscriberState>>> subscribers_by_task_id_;
  std::unordered_map<std::string, LatestTaskState> latest_task_states_by_id_;
  bool shutdown_ = false;
};

}  // namespace a2a::server
