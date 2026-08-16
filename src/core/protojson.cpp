// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "a2a/core/protojson.h"

#include <google/protobuf/descriptor.h>
#include <google/protobuf/struct.pb.h>
#include <google/protobuf/util/json_util.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace a2a::core {
namespace {

constexpr std::string_view kNullLiteral = "null";
constexpr std::string_view kDuplicateFieldError = "ProtoJSON object contains duplicate fields";
constexpr std::size_t kMaximumTrackedJsonDepth = 128U;
constexpr std::size_t kMaximumTrackedProtoFields = 64U;

enum class DuplicateFieldScanResult : std::uint8_t {
  kClean,
  kDuplicate,
  kNeedsFallback,
};

struct ScannedJsonField final {
  DuplicateFieldScanResult result;
  const google::protobuf::Descriptor* message_descriptor;
};

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

void SkipJsonWhitespace(const char*& current, const char* end) noexcept {
  while (current != end && (*current == ' ' || *current == '\t' || *current == '\n' || *current == '\r')) {
    ++current;
  }
}

[[nodiscard]] DuplicateFieldScanResult TrackKnownField(
    const google::protobuf::FieldDescriptor* field,
    std::array<const google::protobuf::FieldDescriptor*, kMaximumTrackedProtoFields>* seen_fields,
    std::size_t* seen_field_count) {
  if (field == nullptr) {
    return DuplicateFieldScanResult::kClean;
  }
  const auto seen = std::span(*seen_fields).first(*seen_field_count);
  if (std::ranges::find(seen, field) != seen.end()) {
    return DuplicateFieldScanResult::kDuplicate;
  }
  if (*seen_field_count == seen_fields->size()) {
    return DuplicateFieldScanResult::kNeedsFallback;
  }
  (*seen_fields)[(*seen_field_count)++] = field;
  return DuplicateFieldScanResult::kClean;
}

[[nodiscard]] ScannedJsonField ScanJsonObjectField(
    const char*& current, const char* end, const google::protobuf::Descriptor* descriptor,
    std::array<const google::protobuf::FieldDescriptor*, kMaximumTrackedProtoFields>* seen_fields,
    std::size_t* seen_field_count) {
  if (current == end || *current != '"') {
    return {DuplicateFieldScanResult::kNeedsFallback, nullptr};
  }

  const char* const key_begin = current + 1;
  if (!SkipJsonString(current, end)) {
    return {DuplicateFieldScanResult::kNeedsFallback, nullptr};
  }
  const char* const key_end = current - 1;
  const auto key_size = static_cast<std::size_t>(key_end - key_begin);
  if (std::memchr(key_begin, '\\', key_size) != nullptr) {
    return {DuplicateFieldScanResult::kNeedsFallback, nullptr};
  }

  const auto name = std::string_view(key_begin, key_size);
  const auto* field = descriptor != nullptr ? FindJsonField(*descriptor, name) : nullptr;
  const auto field_result = TrackKnownField(field, seen_fields, seen_field_count);
  if (field_result != DuplicateFieldScanResult::kClean) {
    return {field_result, nullptr};
  }
  const auto* message_descriptor =
      field != nullptr && field->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE
          ? field->message_type()
          : nullptr;
  return {DuplicateFieldScanResult::kClean, message_descriptor};
}

[[nodiscard]] DuplicateFieldScanResult ScanJsonValue(const char*& current, const char* end,
                                                     const google::protobuf::Descriptor* message_descriptor,
                                                     std::size_t depth);

[[nodiscard]] DuplicateFieldScanResult ScanJsonObject(const char*& current, const char* end,
                                                      const google::protobuf::Descriptor* descriptor,
                                                      std::size_t depth) {
  if (depth >= kMaximumTrackedJsonDepth || current == end || *current != '{') {
    return DuplicateFieldScanResult::kNeedsFallback;
  }
  ++current;

  std::array<const google::protobuf::FieldDescriptor*, kMaximumTrackedProtoFields> seen_fields;
  std::size_t seen_field_count = 0U;

  while (current != end) {
    SkipJsonWhitespace(current, end);
    if (current != end && *current == '}') {
      ++current;
      return DuplicateFieldScanResult::kClean;
    }

    const auto field = ScanJsonObjectField(current, end, descriptor, &seen_fields, &seen_field_count);
    if (field.result != DuplicateFieldScanResult::kClean) {
      return field.result;
    }

    SkipJsonWhitespace(current, end);
    if (current == end || *current != ':') {
      return DuplicateFieldScanResult::kNeedsFallback;
    }
    ++current;

    const auto value_result = ScanJsonValue(current, end, field.message_descriptor, depth + 1U);
    if (value_result != DuplicateFieldScanResult::kClean) {
      return value_result;
    }

    SkipJsonWhitespace(current, end);
    if (current == end) {
      return DuplicateFieldScanResult::kNeedsFallback;
    }
    if (*current == '}') {
      ++current;
      return DuplicateFieldScanResult::kClean;
    }
    if (*current != ',') {
      return DuplicateFieldScanResult::kNeedsFallback;
    }
    ++current;
  }
  return DuplicateFieldScanResult::kNeedsFallback;
}

[[nodiscard]] DuplicateFieldScanResult ScanJsonArray(const char*& current, const char* end,
                                                     const google::protobuf::Descriptor* element_descriptor,
                                                     std::size_t depth) {
  if (depth >= kMaximumTrackedJsonDepth || current == end || *current != '[') {
    return DuplicateFieldScanResult::kNeedsFallback;
  }
  ++current;

  while (current != end) {
    SkipJsonWhitespace(current, end);
    if (current != end && *current == ']') {
      ++current;
      return DuplicateFieldScanResult::kClean;
    }

    const auto value_result = ScanJsonValue(current, end, element_descriptor, depth + 1U);
    if (value_result != DuplicateFieldScanResult::kClean) {
      return value_result;
    }

    SkipJsonWhitespace(current, end);
    if (current == end) {
      return DuplicateFieldScanResult::kNeedsFallback;
    }
    if (*current == ']') {
      ++current;
      return DuplicateFieldScanResult::kClean;
    }
    if (*current != ',') {
      return DuplicateFieldScanResult::kNeedsFallback;
    }
    ++current;
  }
  return DuplicateFieldScanResult::kNeedsFallback;
}

[[nodiscard]] DuplicateFieldScanResult ScanJsonValue(const char*& current, const char* end,
                                                     const google::protobuf::Descriptor* message_descriptor,
                                                     std::size_t depth) {
  SkipJsonWhitespace(current, end);
  if (current == end) {
    return DuplicateFieldScanResult::kNeedsFallback;
  }
  if (*current == '{') {
    return ScanJsonObject(current, end, message_descriptor, depth);
  }
  if (*current == '[') {
    return ScanJsonArray(current, end, message_descriptor, depth);
  }
  if (*current == '"') {
    return SkipJsonString(current, end) ? DuplicateFieldScanResult::kClean : DuplicateFieldScanResult::kNeedsFallback;
  }

  const char* const value_begin = current;
  while (current != end && *current != ',' && *current != ']' && *current != '}' && *current != ' ' &&
         *current != '\t' && *current != '\n' && *current != '\r') {
    ++current;
  }
  return current != value_begin ? DuplicateFieldScanResult::kClean : DuplicateFieldScanResult::kNeedsFallback;
}

[[nodiscard]] DuplicateFieldScanResult ScanDuplicateMessageFields(std::string_view json,
                                                                  const google::protobuf::Descriptor& descriptor) {
  const char* current = json.data();
  const char* const end = current + json.size();
  const auto result = ScanJsonValue(current, end, &descriptor, 0U);
  if (result != DuplicateFieldScanResult::kClean) {
    return result;
  }
  SkipJsonWhitespace(current, end);
  return current == end ? DuplicateFieldScanResult::kClean : DuplicateFieldScanResult::kNeedsFallback;
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

[[nodiscard]] Error BuildDuplicateFieldError() { return Error::Serialization(std::string(kDuplicateFieldError)); }

Result<void> ValidateDuplicateAliases(const google::protobuf::Struct& object,
                                      const google::protobuf::Descriptor& descriptor);

Result<void> ValidateNestedDuplicateAliases(const google::protobuf::FieldDescriptor& field,
                                            const google::protobuf::Value& value) {
  if (field.cpp_type() != google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE) {
    return {};
  }
  if (field.is_repeated()) {
    if (value.kind_case() != google::protobuf::Value::kListValue) {
      return {};
    }
    for (const auto& element : value.list_value().values()) {
      if (element.kind_case() != google::protobuf::Value::kStructValue) {
        continue;
      }
      const auto nested = ValidateDuplicateAliases(element.struct_value(), *field.message_type());
      if (!nested.ok()) {
        return nested.error();
      }
    }
    return {};
  }
  if (value.kind_case() != google::protobuf::Value::kStructValue) {
    return {};
  }
  return ValidateDuplicateAliases(value.struct_value(), *field.message_type());
}

Result<void> ValidateDuplicateAliases(const google::protobuf::Struct& object,
                                      const google::protobuf::Descriptor& descriptor) {
  std::vector<const google::protobuf::FieldDescriptor*> seen_fields;
  seen_fields.reserve(static_cast<std::size_t>(descriptor.field_count()));

  for (const auto& [name, value] : object.fields()) {
    const auto* field = FindJsonField(descriptor, name);
    if (field == nullptr) {
      continue;
    }
    if (std::ranges::find(seen_fields, field) != seen_fields.end()) {
      return BuildDuplicateFieldError();
    }
    seen_fields.push_back(field);

    const auto nested = ValidateNestedDuplicateAliases(*field, value);
    if (!nested.ok()) {
      return nested.error();
    }
  }
  return {};
}

Result<void> ValidateRejectedNullFields(const google::protobuf::Struct& object,
                                        const google::protobuf::Descriptor& descriptor) {
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

Result<void> ValidateTopLevelFields(std::string_view json, const google::protobuf::Message& message,
                                    const ProtoJsonParseOptions& options) {
  const auto& descriptor = *message.GetDescriptor();
  const auto duplicate_scan = options.reject_duplicate_top_level_fields ? ScanDuplicateMessageFields(json, descriptor)
                                                                        : DuplicateFieldScanResult::kClean;
  if (duplicate_scan == DuplicateFieldScanResult::kDuplicate) {
    return BuildDuplicateFieldError();
  }

  const bool null_candidate = options.reject_top_level_null_fields && HasBoundaryNullCandidate(json);
  const bool duplicate_fallback = duplicate_scan == DuplicateFieldScanResult::kNeedsFallback;
  if (!null_candidate && !duplicate_fallback) {
    return {};
  }

  google::protobuf::Struct object;
  const auto object_status = google::protobuf::util::JsonStringToMessage(std::string(json), &object);
  if (!object_status.ok()) {
    return Error::Serialization(object_status.ToString());
  }

  if (duplicate_fallback) {
    const auto duplicate_validation = ValidateDuplicateAliases(object, descriptor);
    if (!duplicate_validation.ok()) {
      return duplicate_validation.error();
    }
  }
  if (!options.reject_top_level_null_fields) {
    return {};
  }
  return ValidateRejectedNullFields(object, descriptor);
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
