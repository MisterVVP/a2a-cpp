// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "stream_cancellation_watcher.h"

#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>

namespace a2a::server {

StreamCancellationWatcher::StreamCancellationWatcher(::grpc::ServerContext* context, ServerStreamSession* stream)
    : StreamCancellationWatcher(
          context == nullptr ? IsCancelled{} : [context] { return context->IsCancelled(); }, stream) {}

StreamCancellationWatcher::StreamCancellationWatcher(IsCancelled is_cancelled, ServerStreamSession* stream)
    : StreamCancellationWatcher(std::move(is_cancelled), stream, Options{}) {}

StreamCancellationWatcher::StreamCancellationWatcher(IsCancelled is_cancelled, ServerStreamSession* stream,
                                                     Options options)
    : is_cancelled_(std::move(is_cancelled)), stream_(stream), options_(std::move(options)) {
  if (is_cancelled_ && stream_ != nullptr && stream_->IsLive()) {
#if A2A_STREAM_CANCELLATION_WATCHER_HAS_JTHREAD
    worker_ = std::jthread([this](const std::stop_token& stop_token) { Watch(stop_token); });
#else
    worker_ = std::thread([this] { Watch(); });
#endif
  }
}

StreamCancellationWatcher::~StreamCancellationWatcher() {
#if !A2A_STREAM_CANCELLATION_WATCHER_HAS_JTHREAD
  {
    std::lock_guard lock(wait_mutex_);
    stop_requested_ = true;
  }
  wait_condition_.notify_all();
  if (worker_.joinable()) {
    worker_.join();
  }
#endif
}

#if A2A_STREAM_CANCELLATION_WATCHER_HAS_JTHREAD
void StreamCancellationWatcher::Watch(const std::stop_token& stop_token) {
  while (!stop_token.stop_requested()) {
    if (is_cancelled_()) {
      stream_->Cancel();
      return;
    }

    std::unique_lock lock(wait_mutex_);
    if (options_.before_wait) {
      options_.before_wait();
    }
    wait_condition_.wait_for(lock, stop_token, options_.poll_interval, [] { return false; });
  }
}
#else
void StreamCancellationWatcher::Watch() {
  while (true) {
    if (is_cancelled_()) {
      stream_->Cancel();
      return;
    }

    std::unique_lock lock(wait_mutex_);
    if (options_.before_wait) {
      options_.before_wait();
    }
    if (wait_condition_.wait_for(lock, options_.poll_interval, [this] { return stop_requested_; })) {
      return;
    }
  }
}
#endif

}  // namespace a2a::server
