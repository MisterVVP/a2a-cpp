// SPDX-License-Identifier: Apache-2.0

#include "a2a/server/task_id_generator.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <utility>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
// clang-format off
#include <windows.h>
#include <bcrypt.h>
// clang-format on
#pragma comment(lib, "bcrypt.lib")
#elif defined(__linux__)
#include <sys/random.h>
#include <sys/types.h>
#elif defined(__APPLE__)
#include <stdlib.h>
#endif

#include "a2a/core/error.h"
#include "a2a/server/request_context.h"

namespace a2a::server {
namespace {

[[nodiscard]] core::Result<void> FillSecureRandomBytes(void* data, std::size_t size) {
  if (size == 0U) {
    return {};
  }
  if (data == nullptr) {
    return core::Error::Internal("Secure random output buffer is required");
  }

#if defined(_WIN32)
  const auto status =
      BCryptGenRandom(nullptr, static_cast<PUCHAR>(data), static_cast<ULONG>(size), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
  if (status < 0) {
    return core::Error::Internal("BCryptGenRandom failed while generating UUIDv7 randomness");
  }
  return {};
#elif defined(__linux__)
  auto* out = static_cast<std::uint8_t*>(data);
  std::size_t offset = 0;
  while (offset < size) {
    const ssize_t read = getrandom(out + offset, size - offset, 0);
    if (read > 0) {
      offset += static_cast<std::size_t>(read);
      continue;
    }
    if (read == -1 && errno == EINTR) {
      continue;
    }
    return core::Error::Internal(std::string("getrandom failed while generating UUIDv7 randomness: ") +
                                 std::strerror(errno));
  }
  return {};
#elif defined(__APPLE__)
  arc4random_buf(data, size);
  return {};
#else
  constexpr std::uint8_t kRandomByteMask = 0xFFU;
  constexpr std::uint32_t kRandomByteShift = 8U;

  std::random_device rd;
  auto* out = static_cast<std::uint8_t*>(data);
  for (std::size_t offset = 0; offset < size;) {
    auto value = rd();
    for (std::size_t byte = 0; byte < sizeof(value) && offset < size; ++byte) {
      out[offset] = static_cast<std::uint8_t>(value & kRandomByteMask);
      ++offset;
      value >>= kRandomByteShift;
    }
  }
  return {};
#endif
}

}  // namespace

core::Result<std::string> UuidV7TaskIdGenerator::GenerateTaskId(const lf::a2a::v1::SendMessageRequest& request,
                                                                const RequestContext& context) {
  (void)request;
  (void)context;
  std::array<std::uint8_t, kUuidByteCount> bytes{};
  const auto random = FillSecureRandomBytes(bytes.data(), bytes.size());
  if (!random.ok()) {
    return random.error();
  }

  const auto now = std::chrono::time_point_cast<std::chrono::milliseconds>(std::chrono::system_clock::now());
  const auto current_ms = static_cast<std::uint64_t>(now.time_since_epoch().count());
  std::uint64_t effective_ms = current_ms;
  std::uint64_t sequence_value = 0;
  {
    std::scoped_lock<std::mutex> lock(mutex_);
    effective_ms = std::max(effective_ms, last_timestamp_ms_);
    if (effective_ms > last_timestamp_ms_) {
      last_timestamp_ms_ = effective_ms;
      sequence_ = ((static_cast<std::uint64_t>(bytes[kVersionByteIndex]) << kTimestampShift8) |
                   static_cast<std::uint64_t>(bytes[kSequenceByteIndex])) &
                  kInitialSequenceMask;
    } else {
      sequence_ = (sequence_ + 1U) & kSequenceMask;
      if (sequence_ == 0U) {
        return core::Error::Internal("UUIDv7 sequence overflow within one millisecond");
      }
    }
    sequence_value = sequence_;
  }
  const std::uint64_t ts = effective_ms & kTimestampMask;
  bytes[kTimestampByteIndex0] = static_cast<std::uint8_t>((ts >> kTimestampShift40) & kByteMask);
  bytes[kTimestampByteIndex1] = static_cast<std::uint8_t>((ts >> kTimestampShift32) & kByteMask);
  bytes[kTimestampByteIndex2] = static_cast<std::uint8_t>((ts >> kTimestampShift24) & kByteMask);
  bytes[kTimestampByteIndex3] = static_cast<std::uint8_t>((ts >> kTimestampShift16) & kByteMask);
  bytes[kTimestampByteIndex4] = static_cast<std::uint8_t>((ts >> kTimestampShift8) & kByteMask);
  bytes[kTimestampByteIndex5] = static_cast<std::uint8_t>(ts & kByteMask);
  bytes[kVersionByteIndex] =
      static_cast<std::uint8_t>(kVersion7Nibble | ((sequence_value >> kTimestampShift8) & kVersionNibbleMask));
  bytes[kSequenceByteIndex] = static_cast<std::uint8_t>(sequence_value & kByteMask);
  bytes[kVariantByteIndex] = static_cast<std::uint8_t>((bytes[kVariantByteIndex] & kVariantKeepMask) | kVariantRfcBits);
  std::array<char, kUuidWithNullSize> uuid{};
  const int written =
      std::snprintf(uuid.data(), uuid.size(), "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                    bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7], bytes[8], bytes[9],
                    bytes[10], bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]);
  if (std::cmp_not_equal(written, kUuidStringSize)) {
    return core::Error::Internal("Failed to format UUIDv7");
  }

  std::string result;
  result.reserve(kPrefix.size() + kUuidStringSize);
  result.append(kPrefix);
  result.append(uuid.data(), kUuidStringSize);
  return result;
}

}  // namespace a2a::server
