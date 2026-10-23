// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include <google/protobuf/struct.pb.h>
#include <google/protobuf/util/json_util.h>
#include <google/protobuf/util/message_differencer.h>
#include <google/protobuf/wrappers.pb.h>
#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "a2a/core/protojson.h"
#include "a2a/v1/a2a.pb.h"

namespace {

constexpr int kParserThreads = 4;
constexpr int kParserIterations = 50;
constexpr std::string_view kParserTask = R"({"id":"thread-task","metadata":{"nested":{"value":[false,1]}}})";

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
  const std::string expected = ReadFixture("tests/fixtures/protojson/send_message_request_golden.json");
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

template <typename T, std::size_t N>
void CheckParserParity(const std::array<std::string_view, N>& fixtures) {
  for (const auto json : fixtures) {
    SCOPED_TRACE(json);
    T expected;
    T actual;
    const auto reference = google::protobuf::util::JsonStringToMessage(std::string(json), &expected);
    const auto parsed = a2a::core::JsonToMessage(json, &actual);
    ASSERT_EQ(parsed.ok(), reference.ok()) << (parsed.ok() ? reference.ToString() : parsed.error().message());
    if (parsed.ok()) {
      EXPECT_TRUE(google::protobuf::util::MessageDifferencer::Equals(expected, actual));
    }
  }
}

TEST(CoreProtoJsonGoldenTest, PreservesProtobufSemantics) {
  CheckParserParity<lf::a2a::v1::GetTaskRequest>(std::to_array<std::string_view>(
      {R"({"id":"t","history_length":3})", R"({"id":"t","historyLength":"3"})", R"({"historyLength":3e1})",
       R"({"i\u0064":"escaped","history\u004cength":3})", R"({"historyLength":2147483647.0000001})",
       R"({"historyLength":2147483648})", R"({"historyLength":null})"}));
  CheckParserParity<lf::a2a::v1::Task>(std::to_array<std::string_view>(
      {R"({"id":"t","status":{"state":"TASK_STATE_WORKING","timestamp":"2026-10-22T12:34:56.123Z"}})",
       R"({"status":{"state":2},"metadata":{"null":null,"list":[1,true,"text",{}]}})", R"({"history":[null]})",
       R"({"status":{"state":"UNKNOWN_ENUM"}})", R"({"unknown":true})"}));
  CheckParserParity<lf::a2a::v1::Part>(std::to_array<std::string_view>(
      {R"({"text":"hello"})", R"({"raw":"AP8="})", R"({"raw":"AP8"})", R"({"raw":"-__v"})", R"({"raw":"!"})",
       R"({"text":"hello","url":"https://example.test/file"})"}));
  CheckParserParity<lf::a2a::v1::AgentCard>(std::to_array<std::string_view>(
      {R"({"name":"card","securitySchemes":{"be\u0061rer":{"httpAuthSecurityScheme":{"scheme":"bearer"}}}})",
       R"({"capabilities":{"streaming":true},"securityRequirements":[{"schemes":{"bearer":{"list":["read"]}}}]})"}));
  CheckParserParity<google::protobuf::Int64Value>(std::to_array<std::string_view>(
      {R"("9223372036854775807")", "9007199254740993", "9007199254740993.0", "9223372036854775808"}));
  CheckParserParity<google::protobuf::Value>(
      std::to_array<std::string_view>({"null", "true", "9007199254740993", R"({"n":null,"array":[1,"two",{}]})"}));
}

TEST(CoreProtoJsonGoldenTest, RejectsMalformedAndAmbiguousProtocolObjects) {
  constexpr auto kInvalid = std::to_array<std::string_view>(
      {R"({"id":"t"} {})", R"({"id":"t",})", R"({"status":{"state":tru}})", R"({"metadata":{"value":01}})",
       R"({"id":"\uD800"})", R"({"\uD800":true})", R"({"metadata":{"value":1e400}})", R"({"id":"first","id":"second"})",
       R"({"id":null})", R"({"status":{"state":1,"state":2}})", R"({"history":[null]})"});
  constexpr a2a::core::ProtoJsonParseOptions kStrict{
      .ignore_unknown_fields = true, .reject_top_level_null_fields = true, .reject_duplicate_top_level_fields = true};
  for (const auto json : kInvalid) {
    lf::a2a::v1::Task task;
    EXPECT_FALSE(a2a::core::JsonToMessage(json, &task, kStrict).ok()) << json;
  }
  constexpr auto kBadAliases = R"({"historyLength":1,"history_length":2})";
  lf::a2a::v1::GetTaskRequest request;
  EXPECT_FALSE(a2a::core::JsonToMessage(kBadAliases, &request, kStrict).ok());
  constexpr auto kInvalidUtf8 = "{\"id\":\"\xff\"}";
  lf::a2a::v1::Task task;
  EXPECT_FALSE(a2a::core::JsonToMessage(kInvalidUtf8, &task).ok());
  constexpr std::size_t kExcessiveDepth = 129U;
  std::string nested(kExcessiveDepth, '[');
  nested.append(kExcessiveDepth, ']');
  google::protobuf::Value value;
  EXPECT_FALSE(a2a::core::JsonToMessage(nested, &value).ok());
  constexpr std::size_t kOversizedBytes = (64U * 1024U * 1024U) + 1U;
  const std::string oversized(kOversizedBytes, ' ');
  EXPECT_FALSE(a2a::core::JsonToMessage(oversized, &value).ok());
}

TEST(CoreProtoJsonGoldenTest, ParserReuseKeepsResultsIndependentAcrossThreads) {
  std::atomic<bool> success{true};
  std::vector<std::thread> workers;
  workers.reserve(static_cast<std::size_t>(kParserThreads));
  for (int index = 0; index < kParserThreads; ++index) {
    workers.emplace_back([&success] {
      for (int iteration = 0; iteration < kParserIterations; ++iteration) {
        lf::a2a::v1::Task task;
        lf::a2a::v1::Task other;
        if (!a2a::core::JsonToMessage(kParserTask, &task).ok() || !a2a::core::JsonToMessage(kParserTask, &other).ok() ||
            task.id() != other.id()) {
          success = false;
        }
      }
    });
  }
  for (auto& worker : workers) {
    worker.join();
  }
  EXPECT_TRUE(success);
}

}  // namespace
