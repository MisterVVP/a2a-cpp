// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "a2a/http/http_client.h"

#include <gtest/gtest.h>

#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include "a2a/client/client.h"
#include "a2a/client/discovery.h"
#include "a2a/client/http_json_transport.h"
#include "a2a/client/json_rpc_transport.h"
#include "a2a/core/http_constants.h"
#include "a2a/core/non_copyable.h"

namespace {

constexpr int kSocketError = -1;
constexpr int kHttpOk = 200;
constexpr int kLoopbackTimeoutMs = 1000;
constexpr int kStreamTimeoutMs = 5000;
constexpr int kShortStreamTimeoutMs = 100;
constexpr int kCancellationRequestTimeoutMs = 30000;
constexpr std::chrono::milliseconds kHangPollInterval{10};
constexpr std::chrono::milliseconds kCancellationDeadline{1000};
constexpr std::string_view kHttpVersion11 = "HTTP/1.1";
constexpr std::string_view kTaskId = "task-1";
constexpr std::string_view kResponseHeaderName = "X-Test-Header";
constexpr std::string_view kResponseHeaderValue = "captured";
constexpr std::string_view kA2aVersionHeader = "A2A-Version";
constexpr std::string_view kA2aVersionValue = "1.0";
constexpr std::string_view kRestTaskBody = R"({"id":"task-1"})";
constexpr std::string_view kJsonRpcTaskBody = R"({"jsonrpc":"2.0","id":"req-1","result":{"id":"task-1"}})";
constexpr std::string_view kSseHeaders =
    "HTTP/1.1 200 OK\r\nA2A-Version: 1.0\r\nContent-Type: text/event-stream\r\nConnection: close\r\n\r\n";
constexpr std::string_view kEmptySseResponse =
    "HTTP/1.1 200 OK\r\nA2A-Version: 1.0\r\nContent-Type: text/event-stream\r\nContent-Length: "
    "0\r\nConnection: close\r\n\r\n";
constexpr std::string_view kFirstSseChunk = "data: first\n\n";
constexpr std::string_view kSecondSseChunk = "data: second\n\n";
constexpr std::string_view kAgentCardBody =
    R"({"supportedInterfaces":[{"protocolBinding":"HTTP+JSON","protocolVersion":"1.0","url":"https://agent.example.com/a2a"}]})";

#if !defined(_WIN32) && defined(A2A_HAS_LIBCURL)
std::string BuildHttpResponse(std::string_view body, std::string_view status = "200 OK") {
  std::string response;
  const std::string content_length = std::to_string(body.size());
  response.reserve(std::string_view("HTTP/1.1 ").size() + status.size() + body.size() + content_length.size() +
                   std::string_view("\r\n: \r\n: \r\nContent-Length: \r\nConnection: close\r\n\r\n").size() +
                   kA2aVersionHeader.size() + kA2aVersionValue.size() + kResponseHeaderName.size() +
                   kResponseHeaderValue.size());
  response.append("HTTP/1.1 ");
  response.append(status);
  response.append("\r\n");
  response.append(kA2aVersionHeader);
  response.append(": ");
  response.append(kA2aVersionValue);
  response.append("\r\n");
  response.append(kResponseHeaderName);
  response.append(": ");
  response.append(kResponseHeaderValue);
  response.append("\r\nContent-Length: ");
  response.append(content_length);
  response.append("\r\nConnection: close\r\n\r\n");
  response.append(body);
  return response;
}

std::string BuildLoopbackUrl(int port, std::string_view scheme = a2a::core::http::kHttpScheme,
                             std::string_view path = "/a2a") {
  const std::string port_text = std::to_string(port);
  std::string url;
  url.reserve(scheme.size() + std::string_view("127.0.0.1:").size() + port_text.size() + path.size());
  url.append(scheme);
  url.append("127.0.0.1:");
  url.append(port_text);
  url.append(path);
  return url;
}

class LoopbackHttpServer final : private a2a::core::NonCopyable {
 public:
  explicit LoopbackHttpServer(std::string response) : response_(std::move(response)) {
    fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    EXPECT_NE(fd_, kSocketError);
    int reuse = 1;
    EXPECT_EQ(::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, static_cast<socklen_t>(sizeof(reuse))), 0);

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    EXPECT_EQ(::bind(fd_, reinterpret_cast<sockaddr*>(&address), static_cast<socklen_t>(sizeof(address))), 0);
    EXPECT_EQ(::listen(fd_, 1), 0);

