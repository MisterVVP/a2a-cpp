#include <gtest/gtest.h>

#include "a2a/core/error.h"
#include "a2a/core/extensions.h"
#include "a2a/core/protojson.h"
#include "a2a/core/result.h"
#include "a2a/core/version.h"
#include "a2a/v1/a2a.pb.h"

namespace {

TEST(CoreVersionTest, EmitsAndValidatesA2AVersion10) {
  EXPECT_EQ(a2a::core::Version::kHeaderName, "A2A-Version");
  EXPECT_EQ(a2a::core::Version::HeaderValue(), "1.0");
  EXPECT_TRUE(a2a::core::Version::IsSupported("1.0"));
  EXPECT_FALSE(a2a::core::Version::IsSupported("2.0"));
}

TEST(CoreExtensionsTest, FormatSortsAndDeduplicatesExtensionsDeterministically) {
  const std::vector<std::string> input = {"streaming", "auth", "streaming", ""};
  EXPECT_EQ(a2a::core::Extensions::Format(input), "auth,streaming");
}

TEST(CoreExtensionsTest, ParseHandlesWhitespaceAndValidation) {
  const auto parsed = a2a::core::Extensions::Parse("  streaming , auth,streaming  ");
  ASSERT_TRUE(parsed.ok());
  EXPECT_EQ(parsed.value(), (std::vector<std::string>{"auth", "streaming"}));

  const auto invalid = a2a::core::Extensions::Parse("valid,bad/token");
  ASSERT_FALSE(invalid.ok());
  EXPECT_EQ(invalid.error().code(), a2a::core::ErrorCode::kValidation);
}

TEST(CoreErrorTest, ErrorCarriesTransportAndProtocolContext) {
  const auto error = a2a::core::Error::RemoteProtocol("Request rejected")
                         .WithTransport("http")
                         .WithProtocolCode("invalid_request")
                         .WithHttpStatus(400);

  EXPECT_EQ(error.code(), a2a::core::ErrorCode::kRemoteProtocol);
  EXPECT_EQ(error.message(), "Request rejected");
  const auto transport = error.transport();
  ASSERT_TRUE(transport.has_value());
  EXPECT_EQ(*transport, "http");

  const auto protocol_code = error.protocol_code();
  ASSERT_TRUE(protocol_code.has_value());
  EXPECT_EQ(*protocol_code, "invalid_request");

  const auto http_status = error.http_status();
  ASSERT_TRUE(http_status.has_value());
  EXPECT_EQ(*http_status, 400);
}

TEST(CoreErrorTest, SupportsAllCoreErrorCategories) {
  const auto validation = a2a::core::Error::Validation("invalid payload");
  EXPECT_EQ(validation.code(), a2a::core::ErrorCode::kValidation);

  const auto unsupported_version = a2a::core::Error::UnsupportedVersion("Unsupported A2A version");
  EXPECT_EQ(unsupported_version.code(), a2a::core::ErrorCode::kUnsupportedVersion);

  const auto network = a2a::core::Error::Network("network unavailable");
  EXPECT_EQ(network.code(), a2a::core::ErrorCode::kNetwork);

  const auto remote_protocol = a2a::core::Error::RemoteProtocol("remote validation failed");
  EXPECT_EQ(remote_protocol.code(), a2a::core::ErrorCode::kRemoteProtocol);

  const auto serialization = a2a::core::Error::Serialization("failed to parse json");
  EXPECT_EQ(serialization.code(), a2a::core::ErrorCode::kSerialization);
}

TEST(CoreResultTest, WorksForValuesAndErrors) {
  constexpr int kExpectedValue = 7;
  a2a::core::Result<int> ok_result = kExpectedValue;
  ASSERT_TRUE(ok_result.ok());
  EXPECT_EQ(ok_result.value(), kExpectedValue);

  a2a::core::Result<int> error_result = a2a::core::Error::Network("connection dropped");
  ASSERT_FALSE(error_result.ok());
  EXPECT_EQ(error_result.error().code(), a2a::core::ErrorCode::kNetwork);

  a2a::core::Result<void> void_ok;
  EXPECT_TRUE(void_ok.ok());

  a2a::core::Result<void> void_error = a2a::core::Error::Serialization("bad payload");
  ASSERT_FALSE(void_error.ok());
  EXPECT_EQ(void_error.error().code(), a2a::core::ErrorCode::kSerialization);
}

TEST(CoreProtoJsonTest, CanRoundTripRepresentativeA2AMessage) {
  lf::a2a::v1::Task task;
  task.set_id("task-001");
  task.mutable_status()->set_state(lf::a2a::v1::TASK_STATE_WORKING);

  const auto json = a2a::core::MessageToJson(task);
  ASSERT_TRUE(json.ok()) << json.error().message();

  lf::a2a::v1::Task parsed_task;
  const auto parse_status = a2a::core::JsonToMessage(json.value(), &parsed_task);
  ASSERT_TRUE(parse_status.ok()) << parse_status.error().message();

  EXPECT_EQ(parsed_task.id(), "task-001");
  EXPECT_EQ(parsed_task.status().state(), lf::a2a::v1::TASK_STATE_WORKING);
}

TEST(CoreProtoJsonTest, EnumSerializationUsesNamesByDefault) {
  lf::a2a::v1::Task task;
  task.mutable_status()->set_state(lf::a2a::v1::TASK_STATE_FAILED);

  const auto json = a2a::core::MessageToJson(task);
  ASSERT_TRUE(json.ok());
  EXPECT_NE(json.value().find("\"TASK_STATE_FAILED\""), std::string::npos);
}

TEST(CoreProtoJsonTest, RejectsUnknownFieldsByDefault) {
  lf::a2a::v1::Task task;
  const auto parse_status =
      a2a::core::JsonToMessage(R"({"id":"task-2","unknownField":true})", &task);
  ASSERT_FALSE(parse_status.ok());
  EXPECT_EQ(parse_status.error().code(), a2a::core::ErrorCode::kSerialization);
}

TEST(CoreProtoJsonTest, CanIgnoreUnknownFieldsWhenRequested) {
  lf::a2a::v1::Task task;
  a2a::core::ProtoJsonParseOptions parse_options;
  parse_options.ignore_unknown_fields = true;

  const auto parse_status =
      a2a::core::JsonToMessage(R"({"id":"task-3","unknownField":true})", &task, parse_options);
  ASSERT_TRUE(parse_status.ok());
  EXPECT_EQ(task.id(), "task-3");
}

TEST(CoreProtoJsonTest, NullMessageTargetReturnsValidationError) {
  const auto parse_status = a2a::core::JsonToMessage("{}", nullptr);
  ASSERT_FALSE(parse_status.ok());
  EXPECT_EQ(parse_status.error().code(), a2a::core::ErrorCode::kValidation);
}

}  // namespace
