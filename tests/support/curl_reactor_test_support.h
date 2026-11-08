// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

#ifdef _WIN32
// Winsock must precede windows.h; Toolhelp requires the Windows types.
// clang-format off
#include <winsock2.h>
#include <windows.h>
#include <tlhelp32.h>
// clang-format on
#else
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <array>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "a2a/http/http_client.h"
#include "http_client_internal.h"

namespace a2a::http::testing {

inline constexpr std::chrono::seconds kReactorTestTimeout{5};
inline constexpr std::chrono::seconds kIdleRequestTimeout{30};
inline constexpr std::string_view kLoopbackUrlPrefix = "http://127.0.0.1:";
inline constexpr std::string_view kStreamTestPath = "/stream";
inline constexpr std::string_view kRequestHeadersEnd = "\r\n\r\n";
inline constexpr std::string_view kContentLengthPrefix = "Content-Length:";
inline constexpr std::string_view kHeaderWhitespace = " \t";
inline constexpr std::string_view kTestSseHeaders =
    "HTTP/1.1 200 OK\r\nA2A-Version: 1.0\r\nContent-Type: text/event-stream\r\nConnection: close\r\n\r\n";
inline constexpr std::string_view kTestFirstChunk = "data: first\n\n";
inline constexpr std::string_view kTestLastChunk = "data: last\n\n";
inline constexpr std::string_view kTestGetMethod = "GET";
inline constexpr std::string_view kSocketFailure = "reactor test socket operation failed";
inline constexpr std::size_t kRequestBufferSize = 4096U;
inline constexpr std::size_t kMaximumRequestHeaders = 65536U;
inline constexpr int kTestListenBacklog = 128;

#ifdef _WIN32
using TestSocketHandle = SOCKET;
inline constexpr auto kInvalidTestSocket = INVALID_SOCKET;
using TestSocketLength = int;
#else
using TestSocketHandle = int;
inline constexpr auto kInvalidTestSocket = -1;
using TestSocketLength = socklen_t;
#endif

class TestSocket final {
 public:
  TestSocket() = default;
  explicit TestSocket(TestSocketHandle value) : value_(value) {}
  ~TestSocket() { Close(); }
  TestSocket(const TestSocket&) = delete;
  TestSocket& operator=(const TestSocket&) = delete;
  TestSocket(TestSocket&& other) noexcept : value_(std::exchange(other.value_, kInvalidTestSocket)) {}
  TestSocket& operator=(TestSocket&& other) noexcept {
    if (this != &other) {
      Close();
      value_ = std::exchange(other.value_, kInvalidTestSocket);
    }
    return *this;
  }
  [[nodiscard]] TestSocketHandle get() const noexcept { return value_; }
  [[nodiscard]] bool valid() const noexcept { return value_ != kInvalidTestSocket; }
  void Close() noexcept {
    if (!valid()) {
      return;
    }
#ifdef _WIN32
    (void)::shutdown(value_, SD_BOTH);
    (void)::closesocket(value_);
#else
    (void)::shutdown(value_, SHUT_RDWR);
    (void)::close(value_);
#endif
    value_ = kInvalidTestSocket;
  }

