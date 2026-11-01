// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cstddef>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "support/curl_reactor_test_support.h"

namespace {

using namespace a2a::http::testing;
using a2a::http::Client;
using a2a::http::detail::CurlStreamReactor;
constexpr std::size_t kSmallStreamCount = 1U;
constexpr std::size_t kMediumStreamCount = 16U;
constexpr std::size_t kLargeStreamCount = 64U;
constexpr std::size_t kBeyondHandleLimit = 96U;
constexpr std::size_t kMaximumSharedThreadGrowth = 8U;
constexpr std::size_t kCancellationThreads = 4U;
constexpr std::size_t kCancelRepeats = 16U;
constexpr std::size_t kRaceIterations = 16U;
constexpr auto kRequestTimer = std::chrono::milliseconds(100);
constexpr std::string_view kExpectedShutdownRejection = "HTTP client is shutting down";
constexpr int kSuccessfulHttpStatus = 200;

void ExpectTerminal(const std::shared_ptr<CurlStreamProbe>& probe, bool success) {
  ASSERT_EQ(probe->result.wait_for(kReactorTestTimeout), std::future_status::ready);
  EXPECT_EQ(probe->result.get().ok(), success);
  std::lock_guard lock(probe->mutex);
  EXPECT_FALSE(probe->chunk_before_metadata);
}

class CurlReactorScaleTest : public ::testing::TestWithParam<std::size_t> {};

TEST_P(CurlReactorScaleTest, EstablishedStreamsUseBoundedThreadsAndCancelIndependently) {
  const auto count = GetParam();
  CurlLoopbackServer server(count);
  Client client;
  const auto baseline_threads = ReactorTestThreadCount();
  std::vector<std::shared_ptr<CurlStreamProbe>> streams;
  streams.reserve(count);
  for (std::size_t index = 0; index < count; ++index) {
    streams.push_back(StartTestStream(client, server.url()));
  }
  ASSERT_TRUE(server.WaitForStreams(count));
  for (const auto& stream : streams) {
    ASSERT_TRUE(stream->WaitForChunk());
  }
  EXPECT_LE(ReactorTestThreadCount(), baseline_threads + kMaximumSharedThreadGrowth);
  streams.front()->cancel();
  ExpectTerminal(streams.front(), false);
  for (std::size_t index = 1; index < count; ++index) {
    EXPECT_EQ(streams[index]->result.wait_for(std::chrono::seconds::zero()), std::future_status::timeout);
  }
  for (std::size_t index = 1; index < count; ++index) {
    // Acceptance order can differ from submission order across reactor shards.
    // Finish all remaining server sockets, then collect all client results.
    (void)server.Finish(index);
  }
  (void)server.Finish(0);
  for (std::size_t index = 1; index < count; ++index) {
    ExpectTerminal(streams[index], true);
  }
}

INSTANTIATE_TEST_SUITE_P(StreamCounts, CurlReactorScaleTest,
                         ::testing::Values(kSmallStreamCount, kMediumStreamCount, kLargeStreamCount,
                                           kBeyondHandleLimit));

TEST(CurlReactorIntegrationTest, AddWakesAnAlreadyIdleReactor) {
  CurlLoopbackServer server(2U);
  Client client;
  auto first = StartTestStream(client, server.url());
  ASSERT_TRUE(first->WaitForChunk());
  auto second = StartTestStream(client, server.url());
  ASSERT_TRUE(second->WaitForChunk());
  first->cancel();
  second->cancel();
  ExpectTerminal(first, false);
  ExpectTerminal(second, false);
}

void ExpectUnaryResponse(std::future<a2a::core::Result<a2a::http::Response>>& result) {
  ASSERT_EQ(result.wait_for(kReactorTestTimeout), std::future_status::ready);
  const auto response = result.get();
  ASSERT_TRUE(response.ok()) << response.error().message();
  EXPECT_EQ(response.value().status_code, kSuccessfulHttpStatus);
  std::string expected(kTestFirstChunk);
  expected.append(kTestLastChunk);
  EXPECT_EQ(response.value().body, expected);
}

TEST(CurlReactorIntegrationTest, SynchronousUnaryCompletesWhilePeerStreamRemainsIdle) {
  CurlLoopbackServer server(2U);
  Client client;
  auto stream = StartTestStream(client, server.url());
  ASSERT_TRUE(stream->WaitForChunk());
  auto unary = std::async(std::launch::async, [&client, url = server.url()] {
    a2a::http::Request request;
    request.method = kTestGetMethod;
    request.url = url;
    request.timeout = kReactorTestTimeout;
    return client.SendRequest(request);
  });
  ASSERT_TRUE(server.WaitForStreams(2U));
  ASSERT_TRUE(server.Finish(1U));
  ExpectUnaryResponse(unary);
  EXPECT_EQ(stream->result.wait_for(std::chrono::seconds::zero()), std::future_status::timeout);
  stream->cancel();
  ExpectTerminal(stream, false);
}

TEST(CurlReactorIntegrationTest, LibcurlTimerCompletesIdleRequest) {
  CurlLoopbackServer server(1U);
  Client client;
  auto stream = StartTestStream(client, server.url(), kRequestTimer);
  ASSERT_TRUE(server.WaitForStreams(1U));
  ExpectTerminal(stream, false);
}

TEST(CurlReactorIntegrationTest, RepeatedConcurrentCancellationCompletesOnce) {
  CurlLoopbackServer server(1U);
  Client client;
  auto stream = StartTestStream(client, server.url());
  ASSERT_TRUE(stream->WaitForChunk());
  std::vector<std::thread> cancellers;
  cancellers.reserve(kCancellationThreads);
  for (std::size_t index = 0; index < kCancellationThreads; ++index) {
    cancellers.emplace_back([stream] {
      for (std::size_t repeat = 0; repeat < kCancelRepeats; ++repeat) {
        stream->cancel();
      }
    });
  }
  for (auto& canceller : cancellers) {
    canceller.join();
  }
  ExpectTerminal(stream, false);
  stream->cancel();
}

TEST(CurlReactorIntegrationTest, ServerCompletionRacesCancellation) {
  for (std::size_t iteration = 0; iteration < kRaceIterations; ++iteration) {
    CurlLoopbackServer server(1U);
    Client client;
    auto stream = StartTestStream(client, server.url());
    ASSERT_TRUE(stream->WaitForChunk());
    std::barrier gate(2);
    std::thread finisher([&] {
      gate.arrive_and_wait();
      (void)server.Finish(0);
    });
    gate.arrive_and_wait();
    stream->cancel();
    finisher.join();
    ASSERT_EQ(stream->result.wait_for(kReactorTestTimeout), std::future_status::ready);
    (void)stream->result.get();  // Either terminal outcome wins, exactly once.
  }
}

void RaceStartAndShutdown() {
  CurlLoopbackServer server(kCancellationThreads);
  Client client;
  std::barrier gate(static_cast<std::ptrdiff_t>(kCancellationThreads + 1U));
  std::vector<std::thread> starters;
  starters.reserve(kCancellationThreads);
  for (std::size_t index = 0; index < kCancellationThreads; ++index) {
    starters.emplace_back([&] {
      gate.arrive_and_wait();
      try {
        auto stream = StartTestStream(client, server.url());
        stream->cancel();
        ExpectTerminal(stream, false);
      } catch (const std::runtime_error& error) {
        // Shutdown may reject registration before any network work starts.
        EXPECT_EQ(std::string_view(error.what()), kExpectedShutdownRejection);
      }
    });
  }
  gate.arrive_and_wait();
  client.Shutdown();
  for (auto& starter : starters) {
    starter.join();
  }
}

TEST(CurlReactorIntegrationTest, ConcurrentStartsAndShutdownDoNotLoseCompletion) {
  for (std::size_t iteration = 0; iteration < kRaceIterations; ++iteration) {
    RaceStartAndShutdown();
  }
}

TEST(CurlReactorIntegrationTest, ShutdownWakesIdleStreamsAndCancellationRemainsSafe) {
  CurlLoopbackServer server(kMediumStreamCount);
  Client client;
  std::vector<std::shared_ptr<CurlStreamProbe>> streams;
  streams.reserve(kMediumStreamCount);
  for (std::size_t index = 0; index < kMediumStreamCount; ++index) {
    streams.push_back(StartTestStream(client, server.url()));
  }
  ASSERT_TRUE(server.WaitForStreams(kMediumStreamCount));
  client.Shutdown();
  for (const auto& stream : streams) {
    stream->cancel();
    ExpectTerminal(stream, false);
  }
}

struct ReactorTransfer final {
  struct EasyDeleter final {
    void operator()(CURL* easy) const noexcept { curl_easy_cleanup(easy); }
  };
  std::unique_ptr<CURL, EasyDeleter> easy{curl_easy_init()};
  std::shared_ptr<CurlStreamReactor::Transfer> transfer = std::make_shared<CurlStreamReactor::Transfer>();
  std::promise<void> first_byte;
  std::future<void> established = first_byte.get_future();
  std::promise<CURLcode> completed;
  std::future<CURLcode> result = completed.get_future();
  bool received = false;

