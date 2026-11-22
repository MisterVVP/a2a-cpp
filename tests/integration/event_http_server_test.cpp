// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)
#include "a2a/server/event_http_server.h"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <boost/asio.hpp>
#include <chrono>
#include <cstdint>
#include <future>
#include <limits>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "../../src/server/transport/event_http_listener.h"
#include "a2a/core/http_constants.h"
#include "a2a/server/engine/async_dispatcher.h"
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
constexpr std::string_view kChunkedEvent = "d\r\ndata: hello\n\n\r\n";
constexpr std::string_view kInitialEvent = "data: initial";
constexpr std::string_view kEventEnd = "\n\n";
constexpr std::size_t kInitialPadding = 64;
constexpr std::size_t kInitialOutputLimit = 128;
constexpr std::string_view kInitialHeader = "X-Initial";
constexpr std::size_t kInitialHeaderPadding = 41;
constexpr std::string_view kFinalChunk = "0\r\n\r\n";
constexpr auto kTimeout = std::chrono::seconds(5);
constexpr std::string_view kEchoCloseRequest = "POST / HTTP/1.1\r\nContent-Length: 4\r\nConnection: close\r\n\r\necho";
constexpr std::string_view kEchoPayload = "echo";
constexpr std::size_t kIdleClients = 64;
constexpr std::size_t kOneNetworkWorker = 1;
constexpr auto kTwoNetworkWorkers = EventHttpServerOptions::kMaxNetworkWorkers;
constexpr std::size_t kNetworkWorkerTestConnections = 8;

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

TEST(EventHttpServer, TerminalWriteReleasesPendingSlotBeforePipelinedRequest) {
  EventHttpServer server(Respond, {.max_pending_requests = 1});
  ASSERT_TRUE(server.Start().ok());
  asio::io_context io;
  auto peer = Connect(io, server);
  const std::string requests = std::string(kRequest).append(kCloseRequest);
  asio::write(peer, asio::buffer(requests));
  const auto response = ReadToEnd(peer);
  const auto first = response.find(kStatus);
  ASSERT_NE(first, std::string::npos);
  EXPECT_NE(response.find(kStatus, first + kStatus.size()), std::string::npos);
  EXPECT_EQ(response.find(kOverload), std::string::npos);
  server.Shutdown();
  server.Join();
}

