#include "a2a/client/auth.h"

#include <gtest/gtest.h>

#include <memory>
#include <string>

namespace {

class FakeTokenProvider final : public a2a::client::OAuth2TokenProvider {
 public:
  explicit FakeTokenProvider(a2a::core::Result<std::string> token) : token_(std::move(token)) {}

  a2a::core::Result<std::string> GetAccessToken(
      const a2a::client::AuthContext& context) const override {
    (void)context;
    return token_;
  }

 private:
  a2a::core::Result<std::string> token_;
};

TEST(AuthTest, ApiKeyProviderValidatesInputAndBuildsHeaders) {
  const a2a::client::AuthContext context;
  a2a::client::ApiKeyCredentialProvider empty_key("", "X-API-Key");
  EXPECT_FALSE(empty_key.GetHeaders(context).ok());

  a2a::client::ApiKeyCredentialProvider empty_header("key", "");
  EXPECT_FALSE(empty_header.GetHeaders(context).ok());

  a2a::client::ApiKeyCredentialProvider valid("key", "X-API-Key");
  const auto headers = valid.GetHeaders(context);
  ASSERT_TRUE(headers.ok());
  EXPECT_EQ(headers.value().at("X-API-Key"), "key");
}

TEST(AuthTest, BearerAndCustomProvidersValidateAndReturnHeaders) {
  const a2a::client::AuthContext context;
  a2a::client::BearerTokenCredentialProvider empty_token("");
  EXPECT_FALSE(empty_token.GetHeaders(context).ok());

  a2a::client::BearerTokenCredentialProvider bearer("tkn");
  const auto bearer_headers = bearer.GetHeaders(context);
  ASSERT_TRUE(bearer_headers.ok());
  EXPECT_EQ(bearer_headers.value().at("Authorization"), "Bearer tkn");

  a2a::client::CustomHeaderCredentialProvider empty_custom({});
  EXPECT_FALSE(empty_custom.GetHeaders(context).ok());

  a2a::client::CustomHeaderCredentialProvider custom({{"X-A", "B"}});
  const auto custom_headers = custom.GetHeaders(context);
  ASSERT_TRUE(custom_headers.ok());
  EXPECT_EQ(custom_headers.value().at("X-A"), "B");
}

TEST(AuthTest, OAuth2ProviderCoversSuccessAndFailurePaths) {
  const a2a::client::AuthContext context;
  a2a::client::OAuth2BearerCredentialProvider no_provider(nullptr);
  EXPECT_FALSE(no_provider.GetHeaders(context).ok());

  auto provider_error = std::make_shared<FakeTokenProvider>(a2a::core::Error::Network("nope"));
  a2a::client::OAuth2BearerCredentialProvider error_case(provider_error);
  EXPECT_FALSE(error_case.GetHeaders(context).ok());

  auto provider_empty = std::make_shared<FakeTokenProvider>(std::string(""));
  a2a::client::OAuth2BearerCredentialProvider empty_case(provider_empty);
  EXPECT_FALSE(empty_case.GetHeaders(context).ok());

  auto provider_ok = std::make_shared<FakeTokenProvider>(std::string("ok-token"));
  a2a::client::OAuth2BearerCredentialProvider ok_case(provider_ok);
  const auto headers = ok_case.GetHeaders(context);
  ASSERT_TRUE(headers.ok());
  EXPECT_EQ(headers.value().at("Authorization"), "Bearer ok-token");
}

TEST(AuthTest, ApplyCredentialProviderValidatesAndMergesHeaders) {
  const a2a::client::AuthContext context;
  a2a::client::ApiKeyCredentialProvider provider("abc", "X-API-Key");
  EXPECT_FALSE(a2a::client::ApplyCredentialProvider(provider, context, nullptr).ok());

  a2a::client::HeaderMap headers{{"X-Existing", "keep"}};
  const auto applied = a2a::client::ApplyCredentialProvider(provider, context, &headers);
  ASSERT_TRUE(applied.ok());
  EXPECT_EQ(headers.at("X-API-Key"), "abc");
  EXPECT_EQ(headers.at("X-Existing"), "keep");
}

}  // namespace
