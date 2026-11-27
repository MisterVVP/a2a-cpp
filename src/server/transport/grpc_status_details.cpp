// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "server/transport/grpc_status_details.h"

#include <cstdint>

namespace a2a::server {
namespace {
constexpr auto kErrorDomain = "a2a-protocol.org";
constexpr auto kErrorInfoType = "type.googleapis.com/google.rpc.ErrorInfo";
int GrpcStatusCodeNumber(::grpc::StatusCode code) {
  constexpr int kStatusOk = 0;
  constexpr int kStatusCancelled = 1;
  constexpr int kStatusUnknown = 2;
  constexpr int kStatusInvalidArgument = 3;
  constexpr int kStatusDeadlineExceeded = 4;
  constexpr int kStatusNotFound = 5;
  constexpr int kStatusAlreadyExists = 6;
  constexpr int kStatusPermissionDenied = 7;
  constexpr int kStatusResourceExhausted = 8;
  constexpr int kStatusFailedPrecondition = 9;
  constexpr int kStatusAborted = 10;
  constexpr int kStatusOutOfRange = 11;
  constexpr int kStatusUnimplemented = 12;
  constexpr int kStatusInternal = 13;
  constexpr int kStatusUnavailable = 14;
  constexpr int kStatusDataLoss = 15;
  constexpr int kStatusUnauthenticated = 16;
  switch (code) {
    case ::grpc::StatusCode::OK:
      return kStatusOk;
    case ::grpc::StatusCode::CANCELLED:
      return kStatusCancelled;
    case ::grpc::StatusCode::UNKNOWN:
      return kStatusUnknown;
    case ::grpc::StatusCode::INVALID_ARGUMENT:
      return kStatusInvalidArgument;
    case ::grpc::StatusCode::DEADLINE_EXCEEDED:
      return kStatusDeadlineExceeded;
    case ::grpc::StatusCode::NOT_FOUND:
      return kStatusNotFound;
    case ::grpc::StatusCode::ALREADY_EXISTS:
      return kStatusAlreadyExists;
    case ::grpc::StatusCode::PERMISSION_DENIED:
      return kStatusPermissionDenied;
    case ::grpc::StatusCode::RESOURCE_EXHAUSTED:
      return kStatusResourceExhausted;
    case ::grpc::StatusCode::FAILED_PRECONDITION:
      return kStatusFailedPrecondition;
    case ::grpc::StatusCode::ABORTED:
      return kStatusAborted;
    case ::grpc::StatusCode::OUT_OF_RANGE:
      return kStatusOutOfRange;
    case ::grpc::StatusCode::UNIMPLEMENTED:
      return kStatusUnimplemented;
    case ::grpc::StatusCode::INTERNAL:
      return kStatusInternal;
    case ::grpc::StatusCode::UNAVAILABLE:
      return kStatusUnavailable;
    case ::grpc::StatusCode::DATA_LOSS:
      return kStatusDataLoss;
    case ::grpc::StatusCode::UNAUTHENTICATED:
      return kStatusUnauthenticated;
    case ::grpc::StatusCode::DO_NOT_USE:
      break;
  }
  return kStatusUnknown;
}

constexpr std::uint64_t kVarintContinuationBit = 0x80U;
constexpr std::uint64_t kVarintPayloadMask = 0x7FU;
constexpr std::uint32_t kVarintShiftBits = 7U;

void AppendVarint(std::string& out, std::uint64_t value) {
  while (value >= kVarintContinuationBit) {
    out.push_back(static_cast<char>((value & kVarintPayloadMask) | kVarintContinuationBit));
    value >>= kVarintShiftBits;
  }
  out.push_back(static_cast<char>(value));
}

void AppendTag(std::string& out, std::uint32_t field_number, std::uint32_t wire_type) {
  AppendVarint(out, (static_cast<std::uint64_t>(field_number) << 3U) | wire_type);
}

void AppendLengthDelimited(std::string& out, std::uint32_t field_number, std::string_view value) {
  AppendTag(out, field_number, 2U);
  AppendVarint(out, static_cast<std::uint64_t>(value.size()));
  out.append(value);
}

std::string SerializeErrorInfo(std::string_view reason) {
  std::string message;
  AppendLengthDelimited(message, 1U, reason);
  AppendLengthDelimited(message, 2U, kErrorDomain);
  return message;
}

std::string SerializeAny(std::string_view type_url, std::string_view value) {
  std::string message;
  AppendLengthDelimited(message, 1U, type_url);
  AppendLengthDelimited(message, 2U, value);
  return message;
}

}  // namespace

std::string SerializeGrpcStatusDetails(::grpc::StatusCode code, std::string_view error_message,
                                       std::string_view reason) {
  std::string message;
  AppendTag(message, 1U, 0U);
  AppendVarint(message, static_cast<std::uint64_t>(GrpcStatusCodeNumber(code)));
  AppendLengthDelimited(message, 2U, error_message);
  AppendLengthDelimited(message, 3U, SerializeAny(kErrorInfoType, SerializeErrorInfo(reason)));
  return message;
}

}  // namespace a2a::server
