#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "a2a/core/result.h"
#include "a2a/v1/a2a.pb.h"

namespace a2a::client {

using HeaderMap = std::unordered_map<std::string, std::string>;
using AuthHeaderHook = std::function<void(HeaderMap& headers)>;

struct MtlsConfig final {
  std::string client_certificate_pem;
  std::string client_private_key_pem;
  std::string trusted_ca_pem;
  std::string server_name_override;
};

struct AuthContext final {
  std::vector<std::string> security_requirements;
  std::unordered_map<std::string, lf::a2a::v1::SecurityScheme> security_schemes;
  std::vector<std::string> oauth2_scopes;
  std::optional<std::string> audience;
};

class CredentialProvider {
 public:
  virtual ~CredentialProvider() = default;
  [[nodiscard]] virtual core::Result<HeaderMap> GetHeaders(const AuthContext& context) const = 0;
};

class ApiKeyCredentialProvider final : public CredentialProvider {
 public:
  ApiKeyCredentialProvider(std::string api_key, std::string header_name = "X-API-Key");

  [[nodiscard]] core::Result<HeaderMap> GetHeaders(const AuthContext& context) const override;

 private:
  std::string api_key_;
  std::string header_name_;
};

class BearerTokenCredentialProvider final : public CredentialProvider {
 public:
  explicit BearerTokenCredentialProvider(std::string token);

  [[nodiscard]] core::Result<HeaderMap> GetHeaders(const AuthContext& context) const override;

 private:
  std::string token_;
};

class CustomHeaderCredentialProvider final : public CredentialProvider {
 public:
  explicit CustomHeaderCredentialProvider(HeaderMap headers);

  [[nodiscard]] core::Result<HeaderMap> GetHeaders(const AuthContext& context) const override;

 private:
  HeaderMap headers_;
};

class OAuth2TokenProvider {
 public:
  virtual ~OAuth2TokenProvider() = default;
  [[nodiscard]] virtual core::Result<std::string> GetAccessToken(
      const AuthContext& context) const = 0;
};

class OAuth2BearerCredentialProvider final : public CredentialProvider {
 public:
  explicit OAuth2BearerCredentialProvider(std::shared_ptr<OAuth2TokenProvider> token_provider);

  [[nodiscard]] core::Result<HeaderMap> GetHeaders(const AuthContext& context) const override;

 private:
  std::shared_ptr<OAuth2TokenProvider> token_provider_;
};

[[nodiscard]] core::Result<void> ApplyCredentialProvider(const CredentialProvider& provider,
                                                         const AuthContext& context,
                                                         HeaderMap* headers);

}  // namespace a2a::client
