#include "a2a/client/sse_parser.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace {

TEST(SseParserTest, ParsesSimpleSequenceAndMultilineData) {
  a2a::client::SseParser parser;
  std::vector<a2a::client::SseEvent> events;

  const auto first_feed = parser.Feed(
      "event: message\ndata: {\"task\":{\"id\":\"t-1\"}}\n\n"
      "data: {\"message\":{\"role\":\"agent\"}}\n"
      "data: {\"metadata\":{}}\n\n",
      [&events](const a2a::client::SseEvent& event) {
        events.push_back(event);
        return a2a::core::Result<void>();
      });

  ASSERT_TRUE(first_feed.ok()) << first_feed.error().message();

  const auto finish = parser.Finish([&events](const a2a::client::SseEvent& event) {
    events.push_back(event);
    return a2a::core::Result<void>();
  });
  ASSERT_TRUE(finish.ok()) << finish.error().message();

  ASSERT_EQ(events.size(), 2U);
  EXPECT_EQ(events[0].event, "message");
  EXPECT_EQ(events[0].data, "{\"task\":{\"id\":\"t-1\"}}");
  EXPECT_EQ(events[1].event, "");
  EXPECT_EQ(events[1].data,
            "{\"message\":{\"role\":\"agent\"}}\n{\"metadata\":{}}");
}

TEST(SseParserTest, HandlesFragmentedFramesAcrossChunks) {
  a2a::client::SseParser parser;
  std::vector<a2a::client::SseEvent> events;

  ASSERT_TRUE(parser.Feed("eve", [&events](const a2a::client::SseEvent& event) {
                      events.push_back(event);
                      return a2a::core::Result<void>();
                    })
                  .ok());
  ASSERT_TRUE(parser.Feed("nt: mes", [&events](const a2a::client::SseEvent& event) {
                      events.push_back(event);
                      return a2a::core::Result<void>();
                    })
                  .ok());
  ASSERT_TRUE(parser.Feed("sage\nda", [&events](const a2a::client::SseEvent& event) {
                      events.push_back(event);
                      return a2a::core::Result<void>();
                    })
                  .ok());
  ASSERT_TRUE(parser.Feed("ta: {}\n\n", [&events](const a2a::client::SseEvent& event) {
                      events.push_back(event);
                      return a2a::core::Result<void>();
                    })
                  .ok());

  const auto finish = parser.Finish([&events](const a2a::client::SseEvent& event) {
    events.push_back(event);
    return a2a::core::Result<void>();
  });

  ASSERT_TRUE(finish.ok()) << finish.error().message();
  ASSERT_EQ(events.size(), 1U);
  EXPECT_EQ(events[0].event, "message");
  EXPECT_EQ(events[0].data, "{}");
}

TEST(SseParserTest, ReportsMalformedFrame) {
  a2a::client::SseParser parser;

  const auto feed = parser.Feed("unknown\n", [](const a2a::client::SseEvent&) {
    return a2a::core::Result<void>();
  });

  ASSERT_FALSE(feed.ok());
  EXPECT_EQ(feed.error().code(), a2a::core::ErrorCode::kSerialization);
}

TEST(SseParserTest, ReportsUnterminatedEventOnFinish) {
  a2a::client::SseParser parser;

  ASSERT_TRUE(parser.Feed("data: {\"task\":{}}\n", [](const a2a::client::SseEvent&) {
                      return a2a::core::Result<void>();
                    })
                  .ok());

  const auto finish = parser.Finish([](const a2a::client::SseEvent&) {
    return a2a::core::Result<void>();
  });

  ASSERT_FALSE(finish.ok());
  EXPECT_EQ(finish.error().code(), a2a::core::ErrorCode::kSerialization);
}

}  // namespace
