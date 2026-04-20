#include "a2a/client/sse_parser.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace {

a2a::core::Result<void> CaptureEvent(std::vector<a2a::client::SseEvent>& events,
                                     const a2a::client::SseEvent& event) {
  events.push_back(event);
  return {};
}

void FeedChunksOrFail(a2a::client::SseParser& parser, const std::vector<std::string>& chunks,
                      std::vector<a2a::client::SseEvent>& events) {
  for (const auto& chunk : chunks) {
    const auto status = parser.Feed(chunk, [&events](const a2a::client::SseEvent& event) {
      return CaptureEvent(events, event);
    });
    ASSERT_TRUE(status.ok()) << status.error().message();
  }
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(SseParserTest, ParsesSimpleSequenceAndMultilineData) {
  a2a::client::SseParser parser;
  std::vector<a2a::client::SseEvent> events;

  const auto first_feed = parser.Feed(
      "event: message\ndata: {\"task\":{\"id\":\"t-1\"}}\n\n"
      "data: {\"message\":{\"role\":\"agent\"}}\n"
      "data: {\"metadata\":{}}\n\n",
      [&events](const a2a::client::SseEvent& event) { return CaptureEvent(events, event); });

  ASSERT_TRUE(first_feed.ok()) << first_feed.error().message();

  const auto finish = parser.Finish(
      [&events](const a2a::client::SseEvent& event) { return CaptureEvent(events, event); });
  ASSERT_TRUE(finish.ok()) << finish.error().message();

  ASSERT_EQ(events.size(), 2U);
  EXPECT_EQ(events[0].event, "message");
  EXPECT_EQ(events[0].data, "{\"task\":{\"id\":\"t-1\"}}");
  EXPECT_EQ(events[1].event, "");
  EXPECT_EQ(events[1].data, "{\"message\":{\"role\":\"agent\"}}\n{\"metadata\":{}}");
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(SseParserTest, HandlesFragmentedFramesAcrossChunks) {
  a2a::client::SseParser parser;
  std::vector<a2a::client::SseEvent> events;
  FeedChunksOrFail(parser, {"eve", "nt: mes", "sage\nda", "ta: {}\n\n"}, events);

  const auto finish = parser.Finish(
      [&events](const a2a::client::SseEvent& event) { return CaptureEvent(events, event); });

  ASSERT_TRUE(finish.ok()) << finish.error().message();
  ASSERT_EQ(events.size(), 1U);
  EXPECT_EQ(events[0].event, "message");
  EXPECT_EQ(events[0].data, "{}");
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(SseParserTest, ReportsMalformedFrame) {
  a2a::client::SseParser parser;

  const auto feed = parser.Feed(
      "unknown\n", [](const a2a::client::SseEvent&) { return a2a::core::Result<void>(); });

  ASSERT_FALSE(feed.ok());
  EXPECT_EQ(feed.error().code(), a2a::core::ErrorCode::kSerialization);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(SseParserTest, ReportsUnterminatedEventOnFinish) {
  a2a::client::SseParser parser;

  const auto feed = parser.Feed("data: {\"task\":{}}\n", [](const a2a::client::SseEvent&) {
    return a2a::core::Result<void>();
  });
  ASSERT_TRUE(feed.ok()) << feed.error().message();

  const auto finish =
      parser.Finish([](const a2a::client::SseEvent&) { return a2a::core::Result<void>(); });

  ASSERT_FALSE(finish.ok());
  EXPECT_EQ(finish.error().code(), a2a::core::ErrorCode::kSerialization);
}

}  // namespace
