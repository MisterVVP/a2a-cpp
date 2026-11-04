// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

// Temporary investigation overlay only. No production target includes this file.
#ifdef _WIN32
// Winsock must precede windows.h.
// clang-format off
#include <winsock2.h>
#include <windows.h>
// clang-format on
#endif

#include <curl/curl.h>

#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iomanip>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace a2a::http::stage_trace {

using Clock = std::chrono::steady_clock;
inline constexpr std::size_t kCapacity = 32768U;
inline constexpr std::size_t kOutputPathCapacity = 32768U;
inline constexpr int kPrecision = 6;
inline constexpr std::string_view kRequestMarker = "?evidence_id=";
inline constexpr const char* kOutputVariable = "A2A_STAGE_TRACE_FILE";
inline constexpr const char* kWriteFailure = "cannot write reactor stage trace";
inline constexpr const char* kOverflowFailure = "reactor stage trace overflow";

enum class Kind : std::uint8_t {
  kOrigin,
  kEnqueue,
  kCommandsBegin,
  kCommandsEnd,
  kAddBegin,
  kAddEnd,
  kActionBegin,
  kActionEnd,
  kWaitBegin,
  kWaitEnd,
  kWake,
  kDrainBegin,
  kDrainEnd,
  kInterest,
  kStale,
  kRequest,
  kCurlHeader,
  kCurlBody,
  kQueueMetadata,
  kDispatchMetadata,
  kQueueBody,
  kDispatchBody,
};

inline constexpr std::array kNames{
    "origin",         "enqueue",           "commands_begin", "commands_end", "add_begin",   "add_end",
    "action_begin",   "action_end",        "wait_begin",     "wait_end",     "wake",        "drain_begin",
    "drain_end",      "interest",          "stale",          "request",      "curl_header", "curl_body",
    "queue_metadata", "dispatch_metadata", "queue_body",     "dispatch_body"};

struct Record final {
  Clock::time_point when;
  Kind kind = Kind::kOrigin;
  std::uintptr_t key = 0U;
  std::int64_t argument = 0;
  std::uint64_t cycles = 0U;
};

struct Buffer final {
  std::unique_ptr<Record[]> records = std::make_unique<Record[]>(kCapacity);
  std::atomic_size_t published{0U};
  std::size_t used = 0U;
  std::uint64_t thread_id = 0U;
  std::atomic_bool overflow{false};
};

inline std::mutex registry_mutex;
inline std::vector<std::unique_ptr<Buffer>> registry;

inline Buffer& ThreadBuffer() {
  thread_local Buffer* buffer = [] {
    auto owned = std::make_unique<Buffer>();
#ifdef _WIN32
    owned->thread_id = GetCurrentThreadId();
#else
    owned->thread_id = std::hash<std::thread::id>{}(std::this_thread::get_id());
#endif
    auto* result = owned.get();
    std::lock_guard lock(registry_mutex);
    registry.push_back(std::move(owned));
    return result;
  }();
  return *buffer;
}

inline void EmitAt(Clock::time_point when, Kind kind, std::uintptr_t key, std::int64_t argument) {
  auto& buffer = ThreadBuffer();
  if (buffer.used == kCapacity) {
    buffer.overflow.store(true, std::memory_order_relaxed);
    return;
  }
  std::uint64_t cycles = 0U;
#ifdef _WIN32
  ULONG64 measured = 0U;
  if (QueryThreadCycleTime(GetCurrentThread(), &measured) != 0) {
    cycles = measured;
  }
#endif
  buffer.records[buffer.used++] =
      Record{.when = when, .kind = kind, .key = key, .argument = argument, .cycles = cycles};
  buffer.published.store(buffer.used, std::memory_order_release);
}

inline void Emit(Kind kind, const void* key = nullptr, std::int64_t argument = 0) {
  // Opaque correlation identity only; this integer is never dereferenced.
  EmitAt(Clock::now(), kind, reinterpret_cast<std::uintptr_t>(key), argument);
}

class Span final {
 public:
  Span(Kind begin, Kind end, const void* key, std::int64_t argument = 0) : end_(end), key_(key), argument_(argument) {
    Emit(begin, key, argument);
  }
  ~Span() { Emit(end_, key_, argument_); }
  Span(const Span&) = delete;
  Span& operator=(const Span&) = delete;

 private:
  Kind end_;
  const void* key_;
  std::int64_t argument_;
};

inline int CurlDebug(CURL* handle, curl_infotype type, char* data, std::size_t size, void* unused) {
  (void)unused;
  if (type == CURLINFO_HEADER_OUT) {
    const std::string_view header(data, size);
    const auto marker = header.find(kRequestMarker);
    std::int64_t request_id = -1;
    if (marker != std::string_view::npos) {
      (void)std::from_chars(header.data() + marker + kRequestMarker.size(), header.data() + header.size(), request_id);
    }
    Emit(Kind::kRequest, handle, request_id);
  } else if (type == CURLINFO_HEADER_IN) {
    Emit(Kind::kCurlHeader, handle, static_cast<std::int64_t>(size));
  } else if (type == CURLINFO_DATA_IN) {
    Emit(Kind::kCurlBody, handle, static_cast<std::int64_t>(size));
  }
  return 0;
}

inline void Dump() {
#ifdef _WIN32
  std::array<char, kOutputPathCapacity> output_path{};
  const DWORD length =
      GetEnvironmentVariableA(kOutputVariable, output_path.data(), static_cast<DWORD>(output_path.size()));
  if (length == 0U || length >= output_path.size()) {
    throw std::runtime_error(kWriteFailure);
  }
  const char* path = output_path.data();
#else
  const char* path = std::getenv(kOutputVariable);
  if (path == nullptr) {
    throw std::runtime_error(kWriteFailure);
  }
#endif
  std::ofstream output(path);
  output << std::fixed << std::setprecision(kPrecision) << "thread,ms,kind,key,argument,cycles\n";
  std::lock_guard lock(registry_mutex);
  for (const auto& buffer : registry) {
    if (buffer->overflow.load(std::memory_order_relaxed)) {
      throw std::runtime_error(kOverflowFailure);
    }
    const auto count = buffer->published.load(std::memory_order_acquire);
    for (std::size_t index = 0; index < count; ++index) {
      const auto& record = buffer->records[index];
      output << buffer->thread_id << ','
             << std::chrono::duration<double, std::milli>(record.when.time_since_epoch()).count() << ','
             << kNames.at(static_cast<std::size_t>(record.kind)) << ',' << record.key << ',' << record.argument << ','
             << record.cycles << '\n';
    }
  }
  output.close();
  if (!output) {
    throw std::runtime_error(kWriteFailure);
  }
}

}  // namespace a2a::http::stage_trace
