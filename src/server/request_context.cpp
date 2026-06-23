// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "a2a/server/request_context.h"

#include <string>
#include <string_view>

#include "a2a/core/string_utils.h"

namespace a2a::server {
namespace {

bool IsAuthSignalHeader(std::string_view lowered_name) {
  return lowered_name == "authorization" || lowered_name == "proxy-authorization" ||
         lowered_name.find("auth") != std::string_view::npos || lowered_name.find("token") != std::string_view::npos ||
         lowered_name.find("api-key") != std::string_view::npos ||
         lowered_name.find("apikey") != std::string_view::npos;
}

}  // namespace

std::unordered_map<std::string, std::string> ExtractAuthMetadata(
    const std::unordered_map<std::string, std::string>& headers) {
  std::unordered_map<std::string, std::string> auth_metadata;

  for (const auto& [name, value] : headers) {
    const std::string lowered_name = core::strings::ToLowerAscii(name);
    if (lowered_name == "authorization" || lowered_name == "proxy-authorization") {
      const std::string trimmed_value(core::strings::TrimAsciiWhitespace(value));
      auth_metadata.insert_or_assign("authorization", trimmed_value);

      const std::string lowered_value = core::strings::ToLowerAscii(trimmed_value);
      constexpr std::string_view kBearerPrefix = "bearer ";
      if (lowered_value.starts_with(kBearerPrefix) && trimmed_value.size() > kBearerPrefix.size()) {
        auth_metadata.insert_or_assign(
            "bearer_token",
            std::string(core::strings::TrimAsciiWhitespace(trimmed_value.substr(kBearerPrefix.size()))));
      }
    }

    if (lowered_name == "x-api-key") {
      auth_metadata.insert_or_assign("api_key", value);
    }

    if (lowered_name == "x-forwarded-client-cert") {
      auth_metadata.insert_or_assign("mtls_client_cert", value);
    }

    if (IsAuthSignalHeader(lowered_name)) {
      auth_metadata.insert_or_assign("header." + lowered_name, value);
    }
  }

  return auth_metadata;
}

}  // namespace a2a::server
