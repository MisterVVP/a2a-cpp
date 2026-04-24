#include <gtest/gtest.h>

#include <string>
#include <unordered_map>

#include "a2a/core/version.h"
#include "a2a/server/rest_transport.h"
#include "a2a/server/server.h"

namespace {

constexpr int kHttpStatusBadRequest = 400;
constexpr int kHttpStatusOk = 200;

class RestAdapterExecutor final : public a2a::server::AgentExecutor {
 public:
  a2a::core::Result<lf::a2a::v1::SendMessageResponse> SendMessage(
      const lf::a2a::v1::SendMessageRequest& request,
      a2a::server::RequestContext& context) override {
    (void)context;
    if (request.message().role().empty()) {
      return a2a::core::Error::Validation("message.role is required");
    }

    lf::a2a::v1::SendMessageResponse response;
    response.mutable_message()->set_role("assistant");
    return response;
  }

  a2a::core::Result<std::unique_ptr<a2a::server::ServerStreamSession>> SendStreamingMessage(
      const lf::a2a::v1::SendMessageRequest& request,
      a2a::server::RequestContext& context) override {
    (void)request;
    (void)context;
    return a2a::core::Error::Validation("streaming unsupported in test executor");
  }

  a2a::core::Result<lf::a2a::v1::Task> GetTask(const lf::a2a::v1::GetTaskRequest& request,
                                               a2a::server::RequestContext& context) override {
    (void)request;
    (void)context;
    return a2a::core::Error::Validation("not implemented");
  }

  a2a::core::Result<a2a::server::ListTasksResponse> ListTasks(
      const a2a::server::ListTasksRequest& request, a2a::server::RequestContext& context) override {
    (void)request;
    (void)context;
    return a2a::core::Error::Validation("not implemented");
  }

  a2a::core::Result<lf::a2a::v1::Task> CancelTask(const lf::a2a::v1::CancelTaskRequest& request,
                                                  a2a::server::RequestContext& context) override {
    (void)request;
    (void)context;
    return a2a::core::Error::Validation("not implemented");
  }
};

a2a::server::DispatchRequest MakeSendMessageDispatchRequest() {
  lf::a2a::v1::SendMessageRequest request;
  request.mutable_message()->set_role("user");
  return {.operation = a2a::server::DispatcherOperation::kSendMessage, .payload = request};
}

TEST(RestAdapterTest, MissingVersionHeaderReturnsProtocolValidationError) {
  RestAdapterExecutor executor;
  a2a::server::Dispatcher dispatcher(&executor);
  a2a::server::RestAdapter adapter(&dispatcher);
  a2a::server::RequestContext context;

  const a2a::server::RestPipelineResponse response =
      adapter.Handle(MakeSendMessageDispatchRequest(), {}, context);

  EXPECT_EQ(response.status_code, kHttpStatusBadRequest);
  if (!response.error.has_value()) {
    GTEST_FAIL() << "Expected protocol error for missing A2A-Version header";
  }
  const a2a::core::Error& error = *response.error;
  EXPECT_EQ(error.code(), a2a::core::ErrorCode::kValidation);
  EXPECT_EQ(error.http_status().value_or(0), kHttpStatusBadRequest);
}

TEST(RestAdapterTest, UnsupportedVersionHeaderReturnsProtocolVersionError) {
  RestAdapterExecutor executor;
  a2a::server::Dispatcher dispatcher(&executor);
  a2a::server::RestAdapter adapter(&dispatcher);
  a2a::server::RequestContext context;

  const std::unordered_map<std::string, std::string> headers = {
      {std::string(a2a::core::Version::kHeaderName), "2.0"}};
  const a2a::server::RestPipelineResponse response =
      adapter.Handle(MakeSendMessageDispatchRequest(), headers, context);

  EXPECT_EQ(response.status_code, kHttpStatusBadRequest);
  if (!response.error.has_value()) {
    GTEST_FAIL() << "Expected protocol error for unsupported A2A-Version header";
  }
  const a2a::core::Error& error = *response.error;
  EXPECT_EQ(error.code(), a2a::core::ErrorCode::kUnsupportedVersion);
  EXPECT_EQ(error.protocol_code(), std::optional<std::string>("2.0"));
  EXPECT_EQ(error.http_status().value_or(0), kHttpStatusBadRequest);
}

TEST(RestAdapterTest, SupportedVersionHeaderSetsVersionOnSuccessfulResponse) {
  RestAdapterExecutor executor;
  a2a::server::Dispatcher dispatcher(&executor);
  a2a::server::RestAdapter adapter(&dispatcher);
  a2a::server::RequestContext context;

  const std::unordered_map<std::string, std::string> headers = {
      {std::string(a2a::core::Version::kHeaderName),
       std::string(a2a::core::Version::kProtocolVersion)}};
  const a2a::server::RestPipelineResponse response =
      adapter.Handle(MakeSendMessageDispatchRequest(), headers, context);

  EXPECT_EQ(response.status_code, kHttpStatusOk);
  EXPECT_FALSE(response.error.has_value());
  EXPECT_TRUE(response.payload.has_value());
  ASSERT_TRUE(response.headers.contains(std::string(a2a::core::Version::kHeaderName)));
  EXPECT_EQ(response.headers.at(std::string(a2a::core::Version::kHeaderName)),
            a2a::core::Version::HeaderValue());
}

}  // namespace