TEST(EventHttpServer, OwnedRequestPreservesBodyAndConnectionClose) {
  EventHttpServer server(EventHttpServer::AsyncHandler(
      [](HttpServerRequest request,
         const engine::RequestExecutionContext& /*context*/) -> asio::awaitable<core::Result<EventHttpResponse>> {
        auto response = Respond(request).value();
        response.body = std::move(request.body);
        co_return EventHttpResponse{.response = std::move(response), .stream = {}};
      }));
  ASSERT_TRUE(server.Start().ok());
  asio::io_context io;
  auto peer = Connect(io, server);
  asio::write(peer, asio::buffer(kEchoCloseRequest));
  const auto response = ReadToEnd(peer);
  EXPECT_NE(response.find(kStatus), std::string::npos);
  EXPECT_TRUE(response.ends_with(kEchoPayload));
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
  EXPECT_NE(response.find(kChunkedEvent), std::string::npos);
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
constexpr std::string_view kDiscoveryName = "native-discovery";
constexpr std::string_view kDiscoveryRequest = "GET /.well-known/agent-card.json HTTP/1.1\r\nConnection: close\r\n\r\n";
constexpr std::string_view kListTasksRequest =
    "GET /a2a/tasks HTTP/1.1\r\nA2A-Version: 1.0\r\nConnection: close\r\n\r\n";

constexpr std::string_view kRpcPath = "/rpc";
constexpr std::string_view kRestStreamPath = "/a2a/message:stream";
constexpr std::string_view kEmptySubscription = "GET /a2a/tasks/empty:subscribe HTTP/1.1\r\nA2A-Version: 1.0\r\n\r\n";
constexpr std::string_view kFailingSubscription = "GET /a2a/tasks/fail:subscribe HTTP/1.1\r\nA2A-Version: 1.0\r\n\r\n";
constexpr std::string_view kBurstSubscription = "GET /a2a/tasks/burst:subscribe HTTP/1.1\r\nA2A-Version: 1.0\r\n\r\n";
constexpr std::string_view kBurstTaskId = "burst";
constexpr std::array<std::string_view, 3> kBurstEventIds{"first-event", "second-event", "third-event"};
constexpr std::string_view kRichTaskId = "rich";
constexpr std::string_view kRichTaskRequest =
    "GET /a2a/tasks/rich HTTP/1.1\r\nA2A-Version: 1.0\r\nConnection: close\r\n\r\n";
constexpr std::string_view kRichTaskArtifact = "rich-artifact";
constexpr std::string_view kShortStatusTask = "short-status";
constexpr std::string_view kLongStatusTask = "long-status";
constexpr std::string_view kShortStatusRequest =
    "GET /a2a/tasks/short-status HTTP/1.1\r\nA2A-Version: 1.0\r\nConnection: close\r\n\r\n";
constexpr std::string_view kLongStatusRequest =
    "GET /a2a/tasks/long-status HTTP/1.1\r\nA2A-Version: 1.0\r\nConnection: close\r\n\r\n";
constexpr std::string_view kStatusText = "working on the task";
constexpr std::size_t kLongStatusBytes = 1024;
constexpr std::string_view kIdleSubscription = "GET /a2a/tasks/idle:subscribe HTTP/1.1\r\nA2A-Version: 1.0\r\n\r\n";
constexpr std::string_view kIdleTaskId = "idle";
constexpr std::size_t kConcurrentNativeRequests = 2;
constexpr std::string_view kIncompleteNativeRequest = "Native request was incomplete";
constexpr std::string_view kMissingNativeReply = "Native reply did not complete";
constexpr std::string_view kFailingTaskId = "fail";
constexpr std::string_view kNativeStreamFailure = "native stream producer failed";
constexpr std::string_view kSseError = "event: error\n";

constexpr std::string_view kPostPrefix = "POST ";
constexpr std::string_view kPostHeaders =
    " HTTP/1.1\r\nA2A-Version: 1.0\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: ";

class NativeBackend final : public engine::CoroutineAgentExecutor {
 public:
  asio::awaitable<core::Result<lf::a2a::v1::Task>> GetTask(lf::a2a::v1::GetTaskRequest request,
                                                           engine::RequestExecutionContext /*context*/) override {
    lf::a2a::v1::Task task;
    task.set_id(request.id());
    if (request.id() == kRichTaskId) {
      task.add_artifacts()->set_artifact_id(std::string(kRichTaskArtifact));
    } else if (request.id() == kShortStatusTask || request.id() == kLongStatusTask) {
      auto* status = task.mutable_status();
      status->set_state(lf::a2a::v1::TASK_STATE_WORKING);
      auto* message = status->mutable_message();
      message->set_message_id(std::string(kNativeId));
      message->set_role(lf::a2a::v1::ROLE_AGENT);
      message->add_parts()->set_text(request.id() == kShortStatusTask ? std::string(kStatusText)
                                                                      : std::string(kLongStatusBytes, 'x'));
    }
    co_return task;
  }
  asio::awaitable<core::Result<ListTasksResponse>> ListTasks(ListTasksRequest /*request*/,
                                                             engine::RequestExecutionContext /*context*/) override {
    lf::a2a::v1::Task task;
    task.set_id(std::string(kNativeId));
    ListTasksResponse response;
    response.tasks.push_back(std::move(task));
    co_return response;
  }
  asio::awaitable<core::Result<void>> SendStreamingMessage(
      lf::a2a::v1::SendMessageRequest /*request*/, engine::RequestExecutionContext /*context*/,
      std::shared_ptr<engine::AsyncServerStreamSession> stream) override {
    lf::a2a::v1::StreamResponse event;
    event.mutable_task()->set_id(std::string(kNativeId));
    co_return co_await stream->Write(std::move(event));
  }

  asio::awaitable<core::Result<void>> SubscribeTask(lf::a2a::v1::GetTaskRequest request,
                                                    engine::RequestExecutionContext context,
                                                    std::shared_ptr<engine::AsyncServerStreamSession> stream) override {
    if (request.id() == kBurstTaskId) {
      for (const auto id : kBurstEventIds) {
        lf::a2a::v1::StreamResponse event;
        event.mutable_task()->set_id(std::string(id));
        auto written = co_await stream->Write(std::move(event));
        if (!written.ok()) {
          co_return written.error();
        }
      }
      co_return core::Result<void>{};
    }
    if (request.id() != kFailingTaskId && request.id() != kIdleTaskId) {
      co_return core::Result<void>{};
    }
    const auto written = co_await SendStreamingMessage({}, std::move(context), std::move(stream));
    if (!written.ok()) {
      co_return written.error();
    }
    if (request.id() == kIdleTaskId) {
      co_return co_await idle_.Wait();
    }
    co_return core::Error::Internal(std::string(kNativeStreamFailure));
  }

 private:
  test::CoroutineReply<void> idle_;
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
  EventHttpServer server(runtime.dispatcher(), {}, {});
  ASSERT_TRUE(server.Start().ok());
  asio::io_context io;
  const std::vector<std::string> requests{std::string(kRestTask), std::string(kListTasksRequest),
                                          Post(kRpcPath, kRpcBody), Post(kRpcPath, kRpcStreamBody),
                                          Post(kRestStreamPath, kRestStreamBody)};
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

void SubmitNativeRequest(const http_runtime::NativeHttpHandler& handler, std::string_view wire,
                         engine::RequestExecutionContext context, http_runtime::NativeHttpCompletion completion,
                         const http_runtime::NativeHttpWorkQueue& work,
                         http_runtime::NativeHttpStreamFactory stream_factory = {}) {
  std::size_t consumed = 0;
  auto parsed = HttpAdapter{}.ParseRequest(wire, consumed);
  if (!parsed.ok()) {
    ADD_FAILURE() << parsed.error().message();
    return;
  }
  auto& request = parsed.value();
  if (!request.has_value()) {
    ADD_FAILURE() << kIncompleteNativeRequest;
    return;
  }
  handler(std::move(*request), std::move(context), std::move(completion), std::move(stream_factory), work);
}

void ExpectNativeSerialization(std::string_view wire, bool offloaded, std::string_view expected) {
  asio::io_context dispatch;
  asio::io_context encoding;
  auto dispatcher =
      std::make_shared<engine::AsyncDispatcher>(std::make_shared<NativeBackend>(), dispatch.get_executor());
  const auto handler = http_runtime::EventHttpHandlerFactory::Native(dispatcher, {}, {});
  engine::RequestExecution connection;
  std::optional<core::Result<HttpServerResponse>> reply;
  SubmitNativeRequest(handler, wire, connection.Context(),
                      [&reply](core::Result<HttpServerResponse> result) { reply.emplace(std::move(result)); },
                      {.executor = encoding.get_executor()});
  (void)dispatch.run();
  EXPECT_EQ(reply.has_value(), !offloaded);
  (void)encoding.run();
  if (!reply.has_value()) {
    ADD_FAILURE() << kMissingNativeReply;
    return;
  }
  const auto& result = *reply;
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result.value().status_code, core::http::kStatusOk);
  EXPECT_NE(result.value().body.find(expected), std::string::npos);
}

TEST(EventHttpServer, NativeSmallTaskReplyEncodesOnDispatcher) {
  ExpectNativeSerialization(kRestTask, false, kNativeId);
}

TEST(EventHttpServer, NativeTaskWithArtifactsEncodesOnApplicationExecutor) {
  ExpectNativeSerialization(kRichTaskRequest, true, kRichTaskArtifact);
}

TEST(EventHttpServer, NativeSmallListReplyEncodesOnDispatcher) {
  ExpectNativeSerialization(kListTasksRequest, false, kNativeId);
}

TEST(EventHttpServer, NativeShortStatusMessageEncodesOnDispatcher) {
  ExpectNativeSerialization(kShortStatusRequest, false, kStatusText);
}

TEST(EventHttpServer, NativeLongStatusMessageEncodesOnApplicationExecutor) {
  ExpectNativeSerialization(kLongStatusRequest, true, kLongStatusTask);
}

TEST(EventHttpServer, NativeConcurrentUnaryRepliesOffloadUntilContentionClears) {
  asio::io_context dispatch;
  asio::io_context encoding;
  auto dispatcher =
      std::make_shared<engine::AsyncDispatcher>(std::make_shared<NativeBackend>(), dispatch.get_executor());
  const auto handler = http_runtime::EventHttpHandlerFactory::Native(dispatcher, {}, {});
  const http_runtime::NativeHttpWorkQueue work{.executor = encoding.get_executor()};
  std::array<engine::RequestExecution, kConcurrentNativeRequests> connections;
  std::size_t completed = 0;
  const auto complete = [&completed](const core::Result<HttpServerResponse>& result) {
    EXPECT_TRUE(result.ok());
    ++completed;
  };
  for (auto& connection : connections) {
    SubmitNativeRequest(handler, kRestTask, connection.Context(), complete, work);
  }
  (void)dispatch.run();
  EXPECT_EQ(completed, 0U);
  (void)encoding.run();
  EXPECT_EQ(completed, kConcurrentNativeRequests);
  dispatch.restart();
  engine::RequestExecution next;
  SubmitNativeRequest(handler, kRestTask, next.Context(), complete, work);
  (void)dispatch.run();
  EXPECT_EQ(completed, kConcurrentNativeRequests + 1);
}

class NativeStreamProbe final : public http_runtime::NativeHttpStream {
 public:
  asio::awaitable<core::Result<void>> Start(HttpServerResponse /*response*/, std::string /*first_event*/) override {
    started = true;
    co_return core::Result<void>{};
  }
  asio::awaitable<core::Result<void>> Write(std::string /*bytes*/) override { co_return core::Result<void>{}; }
  void Finish(std::string /*error_event*/) override {}
  void FinishEmpty(HttpServerResponse /*response*/) override {}
  void Abort() override {}

  bool started = false;
};

TEST(EventHttpServer, IdleNativeStreamLeavesUnarySerializationInline) {
  asio::io_context dispatch;
  asio::io_context encoding;
  auto dispatcher =
      std::make_shared<engine::AsyncDispatcher>(std::make_shared<NativeBackend>(), dispatch.get_executor());
  const auto handler = http_runtime::EventHttpHandlerFactory::Native(dispatcher, {}, {});
  engine::RequestExecution subscription;
  engine::RequestExecution unary;
  const auto stream = std::make_shared<NativeStreamProbe>();
  const http_runtime::NativeHttpWorkQueue work{.executor = encoding.get_executor()};
  SubmitNativeRequest(
      handler, kIdleSubscription, subscription.Context(), [](core::Result<HttpServerResponse>) {}, work,
      [stream] { return std::shared_ptr<http_runtime::NativeHttpStream>(stream); });
  (void)dispatch.poll();
  EXPECT_TRUE(stream->started);

  bool completed = false;
  SubmitNativeRequest(
      handler, kListTasksRequest, unary.Context(),
      [&completed](const core::Result<HttpServerResponse>& result) { completed = result.ok(); }, work);
  (void)dispatch.poll();
  EXPECT_TRUE(completed);
  (void)encoding.poll();
  dispatcher->Shutdown();
  (void)dispatch.run();
}

TEST(EventHttpServer, NativeNetworkWorkersServeConcurrentConnections) {
  engine::AsyncRuntime runtime(std::make_shared<NativeBackend>());
  EventHttpServer server(runtime.dispatcher(), {}, {},
                         {.network_workers = kTwoNetworkWorkers, .second_network_connection_threshold = 0});
  EXPECT_EQ(server.network_workers(), kTwoNetworkWorkers);
  ASSERT_TRUE(server.Start().ok());
  asio::io_context io;
  std::vector<Tcp::socket> peers;
  peers.reserve(kNetworkWorkerTestConnections);
  for (std::size_t i = 0; i < kNetworkWorkerTestConnections; ++i) {
    peers.push_back(Connect(io, server));
  }
  for (auto& peer : peers) {
    asio::write(peer, asio::buffer(kRestTask));
  }
  for (auto& peer : peers) {
    const auto response = ReadToEnd(peer);
    EXPECT_TRUE(response.starts_with(kStatus)) << response;
    EXPECT_NE(response.find(kNativeId), std::string::npos) << response;
  }
  server.Shutdown();
  server.Join();
}

TEST(EventHttpServer, NetworkWorkersRespectAutomaticAndExplicitSelection) {
  engine::AsyncRuntime runtime(std::make_shared<NativeBackend>());
  EventHttpServer automatic_dual(runtime.dispatcher(), {}, {}, {.network_auto_min_cpus = 0});
  EXPECT_EQ(automatic_dual.network_workers(), kTwoNetworkWorkers);
  EventHttpServer automatic_single(runtime.dispatcher(), {}, {},
                                   {.network_auto_min_cpus = std::numeric_limits<unsigned>::max()});
  EXPECT_EQ(automatic_single.network_workers(), kOneNetworkWorker);
  EventHttpServer explicit_single(runtime.dispatcher(), {}, {},
                                  {.network_workers = kOneNetworkWorker, .network_auto_min_cpus = 0});
  EXPECT_EQ(explicit_single.network_workers(), kOneNetworkWorker);
  EventHttpServer generic(Respond, {.network_auto_min_cpus = 0});
  EXPECT_EQ(generic.network_workers(), kOneNetworkWorker);
}

TEST(EventHttpServer, RejectsUnsupportedNetworkWorkerCount) {
  EventHttpServer server(Respond, {.network_workers = kTwoNetworkWorkers + kOneNetworkWorker});
  EXPECT_FALSE(server.Start().ok());
}

TEST(EventHttpServer, NativeDiscoveryCompletesWithoutDispatchAdmission) {
  engine::AsyncRuntime runtime(std::make_shared<NativeBackend>());
  lf::a2a::v1::AgentCard card;
  card.set_name(std::string(kDiscoveryName));
  EventHttpServer server(runtime.dispatcher(), std::move(card), {});
  ASSERT_TRUE(server.Start().ok());
  runtime.Shutdown();

  asio::io_context io;
  auto peer = Connect(io, server);
  asio::write(peer, asio::buffer(kDiscoveryRequest));
  const auto response = ReadToEnd(peer);
  EXPECT_TRUE(response.starts_with(kStatus)) << response;
  EXPECT_NE(response.find(kDiscoveryName), std::string::npos) << response;
  server.Shutdown();
  server.Join();
  runtime.Join();
}

std::string PipelineNativeSubscription(std::string_view subscription) {
  engine::AsyncRuntime runtime(std::make_shared<NativeBackend>());
  EventHttpServer server(runtime.dispatcher(), {}, {}, {.max_pending_requests = 1, .idle_timeout = kTimeout});
  if (const auto started = server.Start(); !started.ok()) {
    ADD_FAILURE() << started.error().message();
    return {};
  }
  asio::io_context io;
  auto peer = Connect(io, server);
  const auto requests = std::string(subscription).append(kRestTask);
  asio::write(peer, asio::buffer(requests));
  return ReadToEnd(peer);
}

void ExpectFinalChunkBeforeNextResponse(const std::string& response) {
  ASSERT_TRUE(response.starts_with(kStatus)) << response;
  const auto end = response.find(kFinalChunk);
  ASSERT_NE(end, std::string::npos) << response;
  const auto next = std::string_view(response).substr(end + kFinalChunk.size());
  EXPECT_TRUE(next.starts_with(kStatus)) << response;
  EXPECT_EQ(next.find(kFinalChunk), std::string::npos) << response;
}

TEST(EventHttpServer, NativeEmptyStreamTerminatesOnceBeforePipelinedResponse) {
  const auto response = PipelineNativeSubscription(kEmptySubscription);
  ExpectFinalChunkBeforeNextResponse(response);
  EXPECT_EQ(response.find(kFinalChunk),
            response.find(core::http::kHeaderDelimiter) + core::http::kHeaderDelimiter.size());
}

TEST(EventHttpServer, NativeStreamErrorTerminatesOnceBeforePipelinedResponse) {
  const auto response = PipelineNativeSubscription(kFailingSubscription);
  ExpectFinalChunkBeforeNextResponse(response);
  const auto error = response.find(kSseError);
  ASSERT_NE(error, std::string::npos) << response;
  EXPECT_LT(error, response.find(kFinalChunk)) << response;
}

TEST(EventHttpServer, NativeReadyEventsKeepWireOrderBeforePipelinedResponse) {
  const auto response = PipelineNativeSubscription(kBurstSubscription);
  ExpectFinalChunkBeforeNextResponse(response);
  std::size_t start = 0;
  for (const auto id : kBurstEventIds) {
    const auto event = response.find(id, start);
    ASSERT_NE(event, std::string::npos) << response;
    start = event + id.size();
  }
  EXPECT_LT(start, response.find(kFinalChunk)) << response;
}

struct DrainProbe final {
  ~DrainProbe() { dropped.set_value(); }
  std::promise<void> dropped;
};

struct DrainProbeHandler final {
  std::shared_ptr<DrainProbe> probe;

  asio::awaitable<core::Result<EventHttpResponse>> operator()(HttpServerRequest request,
                                                              engine::RequestExecutionContext /*context*/) const {
    co_return EventHttpResponse{.response = Respond(request).value(), .stream = {}};
  }
};

using ApplicationExecutor = asio::io_context::executor_type;

void ExpectHandlerRetainedAfterDrain(std::future<ApplicationExecutor>& executor, std::future<void>& probe_dropped,
                                     const std::weak_ptr<const HttpServerResponse>& handler_lifetime) {
  ASSERT_EQ(executor.wait_for(kTimeout), std::future_status::ready);
  ASSERT_EQ(probe_dropped.wait_for(kTimeout), std::future_status::ready);
  const auto marker = std::make_shared<std::promise<void>>();
  auto drained = marker->get_future();
  asio::post(executor.get(), [marker] { marker->set_value(); });
  ASSERT_EQ(drained.wait_for(kTimeout), std::future_status::ready);
  EXPECT_FALSE(handler_lifetime.expired());
}

TEST(EventHttpServer, ShutdownKeepsNativeHandlerAliveUntilInvocationReturns) {
  constexpr std::size_t kShutdownWorkers = 2;
  auto probe = std::make_shared<DrainProbe>();
  auto probe_dropped = probe->dropped.get_future();
  auto response = std::make_shared<const HttpServerResponse>(Respond({}).value());
  const std::weak_ptr<const HttpServerResponse> handler_lifetime = response;
  std::promise<ApplicationExecutor> application;
  auto executor = application.get_future();
  std::promise<void> release;
  auto released = release.get_future();
  http_runtime::NativeHttpHandler handler =
      [response = std::move(response), &application, &released](
          HttpServerRequest, engine::RequestExecutionContext, http_runtime::NativeHttpCompletion complete,
          const http_runtime::NativeHttpStreamFactory&, const http_runtime::NativeHttpWorkQueue& work) {
        auto& wait = released;
        application.set_value(work.executor);
        complete(*response);
        wait.wait();
      };
  http_runtime::EventHttpListener listener(DrainProbeHandler{.probe = std::move(probe)}, std::move(handler),
                                           {.application_threads = kShutdownWorkers, .idle_timeout = kTimeout});
  ASSERT_TRUE(listener.Start().ok());
  asio::io_context io;
  Tcp::socket peer(io);
  peer.connect(Tcp::endpoint(asio::ip::address_v4::loopback(), listener.port()));
  asio::write(peer, asio::buffer(kCloseRequest));
  EXPECT_TRUE(ReadToEnd(peer).starts_with(kStatus));
  listener.Shutdown();
  ExpectHandlerRetainedAfterDrain(executor, probe_dropped, handler_lifetime);
  release.set_value();
  listener.Join();
  EXPECT_TRUE(handler_lifetime.expired());
}

constexpr std::string_view kSubscribeRequest = "GET /a2a/tasks/native:subscribe HTTP/1.1\r\nA2A-Version: 1.0\r\n\r\n";
constexpr auto kBorrowedJoinObservation = std::chrono::milliseconds(50);

http_runtime::NativeHttpHandler BorrowedIngressHandler(asio::thread_pool::executor_type executor,
                                                       std::future<void>& released, std::atomic_bool& returned) {
  return [executor, &released, &returned](
             HttpServerRequest request, engine::RequestExecutionContext, http_runtime::NativeHttpCompletion complete,
             const http_runtime::NativeHttpStreamFactory&, const http_runtime::NativeHttpWorkQueue&) {
    EXPECT_TRUE(executor.running_in_this_thread());
    complete(Respond(request));
    released.wait();
    returned = true;
  };
}

TEST(EventHttpServer, JoinDrainsBorrowedJsonRpcRequestAfterItsResponseFinishes) {
  asio::thread_pool pool(engine::AsyncRuntimeOptions::kDefaultThreads);
  std::promise<void> release;
  auto released = release.get_future();
  std::atomic_bool returned = false;
  const auto executor = pool.get_executor();
  http_runtime::EventHttpListener listener({}, BorrowedIngressHandler(executor, released, returned),
                                           {.idle_timeout = kTimeout}, executor);
  ASSERT_TRUE(listener.Start().ok());
  asio::io_context io;
  Tcp::socket peer(io);
  peer.connect(Tcp::endpoint(asio::ip::address_v4::loopback(), listener.port()));
  const auto request = Post(kRpcPath, kRpcBody);
  asio::write(peer, asio::buffer(request));
  EXPECT_TRUE(ReadToEnd(peer).starts_with(kStatus));
  listener.Shutdown();
  auto joined = std::async(std::launch::async, [&listener, &returned] {
    listener.Join();
    return returned.load();
  });
  EXPECT_EQ(joined.wait_for(kBorrowedJoinObservation), std::future_status::timeout);
  release.set_value();
  ASSERT_EQ(joined.wait_for(kTimeout), std::future_status::ready);
  EXPECT_TRUE(joined.get());
  pool.join();
}

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

TEST(EventHttpServer, NativePendingLimitAppliesAcrossConnections) {
  constexpr std::size_t kPendingLimit = 1;
  constexpr std::size_t kIdlePeers = 4;
  const auto backend = std::make_shared<LiveBackend>();
  engine::AsyncRuntime runtime(backend);
  EventHttpServer server(runtime.dispatcher(), {}, {},
                         {.max_pending_requests = kPendingLimit, .idle_timeout = kTimeout});
  ASSERT_TRUE(server.Start().ok());
  asio::io_context io;
  std::vector<Tcp::socket> idle;
  idle.reserve(kIdlePeers);
  for (std::size_t index = 0; index < kIdlePeers; ++index) {
    idle.push_back(Connect(io, server));
  }
  auto stream = Connect(io, server);
  asio::write(stream, asio::buffer(kSubscribeRequest));
  std::string event;
  asio::read_until(stream, asio::dynamic_buffer(event), kEventEnd);
  EXPECT_NE(event.find(kNativeId), std::string::npos);
  auto unary = Connect(io, server);
  asio::write(unary, asio::buffer(kRestTask));
  EXPECT_NE(ReadToEnd(unary).find(kOverload), std::string::npos);
  server.Shutdown();
  server.Join();
  runtime.Join();
  EXPECT_EQ(backend->completed.load(), kPendingLimit);
}

TEST(EventHttpServer, LiveStreamsSuspendAndShutdownCleansEachSessionOnce) {
  const auto backend = std::make_shared<LiveBackend>();
  engine::AsyncRuntime runtime(backend, {}, {.threads = 1});
  EventHttpServerOptions options;
  options.application_threads = 1;
  EventHttpServer server(runtime.dispatcher(), {}, {}, options);
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
  EventHttpServer server(runtime.dispatcher(), {}, {});
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
  EventHttpServer server(runtime.dispatcher(), {}, {});
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
  EventHttpServer server(runtime.dispatcher(), {}, {}, options);
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
  EventHttpServer server(runtime.dispatcher(), {}, {}, options);
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
  asio::awaitable<core::Result<lf::a2a::v1::Task>> GetTask(
      lf::a2a::v1::GetTaskRequest request, [[maybe_unused]] engine::RequestExecutionContext context) override {
    const auto ready = co_await WaitForRelease();
    if (!ready.ok()) {
      co_return ready.error();
    }
    lf::a2a::v1::Task task;
    task.set_id(request.id());
    co_return task;
  }

  asio::awaitable<core::Result<void>> SubscribeTask(
      [[maybe_unused]] lf::a2a::v1::GetTaskRequest request, [[maybe_unused]] engine::RequestExecutionContext context,
      [[maybe_unused]] std::shared_ptr<engine::AsyncServerStreamSession> stream) override {
    co_return co_await WaitForRelease();
  }
  void Release() { reply_.Resolve(core::Result<void>{}); }
  std::promise<void> started;
  std::atomic_size_t completed{0};

 private:
  asio::awaitable<core::Result<void>> WaitForRelease() {
    started.set_value();
    const auto result = co_await reply_.Wait();
    ++completed;
    co_return result;
  }
  test::CoroutineReply<void> reply_{true};
};

TEST(EventHttpServer, NativeCleanupBeforeFirstEventMayOutliveListener) {
  const auto backend = std::make_shared<DelayedBackend>();
  auto started = backend->started.get_future();
  engine::AsyncRuntime runtime(backend);
  asio::io_context io;
  {
    EventHttpServer server(runtime.dispatcher(), {}, {});
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

TEST(EventHttpServer, NativeUnaryCleanupMayOutliveListener) {
  const auto backend = std::make_shared<DelayedBackend>();
  auto started = backend->started.get_future();
  engine::AsyncRuntime runtime(backend);
  asio::io_context io;
  {
    EventHttpServer server(runtime.dispatcher(), {}, {});
    ASSERT_TRUE(server.Start().ok());
    auto peer = Connect(io, server);
    asio::write(peer, asio::buffer(kRestTask));
    EXPECT_EQ(started.wait_for(kTimeout), std::future_status::ready);
    server.Shutdown();
    server.Join();
    EXPECT_EQ(backend->completed.load(), 0U);
  }
  backend->Release();
  runtime.Join();
  EXPECT_EQ(backend->completed.load(), 1U);
}
}  // namespace
}  // namespace a2a::server
