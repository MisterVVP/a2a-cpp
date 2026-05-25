// SPDX-License-Identifier: Apache-2.0

#include "a2a/server/task_id_generator.h"

#include <array>
#include <chrono>
#include <cstdio>
#include <random>

#include "a2a/core/error.h"
#include "a2a/server/server.h"

namespace a2a::server {

core::Result<std::string> UuidV7TaskIdGenerator::GenerateTaskId(const lf::a2a::v1::SendMessageRequest& request,
                                                                 const RequestContext& context) {
  (void)request;
  (void)context;
  std::array<std::uint8_t, kUuidByteCount> bytes{};
  const auto now = std::chrono::time_point_cast<std::chrono::milliseconds>(std::chrono::system_clock::now());
  const auto current_ms = static_cast<std::uint64_t>(now.time_since_epoch().count());
  std::uint64_t effective_ms = current_ms;
  std::uint64_t sequence_value = 0;
  {
    std::scoped_lock<std::mutex> lock(mutex_);
    if (effective_ms < last_timestamp_ms_) {
      effective_ms = last_timestamp_ms_;
    }
    if (effective_ms > last_timestamp_ms_) {
      last_timestamp_ms_ = effective_ms;
      std::random_device rd;
      sequence_ = static_cast<std::uint64_t>(rd()) & kSequenceMask;
    } else {
      sequence_ = (sequence_ + 1U) & kSequenceMask;
      if (sequence_ == 0U) {
        return core::Error::Internal("UUIDv7 sequence overflow within one millisecond");
      }
    }
    sequence_value = sequence_;
  }
  std::random_device rd;
  for (auto& byte : bytes) {
    byte = static_cast<std::uint8_t>(rd() & kByteMask);
  }
  const std::uint64_t ts = effective_ms & kTimestampMask;
  bytes[0] = static_cast<std::uint8_t>((ts >> kTimestampShift40) & kByteMask);
  bytes[1] = static_cast<std::uint8_t>((ts >> kTimestampShift32) & kByteMask);
  bytes[2] = static_cast<std::uint8_t>((ts >> kTimestampShift24) & kByteMask);
  bytes[3] = static_cast<std::uint8_t>((ts >> kTimestampShift16) & kByteMask);
  bytes[4] = static_cast<std::uint8_t>((ts >> kTimestampShift8) & kByteMask);
  bytes[5] = static_cast<std::uint8_t>(ts & kByteMask);
  bytes[6] = static_cast<std::uint8_t>(kVersion7Nibble | ((sequence_value >> kTimestampShift8) & kVersionNibbleMask));
  bytes[7] = static_cast<std::uint8_t>(sequence_value & kByteMask);
  bytes[8] = static_cast<std::uint8_t>((bytes[8] & kVariantKeepMask) | kVariantRfcBits);
  std::array<char, kUuidWithNullSize> uuid{};
  const int written =
      std::snprintf(uuid.data(), uuid.size(), "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                    bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7], bytes[8], bytes[9],
                    bytes[10], bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]);
  if (written != static_cast<int>(kUuidStringSize)) {
    return core::Error::Internal("Failed to format UUIDv7");
  }
  return std::string(kPrefix) + std::string(uuid.data(), kUuidStringSize);
}

core::Result<std::string> SequentialTaskIdGenerator::GenerateTaskId(const lf::a2a::v1::SendMessageRequest& request,
                                                                     const RequestContext& context) {
  (void)request;
  (void)context;
  std::scoped_lock<std::mutex> lock(mutex_);
  return "task-test-" + std::to_string(next_++);
}

}  // namespace a2a::server
