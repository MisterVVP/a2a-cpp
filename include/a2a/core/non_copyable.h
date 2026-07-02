// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

namespace a2a::core {

class NonCopyable {
 public:
  NonCopyable(const NonCopyable&) = delete;
  NonCopyable& operator=(const NonCopyable&) = delete;

 protected:
  constexpr NonCopyable() noexcept = default;
  constexpr NonCopyable(NonCopyable&&) noexcept = default;
  constexpr NonCopyable& operator=(NonCopyable&&) noexcept = default;
  ~NonCopyable() = default;
};

class NonCopyableOrMovable {
 public:
  NonCopyableOrMovable(const NonCopyableOrMovable&) = delete;
  NonCopyableOrMovable& operator=(const NonCopyableOrMovable&) = delete;
  NonCopyableOrMovable(NonCopyableOrMovable&&) = delete;
  NonCopyableOrMovable& operator=(NonCopyableOrMovable&&) = delete;

 protected:
  constexpr NonCopyableOrMovable() noexcept = default;
  ~NonCopyableOrMovable() = default;
};

}  // namespace a2a::core
