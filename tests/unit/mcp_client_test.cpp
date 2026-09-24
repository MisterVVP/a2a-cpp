#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "gtest/gtest.h"
#include "mcp_client.h"

namespace {
constexpr int kSocketError = -1;
constexpr int kOk = 200;
constexpr int kAccepted = 202;
constexpr std::size_t kResponseCapacityOverhead = 128;
constexpr std::size_t kReceiveBufferSize = 4096;
constexpr auto kTimeout = std::chrono::seconds(2);
constexpr std::string_view kVersion = "2025-06-18";
constexpr std::string_view kUri = "fixture://resource";
constexpr std::string_view kText = "fixture text";

std::string HttpResponse(int status, std::string_view body, std::string_view extra_headers = {}) {
  std::string response;
  response.reserve(kResponseCapacityOverhead + body.size() + extra_headers.size());
  response.append("HTTP/1.1 ").append(std::to_string(status)).append(" Test\r\nContent-Type: application/json\r\n");
  response.append(extra_headers);
  response.append("Content-Length: ").append(std::to_string(body.size())).append("\r\nConnection: close\r\n\r\n");
  response.append(body);
  return response;
}

std::string InitializeResult(std::string_view version = kVersion, bool resources = true) {
  std::string body = R"({"jsonrpc":"2.0","id":0,"result":{"protocolVersion":")";
  body.append(version).append(R"(","capabilities":)");
  body.append(resources ? R"({"resources":{}})" : R"({})");
  body.append("}}");
  return body;
}

class ScriptedServer final {
 public:
  explicit ScriptedServer(std::vector<std::string> responses) : responses_(std::move(responses)) {
    socket_ = ::socket(AF_INET, SOCK_STREAM, 0);
    EXPECT_NE(socket_, kSocketError);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    EXPECT_EQ(::bind(socket_, reinterpret_cast<sockaddr*>(&address), sizeof(address)), 0);
    EXPECT_EQ(::listen(socket_, static_cast<int>(responses_.size())), 0);
    socklen_t size = sizeof(address);
    EXPECT_EQ(::getsockname(socket_, reinterpret_cast<sockaddr*>(&address), &size), 0);
    port_ = ntohs(address.sin_port);
    worker_ = std::thread([this] { Serve(); });
  }

  ScriptedServer(const ScriptedServer&) = delete;
  ScriptedServer& operator=(const ScriptedServer&) = delete;
  ~ScriptedServer() {
    if (worker_.joinable()) {
      worker_.join();
    }
    if (socket_ != kSocketError) {
      ::close(socket_);
    }
  }

  [[nodiscard]] std::string endpoint() const {
    std::string endpoint = "http://127.0.0.1:";
    endpoint.append(std::to_string(port_)).append("/mcp");
    return endpoint;
  }
  [[nodiscard]] std::vector<std::string> requests() const {
    std::scoped_lock lock(mutex_);
    return requests_;
  }

 private:
  static std::string ReadRequest(int client) {
    std::string request;
    std::array<char, kReceiveBufferSize> buffer{};
    while (request.find("\r\n\r\n") == std::string::npos) {
      const auto count = ::recv(client, buffer.data(), buffer.size(), 0);
      if (count <= 0) {
        return request;
      }
      request.append(buffer.data(), static_cast<std::size_t>(count));
    }
    const auto body_start = request.find("\r\n\r\n") + 4;
    const auto length_start = request.find("Content-Length:");
    if (length_start == std::string::npos) {
      return request;
    }
    const auto length = std::stoul(request.substr(length_start + std::string_view("Content-Length:").size()));
    while (request.size() - body_start < length) {
      const auto count = ::recv(client, buffer.data(), buffer.size(), 0);
      if (count <= 0) {
        break;
      }
      request.append(buffer.data(), static_cast<std::size_t>(count));
    }
    return request;
  }

  void Serve() {
    for (const auto& response : responses_) {
      const int client = ::accept(socket_, nullptr, nullptr);
      if (client == kSocketError) {
        return;
      }
      auto request = ReadRequest(client);
      {
        std::scoped_lock lock(mutex_);
        requests_.push_back(std::move(request));
      }
      (void)::send(client, response.data(), response.size(), 0);
      ::close(client);
    }
  }

