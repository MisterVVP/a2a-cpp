// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "a2a/server/request_context.h"

#include <algorithm>
#include <cctype>
#include <ranges>
#include <string>
#include <string_view>

namespace a2a::server {
namespace {

std::string ToLower(std::string_view value) {
  std::string lowered(value);
  std::ranges::transform(lowered, lowered.begin(),
                         [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  return lowered;
}

std::string Trim(std::string_view value) {
  std::size_t begin = 0;
  while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin])) != 0) {
    ++begin;
  }
  std::size_t end = value.size();
  while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
    --end;
  }
  return std::string(value.substr(begin, end - begin));
}

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
    const std::string lowered_name = ToLower(name);
    if (lowered_name == "authorization" || lowered_name == "proxy-authorization") {
      const std::string trimmed_value = Trim(value);
      auth_metadata.insert_or_assign("authorization", trimmed_value);

      const std::string lowered_value = ToLower(trimmed_value);
      constexpr std::string_view kBearerPrefix = "bearer ";
      if (lowered_value.starts_with(kBearerPrefix) && trimmed_value.size() > kBearerPrefix.size()) {
        auth_metadata.insert_or_assign("bearer_token", Trim(trimmed_value.substr(kBearerPrefix.size())));
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
