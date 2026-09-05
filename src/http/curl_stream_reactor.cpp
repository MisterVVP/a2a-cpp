// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "http_client_internal.h"

#if defined(A2A_HAS_LIBCURL)

#if defined(__linux__)
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include <cerrno>
#endif

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>

namespace a2a::http::detail {
namespace {

std::atomic_size_t g_curl_stream_reactors_created{0U};
std::atomic_size_t g_curl_stream_reactors_destroyed{0U};

#if defined(__linux__)
constexpr std::size_t kMaximumReactorEvents = 64U;
#endif

}  // namespace

std::shared_ptr<CurlStreamReactor> CurlStreamReactor::Create() {
  auto reactor = std::shared_ptr<CurlStreamReactor>(new CurlStreamReactor());
  g_curl_stream_reactors_created.fetch_add(1U, std::memory_order_relaxed);
  if (!reactor->valid_) {
    return {};
  }
  reactor->thread_ = std::thread([reactor_pointer = reactor.get()] { reactor_pointer->Run(); });
  return reactor;
}

CurlStreamReactor::~CurlStreamReactor() {
  Shutdown();
#if defined(__linux__)
  CloseDescriptor(timer_descriptor_);
  CloseDescriptor(wakeup_descriptor_);
  CloseDescriptor(epoll_descriptor_);
#endif
  if (multi_handle_ != nullptr) {
    curl_multi_cleanup(multi_handle_);
  }
  g_curl_stream_reactors_destroyed.fetch_add(1U, std::memory_order_relaxed);
}

void CurlStreamReactor::Add(const std::shared_ptr<Transfer>& transfer) {
  Enqueue(Command{.type = CommandType::kAdd, .transfer = transfer});
}

void CurlStreamReactor::Cancel(const std::shared_ptr<Transfer>& transfer) {
  Enqueue(Command{.type = CommandType::kCancel, .transfer = transfer});
}

void CurlStreamReactor::CancelOwner(const void* owner) {
  Enqueue(Command{.type = CommandType::kCancelOwner, .transfer = {}, .owner = owner});
}

void CurlStreamReactor::Shutdown() {
  Enqueue(Command{.type = CommandType::kShutdown, .transfer = {}});
  if (thread_.joinable()) {
    thread_.join();
  }
}

CurlStreamReactor::CurlStreamReactor() : multi_handle_(curl_multi_init()) {
  if (multi_handle_ == nullptr) {
    return;
  }
#if defined(__linux__)
  epoll_descriptor_ = epoll_create1(EPOLL_CLOEXEC);
  wakeup_descriptor_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  timer_descriptor_ = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
  valid_ = epoll_descriptor_ >= 0 && wakeup_descriptor_ >= 0 && timer_descriptor_ >= 0 &&
           AddInternalDescriptor(wakeup_descriptor_) && AddInternalDescriptor(timer_descriptor_);
#else
  valid_ = true;
#endif
  valid_ = valid_ && curl_multi_setopt(multi_handle_, CURLMOPT_SOCKETFUNCTION, &HandleSocket) == CURLM_OK &&
           curl_multi_setopt(multi_handle_, CURLMOPT_SOCKETDATA, this) == CURLM_OK &&
           curl_multi_setopt(multi_handle_, CURLMOPT_TIMERFUNCTION, &HandleTimer) == CURLM_OK &&
           curl_multi_setopt(multi_handle_, CURLMOPT_TIMERDATA, this) == CURLM_OK;
}

void CurlStreamReactor::Enqueue(Command command) {
  {
    std::lock_guard lock(command_mutex_);
    if (shutdown_enqueued_ && command.type != CommandType::kShutdown) {
      Complete(command.transfer, CURLE_ABORTED_BY_CALLBACK);
      return;
    }
    shutdown_enqueued_ = shutdown_enqueued_ || command.type == CommandType::kShutdown;
    commands_.push_back(std::move(command));
  }
  Wake();
}

void CurlStreamReactor::Wake() const noexcept {
#if defined(__linux__)
  constexpr std::uint64_t kWakeValue = 1;
  ssize_t result = 0;
  do {
    result = write(wakeup_descriptor_, &kWakeValue, sizeof(kWakeValue));
  } while (result < 0 && errno == EINTR);
#else
  (void)curl_multi_wakeup(multi_handle_);
#endif
}

void CurlStreamReactor::Run() {
  while (!shutting_down_) {
    ProcessCommands();
    if (shutting_down_) {
      break;
    }
#if defined(__linux__)
    WaitForLinuxEvents();
#else
    WaitForPortableEvents();
#endif
    ReadCompletions();
  }
  for (auto& [easy_handle, transfer] : transfers_) {
    (void)curl_multi_remove_handle(multi_handle_, easy_handle);
    Complete(transfer, CURLE_ABORTED_BY_CALLBACK);
  }
  transfers_.clear();
}

void CurlStreamReactor::ProcessCommands() {
  std::deque<Command> commands;
  {
    std::lock_guard lock(command_mutex_);
    commands.swap(commands_);
  }
  for (auto& command : commands) {
    switch (command.type) {
      case CommandType::kAdd:
        AddOnReactor(std::move(command.transfer));
        break;
      case CommandType::kCancel:
        Remove(command.transfer, CURLE_ABORTED_BY_CALLBACK);
        break;
      case CommandType::kCancelOwner:
        RemoveOwner(command.owner, CURLE_ABORTED_BY_CALLBACK);
        break;
      case CommandType::kShutdown:
        shutting_down_ = true;
        break;
    }
  }
}

void CurlStreamReactor::AddOnReactor(std::shared_ptr<Transfer> transfer) {
  if (shutting_down_) {
    Complete(transfer, CURLE_ABORTED_BY_CALLBACK);
    return;
  }
  const CURLMcode code = curl_multi_add_handle(multi_handle_, transfer->easy_handle);
  if (code != CURLM_OK) {
    Complete(transfer, CURLE_FAILED_INIT);
    return;
  }
  transfers_.emplace(transfer->easy_handle, std::move(transfer));
  int running_handles = 0;
  const CURLMcode action_code = curl_multi_socket_action(multi_handle_, CURL_SOCKET_TIMEOUT, 0, &running_handles);
  if (action_code != CURLM_OK) {
    FailAll(CURLE_RECV_ERROR);
  }
  ReadCompletions();
}

void CurlStreamReactor::Remove(const std::shared_ptr<Transfer>& transfer, CURLcode result) {
  if (transfer == nullptr) {
    return;
  }
  const auto found = transfers_.find(transfer->easy_handle);
  if (found != transfers_.end() && found->second == transfer) {
    (void)curl_multi_remove_handle(multi_handle_, transfer->easy_handle);
    transfers_.erase(found);
  }
  Complete(transfer, result);
}

void CurlStreamReactor::RemoveOwner(const void* owner, CURLcode result) {
  auto current = transfers_.begin();
  while (current != transfers_.end()) {
    if (current->second->owner != owner) {
      ++current;
      continue;
    }
    auto transfer = std::move(current->second);
    (void)curl_multi_remove_handle(multi_handle_, current->first);
    current = transfers_.erase(current);
    Complete(transfer, result);
  }
}

void CurlStreamReactor::Complete(const std::shared_ptr<Transfer>& transfer, CURLcode result) {
  if (transfer == nullptr) {
    return;
  }
  {
    std::lock_guard lock(transfer->mutex);
    if (transfer->done) {
      return;
    }
    transfer->result = result;
    transfer->done = true;
  }
  transfer->completed.notify_all();
  if (transfer->on_complete) {
    transfer->on_complete(result);
  }
}

void CurlStreamReactor::ReadCompletions() {
  int pending = 0;
  while (CURLMsg* message = curl_multi_info_read(multi_handle_, &pending)) {
    if (message->msg != CURLMSG_DONE) {
      continue;
    }
    const auto found = transfers_.find(message->easy_handle);
    if (found == transfers_.end()) {
      continue;
    }
    auto transfer = std::move(found->second);
    transfers_.erase(found);
    (void)curl_multi_remove_handle(multi_handle_, message->easy_handle);
    Complete(transfer, message->data.result);
  }
}

void CurlStreamReactor::FailAll(CURLcode result) {
  for (auto& [easy_handle, transfer] : transfers_) {
    (void)curl_multi_remove_handle(multi_handle_, easy_handle);
    Complete(transfer, result);
  }
  transfers_.clear();
}

#if defined(__linux__)
void CurlStreamReactor::CloseDescriptor(int descriptor) noexcept {
  if (descriptor >= 0) {
    (void)close(descriptor);
  }
}

bool CurlStreamReactor::AddInternalDescriptor(int descriptor) const {
  epoll_event event{.events = EPOLLIN, .data = {.fd = descriptor}};
  return epoll_ctl(epoll_descriptor_, EPOLL_CTL_ADD, descriptor, &event) == 0;
}

void CurlStreamReactor::WaitForLinuxEvents() {
  std::array<epoll_event, kMaximumReactorEvents> events{};
  int count = 0;
  do {
    count = epoll_wait(epoll_descriptor_, events.data(), static_cast<int>(events.size()), -1);
  } while (count < 0 && errno == EINTR);
  if (count < 0) {
    FailAll(CURLE_RECV_ERROR);
    shutting_down_ = true;
    return;
  }
  for (int index = 0; index < count; ++index) {
    const int descriptor = events[static_cast<std::size_t>(index)].data.fd;
    if (descriptor == wakeup_descriptor_) {
      DrainEventDescriptor(wakeup_descriptor_);
      ProcessCommands();
    } else if (descriptor == timer_descriptor_) {
      DrainEventDescriptor(timer_descriptor_);
      int running_handles = 0;
      const CURLMcode code = curl_multi_socket_action(multi_handle_, CURL_SOCKET_TIMEOUT, 0, &running_handles);
      if (code != CURLM_OK) {
        FailAll(CURLE_RECV_ERROR);
      }
    } else {
      ProcessSocket(descriptor, events[static_cast<std::size_t>(index)].events);
    }
  }
}

void CurlStreamReactor::DrainEventDescriptor(int descriptor) noexcept {
  std::uint64_t value = 0;
  while (read(descriptor, &value, sizeof(value)) > 0) {
  }
}

void CurlStreamReactor::ProcessSocket(int descriptor, std::uint32_t events) {
  int action = 0;
  action |= (events & EPOLLIN) != 0U ? CURL_CSELECT_IN : 0;
  action |= (events & EPOLLOUT) != 0U ? CURL_CSELECT_OUT : 0;
  action |= (events & (EPOLLERR | EPOLLHUP)) != 0U ? CURL_CSELECT_ERR : 0;
  int running_handles = 0;
  const CURLMcode code = curl_multi_socket_action(multi_handle_, descriptor, action, &running_handles);
  if (code != CURLM_OK) {
    FailAll(CURLE_RECV_ERROR);
  }
}

int CurlStreamReactor::HandleSocket(CURL* easy_handle, curl_socket_t socket, int action, void* user_data,
                                    void* socket_data) {
  (void)easy_handle;
  (void)socket_data;
  auto* reactor = static_cast<CurlStreamReactor*>(user_data);
  if (action == CURL_POLL_REMOVE) {
    if (epoll_ctl(reactor->epoll_descriptor_, EPOLL_CTL_DEL, socket, nullptr) != 0 && errno != ENOENT &&
        errno != EBADF) {
      // Returning -1 lets libcurl abort after unwinding its socket callback;
      // mutating the multi handle recursively from here is not permitted.
      return -1;
    }
    return 0;
  }
  std::uint32_t events = EPOLLERR | EPOLLHUP;
  events |= action == CURL_POLL_IN || action == CURL_POLL_INOUT ? EPOLLIN : 0U;
  events |= action == CURL_POLL_OUT || action == CURL_POLL_INOUT ? EPOLLOUT : 0U;
  epoll_event event{.events = events, .data = {.fd = socket}};
  if (epoll_ctl(reactor->epoll_descriptor_, EPOLL_CTL_MOD, socket, &event) != 0 &&
      (errno != ENOENT || epoll_ctl(reactor->epoll_descriptor_, EPOLL_CTL_ADD, socket, &event) != 0)) {
    return -1;
  }
  return 0;
}

int CurlStreamReactor::HandleTimer(CURLM* multi_handle, long timeout_ms, void* user_data) {
  (void)multi_handle;
  auto* reactor = static_cast<CurlStreamReactor*>(user_data);
  itimerspec timer{};
  if (timeout_ms >= 0) {
    constexpr long kMillisecondsPerSecond = 1000L;
    constexpr long kNanosecondsPerMillisecond = 1000000L;
    timer.it_value.tv_sec = timeout_ms / kMillisecondsPerSecond;
    timer.it_value.tv_nsec = (timeout_ms % kMillisecondsPerSecond) * kNanosecondsPerMillisecond;
    if (timeout_ms == 0) {
      timer.it_value.tv_nsec = 1;
    }
  }
  if (timerfd_settime(reactor->timer_descriptor_, 0, &timer, nullptr) != 0) {
    // libcurl observes the callback failure after it regains control. Cleanup
    // remains in the reactor loop rather than recursively changing CURLM.
    return -1;
  }
  return 0;
}
#else
void CurlStreamReactor::WaitForPortableEvents() {
  int ready = 0;
  const CURLMcode poll_code = curl_multi_poll(multi_handle_, nullptr, 0, -1, &ready);
  if (poll_code != CURLM_OK) {
    FailAll(CURLE_RECV_ERROR);
    return;
  }
  ProcessCommands();
  int running_handles = 0;
  if (curl_multi_perform(multi_handle_, &running_handles) != CURLM_OK) {
    FailAll(CURLE_RECV_ERROR);
  }
}

int CurlStreamReactor::HandleSocket(CURL*, curl_socket_t, int, void*, void*) { return 0; }
int CurlStreamReactor::HandleTimer(CURLM*, long, void*) { return 0; }
#endif

std::shared_ptr<CurlStreamReactor> CurlStreamReactorPool::Acquire() {
  const std::size_t index = next_reactor_.fetch_add(1U, std::memory_order_relaxed) % kPoolSize;
  std::lock_guard lock(reactor_mutexes_[index]);
  auto reactor = reactors_[index].lock();
  if (reactor == nullptr) {
    reactor = CurlStreamReactor::Create();
    reactors_[index] = reactor;
  }
  return reactor;
}

void CurlStreamReactorPool::CancelOwner(const void* owner) {
  for (std::size_t index = 0; index < kPoolSize; ++index) {
    std::shared_ptr<CurlStreamReactor> reactor;
    {
      std::lock_guard lock(reactor_mutexes_[index]);
      reactor = reactors_[index].lock();
    }
    if (reactor != nullptr) {
      reactor->CancelOwner(owner);
    }
  }
}

std::size_t CurlStreamReactorPool::size() const {
  std::size_t count = 0U;
  for (std::size_t index = 0; index < kPoolSize; ++index) {
    std::lock_guard lock(reactor_mutexes_[index]);
    if (!reactors_[index].expired()) {
      ++count;
    }
  }
  return count;
}

std::pair<std::size_t, std::size_t> GetCurlStreamReactorLifecycleCounts() noexcept {
  return {g_curl_stream_reactors_created.load(std::memory_order_relaxed),
          g_curl_stream_reactors_destroyed.load(std::memory_order_relaxed)};
}

}  // namespace a2a::http::detail

#endif
