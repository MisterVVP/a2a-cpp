// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include <benchmark/benchmark.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "a2a/server/push_notification_delivery.h"
#include "a2a/server/push_notification_service.h"
#include "a2a/server/push_notification_store.h"
#include "bench_common.h"

namespace {

inline constexpr int kHttpNoContentStatus = 204;
inline constexpr std::string_view kAuthScheme = "Bearer";
inline constexpr std::string_view kAuthCredentials = "benchmark-token";
inline constexpr std::string_view kCreateConfigIdPrefix = "push-create-";
inline constexpr std::string_view kSeedConfigIdPrefix = "push-";
inline constexpr std::string_view kPushSeedError = "failed to seed push notification config";
inline constexpr std::string_view kTaskSeedError = "failed to seed task";

std::string BuildIndexedId(std::string_view prefix, std::size_t index) {
  const auto suffix = std::to_string(index);
  std::string id;
  id.reserve(prefix.size() + suffix.size());
  id.append(prefix);
  id.append(suffix);
  return id;
}

class BenchmarkDeliveryClient final : public a2a::server::PushNotificationDeliveryClient {
 public:
  a2a::core::Result<a2a::server::PushDeliveryResult> Deliver(const a2a::server::PushDeliveryRequest& request) override {
    benchmark::DoNotOptimize(request);
    return a2a::server::PushDeliveryResult{.http_status = kHttpNoContentStatus, .error_message = {}};
  }
};

lf::a2a::v1::TaskPushNotificationConfig BuildPushConfig(std::string_view id = a2a::bench::kPushConfigId) {
  lf::a2a::v1::TaskPushNotificationConfig config;
  config.set_task_id(std::string(a2a::bench::kTaskId));
  config.set_id(std::string(id));
  config.set_url(std::string(a2a::bench::kPushUrl));
  config.mutable_authentication()->set_scheme(std::string(kAuthScheme));
  config.mutable_authentication()->set_credentials(std::string(kAuthCredentials));
  return config;
}

std::vector<lf::a2a::v1::TaskPushNotificationConfig> BuildPushConfigs(std::string_view id_prefix,
                                                                      std::size_t config_count) {
  std::vector<lf::a2a::v1::TaskPushNotificationConfig> configs;
  configs.reserve(config_count);
  for (std::size_t index = 0; index < config_count; ++index) {
    configs.push_back(BuildPushConfig(BuildIndexedId(id_prefix, index)));
  }
  return configs;
}

bool SeedPushStore(a2a::server::InMemoryPushNotificationStore& store,
                   const std::vector<lf::a2a::v1::TaskPushNotificationConfig>& configs) {
  for (const auto& config : configs) {
    const auto result = store.CreateOrUpdate(config);
    if (!result.ok()) {
      return false;
    }
  }
  return true;
}

void BM_PushNotificationStore_CreateOrUpdate(benchmark::State& state) {
  const auto configs = BuildPushConfigs(kCreateConfigIdPrefix, a2a::bench::kTaskCount);
  for (auto _ : state) {
    state.PauseTiming();
    a2a::server::InMemoryPushNotificationStore store;
    state.ResumeTiming();

    for (const auto& config : configs) {
      auto result = store.CreateOrUpdate(config);
      benchmark::DoNotOptimize(result);
    }
  }
  state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(configs.size()));
}
BENCHMARK(BM_PushNotificationStore_CreateOrUpdate);

void BM_PushNotificationStore_List_1000Configs(benchmark::State& state) {
  state.PauseTiming();
  const auto configs = BuildPushConfigs(kSeedConfigIdPrefix, a2a::bench::kTaskCount);
  a2a::server::InMemoryPushNotificationStore store;
  if (!SeedPushStore(store, configs)) {
    state.SkipWithError(kPushSeedError.data());
    state.ResumeTiming();
    return;
  }
  state.ResumeTiming();

  for (auto _ : state) {
    auto result = store.List(a2a::bench::kTaskId);
    benchmark::DoNotOptimize(result);
  }
  state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(configs.size()));
}
BENCHMARK(BM_PushNotificationStore_List_1000Configs);

void BM_PushNotificationService_Notify_1000Configs(benchmark::State& state) {
  state.PauseTiming();
  a2a::server::InMemoryTaskStore task_store;
  a2a::server::InMemoryPushNotificationStore push_store;
  BenchmarkDeliveryClient delivery;
  a2a::server::PushNotificationService service(&task_store, &push_store, &delivery);
  const auto task = a2a::bench::BuildTask();
  const auto created_task = task_store.CreateOrUpdate(task);
  if (!created_task.ok()) {
    state.SkipWithError(kTaskSeedError.data());
    state.ResumeTiming();
    return;
  }
  const auto configs = BuildPushConfigs(kSeedConfigIdPrefix, a2a::bench::kTaskCount);
  if (!SeedPushStore(push_store, configs)) {
    state.SkipWithError(kPushSeedError.data());
    state.ResumeTiming();
    return;
  }
  state.ResumeTiming();

  for (auto _ : state) {
    auto result = service.NotifyTaskUpdated(task);
    benchmark::DoNotOptimize(result);
  }
  state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(configs.size()));
}
BENCHMARK(BM_PushNotificationService_Notify_1000Configs);

}  // namespace
