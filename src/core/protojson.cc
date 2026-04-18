#include "a2a/core/protojson.h"

#include <google/protobuf/util/json_util.h>

namespace a2a::core {

Result<std::string> MessageToJson(const google::protobuf::Message& message,
                                  const ProtoJsonWriteOptions& options) {
  google::protobuf::util::JsonPrintOptions print_options;
  print_options.add_whitespace = options.add_whitespace;
  print_options.always_print_primitive_fields = options.always_print_primitive_fields;
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

  google::protobuf::util::JsonParseOptions parse_options;
  parse_options.ignore_unknown_fields = options.ignore_unknown_fields;

  const auto status =
      google::protobuf::util::JsonStringToMessage(std::string(json), message, parse_options);
  if (!status.ok()) {
    return Error::Serialization(status.ToString());
  }

  return {};
}

}  // namespace a2a::core
