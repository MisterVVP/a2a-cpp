#include <gtest/gtest.h>

#include "a2a/v1/a2a.grpc.pb.h"
#include "a2a/v1/a2a.pb.h"

TEST(GeneratedProtoSmokeTest, CanInstantiateGeneratedMessage) {
  lf::a2a::v1::Task task;
  task.set_id("task-123");
  task.mutable_status()->set_state(lf::a2a::v1::TASK_STATE_WORKING);

  EXPECT_EQ(task.id(), "task-123");
  EXPECT_EQ(task.status().state(), lf::a2a::v1::TASK_STATE_WORKING);
}

TEST(GeneratedProtoSmokeTest, GeneratedGrpcServiceIsUsable) {
  const auto* service_descriptor = lf::a2a::v1::A2AService::service_full_name();
  ASSERT_NE(service_descriptor, nullptr);
  EXPECT_STREQ(service_descriptor, "lf.a2a.v1.A2AService");
}
