// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include <atomic>
#include <string>

#define A2A_PERFORMANCE_DRIVER_DISABLE_MAIN
// White-box performance-driver tests intentionally include the implementation to
// inspect internal scenario seams without exposing them as production SDK APIs.
// NOLINTNEXTLINE(bugprone-suspicious-include)
#include "performance/a2a_performance_driver.cpp"
#undef A2A_PERFORMANCE_DRIVER_DISABLE_MAIN

namespace {

constexpr int kSingleRequest = 1;
constexpr int kSingleConcurrency = 1;
constexpr double kNoDurationLimitSeconds = 0.0;
constexpr int kPartialFailureSuccessCount = 5;
constexpr int kPartialFailureFailedCount = 3;
constexpr int kPartialFailureCallbackCount = 8;
constexpr int kPartialFailureEventCount = 7;
constexpr int kConcurrentRequests = 16;
constexpr int kConcurrentWorkers = 4;
constexpr double kDiagnosticTaskUpsertMs = 1.25;
constexpr int kTwoDeliveryAttempts = 2;
constexpr int kOneDeliveryAttempt = 1;
constexpr std::string_view kAggregationScenario = "aggregation-test";
constexpr std::string_view kPayloadTaskId = "payload-task";
constexpr std::string_view kFirstDeliveryTaskId = "first-delivery-task";
constexpr std::string_view kSecondDeliveryTaskId = "second-delivery-task";

int AtomicValue(const std::atomic<int>& value) { return value.load(std::memory_order_relaxed); }

ScenarioResult RunSingleOutcome(OperationOutcome outcome) {
  return RunMeasuredScenario(std::string(kAggregationScenario), kSingleRequest, kSingleConcurrency,
                             kNoDurationLimitSeconds, [&outcome](int worker_index, int index) {
                               (void)worker_index;
                               (void)index;
                               return outcome;
                             });
}

a2a::server::PushDeliveryRequest MakeDeliveryRequest(std::string_view task_id) {
  return {.config = {}, .payload = BuildRepresentativePushPayload(task_id)};
}

void ExpectConcurrentOperationCounts(const ScenarioResult& result) {
  EXPECT_EQ(kConcurrentRequests, result.success);
  EXPECT_EQ(0, result.errors);
  EXPECT_EQ(kPushConfigFanout, result.fanout_per_operation);
}

void ExpectConcurrentDeliveryCounts(const ScenarioResult& result) {
  constexpr int kExpectedDeliveries = kConcurrentRequests * kPushConfigFanout;
  EXPECT_EQ(kExpectedDeliveries, result.callback_count);
  EXPECT_EQ(kExpectedDeliveries, result.successful_deliveries);
  EXPECT_EQ(0, result.failed_deliveries);
  EXPECT_EQ(kExpectedDeliveries, result.total_fanout_count);
}

}  // namespace

TEST(PerformanceOutcomeAggregationTest, FailedOutcomePreservesPartialDeliveryCounters) {
  const ScenarioResult result = RunSingleOutcome({.ok = false,
                                                  .event_count = kPartialFailureEventCount,
                                                  .successful_deliveries = kPartialFailureSuccessCount,
                                                  .failed_deliveries = kPartialFailureFailedCount,
                                                  .callback_count = kPartialFailureCallbackCount,
                                                  .fanout_per_operation = kPushConfigFanout,
                                                  .total_fanout_count = kPartialFailureCallbackCount});

  EXPECT_EQ(0, result.success);
  EXPECT_EQ(1, result.errors);
  EXPECT_TRUE(result.latencies.empty());
  EXPECT_EQ(kPartialFailureEventCount, result.event_count);
  EXPECT_EQ(kPartialFailureSuccessCount, result.successful_deliveries);
  EXPECT_EQ(kPartialFailureFailedCount, result.failed_deliveries);
  EXPECT_EQ(kPartialFailureCallbackCount, result.callback_count);
  EXPECT_EQ(kPushConfigFanout, result.fanout_per_operation);
  EXPECT_EQ(kPartialFailureCallbackCount, result.total_fanout_count);
}

TEST(PerformanceScenarioIsolationTest, CallbackFanoutUsesOnlyPreloadedCallbacks) {
  ScenarioInstrumentation instrumentation;
  ScenarioHarness harness(kInMemoryStore, &instrumentation);
  ASSERT_TRUE(harness.ok());
  instrumentation.task_creates.store(0, std::memory_order_relaxed);
  instrumentation.config_creates.store(0, std::memory_order_relaxed);
  instrumentation.config_lists.store(0, std::memory_order_relaxed);
  instrumentation.payload_builds.store(0, std::memory_order_relaxed);

  const OperationOutcome outcome = harness.Execute(kScenarioPushDeliveryCallbackFanout, 0, 0);

  EXPECT_TRUE(outcome.ok);
  EXPECT_EQ(0, AtomicValue(instrumentation.task_creates));
  EXPECT_EQ(0, AtomicValue(instrumentation.config_creates));
  EXPECT_EQ(0, AtomicValue(instrumentation.config_lists));
  EXPECT_EQ(0, AtomicValue(instrumentation.payload_builds));
  EXPECT_EQ(kPushConfigFanout, outcome.callback_count);
  EXPECT_EQ(kPushConfigFanout, outcome.successful_deliveries);
  EXPECT_EQ(0, outcome.failed_deliveries);
  EXPECT_EQ(kPushConfigFanout, outcome.fanout_per_operation);
  EXPECT_EQ(kPushConfigFanout, outcome.total_fanout_count);
}

