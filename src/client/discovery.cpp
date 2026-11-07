// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "a2a/client/discovery.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <optional>
#include <ranges>
#include <string>
#include <utility>

#include "a2a/core/error.h"
#include "a2a/core/extensions.h"
#include "a2a/core/http_constants.h"
#include "a2a/core/non_copyable.h"
#include "a2a/core/protocol_bindings.h"
#include "a2a/core/protocol_paths.h"
#include "a2a/core/protojson.h"
#include "a2a/core/version.h"
#include "a2a/http/http_client.h"
#include "transport/request_headers.h"

namespace a2a::client {

namespace {

constexpr int kHttpStatusOkMin = 200;
constexpr int kHttpStatusOkMax = 299;
constexpr int kHttpStatusNotFound = 404;
constexpr const char* kHttpTransport = "http";
constexpr const char* kVerifierThrew = "async Agent Card verifier threw an exception";
constexpr const char* kDiscoveryClosed = "async discovery shut down";

std::string Trim(std::string_view input) {
  std::string value(input);
  const auto begin = std::ranges::find_if_not(value, [](unsigned char ch) { return std::isspace(ch) != 0; });
  const auto end = std::ranges::find_if_not(std::ranges::reverse_view(value), [](unsigned char ch) {
                     return std::isspace(ch) != 0;
                   }).base();
  if (begin >= end) {
    return {};
  }
  return {begin, end};
}

HttpResponse ToDiscoveryHttpResponse(a2a::http::Response response) {
  return HttpResponse{.status_code = response.status_code, .body = std::move(response.body)};
}

bool HasHttpScheme(std::string_view url) { return url.starts_with("http://") || url.starts_with("https://"); }

bool HasGrpcScheme(std::string_view url) {
  return url.starts_with("grpc://") || url.starts_with("grpcs://") || url.starts_with("dns:///");
}

bool HasHostPortShape(std::string_view endpoint) { return endpoint.find(':') != std::string_view::npos; }

using a2a::core::protocol_bindings::kGrpc;
using a2a::core::protocol_bindings::kHttpJson;
using a2a::core::protocol_bindings::kJsonRpc;

struct InterfaceEndpoint final {
  std::string_view protocol_binding;
  std::string_view endpoint;
};

bool IsValidInterfaceEndpoint(const InterfaceEndpoint& candidate) {
  if (candidate.protocol_binding == kHttpJson || candidate.protocol_binding == kJsonRpc) {
    return HasHttpScheme(candidate.endpoint);
  }
  if (candidate.protocol_binding == kGrpc) {
    return HasGrpcScheme(candidate.endpoint) || HasHttpScheme(candidate.endpoint) ||
           HasHostPortShape(candidate.endpoint);
  }
  return false;
}

PreferredTransport ToPreferredTransport(std::string_view protocol_binding) {
  if (protocol_binding == kHttpJson) {
    return PreferredTransport::kRest;
  }
  if (protocol_binding == kJsonRpc) {
    return PreferredTransport::kJsonRpc;
  }
  return PreferredTransport::kGrpc;
}

std::optional<std::string_view> ToWireTransport(PreferredTransport transport) {
  switch (transport) {
    case PreferredTransport::kRest:
      return kHttpJson;
    case PreferredTransport::kJsonRpc:
      return kJsonRpc;
    case PreferredTransport::kGrpc:
      return kGrpc;
  }
  return std::nullopt;
}

}  // namespace

HttpFetcher MakeDefaultHttpFetcher(std::optional<http::TlsOptions> tls) {
  return [client = a2a::http::Client{}, tls = std::move(tls)](std::string_view url) -> core::Result<HttpResponse> {
    a2a::http::Request request;
    request.tls = tls;
    request.method = std::string(core::http::kMethodGet);
    request.url = std::string(url);
    request.headers.push_back({std::string(core::Version::kHeaderName), core::Version::HeaderValue()});
    auto response = client.SendRequest(request);
    if (!response.ok()) {
      return response.error();
    }
    return ToDiscoveryHttpResponse(std::move(response.value()));
  };
}

struct DiscoveryClient::AsyncCache final {
  std::mutex mutex;
  std::unordered_map<std::string, CacheEntry> entries;
  std::condition_variable finished;
  std::size_t active_verifications = 0;
  bool closed = false;

