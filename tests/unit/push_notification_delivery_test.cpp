// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "a2a/server/push_notification_delivery.h"

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
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include "a2a/core/http_constants.h"

namespace {

constexpr std::string_view kTaskId = "task-1";
constexpr std::string_view kContextId = "ctx-1";
constexpr std::string_view kConfigId = "push-1";
constexpr std::string_view kAuthScheme = "Bearer";
constexpr std::string_view kCredentials = "credential-value";
constexpr std::string_view kMalformedUrl = "ftp://127.0.0.1/webhook";
constexpr std::string_view kMissingHostUrl = "http:///webhook";
constexpr std::string_view kMissingPortUrl = "http://127.0.0.1:/webhook";
constexpr std::string_view kUnresolvedUrl = "http://invalid.invalid/webhook";
constexpr std::string_view kHttpVersion10 = "HTTP/1.0";
constexpr std::string_view kHttpVersion11 = "HTTP/1.1";
constexpr std::string_view kHttpOkResponse = "HTTP/1.1 204 No Content\r\nContent-Length: 0\r\n\r\n";
constexpr std::string_view kHttpErrorResponse = "HTTP/1.1 500 Internal Server Error\r\nContent-Length: 0\r\n\r\n";
constexpr std::string_view kMalformedResponse = "not an http response\r\n\r\n";
constexpr int kExpectedHttpStatusNoContent = 204;
constexpr int kDeliveryTimeoutMs = 1000;
constexpr int kSocketError = -1;

lf::a2a::v1::StreamResponse BuildPayload() {
  lf::a2a::v1::StreamResponse payload;
  auto* update = payload.mutable_status_update();
  update->set_task_id(std::string(kTaskId));
  update->set_context_id(std::string(kContextId));
  update->mutable_status()->set_state(lf::a2a::v1::TASK_STATE_COMPLETED);
  return payload;
}

lf::a2a::v1::TaskPushNotificationConfig BuildConfig(std::string url) {
  lf::a2a::v1::TaskPushNotificationConfig config;
  config.set_task_id(std::string(kTaskId));
  config.set_id(std::string(kConfigId));
  config.set_url(std::move(url));
  config.mutable_authentication()->set_scheme(std::string(kAuthScheme));
  config.mutable_authentication()->set_credentials(std::string(kCredentials));
  return config;
}

#if !defined(_WIN32) && defined(A2A_HAS_LIBCURL)
std::string BuildLoopbackUrl(int port, std::string_view scheme = a2a::core::http::kHttpScheme) {
  std::string url;
  const std::string port_text = std::to_string(port);
  url.reserve(scheme.size() + std::string_view("127.0.0.1:").size() + port_text.size() +
              std::string_view("/webhook").size());
  url.append(scheme);
  url.append("127.0.0.1:");
  url.append(port_text);
  url.append("/webhook");
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

TEST(PushNotificationDeliveryTest, RejectsUnsupportedUrlScheme) {
  a2a::server::HttpPushNotificationDeliveryClient client{std::chrono::milliseconds(kDeliveryTimeoutMs)};
  const a2a::server::PushDeliveryRequest request{.config = BuildConfig(std::string(kMalformedUrl)),
                                                 .payload = BuildPayload()};

  const auto result = client.Deliver(request);

  EXPECT_FALSE(result.ok());
}

TEST(PushNotificationDeliveryTest, RejectsUrlWithoutHost) {
  a2a::server::HttpPushNotificationDeliveryClient client{std::chrono::milliseconds(kDeliveryTimeoutMs)};
  const a2a::server::PushDeliveryRequest request{.config = BuildConfig(std::string(kMissingHostUrl)),
                                                 .payload = BuildPayload()};

  const auto result = client.Deliver(request);

  EXPECT_FALSE(result.ok());
}

TEST(PushNotificationDeliveryTest, RejectsUrlWithoutPort) {
  a2a::server::HttpPushNotificationDeliveryClient client{std::chrono::milliseconds(kDeliveryTimeoutMs)};
  const a2a::server::PushDeliveryRequest request{.config = BuildConfig(std::string(kMissingPortUrl)),
                                                 .payload = BuildPayload()};

  const auto result = client.Deliver(request);

  EXPECT_FALSE(result.ok());
}

TEST(PushNotificationDeliveryTest, RejectsUnresolvedHost) {
  a2a::server::HttpPushNotificationDeliveryClient client{std::chrono::milliseconds(kDeliveryTimeoutMs)};
  const a2a::server::PushDeliveryRequest request{.config = BuildConfig(std::string(kUnresolvedUrl)),
                                                 .payload = BuildPayload()};

  const auto result = client.Deliver(request);

  EXPECT_FALSE(result.ok());
}

#if !defined(A2A_HAS_LIBCURL)
TEST(PushNotificationDeliveryTest, BuiltInHttpDeliveryReportsDisabledLibcurlSupport) {
  a2a::server::HttpPushNotificationDeliveryClient client{std::chrono::milliseconds(kDeliveryTimeoutMs)};
  const a2a::server::PushDeliveryRequest request{.config = BuildConfig("http://127.0.0.1:1/webhook"),
                                                 .payload = BuildPayload()};

  const auto result = client.Deliver(request);

  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().code(), a2a::core::ErrorCode::kInternal);
}
#endif

#if !defined(_WIN32) && defined(A2A_HAS_LIBCURL)
TEST(PushNotificationDeliveryTest, AttemptsHttpsDeliveryWithBuiltInTlsClient) {
  LoopbackHttpServer server{std::string(kHttpOkResponse)};
  a2a::server::HttpPushNotificationDeliveryOptions options;
  options.timeout = std::chrono::milliseconds(kDeliveryTimeoutMs);
  options.http_version = std::string(kHttpVersion11);
  options.fallback_http_version.clear();
  a2a::server::HttpPushNotificationDeliveryClient client(options);
  const a2a::server::PushDeliveryRequest request{
      .config = BuildConfig(BuildLoopbackUrl(server.port(), a2a::core::http::kHttpsScheme)), .payload = BuildPayload()};

  const auto result = client.Deliver(request);

  EXPECT_FALSE(result.ok());
  EXPECT_FALSE(server.request().empty());
}

TEST(PushNotificationDeliveryTest, DeliversJsonPayloadWithAuthorizationHeader) {
  LoopbackHttpServer server{std::string(kHttpOkResponse)};
  a2a::server::HttpPushNotificationDeliveryOptions options;
  options.timeout = std::chrono::milliseconds(kDeliveryTimeoutMs);
  options.http_version = std::string(kHttpVersion11);
  options.fallback_http_version.clear();
  a2a::server::HttpPushNotificationDeliveryClient client(options);
  const a2a::server::PushDeliveryRequest request{.config = BuildConfig(BuildLoopbackUrl(server.port())),
                                                 .payload = BuildPayload()};

  const auto result = client.Deliver(request);

  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result.value().http_status, kExpectedHttpStatusNoContent);
  EXPECT_NE(server.request().find("POST /webhook HTTP/1.1"), std::string::npos);
  EXPECT_NE(server.request().find("Authorization: Bearer credential-value"), std::string::npos);
  EXPECT_NE(server.request().find("Content-Type: application/json"), std::string::npos);
}

