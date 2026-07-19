// SPDX-License-Identifier: Apache-2.0

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <cerrno>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "a2a/core/agent_card/agent_card_builder.h"
#include "a2a/server/agent_executor.h"
#include "a2a/server/dispatcher.h"
#include "a2a/server/http_adapter.h"
#include "a2a/server/network_utils.h"
#include "a2a/server/rest_server_transport.h"
#include "a2a/server/server_stream_session.h"

namespace {

constexpr char kDefaultEndpoint[] = "127.0.0.1:8080";
constexpr char kRestBasePath[] = "/a2a";
constexpr char kTaskId[] = "example-task-1";
constexpr char kContextId[] = "example-context-1";
constexpr char kAgentReply[] = "ack";
constexpr int kListenBacklog = 16;
constexpr int kReuseAddress = 1;
volatile std::sig_atomic_t kKeepRunning = 1;

void SignalHandler(int signal_number) {
  (void)signal_number;
  kKeepRunning = 0;
}

void PrintUsage(std::string_view program_name) {
  std::cout << "Usage: " << program_name << " [host:port]\n"
            << "Starts an HTTP+JSON SSE example server.\n"
            << "Default endpoint: " << kDefaultEndpoint << "\n"
            << "REST endpoint: http://<host>:<port>" << kRestBasePath << "\n";
}

class SocketTransport final : public a2a::server::HttpByteTransport {
 public:
  explicit SocketTransport(int socket_fd) : socket_fd_(socket_fd) {}

  a2a::core::Result<std::size_t> Read(char* buffer, std::size_t size) override {
    const auto bytes = ::recv(socket_fd_, buffer, static_cast<int>(size), 0);
    if (bytes < 0) {
      return a2a::core::Error::Internal("Socket receive failed");
    }
    return static_cast<std::size_t>(bytes);
  }

  a2a::core::Result<std::size_t> Write(const char* buffer, std::size_t size) override {
    const auto bytes = ::send(socket_fd_, buffer, static_cast<int>(size), 0);
    if (bytes < 0) {
      return a2a::core::Error::Internal("Socket send failed");
    }
    return static_cast<std::size_t>(bytes);
  }

 private:
  int socket_fd_;
};

class OneShotStreamSession final : public a2a::server::ServerStreamSession {
 public:
  explicit OneShotStreamSession(lf::a2a::v1::StreamResponse event) : event_(std::move(event)) {}

  a2a::core::Result<std::optional<lf::a2a::v1::StreamResponse>> Next() override {
    if (sent_) {
      return std::optional<lf::a2a::v1::StreamResponse>{};
    }
    sent_ = true;
    return std::optional<lf::a2a::v1::StreamResponse>{event_};
  }

 private:
  lf::a2a::v1::StreamResponse event_;
  bool sent_ = false;
};

class ExampleExecutor final : public a2a::server::AgentExecutor {
 public:
  a2a::core::Result<lf::a2a::v1::SendMessageResponse> SendMessage(
      const lf::a2a::v1::SendMessageRequest& request, a2a::server::RequestContext& context) override {
    (void)context;
    if (!request.has_message()) {
      return a2a::core::Error::Validation("message is required");
    }

    task_.set_id(request.message().task_id().empty() ? kTaskId : request.message().task_id());
    task_.set_context_id(request.message().context_id().empty() ? kContextId : request.message().context_id());
    task_.mutable_status()->set_state(lf::a2a::v1::TASK_STATE_COMPLETED);
    task_.mutable_status()->mutable_message()->set_role(lf::a2a::v1::ROLE_AGENT);
    task_.mutable_status()->mutable_message()->add_parts()->set_text(kAgentReply);

    lf::a2a::v1::SendMessageResponse response;
    *response.mutable_task() = task_;
    return response;
  }

  a2a::core::Result<std::unique_ptr<a2a::server::ServerStreamSession>> SendStreamingMessage(
      const lf::a2a::v1::SendMessageRequest& request, a2a::server::RequestContext& context) override {
    auto sent = SendMessage(request, context);
    if (!sent.ok()) {
      return sent.error();
    }

    lf::a2a::v1::StreamResponse event;
    event.mutable_status_update()->set_task_id(task_.id());
    event.mutable_status_update()->set_context_id(task_.context_id());
    event.mutable_status_update()->mutable_status()->set_state(lf::a2a::v1::TASK_STATE_COMPLETED);
    std::unique_ptr<a2a::server::ServerStreamSession> stream = std::make_unique<OneShotStreamSession>(std::move(event));
    return stream;
  }

