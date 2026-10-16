// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

#include <string_view>

namespace a2a::tests::security {
inline constexpr std::string_view kPrivateKey = R"(-----BEGIN PRIVATE KEY-----
MC4CAQAwBQYDK2VwBCIEIJ1hsZ3v/VpguoRK9JLsLMREScVpezJpGXA7rAMcrn9g
-----END PRIVATE KEY-----
)";
inline constexpr std::string_view kPublicKey = R"(-----BEGIN PUBLIC KEY-----
MCowBQYDK2VwAyEA11qYAYKxCrfVS/7TyWQHOg7hcvPapiMlrwIaaPcHURo=
-----END PUBLIC KEY-----
)";
inline constexpr std::string_view kKeyId = "rfc8032-test-key";
inline constexpr std::string_view kAliceToken = "security-fixture-alice";
inline constexpr std::string_view kBobToken = "security-fixture-bob";
inline constexpr std::string_view kTaskCredential = "security-fixture-task-credential";
inline constexpr std::string_view kUnknownToken = "security-fixture-invalid";
}  // namespace a2a::tests::security
