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

#include <atomic>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

#include "a2a/core/agent_card/agent_card_builder.h"
#include "a2a/core/agent_card/agent_card_provider.h"
#include "a2a/server/dispatcher.h"
#include "a2a/server/grpc_server_transport.h"
#include "a2a/server/http_adapter.h"
#include "a2a/server/json_rpc_server_transport.h"
#include "a2a/server/network_utils.h"
#include "a2a/server/rest_server_transport.h"
#include "a2a/server/stores/store_factory.h"
#include "a2a/server/transport_mux.h"
#include "example_support.h"
#include "sut/tck_sut.h"

namespace {

using namespace a2a::tests::sut;

constexpr int kListenBacklog = 128;
constexpr int kReuseAddress = 1;
constexpr int kMaxHttpPort = 65534;
// Keep the non-blocking accept loop responsive for HTTP wire-performance clients.
constexpr int kAcceptRetryDelayMillis = 1;
constexpr std::time_t kAgentCardLastModifiedUnix = 1704067200;
constexpr std::string_view kMissingPostgresDsnMessage =
    "A2A_TCK_POSTGRES_DSN must be set when A2A_TCK_STORE_BACKEND=postgres";
constexpr std::string_view kUnsupportedStoreBackendMessage = "Unsupported A2A_TCK_STORE_BACKEND: ";
constexpr std::string_view kInvalidPostgresPoolSizeMessage = "A2A_TCK_POSTGRES_POOL_SIZE must be a positive integer";
constexpr std::string_view kHttpDiagnosticsPrefix = "A2A_HTTP_DIAGNOSTICS";
volatile std::sig_atomic_t kKeepRunning = 1;
std::atomic<std::uint64_t> kAcceptedHttpConnections{0};
std::atomic<std::uint64_t> kCompletedUnaryHttpOperations{0};

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

[[nodiscard]] a2a::core::Result<std::size_t> GetPostgresPoolSize() {
  const std::string_view value = GetEnvironmentValue(kPostgresPoolSizeEnv);
  if (value.empty()) {
    return a2a::server::stores::kDefaultPostgresConnectionPoolSize;
  }
  std::size_t size = 0U;
  const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), size);
  if (error != std::errc{} || end != value.data() + value.size() || size == 0U) {
    return a2a::core::Error::Validation(std::string{kInvalidPostgresPoolSizeMessage});
  }
  return size;
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
  const auto pool_size = GetPostgresPoolSize();
  if (!pool_size.ok()) {
    return pool_size.error();
  }
  a2a::server::stores::PostgresStoreOptions options{
      .connection_string = std::string{dsn},
      .schema = std::string{schema.empty() ? kDefaultPostgresSchema : schema},
      .auto_create_schema = true,
      .connection_pool_size = pool_size.value()};
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

void HandleHttpConnection(int fd, const a2a::server::TransportMux& mux, HttpConnectionRegistry& registry) {
  SocketTransport socket_transport(fd);
  const a2a::server::HttpAdapter adapter;
  a2a::server::HttpConnectionState connection_state;
  while (kKeepRunning != 0) {
    auto parsed = adapter.ReadRequest(socket_transport, connection_state, "localhost");
    if (!parsed.ok()) {
      break;
    }
    a2a::server::HttpServerRequest request = std::move(parsed.value());
    const bool request_reusable = a2a::server::HttpAdapter::IsConnectionReusable(request);
    auto response = mux.RouteRequest(request);
    if (!response.ok()) {
      break;
    }
    const bool is_streaming = static_cast<bool>(response.value().stream_writer);
    const auto written = a2a::server::HttpAdapter::WriteResponse(socket_transport, response.value(), !request_reusable);
    if (!written.ok()) {
      break;
    }
    if (!is_streaming) {
      kCompletedUnaryHttpOperations.fetch_add(1, std::memory_order_relaxed);
    }
    if (!request_reusable || is_streaming) {
      break;
    }
  }
  registry.Remove(fd);
  a2a::server::CloseSocketCrossPlatform(fd);
}

