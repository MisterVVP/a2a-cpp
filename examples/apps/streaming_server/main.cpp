// SPDX-License-Identifier: Apache-2.0

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <cerrno>
#include <chrono>
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
#include <thread>
#include <utility>

#include "a2a/core/agent_card/agent_card_builder.h"
#include "a2a/server/agent_executor.h"
#include "a2a/server/dispatcher.h"
#include "a2a/server/http_adapter.h"
#include "a2a/server/network_utils.h"
#include "a2a/server/rest_server_transport.h"
#include "a2a/server/server_stream_session.h"

namespace {

#ifdef _WIN32
using SocketHandle = SOCKET;
using SocketLength = int;
constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;
#else
using SocketHandle = int;
using SocketLength = socklen_t;
constexpr SocketHandle kInvalidSocket = -1;
#endif

constexpr char kDefaultEndpoint[] = "127.0.0.1:8080";
constexpr char kRestBasePath[] = "/a2a";
constexpr char kTaskId[] = "example-task-1";
constexpr char kContextId[] = "example-context-1";
constexpr char kStatusMessageId[] = "example-status-message-1";
constexpr char kAgentReply[] = "ack";
constexpr char kTaskIdRequiredMessage[] = "task id is required";
constexpr char kTaskNotFoundMessage[] = "task not found";
constexpr int kListenBacklog = 16;
constexpr int kReuseAddress = 1;
constexpr auto kAcceptPollInterval = std::chrono::milliseconds(50);
volatile std::sig_atomic_t kKeepRunning = 1;

void SignalHandler(int signal_number) {
  (void)signal_number;
  kKeepRunning = 0;
}

void CloseSocket(SocketHandle socket) noexcept {
  if (socket == kInvalidSocket) {
    return;
  }
#ifdef _WIN32
  closesocket(socket);
#else
  close(socket);
#endif
}

bool SetNonBlocking(SocketHandle socket) noexcept {
#ifdef _WIN32
  u_long enabled = 1;
  return ioctlsocket(socket, FIONBIO, &enabled) == 0;
#else
  const int flags = fcntl(socket, F_GETFL, 0);
  return flags >= 0 && fcntl(socket, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

bool IsWouldBlockError() noexcept {
#ifdef _WIN32
  return WSAGetLastError() == WSAEWOULDBLOCK;
#else
  return errno == EAGAIN || errno == EWOULDBLOCK;
#endif
}

void PrintUsage(std::string_view program_name) {
  std::cout << "Usage: " << program_name << " [host:port]\n"
            << "Starts an HTTP+JSON SSE example server.\n"
            << "Default endpoint: " << kDefaultEndpoint << "\n"
            << "REST endpoint: http://<host>:<port>" << kRestBasePath << "\n";
}

class SocketTransport final : public a2a::server::HttpByteTransport {
 public:
  explicit SocketTransport(SocketHandle socket) : socket_(socket) {
#ifndef _WIN32
    (void)a2a::server::SetSocketNoDelay(static_cast<int>(socket_));
#endif
  }

  a2a::core::Result<std::size_t> Read(char* buffer, std::size_t size) override {
    const auto bytes = ::recv(socket_, buffer, static_cast<int>(size), 0);
    if (bytes < 0) {
      return a2a::core::Error::Internal("Socket receive failed");
    }
    return static_cast<std::size_t>(bytes);
  }

  a2a::core::Result<std::size_t> Write(const char* buffer, std::size_t size) override {
    const auto bytes = ::send(socket_, buffer, static_cast<int>(size), 0);
    if (bytes < 0) {
      return a2a::core::Error::Internal("Socket send failed");
    }
    return static_cast<std::size_t>(bytes);
  }

 private:
  SocketHandle socket_;
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
  a2a::core::Result<lf::a2a::v1::SendMessageResponse> SendMessage(const lf::a2a::v1::SendMessageRequest& request,
                                                                  a2a::server::RequestContext& context) override {
    (void)context;
    if (!request.has_message()) {
      return a2a::core::Error::Validation("message is required");
    }

    task_.set_id(request.message().task_id().empty() ? kTaskId : request.message().task_id());
    task_.set_context_id(request.message().context_id().empty() ? kContextId : request.message().context_id());
    task_.mutable_status()->set_state(lf::a2a::v1::TASK_STATE_COMPLETED);
    auto* status_message = task_.mutable_status()->mutable_message();
    status_message->set_message_id(kStatusMessageId);
    status_message->set_role(lf::a2a::v1::ROLE_AGENT);
    status_message->add_parts()->set_text(kAgentReply);

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

  a2a::core::Result<lf::a2a::v1::Task> GetTask(const lf::a2a::v1::GetTaskRequest& request,
                                               a2a::server::RequestContext& context) override {
    (void)context;
    if (request.id().empty()) {
      return a2a::core::Error::Validation(kTaskIdRequiredMessage);
    }
    if (task_.id().empty() || request.id() != task_.id()) {
      return a2a::core::Error::Validation(kTaskNotFoundMessage);
    }
    return task_;
  }

  a2a::core::Result<a2a::server::ListTasksResponse> ListTasks(const a2a::server::ListTasksRequest& request,
                                                              a2a::server::RequestContext& context) override {
    (void)request;
    (void)context;
    a2a::server::ListTasksResponse response;
    if (!task_.id().empty()) {
      response.tasks.push_back(task_);
    }
    return response;
  }

  a2a::core::Result<lf::a2a::v1::Task> CancelTask(const lf::a2a::v1::CancelTaskRequest& request,
                                                  a2a::server::RequestContext& context) override {
    (void)context;
    if (request.id().empty()) {
      return a2a::core::Error::Validation(kTaskIdRequiredMessage);
    }
    if (task_.id().empty() || request.id() != task_.id()) {
      return a2a::core::Error::Validation(kTaskNotFoundMessage);
    }
    task_.mutable_status()->set_state(lf::a2a::v1::TASK_STATE_CANCELED);
    return task_;
  }

 private:
  lf::a2a::v1::Task task_;
};

SocketHandle CreateListeningSocket(std::string_view host, int port) {
  const SocketHandle server_socket = ::socket(AF_INET, SOCK_STREAM, 0);
  if (server_socket == kInvalidSocket) {
    return kInvalidSocket;
  }

  int reuse_address = kReuseAddress;
  if (setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse_address),
                 sizeof(reuse_address)) != 0) {
    CloseSocket(server_socket);
    return kInvalidSocket;
  }

  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(static_cast<std::uint16_t>(port));
  const std::string host_text(host);
  if (inet_pton(AF_INET, host_text.c_str(), &address.sin_addr) != 1 ||
      bind(server_socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
      listen(server_socket, kListenBacklog) != 0 || !SetNonBlocking(server_socket)) {
    CloseSocket(server_socket);
    return kInvalidSocket;
  }

  return server_socket;
}

void HandleConnection(SocketHandle client_socket, const a2a::server::RestServerTransport& server) {
  SocketTransport socket_transport(client_socket);
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
  const SocketHandle server_socket = CreateListeningSocket(host, port);
  if (server_socket == kInvalidSocket) {
    std::cerr << "Failed to listen on " << host << ':' << port << '\n';
#ifdef _WIN32
    WSACleanup();
#endif
    return 1;
  }

  std::signal(SIGINT, SignalHandler);
  std::signal(SIGTERM, SignalHandler);

  ExampleExecutor executor;
  a2a::server::Dispatcher dispatcher(&executor);
  auto agent_card = a2a::core::AgentCardBuilder::RestPreset("Streaming Example Agent",
                                                            a2a::server::BuildHttpUrl(host, port, kRestBasePath))
                        .Build();
  agent_card.mutable_capabilities()->set_streaming(true);
  a2a::server::RestServerTransport server(
      &dispatcher, std::move(agent_card),
      {.rest_api_base_path = kRestBasePath, .require_version_header = true, .include_legacy_transport_fields = false});

  std::cout << "streaming_server listening on " << a2a::server::BuildHttpUrl(host, port, kRestBasePath) << '\n';
  std::cout << "Press Ctrl+C to stop.\n";

  while (kKeepRunning != 0) {
    sockaddr_in client_address{};
    SocketLength client_address_size = sizeof(client_address);
    const SocketHandle client_socket =
        accept(server_socket, reinterpret_cast<sockaddr*>(&client_address), &client_address_size);
    if (client_socket == kInvalidSocket) {
      if (IsWouldBlockError()) {
        std::this_thread::sleep_for(kAcceptPollInterval);
        continue;
      }
#ifndef _WIN32
      if (errno == EINTR) {
        continue;
      }
#endif
      std::cerr << "Accept failed\n";
      break;
    }
    HandleConnection(client_socket, server);
    CloseSocket(client_socket);
  }

  CloseSocket(server_socket);
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
