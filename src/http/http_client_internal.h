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
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "a2a/http/http_client.h"

namespace a2a::http::detail {

class CurlStreamReactor final : public std::enable_shared_from_this<CurlStreamReactor> {
 public:
  using ThreadFactory = std::function<std::thread(std::function<void()>)>;

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
  [[nodiscard]] static std::shared_ptr<CurlStreamReactor> Create(const ThreadFactory& thread_factory);
  ~CurlStreamReactor();

  CurlStreamReactor(const CurlStreamReactor&) = delete;
  CurlStreamReactor& operator=(const CurlStreamReactor&) = delete;

  void Add(const std::shared_ptr<Transfer>& transfer);
  void Cancel(const std::shared_ptr<Transfer>& transfer);
  void CancelOwner(const void* owner);
  void Shutdown();
  [[nodiscard]] bool IsCurrentThread() const noexcept;

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
  static constexpr long kDefaultPortablePollTimeoutMs = 1000L;
  long portable_poll_timeout_ms_ = kDefaultPortablePollTimeoutMs;
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
  // Returns false only when a pool-only shard is the calling reactor and its
  // release must be retried from another thread.
  [[nodiscard]] bool ReleaseUnused();
  [[nodiscard]] std::size_t size() const;

 private:
  static constexpr std::size_t kPoolSize = 4U;

  // Pin created shards until client-state teardown can safely release a
  // pool-only reactor from a non-reactor thread.
  std::array<std::shared_ptr<CurlStreamReactor>, kPoolSize> reactors_{};
  mutable std::array<std::mutex, kPoolSize> reactor_mutexes_{};
  std::atomic_size_t next_reactor_{0U};
};

[[nodiscard]] std::pair<std::size_t, std::size_t> GetCurlStreamReactorLifecycleCounts() noexcept;

struct CurlSlistDeleter final {
  void operator()(curl_slist* list) const noexcept;
};

using CurlHeaderList = std::unique_ptr<curl_slist, CurlSlistDeleter>;

struct ClientGlobalState final {
  ClientGlobalState();

  CURLcode code = CURLE_OK;
  std::shared_ptr<CurlStreamReactorPool> stream_reactor_pool;
};

struct RequestSlot final {
  ~RequestSlot();

  CURL* easy_handle = nullptr;
  std::shared_ptr<CurlStreamReactor> reactor;
};

struct StreamSlot final {
  ~StreamSlot();

  CURL* easy_handle = nullptr;
  std::shared_ptr<CurlStreamReactor> reactor;
};

struct ClientState final {
  ~ClientState();

  std::shared_ptr<const ClientGlobalState> global_state;
  std::atomic_size_t client_owners{1U};
  // The first unary and streaming handles share one reactor shard so common
  // request -> stream sequences can reuse the same CURLM connection cache.
  std::mutex reactor_mutex;
  std::weak_ptr<CurlStreamReactor> primary_reactor;
  // Blocking unary requests borrow independent easy handles. This mutex
  // protects only the idle pool and is never held across network I/O.
  std::mutex request_mutex;
  std::vector<std::unique_ptr<RequestSlot>> idle_request_slots;
  // Guards shutdown, stream accounting, and reusable stream easy handles.
  std::mutex stream_mutex;
  std::vector<std::unique_ptr<StreamSlot>> idle_stream_slots;
  bool primary_stream_reactor_assigned = false;
  std::condition_variable streams_finished;
  std::size_t active_streams = 0;
  bool shutting_down = false;
  std::atomic<bool> suppress_stream_callbacks{false};
};

struct StreamHeaderContext final {
  HeaderCapture* header_capture = nullptr;
  StreamCallbackContext* stream_context = nullptr;
};

[[nodiscard]] std::shared_ptr<const ClientGlobalState> EnsureCurlGlobalInit();
[[nodiscard]] bool IsDispatchingStreamCallback(const ClientState* state) noexcept;
[[nodiscard]] std::string BuildCurlErrorMessage(std::string_view prefix, CURLcode code, std::string_view detail);
[[nodiscard]] core::Result<CurlHeaderList> BuildHeaders(const std::vector<Header>& headers);
[[nodiscard]] core::Result<void> ValidateStreamMetadata(StreamCallbackContext* context);
std::size_t WriteStreamResponseHeader(char* contents, std::size_t size, std::size_t nmemb, void* user_data);
[[nodiscard]] core::Result<void> ConfigureCurl(CURL* handle, const Request& request, const CurlHeaderList& headers,
                                               std::string* response_body, HeaderCapture* response_headers);
[[nodiscard]] core::Result<void> ConfigureCurlStream(CURL* handle, const Request& request,
                                                     const CurlHeaderList& headers,
                                                     StreamCallbackContext* stream_context,
                                                     HeaderCapture* response_headers);
[[nodiscard]] core::Result<std::unique_ptr<RequestSlot>> AcquireRequestSlot(ClientState& state);
void ReleaseRequestSlot(ClientState& state, std::unique_ptr<RequestSlot> slot);
[[nodiscard]] core::Result<std::unique_ptr<StreamSlot>> AcquireStreamSlot(ClientState& state);
void ReleaseStreamSlot(ClientState& state, std::unique_ptr<StreamSlot> slot);
[[nodiscard]] CURLcode PerformCurlTransfer(CURL* handle, const std::shared_ptr<CurlStreamReactor>& reactor);

}  // namespace a2a::http::detail

#endif
