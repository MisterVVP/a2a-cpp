// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "a2a/client/sse_parser.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr std::string_view kSimpleFrame = R"(event: message
data: {"task":{"id":"t-1"}}

)";
constexpr std::string_view kSimpleData = R"({"task":{"id":"t-1"}})";
constexpr std::string_view kMessageEvent = "message";
constexpr std::string_view kFirstData = R"({"one":1})";
constexpr std::string_view kSecondData = R"({"two":2})";

using EventList = std::vector<a2a::client::SseEvent>;

a2a::core::Result<void> CaptureEvent(EventList& events, const a2a::client::SseEvent& event) {
  events.push_back(event);
  return {};
}

a2a::core::Result<void> FeedChunk(a2a::client::SseParser& parser, std::string_view chunk, EventList& events) {
  return parser.Feed(chunk, [&events](const a2a::client::SseEvent& event) { return CaptureEvent(events, event); });
}

void ExpectFinishOk(a2a::client::SseParser& parser, EventList& events) {
  const auto finish =
      parser.Finish([&events](const a2a::client::SseEvent& event) { return CaptureEvent(events, event); });
  ASSERT_TRUE(finish.ok()) << finish.error().message();
}

void FeedChunksOrFail(a2a::client::SseParser& parser, const std::vector<std::string_view>& chunks, EventList& events) {
  for (const auto chunk : chunks) {
    const auto status = FeedChunk(parser, chunk, events);
    ASSERT_TRUE(status.ok()) << status.error().message();
  }
}

void ExpectSingleEvent(const EventList& events, std::string_view event_name, std::string_view data) {
  ASSERT_EQ(events.size(), 1U);
  EXPECT_EQ(events[0].event, event_name);
  EXPECT_EQ(events[0].data, data);
}

EventList ParseChunks(const std::vector<std::string_view>& chunks) {
  a2a::client::SseParser parser;
  EventList events;
  FeedChunksOrFail(parser, chunks, events);
  ExpectFinishOk(parser, events);
  return events;
}

TEST(SseParserTest, ParsesOneCompleteEvent) {
  const auto events = ParseChunks({kSimpleFrame});
  ExpectSingleEvent(events, kMessageEvent, kSimpleData);
}

TEST(SseParserTest, ProducesIdenticalOutputForEveryByteSplit) {
  for (std::size_t split = 0; split <= kSimpleFrame.size(); ++split) {
    const auto prefix = kSimpleFrame.substr(0, split);
    const auto suffix = kSimpleFrame.substr(split);
    const auto events = ParseChunks({prefix, suffix});
    ExpectSingleEvent(events, kMessageEvent, kSimpleData);
  }
}

TEST(SseParserTest, ParsesMultipleEventsInOneChunk) {
  const auto events = ParseChunks({R"(data: {"one":1}

data: {"two":2}

)"});
  ASSERT_EQ(events.size(), 2U);
  EXPECT_EQ(events[0].data, kFirstData);
  EXPECT_EQ(events[1].data, kSecondData);
}

TEST(SseParserTest, ParsesCrLfFraming) {
  const auto events = ParseChunks({"event: message\r\ndata: {\"one\":1}\r\n\r\n"});
  ExpectSingleEvent(events, kMessageEvent, kFirstData);
}

TEST(SseParserTest, AcceptsDataFieldWithAndWithoutSingleSpace) {
  const auto events = ParseChunks({"data:{\"one\":1}\n\ndata: {\"two\":2}\n\n"});
  ASSERT_EQ(events.size(), 2U);
  EXPECT_EQ(events[0].data, kFirstData);
  EXPECT_EQ(events[1].data, kSecondData);
}

TEST(SseParserTest, JoinsMultilineDataFields) {
  const auto events = ParseChunks({"data: {\"message\":{}}\ndata: {\"metadata\":{}}\n\n"});
  ExpectSingleEvent(events, "", R"({"message":{}}
{"metadata":{}})");
}

TEST(SseParserTest, IgnoresCommentsHeartbeatsIdsAndRetryFields) {
  const auto events = ParseChunks({": heartbeat\nid: event-1\nretry: 10\ndata: {\"one\":1}\n\n"});
  ExpectSingleEvent(events, "", kFirstData);
}

TEST(SseParserTest, HandlesEmptyChunksBetweenFragments) {
  const auto events = ParseChunks({"data: {\"", "", "one\":1}", "\n\n"});
  ExpectSingleEvent(events, "", kFirstData);
}

TEST(SseParserTest, DispatchesFinalCompleteEventBeforeClosure) {
  a2a::client::SseParser parser;
  EventList events;
  ASSERT_TRUE(FeedChunk(parser, "data: {\"one\":1}\n\n", events).ok());
  ASSERT_EQ(events.size(), 1U);
  ExpectFinishOk(parser, events);
  ASSERT_EQ(events.size(), 1U);
}

TEST(SseParserTest, ReportsUnsupportedMalformedField) {
  a2a::client::SseParser parser;
  EventList events;
  const auto feed = FeedChunk(parser, "unknown: value\n", events);
  ASSERT_FALSE(feed.ok());
  EXPECT_EQ(feed.error().code(), a2a::core::ErrorCode::kSerialization);
}

TEST(SseParserTest, ReportsMissingSeparator) {
  a2a::client::SseParser parser;
  EventList events;
  const auto feed = FeedChunk(parser, "unknown\n", events);
  ASSERT_FALSE(feed.ok());
  EXPECT_EQ(feed.error().code(), a2a::core::ErrorCode::kSerialization);
}

TEST(SseParserTest, ReportsIncompleteFinalFrameDuringFinish) {
  a2a::client::SseParser parser;
  EventList events;
  ASSERT_TRUE(FeedChunk(parser, "data: {\"task\":{}}\n", events).ok());
  const auto finish =
      parser.Finish([&events](const a2a::client::SseEvent& event) { return CaptureEvent(events, event); });
  ASSERT_FALSE(finish.ok());
  EXPECT_EQ(finish.error().code(), a2a::core::ErrorCode::kSerialization);
}

TEST(SseParserTest, PropagatesEventCallbackFailure) {
  a2a::client::SseParser parser;
  const auto feed = parser.Feed("data: {\"one\":1}\n\n", [](const a2a::client::SseEvent&) {
    return a2a::core::Result<void>(a2a::core::Error::Internal("callback failed"));
  });
  ASSERT_FALSE(feed.ok());
  EXPECT_EQ(feed.error().code(), a2a::core::ErrorCode::kInternal);
}

TEST(SseParserTest, EmptyStreamFinalizationSucceeds) {
  a2a::client::SseParser parser;
  EventList events;
  ExpectFinishOk(parser, events);
  EXPECT_TRUE(events.empty());
}

}  // namespace
