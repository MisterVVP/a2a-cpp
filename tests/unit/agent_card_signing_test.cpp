// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "a2a/core/agent_card/agent_card_signing.h"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <string>

#include "a2a/client/discovery.h"
#include "a2a/core/agent_card/agent_card_builder.h"
#include "a2a/core/protojson.h"
#include "support/security/fixtures.h"

namespace {
using namespace a2a::tests::security;
constexpr std::string_view kUnknownKeyMessage = "unknown fixture key";
constexpr std::string_view kBaseUrl = "https://agent.local";
constexpr std::string_view kRestUrl = "https://agent.local/a2a";
constexpr std::string_view kRpcUrl = "https://agent.local/rpc";
constexpr std::string_view kGrpcUrl = "agent.local:443";
constexpr std::string_view kName = "Signing test agent";
constexpr std::string_view kVersion = "1.0.0";
constexpr std::string_view kDescription = "Signed SDK fixture";
constexpr std::string_view kTamperedDescription = "Tampered SDK fixture";
constexpr auto kNow = std::chrono::system_clock::time_point(std::chrono::seconds(1700000000));
constexpr auto kValidityWindow = std::chrono::hours(1);
constexpr std::string_view kCanonicalInput =
    R"({"numbers":[333333333.33333329,1E30,4.50,2e-3,1e-27],"string":"€$\u000f\nA'B\"\\\"/"})";
constexpr std::string_view kCanonicalOutput =
    R"({"numbers":[333333333.3333333,1e+30,4.5,0.002,1e-27],"string":"€$\u000f\nA'B\"\\\"/"})";
constexpr std::string_view kThresholdInput = R"([-0.0,1e-6,1e-7,1e20,1e21,9007199254740992])";
constexpr std::string_view kThresholdOutput = R"([0,0.000001,1e-7,100000000000000000000,1e+21,9007199254740992])";
constexpr std::string_view kUnicodeInput = R"({"דּ":1,"😀":2,"€":3,"\r":4,"1":5,"ö":6,"\u0080":7})";
constexpr std::string_view kEscapedControl = R"(\u0080)";
constexpr std::string_view kUtf8Control = "\xc2\x80";
constexpr std::string_view kUnicodeOutput = R"({"\r":4,"1":5,"\u0080":7,"ö":6,"€":3,"😀":2,"דּ":1})";
constexpr std::string_view kProtectedHeader = "eyJhbGciOiJFZERTQSIsImtpZCI6InJmYzgwMzItdGVzdC1rZXkifQ";
constexpr std::string_view kUnsignedAlgorithmHeader = "eyJhbGciOiJub25lIiwia2lkIjoicmZjODAzMi10ZXN0LWtleSJ9";
constexpr std::string_view kMissingKidHeader = "eyJhbGciOiJFZERTQSJ9";
constexpr std::string_view kUnknownKidHeader = "eyJhbGciOiJFZERTQSIsImtpZCI6InVua25vd24ifQ";
constexpr std::string_view kSignatureArrayPrefix = R"("signatures":[{)";
constexpr std::string_view kUnprotectedHeaderPrefix = R"("header":)";
constexpr std::string_view kJsonMemberSeparator = ",";
constexpr std::array<std::string_view, 4> kInvalidHeaderShapes{R"([])", R"(null)", R"(5)", R"("")"};
constexpr std::string_view kUnknownFieldSuffix = R"(,"unknownSignedExtension":true})";
constexpr std::string_view kDuplicateKeys = R"({"key":1,"\u006bey":2})";
constexpr std::string_view kLoneSurrogate = R"({"key":"\ud800"})";
constexpr int kHttpOk = 200;
constexpr std::size_t kTwoSignatures = 2;

lf::a2a::v1::AgentCard Card() {
  return a2a::core::AgentCardBuilder::ConformancePreset({.rest_url = std::string(kRestUrl),
                                                         .json_rpc_url = std::string(kRpcUrl),
                                                         .grpc_url = std::string(kGrpcUrl)},
                                                        kName, kVersion, kDescription)
      .Build();
}
a2a::core::AgentCardVerificationKey Key() {
  return {.public_key_pem = std::string(kPublicKey),
          .valid_from = kNow - kValidityWindow,
          .valid_until = kNow + kValidityWindow,
          .revoked = false};
}
a2a::core::AgentCardKeyResolver Resolver(a2a::core::AgentCardVerificationKey key) {
  return [key = std::move(key)](std::string_view id) -> a2a::core::Result<a2a::core::AgentCardVerificationKey> {
    if (id != kKeyId) {
      return a2a::core::Error::Validation(std::string(kUnknownKeyMessage));
    }
    return key;
  };
}
TEST(AgentCardSigningTest, Rfc8785NumberAndStringVectors) {
  auto canonical = a2a::core::CanonicalizeJson(kCanonicalInput);
  ASSERT_TRUE(canonical.ok());
  EXPECT_EQ(canonical.value(), kCanonicalOutput);
  auto thresholds = a2a::core::CanonicalizeJson(kThresholdInput);
  ASSERT_TRUE(thresholds.ok());
  EXPECT_EQ(thresholds.value(), kThresholdOutput);
}
TEST(AgentCardSigningTest, SortsUtf16AndRejectsDuplicatesAndInvalidUnicode) {
  auto canonical = a2a::core::CanonicalizeJson(kUnicodeInput);
  ASSERT_TRUE(canonical.ok());
  // JCS emits U+0080 as UTF-8, not an escaped sequence.
  std::string expected(kUnicodeOutput);
  const auto escape = expected.find(kEscapedControl);
  expected.replace(escape, kEscapedControl.size(), kUtf8Control);
  EXPECT_EQ(canonical.value(), expected);
  EXPECT_FALSE(a2a::core::CanonicalizeJson(kDuplicateKeys).ok());
  EXPECT_FALSE(a2a::core::CanonicalizeJson(kLoneSurrogate).ok());
}
TEST(AgentCardSigningTest, SignsDeterministicallyAndExcludesAllSignaturesFromPayload) {
  auto signed_card = a2a::core::SignAgentCard(Card(), kKeyId, kPrivateKey);
  ASSERT_TRUE(signed_card.ok());
  EXPECT_EQ(signed_card.value().signatures(0).protected_(), kProtectedHeader);
  auto twice = a2a::core::SignAgentCard(signed_card.value(), kKeyId, kPrivateKey);
  ASSERT_TRUE(twice.ok());
  ASSERT_EQ(twice.value().signatures_size(), kTwoSignatures);
  EXPECT_EQ(twice.value().signatures(0).signature(), twice.value().signatures(1).signature());
  auto json = a2a::core::MessageToJson(twice.value());
  ASSERT_TRUE(json.ok());
  EXPECT_TRUE(a2a::core::VerifyAgentCardJson(json.value(), Resolver(Key()), kNow).ok());
}
TEST(AgentCardSigningTest, RejectsTamperingUnknownKeysAndExpiredOrRevokedKeys) {
  auto signed_card = a2a::core::SignAgentCard(Card(), kKeyId, kPrivateKey);
  ASSERT_TRUE(signed_card.ok());
  auto json = a2a::core::MessageToJson(signed_card.value());
  ASSERT_TRUE(json.ok());
  auto key = Key();
  key.revoked = true;
  EXPECT_FALSE(a2a::core::VerifyAgentCardJson(json.value(), Resolver(key), kNow).ok());
  key = Key();
  key.valid_until = kNow;
  EXPECT_FALSE(a2a::core::VerifyAgentCardJson(json.value(), Resolver(key), kNow).ok());
  key = Key();
  key.valid_from = kNow + kValidityWindow;
  EXPECT_FALSE(a2a::core::VerifyAgentCardJson(json.value(), Resolver(key), kNow).ok());
  signed_card.value().set_description(std::string(kTamperedDescription));
  auto tampered = a2a::core::MessageToJson(signed_card.value());
  ASSERT_TRUE(tampered.ok());
  EXPECT_FALSE(a2a::core::VerifyAgentCardJson(tampered.value(), Resolver(Key()), kNow).ok());
  EXPECT_FALSE(a2a::core::SignAgentCard(Card(), kKeyId, kPublicKey).ok());
  EXPECT_FALSE(a2a::core::SignAgentCard(Card(), {}, kPrivateKey).ok());
}
TEST(AgentCardSigningTest, RejectsMissingHeadersUnsafeAlgorithmsAndUnknownKeyIds) {
  auto signed_card = a2a::core::SignAgentCard(Card(), kKeyId, kPrivateKey);
  ASSERT_TRUE(signed_card.ok());
  for (const auto header : {kUnsignedAlgorithmHeader, kMissingKidHeader, kUnknownKidHeader}) {
    signed_card.value().mutable_signatures(0)->set_protected_(std::string(header));
    auto json = a2a::core::MessageToJson(signed_card.value());
    ASSERT_TRUE(json.ok());
    EXPECT_FALSE(a2a::core::VerifyAgentCardJson(json.value(), Resolver(Key()), kNow).ok());
  }
}
TEST(AgentCardSigningTest, RawDiscoveryVerificationIncludesUnknownFieldsAndRequiresSignatures) {
  auto signed_card = a2a::core::SignAgentCard(Card(), kKeyId, kPrivateKey);
  ASSERT_TRUE(signed_card.ok());
  auto json = a2a::core::MessageToJson(signed_card.value());
  ASSERT_TRUE(json.ok());
  json.value().pop_back();
  json.value().append(kUnknownFieldSuffix);
  EXPECT_FALSE(a2a::core::VerifyAgentCardJson(json.value(), Resolver(Key()), kNow).ok());
  auto unsigned_json = a2a::core::MessageToJson(Card());
  ASSERT_TRUE(unsigned_json.ok());
  EXPECT_FALSE(a2a::core::VerifyAgentCardJson(unsigned_json.value(), Resolver(Key()), kNow).ok());
}

TEST(AgentCardSigningTest, RejectsMalformedUnprotectedHeaderTypes) {
  auto signed_card = a2a::core::SignAgentCard(Card(), kKeyId, kPrivateKey);
  ASSERT_TRUE(signed_card.ok());
  auto serialized = a2a::core::MessageToJson(signed_card.value());
  ASSERT_TRUE(serialized.ok());
  const auto at = serialized.value().find(kSignatureArrayPrefix);
  ASSERT_NE(at, std::string::npos);
  for (const auto header : kInvalidHeaderShapes) {
    auto json = serialized.value();
    json.insert(at + kSignatureArrayPrefix.size(),
                std::string(kUnprotectedHeaderPrefix).append(header).append(kJsonMemberSeparator));
    EXPECT_FALSE(a2a::core::VerifyAgentCardJson(json, Resolver(Key()), kNow).ok());
  }
}

TEST(AgentCardSigningTest, DiscoveryRevalidatesCachedCardsAgainstCurrentKeyPolicy) {
  auto signed_card = a2a::core::SignAgentCard(Card(), kKeyId, kPrivateKey);
  ASSERT_TRUE(signed_card.ok());
  auto json = a2a::core::MessageToJson(signed_card.value());
  ASSERT_TRUE(json.ok());
  bool revoked = false;
  a2a::client::DiscoveryClient discovery(
      [body = json.value()](std::string_view) -> a2a::core::Result<a2a::client::HttpResponse> {
        return a2a::client::HttpResponse{.status_code = kHttpOk, .body = body};
      });
  discovery.SetAgentCardVerifier([&revoked](std::string_view body) {
    auto key = Key();
    key.revoked = revoked;
    return a2a::core::VerifyAgentCardJson(body, Resolver(key), kNow);
  });
  EXPECT_TRUE(discovery.Fetch(kBaseUrl).ok());
  revoked = true;
  EXPECT_FALSE(discovery.Fetch(kBaseUrl).ok());
}
}  // namespace