  class Verification final : private core::NonCopyableOrMovable {
   public:
    explicit Verification(AsyncCache& cache) : cache_(cache) {
      std::lock_guard lock(cache_.mutex);
      if (!cache_.closed) {
        ++cache_.active_verifications;
        active_ = true;
        core::async_detail::EnterCallback();
      }
    }
    ~Verification() {
      if (active_) {
        core::async_detail::LeaveCallback();
        std::lock_guard lock(cache_.mutex);
        --cache_.active_verifications;
        cache_.finished.notify_all();
      }
    }
    [[nodiscard]] bool IsActive() const { return active_; }

   private:
    AsyncCache& cache_;
    bool active_ = false;
  };

  static AgentCardVerifier SnapshotVerifier(const AgentCardVerifier& verifier, std::shared_ptr<AsyncCache> cache) {
    if (!verifier) {
      return {};
    }
    return [verifier, cache = std::move(cache)](std::string_view json) -> core::Result<void> {
      const Verification verification(*cache);
      if (!verification.IsActive()) {
        return core::Error::Network(kDiscoveryClosed).WithProtocolCode(core::AsyncHandle::kCancelled);
      }
      try {
        return verifier(json);
      } catch (...) {
        return core::Error::Internal(kVerifierThrew);
      }
    };
  }

