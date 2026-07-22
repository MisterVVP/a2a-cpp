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

namespace a2a::server {

constexpr std::chrono::milliseconds kStreamCancellationPollInterval{50};

class StreamCancellationWatcher final : private core::NonCopyable {
 public:
  using IsCancelled = std::function<bool()>;

  StreamCancellationWatcher(::grpc::ServerContext* context, ServerStreamSession* stream);
  StreamCancellationWatcher(IsCancelled is_cancelled, ServerStreamSession* stream);
  ~StreamCancellationWatcher();

 private:
#if defined(__cpp_lib_jthread)
  void Watch(const std::stop_token& stop_token);
#else
  void Watch();
#endif

  IsCancelled is_cancelled_;
  ServerStreamSession* stream_ = nullptr;
  std::mutex wait_mutex_;
  std::condition_variable_any wait_condition_;
#if defined(__cpp_lib_jthread)
  std::jthread worker_;
#else
  bool stop_requested_ = false;
  std::thread worker_;
#endif
};

}  // namespace a2a::server
