// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

#if defined(A2A_HAS_LIBCURL)

#include <curl/curl.h>

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>

namespace a2a::http::detail {

class CurlStreamReactor final : public std::enable_shared_from_this<CurlStreamReactor> {
 public:
  struct Transfer final {
    CURL* easy_handle = nullptr;
    // Non-owning identity; `lifetime` keeps the owning client state alive.
    const void* owner = nullptr;
    std::mutex mutex;
    std::condition_variable completed;
    CURLcode result = CURLE_ABORTED_BY_CALLBACK;
    bool done = false;
    std::function<void(CURLcode)> on_complete;
    std::shared_ptr<void> lifetime;
  };

  [[nodiscard]] static std::shared_ptr<CurlStreamReactor> Create();
  ~CurlStreamReactor();

  CurlStreamReactor(const CurlStreamReactor&) = delete;
  CurlStreamReactor& operator=(const CurlStreamReactor&) = delete;

  void Add(const std::shared_ptr<Transfer>& transfer);
  void Cancel(const std::shared_ptr<Transfer>& transfer);
  void CancelOwner(const void* owner);
  void Shutdown();

 private:
  enum class CommandType : std::uint8_t { kAdd, kCancel, kCancelOwner, kShutdown };

  struct Command final {
    CommandType type = CommandType::kShutdown;
    std::shared_ptr<Transfer> transfer;
    const void* owner = nullptr;
  };

  CurlStreamReactor();

  void Enqueue(Command command);
  void Wake() const noexcept;
  void Run();
  void ProcessCommands();
  void AddOnReactor(std::shared_ptr<Transfer> transfer);
  void Remove(const std::shared_ptr<Transfer>& transfer, CURLcode result);
  void RemoveOwner(const void* owner, CURLcode result);
  static void Complete(const std::shared_ptr<Transfer>& transfer, CURLcode result);
  void ReadCompletions();
  void FailAll(CURLcode result);

#if defined(__linux__)
  static void CloseDescriptor(int descriptor) noexcept;
  [[nodiscard]] bool AddInternalDescriptor(int descriptor) const;
  void WaitForLinuxEvents();
  static void DrainEventDescriptor(int descriptor) noexcept;
  void ProcessSocket(int descriptor, std::uint32_t events);
  static int HandleSocket(CURL* easy_handle, curl_socket_t socket, int action, void* user_data, void* socket_data);
  static int HandleTimer(CURLM* multi_handle, long timeout_ms, void* user_data);
#else
  void WaitForPortableEvents();
  static int HandleSocket(CURL* easy_handle, curl_socket_t socket, int action, void* user_data, void* socket_data);
  static int HandleTimer(CURLM* multi_handle, long timeout_ms, void* user_data);
#endif

  CURLM* multi_handle_ = nullptr;
  std::thread thread_;
  std::mutex command_mutex_;
  std::deque<Command> commands_;
  std::unordered_map<CURL*, std::shared_ptr<Transfer>> transfers_;
  bool valid_ = false;
  bool shutdown_enqueued_ = false;
  bool shutting_down_ = false;
#if defined(__linux__)
  int epoll_descriptor_ = -1;
  int wakeup_descriptor_ = -1;
  int timer_descriptor_ = -1;
#endif
};

class CurlStreamReactorPool final {
 public:
  [[nodiscard]] std::shared_ptr<CurlStreamReactor> Acquire();
  void CancelOwner(const void* owner);
  [[nodiscard]] std::size_t size() const;

 private:
  static constexpr std::size_t kPoolSize = 4U;

  std::array<std::weak_ptr<CurlStreamReactor>, kPoolSize> reactors_{};
  mutable std::array<std::mutex, kPoolSize> reactor_mutexes_{};
  std::atomic_size_t next_reactor_{0U};
};

[[nodiscard]] std::pair<std::size_t, std::size_t> GetCurlStreamReactorLifecycleCounts() noexcept;

}  // namespace a2a::http::detail

#endif
