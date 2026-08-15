// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "a2a/core/protojson.h"

#include <google/protobuf/struct.pb.h>
#include <google/protobuf/util/json_util.h>

#include <string_view>
#include <utility>

namespace a2a::core {

Result<std::string> MessageToJson(const google::protobuf::Message& message, const ProtoJsonWriteOptions& options) {
  google::protobuf::util::JsonPrintOptions print_options;
  print_options.add_whitespace = options.add_whitespace;
#if PROTOBUF_VERSION >= 5026000
  print_options.always_print_fields_with_no_presence = options.always_print_primitive_fields;
#else
  print_options.always_print_primitive_fields = options.always_print_primitive_fields;
#endif
  print_options.preserve_proto_field_names = options.preserve_proto_field_names;
  print_options.always_print_enums_as_ints = options.always_print_enums_as_ints;

  std::string json;
  const auto status = google::protobuf::util::MessageToJsonString(message, &json, print_options);
  if (!status.ok()) {
    return Error::Serialization(status.ToString());
  }

  return json;
}

Result<void> JsonToMessage(std::string_view json, google::protobuf::Message* message,
                           const ProtoJsonParseOptions& options) {
  if (message == nullptr) {
    return Error::Validation("ProtoJSON parse target cannot be null");
  }

  constexpr std::string_view kNullLiteral = "null";
  if (options.reject_top_level_null_fields && json.find(kNullLiteral) != std::string_view::npos) {
    google::protobuf::Struct object;
    const auto object_status = google::protobuf::util::JsonStringToMessage(std::string(json), &object);
    if (!object_status.ok()) {
      return Error::Serialization(object_status.ToString());
    }
    const auto* descriptor = message->GetDescriptor();
    for (const auto& [name, value] : object.fields()) {
      if (value.kind_case() != google::protobuf::Value::kNullValue) {
        continue;
      }
      const auto* field = descriptor->FindFieldByCamelcaseName(name);
      if (field == nullptr) {
        field = descriptor->FindFieldByName(name);
      }
      if (field != nullptr) {
        std::string error_message = "ProtoJSON field must not be null: ";
        error_message.append(name);
        return Error::Serialization(std::move(error_message));
      }
    }
  }

  google::protobuf::util::JsonParseOptions parse_options;
  parse_options.ignore_unknown_fields = options.ignore_unknown_fields;

  const auto status = google::protobuf::util::JsonStringToMessage(std::string(json), message, parse_options);
  if (!status.ok()) {
    return Error::Serialization(status.ToString());
  }

  return {};
}

}  // namespace a2a::core
