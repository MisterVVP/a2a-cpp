// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "a2a/server/task_subscription_service.h"

#include <optional>
#include <utility>

#include "a2a/server/streaming_diagnostics.h"

namespace a2a::server {

TaskSubscriptionService::SubscriptionSession::SubscriptionSession(std::shared_ptr<ServiceState> service_state,
                                                                  std::shared_ptr<SubscriberState> state)
    : service_state_(std::move(service_state)), state_(std::move(state)) {}

TaskSubscriptionService::SubscriptionSession::~SubscriptionSession() { Cancel(); }

core::Result<std::optional<lf::a2a::v1::StreamResponse>> TaskSubscriptionService::SubscriptionSession::Next() {
  return TaskSubscriptionService::WaitForPublishedEvent(state_, std::nullopt);
}

core::Result<std::optional<lf::a2a::v1::StreamResponse>> TaskSubscriptionService::SubscriptionSession::NextFor(
    std::chrono::milliseconds timeout) {
  return TaskSubscriptionService::WaitForPublishedEvent(state_, timeout);
}

bool TaskSubscriptionService::SubscriptionSession::IsLive() const noexcept {
  return state_ != nullptr && (!state_->closed.load() || state_->queued_event_count.load() != 0);
}

void TaskSubscriptionService::SubscriptionSession::Cancel() noexcept {
  if (!cancelled_.exchange(true) && service_state_ != nullptr && state_ != nullptr) {
    TaskSubscriptionService::RemoveSubscriber(service_state_, state_);
  }
}

TaskSubscriptionService::~TaskSubscriptionService() { Shutdown(); }

core::Result<std::unique_ptr<ServerStreamSession>> TaskSubscriptionService::Subscribe(const lf::a2a::v1::Task& task) {
  auto subscriber_state = std::make_shared<SubscriberState>();
  subscriber_state->task_id = task.id();
  const auto service_state = state_;

  std::lock_guard lock(service_state->mutex);
  if (service_state->shutdown) {
    return core::Error::Internal("task subscription service is shut down");
  }

  lf::a2a::v1::Task current_task = task;
  const auto latest = service_state->latest_task_states_by_id.find(subscriber_state->task_id);
  if (latest != service_state->latest_task_states_by_id.end()) {
    current_task.set_context_id(latest->second.context_id);
    *current_task.mutable_status() = latest->second.status;
  }
  if (core::IsTerminalTaskState(current_task.status().state())) {
    return core::protocol_errors::UnsupportedOperation("task is already terminal");
  }
  service_state->subscribers_by_task_id[subscriber_state->task_id].push_back(subscriber_state);
  subscriber_state->events.push_back({.response = BuildCurrentTaskEvent(current_task)});
  subscriber_state->queued_event_count.store(1U);

  return std::unique_ptr<ServerStreamSession>(
      std::make_unique<SubscriptionSession>(service_state, std::move(subscriber_state)));
}

void TaskSubscriptionService::PublishTaskUpdated(const lf::a2a::v1::Task& task) {
  const auto service_state = state_;
  std::lock_guard publication_lock(service_state->publication_mutex);
  std::vector<std::shared_ptr<SubscriberState>> subscribers;
  {
    std::lock_guard lock(service_state->mutex);
    if (service_state->shutdown) {
      return;
    }
    service_state->latest_task_states_by_id.insert_or_assign(
        task.id(), LatestTaskState{.context_id = task.context_id(), .status = task.status()});
    auto iterator = service_state->subscribers_by_task_id.find(task.id());
    if (iterator == service_state->subscribers_by_task_id.end()) {
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
      service_state->subscribers_by_task_id.erase(iterator);
    }
  }

  lf::a2a::v1::StreamResponse event = BuildStatusUpdateEvent(task);
  const bool close_after_event = core::IsTerminalTaskState(task.status().state());
  const std::int64_t publication_started = close_after_event ? streaming_diagnostics::TerminalPublicationBegins() : 0;
  for (std::size_t index = 0; index < subscribers.size(); ++index) {
    const auto& subscriber = subscribers[index];
    {
      std::lock_guard lock(subscriber->mutex);
      if (!subscriber->closed.load()) {
        if (index + 1U == subscribers.size()) {
          subscriber->events.push_back({.response = std::move(event)});
        } else {
          subscriber->events.push_back({.response = event});
        }
        subscriber->events.back().notification_time =
            close_after_event ? streaming_diagnostics::SubscriberNotified(publication_started) : 0;
        subscriber->queued_event_count.fetch_add(1);
        subscriber->closed.store(close_after_event);
      }
    }
    subscriber->ready.notify_one();
  }
}

void TaskSubscriptionService::Shutdown() {
  const auto service_state = state_;
  std::vector<std::shared_ptr<SubscriberState>> subscribers;
  {
    std::lock_guard lock(service_state->mutex);
    if (service_state->shutdown) {
      return;
    }
    service_state->shutdown = true;
    for (auto& entry : service_state->subscribers_by_task_id) {
      for (auto& weak_subscriber : entry.second) {
        if (auto subscriber = weak_subscriber.lock(); subscriber != nullptr) {
          subscribers.push_back(std::move(subscriber));
        }
      }
    }
    service_state->subscribers_by_task_id.clear();
    service_state->latest_task_states_by_id.clear();
  }

  for (const auto& subscriber : subscribers) {
    {
      std::lock_guard lock(subscriber->mutex);
      subscriber->closed.store(true);
    }
    subscriber->ready.notify_all();
  }
}

void TaskSubscriptionService::RemoveSubscriber(const std::shared_ptr<ServiceState>& service_state,
                                               const std::shared_ptr<SubscriberState>& state) {
  {
    std::lock_guard state_lock(state->mutex);
    state->closed.store(true);
  }
  state->ready.notify_all();

  std::lock_guard lock(service_state->mutex);
  auto iterator = service_state->subscribers_by_task_id.find(state->task_id);
  if (iterator == service_state->subscribers_by_task_id.end()) {
    return;
  }
  auto& subscribers = iterator->second;
  std::erase_if(subscribers, [&state](const std::weak_ptr<SubscriberState>& weak_subscriber) {
    auto subscriber = weak_subscriber.lock();
    return subscriber == nullptr || subscriber == state;
  });
  if (subscribers.empty()) {
    service_state->subscribers_by_task_id.erase(iterator);
  }
}

std::optional<lf::a2a::v1::StreamResponse> TaskSubscriptionService::WaitForPublishedEvent(
    const std::shared_ptr<SubscriberState>& state, std::optional<std::chrono::milliseconds> timeout) {
  std::unique_lock lock(state->mutex);
  const auto is_ready = [&state] { return state->closed.load() || !state->events.empty(); };
  if (timeout.has_value()) {
    if (!state->ready.wait_for(lock, *timeout, is_ready)) {
      return std::nullopt;
    }
  } else {
    state->ready.wait(lock, is_ready);
  }
  if (state->events.empty()) {
    return std::nullopt;
  }
  SubscriberState::QueuedEvent event = std::move(state->events.front());
  state->events.pop_front();
  state->queued_event_count.fetch_sub(1);
  streaming_diagnostics::TerminalEventObserved(event.notification_time);
  return std::move(event.response);
}

lf::a2a::v1::StreamResponse TaskSubscriptionService::BuildCurrentTaskEvent(const lf::a2a::v1::Task& task) {
  lf::a2a::v1::StreamResponse event;
  auto* current_task = event.mutable_task();
  *current_task = task;
  current_task->clear_artifacts();
  current_task->clear_history();
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
