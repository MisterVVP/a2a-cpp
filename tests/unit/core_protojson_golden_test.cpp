#include <gtest/gtest.h>

#include <fstream>
#include <iterator>
#include <string>

#include "a2a/core/protojson.h"
#include "a2a/v1/a2a.pb.h"

namespace {

std::string ReadFixture(const std::string& relative_path) {
  std::ifstream fixture(std::string(A2A_SOURCE_DIR) + "/" + relative_path);
  EXPECT_TRUE(fixture.is_open()) << "missing fixture: " << relative_path;
  std::string contents((std::istreambuf_iterator<char>(fixture)), std::istreambuf_iterator<char>());
  while (!contents.empty() && (contents.back() == '\n' || contents.back() == '\r')) {
    contents.pop_back();
  }
  return contents;
}

TEST(CoreProtoJsonGoldenTest, TaskMatchesGoldenFixture) {
  const std::string expected = ReadFixture("tests/fixtures/protojson/task_golden.json");
  lf::a2a::v1::Task task;

  const auto parse = a2a::core::JsonToMessage(expected, &task);
  ASSERT_TRUE(parse.ok()) << parse.error().message();

  const auto encoded = a2a::core::MessageToJson(task);
  ASSERT_TRUE(encoded.ok()) << encoded.error().message();
  EXPECT_EQ(encoded.value(), expected);
}

TEST(CoreProtoJsonGoldenTest, SendMessageRequestMatchesGoldenFixture) {
  const std::string expected =
      ReadFixture("tests/fixtures/protojson/send_message_request_golden.json");
  lf::a2a::v1::SendMessageRequest request;

  const auto parse = a2a::core::JsonToMessage(expected, &request);
  ASSERT_TRUE(parse.ok()) << parse.error().message();

  const auto encoded = a2a::core::MessageToJson(request);
  ASSERT_TRUE(encoded.ok()) << encoded.error().message();
  EXPECT_EQ(encoded.value(), expected);
}

TEST(CoreProtoJsonGoldenTest, StreamResponseMatchesGoldenFixture) {
  const std::string expected = ReadFixture("tests/fixtures/protojson/stream_response_golden.json");
  lf::a2a::v1::StreamResponse response;

  const auto parse = a2a::core::JsonToMessage(expected, &response);
  ASSERT_TRUE(parse.ok()) << parse.error().message();

  const auto encoded = a2a::core::MessageToJson(response);
  ASSERT_TRUE(encoded.ok()) << encoded.error().message();
  EXPECT_EQ(encoded.value(), expected);
}

}  // namespace
