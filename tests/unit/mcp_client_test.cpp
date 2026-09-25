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
constexpr int kAccepted = 202;
constexpr int kNoContent = 204;
constexpr int kInternalServerError = 500;
constexpr std::size_t kInvalidResourceValueCount = 4;
constexpr double kNumericResourceUri = 1.0;
constexpr std::size_t kResponseCapacityOverhead = 128;
constexpr std::size_t kReceiveBufferSize = 4096;
constexpr int kReceiveBufferLength = static_cast<int>(kReceiveBufferSize);
constexpr auto kTimeout = std::chrono::seconds(2);
constexpr std::string_view kVersion = "2025-06-18";
constexpr std::string_view kUri = "fixture://resource";
constexpr std::string_view kText = "fixture text";
constexpr std::string_view kResumeResourceField = "resume_resource";
constexpr std::string_view kTicketResourceField = "ticket_resource";

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

std::vector<std::string> SuccessfulExchange(std::string_view read_body) {
  return {HttpResponse(kOk, InitializeResult(), "mCp-SeSsIoN-iD: unit-session\r\n"), HttpResponse(kAccepted, {}),
          HttpResponse(kOk, read_body), HttpResponse(kNoContent, {})};
}

std::vector<std::string> InitializationExchange(std::string_view body) {
  return {HttpResponse(kOk, body, "Mcp-Session-Id: unit-session\r\n"), HttpResponse(kNoContent, {})};
}

