// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>

#include "a2a_performance_driver.h"

namespace a2a::tests::performance {

double Percentile(const std::vector<double>& sorted_values, double percentile) {
  if (sorted_values.empty()) {
    return 0.0;
  }
  const auto last = static_cast<double>(sorted_values.size() - 1U);
  const auto index = static_cast<std::size_t>(std::llround((percentile / 100.0) * last));
  return sorted_values[std::min(index, sorted_values.size() - 1U)];
}

lf::a2a::v1::SendMessageRequest MakeSendRequest(std::string_view message_id, std::string_view task_id) {
  lf::a2a::v1::SendMessageRequest request;
  request.mutable_message()->set_message_id(std::string(message_id));
  request.mutable_message()->set_role(lf::a2a::v1::ROLE_USER);
  if (!task_id.empty()) {
    request.mutable_message()->set_task_id(std::string(task_id));
  }
  request.mutable_message()->add_parts()->set_text(std::string(kMessageText));
  return request;
}

lf::a2a::v1::TaskPushNotificationConfig MakePushConfig(std::string_view task_id, std::string_view config_id) {
  lf::a2a::v1::TaskPushNotificationConfig config;
  config.set_task_id(std::string(task_id));
  config.set_id(std::string(config_id));
  config.set_url(std::string(kPushCallbackUrl));
  return config;
}

std::string BuildId(std::string_view prefix, int index) {
  std::string value;
  value.reserve(prefix.size() + kIdReserveSlack);
  value.append(prefix);
  value.push_back('-');
  value.append(std::to_string(index));
  return value;
}

std::vector<std::string> SplitCsv(std::string_view value) {
  std::vector<std::string> items;
  std::size_t start = 0;
  while (start <= value.size()) {
    const std::size_t comma = value.find(',', start);
    const std::size_t end = comma == std::string_view::npos ? value.size() : comma;
    if (end > start) {
      items.emplace_back(value.substr(start, end - start));
    }
    if (comma == std::string_view::npos) {
      break;
    }
    start = comma + 1U;
  }
  return items;
}

bool HasArgumentValue(int index, int argc) { return index + 1 < argc; }

void SetStringField(google::protobuf::Struct* object, std::string_view key, std::string_view value) {
  (*object->mutable_fields())[std::string(key)].set_string_value(std::string(value));
}

void SetNumberField(google::protobuf::Struct* object, std::string_view key, double value) {
  (*object->mutable_fields())[std::string(key)].set_number_value(value);
}

void SetIntegerField(google::protobuf::Struct* object, std::string_view key, int value) {
  SetNumberField(object, key, static_cast<double>(value));
}

void PopulateCommonResultFields(google::protobuf::Struct* object, std::string_view scenario, std::string_view transport,
                                std::string_view store_backend, int concurrency, const ScenarioResult& result) {
  SetStringField(object, "scenario", scenario);
  SetStringField(object, "transport", transport);
  SetStringField(object, "store_backend", store_backend);
  SetIntegerField(object, "concurrency", concurrency);
  SetIntegerField(object, "operations", result.operations);
  SetIntegerField(object, "success", result.success);
  SetIntegerField(object, "errors", result.errors);
  SetNumberField(object, "throughput_ops_per_sec", result.throughput);
  SetIntegerField(object, "successful_deliveries", result.successful_deliveries);
  SetIntegerField(object, "failed_deliveries", result.failed_deliveries);
  SetIntegerField(object, "event_count", result.event_count);
  SetIntegerField(object, "callback_count", result.callback_count);
  SetIntegerField(object, "fanout_per_operation", result.fanout_per_operation);
  SetIntegerField(object, "total_fanout_count", result.total_fanout_count);
  SetIntegerField(object, "fanout_count", result.total_fanout_count);
}

void AddLatencyField(google::protobuf::Struct* object, const ScenarioResult& result) {
  google::protobuf::Struct latency;
  SetNumberField(&latency, "p50", Percentile(result.latencies, kP50));
  SetNumberField(&latency, "p90", Percentile(result.latencies, kP90));
  SetNumberField(&latency, "p95", Percentile(result.latencies, kP95));
  SetNumberField(&latency, "p99", Percentile(result.latencies, kP99));
  SetNumberField(&latency, "max", result.latencies.empty() ? 0.0 : result.latencies.back());
  (*object->mutable_fields())["latency_ms"].mutable_struct_value()->Swap(&latency);
  if (!result.first_event_latencies.empty()) {
    google::protobuf::Struct first_event_latency;
    SetNumberField(&first_event_latency, "p50", Percentile(result.first_event_latencies, kP50));
    SetNumberField(&first_event_latency, "p90", Percentile(result.first_event_latencies, kP90));
    SetNumberField(&first_event_latency, "p95", Percentile(result.first_event_latencies, kP95));
    SetNumberField(&first_event_latency, "p99", Percentile(result.first_event_latencies, kP99));
    SetNumberField(&first_event_latency, "max",
                   result.first_event_latencies.empty() ? 0.0 : result.first_event_latencies.back());
    (*object->mutable_fields())["first_event_latency_ms"].mutable_struct_value()->Swap(&first_event_latency);
  }
  if (!result.completion_latencies.empty()) {
    google::protobuf::Struct completion_latency;
    SetNumberField(&completion_latency, "p50", Percentile(result.completion_latencies, kP50));
    SetNumberField(&completion_latency, "p90", Percentile(result.completion_latencies, kP90));
    SetNumberField(&completion_latency, "p95", Percentile(result.completion_latencies, kP95));
    SetNumberField(&completion_latency, "p99", Percentile(result.completion_latencies, kP99));
    SetNumberField(&completion_latency, "max",
                   result.completion_latencies.empty() ? 0.0 : result.completion_latencies.back());
    (*object->mutable_fields())["stream_completion_latency_ms"].mutable_struct_value()->Swap(&completion_latency);
  }
}

void AddPostgresDiagnosticFields(google::protobuf::Struct* object, const ScenarioResult& result) {
  google::protobuf::Struct phases;
  google::protobuf::Struct call_counts;
  google::protobuf::Struct calls_per_operation;
  for (std::size_t phase = 0; phase < result.postgres_phase_latencies.size(); ++phase) {
    const auto& values = result.postgres_phase_latencies[phase];
    google::protobuf::Struct latency;
    SetNumberField(&latency, "p50", Percentile(values, kP50));
    SetNumberField(&latency, "p95", Percentile(values, kP95));
    SetNumberField(&latency, "p99", Percentile(values, kP99));
    SetNumberField(&latency, "max", values.empty() ? 0.0 : values.back());
    (*phases.mutable_fields())[std::string(kPostgresDiagnosticPhaseNames[phase])].mutable_struct_value()->Swap(
        &latency);
    const auto phase_name = std::string(kPostgresDiagnosticPhaseNames[phase]);
    SetNumberField(&call_counts, phase_name, static_cast<double>(result.postgres_phase_call_count[phase]));
    const double per_operation = result.operations == 0 ? 0.0
                                                        : static_cast<double>(result.postgres_phase_call_count[phase]) /
                                                              static_cast<double>(result.operations);
    SetNumberField(&calls_per_operation, phase_name, per_operation);
  }
  (*object->mutable_fields())["postgres_phase_latency_ms"].mutable_struct_value()->Swap(&phases);
  (*object->mutable_fields())["postgres_phase_call_count"].mutable_struct_value()->Swap(&call_counts);
  (*object->mutable_fields())["postgres_phase_calls_per_operation"].mutable_struct_value()->Swap(&calls_per_operation);
}

}  // namespace a2a::tests::performance
