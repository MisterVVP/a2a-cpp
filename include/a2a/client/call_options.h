// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "a2a/client/auth.h"

namespace a2a::client {

struct CallOptions final {
  std::optional<std::chrono::milliseconds> timeout = std::nullopt;
  HeaderMap headers;
  std::vector<std::string> extensions;
  AuthHeaderHook auth_hook;
  std::shared_ptr<const CredentialProvider> credential_provider;
  AuthContext auth_context;
  std::optional<MtlsConfig> mtls = std::nullopt;
};

}  // namespace a2a::client
