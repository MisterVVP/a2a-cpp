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

#include <array>
#include <chrono>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include "a2a/client/client.h"
#include "a2a/client/discovery.h"
#include "a2a/client/http_json_transport.h"
#include "a2a/client/json_rpc_transport.h"
#include "a2a/core/http_constants.h"

namespace {

constexpr int kSocketError = -1;
constexpr int kHttpOk = 200;
constexpr int kLoopbackTimeoutMs = 1000;
constexpr std::string_view kHttpVersion11 = "HTTP/1.1";
constexpr std::string_view kTaskId = "task-1";
constexpr std::string_view kResponseHeaderName = "X-Test-Header";
constexpr std::string_view kResponseHeaderValue = "captured";
constexpr std::string_view kA2aVersionHeader = "A2A-Version";
constexpr std::string_view kA2aVersionValue = "1.0";
constexpr std::string_view kRestTaskBody = R"({"id":"task-1"})";
constexpr std::string_view kJsonRpcTaskBody = R"({"jsonrpc":"2.0","id":"req-1","result":{"id":"task-1"}})";
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

class LoopbackHttpServer final {
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

  LoopbackHttpServer(const LoopbackHttpServer&) = delete;
  LoopbackHttpServer& operator=(const LoopbackHttpServer&) = delete;

  ~LoopbackHttpServer() {
    if (worker_.joinable()) {
      worker_.join();
    }
    if (fd_ != kSocketError) {
      ::close(fd_);
    }
  }

  [[nodiscard]] int port() const noexcept { return port_; }
  [[nodiscard]] const std::string& request() const noexcept { return request_; }

 private:
  void AcceptOnce() {
    const int client = ::accept(fd_, nullptr, nullptr);
    if (client == kSocketError) {
      return;
    }
    std::array<char, a2a::core::http::kReceiveBufferSize> buffer{};
    const auto received = ::recv(client, buffer.data(), buffer.size(), 0);
    if (received > 0) {
      request_.assign(buffer.data(), static_cast<std::size_t>(received));
    }
    (void)::send(client, response_.data(), response_.size(), 0);
    ::close(client);
  }

  int fd_ = kSocketError;
  int port_ = 0;
  std::string response_;
  std::string request_;
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
