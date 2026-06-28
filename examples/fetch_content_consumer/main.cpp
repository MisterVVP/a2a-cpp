// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "a2a/core/version.h"

int main() { return a2a::core::Version::HeaderValue().empty() ? 1 : 0; }
