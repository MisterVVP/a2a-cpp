#include "mcp_client.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

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
#include "resource_validation.h"

namespace {
#ifdef _WIN32
using Socket = SOCKET;
using SocketLength = int;
constexpr Socket kSocketError = INVALID_SOCKET;
#else
using Socket = int;
using SocketLength = socklen_t;
constexpr int kSocketError = -1;
#endif
constexpr int kOk = 200;
constexpr int kInternalServerError = 500;
constexpr std::size_t kInvalidResourceValueCount = 4;
constexpr std::size_t kInvalidResourceUriCount = 4;
constexpr double kNumericResourceUri = 1.0;
constexpr std::size_t kResponseCapacityOverhead = 128;
constexpr std::size_t kReceiveBufferSize = 4096;
constexpr int kReceiveBufferLength = static_cast<int>(kReceiveBufferSize);
constexpr auto kTimeout = std::chrono::seconds(2);
constexpr std::string_view kLoopbackEndpoint = "http://127.0.0.1/mcp";
constexpr std::string_view kUri = "fixture://resource";
constexpr std::string_view kText = "fixture text";
constexpr std::string_view kResumeResourceField = "resume_resource";
constexpr std::string_view kTicketResourceField = "ticket_resource";
constexpr std::string_view kJsonContentType = "application/json";
constexpr std::string_view kSseContentType = "text/event-stream; charset=utf-8";
constexpr std::string_view kSseEventPrefix = "event: message\ndata: ";
constexpr std::string_view kSseEventTerminator = "\n\n";
constexpr std::string_view kRemoteErrorMessage = "Resource not found";
constexpr std::string_view kRemoteErrorProtocolCode = "-32002";
constexpr std::string_view kMalformedErrorBody =
    R"({"jsonrpc":"2.0","id":1,"error":{"code":"invalid","message":"bad code"}})";
constexpr std::string_view kRemoteErrorBody =
    R"({"jsonrpc":"2.0","id":1,"error":{"code":-32002,"message":"Resource not found"}})";

void CloseSocket(Socket socket) {
#ifdef _WIN32
  (void)::closesocket(socket);
#else
  (void)::close(socket);
#endif
}

void ShutdownSocket(Socket socket) {
#ifdef _WIN32
  (void)::shutdown(socket, SD_BOTH);
#else
  (void)::shutdown(socket, SHUT_RDWR);
#endif
}

void SendResponse(Socket socket, std::string_view response) {
#ifdef _WIN32
  (void)::send(socket, response.data(), static_cast<int>(response.size()), 0);
#else
  (void)::send(socket, response.data(), response.size(), 0);
#endif
}

#ifdef _WIN32
bool StartSockets() {
  WSADATA wsa_data{};
  return ::WSAStartup(MAKEWORD(2, 2), &wsa_data) == 0;
}
#endif

std::string HttpResponse(int status, std::string_view body, std::string_view extra_headers = {},
                         std::string_view content_type = kJsonContentType) {
  std::string response;
  response.reserve(kResponseCapacityOverhead + body.size() + extra_headers.size());
  response.append("HTTP/1.1 ").append(std::to_string(status)).append(" Test\r\nContent-Type: ");
  response.append(content_type).append("\r\n");
  response.append(extra_headers);
  response.append("Content-Length: ").append(std::to_string(body.size())).append("\r\nConnection: close\r\n\r\n");
  response.append(body);
  return response;
}

class ScriptedServer final {
 public:
  explicit ScriptedServer(std::vector<std::string> responses) : responses_(std::move(responses)) {
#ifdef _WIN32
    winsock_started_ = StartSockets();
    EXPECT_TRUE(winsock_started_);
    if (!winsock_started_) {
      return;
    }
#endif
    socket_ = ::socket(AF_INET, SOCK_STREAM, 0);
    EXPECT_NE(socket_, kSocketError);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    EXPECT_EQ(::bind(socket_, reinterpret_cast<sockaddr*>(&address), static_cast<SocketLength>(sizeof(address))), 0);
    EXPECT_EQ(::listen(socket_, static_cast<int>(responses_.size())), 0);
    auto size = static_cast<SocketLength>(sizeof(address));
    EXPECT_EQ(::getsockname(socket_, reinterpret_cast<sockaddr*>(&address), &size), 0);
    port_ = ntohs(address.sin_port);
    worker_ = std::thread([this] { Serve(); });
  }

