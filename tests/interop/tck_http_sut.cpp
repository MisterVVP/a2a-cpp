// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include <grpcpp/security/server_credentials.h>
#include <grpcpp/server.h>
#include <grpcpp/server_builder.h>

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
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "a2a/core/agent_card_builder.h"
#include "a2a/core/protocol_bindings.h"
#include "a2a/server/grpc_server_transport.h"
#include "a2a/server/http_adapter.h"
#include "a2a/server/json_rpc_server_transport.h"
#include "a2a/server/rest_server_transport.h"
#include "a2a/server/server.h"
#include "a2a/server/transport_mux.h"
#include "example_support.h"

namespace {
constexpr int kListenBacklog = 128;
constexpr int kDefaultPort = 50061;
constexpr int kGrpcPortOffset = 1;
constexpr int kReuseAddress = 1;
constexpr std::time_t kAgentCardLastModifiedUnix = 1704067200;
volatile std::sig_atomic_t kKeepRunning = 1;

void SignalHandler(int signal_number) {
  (void)signal_number;
  kKeepRunning = 0;
}

class SocketTransport final : public a2a::server::HttpByteTransport {
 public:
  explicit SocketTransport(int fd) : fd_(fd) {}

  a2a::core::Result<std::size_t> Read(char* buffer, std::size_t size) override {
    const auto bytes = ::recv(fd_, buffer, size, 0);
    if (bytes < 0) {
      return a2a::core::Error::Internal("Socket recv failed");
    }
    return static_cast<std::size_t>(bytes);
  }

  a2a::core::Result<std::size_t> Write(const char* buffer, std::size_t size) override {
    const auto bytes = ::send(fd_, buffer, size, 0);
    if (bytes < 0) {
      return a2a::core::Error::Internal("Socket send failed");
    }
    return static_cast<std::size_t>(bytes);
  }

 private:
  int fd_;
};

}  // namespace

int main(int argc, char** argv) {
  const std::string endpoint = (argc > 1) ? argv[1] : "127.0.0.1:" + std::to_string(kDefaultPort);
  const auto pos = endpoint.find(':');
  if (pos == std::string::npos) {
    return 1;
  }
  const std::string host = endpoint.substr(0, pos);
  const int port = std::stoi(endpoint.substr(pos + 1));
  const int grpc_port = port + kGrpcPortOffset;

  std::signal(SIGINT, SignalHandler);
  std::signal(SIGTERM, SignalHandler);

  auto agent_card = a2a::core::AgentCardBuilder()
                        .SetName("TCK HTTP SUT")
                        .SetVersion("0.1.0")
                        .SetDescription("Conformance-focused local SUT for A2A")
                        .AddDefaultInputMode("text/plain")
                        .AddDefaultOutputMode("text/plain")
                        .AddInterface({.binding = a2a::core::protocol_bindings::kJsonRpc,
                                       .version = "1.0",
                                       .url = "http://localhost:" + std::to_string(port) + "/rpc"})
                        .AddInterface({.binding = a2a::core::protocol_bindings::kHttpJson,
                                       .version = "1.0",
                                       .url = "http://localhost:" + std::to_string(port) + "/a2a"})
                        .AddInterface({.binding = a2a::core::protocol_bindings::kGrpc,
                                       .version = "1.0",
                                       .url = "localhost:" + std::to_string(grpc_port)})
                        .Build();
  auto* capabilities = agent_card.mutable_capabilities();
  capabilities->set_streaming(true);
  capabilities->set_push_notifications(false);
  auto* skill = agent_card.add_skills();
  skill->set_id("echo");
  skill->set_name("Echo Skill");
  skill->set_description("Echoes incoming text for conformance validation");
  skill->add_input_modes("text/plain");
  skill->add_output_modes("text/plain");
  skill->add_tags("conformance");

  a2a::examples::ExampleExecutor executor;
  a2a::server::Dispatcher dispatcher(&executor);
  a2a::server::GrpcServerTransport grpc(&dispatcher);
  a2a::server::RestServerTransport rest(
      &dispatcher, agent_card,
      {.rest_api_base_path = "/a2a",
       .include_legacy_transport_fields = false,
       .agent_card_cache_settings = a2a::server::RestServerTransportOptions::AgentCardCacheSettings{
           .cache_control = "public, max-age=300",
           .last_modified = std::chrono::system_clock::from_time_t(kAgentCardLastModifiedUnix)}});
  a2a::server::JsonRpcServerTransport jsonrpc(&dispatcher, {.rpc_path = "/rpc", .require_version_header = false});

#ifdef _WIN32
  WSADATA wsa_data;
  if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
    return 1;
  }
#endif

  int server_fd = ::socket(AF_INET, SOCK_STREAM, 0);
  int opt = kReuseAddress;
  setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&opt), sizeof(opt));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<uint16_t>(port));
  inet_pton(AF_INET, host.c_str(), &addr.sin_addr);
  if (bind(server_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    return 1;
  }
  if (listen(server_fd, kListenBacklog) != 0) {
    return 1;
  }

  grpc::ServerBuilder grpc_builder;
  grpc_builder.AddListeningPort(host + ":" + std::to_string(grpc_port), grpc::InsecureServerCredentials());
  grpc_builder.RegisterService(&grpc);
  std::unique_ptr<grpc::Server> grpc_server = grpc_builder.BuildAndStart();
  if (!grpc_server) {
    return 1;
  }

  a2a::server::TransportMux mux(
      {.normalization_policy = a2a::server::TransportMux::PathNormalizationPolicy::kRootToDefaultPath,
       .default_path = "/rpc"});
  mux.RegisterJsonRpcRoute(jsonrpc);
  mux.RegisterRestRoute(rest);

  while (kKeepRunning != 0) {
    sockaddr_in client{};
    socklen_t len = sizeof(client);
    const int fd = accept(server_fd, reinterpret_cast<sockaddr*>(&client), &len);
    if (fd < 0) {
      continue;
    }
    SocketTransport socket_transport(fd);
    const a2a::server::HttpAdapter adapter;
    auto parsed = adapter.ReadRequest(socket_transport, "localhost");
    if (!parsed.ok()) {
      a2a::server::CloseSocketCrossPlatform(fd);
      continue;
    }

    a2a::server::HttpServerRequest request = std::move(parsed.value());
    auto response = mux.RouteRequest(request);
    if (response.ok()) {
      (void)a2a::server::HttpAdapter::WriteResponse(socket_transport, response.value());
    }
    a2a::server::CloseSocketCrossPlatform(fd);
  }
  grpc_server->Shutdown();
  a2a::server::CloseSocketCrossPlatform(server_fd);
#ifdef _WIN32
  WSACleanup();
#endif
  return 0;
}
