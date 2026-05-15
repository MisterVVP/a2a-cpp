#include <arpa/inet.h>
#include <grpcpp/security/server_credentials.h>
#include <grpcpp/server.h>
#include <grpcpp/server_builder.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
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
volatile std::sig_atomic_t kKeepRunning = 1;

void SignalHandler(int) { kKeepRunning = 0; }

std::optional<std::string> ReadRequest(int fd) {
  std::string request;
  request.reserve(8192);
  char buffer[4096];
  while (request.find("\r\n\r\n") == std::string::npos) {
    const auto n = ::recv(fd, buffer, sizeof(buffer), 0);
    if (n <= 0) return std::nullopt;
    request.append(buffer, static_cast<size_t>(n));
    if (request.size() > 1024 * 1024) return std::nullopt;
  }
  const auto header_end = request.find("\r\n\r\n");
  const auto headers = request.substr(0, header_end);
  std::size_t content_length = 0;
  std::istringstream stream(headers);
  std::string line;
  std::getline(stream, line);
  while (std::getline(stream, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.rfind("Content-Length:", 0) == 0) {
      content_length = static_cast<std::size_t>(std::stoul(line.substr(15)));
    }
  }
  const auto body_start = header_end + 4;
  while (request.size() < body_start + content_length) {
    const auto n = ::recv(fd, buffer, sizeof(buffer), 0);
    if (n <= 0) return std::nullopt;
    request.append(buffer, static_cast<size_t>(n));
  }
  return request;
}

void WriteResponse(int fd, const a2a::server::HttpServerResponse& response) {
  std::ostringstream out;
  out << "HTTP/1.1 " << response.status_code << " OK\r\n";
  bool has_length = false;
  for (const auto& [k, v] : response.headers) {
    if (k == "Content-Length" || k == "content-length") has_length = true;
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
  const std::string endpoint = (argc > 1) ? argv[1] : "127.0.0.1:50061";
  const auto pos = endpoint.find(':');
  if (pos == std::string::npos) return 1;
  const std::string host = endpoint.substr(0, pos);
  const int port = std::stoi(endpoint.substr(pos + 1));
  const int grpc_port = port + 1;

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
  jsonrpc_interface->set_url("http://localhost:50061/rpc");
  auto* rest_interface = agent_card.add_supported_interfaces();
  rest_interface->set_protocol_binding(std::string(a2a::core::protocol_bindings::kHttpJson));
  rest_interface->set_protocol_version("1.0");
  rest_interface->set_url("http://localhost:50061/a2a");
  auto* grpc_interface = agent_card.add_supported_interfaces();
  grpc_interface->set_protocol_binding(std::string(a2a::core::protocol_bindings::kGrpc));
  grpc_interface->set_protocol_version("1.0");
  grpc_interface->set_url("localhost:" + std::to_string(grpc_port));

  a2a::examples::ExampleExecutor executor;
  a2a::server::Dispatcher dispatcher(&executor);
  a2a::server::GrpcServerTransport grpc(&dispatcher);
  a2a::server::RestServerTransport rest(&dispatcher, agent_card, {.rest_api_base_path = "/a2a"});
  a2a::server::JsonRpcServerTransport jsonrpc(
      &dispatcher, {.rpc_path = "/rpc", .require_version_header = false});

  int server_fd = ::socket(AF_INET, SOCK_STREAM, 0);
  int opt = 1;
  setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<uint16_t>(port));
  inet_pton(AF_INET, host.c_str(), &addr.sin_addr);
  if (bind(server_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) return 1;
  if (listen(server_fd, 128) != 0) return 1;

  grpc::ServerBuilder grpc_builder;
  grpc_builder.AddListeningPort(host + ":" + std::to_string(grpc_port),
                                grpc::InsecureServerCredentials());
  grpc_builder.RegisterService(&grpc);
  std::unique_ptr<grpc::Server> grpc_server = grpc_builder.BuildAndStart();
  if (!grpc_server) {
    return 1;
  }

  while (kKeepRunning != 0) {
    sockaddr_in client{};
    socklen_t len = sizeof(client);
    const int fd = accept(server_fd, reinterpret_cast<sockaddr*>(&client), &len);
    if (fd < 0) continue;
    const auto req_opt = ReadRequest(fd);
    if (!req_opt) {
      close(fd);
      continue;
    }
    const std::string req = *req_opt;
    const auto line_end = req.find("\r\n");
    const auto first_line = req.substr(0, line_end);
    std::istringstream fl(first_line);
    std::string method, target;
    fl >> method >> target;

    std::unordered_map<std::string, std::string> headers;
    const auto header_end = req.find("\r\n\r\n");
    std::istringstream hs(req.substr(line_end + 2, header_end - (line_end + 2)));
    std::string hline;
    while (std::getline(hs, hline)) {
      if (!hline.empty() && hline.back() == '\r') hline.pop_back();
      const auto c = hline.find(':');
      if (c != std::string::npos) headers[hline.substr(0, c)] = hline.substr(c + 2);
    }
    const std::string body = req.substr(header_end + 4);

    a2a::server::HttpServerRequest request{.method = method,
                                           .target = target,
                                           .headers = headers,
                                           .body = body,
                                           .remote_address = "127.0.0.1"};
    auto response = rest.Handle(request);
    if (target == "/rpc" || target == "/") {
      request.target = "/rpc";
      response = jsonrpc.Handle(request);
    }
    if (response.ok()) {
      WriteResponse(fd, response.value());
    }
    close(fd);
  }
  grpc_server->Shutdown();
  close(server_fd);
  return 0;
}
