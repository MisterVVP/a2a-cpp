// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>

#include "a2a/core/result.h"
#include "a2a/v1/a2a.pb.h"

namespace a2a::server {

struct RequestContext;

class TaskIdGenerator {
 public:
  virtual ~TaskIdGenerator() = default;
  [[nodiscard]] virtual core::Result<std::string> GenerateTaskId(const lf::a2a::v1::SendMessageRequest& request,
                                                                 const RequestContext& context) = 0;
};

class UuidV7TaskIdGenerator final : public TaskIdGenerator {
 public:
  [[nodiscard]] core::Result<std::string> GenerateTaskId(const lf::a2a::v1::SendMessageRequest& request,
                                                         const RequestContext& context) override;

 private:
  static constexpr std::size_t kUuidByteCount = 16;
  static constexpr std::uint8_t kByteMask = 0xFFU;
  static constexpr std::uint8_t kVersionNibbleMask = 0x0FU;
  static constexpr std::uint8_t kVersion7Nibble = 0x70U;
  static constexpr std::uint8_t kVariantKeepMask = 0x3FU;
  static constexpr std::uint8_t kVariantRfcBits = 0x80U;
  static constexpr std::size_t kUuidStringSize = 36;
  static constexpr std::size_t kUuidWithNullSize = kUuidStringSize + 1;
  static constexpr std::uint32_t kTimestampShift40 = 40U;
  static constexpr std::uint32_t kTimestampShift32 = 32U;
  static constexpr std::uint32_t kTimestampShift24 = 24U;
  static constexpr std::uint32_t kTimestampShift16 = 16U;
  static constexpr std::uint32_t kTimestampShift8 = 8U;
  static constexpr std::size_t kTimestampByteIndex0 = 0;
  static constexpr std::size_t kTimestampByteIndex1 = 1;
  static constexpr std::size_t kTimestampByteIndex2 = 2;
  static constexpr std::size_t kTimestampByteIndex3 = 3;
  static constexpr std::size_t kTimestampByteIndex4 = 4;
  static constexpr std::size_t kTimestampByteIndex5 = 5;
  static constexpr std::size_t kVersionByteIndex = 6;
  static constexpr std::size_t kSequenceByteIndex = 7;
  static constexpr std::size_t kVariantByteIndex = 8;
  static constexpr std::string_view kPrefix = "task-";
  static constexpr std::uint64_t kTimestampMask = 0x0000FFFFFFFFFFFFULL;
  static constexpr std::uint64_t kSequenceMask = 0x0000000000000FFFULL;

  std::mutex mutex_;
  std::uint64_t last_timestamp_ms_ = 0;
  std::uint64_t sequence_ = 0;
};

class SequentialTaskIdGenerator final : public TaskIdGenerator {
 public:
  [[nodiscard]] core::Result<std::string> GenerateTaskId(const lf::a2a::v1::SendMessageRequest& request,
                                                         const RequestContext& context) override;

 private:
  std::mutex mutex_;
  std::uint64_t next_ = 1;
};

}  // namespace a2a::server