int RunTckSut(int argc, char** argv) {
  const std::string endpoint = (argc > 1) ? argv[1] : std::string(kDefaultHost) + ":" + std::to_string(kDefaultPort);
  auto parsed_endpoint = a2a::server::ParseHostPortEndpoint(endpoint, kMaxHttpPort);
  if (!parsed_endpoint.ok()) {
    std::cerr << parsed_endpoint.error().message() << '\n';
    return 1;
  }
  const std::string& host = parsed_endpoint.value().host;
  const int port = parsed_endpoint.value().port;
  const int grpc_port = port + kGrpcPortOffset;
  const SutEndpoints endpoints{.rest_url = a2a::server::BuildHttpUrl(host, port, kRestApiBasePath),
                               .json_rpc_url = a2a::server::BuildHttpUrl(host, port, kJsonRpcPath),
                               .grpc_url = host + ":" + std::to_string(grpc_port)};

  std::signal(SIGINT, SignalHandler);
  std::signal(SIGTERM, SignalHandler);

  const char* extended_card_mode_value = std::getenv(kExtendedCardModeEnv);
  const std::string_view extended_card_mode =
      extended_card_mode_value == nullptr ? kExtendedCardModeConfigured : std::string_view(extended_card_mode_value);
  if (extended_card_mode != kExtendedCardModeConfigured && extended_card_mode != kExtendedCardModeDeclaredOnly &&
      extended_card_mode != kExtendedCardModeDisabled) {
    std::cerr << "Unsupported A2A_TCK_EXTENDED_AGENT_CARD_MODE: " << extended_card_mode << '\n';
    return 1;
  }
  const bool declares_extended_card = extended_card_mode != kExtendedCardModeDisabled;
  const bool configures_extended_card = extended_card_mode == kExtendedCardModeConfigured;

  auto agent_card =
      a2a::core::AgentCardBuilder::ConformancePreset(
          {.rest_url = endpoints.rest_url, .json_rpc_url = endpoints.json_rpc_url, .grpc_url = endpoints.grpc_url},
          "TCK SUT", "0.1.0", "Conformance-focused local SUT for A2A")
          .WithPushNotifications(true)
          .WithExtendedAgentCard(declares_extended_card)
          .Build();

  std::optional<lf::a2a::v1::AgentCard> extended_agent_card;
  if (configures_extended_card) {
    extended_agent_card = agent_card;
    extended_agent_card->set_description("Extended conformance-focused local SUT card for A2A");
  }

  auto store_bundle = CreateStoreBundleFromEnvironment();
  if (!store_bundle.ok()) {
    std::cerr << "Failed to create TCK SUT store bundle: " << store_bundle.error().message() << '\n';
    return 1;
  }

  a2a::examples::ExampleExecutorOptions executor_options;
  executor_options.task_store = store_bundle.value().task_store.get();
  executor_options.push_store = store_bundle.value().push_store.get();
  a2a::examples::ExampleExecutor executor(std::move(executor_options));
  auto agent_card_provider = std::make_shared<a2a::core::StaticAgentCardProvider>(extended_agent_card);
  a2a::server::Dispatcher dispatcher(&executor, agent_card_provider);
  a2a::server::GrpcServerTransportOptions grpc_options;
  grpc_options.required_extensions = {std::string(kRequiredExtensionUri)};
  a2a::server::GrpcServerTransport grpc(&dispatcher, std::move(grpc_options));

  a2a::server::RestServerTransportOptions rest_options;
  rest_options.rest_api_base_path = std::string(kRestApiBasePath);
  rest_options.include_legacy_transport_fields = false;
  rest_options.required_extensions = {std::string(kRequiredExtensionUri)};
  rest_options.agent_card_cache_settings = a2a::server::RestServerTransportOptions::AgentCardCacheSettings{
      .cache_control = "public, max-age=300",
      .last_modified = std::chrono::system_clock::from_time_t(kAgentCardLastModifiedUnix)};
  a2a::server::RestServerTransport rest(&dispatcher, agent_card, std::move(rest_options));

  a2a::server::JsonRpcServerTransportOptions jsonrpc_options;
  jsonrpc_options.rpc_path = std::string(kJsonRpcPath);
  jsonrpc_options.require_version_header = false;
  jsonrpc_options.required_extensions = {std::string(kRequiredExtensionUri)};
  a2a::server::JsonRpcServerTransport jsonrpc(&dispatcher, std::move(jsonrpc_options));

#ifdef _WIN32
  WSADATA wsa_data;
  if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
    return 1;
  }
