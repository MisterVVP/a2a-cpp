// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

#include <grpcpp/server_context.h>

#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

#include "a2a/core/non_copyable.h"
#include "a2a/server/server_stream_session.h"

#if defined(__cpp_lib_jthread) && !defined(A2A_FORCE_STREAM_CANCELLATION_WATCHER_THREAD_FALLBACK)
#define A2A_STREAM_CANCELLATION_WATCHER_HAS_JTHREAD 1
#else
#define A2A_STREAM_CANCELLATION_WATCHER_HAS_JTHREAD 0
#endif

namespace a2a::server {

constexpr std::chrono::milliseconds kStreamCancellationPollInterval{50};

class StreamCancellationWatcher final : private core::NonCopyable {
 public:
  using IsCancelled = std::function<bool()>;
  using BeforeWait = std::function<void()>;

  struct Options final {
    std::chrono::milliseconds poll_interval = kStreamCancellationPollInterval;
    BeforeWait before_wait;
  };

  StreamCancellationWatcher(::grpc::ServerContext* context, ServerStreamSession* stream);
  StreamCancellationWatcher(IsCancelled is_cancelled, ServerStreamSession* stream);
  StreamCancellationWatcher(IsCancelled is_cancelled, ServerStreamSession* stream, Options options);
  ~StreamCancellationWatcher();

 private:
#if A2A_STREAM_CANCELLATION_WATCHER_HAS_JTHREAD
  void Watch(const std::stop_token& stop_token);
#else
  void Watch();
#endif

  IsCancelled is_cancelled_;
  ServerStreamSession* stream_ = nullptr;
  Options options_;
  std::mutex wait_mutex_;
  std::condition_variable_any wait_condition_;
#if A2A_STREAM_CANCELLATION_WATCHER_HAS_JTHREAD
  std::jthread worker_;
#else
  bool stop_requested_ = false;
  std::thread worker_;
#endif
};

}  // namespace a2a::server
