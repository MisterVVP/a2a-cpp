// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include <benchmark/benchmark.h>

#include <string>

#include "a2a/server/push_notification_delivery.h"
#include "a2a/server/push_notification_service.h"
#include "a2a/server/push_notification_store.h"
#include "bench_common.h"

namespace {

class BenchmarkDeliveryClient final : public a2a::server::PushNotificationDeliveryClient {
 public:
  a2a::core::Result<a2a::server::PushDeliveryResult> Deliver(const a2a::server::PushDeliveryRequest& request) override {
    benchmark::DoNotOptimize(request);
    return a2a::server::PushDeliveryResult{.http_status = 204, .error_message = {}};
  }
};

lf::a2a::v1::TaskPushNotificationConfig BuildPushConfig(std::string_view id = a2a::bench::kPushConfigId) {
  lf::a2a::v1::TaskPushNotificationConfig config;
  config.set_task_id(std::string(a2a::bench::kTaskId));
  config.set_id(std::string(id));
  config.set_url(std::string(a2a::bench::kPushUrl));
  config.mutable_authentication()->set_scheme("Bearer");
  config.mutable_authentication()->set_credentials("benchmark-token");
  return config;
}

void BM_PushNotificationStore_CreateOrUpdate(benchmark::State& state) {
  std::size_t index = 0;
  for (auto _ : state) {
    state.PauseTiming();
    a2a::server::InMemoryPushNotificationStore store;
    auto config = BuildPushConfig("push-create-" + std::to_string(index));
    ++index;
    state.ResumeTiming();
    auto result = store.CreateOrUpdate(config);
    benchmark::DoNotOptimize(result);
  }
}
BENCHMARK(BM_PushNotificationStore_CreateOrUpdate);

void BM_PushNotificationStore_List_1000Configs(benchmark::State& state) {
  a2a::server::InMemoryPushNotificationStore store;
  for (std::size_t index = 0; index < a2a::bench::kTaskCount; ++index) {
    const auto result = store.CreateOrUpdate(BuildPushConfig("push-" + std::to_string(index)));
    benchmark::DoNotOptimize(result);
  }
  for (auto _ : state) {
    auto result = store.List(a2a::bench::kTaskId);
    benchmark::DoNotOptimize(result);
  }
}
BENCHMARK(BM_PushNotificationStore_List_1000Configs);

void BM_PushNotificationService_Notify_1000Configs(benchmark::State& state) {
  a2a::server::InMemoryTaskStore task_store;
  a2a::server::InMemoryPushNotificationStore push_store;
  BenchmarkDeliveryClient delivery;
  a2a::server::PushNotificationService service(&task_store, &push_store, &delivery);
  const auto task = a2a::bench::BuildTask();
  const auto created_task = task_store.CreateOrUpdate(task);
  benchmark::DoNotOptimize(created_task);
  for (std::size_t index = 0; index < a2a::bench::kTaskCount; ++index) {
    const auto result = push_store.CreateOrUpdate(BuildPushConfig("push-" + std::to_string(index)));
    benchmark::DoNotOptimize(result);
  }
  for (auto _ : state) {
    auto result = service.NotifyTaskUpdated(task);
    benchmark::DoNotOptimize(result);
  }
}
BENCHMARK(BM_PushNotificationService_Notify_1000Configs);

}  // namespace