TEST(McpClientTest, ReadsTextAndForwardsNegotiatedSession) {
  ScriptedServer server(SuccessfulExchange(
      R"({"jsonrpc":"2.0","id":1,"result":{"contents":[{"uri":"fixture://resource","text":"fixture text"}]}})"));
  const auto result = tutorial_mcp::Client(server.endpoint(), kTimeout).ReadResource(kUri);
  ASSERT_TRUE(result.ok()) << result.error().message();
  EXPECT_EQ(result.value(), kText);
  const auto requests = server.requests();
  ASSERT_EQ(requests.size(), 4U);
  EXPECT_NE(requests[0].find(R"("protocolVersion":"2025-06-18")"), std::string::npos);
  EXPECT_NE(requests[1].find("Mcp-Session-Id: unit-session"), std::string::npos);
  EXPECT_NE(requests[2].find("Mcp-Session-Id: unit-session"), std::string::npos);
  EXPECT_NE(requests[2].find(R"("uri":"fixture://resource")"), std::string::npos);
  EXPECT_EQ(requests[3].find("DELETE /mcp HTTP/1.1"), 0U);
  EXPECT_NE(requests[3].find("Mcp-Session-Id: unit-session"), std::string::npos);
}

TEST(McpClientTest, RejectsMalformedInitializationEnvelopeAndTerminatesSession) {
  ScriptedServer server(InitializationExchange("not-json"));
  const auto result = tutorial_mcp::Client(server.endpoint(), kTimeout).ReadResource(kUri);
  ASSERT_FALSE(result.ok());
  EXPECT_NE(result.error().message().find("invalid JSON"), std::string::npos);
  const auto requests = server.requests();
  ASSERT_EQ(requests.size(), 2U);
  EXPECT_EQ(requests[1].find("DELETE /mcp HTTP/1.1"), 0U);
}

TEST(McpClientTest, RejectsInitializationResponseWithoutJsonRpcVersion) {
  ScriptedServer server(
      InitializationExchange(R"({"id":0,"result":{"protocolVersion":"2025-06-18","capabilities":{"resources":{}}}})"));
  const auto result = tutorial_mcp::Client(server.endpoint(), kTimeout).ReadResource(kUri);
  ASSERT_FALSE(result.ok());
  EXPECT_NE(result.error().message().find("missing jsonrpc"), std::string::npos);
}

TEST(McpClientTest, RejectsUnsupportedJsonRpcVersion) {
  ScriptedServer server(InitializationExchange(
      R"({"jsonrpc":"1.0","id":0,"result":{"protocolVersion":"2025-06-18","capabilities":{"resources":{}}}})"));
  const auto result = tutorial_mcp::Client(server.endpoint(), kTimeout).ReadResource(kUri);
  ASSERT_FALSE(result.ok());
  EXPECT_NE(result.error().message().find("unsupported JSON-RPC version"), std::string::npos);
}

TEST(McpClientTest, RejectsMismatchedInitializationResponseId) {
  ScriptedServer server(InitializationExchange(
      R"({"jsonrpc":"2.0","id":99,"result":{"protocolVersion":"2025-06-18","capabilities":{"resources":{}}}})"));
  const auto result = tutorial_mcp::Client(server.endpoint(), kTimeout).ReadResource(kUri);
  ASSERT_FALSE(result.ok());
  EXPECT_NE(result.error().message().find("id does not match"), std::string::npos);
}

TEST(McpClientTest, RejectsInitializationResponseWithoutId) {
  ScriptedServer server(InitializationExchange(
      R"({"jsonrpc":"2.0","result":{"protocolVersion":"2025-06-18","capabilities":{"resources":{}}}})"));
  const auto result = tutorial_mcp::Client(server.endpoint(), kTimeout).ReadResource(kUri);
  ASSERT_FALSE(result.ok());
  EXPECT_NE(result.error().message().find("missing id"), std::string::npos);
}

TEST(McpClientTest, RejectsInvalidInitializationResponseIdType) {
  ScriptedServer server(InitializationExchange(
      R"({"jsonrpc":"2.0","id":"0","result":{"protocolVersion":"2025-06-18","capabilities":{"resources":{}}}})"));
  const auto result = tutorial_mcp::Client(server.endpoint(), kTimeout).ReadResource(kUri);
  ASSERT_FALSE(result.ok());
  EXPECT_NE(result.error().message().find("id must be a number"), std::string::npos);
}

TEST(McpClientTest, RejectsMismatchedResourceResponseId) {
  ScriptedServer server(SuccessfulExchange(
      R"({"jsonrpc":"2.0","id":99,"result":{"contents":[{"uri":"fixture://resource","text":"fixture text"}]}})"));
  const auto result = tutorial_mcp::Client(server.endpoint(), kTimeout).ReadResource(kUri);
  ASSERT_FALSE(result.ok());
  EXPECT_NE(result.error().message().find("id does not match"), std::string::npos);
}

TEST(McpClientTest, TerminatesSessionReturnedWithInitializationHttpError) {
  ScriptedServer server(
      {HttpResponse(kInternalServerError, {}, "Mcp-Session-Id: unit-session\r\n"), HttpResponse(kNoContent, {})});
  const auto result = tutorial_mcp::Client(server.endpoint(), kTimeout).ReadResource(kUri);
  ASSERT_FALSE(result.ok());
  EXPECT_NE(result.error().message().find("HTTP 500"), std::string::npos);
  const auto requests = server.requests();
  ASSERT_EQ(requests.size(), 2U);
  EXPECT_EQ(requests[1].find("DELETE /mcp HTTP/1.1"), 0U);
  EXPECT_NE(requests[1].find("Mcp-Session-Id: unit-session"), std::string::npos);
}

TEST(McpClientTest, RejectsUnsupportedVersion) {
  ScriptedServer server(InitializationExchange(InitializeResult("unsupported")));
  const auto result = tutorial_mcp::Client(server.endpoint(), kTimeout).ReadResource(kUri);
  ASSERT_FALSE(result.ok());
  EXPECT_NE(result.error().message().find("unsupported protocol version"), std::string::npos);
}

TEST(McpClientTest, RejectsMissingResourcesCapability) {
  ScriptedServer server(InitializationExchange(InitializeResult(kVersion, false)));
  const auto result = tutorial_mcp::Client(server.endpoint(), kTimeout).ReadResource(kUri);
  ASSERT_FALSE(result.ok());
  EXPECT_NE(result.error().message().find("resources capability"), std::string::npos);
}

TEST(McpClientTest, RejectsInitializationNotificationFailure) {
  ScriptedServer server({HttpResponse(kOk, InitializeResult(), "Mcp-Session-Id: unit-session\r\n"),
                         HttpResponse(kOk, "{}"), HttpResponse(kNoContent, {})});
  const auto result = tutorial_mcp::Client(server.endpoint(), kTimeout).ReadResource(kUri);
  ASSERT_FALSE(result.ok());
  EXPECT_NE(result.error().message().find("notification was not accepted"), std::string::npos);
}

TEST(McpClientTest, RejectsInvalidResourceContentShapes) {
  constexpr std::array<std::string_view, 3> invalid_results = {
      R"({"jsonrpc":"2.0","id":1,"result":{}})", R"({"jsonrpc":"2.0","id":1,"result":{"contents":[]}})",
      R"({"jsonrpc":"2.0","id":1,"result":{"contents":[{"blob":"AA=="}]}})"};
  for (const auto body : invalid_results) {
    ScriptedServer server(SuccessfulExchange(body));
    const auto result = tutorial_mcp::Client(server.endpoint(), kTimeout).ReadResource(kUri);
    EXPECT_FALSE(result.ok());
  }
}

TEST(McpClientTest, RejectsResourceContentWithoutUri) {
  ScriptedServer server(
      SuccessfulExchange(R"({"jsonrpc":"2.0","id":1,"result":{"contents":[{"text":"fixture text"}]}})"));
  const auto result = tutorial_mcp::Client(server.endpoint(), kTimeout).ReadResource(kUri);
  ASSERT_FALSE(result.ok());
  EXPECT_NE(result.error().message().find("content URI is missing"), std::string::npos);
}

TEST(McpClientTest, RejectsNonStringResourceContentUri) {
  ScriptedServer server(
      SuccessfulExchange(R"({"jsonrpc":"2.0","id":1,"result":{"contents":[{"uri":7,"text":"fixture text"}]}})"));
  const auto result = tutorial_mcp::Client(server.endpoint(), kTimeout).ReadResource(kUri);
  ASSERT_FALSE(result.ok());
  EXPECT_NE(result.error().message().find("content URI must be a string"), std::string::npos);
}

TEST(McpClientTest, RejectsMismatchedResourceContentUri) {
  ScriptedServer server(SuccessfulExchange(
      R"({"jsonrpc":"2.0","id":1,"result":{"contents":[{"uri":"fixture://different","text":"fixture text"}]}})"));
  const auto result = tutorial_mcp::Client(server.endpoint(), kTimeout).ReadResource(kUri);
  ASSERT_FALSE(result.ok());
  EXPECT_NE(result.error().message().find("content URI does not match"), std::string::npos);
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
}  // namespace