  ScriptedServer(const ScriptedServer&) = delete;
  ScriptedServer& operator=(const ScriptedServer&) = delete;
  ~ScriptedServer() {
    if (socket_ != kSocketError) {
      ShutdownSocket(socket_);
      CloseSocket(socket_);
    }
    if (worker_.joinable()) {
      worker_.join();
    }
    socket_ = kSocketError;
#ifdef _WIN32
    if (winsock_started_) {
      (void)::WSACleanup();
    }
#endif
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
  static std::string ReadRequest(Socket client) {
    std::string request;
    std::array<char, kReceiveBufferSize> buffer{};
    while (request.find("\r\n\r\n") == std::string::npos) {
      const auto count = ::recv(client, buffer.data(), kReceiveBufferLength, 0);
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
      const auto count = ::recv(client, buffer.data(), kReceiveBufferLength, 0);
      if (count <= 0) {
        break;
      }
      request.append(buffer.data(), static_cast<std::size_t>(count));
    }
    return request;
  }

  void Serve() {
    for (const auto& response : responses_) {
      const Socket client = ::accept(socket_, nullptr, nullptr);
      if (client == kSocketError) {
        return;
      }
      auto request = ReadRequest(client);
      {
        std::scoped_lock lock(mutex_);
        requests_.push_back(std::move(request));
      }
      SendResponse(client, response);
      CloseSocket(client);
    }
  }

  Socket socket_ = kSocketError;
  std::uint16_t port_ = 0;
  std::vector<std::string> responses_;
  mutable std::mutex mutex_;
  std::vector<std::string> requests_;
  std::thread worker_;
#ifdef _WIN32
  bool winsock_started_ = false;
#endif
};

std::string SuccessfulBody(std::string_view uri = kUri, std::string_view text = kText) {
  std::string body = R"({"jsonrpc":"2.0","id":1,"result":{"contents":[{"uri":")";
  body.append(uri).append(R"(","text":")").append(text).append(R"("}]}})");
  return body;
}

std::string SseBody(std::string_view data) {
  std::string body;
  body.reserve(kSseEventPrefix.size() + data.size() + kSseEventTerminator.size());
  body.append(kSseEventPrefix).append(data).append(kSseEventTerminator);
  return body;
}

a2a::core::Result<std::string> ReadFromResponse(std::string body, std::string_view token = "unit-secret") {
  ScriptedServer server({HttpResponse(kOk, body)});
  return tutorial_mcp::Client(server.endpoint(), std::string(token), kTimeout).ReadResource(kUri);
}

TEST(McpClientTest, SendsStatelessAuthenticatedProtocolRequestAndReadsText) {
  ScriptedServer server({HttpResponse(kOk, SuccessfulBody())});
  const auto result = tutorial_mcp::Client(server.endpoint(), "unit-secret", kTimeout).ReadResource(kUri);
  ASSERT_TRUE(result.ok()) << result.error().message();
  EXPECT_EQ(result.value(), kText);
  const auto requests = server.requests();
  ASSERT_EQ(requests.size(), 1U);
  EXPECT_NE(requests[0].find("MCP-Protocol-Version: 2025-11-25"), std::string::npos);
  EXPECT_NE(requests[0].find("Mcp-Method: resources/read"), std::string::npos);
  EXPECT_NE(requests[0].find("Mcp-Name: fixture://resource"), std::string::npos);
  EXPECT_NE(requests[0].find("Authorization: Bearer unit-secret"), std::string::npos);
  EXPECT_NE(requests[0].find(R"("io.modelcontextprotocol/protocolVersion":"2025-11-25")"), std::string::npos);
  EXPECT_NE(requests[0].find(R"("io.modelcontextprotocol/clientInfo")"), std::string::npos);
  EXPECT_NE(requests[0].find(R"("io.modelcontextprotocol/clientCapabilities":{})"), std::string::npos);
}

TEST(McpClientTest, RejectsInvalidResponses) {
  constexpr std::array<std::string_view, 6> invalid_responses = {
      "not-json",
      R"({"jsonrpc":"1.0","id":1,"result":{"contents":[]}})",
      R"({"jsonrpc":"2.0","id":7,"result":{"contents":[]}})",
      R"({"jsonrpc":"2.0","id":1,"result":{}})",
      R"({"jsonrpc":"2.0","id":1,"result":{"contents":[{"uri":"fixture://resource","text":""}]}})",
      kMalformedErrorBody};
  for (const auto response : invalid_responses) {
    EXPECT_FALSE(ReadFromResponse(std::string(response)).ok());
  }
}

