// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "a2a/core/protojson.h"

#include <google/protobuf/descriptor.h>
#include <google/protobuf/struct.pb.h>
#include <google/protobuf/util/json_util.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>

namespace a2a::core {
namespace {

constexpr std::string_view kNullLiteral = "null";
constexpr std::size_t kMaximumTrackedJsonDepth = 128U;
constexpr std::size_t kMaximumTrackedProtoFields = 64U;

[[nodiscard]] bool SkipJsonString(const char*& current, const char* end) noexcept {
  if (current == end || *current != '"') {
    return false;
  }
  ++current;
  while (current != end) {
    if (*current == '"') {
      ++current;
      return true;
    }
    if (*current == '\\') {
      ++current;
      if (current == end) {
        return false;
      }
    }
    ++current;
  }
  return false;
}

[[nodiscard]] bool IsBoundaryNullDepth(const std::array<char, kMaximumTrackedJsonDepth>& containers,
                                       std::size_t depth) noexcept {
  if (depth == 1U) {
    return containers[0] == '{';
  }
  return depth == 2U && containers[0] == '{' && containers[1] == '[';
}

[[nodiscard]] bool HasBoundaryNullCandidate(std::string_view json) noexcept {
  if (json.find(kNullLiteral) == std::string_view::npos) {
    return false;
  }

  std::array<char, kMaximumTrackedJsonDepth> containers{};
  std::size_t depth = 0U;
  const char* current = json.data();
  const char* const end = current + json.size();

  while (current != end) {
    if (*current == '"') {
      if (!SkipJsonString(current, end)) {
        return true;
      }
      continue;
    }
    if (*current == '{' || *current == '[') {
      if (depth == containers.size()) {
        return true;
      }
      containers[depth++] = *current++;
      continue;
    }
    if (*current == '}' || *current == ']') {
      if (depth == 0U) {
        return true;
      }
      --depth;
      ++current;
      continue;
    }
    const auto remaining = static_cast<std::size_t>(end - current);
    if (IsBoundaryNullDepth(containers, depth) && remaining >= kNullLiteral.size() &&
        std::memcmp(current, kNullLiteral.data(), kNullLiteral.size()) == 0) {
      return true;
    }
    ++current;
  }
  return false;
}

[[nodiscard]] const google::protobuf::FieldDescriptor* FindJsonField(const google::protobuf::Descriptor& descriptor,
                                                                     std::string_view name) {
  for (int index = 0; index < descriptor.field_count(); ++index) {
    const auto* field = descriptor.field(index);
    if (field->json_name() == name || field->name() == name) {
      return field;
    }
  }
  return nullptr;
}

[[nodiscard]] bool IsDuplicateKnownField(std::string_view name, const google::protobuf::Descriptor& descriptor,
                                         std::array<std::string_view, kMaximumTrackedProtoFields>* seen_fields,
                                         std::size_t* seen_field_count) {
  if (FindJsonField(descriptor, name) == nullptr) {
    return false;
  }
  if (std::ranges::find(*seen_fields, name) != seen_fields->end()) {
    return true;
  }
  if (*seen_field_count == seen_fields->size()) {
    return true;
  }
  (*seen_fields)[(*seen_field_count)++] = name;
  return false;
}

[[nodiscard]] bool ScanTopLevelKey(const char*& current, const char* end, bool is_top_level_key,
                                   const google::protobuf::Descriptor& descriptor,
                                   std::array<std::string_view, kMaximumTrackedProtoFields>* seen_fields,
                                   std::size_t* seen_field_count) {
  const char* const key_begin = current + 1;
  if (!SkipJsonString(current, end)) {
    return true;
  }
  if (!is_top_level_key) {
    return false;
  }

  const char* const key_end = current - 1;
  const auto key_size = static_cast<std::size_t>(key_end - key_begin);
  if (std::memchr(key_begin, '\\', key_size) != nullptr) {
    return true;
  }
  return IsDuplicateKnownField(std::string_view(key_begin, key_size), descriptor, seen_fields, seen_field_count);
}

[[nodiscard]] bool HasDuplicateTopLevelFieldCandidate(std::string_view json,
                                                      const google::protobuf::Descriptor& descriptor) {
  std::array<std::string_view, kMaximumTrackedProtoFields> seen_fields{};
  std::size_t depth = 0U;
  std::size_t seen_field_count = 0U;
  bool expect_top_level_key = false;
  const char* current = json.data();
  const char* const end = current + json.size();

  while (current != end) {
    if (*current == '"') {
      const bool is_top_level_key = depth == 1U && expect_top_level_key;
      expect_top_level_key = false;
      if (ScanTopLevelKey(current, end, is_top_level_key, descriptor, &seen_fields, &seen_field_count)) {
        return true;
      }
      continue;
    }
    if (*current == '{' || *current == '[') {
      ++depth;
      if (depth == 1U && current[0] == '{') {
        expect_top_level_key = true;
      }
      ++current;
      continue;
    }
    if (*current == '}' || *current == ']') {
      if (depth == 0U) {
        return true;
      }
      --depth;
      ++current;
      continue;
    }
    if (depth == 1U && *current == ',') {
      expect_top_level_key = true;
    }
    ++current;
  }
  return false;
}

[[nodiscard]] bool RepeatedMessageContainsNull(const google::protobuf::FieldDescriptor& field,
                                               const google::protobuf::Value& value) {
  if (!field.is_repeated() || field.cpp_type() != google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE ||
      value.kind_case() != google::protobuf::Value::kListValue) {
    return false;
  }
  return std::ranges::any_of(value.list_value().values(), [](const auto& element) {
    return element.kind_case() == google::protobuf::Value::kNullValue;
  });
}

[[nodiscard]] Error BuildNullFieldError(std::string_view name, bool repeated_element) {
  std::string message = repeated_element ? "ProtoJSON repeated message field must not contain null: "
                                         : "ProtoJSON field must not be null: ";
  message.append(name);
  return Error::Serialization(std::move(message));
}

Result<void> ValidateTopLevelFields(std::string_view json, const google::protobuf::Message& message,
                                    const ProtoJsonParseOptions& options) {
  const auto& descriptor = *message.GetDescriptor();
  const bool null_candidate = options.reject_top_level_null_fields && HasBoundaryNullCandidate(json);
  const bool duplicate_candidate =
      options.reject_duplicate_top_level_fields && HasDuplicateTopLevelFieldCandidate(json, descriptor);
  if (!null_candidate && !duplicate_candidate) {
    return {};
  }

  google::protobuf::Struct object;
  const auto object_status = google::protobuf::util::JsonStringToMessage(std::string(json), &object);
  if (!object_status.ok()) {
    return Error::Serialization(object_status.ToString());
  }

  if (!options.reject_top_level_null_fields) {
    return {};
  }
  for (const auto& [name, value] : object.fields()) {
    const auto* field = FindJsonField(descriptor, name);
    if (field == nullptr) {
      continue;
    }
    if (value.kind_case() == google::protobuf::Value::kNullValue) {
      return BuildNullFieldError(name, false);
    }
    if (RepeatedMessageContainsNull(*field, value)) {
      return BuildNullFieldError(name, true);
    }
  }
  return {};
}

}  // namespace

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

  if (options.reject_top_level_null_fields || options.reject_duplicate_top_level_fields) {
    const auto top_level_validation = ValidateTopLevelFields(json, *message, options);
    if (!top_level_validation.ok()) {
      return top_level_validation.error();
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