    sockaddr_in bound_address{};
    auto bound_size = static_cast<socklen_t>(sizeof(bound_address));
    EXPECT_EQ(::getsockname(fd_, reinterpret_cast<sockaddr*>(&bound_address), &bound_size), 0);
    port_ = static_cast<int>(ntohs(bound_address.sin_port));

    worker_ = std::thread([this] { AcceptOnce(); });
  }

  ~LoopbackHttpServer() {
    if (worker_.joinable()) {
      worker_.join();
    }
    if (fd_ != kSocketError) {
      ::close(fd_);
    }
  }

  [[nodiscard]] int port() const noexcept { return port_; }

  [[nodiscard]] std::string request() const {
    std::lock_guard lock(mutex_);
    return request_;
  }

  [[nodiscard]] bool WaitForRequest(std::chrono::milliseconds timeout) {
    std::unique_lock lock(mutex_);
    return cv_.wait_for(lock, timeout, [this] { return request_observed_; });
  }

 private:
  void AcceptOnce() {
    const int client = ::accept(fd_, nullptr, nullptr);
    if (client == kSocketError) {
      return;
    }
    auto request = ReadCompleteRequest(client);
    {
      std::lock_guard lock(mutex_);
      request_ = std::move(request);
      request_observed_ = true;
    }
    cv_.notify_all();
    (void)::send(client, response_.data(), response_.size(), 0);
    ::close(client);
  }

  static std::string ReadCompleteRequest(int client) {
    std::string request;
    std::array<char, a2a::core::http::kReceiveBufferSize> buffer{};
    while (request.find("\r\n\r\n") == std::string::npos) {
      const auto received = ::recv(client, buffer.data(), buffer.size(), 0);
      if (received <= 0) {
        return request;
      }
      request.append(buffer.data(), static_cast<std::size_t>(received));
    }
    const auto headers_end = request.find("\r\n\r\n");
    const auto length_header = request.find("Content-Length:");
    if (length_header == std::string::npos) {
      return request;
    }
    const auto value_start = length_header + std::string_view("Content-Length:").size();
    const auto value_end = request.find("\r\n", value_start);
    const auto content_length =
        static_cast<std::size_t>(std::stoul(request.substr(value_start, value_end - value_start)));
    const auto body_start = headers_end + std::string_view("\r\n\r\n").size();
    while (request.size() - body_start < content_length) {
      const auto received = ::recv(client, buffer.data(), buffer.size(), 0);
      if (received <= 0) {
        return request;
      }
      request.append(buffer.data(), static_cast<std::size_t>(received));
    }
    return request;
  }

  int fd_ = kSocketError;
  int port_ = 0;
  std::string response_;
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::string request_;
  bool request_observed_ = false;
  std::thread worker_;
};

class ConcurrentSseLoopbackServer final : private a2a::core::NonCopyable {
 public:
  ConcurrentSseLoopbackServer() {
    fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    EXPECT_NE(fd_, kSocketError);
    int reuse = 1;
    EXPECT_EQ(::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, static_cast<socklen_t>(sizeof(reuse))), 0);

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    EXPECT_EQ(::bind(fd_, reinterpret_cast<sockaddr*>(&address), static_cast<socklen_t>(sizeof(address))), 0);
    EXPECT_EQ(::listen(fd_, 2), 0);

    sockaddr_in bound_address{};
    auto bound_size = static_cast<socklen_t>(sizeof(bound_address));
    EXPECT_EQ(::getsockname(fd_, reinterpret_cast<sockaddr*>(&bound_address), &bound_size), 0);
    port_ = static_cast<int>(ntohs(bound_address.sin_port));