 private:
  lf::a2a::v1::Task task_;
};

int CreateListeningSocket(std::string_view host, int port) {
  const int server_fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (server_fd < 0) {
    return -1;
  }

  int reuse_address = kReuseAddress;
  if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse_address),
                 sizeof(reuse_address)) != 0) {
    a2a::server::CloseSocketCrossPlatform(server_fd);
    return -1;
  }

  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(static_cast<std::uint16_t>(port));
  const std::string host_text(host);
  if (inet_pton(AF_INET, host_text.c_str(), &address.sin_addr) != 1 ||
      bind(server_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
      listen(server_fd, kListenBacklog) != 0) {
    a2a::server::CloseSocketCrossPlatform(server_fd);
    return -1;
  }

  return server_fd;
}

void HandleConnection(int client_fd, const a2a::server::RestServerTransport& server) {
  SocketTransport socket_transport(client_fd);
  const a2a::server::HttpAdapter adapter;
  auto request = adapter.ReadRequest(socket_transport, "localhost");
  if (!request.ok()) {
    std::cerr << "Failed to read request: " << request.error().message() << '\n';
    return;
  }

  auto response = server.Handle(request.value());
  if (!response.ok()) {
    std::cerr << "Failed to handle request: " << response.error().message() << '\n';
    return;
  }

  const auto written = a2a::server::HttpAdapter::WriteResponse(socket_transport, response.value());
  if (!written.ok()) {
    std::cerr << "Failed to write response: " << written.error().message() << '\n';
  }
}

int RunServer(std::string_view endpoint) {
  auto parsed_endpoint = a2a::server::ParseHostPortEndpoint(endpoint);
  if (!parsed_endpoint.ok()) {
    std::cerr << parsed_endpoint.error().message() << '\n';
    return 1;
  }

#ifdef _WIN32
  WSADATA wsa_data;
  if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
    std::cerr << "Failed to initialize Winsock\n";
    return 1;
  }
#endif

  const auto& host = parsed_endpoint.value().host;
  const int port = parsed_endpoint.value().port;
  const int server_fd = CreateListeningSocket(host, port);
  if (server_fd < 0) {
    std::cerr << "Failed to listen on " << host << ':' << port << ": " << std::strerror(errno) << '\n';
#ifdef _WIN32
    WSACleanup();
#endif
    return 1;
  }

  std::signal(SIGINT, SignalHandler);
  std::signal(SIGTERM, SignalHandler);

  ExampleExecutor executor;
  a2a::server::Dispatcher dispatcher(&executor);
  auto agent_card =
      a2a::core::AgentCardBuilder::RestPreset("Streaming Example Agent", a2a::server::BuildHttpUrl(host, port, kRestBasePath))
          .Build();
  agent_card.mutable_capabilities()->set_streaming(true);
  a2a::server::RestServerTransport server(
      &dispatcher, std::move(agent_card),
      {.rest_api_base_path = kRestBasePath, .require_version_header = true, .include_legacy_transport_fields = false});

  std::cout << "streaming_server listening on " << a2a::server::BuildHttpUrl(host, port, kRestBasePath) << '\n';
  std::cout << "Press Ctrl+C to stop.\n";

  while (kKeepRunning != 0) {
    sockaddr_in client_address{};
    socklen_t client_address_size = sizeof(client_address);
    const int client_fd = accept(server_fd, reinterpret_cast<sockaddr*>(&client_address), &client_address_size);
    if (client_fd < 0) {
      if (errno == EINTR) {
        continue;
      }
      std::cerr << "Accept failed: " << std::strerror(errno) << '\n';
      break;
    }
    HandleConnection(client_fd, server);
    a2a::server::CloseSocketCrossPlatform(client_fd);
  }

  a2a::server::CloseSocketCrossPlatform(server_fd);
#ifdef _WIN32
  WSACleanup();
#endif
  return 0;
}

}  // namespace

int main(int argc, char** argv) noexcept {
  try {
    if (argc > 1 && std::string_view(argv[1]) == "--help") {
      PrintUsage(argv[0]);
      return 0;
    }
    if (argc > 2) {
      PrintUsage(argv[0]);
      return 1;
    }
    return RunServer(argc == 2 ? std::string_view(argv[1]) : std::string_view(kDefaultEndpoint));
  } catch (const std::exception& exception) {
    std::cerr << "streaming_server failed: " << exception.what() << '\n';
    return 1;
  }
}
