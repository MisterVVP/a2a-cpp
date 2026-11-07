// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include "a2a/client/client.h"

namespace a2a::client::detail {
using Interceptors = std::vector<std::shared_ptr<ClientInterceptor>>;
void NotifyBefore(const Interceptors& interceptors, const ClientCallContext& context);
void NotifyAfter(const Interceptors& interceptors, const ClientCallContext& context, const ClientCallResult& result);

// Both facades compose one transport/interceptor/lifecycle identity.
struct ClientState final : private core::NonCopyableOrMovable {
  explicit ClientState(std::unique_ptr<ClientTransport> input) : transport(std::move(input)) {}
  [[nodiscard]] std::shared_ptr<ClientTransport> TransportSnapshot() const;
  [[nodiscard]] std::vector<std::shared_ptr<ClientInterceptor>> InterceptorSnapshot() const;
  void AddInterceptor(std::shared_ptr<ClientInterceptor> interceptor);
  [[nodiscard]] core::Result<void> Shutdown();
  void ReleaseClosingTransport();

 private:
  mutable std::mutex transport_mutex;
  std::shared_ptr<ClientTransport> transport;
  std::shared_ptr<ClientTransport> closing_transport;
  mutable std::mutex interceptor_mutex;
  std::vector<std::shared_ptr<ClientInterceptor>> interceptors;
};
}  // namespace a2a::client::detail
