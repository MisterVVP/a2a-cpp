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
#include "core/json.h"

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
void CheckParserParity(const std::array<std::string_view, N>& fixtures, std::string_view expected_json = {}) {
  for (const auto json : fixtures) {
    SCOPED_TRACE(json);
    T expected;
    T actual;
    const auto reference = google::protobuf::util::JsonStringToMessage(
        std::string(expected_json.empty() ? json : expected_json), &expected);
    const auto parsed = a2a::core::JsonToMessage(json, &actual);
    ASSERT_EQ(parsed.ok(), reference.ok()) << (parsed.ok() ? reference.ToString() : parsed.error().message());
    if (parsed.ok()) {
      EXPECT_TRUE(google::protobuf::util::MessageDifferencer::Equals(expected, actual));
    }
  }
}

TEST(CoreProtoJsonGoldenTest, PreservesProtobufSemantics) {
  CheckParserParity<lf::a2a::v1::Task>(
      std::to_array<std::string_view>({R"({"history":[{"messageId":"first"}],"history":[{"messageId":"last"}]})"}),
      R"({"history":[{"messageId":"last"}]})");
  CheckParserParity<lf::a2a::v1::Task>(
      std::to_array<std::string_view>({R"({"history":[{"messageId":"first"}],"history":[]})"}), R"({"history":[]})");
  CheckParserParity<lf::a2a::v1::Task>(
      std::to_array<std::string_view>({R"({"status":{"state":1},"status":{"timestamp":"2026-10-22T12:34:56Z"}})"}),
      R"({"status":{"timestamp":"2026-10-22T12:34:56Z"}})");
  CheckParserParity<lf::a2a::v1::Task>(std::to_array<std::string_view>({R"({"metadata":{"first":1},"metadata":{}})"}),
                                       R"({"metadata":{}})");
  CheckParserParity<lf::a2a::v1::AgentCard>(
      std::to_array<std::string_view>(
          {R"({"securitySchemes":{"bearer":{"httpAuthSecurityScheme":{"scheme":"bearer"}}},"security_schemes":{}})"}),
      R"({"securitySchemes":{}})");
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

template <std::size_t N>
void CheckRejectedTasks(const std::array<std::string_view, N>& fixtures,
                        const a2a::core::ProtoJsonParseOptions& options = {}) {
  for (const auto json : fixtures) {
    lf::a2a::v1::Task task;
    EXPECT_FALSE(a2a::core::JsonToMessage(json, &task, options).ok()) << json;
  }
}

TEST(CoreProtoJsonGoldenTest, RejectsMalformedAndAmbiguousProtocolObjects) {
  constexpr auto kInvalid = std::to_array<std::string_view>(
      {R"({"id":"t"} {})", R"({"id":"t",})", R"({"status":{"state":tru}})", R"({"metadata":{"value":01}})",
       R"({"id":"\uD800"})", R"({"\uD800":true})", R"({"metadata":{"value":1e400}})", R"({"id":"first","id":"second"})",
       R"({"id":null})", R"({"status":{"state":1,"state":2}})", R"({"history":[null]})",
       R"({"history":[],"history":[]})", R"({"status":{},"status":{}})"});
  constexpr a2a::core::ProtoJsonParseOptions kStrict{
      .ignore_unknown_fields = true, .reject_top_level_null_fields = true, .reject_duplicate_top_level_fields = true};
  CheckRejectedTasks(kInvalid, kStrict);
  constexpr auto kBadAliases = R"({"historyLength":1,"history_length":2})";
  lf::a2a::v1::GetTaskRequest request;
  EXPECT_FALSE(a2a::core::JsonToMessage(kBadAliases, &request, kStrict).ok());
  constexpr auto kInvalidUtf8 = "{\"id\":\"\xff\"}";
  lf::a2a::v1::Task task;
  EXPECT_FALSE(a2a::core::JsonToMessage(kInvalidUtf8, &task).ok());
  constexpr auto kDuplicateFallbacks =
      std::to_array<std::string_view>({R"({"history":[{"messageId":"first"}],"history":[],"id":null})",
                                       R"({"status":{"state":1},"status":{},"metadata":{"value":null}})"});
  CheckRejectedTasks(kDuplicateFallbacks);
  constexpr std::size_t kExcessiveDepth = 129U;
  std::string nested(kExcessiveDepth, '[');
  nested.append(kExcessiveDepth, ']');
  google::protobuf::Value value;
  EXPECT_FALSE(a2a::core::JsonToMessage(nested, &value).ok());
  constexpr std::size_t kOversizedBytes = (64U * 1024U * 1024U) + 1U;
  const std::string oversized(kOversizedBytes, ' ');
  EXPECT_FALSE(a2a::core::JsonToMessage(oversized, &value).ok());
}

void CheckOversizedParsersAreNotCached() {
  using namespace a2a::core::internal;
  std::string input(kMaximumCachedJsonBytes + 1U, 'x');
  input.front() = '"';
  input.back() = '"';
  {
    JsonDocument document;
    ASSERT_TRUE(document.Parse(input).ok());
    JsonCursor cursor(input);
    EXPECT_GT(document.capacity(), kMaximumCachedJsonBytes);
    EXPECT_GT(cursor.capacity(), kMaximumCachedJsonBytes);
  }
  JsonDocument fresh_document;
  constexpr std::string_view kEmptyObject = "{}";
  JsonCursor fresh_cursor(kEmptyObject);
  EXPECT_LE(fresh_document.capacity(), kMaximumCachedJsonBytes);
  EXPECT_LE(fresh_cursor.capacity(), kMaximumCachedJsonBytes);
}

TEST(CoreProtoJsonGoldenTest, ParserReuseKeepsResultsIndependentAcrossThreads) {
  CheckOversizedParsersAreNotCached();
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
