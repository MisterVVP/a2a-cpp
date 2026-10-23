// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "json.h"

#include <string>
#include <unordered_set>
#include <utility>

namespace a2a::core::internal {
namespace {
thread_local std::unique_ptr<simdjson::dom::parser> cached_parser;
thread_local std::unique_ptr<simdjson::ondemand::parser> cached_cursor;
constexpr std::string_view kJsonSizeError = "JSON input exceeds the 64 MiB limit";
constexpr std::string_view kDuplicateEnvelopeMember = "JSON object contains duplicate envelope members";
}  // namespace

JsonDocument::JsonDocument() : parser_(std::move(cached_parser)) {
  if (parser_ == nullptr) {
    parser_ = std::make_unique<simdjson::dom::parser>(kMaximumJsonBytes);
  }
}

JsonDocument::~JsonDocument() {
  if (cached_parser == nullptr && capacity() <= kMaximumCachedJsonBytes) {
    cached_parser = std::move(parser_);
  }
}

Result<void> JsonDocument::Parse(std::string_view input) {
  if (input.size() > kMaximumJsonBytes) {
    return Error::Serialization(std::string(kJsonSizeError));
  }
  const auto allocation = parser_->allocate(input.size(), kMaximumJsonDepth);
  if (allocation != simdjson::SUCCESS) {
    return Error::Serialization(simdjson::error_message(allocation));
  }
  const auto error = parser_->parse(input.data(), input.size()).get(root_);
  if (error != simdjson::SUCCESS) {
    return Error::Serialization(simdjson::error_message(error));
  }
  return {};
}

Result<void> JsonDocument::ParseObject(std::string_view input) {
  const auto parsed = Parse(input);
  if (!parsed.ok()) {
    return parsed.error();
  }
  simdjson::dom::object object;
  const auto error = root_.get_object().get(object);
  if (error != simdjson::SUCCESS) {
    return Error::Serialization(simdjson::error_message(error));
  }
  std::unordered_set<std::string_view> names;
  for (const auto member : object) {
    if (!names.insert(member.key).second) {
      return Error::Serialization(std::string(kDuplicateEnvelopeMember));
    }
  }
  return {};
}

JsonCursor::JsonCursor(std::string_view input) : input_(input), parser_(std::move(cached_cursor)) {
  if (parser_ == nullptr) {
    parser_ = std::make_unique<simdjson::ondemand::parser>(kMaximumJsonBytes);
  }
  root_ = JsonRead(parser_->iterate(input_));
}

JsonCursor::~JsonCursor() {
  if (cached_cursor == nullptr && capacity() <= kMaximumCachedJsonBytes) {
    cached_cursor = std::move(parser_);
  }
}

std::string JsonMember(std::string_view input, std::string_view name) {
  JsonCursor cursor(input);
  for (auto member : JsonRead(cursor.root().get_object())) {
    if (JsonRead(member.unescaped_key()) == name) {
      auto value = JsonRead(member.value());
      return std::string(JsonRead(simdjson::to_json_string(value)));
    }
  }
  throw std::invalid_argument(simdjson::error_message(simdjson::NO_SUCH_FIELD));
}

google::protobuf::Value JsonToValue(simdjson::dom::element value) {
  google::protobuf::Value result;
  using Type = simdjson::dom::element_type;
  switch (value.type()) {
    case Type::OBJECT:
      result.mutable_struct_value();
      for (const auto member : value.get_object()) {
        (*result.mutable_struct_value()->mutable_fields())[std::string(member.key)] = JsonToValue(member.value);
      }
      break;
    case Type::ARRAY:
      result.mutable_list_value();
      for (const auto element : value.get_array()) {
        *result.mutable_list_value()->add_values() = JsonToValue(element);
      }
      break;
    case Type::STRING:
      result.set_string_value(std::string(std::string_view(value.get_string())));
      break;
    case Type::BOOL:
      result.set_bool_value(value.get_bool());
      break;
    case Type::NULL_VALUE:
      result.set_null_value(google::protobuf::NULL_VALUE);
      break;
    default:
      result.set_number_value(value.get_double());
      break;
  }
  return result;
}

}  // namespace a2a::core::internal
