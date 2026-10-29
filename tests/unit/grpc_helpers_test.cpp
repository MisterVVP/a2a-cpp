// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include <google/protobuf/any.pb.h>
#include <google/protobuf/unknown_field_set.h>
#include <gtest/gtest.h>

#include <array>
#include <string>
#include <string_view>

#include "a2a/core/http_constants.h"
#include "a2a/core/protocol_errors.h"
#include "server/transport/grpc_error.h"
#include "server/transport/grpc_status_details.h"
#include "server/transport/grpc_task_conversion.h"

namespace {
using namespace a2a;
constexpr auto kMessage = "test error";
constexpr auto kTaskId = "task-grpc-helpers";
constexpr auto kPageToken = "page-grpc-helpers";
constexpr auto kContextId = "context-grpc-helpers";
constexpr auto kErrorInfoType = "type.googleapis.com/google.rpc.ErrorInfo";
constexpr auto kErrorDomain = "a2a-protocol.org";
constexpr auto kTaskNotFoundReason = "TASK_NOT_FOUND";
constexpr auto kCancelledReason = "INTERNAL_ERROR";
constexpr std::size_t kLongMessageLength = 512;
constexpr int kStatusFieldCount = 3;
constexpr int kErrorInfoFieldCount = 2;
constexpr int kStatusNotFoundNumber = 5;
constexpr int kPageSize = 7;
constexpr int kHistoryLength = 3;
constexpr int kTotalSize = 11;
constexpr int kTimestampSeconds = 123;
constexpr int kInvalidHistoryLength = -1;

struct ErrorCase {
  core::Error error;
  ::grpc::StatusCode status;
  std::string_view name;
  std::string_view reason;
};
const std::array kErrorCases{ErrorCase{.error = core::Error::Validation(kMessage),
                                       .status = ::grpc::StatusCode::INVALID_ARGUMENT,
                                       .name = "validation",
                                       .reason = "VALIDATION_ERROR"},
                             ErrorCase{.error = core::Error::UnsupportedVersion(kMessage),
                                       .status = ::grpc::StatusCode::UNIMPLEMENTED,
                                       .name = "unsupported_version",
                                       .reason = "VERSION_NOT_SUPPORTED"},
                             ErrorCase{.error = core::Error::Network(kMessage),
                                       .status = ::grpc::StatusCode::UNAVAILABLE,
                                       .name = "network",
                                       .reason = "NETWORK_ERROR"},
                             ErrorCase{.error = core::Error::RemoteProtocol(kMessage),
                                       .status = ::grpc::StatusCode::FAILED_PRECONDITION,
                                       .name = "remote_protocol",
                                       .reason = "REMOTE_PROTOCOL_ERROR"},
                             ErrorCase{.error = core::Error::Serialization(kMessage),
                                       .status = ::grpc::StatusCode::INTERNAL,
                                       .name = "serialization",
                                       .reason = "SERIALIZATION_ERROR"},
                             ErrorCase{.error = core::Error::Internal(kMessage),
                                       .status = ::grpc::StatusCode::INTERNAL,
                                       .name = "internal",
                                       .reason = "INTERNAL_ERROR"}};

void CheckErrorClassification(const ErrorCase& entry) {
  EXPECT_EQ(server::GrpcStatusCode(entry.error), entry.status);
  EXPECT_EQ(server::GrpcErrorCodeName(entry.error.code()), entry.name);
  EXPECT_EQ(server::GrpcErrorInfoReason(entry.error), entry.reason);
  const auto status = server::GrpcErrorStatus(entry.error, nullptr);
  EXPECT_EQ(status.error_code(), entry.status);
  EXPECT_EQ(status.error_message(), kMessage);
}
TEST(GrpcErrorTest, ClassifiesEveryCoreErrorWithoutAServerContext) {
  for (const auto& entry : kErrorCases) {
    SCOPED_TRACE(entry.name);
    CheckErrorClassification(entry);
  }
}

TEST(GrpcErrorTest, PreservesProtocolAndAuthenticationStatusMapping) {
  EXPECT_EQ(server::GrpcStatusCode(core::protocol_errors::TaskNotFound()), ::grpc::StatusCode::NOT_FOUND);
  EXPECT_EQ(server::GrpcStatusCode(core::protocol_errors::TaskNotCancelable()),
            ::grpc::StatusCode::FAILED_PRECONDITION);
  EXPECT_EQ(server::GrpcStatusCode(core::protocol_errors::PushNotificationNotSupported()),
            ::grpc::StatusCode::UNIMPLEMENTED);
  EXPECT_EQ(server::GrpcStatusCode(core::protocol_errors::UnsupportedOperation()), ::grpc::StatusCode::UNIMPLEMENTED);
}

TEST(GrpcErrorTest, PreservesAuthenticationStatusMapping) {
  EXPECT_EQ(server::GrpcStatusCode(core::protocol_errors::Unauthenticated()), ::grpc::StatusCode::UNAUTHENTICATED);
  EXPECT_EQ(server::GrpcStatusCode(core::protocol_errors::PermissionDenied()), ::grpc::StatusCode::PERMISSION_DENIED);
}

TEST(GrpcErrorTest, LifecycleStatusOverrideRetainsApplicationErrorMessage) {
  const auto status =
      server::GrpcErrorStatus(core::Error::Internal(kMessage), nullptr, {}, ::grpc::StatusCode::DEADLINE_EXCEEDED);
  EXPECT_EQ(status.error_code(), ::grpc::StatusCode::DEADLINE_EXCEEDED);
  EXPECT_EQ(status.error_message(), kMessage);
}

void CheckErrorInfoFields(const google::protobuf::UnknownFieldSet& info) {
  EXPECT_EQ(info.field(0).number(), 1);
  EXPECT_EQ(info.field(0).length_delimited(), kTaskNotFoundReason);
  EXPECT_EQ(info.field(1).number(), 2);
  EXPECT_EQ(info.field(1).length_delimited(), kErrorDomain);
}

void CheckErrorInfo(const std::string& encoded) {
  google::protobuf::Any detail;
  ASSERT_TRUE(detail.ParseFromString(encoded));
  EXPECT_EQ(detail.type_url(), kErrorInfoType);
  google::protobuf::UnknownFieldSet info;
  ASSERT_TRUE(info.ParseFromString(detail.value()));
  ASSERT_EQ(info.field_count(), kErrorInfoFieldCount);
  CheckErrorInfoFields(info);
}

TEST(GrpcStatusDetailsTest, EncodesStatusMessageAndTypedErrorInfoWithLongLengths) {
  const std::string message(kLongMessageLength, 'x');
  const auto encoded = server::SerializeGrpcStatusDetails(::grpc::StatusCode::NOT_FOUND, message, kTaskNotFoundReason);
  google::protobuf::UnknownFieldSet status;
  ASSERT_TRUE(status.ParseFromString(encoded));
  ASSERT_EQ(status.field_count(), kStatusFieldCount);
  EXPECT_EQ(status.field(0).number(), 1);
  EXPECT_EQ(status.field(0).varint(), kStatusNotFoundNumber);
  EXPECT_EQ(status.field(1).number(), 2);
  EXPECT_EQ(status.field(1).length_delimited(), message);
  EXPECT_EQ(status.field(2).number(), 3);
  CheckErrorInfo(status.field(2).length_delimited());
}

TEST(GrpcStatusDetailsTest, EncodesCancellationStatusIndependentlyOfErrorClassification) {
  const auto encoded = server::SerializeGrpcStatusDetails(::grpc::StatusCode::CANCELLED, kMessage, kCancelledReason);
  google::protobuf::UnknownFieldSet status;
  ASSERT_TRUE(status.ParseFromString(encoded));
  ASSERT_EQ(status.field_count(), kStatusFieldCount);
  EXPECT_EQ(status.field(0).varint(), static_cast<unsigned>(::grpc::StatusCode::CANCELLED));
}

TEST(GrpcTaskConversionTest, PreservesOptionalListFieldsAndRejectsInvalidHistory) {
  lf::a2a::v1::ListTasksRequest request;
  request.set_page_size(kPageSize);
  request.set_page_token(kPageToken);
  request.set_context_id(kContextId);
  request.set_status(lf::a2a::v1::TASK_STATE_WORKING);
  request.set_history_length(kHistoryLength);
  request.set_include_artifacts(false);
  request.mutable_status_timestamp_after()->set_seconds(kTimestampSeconds);
  const auto parsed = server::ParseGrpcListTasksRequest(request);
  ASSERT_TRUE(parsed.ok());
  EXPECT_EQ(parsed.value().page_size, kPageSize);
  EXPECT_EQ(parsed.value().page_token, kPageToken);
  EXPECT_EQ(parsed.value().context_id, kContextId);
  EXPECT_EQ(parsed.value().status_filter, lf::a2a::v1::TASK_STATE_WORKING);
  EXPECT_EQ(parsed.value().history_length, kHistoryLength);
  EXPECT_FALSE(parsed.value().include_artifacts);
  ASSERT_TRUE(parsed.value().status_timestamp_after.has_value());
  EXPECT_EQ(parsed.value().status_timestamp_after.value_or(google::protobuf::Timestamp{}).seconds(), kTimestampSeconds);
  request.set_history_length(kInvalidHistoryLength);
  EXPECT_FALSE(server::ParseGrpcListTasksRequest(request).ok());
}

TEST(GrpcTaskConversionTest, WritesTasksAndPaginationWithoutChangingPayload) {
  server::ListTasksResponse payload;
  payload.tasks.emplace_back().set_id(kTaskId);
  payload.page_size = kPageSize;
  payload.total_size = kTotalSize;
  payload.next_page_token = kPageToken;
  lf::a2a::v1::ListTasksResponse response;
  server::WriteGrpcListTasksResponse(payload, response);
  ASSERT_EQ(response.tasks_size(), 1);
  EXPECT_EQ(response.tasks(0).id(), kTaskId);
  EXPECT_EQ(payload.tasks.front().id(), kTaskId);
  EXPECT_EQ(response.page_size(), kPageSize);
  EXPECT_EQ(response.total_size(), kTotalSize);
  EXPECT_EQ(response.next_page_token(), kPageToken);
}
}  // namespace