TEST(PushNotificationDeliveryTest, OmitsAuthorizationHeaderWhenSchemeIsEmpty) {
  LoopbackHttpServer server{std::string(kHttpOkResponse)};
  a2a::server::HttpPushNotificationDeliveryOptions options;
  options.timeout = std::chrono::milliseconds(kDeliveryTimeoutMs);
  options.http_version = std::string(kHttpVersion11);
  options.fallback_http_version.clear();
  a2a::server::HttpPushNotificationDeliveryClient client(options);
  auto config = BuildConfig(BuildLoopbackUrl(server.port()));
  config.mutable_authentication()->clear_scheme();
  const a2a::server::PushDeliveryRequest request{.config = config, .payload = BuildPayload()};

  const auto result = client.Deliver(request);

  ASSERT_TRUE(result.ok());
  EXPECT_EQ(server.request().find("Authorization:"), std::string::npos);
}

TEST(PushNotificationDeliveryTest, RejectsHttp10PrimaryVersion) {
  a2a::server::HttpPushNotificationDeliveryOptions options;
  options.timeout = std::chrono::milliseconds(kDeliveryTimeoutMs);
  options.http_version = std::string(kHttpVersion10);
  options.fallback_http_version = std::string(kHttpVersion11);
  a2a::server::HttpPushNotificationDeliveryClient client(options);
  const a2a::server::PushDeliveryRequest request{.config = BuildConfig(std::string(kUnresolvedUrl)),
                                                 .payload = BuildPayload()};

  const auto result = client.Deliver(request);

  EXPECT_FALSE(result.ok());
  EXPECT_EQ(result.error().code(), a2a::core::ErrorCode::kValidation);
}

TEST(PushNotificationDeliveryTest, RejectsHttp10FallbackVersion) {
  a2a::server::HttpPushNotificationDeliveryOptions options;
  options.timeout = std::chrono::milliseconds(kDeliveryTimeoutMs);
  options.http_version = std::string(kHttpVersion11);
  options.fallback_http_version = std::string(kHttpVersion10);
  a2a::server::HttpPushNotificationDeliveryClient client(options);
  const a2a::server::PushDeliveryRequest request{.config = BuildConfig(std::string(kUnresolvedUrl)),
                                                 .payload = BuildPayload()};

  const auto result = client.Deliver(request);

  EXPECT_FALSE(result.ok());
  EXPECT_EQ(result.error().code(), a2a::core::ErrorCode::kValidation);
}

TEST(PushNotificationDeliveryTest, RejectsNonSuccessWebhookStatus) {
  LoopbackHttpServer server{std::string(kHttpErrorResponse)};
  a2a::server::HttpPushNotificationDeliveryOptions options;
  options.timeout = std::chrono::milliseconds(kDeliveryTimeoutMs);
  options.http_version = std::string(kHttpVersion11);
  options.fallback_http_version.clear();
  a2a::server::HttpPushNotificationDeliveryClient client(options);
  const a2a::server::PushDeliveryRequest request{.config = BuildConfig(BuildLoopbackUrl(server.port())),
                                                 .payload = BuildPayload()};

  const auto result = client.Deliver(request);

  EXPECT_FALSE(result.ok());
}

TEST(PushNotificationDeliveryTest, RejectsMalformedWebhookResponse) {
  LoopbackHttpServer server{std::string(kMalformedResponse)};
  a2a::server::HttpPushNotificationDeliveryOptions options;
  options.timeout = std::chrono::milliseconds(kDeliveryTimeoutMs);
  options.http_version = std::string(kHttpVersion11);
  options.fallback_http_version.clear();
  a2a::server::HttpPushNotificationDeliveryClient client(options);
  const a2a::server::PushDeliveryRequest request{.config = BuildConfig(BuildLoopbackUrl(server.port())),
                                                 .payload = BuildPayload()};

  const auto result = client.Deliver(request);

  EXPECT_FALSE(result.ok());
}
#endif