    worker_ = std::thread([this] { AcceptTwoStreams(); });
  }

  ~ConcurrentSseLoopbackServer() {
    ReleaseFirstStream();
    if (worker_.joinable()) {
      worker_.join();
    }
    if (fd_ != kSocketError) {
      ::close(fd_);
    }
  }

  [[nodiscard]] int port() const noexcept { return port_; }

  bool WaitForFirstStream(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mu_);
    return cv_.wait_for(lock, timeout, [this] { return first_stream_open_; });
  }

  bool WaitForSecondStream(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mu_);
    return cv_.wait_for(lock, timeout, [this] { return second_stream_open_; });
  }

  void ReleaseFirstStream() {
    {
      std::lock_guard<std::mutex> lock(mu_);
      release_first_stream_ = true;
    }
    cv_.notify_all();
  }

  [[nodiscard]] bool first_stream_closed() const noexcept { return first_stream_closed_.load(); }

 private:
  static void ReadRequest(int client) {
    std::array<char, a2a::core::http::kReceiveBufferSize> buffer{};
    (void)::recv(client, buffer.data(), buffer.size(), 0);
  }

  void AcceptTwoStreams() {
    const int first = ::accept(fd_, nullptr, nullptr);
    if (first == kSocketError) {
      return;
    }
    first_worker_ = std::thread([this, first] { HandleFirstStream(first); });

    const int second = ::accept(fd_, nullptr, nullptr);
    if (second != kSocketError) {
      HandleSecondStream(second);
    }
    if (first_worker_.joinable()) {
      first_worker_.join();
    }
  }

  void HandleFirstStream(int client) {
    ReadRequest(client);
    (void)::send(client, kSseHeaders.data(), kSseHeaders.size(), 0);
    (void)::send(client, kFirstSseChunk.data(), kFirstSseChunk.size(), 0);
    {
      std::lock_guard<std::mutex> lock(mu_);
      first_stream_open_ = true;
    }
    cv_.notify_all();
    {
      std::unique_lock<std::mutex> lock(mu_);
      cv_.wait(lock, [this] { return release_first_stream_; });
    }
    first_stream_closed_.store(true);
    ::close(client);
  }

  void HandleSecondStream(int client) {
    ReadRequest(client);
    (void)::send(client, kSseHeaders.data(), kSseHeaders.size(), 0);
    (void)::send(client, kSecondSseChunk.data(), kSecondSseChunk.size(), 0);
    {
      std::lock_guard<std::mutex> lock(mu_);
      second_stream_open_ = true;
    }
    cv_.notify_all();
    ::close(client);
  }

  int fd_ = kSocketError;
  int port_ = 0;
  mutable std::mutex mu_;
  std::condition_variable cv_;
  bool first_stream_open_ = false;
  bool second_stream_open_ = false;
  bool release_first_stream_ = false;
  std::atomic_bool first_stream_closed_{false};
  std::thread worker_;
  std::thread first_worker_;
};

class OpenSseLoopbackServer final : private a2a::core::NonCopyable {
 public:
  OpenSseLoopbackServer() {
    fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    EXPECT_NE(fd_, kSocketError);
    int reuse = 1;
    EXPECT_EQ(::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, static_cast<socklen_t>(sizeof(reuse))), 0);

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    EXPECT_EQ(::bind(fd_, reinterpret_cast<sockaddr*>(&address), static_cast<socklen_t>(sizeof(address))), 0);
    EXPECT_EQ(::listen(fd_, 1), 0);

    sockaddr_in bound_address{};
    auto bound_size = static_cast<socklen_t>(sizeof(bound_address));
    EXPECT_EQ(::getsockname(fd_, reinterpret_cast<sockaddr*>(&bound_address), &bound_size), 0);
    port_ = static_cast<int>(ntohs(bound_address.sin_port));
    worker_ = std::thread([this] { AcceptAndKeepOpen(); });
  }

  ~OpenSseLoopbackServer() {
    Release();
    if (fd_ != kSocketError) {
      ::shutdown(fd_, SHUT_RDWR);
    }
    if (worker_.joinable()) {
      worker_.join();
    }
    if (fd_ != kSocketError) {
      ::close(fd_);
    }
  }

  [[nodiscard]] int port() const noexcept { return port_; }

  [[nodiscard]] bool WaitForOpenStream(std::chrono::milliseconds timeout) {
    std::unique_lock lock(mutex_);
    return cv_.wait_for(lock, timeout, [this] { return stream_open_; });
  }

  [[nodiscard]] bool released() const noexcept { return released_.load(); }

 private:
  void Release() {
    released_.store(true);
    cv_.notify_all();
  }

  void AcceptAndKeepOpen() {
    const int client = ::accept(fd_, nullptr, nullptr);
    if (client == kSocketError) {
      return;
    }
    std::array<char, a2a::core::http::kReceiveBufferSize> buffer{};
    (void)::recv(client, buffer.data(), buffer.size(), 0);
    (void)::send(client, kSseHeaders.data(), kSseHeaders.size(), 0);
    {
      std::lock_guard lock(mutex_);
      stream_open_ = true;
    }
    cv_.notify_all();
    {
      std::unique_lock lock(mutex_);
      cv_.wait(lock, [this] { return released_.load(); });
    }
    ::close(client);
  }

  int fd_ = kSocketError;
  int port_ = 0;
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  bool stream_open_ = false;
  std::atomic_bool released_{false};
  std::thread worker_;
};

