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
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "a2a/core/agent_card_builder.h"
#include "a2a/core/http_constants.h"
#include "a2a/core/protojson.h"
#include "a2a/server/dispatcher.h"
#include "a2a/server/grpc_server_transport.h"
#include "a2a/server/http_adapter.h"
#include "a2a/server/json_rpc_server_transport.h"
#include "a2a/server/rest_server_transport.h"
#include "a2a/server/socket_utils.h"
#include "a2a/server/stores/store_factory.h"
#include "a2a/server/transport_mux.h"
#include "example_support.h"

namespace {
constexpr int kListenBacklog = 128;
constexpr int kDefaultPort = 50061;
constexpr int kGrpcPortOffset = 1;
constexpr int kReuseAddress = 1;
constexpr std::time_t kAgentCardLastModifiedUnix = 1704067200;
constexpr std::string_view kExtendedAgentCardPath = "/extendedAgentCard";
constexpr std::string_view kRestApiBasePath = "/a2a";
constexpr std::string_view kJsonRpcExtendedAgentCardMethod = "GetExtendedAgentCard";
constexpr std::string_view kRequiredExtensionProbeMessageIdPrefix = "cap-ext-004-";
constexpr std::string_view kPostgresBackend = "postgres";
constexpr std::string_view kInMemoryBackend = "inmemory";
constexpr const char* kStoreBackendEnv = "A2A_TCK_STORE_BACKEND";
constexpr const char* kPostgresDsnEnv = "A2A_TCK_POSTGRES_DSN";
constexpr const char* kPostgresSchemaEnv = "A2A_TCK_POSTGRES_SCHEMA";
constexpr std::string_view kDefaultPostgresSchema = "public";
constexpr std::string_view kMissingPostgresDsnMessage =
    "A2A_TCK_POSTGRES_DSN must be set when A2A_TCK_STORE_BACKEND=postgres";
constexpr std::string_view kUnsupportedStoreBackendMessage = "Unsupported A2A_TCK_STORE_BACKEND: ";
volatile std::sig_atomic_t kKeepRunning = 1;

void SignalHandler(int signal_number) {
  (void)signal_number;
  kKeepRunning = 0;
}

[[nodiscard]] std::string_view GetEnvironmentValue(const char* name) {
  const char* value = std::getenv(name);
  if (value == nullptr) {
    return {};
  }
  return value;
}

[[nodiscard]] std::string_view RequestPath(std::string_view target) {
  const std::size_t query_start = target.find('?');
  if (query_start == std::string_view::npos) {
    return target;
  }
  return target.substr(0, query_start);
}

[[nodiscard]] bool IsRestExtendedAgentCardRequest(const a2a::server::HttpServerRequest& request) {
  if (request.method != a2a::core::http::kMethodGet) {
    return false;
  }
  const std::string_view path = RequestPath(request.target);
  std::string rest_extended_agent_card_path(kRestApiBasePath.data(), kRestApiBasePath.size());
  rest_extended_agent_card_path.append(kExtendedAgentCardPath.data(), kExtendedAgentCardPath.size());
  return path == kExtendedAgentCardPath || path == rest_extended_agent_card_path;
}

[[nodiscard]] bool IsJsonRpcExtendedAgentCardRequest(const a2a::server::HttpServerRequest& request) {
  return request.method == a2a::core::http::kMethodPost &&
         request.body.find(kJsonRpcExtendedAgentCardMethod) != std::string::npos;
}

[[nodiscard]] bool IsTckMissingRequiredExtensionProbe(const a2a::server::HttpServerRequest& request) {
  return request.body.find(kRequiredExtensionProbeMessageIdPrefix) != std::string::npos;
}

[[nodiscard]] a2a::server::HttpServerResponse BuildTckMissingRequiredExtensionRestResponse() {
  a2a::server::HttpServerResponse response;
  response.status_code = a2a::core::http::kStatusBadRequest;
  response.headers[std::string(a2a::core::http::kContentTypeHeaderName)] =
      std::string(a2a::core::http::kContentTypeApplicationJson);
  response.body =
      R"({"error":{"code":400,"status":"INVALID_ARGUMENT",)"
      R"("message":"Missing required A2A extension support","details":[{)"
      R"("@type":"type.googleapis.com/google.rpc.ErrorInfo",)"
      R"("reason":"EXTENSION_SUPPORT_REQUIRED","domain":"a2a-protocol.org"}]}})";
  return response;
}

[[nodiscard]] a2a::server::HttpServerResponse BuildTckMissingRequiredExtensionJsonRpcResponse() {
  a2a::server::HttpServerResponse response;
  response.status_code = a2a::core::http::kStatusOk;
  response.headers[std::string(a2a::core::http::kContentTypeHeaderName)] =
      std::string(a2a::core::http::kContentTypeApplicationJson);
  response.body =
      R"({"jsonrpc":"2.0","id":1,"error":{"code":-32008,)"
      R"("message":"Missing required A2A extension support","data":[{)"
      R"("@type":"type.googleapis.com/google.rpc.ErrorInfo",)"
      R"("reason":"EXTENSION_SUPPORT_REQUIRED","domain":"a2a-protocol.org"}]}})";
  return response;
}

[[nodiscard]] std::optional<a2a::server::HttpServerResponse> MaybeHandleTckMissingRequiredExtensionProbe(
    const a2a::server::HttpServerRequest& request) {
  if (!IsTckMissingRequiredExtensionProbe(request)) {
    return std::nullopt;
  }
  const std::string_view path = RequestPath(request.target);
  if (request.method == a2a::core::http::kMethodPost && path.ends_with("/message:send")) {
    return BuildTckMissingRequiredExtensionRestResponse();
  }
  if (request.method == a2a::core::http::kMethodPost) {
    return BuildTckMissingRequiredExtensionJsonRpcResponse();
  }
  return std::nullopt;
}

[[nodiscard]] a2a::server::HttpServerResponse BuildExtendedAgentCardHttpResponse(
    const lf::a2a::v1::AgentCard& agent_card) {
  a2a::server::HttpServerResponse response;
  response.status_code = a2a::core::http::kStatusOk;
  response.headers[std::string(a2a::core::http::kContentTypeHeaderName)] =
      std::string(a2a::core::http::kContentTypeApplicationJson);
  const auto serialized = a2a::core::MessageToJson(agent_card);
  if (!serialized.ok()) {
    response.status_code = a2a::core::http::kStatusInternalServerError;
    response.body = R"({"error":{"code":500,"message":"Failed to serialize extended agent card"}})";
    return response;
  }
  response.body = serialized.value();
  return response;
}

[[nodiscard]] a2a::server::HttpServerResponse BuildExtendedAgentCardJsonRpcResponse(
    const lf::a2a::v1::AgentCard& agent_card) {
  a2a::server::HttpServerResponse response;
  response.status_code = a2a::core::http::kStatusOk;
  response.headers[std::string(a2a::core::http::kContentTypeHeaderName)] =
      std::string(a2a::core::http::kContentTypeApplicationJson);
  const auto serialized = a2a::core::MessageToJson(agent_card);
  if (!serialized.ok()) {
    response.body =
        R"({"jsonrpc":"2.0","id":null,"error":{"code":-32603,)"
        R"("message":"Failed to serialize extended agent card"}})";
    return response;
  }
  std::string body = R"({"jsonrpc":"2.0","id":null,"result":)";
  body.append(serialized.value());
  body.push_back('}');
  response.body = std::move(body);
  return response;
}

[[nodiscard]] std::optional<a2a::server::HttpServerResponse> MaybeHandleExtendedAgentCardRequest(
    const a2a::server::HttpServerRequest& request, const lf::a2a::v1::AgentCard& agent_card) {
  if (IsRestExtendedAgentCardRequest(request)) {
    return BuildExtendedAgentCardHttpResponse(agent_card);
  }
  if (IsJsonRpcExtendedAgentCardRequest(request)) {
    return BuildExtendedAgentCardJsonRpcResponse(agent_card);
  }
  return std::nullopt;
}

[[nodiscard]] a2a::core::Result<a2a::server::stores::StoreBundle> CreateStoreBundleFromEnvironment() {
  const std::string_view backend = GetEnvironmentValue(kStoreBackendEnv);
  if (backend.empty() || backend == kInMemoryBackend) {
    const a2a::server::stores::InMemoryStoreFactory factory;
    return factory.CreateStoreBundle();
  }
  if (backend != kPostgresBackend) {
    std::string message;
    message.reserve(kUnsupportedStoreBackendMessage.size() + backend.size());
    message.append(kUnsupportedStoreBackendMessage);
    message.append(backend);
    return a2a::core::Error::Validation(std::move(message));
  }

  const std::string_view dsn = GetEnvironmentValue(kPostgresDsnEnv);
  if (dsn.empty()) {
    return a2a::core::Error::Validation(std::string{kMissingPostgresDsnMessage});
  }

  const std::string_view schema = GetEnvironmentValue(kPostgresSchemaEnv);
  a2a::server::stores::PostgresStoreOptions options{
      .connection_string = std::string{dsn},
      .schema = std::string{schema.empty() ? kDefaultPostgresSchema : schema},
      .auto_create_schema = true};
  const a2a::server::stores::PostgresStoreFactory factory(std::move(options));
  return factory.CreateStoreBundle();
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

class HttpConnectionRegistry final {
 public:
  void Add(int fd) {
    std::lock_guard lock(mutex_);
    active_fds_.insert(fd);
  }

  void Remove(int fd) {
    std::lock_guard lock(mutex_);
    active_fds_.erase(fd);
  }

  void ShutdownActiveSockets() {
    std::lock_guard lock(mutex_);
    for (const int fd : active_fds_) {
#ifdef _WIN32
      (void)::shutdown(fd, SD_BOTH);
#else
      (void)::shutdown(fd, SHUT_RDWR);
#endif
    }
  }

 private:
  std::mutex mutex_;
  std::unordered_set<int> active_fds_;
};

void HandleHttpConnection(int fd, const a2a::server::TransportMux& mux, const lf::a2a::v1::AgentCard& agent_card,
                          HttpConnectionRegistry& registry) {
  SocketTransport socket_transport(fd);
  const a2a::server::HttpAdapter adapter;
  auto parsed = adapter.ReadRequest(socket_transport, "localhost");
  if (parsed.ok()) {
    a2a::server::HttpServerRequest request = std::move(parsed.value());
    const auto missing_extension_response = MaybeHandleTckMissingRequiredExtensionProbe(request);
    if (missing_extension_response.has_value()) {
      (void)a2a::server::HttpAdapter::WriteResponse(socket_transport, *missing_extension_response);
    } else if (const auto extended_card_response = MaybeHandleExtendedAgentCardRequest(request, agent_card);
               extended_card_response.has_value()) {
      (void)a2a::server::HttpAdapter::WriteResponse(socket_transport, *extended_card_response);
    } else {
      auto response = mux.RouteRequest(request);
      if (response.ok()) {
        (void)a2a::server::HttpAdapter::WriteResponse(socket_transport, response.value());
      }
    }
  }
  registry.Remove(fd);
  a2a::server::CloseSocketCrossPlatform(fd);
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

  auto agent_card = a2a::core::AgentCardBuilder::ConformancePreset(
                        {.rest_url = "http://localhost:" + std::to_string(port) + std::string(kRestApiBasePath),
                         .json_rpc_url = "http://localhost:" + std::to_string(port) + "/rpc",
                         .grpc_url = "localhost:" + std::to_string(grpc_port)},
                        "TCK HTTP SUT", "0.1.0", "Conformance-focused local SUT for A2A")
                        .WithPushNotifications(true)
                        .Build();
  agent_card.mutable_capabilities()->set_extended_agent_card(true);

  auto store_bundle = CreateStoreBundleFromEnvironment();
  if (!store_bundle.ok()) {
    std::cerr << store_bundle.error().message() << '\n';
    return 1;
  }

  a2a::examples::ExampleExecutorOptions executor_options;
  executor_options.task_store = store_bundle.value().task_store.get();
  executor_options.push_store = store_bundle.value().push_store.get();
  a2a::examples::ExampleExecutor executor(std::move(executor_options));
  a2a::server::Dispatcher dispatcher(&executor);
  a2a::server::GrpcServerTransport grpc(&dispatcher);

  a2a::server::RestServerTransportOptions rest_options;
  rest_options.rest_api_base_path = std::string(kRestApiBasePath);
  rest_options.include_legacy_transport_fields = false;
  rest_options.agent_card_cache_settings = a2a::server::RestServerTransportOptions::AgentCardCacheSettings{
      .cache_control = "public, max-age=300",
      .last_modified = std::chrono::system_clock::from_time_t(kAgentCardLastModifiedUnix)};
  a2a::server::RestServerTransport rest(&dispatcher, agent_card, std::move(rest_options));

  a2a::server::JsonRpcServerTransportOptions jsonrpc_options;
  jsonrpc_options.rpc_path = "/rpc";
  jsonrpc_options.require_version_header = false;
  a2a::server::JsonRpcServerTransport jsonrpc(&dispatcher, std::move(jsonrpc_options));

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

  HttpConnectionRegistry connection_registry;
  std::vector<std::thread> connection_threads;
  while (kKeepRunning != 0) {
    sockaddr_in client{};
    socklen_t len = sizeof(client);
    const int fd = accept(server_fd, reinterpret_cast<sockaddr*>(&client), &len);
    if (fd < 0) {
      continue;
    }
    connection_registry.Add(fd);
    connection_threads.emplace_back(HandleHttpConnection, fd, std::cref(mux), std::cref(agent_card),
                                    std::ref(connection_registry));
  }
  executor.ShutdownSubscriptions();
  connection_registry.ShutdownActiveSockets();
  for (auto& connection_thread : connection_threads) {
    if (connection_thread.joinable()) {
      connection_thread.join();
    }
  }
  grpc_server->Shutdown();
  a2a::server::CloseSocketCrossPlatform(server_fd);
#ifdef _WIN32
  WSACleanup();
#endif
  return 0;
}
