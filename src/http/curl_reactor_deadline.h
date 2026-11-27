// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

#include <algorithm>
#include <chrono>
#include <limits>
#include <optional>

namespace a2a::http::detail {

// Owned by the reactor thread. A zero libcurl timer is deferred until the
// callback unwinds; a negative timer removes the deadline entirely.
class CurlReactorDeadline final {
 public:
  using Clock = std::chrono::steady_clock;

  void Update(long timeout_ms, Clock::time_point now = Clock::now()) noexcept {
    deadline_.reset();
    if (timeout_ms >= 0) {
      deadline_ = now + std::chrono::milliseconds(timeout_ms);
    }
  }

  [[nodiscard]] int WaitMilliseconds(Clock::time_point now = Clock::now()) const noexcept {
    if (!deadline_) {
      return -1;
    }
    if (*deadline_ <= now) {
      return 0;
    }
    // Round up so sub-millisecond remainders do not cause busy spinning.
    const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(*deadline_ - now).count();
    constexpr auto kMaximumWait = static_cast<std::chrono::milliseconds::rep>((std::numeric_limits<int>::max)());
    return static_cast<int>((std::min)(remaining, kMaximumWait));
  }

  [[nodiscard]] bool TakeExpired(Clock::time_point now = Clock::now()) noexcept {
    if (!deadline_ || *deadline_ > now) {
      return false;
    }
    deadline_.reset();
    return true;
  }

 private:
  std::optional<Clock::time_point> deadline_;
};

}  // namespace a2a::http::detail