class HangingLoopbackServer final : private a2a::core::NonCopyable {
 public:
  HangingLoopbackServer() {
    fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    EXPECT_NE(fd_, kSocketError);
    int reuse = 1;
    EXPECT_EQ(::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, static_cast<socklen_t>(sizeof(reuse))), 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    EXPECT_EQ(::bind(fd_, reinterpret_cast<sockaddr*>(&address), static_cast<socklen_t>(sizeof(address))), 0);
    EXPECT_EQ(::listen(fd_, 1), 0);
    sockaddr_in bound_address{};
    auto bound_size = static_cast<socklen_t>(sizeof(bound_address));
    EXPECT_EQ(::getsockname(fd_, reinterpret_cast<sockaddr*>(&bound_address), &bound_size), 0);
    port_ = static_cast<int>(ntohs(bound_address.sin_port));
    worker_ = std::thread([this] { AcceptAndHang(); });
  }

  ~HangingLoopbackServer() {
    release_.store(true);
    if (fd_ != kSocketError) {
      ::shutdown(fd_, SHUT_RDWR);
    }
    if (worker_.joinable()) {
      worker_.join();
    }
    if (fd_ != kSocketError) {
      ::close(fd_);
    }
  }

  [[nodiscard]] int port() const noexcept { return port_; }

 private:
  void AcceptAndHang() {
    const int client = ::accept(fd_, nullptr, nullptr);
    if (client == kSocketError) {
      return;
    }
    std::array<char, a2a::core::http::kReceiveBufferSize> buffer{};
    (void)::recv(client, buffer.data(), buffer.size(), 0);
    while (!release_.load()) {
      std::this_thread::sleep_for(kHangPollInterval);
    }
    ::close(client);
  }

  int fd_ = kSocketError;
  int port_ = 0;
  std::atomic_bool release_{false};
  std::thread worker_;
};

#endif

}  // namespace

#if !defined(_WIN32) && defined(A2A_HAS_LIBCURL)
TEST(SharedHttpClientTest, CapturesResponseHeaders) {
  LoopbackHttpServer server(BuildHttpResponse(kRestTaskBody));
  a2a::http::Client client;
  a2a::http::Request request;
  request.method = "GET";
  request.url = BuildLoopbackUrl(server.port());
  request.timeout = std::chrono::milliseconds(kLoopbackTimeoutMs);
  request.http_version = std::string(kHttpVersion11);

  const auto response = client.SendRequest(request);

  ASSERT_TRUE(response.ok()) << response.error().message();
  EXPECT_EQ(response.value().status_code, kHttpOk);
  ASSERT_GE(response.value().headers.size(), 2U);
  EXPECT_EQ(response.value().headers.front().name, kA2aVersionHeader);
  EXPECT_EQ(response.value().headers.front().value, kA2aVersionValue);
  EXPECT_EQ(response.value().body, kRestTaskBody);
}

TEST(SharedHttpClientTest, AttemptsHttpsThroughLibcurlTlsPath) {
  LoopbackHttpServer server(BuildHttpResponse(kRestTaskBody));
  a2a::http::Client client;
  a2a::http::Request request;
  request.method = "GET";
  request.url = BuildLoopbackUrl(server.port(), a2a::core::http::kHttpsScheme);
  request.timeout = std::chrono::milliseconds(kLoopbackTimeoutMs);
  request.http_version = std::string(kHttpVersion11);

  const auto response = client.SendRequest(request);

  EXPECT_FALSE(response.ok());
  EXPECT_TRUE(server.WaitForRequest(std::chrono::milliseconds(kLoopbackTimeoutMs)));
  EXPECT_FALSE(server.request().empty());
}

