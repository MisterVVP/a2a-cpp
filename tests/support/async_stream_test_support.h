// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

#include <atomic>
#include <boost/asio/experimental/concurrent_channel.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <condition_variable>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "a2a/server/engine/async_task_subscription.h"
#include "a2a/server/engine/coroutine_agent_executor.h"
#include "a2a/server/task_subscription_service.h"
#include "support/asio_test_support.h"

namespace a2a::test {
inline constexpr auto kStreamTask = "stream-task";
inline constexpr auto kStreamTenant = "stream-tenant";
inline constexpr auto kStreamError = "stream backend error";
inline constexpr auto kStreamHeader = "x-stream-metadata";
inline constexpr auto kStreamMetadata = "owned-stream-metadata";
inline constexpr std::size_t kStreamEvents = 3;
inline constexpr std::size_t kStreamConcurrency = 64;
inline constexpr std::size_t kStreamSmallCapacity = 2;
using StreamAck = boost::asio::experimental::concurrent_channel<void(boost::system::error_code, bool)>;

inline lf::a2a::v1::Task StreamTask(lf::a2a::v1::TaskState state = lf::a2a::v1::TASK_STATE_WORKING) {
  lf::a2a::v1::Task task;
  task.set_id(kStreamTask);
  task.set_context_id(kStreamTenant);
  task.mutable_status()->set_state(state);
  return task;
}
inline server::DispatchRequest StreamRequest(bool subscribe = false) {
  if (subscribe) {
    lf::a2a::v1::GetTaskRequest request;
    request.set_id(kStreamTask);
    request.set_tenant(kStreamTenant);
    return {.operation = server::DispatcherOperation::kSubscribeTask, .payload = request};
  }
  return {.operation = server::DispatcherOperation::kSendStreamingMessage,
          .payload = lf::a2a::v1::SendMessageRequest{}};
}
class StreamBackend final : public server::engine::CoroutineAgentExecutor {
 public:
  boost::asio::awaitable<core::Result<void>> SendStreamingMessage(
      lf::a2a::v1::SendMessageRequest /*request*/, server::engine::RequestExecutionContext context,
      std::shared_ptr<server::engine::AsyncServerStreamSession> stream) override {
    Record(context);
    for (std::size_t index = 0; index < event_count; ++index) {
      lf::a2a::v1::StreamResponse event;
      event.mutable_message()->set_message_id(std::to_string(index));
      event.mutable_message()->set_task_id(kStreamTask);
      if (payload_bytes != 0) {
        event.mutable_message()->add_parts()->set_text(std::string(payload_bytes, 'x'));
      }
      const auto result = co_await stream->Write(std::move(event));
      if (!result.ok()) {
        co_return result.error();
      }
      ++acknowledged;
    }
    co_return fail ? core::Result<void>(core::Error::Validation(kStreamError)) : core::Result<void>{};
  }
  boost::asio::awaitable<core::Result<void>> SubscribeTask(
      lf::a2a::v1::GetTaskRequest request, server::engine::RequestExecutionContext context,
      std::shared_ptr<server::engine::AsyncServerStreamSession> stream) override {
    Record(context);
    {
      std::lock_guard lock(mutex);
      tenant = request.tenant();
    }
    co_return co_await server::engine::AsyncTaskSubscription::Run(publications, StreamTask(), context,
                                                                  std::move(stream), capacity);
  }
  bool WaitStarted(std::size_t count = 1) {
    std::unique_lock lock(mutex);
    return changed.wait_for(lock, kCoroutineWait, [&] { return started >= count; });
  }
  std::size_t ThreadCount() {
    std::lock_guard lock(mutex);
    return threads.size();
  }
  server::TaskSubscriptionService publications;
  std::size_t capacity = kStreamSmallCapacity;
  bool fail = false;
  std::size_t event_count = kStreamEvents;
  std::size_t payload_bytes = 0;
  std::atomic_size_t started = 0;
  std::atomic_size_t acknowledged = 0;
  std::mutex mutex;
  std::condition_variable changed;
  std::set<std::thread::id> threads;
  server::RequestContext metadata;
  std::string tenant;

 private:
  void Record(const server::engine::RequestExecutionContext& context) {
    {
      std::lock_guard lock(mutex);
      metadata = context.Metadata();
      threads.insert(std::this_thread::get_id());
      ++started;
    }
    changed.notify_all();
  }
};

class FakeStreamSink final : public server::engine::AsyncServerStreamSession {
 public:
  boost::asio::awaitable<core::Result<void>> Write(lf::a2a::v1::StreamResponse event) override {
    auto acknowledgement = std::make_shared<StreamAck>(co_await boost::asio::this_coro::executor, 1);
    events.push_back(std::move(event));
    pending = acknowledgement;
    if (automatic) {
      (void)acknowledgement->try_send(boost::system::error_code{}, true);
    }
    const bool accepted = co_await acknowledgement->async_receive(boost::asio::use_awaitable);
    co_return accepted ? core::Result<void>{} : core::Error::Network(kStreamError);
  }
  void Acknowledge(bool ok = true) {
    if (pending != nullptr) {
      (void)pending->try_send(boost::system::error_code{}, ok);
    }
  }
  bool automatic = true;
  std::vector<lf::a2a::v1::StreamResponse> events;
  std::shared_ptr<StreamAck> pending;
};
}  // namespace a2a::test
