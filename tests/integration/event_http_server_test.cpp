// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)
#include "a2a/server/event_http_server.h"

#include <gtest/gtest.h>

#include <atomic>
#include <boost/asio.hpp>
#include <chrono>
#include <cstdint>
#include <future>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

#include "a2a/core/http_constants.h"
#include "a2a/server/engine/async_runtime.h"
#include "a2a/server/event_http_dispatcher.h"
#include "support/coroutine_reply.h"

namespace a2a::server {
namespace {
namespace asio = boost::asio;
using Tcp = asio::ip::tcp;
constexpr std::string_view kRequest = "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n";
constexpr std::string_view kCloseRequest = "GET / HTTP/1.1\r\nConnection: close\r\n\r\n";
constexpr std::string_view kStatus = "HTTP/1.1 200 OK";

constexpr std::string_view kOverload = "503 Service Unavailable";
constexpr std::string_view kBody = "ok";
constexpr std::string_view kEvent = "data: hello\n\n";
constexpr std::string_view kInitialEvent = "data: initial";
constexpr std::string_view kEventEnd = "\n\n";
constexpr std::size_t kInitialPadding = 64;
constexpr std::size_t kInitialOutputLimit = 128;
constexpr std::string_view kInitialHeader = "X-Initial";
constexpr std::size_t kInitialHeaderPadding = 41;
constexpr std::string_view kFinalChunk = "0\r\n\r\n";
constexpr auto kTimeout = std::chrono::seconds(5);
constexpr std::size_t kIdleClients = 64;

core::Result<HttpServerResponse> Respond(const HttpServerRequest& /*request*/) {
  HttpServerResponse response;
  response.status_code = core::http::kStatusOk;
  response.body = kBody;
  return response;
}
Tcp::socket Connect(asio::io_context& io, const EventHttpServer& server) {
  Tcp::socket socket(io);
  socket.connect(Tcp::endpoint(asio::ip::address_v4::loopback(), server.port()));
  return socket;
}
std::string ReadToEnd(Tcp::socket& socket) {
  std::string bytes;
  boost::system::error_code error;
  asio::read(socket, asio::dynamic_buffer(bytes), error);
  EXPECT_EQ(error, asio::error::eof);
  return bytes;
}

TEST(EventHttpServer, FragmentationPipeliningAndHalfClose) {
  EventHttpServer server(Respond);
  ASSERT_TRUE(server.Start().ok());
  asio::io_context io;
  auto socket = Connect(io, server);
  for (const char byte : kRequest) {
    asio::write(socket, asio::buffer(&byte, 1));
  }
  asio::write(socket, asio::buffer(kCloseRequest));
  socket.shutdown(Tcp::socket::shutdown_send);
  const auto response = ReadToEnd(socket);
  const auto first = response.find(kStatus);
  ASSERT_NE(first, std::string::npos);
  EXPECT_NE(response.find(kStatus, first + kStatus.size()), std::string::npos);
  server.Shutdown();
  server.Join();
}

TEST(EventHttpServer, IdleConnectionsDoNotOccupyApplicationWorkers) {
  EventHttpServerOptions options;
  options.application_threads = 1;
  EventHttpServer server(Respond, options);
  ASSERT_TRUE(server.Start().ok());
  asio::io_context io;
  std::vector<Tcp::socket> idle;
  idle.reserve(kIdleClients);
  for (std::size_t index = 0; index < kIdleClients; ++index) {
    idle.push_back(Connect(io, server));
  }
  auto active = Connect(io, server);
  asio::write(active, asio::buffer(kCloseRequest));
  EXPECT_NE(ReadToEnd(active).find(kBody), std::string::npos);
  server.Shutdown();
  server.Join();
}

asio::awaitable<core::Result<EventHttpResponse>> StreamResponse(HttpServerRequest /*request*/,
                                                                engine::RequestExecutionContext /*context*/) {
  EventHttpResponse result;
  result.response.status_code = core::http::kStatusOk;
  result.stream = [](std::shared_ptr<HttpEventStream> stream, const engine::RequestExecutionContext& /*context*/) {
    return stream->Write(std::string(kEvent));
  };
  co_return result;
}

TEST(EventHttpServer, FiniteStreamUsesChunksAndConnectionReuse) {
  EventHttpServer server{EventHttpServer::AsyncHandler(StreamResponse)};
  ASSERT_TRUE(server.Start().ok());
  asio::io_context io;
  auto socket = Connect(io, server);
  asio::write(socket, asio::buffer(kRequest));
  asio::write(socket, asio::buffer(kCloseRequest));
  const auto response = ReadToEnd(socket);
  EXPECT_NE(response.find(kEvent), std::string::npos);
  const auto end = response.find(kFinalChunk);
  ASSERT_NE(end, std::string::npos);
  EXPECT_NE(response.find(kStatus, end), std::string::npos);
  server.Shutdown();
  server.Join();
}

TEST(EventHttpServer, InitialBodyPrecedesProducerWithinOutputBound) {
  std::string initial(kInitialEvent);
  initial.append(kInitialPadding, 'x');
  initial.append(kEventEnd);
  for (const auto limit : {EventHttpServerOptions::kDefaultBufferBytes, kInitialOutputLimit}) {
    EventHttpServer::AsyncHandler handler =
        [initial](HttpServerRequest request,
                  engine::RequestExecutionContext context) -> asio::awaitable<core::Result<EventHttpResponse>> {
      auto response = co_await StreamResponse(std::move(request), std::move(context));
      response.value().response.body = initial;
      response.value().response.headers[std::string(kInitialHeader)] = std::string(kInitialHeaderPadding, 'x');
      co_return response;
    };
    EventHttpServer server(std::move(handler), {.max_output_bytes = limit});
    ASSERT_TRUE(server.Start().ok());
    asio::io_context io;
    auto socket = Connect(io, server);
    asio::write(socket, asio::buffer(kCloseRequest));
    const auto response = ReadToEnd(socket);
    const auto first = response.find(initial);
    ASSERT_NE(first, std::string::npos);
    EXPECT_NE(response.find(kEvent, first + initial.size()), std::string::npos);
    EXPECT_NE(response.find(kFinalChunk), std::string::npos);
    server.Shutdown();
    server.Join();
  }
}

TEST(EventHttpServer, PendingWorkIsBoundedAndShutdownCancels) {
  std::promise<void> entered;
  std::promise<void> release;
  auto released = release.get_future().share();
  EventHttpServerOptions options;
  options.max_pending_requests = 1;
  EventHttpServer server(
      [&](const HttpServerRequest& request) {
        entered.set_value();
        released.wait();
        return Respond(request);
      },
      options);
  ASSERT_TRUE(server.Start().ok());
  asio::io_context io;
  auto first = Connect(io, server);
  asio::write(first, asio::buffer(kRequest));
  ASSERT_EQ(entered.get_future().wait_for(kTimeout), std::future_status::ready);
  auto second = Connect(io, server);
  asio::write(second, asio::buffer(kCloseRequest));
  EXPECT_NE(ReadToEnd(second).find(kOverload), std::string::npos);
  server.Shutdown();
  release.set_value();
  server.Join();
}
constexpr std::string_view kRestTask =
    "GET /a2a/tasks/native HTTP/1.1\r\nA2A-Version: 1.0\r\nConnection: close\r\n\r\n";
constexpr std::string_view kRpcBody = R"({"jsonrpc":"2.0","id":7,"method":"GetTask","params":{"id":"native"}})";
constexpr std::string_view kRpcStreamBody =
    R"({"jsonrpc":"2.0","id":7,"method":"SendStreamingMessage","params":{"message":{"messageId":"m","role":"user","parts":[{"text":"hello"}]}}})";
constexpr std::string_view kRestStreamBody =
    R"({"message":{"messageId":"m","role":"user","parts":[{"text":"hello"}]}})";
constexpr std::string_view kNativeId = "native";
constexpr std::string_view kRpcPath = "/rpc";
constexpr std::string_view kRestStreamPath = "/a2a/message:stream";

constexpr std::string_view kPostPrefix = "POST ";
constexpr std::string_view kPostHeaders =
    " HTTP/1.1\r\nA2A-Version: 1.0\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: ";

class NativeBackend final : public engine::CoroutineAgentExecutor {
 public:
  asio::awaitable<core::Result<lf::a2a::v1::Task>> GetTask(lf::a2a::v1::GetTaskRequest request,
                                                           engine::RequestExecutionContext /*context*/) override {
    lf::a2a::v1::Task task;
    task.set_id(request.id());
    co_return task;
  }
  asio::awaitable<core::Result<void>> SendStreamingMessage(
      lf::a2a::v1::SendMessageRequest /*request*/, engine::RequestExecutionContext /*context*/,
      std::shared_ptr<engine::AsyncServerStreamSession> stream) override {
    lf::a2a::v1::StreamResponse event;
    event.mutable_task()->set_id(std::string(kNativeId));
    co_return co_await stream->Write(std::move(event));
  }
};

std::string Post(std::string_view path, std::string_view body) {
  std::string bytes(kPostPrefix);
  bytes.append(path);
  bytes.append(kPostHeaders);
  bytes.append(std::to_string(body.size()));
  bytes.append(core::http::kHeaderDelimiter);
  bytes.append(body);
  return bytes;
}

TEST(EventHttpServer, NativeDispatcherReusesRestAndJsonRpcValidationAndSseEncoding) {
  engine::AsyncRuntime runtime(std::make_shared<NativeBackend>());
  EventHttpServer server(MakeEventHttpHandler(runtime.dispatcher(), {}));
  ASSERT_TRUE(server.Start().ok());
  asio::io_context io;
  const std::vector<std::string> requests{std::string(kRestTask), Post(kRpcPath, kRpcBody),
                                          Post(kRpcPath, kRpcStreamBody), Post(kRestStreamPath, kRestStreamBody)};
  for (const auto& request : requests) {
    auto socket = Connect(io, server);
    asio::write(socket, asio::buffer(request));
    const auto response = ReadToEnd(socket);
    EXPECT_NE(response.find(kStatus), std::string::npos) << response;
    EXPECT_NE(response.find(kNativeId), std::string::npos) << response;
  }
  server.Shutdown();
  server.Join();
  runtime.Shutdown();
  runtime.Join();
}
constexpr std::string_view kSubscribeRequest = "GET /a2a/tasks/native:subscribe HTTP/1.1\r\nA2A-Version: 1.0\r\n\r\n";
constexpr std::size_t kLiveClients = 32;
constexpr std::size_t kLargeBody = std::size_t{2} * 1024U * 1024U;
constexpr auto kIdleDeadline = std::chrono::milliseconds(50);
constexpr auto kActiveIdleDeadline = std::chrono::milliseconds(500);
constexpr auto kActivityInterval = std::chrono::milliseconds(25);

class LiveBackend final : public engine::CoroutineAgentExecutor {
 public:
  asio::awaitable<core::Result<void>> SubscribeTask(lf::a2a::v1::GetTaskRequest /*request*/,
                                                    engine::RequestExecutionContext /*context*/,
                                                    std::shared_ptr<engine::AsyncServerStreamSession> stream) override {
    struct Cleanup final {
      explicit Cleanup(LiveBackend& backend) : backend(backend) {}
      ~Cleanup() {
        if (backend.completed.fetch_add(1) == 0) {
          backend.first_completed.set_value();
        }
      }
      LiveBackend& backend;
    } cleanup(*this);
    const auto origin = std::this_thread::get_id();
    lf::a2a::v1::StreamResponse event;
    event.mutable_task()->set_id(std::string(kNativeId));
    auto written = co_await stream->Write(std::move(event));
    if (origin != std::this_thread::get_id()) {
      wrong_thread.store(true);
    }
    if (!written.ok()) {
      co_return written.error();
    }
    asio::steady_timer timer(co_await asio::this_coro::executor);
    timer.expires_at(asio::steady_timer::time_point::max());
    boost::system::error_code error;
    co_await timer.async_wait(asio::redirect_error(asio::use_awaitable, error));
    co_return core::Result<void>{};
  }
  std::atomic_size_t completed{0};
  std::atomic_bool wrong_thread{false};
  std::promise<void> first_completed;
};

TEST(EventHttpServer, LiveStreamsSuspendAndShutdownCleansEachSessionOnce) {
  const auto backend = std::make_shared<LiveBackend>();
  engine::AsyncRuntime runtime(backend, {}, {.threads = 1});
  EventHttpServerOptions options;
  options.application_threads = 1;
  EventHttpServer server(MakeEventHttpHandler(runtime.dispatcher(), {}), options);
  ASSERT_TRUE(server.Start().ok());
  asio::io_context io;
  std::vector<Tcp::socket> sockets;
  sockets.reserve(kLiveClients);
  for (std::size_t index = 0; index < kLiveClients; ++index) {
    sockets.push_back(Connect(io, server));
    asio::write(sockets.back(), asio::buffer(kSubscribeRequest));
    std::string event;
    asio::read_until(sockets.back(), asio::dynamic_buffer(event), kEventEnd);
    ASSERT_NE(event.find(kNativeId), std::string::npos);
  }
  server.Shutdown();
  server.Join();
  runtime.Shutdown();
  runtime.Join();
  EXPECT_EQ(backend->completed.load(), kLiveClients);
  EXPECT_FALSE(backend->wrong_thread.load());
}

TEST(EventHttpServer, DisconnectCancelsLiveProducer) {
  const auto backend = std::make_shared<LiveBackend>();
  engine::AsyncRuntime runtime(backend);
  EventHttpServer server(MakeEventHttpHandler(runtime.dispatcher(), {}));
  ASSERT_TRUE(server.Start().ok());
  asio::io_context io;
  auto socket = Connect(io, server);
  asio::write(socket, asio::buffer(kSubscribeRequest));
  std::string event;
  asio::read_until(socket, asio::dynamic_buffer(event), kEventEnd);
  socket.close();
  server.Shutdown();
  server.Join();
  runtime.Shutdown();
  runtime.Join();
  EXPECT_EQ(backend->completed.load(), 1U);
}

TEST(EventHttpServer, PeerResetCancelsLiveProducerBeforeShutdown) {
  const auto backend = std::make_shared<LiveBackend>();
  auto completed = backend->first_completed.get_future();
  engine::AsyncRuntime runtime(backend);
  EventHttpServer server(MakeEventHttpHandler(runtime.dispatcher(), {}));
  ASSERT_TRUE(server.Start().ok());
  asio::io_context io;
  auto peer = Connect(io, server);
  asio::write(peer, asio::buffer(kSubscribeRequest));
  std::string event;
  asio::read_until(peer, asio::dynamic_buffer(event), kEventEnd);
  peer.set_option(asio::socket_base::linger(true, 0));
  peer.close();
  EXPECT_EQ(completed.wait_for(kTimeout), std::future_status::ready);
  server.Shutdown();
  server.Join();
  runtime.Shutdown();
  runtime.Join();
  EXPECT_EQ(backend->completed.load(), 1U);
}

constexpr std::string_view kFiniteTarget = "/finite";
constexpr std::string_view kFiniteRequest = "GET /finite HTTP/1.1\r\n\r\n";
constexpr std::string_view kFiniteCloseRequest = "GET /finite HTTP/1.1\r\nConnection: close\r\n\r\n";
constexpr std::string_view kLastEvent = "data: final\n\n";
constexpr auto kHalfCloseObservation = std::chrono::milliseconds(100);

struct HalfClosedStream final {
  test::CoroutineReply<void> release{true};
  std::promise<void> stopped;
};

asio::awaitable<core::Result<void>> ProduceHalfClosedStream(std::shared_ptr<HalfClosedStream> state,
                                                            std::shared_ptr<HttpEventStream> stream,
                                                            engine::RequestExecutionContext context) {
  std::stop_callback stopped(context.StopToken(), [state] { state->stopped.set_value(); });
  const auto first = co_await stream->Write(std::string(kEvent));
  if (!first.ok()) {
    co_return first.error();
  }
  const auto released = co_await state->release.Wait();
  if (!released.ok()) {
    co_return released.error();
  }
  co_return co_await stream->Write(std::string(kLastEvent));
}

asio::awaitable<core::Result<EventHttpResponse>> HalfClosedResponse(std::shared_ptr<HalfClosedStream> state,
                                                                    HttpServerRequest request) {
  EventHttpResponse response;
  response.response = Respond(request).value();
  if (request.target == kFiniteTarget) {
    response.stream = [state](std::shared_ptr<HttpEventStream> stream, engine::RequestExecutionContext context) {
      return ProduceHalfClosedStream(state, std::move(stream), std::move(context));
    };
  }
  co_return response;
}

void ExpectHalfClosedStream(bool pipeline) {
  const auto state = std::make_shared<HalfClosedStream>();
  auto stopped = state->stopped.get_future();
  EventHttpServer::AsyncHandler handler = [state](HttpServerRequest request, engine::RequestExecutionContext) {
    return HalfClosedResponse(state, std::move(request));
  };
  EventHttpServer server(std::move(handler));
  ASSERT_TRUE(server.Start().ok());
  asio::io_context io;
  auto peer = Connect(io, server);
  asio::write(peer, asio::buffer(pipeline ? kFiniteRequest : kFiniteCloseRequest));
  std::string first;
  asio::read_until(peer, asio::dynamic_buffer(first), kEventEnd);
  if (pipeline) {
    asio::write(peer, asio::buffer(kCloseRequest));
  }
  peer.shutdown(Tcp::socket::shutdown_send);
  const auto cancellation = stopped.wait_for(kHalfCloseObservation);
  state->release.Resolve(core::Result<void>{});
  const auto remaining = ReadToEnd(peer);
  server.Shutdown();
  server.Join();
  EXPECT_EQ(cancellation, std::future_status::timeout);
  EXPECT_NE(remaining.find(kLastEvent), std::string::npos);
  const auto terminal = remaining.find(kFinalChunk);
  EXPECT_NE(terminal, std::string::npos);
  if (pipeline) {
    EXPECT_NE(remaining.find(kStatus, terminal), std::string::npos);
  }
}

TEST(EventHttpServer, SendHalfCloseAfterFirstEventDrainsFiniteStream) { ExpectHalfClosedStream(false); }

TEST(EventHttpServer, SendHalfCloseAfterFirstEventDrainsStreamAndPipeline) { ExpectHalfClosedStream(true); }

using ContextPromise = std::promise<engine::RequestExecutionContext>;
constexpr std::string_view kProducerFailure = "stream producer failed";

constexpr std::string_view kNotImplemented = "501 Not Implemented";

asio::awaitable<core::Result<void>> FailProducer(std::shared_ptr<ContextPromise> context_promise,
                                                 std::shared_ptr<HttpEventStream> stream,
                                                 engine::RequestExecutionContext context) {
  context_promise->set_value(std::move(context));
  const auto written = co_await stream->Write(std::string(kEvent));
  if (!written.ok()) {
    co_return written.error();
  }
  co_return core::Error::Internal(std::string(kProducerFailure));
}

asio::awaitable<core::Result<EventHttpResponse>> FailingResponse(std::shared_ptr<ContextPromise> promise) {
  EventHttpResponse response;
  response.response = Respond({}).value();
  response.stream = [promise](std::shared_ptr<HttpEventStream> stream, engine::RequestExecutionContext context) {
    return FailProducer(promise, std::move(stream), std::move(context));
  };
  co_return response;
}

TEST(EventHttpServer, ProducerFailureSignalsCancellationAfterHeaders) {
  auto promise = std::make_shared<ContextPromise>();
  auto context = promise->get_future();
  EventHttpServer::AsyncHandler handler = [promise](HttpServerRequest, engine::RequestExecutionContext) {
    return FailingResponse(promise);
  };
  EventHttpServer server(std::move(handler));
  ASSERT_TRUE(server.Start().ok());
  asio::io_context io;
  auto peer = Connect(io, server);
  asio::write(peer, asio::buffer(kRequest));
  const auto response = ReadToEnd(peer);
  EXPECT_NE(response.find(kEvent), std::string::npos);
  EXPECT_EQ(response.find(kFinalChunk), std::string::npos);
  EXPECT_TRUE(context.get().StopToken().stop_requested());
  server.Shutdown();
  server.Join();
}

asio::awaitable<core::Result<EventHttpResponse>> LegacyStreamResponse(std::shared_ptr<std::atomic_bool> called) {
  EventHttpResponse response;
  response.response = Respond({}).value();
  response.response.stream_writer = [called](HttpByteTransport&) {
    called->store(true);
    return core::Result<void>{};
  };
  response.stream = [called](std::shared_ptr<HttpEventStream>,
                             engine::RequestExecutionContext) -> asio::awaitable<core::Result<void>> {
    called->store(true);
    co_return core::Result<void>{};
  };
  co_return response;
}

TEST(EventHttpServer, BlockingWriterIsRejectedWithoutInvokingEitherProducer) {
  auto called = std::make_shared<std::atomic_bool>(false);
  EventHttpServer::AsyncHandler handler = [called](HttpServerRequest, engine::RequestExecutionContext) {
    return LegacyStreamResponse(called);
  };
  EventHttpServer server(std::move(handler));
  ASSERT_TRUE(server.Start().ok());
  asio::io_context io;
  auto peer = Connect(io, server);
  asio::write(peer, asio::buffer(kCloseRequest));
  EXPECT_NE(ReadToEnd(peer).find(kNotImplemented), std::string::npos);
  EXPECT_FALSE(called->load());
  server.Shutdown();
  server.Join();
}

constexpr std::size_t kRejectedHeaderLimit = 32;

TEST(EventHttpServer, RejectedStreamHeadersCancelNativeProducerBeforeShutdown) {
  const auto backend = std::make_shared<LiveBackend>();
  auto completed = backend->first_completed.get_future();
  engine::AsyncRuntime runtime(backend);
  EventHttpServerOptions options;
  options.max_output_bytes = kRejectedHeaderLimit;
  EventHttpServer server(MakeEventHttpHandler(runtime.dispatcher(), {}), options);
  ASSERT_TRUE(server.Start().ok());
  asio::io_context io;
  auto peer = Connect(io, server);
  asio::write(peer, asio::buffer(kSubscribeRequest));
  EXPECT_TRUE(ReadToEnd(peer).empty());
  EXPECT_EQ(completed.wait_for(kTimeout), std::future_status::ready);
  server.Shutdown();
  server.Join();
  runtime.Shutdown();
  runtime.Join();
  EXPECT_EQ(backend->completed.load(), 1U);
}

TEST(EventHttpServer, IdleTimeoutAndOutputLimitReleaseConnections) {
  EventHttpServerOptions options;
  options.idle_timeout = kIdleDeadline;
  EventHttpServer server(
      [](const HttpServerRequest& /*request*/) {
        auto response = Respond({});
        response.value().body.assign(kLargeBody, 'x');
        return response;
      },
      options);
  ASSERT_TRUE(server.Start().ok());
  asio::io_context io;
  auto idle = Connect(io, server);
  EXPECT_TRUE(ReadToEnd(idle).empty());
  auto large = Connect(io, server);
  asio::write(large, asio::buffer(kCloseRequest));
  EXPECT_TRUE(ReadToEnd(large).empty());
  server.Shutdown();
  server.Join();
}

TEST(EventHttpServer, ActivityRefreshesIdleDeadlineBeforeConnectionEventuallyExpires) {
  EventHttpServerOptions options;
  options.idle_timeout = kActiveIdleDeadline;
  EventHttpServer server(Respond, options);
  ASSERT_TRUE(server.Start().ok());
  asio::io_context io;
  auto socket = Connect(io, server);
  const auto until = std::chrono::steady_clock::now() + kActiveIdleDeadline * 2;
  do {
    asio::write(socket, asio::buffer(kRequest));
    std::string response;
    asio::read_until(socket, asio::dynamic_buffer(response), kBody);
    EXPECT_NE(response.find(kStatus), std::string::npos);
    std::this_thread::sleep_for(kActivityInterval);
  } while (std::chrono::steady_clock::now() < until);
  EXPECT_TRUE(ReadToEnd(socket).empty());
  server.Shutdown();
  server.Join();
}

constexpr std::string_view kSlowRequest = "GET /slow HTTP/1.1\r\n\r\n";
constexpr std::string_view kSlowTarget = "/slow";
constexpr std::string_view kHeaderEnd = "\r\n\r\n";
constexpr std::size_t kSlowChunkBytes = std::size_t{64} * 1024U;

struct SlowState final {
  std::promise<void> entered;
  std::promise<void> finished;
  std::atomic_size_t cleanup{0};
};
asio::awaitable<core::Result<void>> SlowProduce(std::shared_ptr<SlowState> state,
                                                std::shared_ptr<HttpEventStream> stream,
                                                engine::RequestExecutionContext context) {
  struct Cleanup final {
    explicit Cleanup(SlowState& state) : state(state) {}
    ~Cleanup() {
      ++state.cleanup;
      state.finished.set_value();
    }
    SlowState& state;
  } cleanup(*state);
  state->entered.set_value();
  while (!context.StopToken().stop_requested()) {
    auto written = co_await stream->Write(std::string(kSlowChunkBytes, 'x'));
    if (!written.ok()) {
      co_return written.error();
    }
  }
  co_return core::Result<void>{};
}
asio::awaitable<core::Result<EventHttpResponse>> SlowHandle(std::shared_ptr<SlowState> state,
                                                            HttpServerRequest request) {
  EventHttpResponse response;
  response.response = Respond(request).value();
  if (request.target == kSlowTarget) {
    response.stream = [state](std::shared_ptr<HttpEventStream> stream, engine::RequestExecutionContext context) {
      return SlowProduce(state, std::move(stream), std::move(context));
    };
  }
  co_return response;
}

TEST(EventHttpServer, SlowReaderDoesNotStallUnaryAndShutdownWakesPendingWrite) {
  const auto state = std::make_shared<SlowState>();
  auto entered = state->entered.get_future();
  auto finished = state->finished.get_future();
  EventHttpServerOptions options;
  options.application_threads = 1;
  EventHttpServer server(
      EventHttpServer::AsyncHandler([state](HttpServerRequest request, engine::RequestExecutionContext /*context*/) {
        return SlowHandle(state, std::move(request));
      }),
      options);
  ASSERT_TRUE(server.Start().ok());
  asio::io_context io;
  auto slow = Connect(io, server);
  slow.set_option(asio::socket_base::receive_buffer_size(static_cast<int>(kSlowChunkBytes)));
  asio::write(slow, asio::buffer(kSlowRequest));
  std::string headers;
  asio::read_until(slow, asio::dynamic_buffer(headers), kHeaderEnd);
  ASSERT_EQ(entered.wait_for(kTimeout), std::future_status::ready);
  auto unary = Connect(io, server);
  asio::write(unary, asio::buffer(kCloseRequest));
  EXPECT_NE(ReadToEnd(unary).find(kBody), std::string::npos);
  server.Shutdown();
  server.Join();
  EXPECT_EQ(finished.wait_for(kTimeout), std::future_status::ready);
  EXPECT_EQ(state->cleanup.load(), 1U);
}

constexpr int kSlowReceiveBytes = 4096;
constexpr auto kBackpressureObservation = std::chrono::milliseconds(100);
constexpr std::size_t kMaxSlowEvents = 4096;
constexpr std::string_view kAgentCardRequest = "GET /.well-known/agent-card.json HTTP/1.1\r\nConnection: close\r\n\r\n";

class SlowNativeBackend final : public engine::CoroutineAgentExecutor {
 public:
  asio::awaitable<core::Result<void>> SubscribeTask(lf::a2a::v1::GetTaskRequest /*request*/,
                                                    engine::RequestExecutionContext context,
                                                    std::shared_ptr<engine::AsyncServerStreamSession> stream) override {
    struct Cleanup final {
      explicit Cleanup(SlowNativeBackend& backend) : backend(backend), origin(std::this_thread::get_id()) {}
      ~Cleanup() {
        backend.wrong_thread.store(origin != std::this_thread::get_id());
        ++backend.cleanup;
        backend.finished.set_value();
      }
      SlowNativeBackend& backend;
      const std::thread::id origin;
    } cleanup(*this);
    std::stop_callback stop(context.StopToken(), [this] { stopped.set_value(); });
    lf::a2a::v1::StreamResponse event;
    auto* task = event.mutable_task();
    task->set_id(std::string(kNativeId));
    task->set_context_id("ctx");
    task->mutable_status()->set_state(lf::a2a::v1::TASK_STATE_WORKING);
    auto* message = task->mutable_status()->mutable_message();
    message->set_message_id("bulk");
    message->set_role(lf::a2a::v1::ROLE_AGENT);
    message->add_parts()->set_text(std::string(kSlowChunkBytes, 'x'));
    for (std::size_t index = 0; index < kMaxSlowEvents; ++index) {
      ++writes;
      const auto written = co_await stream->Write(event);
      if (!written.ok()) {
        co_return written.error();
      }
    }
    co_return core::Result<void>{};
  }
  std::atomic_size_t writes{0};
  std::atomic_size_t cleanup{0};
  std::atomic_bool wrong_thread{false};
  std::promise<void> stopped;
  std::promise<void> finished;
};

bool WaitForBackpressure(const SlowNativeBackend& backend) {
  const auto deadline = std::chrono::steady_clock::now() + kTimeout;
  while (std::chrono::steady_clock::now() < deadline) {
    const auto count = backend.writes.load();
    std::this_thread::sleep_for(kBackpressureObservation);
    if (count > 1U && count == backend.writes.load() && backend.cleanup.load() == 0U) {
      return true;
    }
  }
  return false;
}

enum class ConcurrentClose : std::uint8_t { kNone, kPeerReset, kListenerShutdown };

void ExpectNativeCleanup(const SlowNativeBackend& backend, std::future<void>& stopped, std::future<void>& finished) {
  EXPECT_EQ(stopped.wait_for(kTimeout), std::future_status::ready);
  EXPECT_EQ(finished.wait_for(kTimeout), std::future_status::ready);
  EXPECT_EQ(backend.cleanup.load(), 1U);
  EXPECT_FALSE(backend.wrong_thread.load());
}

void ExpectNativeWriteCancellation(ConcurrentClose close) {
  const auto backend = std::make_shared<SlowNativeBackend>();
  auto stopped = backend->stopped.get_future();
  auto finished = backend->finished.get_future();
  engine::AsyncRuntime runtime(backend, {}, {.threads = 1});
  EventHttpServerOptions options;
  options.application_threads = 1;
  options.heartbeat_interval = std::chrono::milliseconds::zero();
  EventHttpServer server(MakeEventHttpHandler(runtime.dispatcher(), {}), options);
  ASSERT_TRUE(server.Start().ok());
  asio::io_context io;
  Tcp::socket peer(io);
  peer.open(Tcp::v4());
  peer.set_option(asio::socket_base::receive_buffer_size(kSlowReceiveBytes));
  peer.connect({asio::ip::address_v4::loopback(), server.port()});
  asio::write(peer, asio::buffer(kSubscribeRequest));
  std::string headers;
  asio::read_until(peer, asio::dynamic_buffer(headers), kHeaderEnd);
  EXPECT_TRUE(WaitForBackpressure(*backend));
  runtime.Shutdown();
  if (close == ConcurrentClose::kPeerReset) {
    peer.set_option(asio::socket_base::linger(true, 0));
    peer.close();
  } else if (close == ConcurrentClose::kListenerShutdown) {
    server.Shutdown();
  }
  ExpectNativeCleanup(*backend, stopped, finished);
  if (close == ConcurrentClose::kNone) {
    auto unary = Connect(io, server);
    asio::write(unary, asio::buffer(kAgentCardRequest));
    EXPECT_NE(ReadToEnd(unary).find(kStatus), std::string::npos);
  }
  server.Shutdown();
  server.Join();
  runtime.Join();
}

TEST(EventHttpServer, NativeCancellationWakesBackpressuredWriteBeforeListenerShutdown) {
  ExpectNativeWriteCancellation(ConcurrentClose::kNone);
}

TEST(EventHttpServer, NativeWriteCancellationRacesPeerReset) {
  ExpectNativeWriteCancellation(ConcurrentClose::kPeerReset);
}

TEST(EventHttpServer, NativeWriteCancellationRacesListenerShutdown) {
  ExpectNativeWriteCancellation(ConcurrentClose::kListenerShutdown);
}

constexpr std::string_view kMalformedRequest = "POST / HTTP/1.1\r\nContent-Length: -1\r\n\r\n";
constexpr std::string_view kBadRequestStatus = "400 Bad Request";

TEST(EventHttpServer, ConnectionLimitAndMalformedInputRemainIsolated) {
  EventHttpServerOptions options;
  options.max_connections = 1;
  EventHttpServer server(Respond, options);
  ASSERT_TRUE(server.Start().ok());
  asio::io_context io;
  auto first = Connect(io, server);
  asio::write(first, asio::buffer(kRequest));
  std::string response;
  asio::read_until(first, asio::dynamic_buffer(response), kHeaderEnd);
  auto excess = Connect(io, server);
  EXPECT_TRUE(ReadToEnd(excess).empty());
  asio::write(first, asio::buffer(kMalformedRequest));
  response.append(ReadToEnd(first));
  EXPECT_NE(response.find(kBadRequestStatus), std::string::npos);
  auto next = Connect(io, server);
  asio::write(next, asio::buffer(kCloseRequest));
  EXPECT_NE(ReadToEnd(next).find(kStatus), std::string::npos);
  server.Shutdown();
  server.Join();
}

TEST(EventHttpServer, OptionsAndStartLifecycleAreValidated) {
  EventHttpServer missing(EventHttpServer::Handler{});
  EXPECT_FALSE(missing.Start().ok());
  EventHttpServerOptions options;
  options.max_pending_requests = 0;
  EventHttpServer invalid(Respond, options);
  EXPECT_FALSE(invalid.Start().ok());
  EventHttpServer server(Respond);
  ASSERT_TRUE(server.Start().ok());
  EXPECT_FALSE(server.Start().ok());
  server.Shutdown();
  server.Shutdown();
  server.Join();
}
using SinkPromise = std::promise<std::shared_ptr<HttpEventStream>>;
asio::awaitable<core::Result<void>> RetainSink(std::shared_ptr<SinkPromise> promise,
                                               std::shared_ptr<HttpEventStream> stream) {
  promise->set_value(std::move(stream));
  co_return core::Result<void>{};
}
asio::awaitable<core::Result<EventHttpResponse>> RetainedResponse(std::shared_ptr<SinkPromise> promise) {
  EventHttpResponse response;
  response.response = Respond({}).value();
  response.stream = [promise](std::shared_ptr<HttpEventStream> stream,
                              const engine::RequestExecutionContext& /*context*/) {
    return RetainSink(promise, std::move(stream));
  };
  co_return response;
}

void ExpectRejectedWrite(asio::io_context& io, const std::shared_ptr<HttpEventStream>& stream) {
  for (const auto bytes : {kEvent, std::string_view{}}) {
    io.restart();
    auto result = asio::co_spawn(io, stream->Write(std::string(bytes)), asio::use_future);
    (void)io.run();
    EXPECT_FALSE(result.get().ok());
  }
}

TEST(EventHttpServer, RetainedStreamSinkRejectsWritesAfterListenerDestruction) {
  auto promise = std::make_shared<SinkPromise>();
  auto sink = promise->get_future();
  asio::io_context io;
  {
    EventHttpServer::AsyncHandler handler = [promise](HttpServerRequest /*request*/,
                                                      const engine::RequestExecutionContext& /*context*/) {
      return RetainedResponse(promise);
    };
    EventHttpServer server(std::move(handler));
    ASSERT_TRUE(server.Start().ok());
    auto peer = Connect(io, server);
    asio::write(peer, asio::buffer(kCloseRequest));
    EXPECT_NE(ReadToEnd(peer).find(kFinalChunk), std::string::npos);
    server.Shutdown();
    server.Join();
  }
  const auto retained = sink.get();
  ExpectRejectedWrite(io, retained);
}

constexpr std::string_view kHoldTarget = "/hold";
constexpr std::string_view kHoldRequest = "GET /hold HTTP/1.1\r\n\r\n";
asio::awaitable<core::Result<EventHttpResponse>> UnaryResponse(HttpServerRequest request) {
  co_return EventHttpResponse{.response = Respond(request).value(), .stream = {}};
}

TEST(EventHttpServer, CompletedStreamSinkCannotCorruptReusedConnection) {
  auto promise = std::make_shared<SinkPromise>();
  auto sink = promise->get_future();
  EventHttpServer server(EventHttpServer::AsyncHandler(
      [promise](HttpServerRequest request, const engine::RequestExecutionContext& /*context*/) {
        if (request.target == kHoldTarget) {
          return RetainedResponse(promise);
        }
        return UnaryResponse(std::move(request));
      }));
  ASSERT_TRUE(server.Start().ok());
  asio::io_context io;
  auto peer = Connect(io, server);
  asio::write(peer, asio::buffer(kHoldRequest));
  std::string first;
  asio::read_until(peer, asio::dynamic_buffer(first), kFinalChunk);
  ExpectRejectedWrite(io, sink.get());
  asio::write(peer, asio::buffer(kCloseRequest));
  const auto second = ReadToEnd(peer);
  EXPECT_NE(second.find(kStatus), std::string::npos);
  EXPECT_EQ(second.find(kEvent), std::string::npos);
  server.Shutdown();
  server.Join();
}

class DelayedBackend final : public engine::CoroutineAgentExecutor {
 public:
  asio::awaitable<core::Result<void>> SubscribeTask(
      lf::a2a::v1::GetTaskRequest /*request*/, engine::RequestExecutionContext /*context*/,
      std::shared_ptr<engine::AsyncServerStreamSession> /*stream*/) override {
    started.set_value();
    const auto result = co_await reply_.Wait();
    ++completed;
    co_return result;
  }
  void Release() { reply_.Resolve(core::Result<void>{}); }
  std::promise<void> started;
  std::atomic_size_t completed{0};

 private:
  test::CoroutineReply<void> reply_{true};
};

TEST(EventHttpServer, NativeCleanupBeforeFirstEventMayOutliveListener) {
  const auto backend = std::make_shared<DelayedBackend>();
  auto started = backend->started.get_future();
  engine::AsyncRuntime runtime(backend);
  asio::io_context io;
  {
    EventHttpServer server(MakeEventHttpHandler(runtime.dispatcher(), {}));
    ASSERT_TRUE(server.Start().ok());
    auto peer = Connect(io, server);
    asio::write(peer, asio::buffer(kSubscribeRequest));
    EXPECT_EQ(started.wait_for(kTimeout), std::future_status::ready);
    server.Shutdown();
    server.Join();
    EXPECT_EQ(backend->completed.load(), 0U);
  }
  backend->Release();
  runtime.Shutdown();
  runtime.Join();
  EXPECT_EQ(backend->completed.load(), 1U);
}
}  // namespace
}  // namespace a2a::server
