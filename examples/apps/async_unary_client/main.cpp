// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include <chrono>
#include <future>
#include <iostream>
#include <string_view>
#include <utility>

#include "a2a/client/async_client.h"
#include "a2a/client/async_transport.h"
#include "a2a/client/client.h"
#include "a2a/client/http_json_transport.h"

namespace {
constexpr std::string_view kSmoke = "--smoke";
constexpr auto kSmokeEndpoint = "http://127.0.0.1:1";
constexpr auto kUsage = "usage: async_unary_client REST_ENDPOINT | --smoke\n";
constexpr auto kWait = std::chrono::seconds(5);
constexpr int kArgumentCount = 2;
}  // namespace

int main(int argc, char** argv) {
  if (argc != kArgumentCount) {
    std::cerr << kUsage;
    return 1;
  }
  const bool smoke = std::string_view(argv[1]) == kSmoke;
  a2a::client::ResolvedInterface resolved;
  resolved.transport = a2a::client::PreferredTransport::kRest;
  resolved.url = smoke ? kSmokeEndpoint : argv[1];
  a2a::client::A2AClient client(a2a::client::HttpJsonTransport::CreateDefault(std::move(resolved)));
  std::promise<bool> completed;
  auto future = completed.get_future();
  a2a::client::CallOptions options;
  options.timeout = kWait;
  auto call = client.Async().ListTasks(
      {},
      [&](a2a::core::Result<a2a::client::ListTasksResponse> result) {
        if (result.ok()) {
          std::cout << result.value().tasks.size() << '\n';
        } else {
          std::cerr << result.error().message() << '\n';
        }
        completed.set_value(result.ok());
      },
      options);
  if (!call.ok()) {
    return 1;
  }
  // Other application work can run here. The callback owns completion delivery.
  if (future.wait_for(kWait) != std::future_status::ready) {
    call.value().Cancel();
    call.value().Wait();
    return 1;
  }
  const bool ok = future.get();
  call.value().Wait();
  return (smoke || ok) ? 0 : 1;
}