TEST(PerformanceScenarioIsolationTest, ListManyConfigsUsesOnlyOneConfigList) {
  ScenarioInstrumentation instrumentation;
  ScenarioHarness harness(kInMemoryStore, &instrumentation);
  ASSERT_TRUE(harness.ok());
  instrumentation.task_creates.store(0, std::memory_order_relaxed);
  instrumentation.config_creates.store(0, std::memory_order_relaxed);
  instrumentation.config_lists.store(0, std::memory_order_relaxed);
  instrumentation.payload_builds.store(0, std::memory_order_relaxed);

  const OperationOutcome outcome = harness.Execute(kScenarioPushConfigListManyConfigs, 0, 0);

  EXPECT_TRUE(outcome.ok);
  EXPECT_EQ(0, AtomicValue(instrumentation.task_creates));
  EXPECT_EQ(0, AtomicValue(instrumentation.config_creates));
  EXPECT_EQ(1, AtomicValue(instrumentation.config_lists));
  EXPECT_EQ(0, AtomicValue(instrumentation.payload_builds));
  EXPECT_EQ(kPushConfigFanout, outcome.event_count);
  EXPECT_EQ(kPushConfigFanout, outcome.fanout_per_operation);
  EXPECT_EQ(kPushConfigFanout, outcome.total_fanout_count);
}

TEST(PerformanceScenarioIsolationTest, CreateManyConfigsUsesPreseededTask) {
  ScenarioInstrumentation instrumentation;
  ScenarioHarness harness(kInMemoryStore, &instrumentation);
  ASSERT_TRUE(harness.ok());
  instrumentation.task_creates.store(0, std::memory_order_relaxed);
  instrumentation.config_creates.store(0, std::memory_order_relaxed);
  instrumentation.config_lists.store(0, std::memory_order_relaxed);
  instrumentation.payload_builds.store(0, std::memory_order_relaxed);

  const OperationOutcome outcome = harness.Execute(kScenarioPushConfigCreateMany, 0, 0);

  EXPECT_TRUE(outcome.ok);
  EXPECT_EQ(0, AtomicValue(instrumentation.task_creates));
  EXPECT_EQ(kPushConfigFanout, AtomicValue(instrumentation.config_creates));
  EXPECT_EQ(0, AtomicValue(instrumentation.config_lists));
  EXPECT_EQ(0, AtomicValue(instrumentation.payload_builds));
  EXPECT_EQ(kPushConfigFanout, outcome.event_count);
  EXPECT_EQ(kPushConfigFanout, outcome.fanout_per_operation);
  EXPECT_EQ(kPushConfigFanout, outcome.total_fanout_count);
}

TEST(PerformanceScenarioIsolationTest, EndToEndUsesObservedPartialFailureDeliveryCounts) {
  ScenarioHarness harness(kInMemoryStore);
  ASSERT_TRUE(harness.ok());
  harness.set_failing_delivery_index(kPartialFailureSuccessCount + 1);

  const OperationOutcome outcome = harness.Execute(kScenarioPushNotifyEndToEndManyConfigs, 0, 0);

  EXPECT_FALSE(outcome.ok);
  EXPECT_EQ(kPartialFailureSuccessCount, outcome.successful_deliveries);
  EXPECT_EQ(1, outcome.failed_deliveries);
  EXPECT_EQ(kPartialFailureSuccessCount + 1, outcome.callback_count);
  EXPECT_EQ(kPushConfigFanout, outcome.fanout_per_operation);
  EXPECT_EQ(kPartialFailureSuccessCount + 1, outcome.total_fanout_count);
}

TEST(RecordingPushDeliveryTest, MaintainsAndTakesIndependentTaskStatistics) {
  RecordingPushDelivery delivery;
  ASSERT_TRUE(delivery.Deliver(MakeDeliveryRequest(kFirstDeliveryTaskId)).ok());
  ASSERT_TRUE(delivery.Deliver(MakeDeliveryRequest(kSecondDeliveryTaskId)).ok());
  ASSERT_TRUE(delivery.Deliver(MakeDeliveryRequest(kFirstDeliveryTaskId)).ok());

  const DeliveryStats first_stats = delivery.TakeStats(kFirstDeliveryTaskId);
  EXPECT_EQ(kTwoDeliveryAttempts, first_stats.attempted);
  EXPECT_EQ(kTwoDeliveryAttempts, first_stats.succeeded);
  EXPECT_EQ(0, first_stats.failed);
  EXPECT_EQ(0, delivery.TakeStats(kFirstDeliveryTaskId).attempted);

  const DeliveryStats second_stats = delivery.TakeStats(kSecondDeliveryTaskId);
  EXPECT_EQ(kOneDeliveryAttempt, second_stats.attempted);
  EXPECT_EQ(kOneDeliveryAttempt, second_stats.succeeded);
  EXPECT_EQ(0, second_stats.failed);
}