  void Close() {
    std::lock_guard lock(mutex);
    closed = true;
  }
  void WaitForVerification() {
    if (core::async_detail::IsCallbackThread()) {
      return;
    }
    std::unique_lock lock(mutex);
    finished.wait(lock, [this] { return active_verifications == 0; });
  }
};

DiscoveryClient::DiscoveryClient(HttpFetcher fetcher, std::chrono::seconds cache_ttl, HttpAsyncFetcher async_fetcher)
    : async_cache_(std::make_shared<AsyncCache>()),
      async_fetcher_(std::move(async_fetcher)),
      fetcher_(std::move(fetcher)),
      cache_ttl_(cache_ttl) {}

DiscoveryClient DiscoveryClient::CreateDefault(std::chrono::seconds cache_ttl) {
  return DiscoveryClient(MakeDefaultHttpFetcher(), cache_ttl,
                         [client = http::Client{}](http::Request request, core::Completion<HttpResponse> completion) {
                           return client.SendRequestAsync(
                               std::move(request),
                               [completion = std::move(completion)](core::Result<http::Response> response) {
                                 if (!response.ok()) {
                                   completion(response.error());
                                 } else {
                                   completion(ToDiscoveryHttpResponse(std::move(response.value())));
                                 }
                               });
                         });
}

void DiscoveryClient::SetAgentCardVerifier(AgentCardVerifier verifier) {
  verifier_ = std::move(verifier);
  cache_.clear();
  std::lock_guard lock(async_cache_->mutex);
  async_cache_->entries.clear();
}

namespace {
constexpr std::chrono::seconds kAsyncDiscoveryTimeout{30};
core::Result<http::Request> BuildAsyncDiscoveryRequest(const std::string& url, const CallOptions& options) {
  HeaderMap headers = options.headers;
  if (!options.extensions.empty()) {
    headers[std::string(core::Extensions::kHeaderName)] = core::Extensions::Format(options.extensions);
  }
  if (options.auth_hook) {
    options.auth_hook(headers);
  }
  if (options.credential_provider) {
    auto applied = ApplyCredentialProvider(*options.credential_provider, options.auth_context, &headers);
    if (!applied.ok()) {
      return applied.error();
    }
  }
  const auto version = internal::ApplyProtocolVersion(options, headers);
  if (!version.ok()) {
    return version.error();
  }
  http::Request request;
  request.url = url;
  request.method = std::string(core::http::kMethodGet);
  request.timeout = options.timeout.value_or(kAsyncDiscoveryTimeout);
  request.tls = options.tls;
  request.headers.reserve(headers.size());
  for (const auto& [name, value] : headers) {
    request.headers.push_back({name, value});
  }
  return request;
}
constexpr const char* kMissingAsyncFetcher = "async discovery fetcher is not configured";
constexpr const char* kDiscoveryFailed = "Agent Card discovery failed";
constexpr const char* kMtlsUnsupported = "default async discovery does not support mTLS";
constexpr const char* kDiscoveryStartFailed = "async discovery initiation threw an exception";

}  // namespace
void DiscoveryClient::Shutdown() const {
  const auto cache = async_cache_;
  const auto scope = async_scope_;
  cache->Close();
  scope->Shutdown();
  cache->WaitForVerification();
}
core::Result<core::AsyncHandle> DiscoveryClient::FetchAsync(std::string_view base_url,
                                                            core::Completion<lf::a2a::v1::AgentCard> completion,
                                                            const CallOptions& options) {
  return StartFetchAsync(base_url, false, std::move(completion), options);
}
core::Result<core::AsyncHandle> DiscoveryClient::FetchExtendedAgentCardAsync(
    std::string_view base_url, core::Completion<lf::a2a::v1::AgentCard> completion, const CallOptions& options) {
  return StartFetchAsync(base_url, true, std::move(completion), options);
}

core::Result<core::AsyncHandle> DiscoveryClient::StartFetchAsync(std::string_view base_url, bool extended,
                                                                 core::Completion<lf::a2a::v1::AgentCard> completion,
                                                                 const CallOptions& options) {
  auto started = async_scope_->Start<lf::a2a::v1::AgentCard>(std::move(completion),
                                                             options.timeout.value_or(kAsyncDiscoveryTimeout));
  if (!started.ok()) {
    return started.error();
  }
  auto token = std::move(started.value());
  if (!token.IsActive()) {
    return token.handle();
  }
  auto url = extended ? BuildExtendedDiscoveryUrl(base_url) : BuildDiscoveryUrl(base_url);
  if (!url.ok()) {
    token.Complete(url.error());
    return token.handle();
  }
  try {
    StartDiscoveryRequest(url.value(), extended, options, token);
  } catch (...) {
    token.Complete(core::Error::Internal(kDiscoveryStartFailed));
  }
  return token.handle();
}

void DiscoveryClient::StartDiscoveryRequest(const std::string& url, bool extended, const CallOptions& options,
                                            const core::AsyncCompletion<lf::a2a::v1::AgentCard>& token) {
  // Parsing uses an owned verifier snapshot and never refers back to this client.
  auto verifier = AsyncCache::SnapshotVerifier(verifier_, async_cache_);
  if (!extended) {
    std::optional<CacheEntry> cached;
    {
      std::lock_guard lock(async_cache_->mutex);
      const auto found = async_cache_->entries.find(url);
      if (found != async_cache_->entries.end() && found->second.expires_at >= std::chrono::steady_clock::now()) {
        cached = found->second;
      }
    }
    if (cached && (!verifier || verifier(cached->json).ok())) {
      token.Complete(cached->card);
      return;
    }
  }
  if (!async_fetcher_) {
    token.Complete(core::Error::Validation(kMissingAsyncFetcher));
    return;
  }
  if (options.mtls) {
    token.Complete(core::Error::Validation(kMtlsUnsupported));
    return;
  }
  auto request = BuildAsyncDiscoveryRequest(url, options);
  if (!request.ok()) {
    token.Complete(request.error());
    return;
  }
  auto fetched =
      async_fetcher_(std::move(request.value()), [token, verifier = std::move(verifier), cache = async_cache_, url,
                                                  extended, ttl = cache_ttl_](core::Result<HttpResponse> response) {
        if (!response.ok()) {
          token.Complete(response.error());
          return;
        }
        if (response.value().status_code < kHttpStatusOkMin || response.value().status_code > kHttpStatusOkMax) {
          token.Complete(core::Error::RemoteProtocol(kDiscoveryFailed)
                             .WithTransport(kHttpTransport)
                             .WithHttpStatus(response.value().status_code));
          return;
        }
        auto card = VerifyAndParseAgentCard(response.value().body, verifier);
        if (card.ok() && !extended) {
          std::lock_guard lock(cache->mutex);
          cache->entries.insert_or_assign(url, CacheEntry{.card = card.value(),
                                                          .json = std::move(response.value().body),
                                                          .expires_at = std::chrono::steady_clock::now() + ttl});
        }
        token.Complete(std::move(card));
      });
  if (!fetched.ok()) {
    token.Complete(fetched.error());
    return;
  }
  token.RegisterCancellation([handle = std::move(fetched.value())] { handle.Cancel(); });
}

core::Result<lf::a2a::v1::AgentCard> DiscoveryClient::Fetch(std::string_view base_url) {
  const auto discovery_url = BuildDiscoveryUrl(base_url);
  if (!discovery_url.ok()) {
    return discovery_url.error();
  }

  const auto now = std::chrono::steady_clock::now();
  const auto cached = cache_.find(discovery_url.value());
  if (cached != cache_.end() && cached->second.expires_at >= now) {
    if (!verifier_ || verifier_(cached->second.json).ok()) {
      return cached->second.card;
    }
    // Key policy may have changed before the cache TTL. Re-fetch and verify a
    // replacement; never fall back to a card that no longer verifies.
    cache_.erase(cached);
  }

  auto response = fetcher_(discovery_url.value());
  if (!response.ok()) {
    return response.error();
  }
  if (response.value().status_code == kHttpStatusNotFound) {
    return core::Error::RemoteProtocol("Agent Card not found at well-known discovery endpoint")
        .WithTransport("http")
        .WithHttpStatus(kHttpStatusNotFound);
  }
  if (response.value().status_code < kHttpStatusOkMin || response.value().status_code > kHttpStatusOkMax) {
    return core::Error::RemoteProtocol("Agent Card discovery failed")
        .WithTransport("http")
        .WithHttpStatus(response.value().status_code);
  }

  auto card = VerifyAndParseAgentCard(response.value().body, verifier_);
  if (!card.ok()) {
    return card.error();
  }

  cache_[discovery_url.value()] =
      CacheEntry{.card = card.value(), .json = std::move(response.value().body), .expires_at = now + cache_ttl_};
  return card;
}

core::Result<lf::a2a::v1::AgentCard> DiscoveryClient::FetchExtendedAgentCard(std::string_view base_url) {
  const auto discovery_url = BuildExtendedDiscoveryUrl(base_url);
  if (!discovery_url.ok()) {
    return discovery_url.error();
  }

  const auto response = fetcher_(discovery_url.value());
  if (!response.ok()) {
    return response.error();
  }
  if (response.value().status_code == kHttpStatusNotFound) {
    return core::Error::RemoteProtocol("Extended Agent Card not found at discovery endpoint")
        .WithTransport("http")
        .WithHttpStatus(kHttpStatusNotFound);
  }
  if (response.value().status_code < kHttpStatusOkMin || response.value().status_code > kHttpStatusOkMax) {
    return core::Error::RemoteProtocol("Extended Agent Card discovery failed")
        .WithTransport("http")
        .WithHttpStatus(response.value().status_code);
  }

  return VerifyAndParseAgentCard(response.value().body, verifier_);
}

core::Result<lf::a2a::v1::AgentCard> DiscoveryClient::VerifyAndParseAgentCard(std::string_view json,
                                                                              const AgentCardVerifier& verifier) {
  if (verifier) {
    auto verified = verifier(json);
    if (!verified.ok()) {
      return verified.error();
    }
  }
  lf::a2a::v1::AgentCard card;
  const auto parse = core::JsonToMessage(json, &card, {.ignore_unknown_fields = true});
  if (!parse.ok()) {
    return parse.error();
  }

  const auto validation = ValidateAgentCard(card);
  if (!validation.ok()) {
    return validation.error();
  }

  return card;
}

core::Result<std::string> DiscoveryClient::BuildDiscoveryUrl(std::string_view base_url) {
  std::string normalized = Trim(base_url);
  if (normalized.empty()) {
    return core::Error::Validation("Base URL is required for Agent Card discovery");
  }
  if (!HasHttpScheme(normalized)) {
    return core::Error::Validation("Base URL must start with http:// or https://");
  }

  while (!normalized.empty() && normalized.back() == '/') {
    normalized.pop_back();
  }
  return normalized + std::string(core::protocol_paths::kAgentCard);
}

core::Result<std::string> DiscoveryClient::BuildExtendedDiscoveryUrl(std::string_view base_url) {
  std::string normalized = Trim(base_url);
  if (normalized.empty()) {
    return core::Error::Validation("Base URL is required for extended Agent Card discovery");
  }
  if (!HasHttpScheme(normalized)) {
    return core::Error::Validation("Base URL must start with http:// or https://");
  }

  while (!normalized.empty() && normalized.back() == '/') {
    normalized.pop_back();
  }
  normalized.append(core::protocol_paths::kExtendedAgentCard);
  return normalized;
}

core::Result<void> DiscoveryClient::ValidateAgentCard(const lf::a2a::v1::AgentCard& card) {
  if (card.supported_interfaces().empty()) {
    return core::Error::Validation("Agent Card must include at least one supported interface");
  }

  for (const auto& iface : card.supported_interfaces()) {
    if (iface.protocol_binding().empty()) {
      return core::Error::Validation("Agent Card contains an interface with unspecified protocol binding");
    }
    if (iface.protocol_version().empty()) {
      return core::Error::Validation("Agent Card contains an interface with no protocol version");
    }
    if (!core::Version::IsSupported(iface.protocol_version())) {
      return core::Error::UnsupportedVersion("Only A2A protocol version 1.0 is supported");
    }
    if (iface.url().empty()) {
      return core::Error::Validation("Agent Card contains an interface without a URL");
    }
    if (!IsValidInterfaceEndpoint({.protocol_binding = iface.protocol_binding(), .endpoint = iface.url()})) {
      return core::Error::Validation("Agent Card interface endpoint is invalid for its protocol binding");
    }
    for (const auto& requirement : card.security_requirements()) {
      for (const auto& [scheme_name, _] : requirement.schemes()) {
        if (!card.security_schemes().contains(scheme_name)) {
          return core::Error::Validation("Agent Card security requirement references an unknown security scheme: " +
                                         scheme_name);
        }
      }
    }
  }
  return {};
}

std::array<std::string_view, 3> BuildTransportOrder(std::string_view preferred_wire) {
  if (preferred_wire == kHttpJson) {
    return {kHttpJson, kJsonRpc, kGrpc};
  }
  if (preferred_wire == kJsonRpc) {
    return {kJsonRpc, kHttpJson, kGrpc};
  }
  return {kGrpc, kHttpJson, kJsonRpc};
}

void PopulateSecurityMetadata(const lf::a2a::v1::AgentCard& card, ResolvedInterface& resolved) {
  for (const auto& requirement : card.security_requirements()) {
    for (const auto& [scheme_name, _] : requirement.schemes()) {
      resolved.security_requirements.push_back(scheme_name);
    }
  }
  for (const auto& name : resolved.security_requirements) {
    const auto scheme = card.security_schemes().find(name);
    if (scheme != card.security_schemes().end()) {
      resolved.security_schemes.emplace(name, scheme->second);
    }
  }
}

core::Result<ResolvedInterface> AgentCardResolver::SelectPreferredInterface(const lf::a2a::v1::AgentCard& card,
                                                                            PreferredTransport preferred) {
  const auto preferred_wire = ToWireTransport(preferred);
  if (!preferred_wire.has_value()) {
    return core::Error::Validation("Invalid preferred transport requested");
  }

  for (const auto transport : BuildTransportOrder(preferred_wire.value())) {
    for (const auto& iface : card.supported_interfaces()) {
      if (iface.protocol_binding() != transport || !ValidateInterface(iface).ok()) {
        continue;
      }
      ResolvedInterface resolved;
      resolved.transport = ToPreferredTransport(iface.protocol_binding());
      resolved.url = iface.url();
      PopulateSecurityMetadata(card, resolved);
      return resolved;
    }
  }

  return core::Error::Validation("No usable interface found for this Agent Card");
}

core::Result<void> AgentCardResolver::ValidateInterface(const lf::a2a::v1::AgentInterface& iface) {
  if (iface.protocol_binding().empty()) {
    return core::Error::Validation("Unsupported protocol binding");
  }
  if (iface.url().empty()) {
    return core::Error::Validation("Missing interface URL");
  }
  if (!IsValidInterfaceEndpoint({.protocol_binding = iface.protocol_binding(), .endpoint = iface.url()})) {
    return core::Error::Validation("Interface endpoint is invalid for its protocol binding");
  }
  return {};
}

}  // namespace a2a::client
