#include "a2a/client/client.h"

#include <gtest/gtest.h>

namespace {

TEST(A2AClientTest, ReturnsInternalErrorWhenTransportNotConfigured) {
  a2a::client::A2AClient client(nullptr);

  lf::a2a::v1::GetTaskRequest request;
  request.set_id("t-1");

  const auto response = client.GetTask(request);
  ASSERT_FALSE(response.ok());
  EXPECT_EQ(response.error().code(), a2a::core::ErrorCode::kInternal);

  class NoopObserver final : public a2a::client::StreamObserver {
   public:
    void OnEvent(const lf::a2a::v1::StreamResponse& response) override { (void)response; }
    void OnError(const a2a::core::Error& error) override { (void)error; }
    void OnCompleted() override {}
  } observer;

  lf::a2a::v1::SendMessageRequest stream_request;
  stream_request.mutable_message()->set_role("user");
  const auto stream_response = client.SendStreamingMessage(stream_request, observer);
  ASSERT_FALSE(stream_response.ok());
  EXPECT_EQ(stream_response.error().code(), a2a::core::ErrorCode::kInternal);
}

}  // namespace
