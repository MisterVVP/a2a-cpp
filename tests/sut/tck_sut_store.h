// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

#include "a2a/core/result.h"
#include "a2a/server/stores/store_factory.h"

namespace a2a::tests::sut {

[[nodiscard]] core::Result<server::stores::StoreBundle> CreateStoreBundleFromEnvironment();

}  // namespace a2a::tests::sut