 private:
  TestSocketHandle value_ = kInvalidTestSocket;
};

inline bool SendTestBytes(TestSocketHandle socket, std::string_view bytes) {
  while (!bytes.empty()) {
#ifdef _WIN32
    const auto sent = ::send(socket, bytes.data(), static_cast<int>(bytes.size()), 0);
#else
    const auto sent = ::send(socket, bytes.data(), bytes.size(), MSG_NOSIGNAL);
#endif
    if (sent <= 0) {
      return false;
    }
    bytes.remove_prefix(static_cast<std::size_t>(sent));
  }
  return true;
}

inline sockaddr_in TestLoopbackAddress() {
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  return address;
}

inline TestSocket MakeTestListener(unsigned short* port) {
  TestSocket listener(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
  auto address = TestLoopbackAddress();
  TestSocketLength length = sizeof(address);
  if (!listener.valid() || ::bind(listener.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
      ::listen(listener.get(), kTestListenBacklog) != 0 ||
      ::getsockname(listener.get(), reinterpret_cast<sockaddr*>(&address), &length) != 0) {
    throw std::runtime_error(std::string(kSocketFailure));
  }
  *port = ntohs(address.sin_port);
  return listener;
}

inline std::pair<TestSocket, TestSocket> MakeTestSocketPair() {
  unsigned short port = 0;
  const auto listener = MakeTestListener(&port);
  auto address = TestLoopbackAddress();
  address.sin_port = htons(port);
  TestSocket writer(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
  if (!writer.valid() || ::connect(writer.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
    throw std::runtime_error(std::string(kSocketFailure));
  }
  TestSocket reader(::accept(listener.get(), nullptr, nullptr));
  if (!reader.valid()) {
    throw std::runtime_error(std::string(kSocketFailure));
  }
  return {std::move(reader), std::move(writer)};
}

inline bool HasCompleteTestRequest(std::string_view request) {
  const auto headers_end = request.find(kRequestHeadersEnd);
  if (headers_end == std::string_view::npos) {
    return false;
  }
  const auto length_header = request.find(kContentLengthPrefix);
  if (length_header == std::string_view::npos || length_header >= headers_end) {
    return true;
  }
  const auto value_start = request.find_first_not_of(kHeaderWhitespace, length_header + kContentLengthPrefix.size());
  if (value_start == std::string_view::npos || value_start >= headers_end) {
    return false;
  }
  std::size_t body_size = 0;
  const auto parsed = std::from_chars(request.data() + value_start, request.data() + headers_end, body_size);
  return parsed.ec == std::errc{} && body_size <= kMaximumRequestHeaders &&
         request.size() >= headers_end + kRequestHeadersEnd.size() + body_size;
}

inline bool ReadTestRequest(TestSocketHandle socket) {
#ifdef _WIN32
  constexpr DWORD kReceiveTimeoutMs = 5000;
  (void)setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&kReceiveTimeoutMs),
                   sizeof(kReceiveTimeoutMs));
#else
  constexpr timeval kReceiveTimeout{.tv_sec = 5, .tv_usec = 0};
  (void)setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &kReceiveTimeout, sizeof(kReceiveTimeout));
#endif
  std::array<char, kRequestBufferSize> buffer{};
  std::string request;
  while (request.size() < kMaximumRequestHeaders) {
    const auto received = ::recv(socket, buffer.data(), static_cast<int>(buffer.size()), 0);
    if (received <= 0) {
      return false;
    }
    request.append(buffer.data(), static_cast<std::size_t>(received));
    // Consume POST bodies before closing later, so fragmented JSON-RPC writes
    // cannot turn a successful finite response into a TCP reset.
    if (HasCompleteTestRequest(request)) {
      return true;
    }
  }
  return false;
}

// One server worker for every stream count. Responses are explicitly released
// by the test; no per-connection thread or timing-based response scheduling.
class CurlLoopbackServer final {
 public:
  explicit CurlLoopbackServer(std::size_t expected, std::string_view initial = kTestFirstChunk,
                              std::string_view headers = kTestSseHeaders)
      : global_(detail::EnsureCurlGlobalInit()),
        expected_(expected),
        initial_response_(std::string(headers).append(initial)),
        listener_(MakeTestListener(&port_)),
        worker_([this, listener = listener_.get()] { AcceptStreams(listener); }) {}
  ~CurlLoopbackServer() {
    listener_.Close();
    worker_.join();
  }
  CurlLoopbackServer(const CurlLoopbackServer&) = delete;
  CurlLoopbackServer& operator=(const CurlLoopbackServer&) = delete;

  [[nodiscard]] std::string url() const {
    std::string result(kLoopbackUrlPrefix);
    result.append(std::to_string(port_));
    result.append(kStreamTestPath);
    return result;
  }
  [[nodiscard]] bool WaitForStreams(std::size_t count) {
    std::unique_lock lock(mutex_);
    return changed_.wait_for(lock, kReactorTestTimeout, [this, count] { return clients_.size() >= count; });
  }
  [[nodiscard]] bool Finish(std::size_t index, std::string_view last = kTestLastChunk) {
    std::lock_guard lock(mutex_);
    const bool sent = SendTestBytes(clients_.at(index).get(), last);
    clients_.at(index).Close();
    return sent;
  }

 private:
  void AcceptStreams(TestSocketHandle listener) {
    for (std::size_t index = 0; index < expected_; ++index) {
      TestSocket client(::accept(listener, nullptr, nullptr));
      if (!client.valid() || !ReadTestRequest(client.get())) {
        return;
      }
      {
        std::lock_guard lock(mutex_);
        // Publish the connection before a fast first-event callback can call
        // Finish(). Hold the same lock across the send and registration.
        // Send headers and the first chunk together: two tiny writes can put
        // the chunk behind Nagle/delayed ACK instead of measuring the reactor.
        if (!SendTestBytes(client.get(), initial_response_)) {
          return;
        }
        clients_.push_back(std::move(client));
      }
      changed_.notify_all();
    }
  }

  std::shared_ptr<const detail::ClientGlobalState> global_;
  const std::size_t expected_;
  const std::string initial_response_;
  unsigned short port_ = 0;
  TestSocket listener_;
  std::mutex mutex_;
  std::condition_variable changed_;
  std::vector<TestSocket> clients_;
  std::thread worker_;
};

struct CurlStreamProbe final {
  using Clock = std::chrono::steady_clock;
  std::mutex mutex;
  std::condition_variable changed;
  std::function<void()> cancel;
  std::promise<core::Result<Response>> completed;
  std::future<core::Result<Response>> result = completed.get_future();
  std::string bytes;
  bool metadata = false;
  bool chunk_before_metadata = false;
  Clock::time_point started = Clock::now();
  Clock::time_point first_event{};
  Clock::time_point finished{};

  [[nodiscard]] bool WaitForChunk() {
    std::unique_lock lock(mutex);
    return changed.wait_for(lock, kReactorTestTimeout, [this] { return !bytes.empty(); });
  }
};

inline std::shared_ptr<CurlStreamProbe> StartTestStream(const Client& client, std::string url,
                                                        std::chrono::milliseconds timeout = kIdleRequestTimeout) {
  auto probe = std::make_shared<CurlStreamProbe>();
  Request request{
      .method = std::string(kTestGetMethod), .url = std::move(url), .headers = {}, .body = {}, .timeout = timeout};
  const auto started = client.StartStreamRequest(
      std::move(request),
      [probe](const Response&) -> core::Result<void> {
        std::lock_guard lock(probe->mutex);
        probe->metadata = true;
        return {};
      },
      [probe](std::string_view bytes) -> core::Result<void> {
        {
          std::lock_guard lock(probe->mutex);
          probe->chunk_before_metadata = probe->chunk_before_metadata || !probe->metadata;
          if (probe->bytes.empty()) {
            probe->first_event = CurlStreamProbe::Clock::now();
          }
          probe->bytes.append(bytes);
        }
        probe->changed.notify_all();
        return {};
      },
      [] { return false; }, [probe](const std::function<void()>& cancel) { probe->cancel = cancel; },
      [probe](core::Result<Response> result) {
        probe->finished = CurlStreamProbe::Clock::now();
        probe->completed.set_value(std::move(result));
      });
  if (!started.ok()) {
    throw std::runtime_error(std::string(started.error().message()));
  }
  return probe;
}

inline std::size_t ReactorTestThreadCount() {
#ifdef _WIN32
  const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (snapshot == INVALID_HANDLE_VALUE) {
    throw std::runtime_error(std::string(kSocketFailure));
  }
  THREADENTRY32 entry{};
  entry.dwSize = sizeof(entry);
  std::size_t count = 0;
  if (Thread32First(snapshot, &entry) != 0) {
    do {
      count += entry.th32OwnerProcessID == GetCurrentProcessId() ? 1U : 0U;
    } while (Thread32Next(snapshot, &entry) != 0);
  }
  (void)CloseHandle(snapshot);
  if (count == 0U) {
    throw std::runtime_error(std::string(kSocketFailure));
  }
  return count;
#elif defined(__linux__)
  constexpr std::string_view kThreadDirectory = "/proc/self/task";
  std::size_t count = 0;
  for (const auto& entry : std::filesystem::directory_iterator(kThreadDirectory)) {
    (void)entry;
    ++count;
  }
  return count;
#else
  return 0;
#endif
}

}  // namespace a2a::http::testing