#endif

  int server_fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (server_fd < 0) {
    std::cerr << "Failed to create HTTP listening socket: " << std::strerror(errno) << '\n';
    return 1;
  }
  int opt = kReuseAddress;
  if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&opt), sizeof(opt)) != 0) {
    std::cerr << "Failed to configure HTTP listening socket reuse: " << std::strerror(errno) << '\n';
    a2a::server::CloseSocketCrossPlatform(server_fd);
    return 1;
  }
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<uint16_t>(port));
  if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
    std::cerr << "Invalid IPv4 host for TCK SUT HTTP listener: " << host << '\n';
    a2a::server::CloseSocketCrossPlatform(server_fd);
    return 1;
  }
  if (bind(server_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    std::cerr << "Failed to bind TCK SUT HTTP listener on " << host << ':' << port << ": " << std::strerror(errno)
              << '\n';
    a2a::server::CloseSocketCrossPlatform(server_fd);
    return 1;
  }
  if (listen(server_fd, kListenBacklog) != 0) {
    std::cerr << "Failed to listen on TCK SUT HTTP endpoint " << host << ':' << port << ": " << std::strerror(errno)
              << '\n';
    a2a::server::CloseSocketCrossPlatform(server_fd);
    return 1;
  }
  if (!a2a::server::SetSocketNonBlocking(server_fd)) {
    std::cerr << "Failed to configure non-blocking TCK SUT HTTP listener: " << std::strerror(errno) << '\n';
    a2a::server::CloseSocketCrossPlatform(server_fd);
    return 1;
  }

  grpc::ServerBuilder grpc_builder;
  grpc_builder.AddListeningPort(host + ":" + std::to_string(grpc_port), grpc::InsecureServerCredentials());
  grpc_builder.RegisterService(&grpc);
  std::unique_ptr<grpc::Server> grpc_server = grpc_builder.BuildAndStart();
  if (!grpc_server) {
    std::cerr << "Failed to start TCK SUT gRPC server on " << host << ':' << grpc_port << '\n';
    a2a::server::CloseSocketCrossPlatform(server_fd);
    return 1;
  }

  a2a::server::TransportMux mux(
      {.normalization_policy = a2a::server::TransportMux::PathNormalizationPolicy::kRootToDefaultPath,
       .default_path = std::string(kJsonRpcPath)});
  mux.RegisterJsonRpcRoute(jsonrpc);
  mux.RegisterRestRoute(rest);

  HttpConnectionRegistry connection_registry;
  std::vector<std::thread> connection_threads;
  while (kKeepRunning != 0) {
    sockaddr_in client{};
    socklen_t len = sizeof(client);
    const int fd = accept(server_fd, reinterpret_cast<sockaddr*>(&client), &len);
    if (fd < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
        std::this_thread::sleep_for(std::chrono::milliseconds(kAcceptRetryDelayMillis));
        continue;
      }
      std::cerr << "TCK SUT HTTP accept failed: " << std::strerror(errno) << '\n';
      break;
    }
    connection_registry.Add(fd);
    kAcceptedHttpConnections.fetch_add(1, std::memory_order_relaxed);
    connection_threads.emplace_back(HandleHttpConnection, fd, std::cref(mux), std::ref(connection_registry));
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
  const std::uint64_t accepted_connections = kAcceptedHttpConnections.load(std::memory_order_relaxed);
  const std::uint64_t completed_operations = kCompletedUnaryHttpOperations.load(std::memory_order_relaxed);
  const double operations_per_connection =
      accepted_connections == 0 ? 0.0
                                : static_cast<double>(completed_operations) / static_cast<double>(accepted_connections);
  std::cout << kHttpDiagnosticsPrefix << " accepted_connections=" << accepted_connections
            << " completed_unary_operations=" << completed_operations
            << " operations_per_connection=" << operations_per_connection << '\n';
#ifdef _WIN32
  WSACleanup();
#endif
  return 0;
}

}  // namespace

int main(int argc, char** argv) noexcept {
  try {
    return RunTckSut(argc, argv);
  } catch (const std::exception& ex) {
    std::cerr << "Unhandled TCK SUT exception: " << ex.what() << '\n';
    return 1;
  }
}
