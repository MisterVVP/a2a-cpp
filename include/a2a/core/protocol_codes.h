// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

#include <string_view>

namespace a2a::core::protocol_codes {

inline constexpr std::string_view kTaskNotFound = "-32001";
inline constexpr std::string_view kTaskNotCancelable = "-32002";
inline constexpr std::string_view kPushNotificationNotSupported = "-32003";
inline constexpr std::string_view kUnsupportedOperation = "-32004";
inline constexpr std::string_view kContentTypeNotSupported = "-32005";
inline constexpr std::string_view kInvalidAgentResponse = "-32006";
inline constexpr std::string_view kExtendedAgentCardNotConfigured = "-32007";
inline constexpr std::string_view kExtensionSupportRequired = "-32008";
inline constexpr std::string_view kVersionNotSupported = "-32009";

}  // namespace a2a::core::protocol_codes
