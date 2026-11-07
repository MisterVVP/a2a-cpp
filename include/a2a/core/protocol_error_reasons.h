// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

namespace a2a::core::protocol_error_reasons {

inline constexpr char kTaskNotFound[] = "TASK_NOT_FOUND";
inline constexpr char kTaskNotCancelable[] = "TASK_NOT_CANCELABLE";
inline constexpr char kPushNotificationNotSupported[] = "PUSH_NOTIFICATION_NOT_SUPPORTED";
inline constexpr char kUnsupportedOperation[] = "UNSUPPORTED_OPERATION";
inline constexpr char kContentTypeNotSupported[] = "CONTENT_TYPE_NOT_SUPPORTED";
inline constexpr char kInvalidAgentResponse[] = "INVALID_AGENT_RESPONSE";
inline constexpr char kExtendedAgentCardNotConfigured[] = "EXTENDED_AGENT_CARD_NOT_CONFIGURED";
inline constexpr char kExtensionSupportRequired[] = "EXTENSION_SUPPORT_REQUIRED";
inline constexpr char kVersionNotSupported[] = "VERSION_NOT_SUPPORTED";
inline constexpr char kValidation[] = "VALIDATION_ERROR";
inline constexpr char kNetwork[] = "NETWORK_ERROR";
inline constexpr char kRemoteProtocol[] = "REMOTE_PROTOCOL_ERROR";
inline constexpr char kSerialization[] = "SERIALIZATION_ERROR";
inline constexpr char kInternal[] = "INTERNAL_ERROR";
inline constexpr char kInvalidParams[] = "INVALID_PARAMS";

}  // namespace a2a::core::protocol_error_reasons