  int socket_ = kSocketError;
  std::uint16_t port_ = 0;
  std::vector<std::string> responses_;
  mutable std::mutex mutex_;
  std::vector<std::string> requests_;
  std::thread worker_;
};

std::vector<std::string> SuccessfulExchange(std::string_view read_body) {
  return {HttpResponse(kOk, InitializeResult(), "mCp-SeSsIoN-iD: unit-session\r\n"),
          HttpResponse(kAccepted, {}), HttpResponse(kOk, read_body)};
}

TEST(McpClientTest, ReadsTextAndForwardsNegotiatedSession) {
  ScriptedServer server(SuccessfulExchange(
      R"({"jsonrpc":"2.0","id":1,"result":{"contents":[{"uri":"fixture://resource","text":"fixture text"}]}})"));
  const auto result = tutorial_mcp::Client(server.endpoint(), kTimeout).ReadResource(kUri);
  ASSERT_TRUE(result.ok()) << result.error().message();
  EXPECT_EQ(result.value(), kText);
  const auto requests = server.requests();
  ASSERT_EQ(requests.size(), 3U);
  EXPECT_NE(requests[0].find(R"("protocolVersion":"2025-06-18")"), std::string::npos);
  EXPECT_NE(requests[1].find("Mcp-Session-Id: unit-session"), std::string::npos);
  EXPECT_NE(requests[2].find("Mcp-Session-Id: unit-session"), std::string::npos);
  EXPECT_NE(requests[2].find(R"("uri":"fixture://resource")"), std::string::npos);
}

TEST(McpClientTest, RejectsMalformedInitializationEnvelope) {
  ScriptedServer server({HttpResponse(kOk, "not-json")});
  const auto result = tutorial_mcp::Client(server.endpoint(), kTimeout).ReadResource(kUri);
  ASSERT_FALSE(result.ok());
  EXPECT_NE(result.error().message().find("invalid JSON"), std::string::npos);
}

TEST(McpClientTest, RejectsUnsupportedVersion) {
  ScriptedServer server({HttpResponse(kOk, InitializeResult("unsupported"))});
  const auto result = tutorial_mcp::Client(server.endpoint(), kTimeout).ReadResource(kUri);
  ASSERT_FALSE(result.ok());
  EXPECT_NE(result.error().message().find("unsupported protocol version"), std::string::npos);
}

TEST(McpClientTest, RejectsMissingResourcesCapability) {
  ScriptedServer server({HttpResponse(kOk, InitializeResult(kVersion, false))});
  const auto result = tutorial_mcp::Client(server.endpoint(), kTimeout).ReadResource(kUri);
  ASSERT_FALSE(result.ok());
  EXPECT_NE(result.error().message().find("resources capability"), std::string::npos);
}

TEST(McpClientTest, RejectsInitializationNotificationFailure) {
  ScriptedServer server({HttpResponse(kOk, InitializeResult()), HttpResponse(kOk, "{}")});
  const auto result = tutorial_mcp::Client(server.endpoint(), kTimeout).ReadResource(kUri);
  ASSERT_FALSE(result.ok());
  EXPECT_NE(result.error().message().find("notification was not accepted"), std::string::npos);
}

TEST(McpClientTest, RejectsInvalidResourceContentShapes) {
  constexpr std::array<std::string_view, 3> invalid_results = {
      R"({"jsonrpc":"2.0","id":1,"result":{}})",
      R"({"jsonrpc":"2.0","id":1,"result":{"contents":[]}})",
      R"({"jsonrpc":"2.0","id":1,"result":{"contents":[{"blob":"AA=="}]}})"};
  for (const auto body : invalid_results) {
    ScriptedServer server(SuccessfulExchange(body));
    const auto result = tutorial_mcp::Client(server.endpoint(), kTimeout).ReadResource(kUri);
    EXPECT_FALSE(result.ok());
  }
}
}  // namespace
