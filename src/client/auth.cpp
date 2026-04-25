#include "a2a/client/auth.h"

#include <utility>

#include "a2a/core/error.h"

namespace a2a::client {

ApiKeyCredentialProvider::ApiKeyCredentialProvider(std::string api_key, std::string header_name)
    : api_key_(std::move(api_key)), header_name_(std::move(header_name)) {}

core::Result<HeaderMap> ApiKeyCredentialProvider::GetHeaders(const AuthContext& context) const {
  (void)context;
  if (api_key_.empty()) {
    return core::Error::Validation("API key is required");
  }
  if (header_name_.empty()) {
    return core::Error::Validation("API key header name is required");
  }

  HeaderMap headers;
  headers.emplace(header_name_, api_key_);
  return headers;
}

BearerTokenCredentialProvider::BearerTokenCredentialProvider(std::string token)
    : token_(std::move(token)) {}

core::Result<HeaderMap> BearerTokenCredentialProvider::GetHeaders(
    const AuthContext& context) const {
  (void)context;
  if (token_.empty()) {
    return core::Error::Validation("Bearer token is required");
  }

  HeaderMap headers;
  headers.emplace("Authorization", "Bearer " + token_);
  return headers;
}

CustomHeaderCredentialProvider::CustomHeaderCredentialProvider(HeaderMap headers)
    : headers_(std::move(headers)) {}

core::Result<HeaderMap> CustomHeaderCredentialProvider::GetHeaders(
    const AuthContext& context) const {
  (void)context;
  if (headers_.empty()) {
    return core::Error::Validation("At least one custom auth header is required");
  }
  return headers_;
}

OAuth2BearerCredentialProvider::OAuth2BearerCredentialProvider(
    std::shared_ptr<OAuth2TokenProvider> token_provider)
    : token_provider_(std::move(token_provider)) {}

core::Result<HeaderMap> OAuth2BearerCredentialProvider::GetHeaders(
    const AuthContext& context) const {
  if (token_provider_ == nullptr) {
    return core::Error::Internal("OAuth2 token provider is not configured");
  }

  const auto token = token_provider_->GetAccessToken(context);
  if (!token.ok()) {
    return token.error();
  }
  if (token.value().empty()) {
    return core::Error::Validation("OAuth2 token provider returned an empty access token");
  }

  HeaderMap headers;
  headers.emplace("Authorization", "Bearer " + token.value());
  return headers;
}

core::Result<void> ApplyCredentialProvider(const CredentialProvider& provider,
                                           const AuthContext& context, HeaderMap* headers) {
  if (headers == nullptr) {
    return core::Error::Internal("Auth header output map is required");
  }

  const auto auth_headers = provider.GetHeaders(context);
  if (!auth_headers.ok()) {
    return auth_headers.error();
  }

  for (const auto& [name, value] : auth_headers.value()) {
    headers->insert_or_assign(name, value);
  }
  return {};
}

}  // namespace a2a::client
