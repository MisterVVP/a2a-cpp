// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "a2a/server/task_subscription_service.h"

#include <optional>
#include <utility>

namespace a2a::server {

TaskSubscriptionService::SubscriptionSession::SubscriptionSession(TaskSubscriptionService* owner,
                                                                  std::shared_ptr<SubscriberState> state)
    : owner_(owner), state_(std::move(state)), coroutine_(TaskSubscriptionService::RunSubscription(state_)) {}

TaskSubscriptionService::SubscriptionSession::~SubscriptionSession() { Cancel(); }

core::Result<std::optional<lf::a2a::v1::StreamResponse>> TaskSubscriptionService::SubscriptionSession::Next() {
  try {
    return coroutine_.Next();
  } catch (const std::exception& ex) {
    return core::Error::Internal(ex.what());
  }
}

void TaskSubscriptionService::SubscriptionSession::Cancel() noexcept {
  if (owner_ != nullptr && state_ != nullptr) {
    owner_->RemoveSubscriber(state_);
    owner_ = nullptr;
  }
}

core::Result<std::unique_ptr<ServerStreamSession>> TaskSubscriptionService::Subscribe(const lf::a2a::v1::Task& task) {
  if (core::IsTerminalTaskState(task.status().state())) {
    return core::protocol_errors::UnsupportedOperation("task is already terminal");
  }

  auto state = std::make_shared<SubscriberState>();
  state->task_id = task.id();
  state->current_task = task;

  {
    std::lock_guard lock(mutex_);
    subscribers_by_task_id_[state->task_id].push_back(state);
  }

  return std::unique_ptr<ServerStreamSession>(std::make_unique<SubscriptionSession>(this, std::move(state)));
}

void TaskSubscriptionService::PublishTaskUpdated(const lf::a2a::v1::Task& task) {
  std::vector<std::shared_ptr<SubscriberState>> subscribers;
  {
    std::lock_guard lock(mutex_);
    auto iterator = subscribers_by_task_id_.find(task.id());
    if (iterator == subscribers_by_task_id_.end()) {
      return;
    }
    auto& weak_subscribers = iterator->second;
    std::erase_if(weak_subscribers, [&subscribers](const std::weak_ptr<SubscriberState>& weak_subscriber) {
      auto subscriber = weak_subscriber.lock();
      if (subscriber == nullptr) {
        return true;
      }
      subscribers.push_back(std::move(subscriber));
      return false;
    });
    if (weak_subscribers.empty() || core::IsTerminalTaskState(task.status().state())) {
      subscribers_by_task_id_.erase(iterator);
    }
  }

  lf::a2a::v1::StreamResponse event = BuildStatusUpdateEvent(task);
  const bool close_after_event = core::IsTerminalTaskState(task.status().state());
  for (const auto& subscriber : subscribers) {
    {
      std::lock_guard lock(subscriber->mutex);
      if (!subscriber->closed) {
        subscriber->events.push_back(event);
        subscriber->closed = close_after_event;
      }
    }
    subscriber->ready.notify_one();
  }
}

void TaskSubscriptionService::RemoveSubscriber(const std::shared_ptr<SubscriberState>& state) {
  {
    std::lock_guard state_lock(state->mutex);
    state->closed = true;
  }
  state->ready.notify_all();

  std::lock_guard lock(mutex_);
  auto iterator = subscribers_by_task_id_.find(state->task_id);
  if (iterator == subscribers_by_task_id_.end()) {
    return;
  }
  auto& subscribers = iterator->second;
  std::erase_if(subscribers, [&state](const std::weak_ptr<SubscriberState>& weak_subscriber) {
    auto subscriber = weak_subscriber.lock();
    return subscriber == nullptr || subscriber == state;
  });
  if (subscribers.empty()) {
    subscribers_by_task_id_.erase(iterator);
  }
}

std::optional<lf::a2a::v1::StreamResponse> TaskSubscriptionService::WaitForPublishedEvent(
    const std::shared_ptr<SubscriberState>& state) {
  std::unique_lock lock(state->mutex);
  state->ready.wait(lock, [&state] { return state->closed || !state->events.empty(); });
  if (state->events.empty()) {
    return std::nullopt;
  }
  lf::a2a::v1::StreamResponse event = std::move(state->events.front());
  state->events.pop_front();
  return event;
}

StreamResponseCoroutine TaskSubscriptionService::RunSubscription(std::shared_ptr<SubscriberState> state) {
  co_yield BuildCurrentTaskEvent(state->current_task);

  for (auto event = WaitForPublishedEvent(state); event.has_value(); event = WaitForPublishedEvent(state)) {
    const bool is_terminal =
        event->has_status_update() && core::IsTerminalTaskState(event->status_update().status().state());
    co_yield std::move(event.value());
    if (is_terminal) {
      co_return;
    }
  }
}

lf::a2a::v1::StreamResponse TaskSubscriptionService::BuildCurrentTaskEvent(const lf::a2a::v1::Task& task) {
  lf::a2a::v1::StreamResponse event;
  auto* current_task = event.mutable_task();
  *current_task = task;
  current_task->clear_artifacts();
  return event;
}

lf::a2a::v1::StreamResponse TaskSubscriptionService::BuildStatusUpdateEvent(const lf::a2a::v1::Task& task) {
  lf::a2a::v1::StreamResponse event;
  auto* update = event.mutable_status_update();
  update->set_task_id(task.id());
  update->set_context_id(task.context_id());
  *update->mutable_status() = task.status();
  return event;
}

}  // namespace a2a::server