TEST(McpClientTest, ReadsTextFromSseResponse) {
  ScriptedServer server({HttpResponse(kOk, SseBody(SuccessfulBody()), {}, kSseContentType)});
  const auto result = tutorial_mcp::Client(server.endpoint(), "unit-secret", kTimeout).ReadResource(kUri);
  ASSERT_TRUE(result.ok()) << result.error().message();
  EXPECT_EQ(result.value(), kText);
}

TEST(McpClientTest, PreservesJsonRpcErrorCodeAndMessage) {
  const auto result = ReadFromResponse(std::string(kRemoteErrorBody));
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().code(), a2a::core::ErrorCode::kRemoteProtocol);
  EXPECT_EQ(result.error().message(), kRemoteErrorMessage);
  ASSERT_TRUE(result.error().protocol_code().has_value());
  EXPECT_EQ(*result.error().protocol_code(), kRemoteErrorProtocolCode);
}

TEST(McpClientTest, RejectsMismatchedResourceUri) {
  const auto result = ReadFromResponse(SuccessfulBody("fixture://different"));
  ASSERT_FALSE(result.ok());
  EXPECT_NE(result.error().message().find("URI does not match"), std::string::npos);
}

TEST(McpClientTest, RequiresUrlAndTokenWithoutExposingToken) {
  const auto missing_url = tutorial_mcp::Client("", "top-secret", kTimeout).ReadResource(kUri);
  ASSERT_FALSE(missing_url.ok());
  EXPECT_EQ(missing_url.error().message().find("top-secret"), std::string::npos);
  const auto missing_token = tutorial_mcp::Client("http://127.0.0.1/mcp", "", kTimeout).ReadResource(kUri);
  ASSERT_FALSE(missing_token.ok());
  EXPECT_NE(missing_token.error().message().find("token is required"), std::string::npos);
}

TEST(McpClientTest, RejectsResourceUriControlCharactersBeforeSendingRequest) {
  constexpr std::string_view kInjectedUri = "fixture://resource\r\nX-Injected: value";
  const auto result =
      tutorial_mcp::Client(std::string(kLoopbackEndpoint), "unit-secret", kTimeout).ReadResource(kInjectedUri);
  ASSERT_FALSE(result.ok());
  EXPECT_NE(result.error().message().find("control characters"), std::string::npos);
}

TEST(McpClientTest, ReportsHttpFailureWithoutExposingToken) {
  ScriptedServer server({HttpResponse(kInternalServerError, "failure")});
  const auto result = tutorial_mcp::Client(server.endpoint(), "top-secret", kTimeout).ReadResource(kUri);
  ASSERT_FALSE(result.ok());
  EXPECT_NE(result.error().message().find("HTTP 500"), std::string::npos);
  EXPECT_EQ(result.error().message().find("top-secret"), std::string::npos);
}

TEST(ResourceValidationTest, AcceptsNonEmptyStringUri) {
  google::protobuf::Value value;
  value.set_string_value(std::string(kUri));
  EXPECT_TRUE(tutorial_mcp::ValidateResourceUriField(value, kResumeResourceField).ok());
}

TEST(ResourceValidationTest, RejectsEmptyOrNonStringUri) {
  std::array<google::protobuf::Value, kInvalidResourceValueCount> invalid_values;
  invalid_values[0].set_string_value("");
  invalid_values[1].set_number_value(kNumericResourceUri);
  invalid_values[2].set_bool_value(true);
  invalid_values[3].set_null_value(google::protobuf::NULL_VALUE);
  for (const auto& value : invalid_values) {
    const auto result = tutorial_mcp::ValidateResourceUriField(value, kTicketResourceField);
    ASSERT_FALSE(result.ok());
    EXPECT_NE(result.error().message().find("ticket_resource must be a non-empty string"), std::string::npos);
  }
}

TEST(ResourceValidationTest, RejectsUriControlCharacters) {
  constexpr std::array<std::string_view, kInvalidResourceUriCount> invalid_uris = {
      "fixture://resource\rheader", "fixture://resource\nheader", "fixture://resource\theader",
      std::string_view{"fixture://resource\x7f", 19}};
  for (const auto uri : invalid_uris) {
    google::protobuf::Value value;
    value.set_string_value(std::string(uri));
    const auto result = tutorial_mcp::ValidateResourceUriField(value, kTicketResourceField);
    ASSERT_FALSE(result.ok());
    EXPECT_NE(result.error().message().find("control characters"), std::string::npos);
  }
}
}  // namespace