TEST(DefaultHttpRequesterTest, RestTransportUsesSharedLibcurlRequester) {
  LoopbackHttpServer server(BuildHttpResponse(kRestTaskBody));
  a2a::client::ResolvedInterface resolved;
  resolved.transport = a2a::client::PreferredTransport::kRest;
  resolved.url = BuildLoopbackUrl(server.port());
  auto transport = a2a::client::HttpJsonTransport::CreateDefault(resolved);
  a2a::client::A2AClient client(std::move(transport));
  lf::a2a::v1::GetTaskRequest request;
  request.set_id(std::string(kTaskId));

  const auto response = client.GetTask(request);

  ASSERT_TRUE(response.ok()) << response.error().message();
  EXPECT_EQ(response.value().id(), kTaskId);
  EXPECT_NE(server.request().find("GET /a2a/tasks/task-1 HTTP/1.1"), std::string::npos);
}

TEST(DefaultHttpRequesterTest, JsonRpcTransportUsesSharedLibcurlRequester) {
  LoopbackHttpServer server(BuildHttpResponse(kJsonRpcTaskBody));
  a2a::client::ResolvedInterface resolved;
  resolved.transport = a2a::client::PreferredTransport::kJsonRpc;
  resolved.url = BuildLoopbackUrl(server.port(), a2a::core::http::kHttpScheme, "/rpc");
  auto transport = a2a::client::JsonRpcTransport::CreateDefault(
      resolved, a2a::client::JsonRpcTransport::kDefaultTimeout, [] { return "req-1"; });
  a2a::client::A2AClient client(std::move(transport));
  lf::a2a::v1::GetTaskRequest request;
  request.set_id(std::string(kTaskId));

  const auto response = client.GetTask(request);

  ASSERT_TRUE(response.ok()) << response.error().message();
  EXPECT_EQ(response.value().id(), kTaskId);
  EXPECT_NE(server.request().find("POST /rpc HTTP/1.1"), std::string::npos);
}

TEST(DefaultHttpFetcherTest, DiscoveryUsesSharedLibcurlFetcher) {
  LoopbackHttpServer server(BuildHttpResponse(kAgentCardBody));
  auto client = a2a::client::DiscoveryClient::CreateDefault();

  const auto response = client.Fetch(BuildLoopbackUrl(server.port(), a2a::core::http::kHttpScheme, ""));

  ASSERT_TRUE(response.ok()) << response.error().message();
  ASSERT_EQ(response.value().supported_interfaces().size(), 1);
  EXPECT_NE(server.request().find("GET /.well-known/agent-card.json HTTP/1.1"), std::string::npos);
}

TEST(SharedHttpClientTest, StreamRequestTimesOutOpenStream) {
  HangingLoopbackServer server;
  a2a::http::Client client;
  a2a::http::Request request;
  request.method = "GET";
  request.url = BuildLoopbackUrl(server.port(), a2a::core::http::kHttpScheme, "/stream");
  request.timeout = std::chrono::milliseconds(kShortStreamTimeoutMs);
  request.http_version = std::string(kHttpVersion11);

  const auto response = client.StreamRequest(
      request, [](const a2a::http::Response&) -> a2a::core::Result<void> { return {}; },
      [](std::string_view) -> a2a::core::Result<void> { return {}; }, [] { return false; });

  EXPECT_FALSE(response.ok());
}

struct StreamRequestCapture final {
  bool metadata_before_chunk = false;
  bool metadata_received = false;
  std::string chunks;
};

[[nodiscard]] a2a::core::Result<a2a::http::Response> ExecuteCapturedStreamRequest(a2a::http::Client& client,
                                                                                  const a2a::http::Request& request,
                                                                                  StreamRequestCapture& capture) {
  return client.StreamRequest(
      request,
      [&capture](const a2a::http::Response& metadata) -> a2a::core::Result<void> {
        capture.metadata_received = true;
        if (metadata.status_code != kHttpOk) {
          return a2a::core::Error::Internal("unexpected stream metadata status");
        }
        return {};
      },
      [&capture](std::string_view chunk) -> a2a::core::Result<void> {
        capture.metadata_before_chunk = capture.metadata_received;
        capture.chunks.append(chunk);
        return {};
      },
      [] { return false; });
}

[[nodiscard]] std::future<a2a::core::Result<a2a::http::Response>> StartCancellableStreamRequest(
    a2a::http::Client* client, const a2a::http::Request& request, std::atomic_bool* cancelled,
    std::atomic_int* chunks) {
  return std::async(std::launch::async, [client, request, cancelled, chunks] {
    return client->StreamRequest(
        request,
        [](const a2a::http::Response& metadata) -> a2a::core::Result<void> {
          if (metadata.status_code != kHttpOk) {
            return a2a::core::Error::Internal("unexpected stream metadata status");
          }
          return {};
        },
        [chunks](std::string_view) -> a2a::core::Result<void> {
          chunks->fetch_add(1);
          return {};
        },
        [cancelled] { return cancelled->load(); });
  });
}

