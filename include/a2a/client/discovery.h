#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "a2a/core/result.h"
#include "a2a/v1/a2a.pb.h"

namespace a2a::client {

inline constexpr std::chrono::seconds kDefaultDiscoveryCacheTtl{300};

enum class PreferredTransport : std::uint8_t {
  kRest,
  kJsonRpc,
  kGrpc,
};

struct HttpResponse final {
  int status_code = 0;
  std::string body;
};

using HttpFetcher = std::function<core::Result<HttpResponse>(std::string_view url)>;

struct ResolvedInterface final {
  PreferredTransport transport = PreferredTransport::kRest;
  std::string url;
  std::vector<std::string> security_requirements;
  std::unordered_map<std::string, lf::a2a::v1::SecurityScheme> security_schemes;
};

class DiscoveryClient final {
 public:
  explicit DiscoveryClient(HttpFetcher fetcher,
                           std::chrono::seconds cache_ttl = kDefaultDiscoveryCacheTtl);

  [[nodiscard]] core::Result<lf::a2a::v1::AgentCard> Fetch(std::string_view base_url);

 private:
  struct CacheEntry final {
    lf::a2a::v1::AgentCard card;
    std::chrono::steady_clock::time_point expires_at;
  };

  [[nodiscard]] static core::Result<std::string> BuildDiscoveryUrl(std::string_view base_url);
  [[nodiscard]] static core::Result<void> ValidateAgentCard(const lf::a2a::v1::AgentCard& card);

  HttpFetcher fetcher_;
  std::chrono::seconds cache_ttl_;
  std::unordered_map<std::string, CacheEntry> cache_;
};

class AgentCardResolver final {
 public:
  [[nodiscard]] static core::Result<ResolvedInterface> SelectPreferredInterface(
      const lf::a2a::v1::AgentCard& card, PreferredTransport preferred);

 private:
  [[nodiscard]] static core::Result<void> ValidateInterface(
      const lf::a2a::v1::AgentInterface& iface);
};

}  // namespace a2a::client
