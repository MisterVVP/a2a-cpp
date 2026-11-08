// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include <array>
#include <atomic>
#include <chrono>
#include <exception>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "a2a/server/async_push_notification_delivery.h"
#include "support/curl_reactor_test_support.h"

namespace {
using namespace a2a;
constexpr std::array<std::size_t, 3> kCounts{1, 16, 64};
constexpr auto kTimeout = std::chrono::seconds(30);
constexpr auto kEmpty = "";
constexpr auto kTask = "evidence-task";
constexpr auto kCsvHeader = "mode,in_flight,threads_before,threads_pending,elapsed_us,errors\n";
constexpr auto kAsync = "async";
constexpr auto kSync = "sync";
constexpr std::size_t kThreadBound = 9;

server::PushDeliveryRequest Request(const std::string& url) {
  server::PushDeliveryRequest request;
  request.config.set_url(url);
  request.payload.mutable_status_update()->set_task_id(kTask);
  request.payload.mutable_status_update()->mutable_status()->set_state(lf::a2a::v1::TASK_STATE_COMPLETED);
  return request;
}
bool Measure(std::size_t count, bool asynchronous) {
  http::testing::CurlLoopbackServer webhook(count, kEmpty);
  server::HttpPushNotificationDeliveryOptions options{
      .timeout = kTimeout, .http_version = std::string(core::http::kHttpVersion11), .fallback_http_version = {}};
  std::atomic<std::size_t> errors{0};
  server::HttpPushNotificationDeliveryClient sync(options);
  server::HttpAsyncPushNotificationDeliveryClient async(options);
  const auto before = http::testing::ReactorTestThreadCount();
  const auto began = std::chrono::steady_clock::now();
  const auto request = Request(webhook.url());
  std::vector<core::AsyncHandle> handles;
  std::vector<std::thread> workers;
  handles.reserve(count);
  workers.reserve(count);
  for (std::size_t index = 0; index < count; ++index) {
    if (asynchronous) {
      auto started = async.DeliverAsync(request, [&](auto result) { errors += result.ok() ? 0U : 1U; });
      if (started.ok()) {
        handles.push_back(std::move(started.value()));
      } else {
        ++errors;
      }
    } else {
      workers.emplace_back([&] { errors += sync.Deliver(request).ok() ? 0U : 1U; });
    }
  }
  const bool connected = webhook.WaitForStreams(count);
  const auto pending = http::testing::ReactorTestThreadCount();
  if (connected) {
    for (std::size_t index = 0; index < count; ++index) {
      (void)webhook.Finish(index, kEmpty);
    }
  } else {
    async.Shutdown();
  }
  for (const auto& handle : handles) {
    handle.Wait();
  }
  for (auto& worker : workers) {
    worker.join();
  }
  const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - began);
  std::cout << (asynchronous ? kAsync : kSync) << ',' << count << ',' << before << ',' << pending << ','
            << elapsed.count() << ',' << errors.load() << '\n';
  return connected && errors == 0U && (!asynchronous || pending <= before + kThreadBound);
}
}  // namespace
int main() try {
  std::cout << kCsvHeader;
  bool passed = true;
  for (const auto count : kCounts) {
    passed = Measure(count, false) && passed;
    passed = Measure(count, true) && passed;
  }
  return passed ? 0 : 1;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
} catch (...) {
  return 1;
}
