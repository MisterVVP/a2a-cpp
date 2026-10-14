// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

#include <google/protobuf/struct.pb.h>

#include <string>
#include <string_view>

namespace a2a::core::internal {
[[nodiscard]] google::protobuf::Value ParseSigningJson(std::string_view input);
[[nodiscard]] std::string Canonicalize(const google::protobuf::Value& value);
}  // namespace a2a::core::internal