TEST(RecordingPushDeliveryTest, FailureIndexAppliesIndependentlyToEachTask) {
  RecordingPushDelivery delivery;
  delivery.set_failing_delivery_index(kTwoDeliveryAttempts);
  ASSERT_TRUE(delivery.Deliver(MakeDeliveryRequest(kFirstDeliveryTaskId)).ok());
  ASSERT_FALSE(delivery.Deliver(MakeDeliveryRequest(kFirstDeliveryTaskId)).ok());
  ASSERT_TRUE(delivery.Deliver(MakeDeliveryRequest(kSecondDeliveryTaskId)).ok());
  ASSERT_FALSE(delivery.Deliver(MakeDeliveryRequest(kSecondDeliveryTaskId)).ok());

  const DeliveryStats first_stats = delivery.TakeStats(kFirstDeliveryTaskId);
  EXPECT_EQ(kTwoDeliveryAttempts, first_stats.attempted);
  EXPECT_EQ(kOneDeliveryAttempt, first_stats.succeeded);
  EXPECT_EQ(kOneDeliveryAttempt, first_stats.failed);

  const DeliveryStats second_stats = delivery.TakeStats(kSecondDeliveryTaskId);
  EXPECT_EQ(kTwoDeliveryAttempts, second_stats.attempted);
  EXPECT_EQ(kOneDeliveryAttempt, second_stats.succeeded);
  EXPECT_EQ(kOneDeliveryAttempt, second_stats.failed);
}

TEST(PerformanceScenarioConcurrencyTest, EndToEndAggregatesExactPerTaskDeliveryCounts) {
  ScenarioHarness harness(kInMemoryStore);
  ASSERT_TRUE(harness.ok());

  const ScenarioResult result =
      RunMeasuredScenario(std::string(kScenarioPushNotifyEndToEndManyConfigs), kConcurrentRequests, kConcurrentWorkers,
                          kNoDurationLimitSeconds, [&harness](int worker_index, int index) {
                            return harness.Execute(kScenarioPushNotifyEndToEndManyConfigs, worker_index, index);
                          });

  ExpectConcurrentOperationCounts(result);
  ExpectConcurrentDeliveryCounts(result);
}

TEST(PerformancePayloadTest, RepresentativePayloadMatchesProductionHelperShape) {
  const lf::a2a::v1::Task task = BuildRepresentativePushPayloadTask(kPayloadTaskId);
  const lf::a2a::v1::StreamResponse benchmark_payload = BuildRepresentativePushPayload(kPayloadTaskId);
  const lf::a2a::v1::StreamResponse production_payload = a2a::server::BuildTaskStatusUpdatePayload(task);

  ASSERT_TRUE(benchmark_payload.has_status_update());
  ASSERT_TRUE(production_payload.has_status_update());
  EXPECT_EQ(production_payload.status_update().task_id(), benchmark_payload.status_update().task_id());
  EXPECT_EQ(production_payload.status_update().context_id(), benchmark_payload.status_update().context_id());
  EXPECT_EQ(production_payload.status_update().status().SerializeAsString(),
            benchmark_payload.status_update().status().SerializeAsString());
}

TEST(PerformanceDiagnosticsTest, SerializesPerPhasePostgresLatency) {
  const ScenarioResult result = RunMeasuredScenario(std::string(kScenarioPushNotifyEndToEndManyConfigs), kSingleRequest,
                                                    kSingleConcurrency, kNoDurationLimitSeconds, [](int, int) {
                                                      OperationOutcome outcome{.ok = true};
                                                      outcome.postgres_phase_latency_ms[1] = kDiagnosticTaskUpsertMs;
                                                      return outcome;
                                                    });
  google::protobuf::Struct object;
  AddPostgresDiagnosticFields(&object, result);

  const auto& phases = object.fields().at("postgres_phase_latency_ms").struct_value().fields();
  const auto& task_upsert = phases.at("task_upsert").struct_value().fields();
  EXPECT_DOUBLE_EQ(task_upsert.at("p95").number_value(), kDiagnosticTaskUpsertMs);
  EXPECT_DOUBLE_EQ(task_upsert.at("p99").number_value(), kDiagnosticTaskUpsertMs);
  EXPECT_DOUBLE_EQ(task_upsert.at("max").number_value(), kDiagnosticTaskUpsertMs);
}
