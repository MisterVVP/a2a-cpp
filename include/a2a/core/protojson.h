// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

#include <google/protobuf/message.h>

#include <string>
#include <string_view>

#include "result.h"

namespace a2a::core {

struct ProtoJsonWriteOptions {
  bool add_whitespace = false;
  bool always_print_primitive_fields = false;
  bool preserve_proto_field_names = false;
  bool always_print_enums_as_ints = false;
};

struct ProtoJsonParseOptions {
  bool ignore_unknown_fields = false;
  // Also rejects null elements in top-level repeated message fields.
  bool reject_top_level_null_fields = false;
  // Rejects duplicate JSON members in the target message and nested message values.
  bool reject_duplicate_top_level_fields = false;
};

[[nodiscard]] Result<std::string> MessageToJson(const google::protobuf::Message& message,
                                                const ProtoJsonWriteOptions& options = {});

[[nodiscard]] Result<void> JsonToMessage(std::string_view json, google::protobuf::Message* message,
                                         const ProtoJsonParseOptions& options = {});

}  // namespace a2a::core