  explicit ReactorTransfer(const std::string& url) {
    if (easy == nullptr) {
      throw std::runtime_error(std::string(kSocketFailure));
    }
    transfer->easy_handle = easy.get();
    transfer->on_complete = [this](CURLcode code) { completed.set_value(code); };
    (void)curl_easy_setopt(easy.get(), CURLOPT_URL, url.c_str());
    (void)curl_easy_setopt(easy.get(), CURLOPT_NOSIGNAL, 1L);
    (void)curl_easy_setopt(easy.get(), CURLOPT_WRITEFUNCTION, &Receive);
    (void)curl_easy_setopt(easy.get(), CURLOPT_WRITEDATA, this);
  }
  static std::size_t Receive(char* bytes, std::size_t size, std::size_t count, void* data) {
    (void)bytes;
    auto* probe = static_cast<ReactorTransfer*>(data);
    if (!probe->received) {
      probe->received = true;
      probe->first_byte.set_value();
    }
    return size * count;
  }
};

std::vector<std::unique_ptr<ReactorTransfer>> StartReactorTransfers(const CurlLoopbackServer& server,
                                                                    CurlStreamReactor& reactor) {
  std::vector<std::unique_ptr<ReactorTransfer>> streams;
  streams.reserve(kBeyondHandleLimit);
  for (std::size_t index = 0; index < kBeyondHandleLimit; ++index) {
    streams.push_back(std::make_unique<ReactorTransfer>(server.url()));
    reactor.Add(streams.back()->transfer);
  }
  return streams;
}

bool AllTransfersEstablished(const std::vector<std::unique_ptr<ReactorTransfer>>& streams) {
  return std::ranges::all_of(streams, [](const auto& stream) {
    return stream->established.wait_for(kReactorTestTimeout) == std::future_status::ready;
  });
}

void ExpectReactorCancelled(ReactorTransfer& stream) {
  ASSERT_EQ(stream.result.wait_for(kReactorTestTimeout), std::future_status::ready);
  EXPECT_EQ(stream.result.get(), CURLE_ABORTED_BY_CALLBACK);
}

TEST(CurlReactorIntegrationTest, OneReactorOwnsMoreThanSixtyFourEstablishedTransfers) {
  CurlLoopbackServer server(kBeyondHandleLimit);
  std::vector<std::unique_ptr<ReactorTransfer>> streams;
  auto reactor = CurlStreamReactor::Create();
  ASSERT_NE(reactor, nullptr);
  streams = StartReactorTransfers(server, *reactor);
  ASSERT_TRUE(server.WaitForStreams(kBeyondHandleLimit));
  ASSERT_TRUE(AllTransfersEstablished(streams));
  reactor->Cancel(streams.front()->transfer);
  ExpectReactorCancelled(*streams.front());
  EXPECT_EQ(streams.back()->result.wait_for(std::chrono::seconds::zero()), std::future_status::timeout);
  reactor->Shutdown();
  for (std::size_t index = 1; index < streams.size(); ++index) {
    ExpectReactorCancelled(*streams[index]);
  }
}

TEST(CurlReactorIntegrationTest, ShutdownCommandWakesAnEmptyReactor) {
  const auto global = a2a::http::detail::EnsureCurlGlobalInit();
  auto reactor = CurlStreamReactor::Create();
  ASSERT_NE(reactor, nullptr);
  auto stopped = std::async(std::launch::async, [reactor] { reactor->Shutdown(); });
  ASSERT_EQ(stopped.wait_for(kReactorTestTimeout), std::future_status::ready);
}

}  // namespace
