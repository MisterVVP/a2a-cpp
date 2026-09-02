// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "a2a/server/task_subscription_service.h"

#include <optional>
#include <utility>

#if defined(A2A_ENABLE_SUBSCRIPTION_DIAGNOSTICS)
#include "core/subscription_diagnostics.h"
#endif

namespace a2a::server {

std::optional<lf::a2a::v1::StreamResponse> StreamResponseCoroutine::Next() { return WaitForNext(std::nullopt); }

std::optional<lf::a2a::v1::StreamResponse> StreamResponseCoroutine::NextFor(std::chrono::milliseconds timeout) {
  return WaitForNext(timeout);
}

void StreamResponseCoroutine::Start() noexcept {
  if (!started_ && handle_) {
    started_ = true;
    std::lock_guard resume_lock(handle_.promise().resume_mutex_);
    handle_.resume();
  }
}

std::optional<lf::a2a::v1::StreamResponse> StreamResponseCoroutine::WaitForNext(
    std::optional<std::chrono::milliseconds> timeout) {
  Start();
  if (!handle_) {
    return std::nullopt;
  }
  auto& promise = handle_.promise();
  std::unique_lock lock(promise.mutex_);
  const auto ready = [&promise] { return promise.current_value_.has_value() || promise.done_.load(); };
  if (timeout.has_value() && !promise.ready_.wait_for(lock, *timeout, ready)) {
    return std::nullopt;
  }
  if (!timeout.has_value()) {
    promise.ready_.wait(lock, ready);
  }
  if (promise.exception_ != nullptr) {
    std::rethrow_exception(promise.exception_);
  }
  if (!promise.current_value_.has_value()) {
    return std::nullopt;
  }
  auto value = std::move(promise.current_value_);
  promise.current_value_.reset();
  lock.unlock();
  std::lock_guard resume_lock(promise.resume_mutex_);
  handle_.resume();
  return value;
}

class TaskSubscriptionService::SubscriberEventAwaitable final {
 public:
  explicit SubscriberEventAwaitable(std::shared_ptr<SubscriberState> state) : state_(std::move(state)) {}

  [[nodiscard]] bool await_ready() const noexcept {
    std::lock_guard lock(state_->mutex);
    return state_->closed.load() || !state_->events.empty();
  }
  [[nodiscard]] bool await_suspend(
      std::coroutine_handle<StreamResponseCoroutine::promise_type> continuation) const noexcept {
    std::lock_guard lock(state_->mutex);
    if (state_->closed.load() || !state_->events.empty()) {
      return false;
    }
    state_->continuation = continuation;
    return true;
  }
  [[nodiscard]] std::optional<lf::a2a::v1::StreamResponse> await_resume() const {
    std::lock_guard lock(state_->mutex);
    if (state_->events.empty()) {
      return std::nullopt;
    }
    auto event = std::move(state_->events.front());
    state_->events.pop_front();
    state_->queued_event_count.fetch_sub(1);
    return *event;
  }

