// SPDX-License-Identifier: Apache-2.0

#include "a2a_performance_driver.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>

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

void PopulateCommonResultFields(google::protobuf::Struct* object, std::string_view scenario,
                                std::string_view transport, std::string_view store_backend, int concurrency,
                                const ScenarioResult& result) {
  SetStringField(object, "scenario", scenario);
  SetStringField(object, "transport", transport);
  SetStringField(object, "store_backend", store_backend);
  SetIntegerField(object, "concurrency", concurrency);
  SetIntegerField(object, "operations", result.operations);
  SetIntegerField(object, "success", result.success);
  SetIntegerField(object, "errors", result.errors);
  SetNumberField(object, "throughput_ops_per_sec", result.throughput);
}

void AddLatencyField(google::protobuf::Struct* object, const ScenarioResult& result) {
  google::protobuf::Struct latency;
  SetNumberField(&latency, "p50", Percentile(result.latencies, kP50));
  SetNumberField(&latency, "p90", Percentile(result.latencies, kP90));
  SetNumberField(&latency, "p95", Percentile(result.latencies, kP95));
  SetNumberField(&latency, "p99", Percentile(result.latencies, kP99));
  SetNumberField(&latency, "max", result.latencies.empty() ? 0.0 : result.latencies.back());
  (*object->mutable_fields())["latency_ms"].mutable_struct_value()->Swap(&latency);
}

}  // namespace a2a::tests::performance
