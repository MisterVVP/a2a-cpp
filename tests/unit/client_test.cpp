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
}

}  // namespace