 private:
  std::shared_ptr<SubscriberState> state_;
};

TaskSubscriptionService::SubscriptionSession::SubscriptionSession(std::shared_ptr<ServiceState> service_state,
                                                                  std::shared_ptr<SubscriberState> state)
    : service_state_(std::move(service_state)),
      state_(std::move(state)),
      coroutine_(TaskSubscriptionService::RunSubscription(state_)) {}

TaskSubscriptionService::SubscriptionSession::~SubscriptionSession() { Cancel(); }

core::Result<std::optional<lf::a2a::v1::StreamResponse>> TaskSubscriptionService::SubscriptionSession::Next() {
  try {
    return coroutine_.Next();
  } catch (const std::exception& ex) {
    return core::Error::Internal(ex.what());
  }
}

core::Result<std::optional<lf::a2a::v1::StreamResponse>> TaskSubscriptionService::SubscriptionSession::NextFor(
    std::chrono::milliseconds timeout) {
  try {
    return coroutine_.NextFor(timeout);
  } catch (const std::exception& ex) {
    return core::Error::Internal(ex.what());
  }
}

bool TaskSubscriptionService::SubscriptionSession::IsLive() const noexcept {
  return state_ != nullptr && !coroutine_.IsDone();
}

void TaskSubscriptionService::SubscriptionSession::Cancel() noexcept {
  if (!cancelled_.exchange(true) && service_state_ != nullptr && state_ != nullptr) {
    TaskSubscriptionService::RemoveSubscriber(service_state_, state_);
    TaskSubscriptionService::WaitForResumes(state_);
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

  subscriber_state->current_task = task;
  const auto latest = service_state->latest_task_states_by_id.find(subscriber_state->task_id);
  if (latest != service_state->latest_task_states_by_id.end()) {
    subscriber_state->current_task.set_context_id(latest->second.context_id);
    *subscriber_state->current_task.mutable_status() = latest->second.status;
  }
  if (core::IsTerminalTaskState(subscriber_state->current_task.status().state())) {
    return core::protocol_errors::UnsupportedOperation("task is already terminal");
  }
  service_state->subscribers_by_task_id[subscriber_state->task_id].push_back(subscriber_state);

  return std::unique_ptr<ServerStreamSession>(
      std::make_unique<SubscriptionSession>(service_state, std::move(subscriber_state)));
}

void TaskSubscriptionService::PublishTaskUpdated(const lf::a2a::v1::Task& task) {
#if defined(A2A_ENABLE_SUBSCRIPTION_DIAGNOSTICS)
  const core::subscription_diagnostics::ScopedTimer timer(
      core::subscription_diagnostics::Phase::kTerminalPublicationTotal,
      core::IsTerminalTaskState(task.status().state()));
#endif
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

  const auto event = std::make_shared<const lf::a2a::v1::StreamResponse>(BuildStatusUpdateEvent(task));
  const bool close_after_event = core::IsTerminalTaskState(task.status().state());
  for (const auto& subscriber : subscribers) {
    {
      std::lock_guard lock(subscriber->mutex);
      if (!subscriber->closed.load()) {
        subscriber->events.push_back(event);
        subscriber->queued_event_count.fetch_add(1);
        subscriber->closed.store(close_after_event);
      }
    }
    SignalSubscriber(subscriber);
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
    SignalSubscriber(subscriber);
  }
}

void TaskSubscriptionService::RemoveSubscriber(const std::shared_ptr<ServiceState>& service_state,
                                               const std::shared_ptr<SubscriberState>& state) {
  {
    std::lock_guard state_lock(state->mutex);
    state->closed.store(true);
  }
  SignalSubscriber(state);

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

void TaskSubscriptionService::SignalSubscriber(const std::shared_ptr<SubscriberState>& state) {
  std::coroutine_handle<StreamResponseCoroutine::promise_type> continuation;
  {
    std::lock_guard lock(state->mutex);
    continuation = std::exchange(state->continuation, {});
    if (continuation) {
      ++state->active_resumes;
    }
  }
  if (continuation) {
#if defined(A2A_ENABLE_SUBSCRIPTION_DIAGNOSTICS)
    const core::subscription_diagnostics::ScopedTimer timer(
        core::subscription_diagnostics::Phase::kSubscriberResumeCallback);
#endif
    std::lock_guard resume_lock(continuation.promise().resume_mutex_);
    continuation.resume();
    {
      std::lock_guard lock(state->mutex);
      --state->active_resumes;
    }
    state->ready.notify_all();
  }
}

void TaskSubscriptionService::WaitForResumes(const std::shared_ptr<SubscriberState>& state) {
  std::unique_lock lock(state->mutex);
  state->ready.wait(lock, [&state] { return state->active_resumes == 0; });
}

StreamResponseCoroutine TaskSubscriptionService::RunSubscription(std::shared_ptr<SubscriberState> state) {
  co_yield BuildCurrentTaskEvent(state->current_task);

  while (true) {
    auto event = co_await SubscriberEventAwaitable(state);
    if (!event.has_value()) {
      co_return;
    }
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
