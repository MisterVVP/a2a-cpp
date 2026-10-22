// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "a2a/core/protojson.h"

#include <google/protobuf/descriptor.h>
#include <google/protobuf/struct.pb.h>
#include <google/protobuf/util/json_util.h>
#include <google/protobuf/wrappers.pb.h>

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "json.h"

namespace a2a::core {
namespace {

using Field = google::protobuf::FieldDescriptor;
using Descriptor = google::protobuf::Descriptor;
using Element = simdjson::ondemand::value;
using internal::JsonRead;
constexpr std::string_view kInvalidField = "JSON value does not match the protobuf field type";
constexpr std::string_view kDuplicateField = "ProtoJSON object contains duplicate fields";
constexpr std::string_view kUnknownField = "Unknown ProtoJSON field";
constexpr std::string_view kNullField = "ProtoJSON field must not be null";
constexpr std::string_view kNullElement = "ProtoJSON repeated field must not contain null";

[[noreturn]] void Reject(std::string_view reason) { throw std::invalid_argument(std::string(reason)); }

const Field* FindField(const Descriptor& descriptor, std::string_view name) {
  for (int index = 0; index < descriptor.field_count(); ++index) {
    const auto* field = descriptor.field(index);
    if (field->name() == name || field->json_name() == name) {
      return field;
    }
  }
  return nullptr;
}

// Protobuf remains the semantic decoder for uncommon scalar types and
// well-known types. Keeping this narrow fallback avoids a second base64,
// timestamp, Any, or 64-bit numeric compatibility implementation.
bool NeedsProtobufDecoder(const Descriptor& descriptor) {
  if (descriptor.well_known_type() != Descriptor::WELLKNOWNTYPE_UNSPECIFIED) {
    return true;
  }
  for (int index = 0; index < descriptor.field_count(); ++index) {
    const auto* field = descriptor.field(index);
    if ((field->is_map() && field->message_type()->map_key()->cpp_type() != Field::CPPTYPE_STRING) ||
        (field->is_map() && NeedsProtobufDecoder(*field->message_type())) ||
        (field->cpp_type() != Field::CPPTYPE_STRING && field->cpp_type() != Field::CPPTYPE_BOOL &&
         field->cpp_type() != Field::CPPTYPE_INT32 && field->cpp_type() != Field::CPPTYPE_ENUM &&
         field->cpp_type() != Field::CPPTYPE_MESSAGE)) {
      return true;
    }
  }
  return false;
}

std::int32_t ReadInt32(Element value) {
  const auto raw = JsonRead(simdjson::to_json_string(value));
  std::int32_t number = 0;
  const auto result = std::from_chars(raw.data(), raw.data() + raw.size(), number);
  if (result.ec == std::errc{} && result.ptr == raw.data() + raw.size()) {
    return number;
  }
  // Quoted integers and exponent notation follow protobuf's exact range and
  // integrality rules, including decimal tokens that binary64 would round.
  google::protobuf::Int32Value parsed;
  const auto status = google::protobuf::util::JsonStringToMessage(std::string(raw), &parsed);
  if (!status.ok()) {
    Reject(kInvalidField);
  }
  return parsed.value();
}

std::string ReadString(Element value, const Field& field) {
  if (field.type() != Field::TYPE_BYTES) {
    return std::string(JsonRead(value.get_string()));
  }
  google::protobuf::BytesValue bytes;
  const auto raw = JsonRead(simdjson::to_json_string(value));
  const auto status = google::protobuf::util::JsonStringToMessage(std::string(raw), &bytes);
  if (!status.ok()) {
    Reject(status.ToString());
  }
  return std::move(*bytes.mutable_value());
}

bool ContainsNull(simdjson::dom::element value) {
  if (value.is_null()) {
    return true;
  }
  if (value.is_array()) {
    return std::ranges::any_of(value.get_array(), [](const auto child) { return ContainsNull(child); });
  }
  if (value.is_object()) {
    return std::ranges::any_of(value.get_object(), [](const auto member) { return ContainsNull(member.value); });
  }
  return false;
}

void DecodeMessage(Element value, google::protobuf::Message* message, const ProtoJsonParseOptions& options);

void DecodeSingleField(Element value, google::protobuf::Message* message, const Field& field,
                       const ProtoJsonParseOptions& options) {
  const auto* reflection = message->GetReflection();
  switch (field.cpp_type()) {
    case Field::CPPTYPE_MESSAGE:
      DecodeMessage(
          value,
          field.is_repeated() ? reflection->AddMessage(message, &field) : reflection->MutableMessage(message, &field),
          options);
      return;
    case Field::CPPTYPE_STRING: {
      const std::string text = ReadString(value, field);
      if (field.is_repeated()) {
        reflection->AddString(message, &field, text);
      } else {
        reflection->SetString(message, &field, text);
      }
      return;
    }
    case Field::CPPTYPE_BOOL:
      if (field.is_repeated()) {
        reflection->AddBool(message, &field, JsonRead(value.get_bool()));
      } else {
        reflection->SetBool(message, &field, JsonRead(value.get_bool()));
      }
      return;
    case Field::CPPTYPE_INT32:
      if (field.is_repeated()) {
        reflection->AddInt32(message, &field, ReadInt32(value));
      } else {
        reflection->SetInt32(message, &field, ReadInt32(value));
      }
      return;
    case Field::CPPTYPE_ENUM: {
      int number = 0;
      if (JsonRead(value.type()) == simdjson::ondemand::json_type::string) {
        const auto* entry = field.enum_type()->FindValueByName(std::string(JsonRead(value.get_string())));
        if (entry == nullptr) {
          if (options.ignore_unknown_fields) {
            return;
          }
          Reject(kInvalidField);
        }
        number = entry->number();
      } else {
        number = ReadInt32(value);
      }
      if (field.is_repeated()) {
        reflection->AddEnumValue(message, &field, number);
      } else {
        reflection->SetEnumValue(message, &field, number);
      }
      return;
    }
    default:
      Reject(kInvalidField);
  }
}

void DecodeField(Element value, google::protobuf::Message* message, const Field& field,
                 const ProtoJsonParseOptions& options) {
  if (field.is_map()) {
    const auto* key = field.message_type()->map_key();
    const auto* map_value = field.message_type()->map_value();
    // Non-string map keys use protobuf's numeric/bool key semantics.
    if (key->cpp_type() != Field::CPPTYPE_STRING) {
      Reject(kInvalidField);
    }
    for (auto member_result : JsonRead(value.get_object())) {
      auto member = JsonRead(std::move(member_result));
      const auto name = JsonRead(member.unescaped_key());
      auto member_value = member.value();
      auto* entry = message->GetReflection()->AddMessage(message, &field);
      entry->GetReflection()->SetString(entry, key, std::string(name));
      if (JsonRead(member_value.is_null())) {
        Reject(kNullElement);
      }
      DecodeSingleField(member_value, entry, *map_value, options);
    }
    return;
  }
  if (field.is_repeated()) {
    for (auto element_result : JsonRead(value.get_array())) {
      auto element = JsonRead(element_result);
      if (JsonRead(element.is_null())) {
        Reject(kNullElement);
      }
      DecodeSingleField(element, message, field, options);
    }
    return;
  }
  DecodeSingleField(value, message, field, options);
}

void ValidateField(bool is_null, const Field& field, const std::vector<const Field*>& seen,
                   const ProtoJsonParseOptions& options, bool root) {
  if (options.reject_duplicate_top_level_fields && std::ranges::find(seen, &field) != seen.end()) {
    Reject(kDuplicateField);
  }
  if (is_null && root && options.reject_top_level_null_fields) {
    Reject(kNullField);
  }
}

void ValidateFields(simdjson::dom::element value, const Descriptor& descriptor, const ProtoJsonParseOptions& options,
                    bool root);

void ValidateNestedField(simdjson::dom::element value, const Field& field, const ProtoJsonParseOptions& options,
                         bool root) {
  if (field.cpp_type() != Field::CPPTYPE_MESSAGE) {
    return;
  }
  if (field.is_map() && value.is_object()) {
    const auto* map_value = field.message_type()->map_value();
    if (map_value->cpp_type() == Field::CPPTYPE_MESSAGE) {
      for (const auto entry : value.get_object()) {
        ValidateFields(entry.value, *map_value->message_type(), options, false);
      }
    }
  } else if (field.is_repeated() && value.is_array()) {
    for (const auto element : value.get_array()) {
      if (root && options.reject_top_level_null_fields && element.is_null()) {
        Reject(kNullElement);
      }
      ValidateFields(element, *field.message_type(), options, false);
    }
  } else {
    ValidateFields(value, *field.message_type(), options, false);
  }
}

void ValidateFields(simdjson::dom::element value, const Descriptor& descriptor, const ProtoJsonParseOptions& options,
                    bool root) {
  if (!value.is_object() || descriptor.well_known_type() != Descriptor::WELLKNOWNTYPE_UNSPECIFIED) {
    return;
  }
  std::vector<const Field*> seen;
  seen.reserve(static_cast<std::size_t>(descriptor.field_count()));
  for (const auto member : value.get_object()) {
    const auto* field = FindField(descriptor, member.key);
    if (field == nullptr) {
      continue;
    }
    ValidateField(member.value.is_null(), *field, seen, options, root);
    seen.push_back(field);
    ValidateNestedField(member.value, *field, options, root);
  }
}

void DecodeMessage(Element value, google::protobuf::Message* message, const ProtoJsonParseOptions& options) {
  const auto& descriptor = *message->GetDescriptor();
  if (NeedsProtobufDecoder(descriptor)) {
    const auto raw = JsonRead(simdjson::to_json_string(value));
    if (descriptor.well_known_type() == Descriptor::WELLKNOWNTYPE_VALUE ||
        descriptor.well_known_type() == Descriptor::WELLKNOWNTYPE_STRUCT ||
        descriptor.well_known_type() == Descriptor::WELLKNOWNTYPE_LISTVALUE) {
      const auto parsed = JsonToMessage(raw, message, options);
      if (!parsed.ok()) {
        Reject(parsed.error().message());
      }
      return;
    }
    google::protobuf::util::JsonParseOptions parse_options;
    parse_options.ignore_unknown_fields = options.ignore_unknown_fields;
    const auto status = google::protobuf::util::JsonStringToMessage(std::string(raw), message, parse_options);
    if (!status.ok()) {
      Reject(status.ToString());
    }
    return;
  }
  for (auto member_result : JsonRead(value.get_object())) {
    auto member = JsonRead(std::move(member_result));
    const auto name = JsonRead(member.unescaped_key());
    auto member_value = member.value();
    const auto* field = FindField(descriptor, name);
    if (field == nullptr) {
      if (!options.ignore_unknown_fields) {
        Reject(kUnknownField);
      }
      continue;
    }
    const bool is_null = JsonRead(member_value.is_null());
    if (is_null) {
      continue;
    }
    const auto* oneof = field->real_containing_oneof();
    if (oneof != nullptr && message->GetReflection()->HasOneof(*message, oneof)) {
      Reject(kDuplicateField);
    }
    DecodeField(member_value, message, *field, options);
  }
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
  internal::JsonDocument document;
  const auto parsed = document.Parse(json);
  if (!parsed.ok()) {
    return parsed.error();
  }
  try {
    if (options.reject_top_level_null_fields || options.reject_duplicate_top_level_fields) {
      ValidateFields(document.root(), *message->GetDescriptor(), options, true);
    }
    message->Clear();
    const auto type = message->GetDescriptor()->well_known_type();
    if (type == Descriptor::WELLKNOWNTYPE_VALUE) {
      message->CopyFrom(internal::JsonToValue(document.root()));
    } else if (type == Descriptor::WELLKNOWNTYPE_STRUCT && document.root().is_object()) {
      message->CopyFrom(internal::JsonToValue(document.root()).struct_value());
    } else if (type == Descriptor::WELLKNOWNTYPE_LISTVALUE && document.root().is_array()) {
      message->CopyFrom(internal::JsonToValue(document.root()).list_value());
    } else if (NeedsProtobufDecoder(*message->GetDescriptor()) ||
               (json.find("null") != std::string_view::npos && ContainsNull(document.root()))) {
      google::protobuf::util::JsonParseOptions parse_options;
      parse_options.ignore_unknown_fields = options.ignore_unknown_fields;
      const auto status = google::protobuf::util::JsonStringToMessage(std::string(json), message, parse_options);
      if (!status.ok()) {
        Reject(status.ToString());
      }
    } else {
      internal::JsonCursor cursor(json);
      DecodeMessage(JsonRead(cursor.root().get_value()), message, options);
    }
    return {};
  } catch (const simdjson::simdjson_error& error) {
    return Error::Serialization(error.what());
  } catch (const std::invalid_argument& error) {
    return Error::Serialization(error.what());
  }
}

}  // namespace a2a::core
