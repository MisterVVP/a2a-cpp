// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "server/transport/grpc_request_context.h"

#include <grpcpp/create_channel.h>
#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "a2a/core/protocol_codes.h"
#include "a2a/core/version.h"

#if __has_include(<grpcpp/test/server_context_test_spouse.h>)
#include <grpcpp/test/server_context_test_spouse.h>

namespace {
using namespace a2a;
constexpr auto kUnusedTarget = "127.0.0.1:1";
constexpr auto kRequiredExtension = "urn:a2a:test:grpc-context";
constexpr auto kExtensionsKey = "a2a-extensions";
constexpr auto kAuthorizationKey = "authorization";
constexpr auto kAuthorizationValue = "Bearer context-token";
constexpr auto kBearerKey = "bearer_token";
constexpr auto kToken = "context-token";
constexpr auto kRequestIdKey = "x-request-id";
constexpr auto kRequestId = "grpc-context-request";

class GrpcRequestContextTest : public ::testing::Test {
 private:
  std::shared_ptr<::grpc::Channel> runtime_ =
      ::grpc::CreateChannel(kUnusedTarget, ::grpc::InsecureChannelCredentials());
};

TEST_F(GrpcRequestContextTest, OwnsValidatedVersionAuthAndCustomHeaders) {
  ::grpc::ServerContext context;
  ::grpc::testing::ServerContextTestSpouse spouse(&context);
  spouse.AddClientMetadata(std::string(core::Version::kHeaderName), core::Version::HeaderValue());
  spouse.AddClientMetadata(kExtensionsKey, kRequiredExtension);
  spouse.AddClientMetadata(kAuthorizationKey, kAuthorizationValue);
  spouse.AddClientMetadata(kRequestIdKey, kRequestId);
  const auto result =
      server::BuildGrpcRequestContext(context, server::RequiredExtensionsValidator({kRequiredExtension}));
  ASSERT_TRUE(result.ok());
  const auto& metadata = result.value().request_context;
  EXPECT_EQ(metadata.protocol_version, core::Version::kProtocolVersion);
  EXPECT_EQ(metadata.client_headers.at(kRequestIdKey), kRequestId);
  EXPECT_EQ(metadata.auth_metadata.at(kBearerKey), kToken);
  EXPECT_EQ(result.value().activated_extensions, std::vector<std::string>{kRequiredExtension});
}

TEST_F(GrpcRequestContextTest, RejectsMissingVersionBeforeDispatch) {
  ::grpc::ServerContext context;
  const auto result = server::BuildGrpcRequestContext(context, server::RequiredExtensionsValidator{});
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().code(), core::ErrorCode::kUnsupportedVersion);
}

TEST_F(GrpcRequestContextTest, RejectsMissingRequiredExtensionBeforeDispatch) {
  ::grpc::ServerContext context;
  ::grpc::testing::ServerContextTestSpouse spouse(&context);
  spouse.AddClientMetadata(std::string(core::Version::kHeaderName), core::Version::HeaderValue());
  const auto result =
      server::BuildGrpcRequestContext(context, server::RequiredExtensionsValidator({kRequiredExtension}));
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().protocol_code(), core::protocol_codes::kExtensionSupportRequired);
}
}  // namespace
#endif