TEST(SharedHttpClientTest, StreamRequestStopsPromptlyWhenCancellationCallbackChanges) {
  OpenSseLoopbackServer server;
  a2a::http::Client client;
  a2a::http::Request request;
  request.method = "GET";
  request.url = BuildLoopbackUrl(server.port(), a2a::core::http::kHttpScheme, "/stream");
  request.timeout = std::chrono::milliseconds(kCancellationRequestTimeoutMs);
  request.http_version = std::string(kHttpVersion11);

  std::atomic_bool cancelled{false};
  std::atomic_int chunks{0};
  auto response_future = StartCancellableStreamRequest(&client, request, &cancelled, &chunks);

  ASSERT_TRUE(server.WaitForOpenStream(std::chrono::milliseconds(kStreamTimeoutMs)));
  EXPECT_FALSE(server.released());
  const auto cancellation_started = std::chrono::steady_clock::now();
  cancelled.store(true);
  ASSERT_EQ(response_future.wait_for(kCancellationDeadline), std::future_status::ready);
  const auto cancellation_elapsed = std::chrono::steady_clock::now() - cancellation_started;
  const auto response = response_future.get();

  EXPECT_LT(cancellation_elapsed, kCancellationDeadline);
  EXPECT_FALSE(response.ok());
  EXPECT_FALSE(server.released());
  EXPECT_EQ(chunks.load(), 0);
}

TEST(SharedHttpClientTest, StreamRequestSendsFullRequestAndHandlesFragmentedResponse) {
  LoopbackHttpServer server{std::string(kSseHeaders) + "data: fir" + "st\n\n"};
  a2a::http::Client client;
  a2a::http::Request request;
  request.method = "POST";
  request.url = BuildLoopbackUrl(server.port(), a2a::core::http::kHttpScheme, "/stream");
  request.headers = {{.name = "X-Stream-Test", .value = "yes"}};
  request.body = R"({"hello":"world"})";
  request.timeout = std::chrono::milliseconds(kStreamTimeoutMs);
  request.http_version = std::string(kHttpVersion11);

  StreamRequestCapture capture;
  const auto response = ExecuteCapturedStreamRequest(client, request, capture);

  ASSERT_TRUE(response.ok()) << response.error().message();
  EXPECT_TRUE(capture.metadata_before_chunk);
  EXPECT_NE(capture.chunks.find(kFirstSseChunk), std::string::npos);
  EXPECT_NE(server.request().find("POST /stream HTTP/1.1"), std::string::npos);
  EXPECT_NE(server.request().find("X-Stream-Test: yes"), std::string::npos);
  EXPECT_NE(server.request().find(R"({"hello":"world"})"), std::string::npos);
}

TEST(SharedHttpClientTest, StreamRequestPropagatesHandlerFailuresAndConnectionFailure) {
  LoopbackHttpServer metadata_server{std::string(kSseHeaders)};
  a2a::http::Client client;
  a2a::http::Request request;
  request.method = "GET";
  request.url = BuildLoopbackUrl(metadata_server.port(), a2a::core::http::kHttpScheme, "/stream");
  request.timeout = std::chrono::milliseconds(kStreamTimeoutMs);
  request.http_version = std::string(kHttpVersion11);

  const auto metadata_failure = client.StreamRequest(
      request,
      [](const a2a::http::Response&) -> a2a::core::Result<void> {
        return a2a::core::Error::Internal("metadata failed");
      },
      [](std::string_view) -> a2a::core::Result<void> { return {}; }, [] { return false; });
  EXPECT_FALSE(metadata_failure.ok());

  LoopbackHttpServer chunk_server{std::string(kSseHeaders) + std::string(kFirstSseChunk)};
  request.url = BuildLoopbackUrl(chunk_server.port(), a2a::core::http::kHttpScheme, "/stream");
  const auto chunk_failure = client.StreamRequest(
      request, [](const a2a::http::Response&) -> a2a::core::Result<void> { return {}; },
      [](std::string_view) -> a2a::core::Result<void> { return a2a::core::Error::Internal("chunk failed"); },
      [] { return false; });
  EXPECT_FALSE(chunk_failure.ok());

  request.url = BuildLoopbackUrl(metadata_server.port(), a2a::core::http::kHttpScheme, "/stream");
  const auto connection_failure = client.StreamRequest(
      request, [](const a2a::http::Response&) -> a2a::core::Result<void> { return {}; },
      [](std::string_view) -> a2a::core::Result<void> { return {}; }, [] { return false; });
  EXPECT_FALSE(connection_failure.ok());
}

