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

#include "a2a/core/protocol_bindings.h"
#include "a2a/server/grpc_server_transport.h"
#include "a2a/server/json_rpc_server_transport.h"
#include "a2a/server/rest_server_transport.h"
#include "a2a/server/server.h"
#include "example_support.h"

namespace {
constexpr std::size_t kInitialRequestCapacity = 8192;
constexpr std::size_t kBufferSize = 4096;
constexpr std::size_t kRequestSizeUnit = 1024;
constexpr std::size_t kMaxRequestSize = kRequestSizeUnit * kRequestSizeUnit;
constexpr std::size_t kContentLengthPrefixSize = std::string_view("Content-Length:").size();
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

void CloseSocket(int fd) {
#ifdef _WIN32
  closesocket(fd);
#else
  close(fd);
#endif
}

std::optional<std::string> ReadRequest(int fd) {
  std::string request;
  request.reserve(kInitialRequestCapacity);
  std::array<char, kBufferSize> buffer{};
  while (request.find("\r\n\r\n") == std::string::npos) {
    const auto n = ::recv(fd, buffer.data(), buffer.size(), 0);
    if (n <= 0) {
      return std::nullopt;
    }
    request.append(buffer.data(), static_cast<std::size_t>(n));
    if (request.size() > kMaxRequestSize) {
      return std::nullopt;
    }
  }
  const auto header_end = request.find("\r\n\r\n");
  const auto headers = request.substr(0, header_end);
  std::size_t content_length = 0;
  std::istringstream stream(headers);
  std::string line;
  std::getline(stream, line);
  while (std::getline(stream, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (line.starts_with("Content-Length:")) {
      content_length = static_cast<std::size_t>(std::stoul(line.substr(kContentLengthPrefixSize)));
    }
  }
  const auto body_start = header_end + std::string_view("\r\n\r\n").size();
  while (request.size() < body_start + content_length) {
    const auto n = ::recv(fd, buffer.data(), buffer.size(), 0);
    if (n <= 0) {
      return std::nullopt;
    }
    request.append(buffer.data(), static_cast<std::size_t>(n));
  }
  return request;
}

void WriteResponse(int fd, const a2a::server::HttpServerResponse& response) {
  std::ostringstream out;
  out << "HTTP/1.1 " << response.status_code << " OK\r\n";
  bool has_length = false;
  for (const auto& [k, v] : response.headers) {
    if (k == "Content-Length" || k == "content-length") {
      has_length = true;
    }
    out << k << ": " << v << "\r\n";
  }
  if (!has_length) {
    out << "Content-Length: " << response.body.size() << "\r\n";
  }
  out << "Connection: close\r\n\r\n";
  out << response.body;
  const auto payload = out.str();
  (void)::send(fd, payload.data(), payload.size(), 0);
}

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

  lf::a2a::v1::AgentCard agent_card;
  agent_card.set_name("TCK HTTP SUT");
  agent_card.set_version("0.1.0");
  agent_card.set_description("Conformance-focused local SUT for A2A");
  agent_card.add_default_input_modes("text/plain");
  agent_card.add_default_output_modes("text/plain");
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
  auto* jsonrpc_interface = agent_card.add_supported_interfaces();
  jsonrpc_interface->set_protocol_binding(std::string(a2a::core::protocol_bindings::kJsonRpc));
  jsonrpc_interface->set_protocol_version("1.0");
  jsonrpc_interface->set_url("http://localhost:" + std::to_string(port) + "/rpc");
  auto* rest_interface = agent_card.add_supported_interfaces();
  rest_interface->set_protocol_binding(std::string(a2a::core::protocol_bindings::kHttpJson));
  rest_interface->set_protocol_version("1.0");
  rest_interface->set_url("http://localhost:" + std::to_string(port) + "/a2a");
  auto* grpc_interface = agent_card.add_supported_interfaces();
  grpc_interface->set_protocol_binding(std::string(a2a::core::protocol_bindings::kGrpc));
  grpc_interface->set_protocol_version("1.0");
  grpc_interface->set_url("localhost:" + std::to_string(grpc_port));

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

  while (kKeepRunning != 0) {
    sockaddr_in client{};
    socklen_t len = sizeof(client);
    const int fd = accept(server_fd, reinterpret_cast<sockaddr*>(&client), &len);
    if (fd < 0) {
      continue;
    }
    const auto req_opt = ReadRequest(fd);
    if (!req_opt) {
      CloseSocket(fd);
      continue;
    }
    const std::string& req = *req_opt;
    const auto line_end = req.find("\r\n");
    const auto first_line = req.substr(0, line_end);
    std::istringstream fl(first_line);
    std::string method;
    std::string target;
    fl >> method >> target;

    std::unordered_map<std::string, std::string> headers;
    const auto header_end = req.find("\r\n\r\n");
    std::istringstream hs(req.substr(line_end + 2, header_end - (line_end + 2)));
    std::string hline;
    while (std::getline(hs, hline)) {
      if (!hline.empty() && hline.back() == '\r') {
        hline.pop_back();
      }
      const auto c = hline.find(':');
      if (c != std::string::npos) {
        headers[hline.substr(0, c)] = hline.substr(c + 2);
      }
    }
    const std::string body = req.substr(header_end + std::string_view("\r\n\r\n").size());

    a2a::server::HttpServerRequest request{
        .method = method, .target = target, .headers = headers, .body = body, .remote_address = "localhost"};
    auto response = rest.Handle(request);
    if (target == "/rpc" || target == "/") {
      request.target = "/rpc";
      response = jsonrpc.Handle(request);
    }
    if (response.ok()) {
      WriteResponse(fd, response.value());
    }
    CloseSocket(fd);
  }
  grpc_server->Shutdown();
  CloseSocket(server_fd);
#ifdef _WIN32
  WSACleanup();
#endif
  return 0;
}
