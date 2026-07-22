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
    : is_cancelled_(std::move(is_cancelled)), stream_(stream) {
  if (is_cancelled_ && stream_ != nullptr && stream_->IsLive()) {
    worker_ = std::jthread([this](std::stop_token stop_token) { Watch(stop_token); });
  }
}

void StreamCancellationWatcher::Watch(std::stop_token stop_token) {
  while (!stop_token.stop_requested()) {
    if (is_cancelled_()) {
      stream_->Cancel();
      return;
    }

    std::unique_lock lock(wait_mutex_);
    wait_condition_.wait_for(lock, stop_token, kStreamCancellationPollInterval, [] { return false; });
  }
}

}  // namespace a2a::server