TEST(SharedHttpClientTest, EmptyStreamStillDeliversMetadata) {
  LoopbackHttpServer server{std::string(kEmptySseResponse)};
  a2a::http::Client client;
  a2a::http::Request request;
  request.method = "GET";
  request.url = BuildLoopbackUrl(server.port(), a2a::core::http::kHttpScheme, "/stream");
  request.timeout = std::chrono::milliseconds(kStreamTimeoutMs);
  request.http_version = std::string(kHttpVersion11);

  bool metadata_received = false;
  bool chunk_received = false;
  const auto response = client.StreamRequest(
      request,
      [&metadata_received](const a2a::http::Response& metadata) -> a2a::core::Result<void> {
        metadata_received = true;
        EXPECT_EQ(metadata.status_code, kHttpOk);
        return {};
      },
      [&chunk_received](std::string_view) -> a2a::core::Result<void> {
        chunk_received = true;
        return {};
      },
      [] { return false; });

  ASSERT_TRUE(response.ok()) << response.error().message();
  EXPECT_TRUE(metadata_received);
  EXPECT_FALSE(chunk_received);
  EXPECT_EQ(response.value().status_code, kHttpOk);
}

TEST(SharedHttpClientTest, ConcurrentStreamsDoNotSerializeBehindSharedEasyHandle) {
  ConcurrentSseLoopbackServer server;
  a2a::http::Client client;
  a2a::http::Request request;
  request.method = "GET";
  request.url = BuildLoopbackUrl(server.port(), a2a::core::http::kHttpScheme, "/stream");
  request.timeout = std::chrono::milliseconds(kStreamTimeoutMs);
  request.http_version = std::string(kHttpVersion11);

  std::atomic_bool first_cancelled{false};
  bool second_received_chunk = false;
  std::mutex second_mu;
  std::condition_variable second_cv;
  auto metadata = [](const a2a::http::Response&) -> a2a::core::Result<void> { return {}; };
  auto first_worker = std::thread([&] {
    (void)client.StreamRequest(
        request, metadata, [](std::string_view) -> a2a::core::Result<void> { return {}; },
        [&first_cancelled] { return first_cancelled.load(); });
  });

  ASSERT_TRUE(server.WaitForFirstStream(std::chrono::milliseconds(kStreamTimeoutMs)));
  ASSERT_FALSE(server.first_stream_closed());

  auto second_worker = std::thread([&] {
    (void)client.StreamRequest(
        request, metadata,
        [&second_received_chunk, &second_mu, &second_cv](std::string_view chunk) -> a2a::core::Result<void> {
          if (chunk.find(kSecondSseChunk) != std::string_view::npos) {
            {
              std::lock_guard<std::mutex> lock(second_mu);
              second_received_chunk = true;
            }
            second_cv.notify_all();
          }
          return {};
        },
        [] { return false; });
  });

  EXPECT_TRUE(server.WaitForSecondStream(std::chrono::milliseconds(kStreamTimeoutMs)));
  {
    std::unique_lock<std::mutex> lock(second_mu);
    EXPECT_TRUE(second_cv.wait_for(lock, std::chrono::milliseconds(kStreamTimeoutMs),
                                   [&second_received_chunk] { return second_received_chunk; }));
  }
  EXPECT_FALSE(server.first_stream_closed());

  server.ReleaseFirstStream();
  first_cancelled.store(true);
  second_worker.join();
  first_worker.join();
}

#else
TEST(SharedHttpClientTest, SendRequestReportsDisabledLibcurlSupport) {
  a2a::http::Client client;
  a2a::http::Request request;
  request.method = "GET";
  request.url = "http://127.0.0.1/";

  const auto response = client.SendRequest(request);

  ASSERT_FALSE(response.ok());
  EXPECT_EQ(response.error().code(), a2a::core::ErrorCode::kInternal);
  EXPECT_EQ(response.error().transport().value_or(""), "http");
}
#endif
