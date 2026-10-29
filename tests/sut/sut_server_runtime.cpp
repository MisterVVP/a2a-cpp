// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "sut/sut_server_runtime.h"

#include <grpcpp/security/server_credentials.h>
#include <grpcpp/server.h>
#include <grpcpp/server_builder.h>

#include <chrono>
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <utility>

#include "a2a/server/network_utils.h"
#include "sut/sut_agent_options.h"
#include "sut/tck_sut_http_server.h"

namespace a2a::tests::sut {
namespace {
constexpr int kMaxHttpPort = 65534;
constexpr std::time_t kAgentCardLastModifiedUnix = 1704067200;
constexpr auto kCacheControl = "public, max-age=300";
constexpr auto kInvalidTls = "SUT TLS requires readable certificate and key files";
constexpr auto kHttpJoined = " shutdown: HTTP connection threads joined\n";
volatile std::sig_atomic_t kKeepRunning = 1;
void SignalHandler([[maybe_unused]] int signal_number) { kKeepRunning = 0; }
void SetSignals() {
  kKeepRunning = 1;
  std::signal(SIGINT, SignalHandler);
  std::signal(SIGTERM, SignalHandler);
#ifndef _WIN32
  std::signal(SIGPIPE, SIG_IGN);
#else
  std::signal(SIGBREAK, SignalHandler);
#endif
}
std::shared_ptr<::grpc::ServerCredentials> GrpcCredentials() {
  const char* certificate_path = std::getenv(kTlsCertificateFileEnv);
  const char* key_path = std::getenv(kTlsKeyFileEnv);
  if (certificate_path == nullptr && key_path == nullptr) {
    return ::grpc::InsecureServerCredentials();
  }
  if (certificate_path == nullptr || key_path == nullptr) {
    return nullptr;
  }
  std::ifstream certificate_file(certificate_path);
  std::ifstream key_file(key_path);
  if (!certificate_file || !key_file) {
    return nullptr;
  }
  ::grpc::SslServerCredentialsOptions ssl;
  ssl.pem_key_cert_pairs.push_back({std::string(std::istreambuf_iterator<char>(key_file), {}),
                                    std::string(std::istreambuf_iterator<char>(certificate_file), {})});
  return ::grpc::SslServerCredentials(ssl);
}
}  // namespace
core::Result<SutServerConfig> ReadSutServerConfig(int argc, char** argv) {
  std::string endpoint;
  if (argc > 1) {
    endpoint = argv[1];
  } else {
    endpoint.assign(kDefaultHost).append(":").append(std::to_string(kDefaultPort));
  }
  auto parsed = server::ParseHostPortEndpoint(endpoint, kMaxHttpPort);
  if (!parsed.ok()) {
    return parsed.error();
  }
  SutServerConfig config;
  config.listener = {
      .host = parsed.value().host, .port = parsed.value().port, .grpc_port = parsed.value().port + kGrpcPortOffset};
  const auto& host = config.listener.host;
  config.endpoints = {.rest_url = server::BuildHttpUrl(host, config.listener.port, kRestApiBasePath),
                      .json_rpc_url = server::BuildHttpUrl(host, config.listener.port, kJsonRpcPath),
                      .grpc_url = std::string(host).append(":").append(std::to_string(config.listener.grpc_port))};
  if (const char* public_base = std::getenv(kPublicHttpsBaseEnv)) {
    config.endpoints.rest_url = std::string(public_base).append(kRestApiBasePath);
    config.endpoints.json_rpc_url = std::string(public_base).append(kJsonRpcPath);
  }
  if (const char* public_target = std::getenv(kPublicGrpcTargetEnv)) {
    config.endpoints.grpc_url = public_target;
  }
  return config;
}
server::RestServerTransportOptions SutRestOptions() {
  server::RestServerTransportOptions rest;
  rest.rest_api_base_path = std::string(kRestApiBasePath);
  rest.include_legacy_transport_fields = false;
  rest.required_extensions = {std::string(kRequiredExtensionUri)};
  rest.agent_card_cache_settings = server::RestServerTransportOptions::AgentCardCacheSettings{
      .cache_control = kCacheControl,
      .last_modified = std::chrono::system_clock::from_time_t(kAgentCardLastModifiedUnix)};
  return rest;
}
class SutServerRuntime::Impl final {
 public:
  Impl(SutServerConfig config, SutRuntimeOptions options) : config_(std::move(config)), options_(options) {}
  SutServerConfig config_;
  SutRuntimeOptions options_;
  std::unique_ptr<TckHttpServer> http_;
  std::unique_ptr<::grpc::Server> grpc_;
};
SutServerRuntime::SutServerRuntime(SutServerConfig config, SutRuntimeOptions options)
    : impl_(std::make_unique<Impl>(std::move(config), options)) {
  SetSignals();
}
SutServerRuntime::~SutServerRuntime() {
  ShutdownHttp();
  ShutdownGrpc();
}
bool SutServerRuntime::Start(const server::TransportMux& mux, ::grpc::Service& service) {
  auto credentials = GrpcCredentials();
  if (credentials == nullptr) {
    std::cerr << kInvalidTls << '\n';
    return false;
  }
  const auto& config = impl_->config_.listener;
  impl_->http_ = std::make_unique<TckHttpServer>(config.host, config.port, mux, impl_->options_.observer);
  if (!impl_->http_->Start()) {
    return false;
  }
  ::grpc::ServerBuilder builder;
  builder.AddListeningPort(std::string(config.host).append(":").append(std::to_string(config.grpc_port)), credentials);
  builder.RegisterService(&service);
  impl_->grpc_ = builder.BuildAndStart();
  return impl_->grpc_ != nullptr;
}
void SutServerRuntime::Run() { impl_->http_->AcceptConnections(kKeepRunning); }
void SutServerRuntime::ShutdownHttp() {
  if (impl_->http_ != nullptr) {
    impl_->http_->ShutdownActiveSockets();
    impl_->http_->JoinConnections();
    impl_->http_.reset();
    std::cerr << impl_->options_.display_name << kHttpJoined;
    if (impl_->options_.observer != nullptr) {
      impl_->options_.observer->OnShutdown();
    }
  }
}
void SutServerRuntime::ShutdownGrpc() {
  if (impl_->grpc_ != nullptr) {
    impl_->grpc_->Shutdown();
    impl_->grpc_->Wait();
    impl_->grpc_.reset();
  }
}
}  // namespace a2a::tests::sut
