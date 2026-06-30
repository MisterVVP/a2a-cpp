// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "a2a/core/non_copyable.h"

#include <type_traits>

#include <gtest/gtest.h>

namespace {

class MoveOnlyDerived final : private a2a::core::NonCopyable {
 public:
  MoveOnlyDerived() = default;
  MoveOnlyDerived(MoveOnlyDerived&&) noexcept = default;
  MoveOnlyDerived& operator=(MoveOnlyDerived&&) noexcept = default;
};

class ImmovableDerived final : private a2a::core::NonCopyableOrMovable {
 public:
  ImmovableDerived() = default;
};

static_assert(!std::is_copy_constructible_v<MoveOnlyDerived>);
static_assert(!std::is_copy_assignable_v<MoveOnlyDerived>);
static_assert(std::is_move_constructible_v<MoveOnlyDerived>);
static_assert(std::is_move_assignable_v<MoveOnlyDerived>);

static_assert(!std::is_copy_constructible_v<ImmovableDerived>);
static_assert(!std::is_copy_assignable_v<ImmovableDerived>);
static_assert(!std::is_move_constructible_v<ImmovableDerived>);
static_assert(!std::is_move_assignable_v<ImmovableDerived>);

}  // namespace

TEST(NonCopyableTest, TraitsAreEnforcedAtCompileTime) { SUCCEED(); }
