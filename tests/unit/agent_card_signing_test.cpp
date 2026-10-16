// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "a2a/core/agent_card/agent_card_signing.h"

#include <google/protobuf/struct.pb.h>
#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>

#include "a2a/client/discovery.h"
#include "a2a/core/agent_card/agent_card_builder.h"
#include "a2a/core/non_copyable.h"
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
constexpr std::string_view kProtectedHeader = "eyJhbGciOiJFZERTQSIsImtpZCI6InJmYzgwMzItdGVzdC1rZXkiLCJ0eXAiOiJKT1NFIn0";
constexpr std::string_view kUnsignedAlgorithmHeader = "eyJhbGciOiJub25lIiwia2lkIjoicmZjODAzMi10ZXN0LWtleSJ9";
constexpr std::string_view kMissingKidHeader = "eyJhbGciOiJFZERTQSJ9";
constexpr std::string_view kUnknownKidHeader = "eyJhbGciOiJFZERTQSIsImtpZCI6InVua25vd24ifQ";
constexpr std::string_view kSignatureArrayPrefix = R"("signatures":[{)";
constexpr std::string_view kUnprotectedHeaderPrefix = R"("header":)";
constexpr std::string_view kJsonMemberSeparator = ",";
constexpr std::array<std::string_view, 4> kInvalidHeaderShapes{R"([])", R"(null)", R"(5)", R"("")"};
constexpr std::string_view kUnknownFieldSuffix = R"(,"unknownSignedExtension":true})";
constexpr int kHttpOk = 200;
constexpr std::size_t kTwoSignatures = 2;
constexpr std::string_view kRotatedKeyId = "rotated-fixture-key";
constexpr std::string_view kRotatedDescription = "Rotated SDK fixture";
// RFC 8032 section 7.1, second public Ed25519 test vector.
constexpr std::string_view kRotatedPrivateKey = R"(-----BEGIN PRIVATE KEY-----
MC4CAQAwBQYDK2VwBCIEIEzNCJso/5banbbDRuwRTg9bijGfNaumJNqM9u1PuKb7
-----END PRIVATE KEY-----
)";
constexpr std::string_view kRotatedPublicKey = R"(-----BEGIN PUBLIC KEY-----
MCowBQYDK2VwAyEAPUAXw+hDiVqStwqnTRt+vJyYLM8uxJaMwM1V8Sr0Zgw=
-----END PUBLIC KEY-----
)";
constexpr std::size_t kInitialFetchCount = 1;
constexpr std::size_t kRefreshedFetchCount = 2;
constexpr std::size_t kRetriedFetchCount = 3;
constexpr std::string_view kDiscoveryUnavailable = "fixture discovery unavailable";
constexpr std::string_view kInvalidDiscoveryJson = "invalid discovery JSON";
constexpr std::size_t kDiscoveryEndpointCount = 2;

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

a2a::core::Result<std::string> SignedCardJson(std::string_view id, std::string_view description,
                                              std::string_view private_key = kPrivateKey) {
  auto card = Card();
  card.set_description(std::string(description));
  auto signed_card = a2a::core::SignAgentCard(card, id, private_key);
  if (!signed_card.ok()) {
    return signed_card.error();
  }
  return a2a::core::MessageToJson(signed_card.value());
}

std::string DiscoveryCardJson(std::string_view id, std::string_view description,
                              std::string_view private_key = kPrivateKey) {
  auto json = SignedCardJson(id, description, private_key);
  EXPECT_TRUE(json.ok());
  return json.ok() ? std::move(json.value()) : std::string{};
}

TEST(AgentCardSigningTest, DiscoveryRevalidatesCachedCardsAgainstCurrentKeyPolicy) {
  const auto json = DiscoveryCardJson(kKeyId, kDescription);
  bool revoked = false;
  std::size_t fetches = 0;
  a2a::client::DiscoveryClient discovery(
      [body = json, &fetches](std::string_view) -> a2a::core::Result<a2a::client::HttpResponse> {
        ++fetches;
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
  EXPECT_EQ(fetches, kRefreshedFetchCount);
  EXPECT_FALSE(discovery.Fetch(kBaseUrl).ok());
  EXPECT_EQ(fetches, kRetriedFetchCount);
}

TEST(AgentCardSigningTest, DiscoveryVerifiesPublicAndExtendedBodiesBeforeParsing) {
  a2a::client::DiscoveryClient discovery([](std::string_view) -> a2a::core::Result<a2a::client::HttpResponse> {
    return a2a::client::HttpResponse{.status_code = kHttpOk, .body = std::string(kInvalidDiscoveryJson)};
  });
  std::size_t checks = 0;
  discovery.SetAgentCardVerifier([&checks](std::string_view json) -> a2a::core::Result<void> {
    ++checks;
    EXPECT_EQ(json, kInvalidDiscoveryJson);
    return a2a::core::Error::Validation(std::string(kUnknownKeyMessage));
  });
  for (const bool extended : {false, true}) {
    auto result = extended ? discovery.FetchExtendedAgentCard(kBaseUrl) : discovery.Fetch(kBaseUrl);
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.error().message(), kUnknownKeyMessage);
  }
  EXPECT_EQ(checks, kDiscoveryEndpointCount);
}

class DiscoveryRotation final : private a2a::core::NonCopyable {
 public:
  explicit DiscoveryRotation(bool expire)
      : expire_(expire),
        body_(DiscoveryCardJson(kKeyId, kDescription)),
        rotated_(DiscoveryCardJson(kRotatedKeyId, kRotatedDescription, kRotatedPrivateKey)),
        discovery_([this](std::string_view) { return Fetch(); }) {
    discovery_.SetAgentCardVerifier([this](std::string_view json) {
      return a2a::core::VerifyAgentCardJson(json, [this](std::string_view id) { return Resolve(id); }, kNow);
    });
  }
  void CheckInitialCache() {
    ASSERT_TRUE(discovery_.Fetch(kBaseUrl).ok());
    ASSERT_TRUE(discovery_.Fetch(kBaseUrl).ok());
    EXPECT_EQ(fetches_, kInitialFetchCount);
  }
  void CheckRefreshFailure() {
    retired_ = true;
    body_ = rotated_;
    unavailable_ = true;
    auto failed = discovery_.Fetch(kBaseUrl);
    ASSERT_FALSE(failed.ok());
    EXPECT_EQ(failed.error().code(), a2a::core::ErrorCode::kNetwork);
  }
  void CheckReplacementCache() {
    unavailable_ = false;
    auto replacement = discovery_.Fetch(kBaseUrl);
    ASSERT_TRUE(replacement.ok());
    EXPECT_EQ(replacement.value().description(), kRotatedDescription);
    ASSERT_TRUE(discovery_.Fetch(kBaseUrl).ok());
    EXPECT_EQ(fetches_, kRetriedFetchCount);
  }

 private:
  a2a::core::Result<a2a::client::HttpResponse> Fetch() {
    ++fetches_;
    if (unavailable_) {
      return a2a::core::Error::Network(std::string(kDiscoveryUnavailable));
    }
    return a2a::client::HttpResponse{.status_code = kHttpOk, .body = body_};
  }
  a2a::core::Result<a2a::core::AgentCardVerificationKey> Resolve(std::string_view id) const {
    if (id != kKeyId && id != kRotatedKeyId) {
      return a2a::core::Error::Validation(std::string(kUnknownKeyMessage));
    }
    auto key = Key();
    key.public_key_pem = id == kRotatedKeyId ? kRotatedPublicKey : kPublicKey;
    if (id == kKeyId && retired_) {
      key.revoked = !expire_;
      key.valid_until = expire_ ? kNow : key.valid_until;
    }
    return key;
  }
  bool expire_;
  bool retired_ = false;
  bool unavailable_ = false;
  std::size_t fetches_ = 0;
  std::string body_;
  std::string rotated_;
  a2a::client::DiscoveryClient discovery_;
};

void CheckDiscoveryRotation(bool expire) {
  DiscoveryRotation rotation(expire);
  rotation.CheckInitialCache();
  rotation.CheckRefreshFailure();
  rotation.CheckReplacementCache();
}

TEST(AgentCardSigningTest, DiscoveryRefreshesAfterCachedSigningKeyIsRevoked) { CheckDiscoveryRotation(false); }

TEST(AgentCardSigningTest, DiscoveryRefreshesAfterCachedSigningKeyExpires) { CheckDiscoveryRotation(true); }

constexpr std::string_view kVectorCard = "card";
constexpr std::string_view kVectorKid = "kid";
constexpr std::string_view kVectorPublicKey = "publicKeyPem";
constexpr std::string_view kUnprotectedComment = R"({"comment":"public annotation"})";
constexpr std::string_view kOverlappingUnprotected = R"({"alg":"EdDSA"})";

google::protobuf::Struct ReadVectorFile(const char* path) {
  std::ifstream file(path);
  EXPECT_TRUE(file.is_open());
  const std::string json{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
  google::protobuf::Struct vector;
  EXPECT_TRUE(a2a::core::JsonToMessage(json, &vector).ok());
  return vector;
}

a2a::core::AgentCardKeyResolver VectorResolver(const google::protobuf::Struct& vector) {
  auto key = Key();
  key.public_key_pem = vector.fields().at(std::string(kVectorPublicKey)).string_value();
  const auto kid = vector.fields().at(std::string(kVectorKid)).string_value();
  return [key, kid](std::string_view id) -> a2a::core::Result<a2a::core::AgentCardVerificationKey> {
    if (id != kid) {
      return a2a::core::Error::Validation(std::string(kUnknownKeyMessage));
    }
    return key;
  };
}

TEST(AgentCardSigningTest, AcceptsPythonAndGoSharedSignedCardWithJoseHeader) {
  const auto vector = ReadVectorFile(A2A_SIGNING_INTEROP_VECTOR_FILE);
  const auto resolver = VectorResolver(vector);
  auto raw = a2a::core::MessageToJson(vector.fields().at(std::string(kVectorCard)));
  ASSERT_TRUE(raw.ok());
  EXPECT_TRUE(a2a::core::VerifyAgentCardJson(raw.value(), resolver, kNow).ok());
}

TEST(AgentCardSigningTest, TrustedSignatureSurvivesUnknownInvalidAndRevokedRotationKeys) {
  auto card = a2a::core::SignAgentCard(Card(), kKeyId, kPrivateKey);
  ASSERT_TRUE(card.ok());
  auto* unknown = card.value().add_signatures();
  *unknown = card.value().signatures(0);
  unknown->set_protected_(std::string(kUnknownKidHeader));
  card.value().mutable_signatures()->SwapElements(0, 1);
  auto raw = a2a::core::MessageToJson(card.value());
  ASSERT_TRUE(raw.ok());
  EXPECT_TRUE(a2a::core::VerifyAgentCardJson(raw.value(), Resolver(Key()), kNow).ok());
  auto revoked = Key();
  revoked.revoked = true;
  EXPECT_FALSE(a2a::core::VerifyAgentCardJson(raw.value(), Resolver(revoked), kNow).ok());
  card.value().mutable_signatures()->RemoveLast();
  raw = a2a::core::MessageToJson(card.value());
  ASSERT_TRUE(raw.ok());
  EXPECT_FALSE(a2a::core::VerifyAgentCardJson(raw.value(), Resolver(Key()), kNow).ok());
}

lf::a2a::v1::AgentCard SignatureLimitCard() {
  auto signed_card = a2a::core::SignAgentCard(Card(), kKeyId, kPrivateKey);
  EXPECT_TRUE(signed_card.ok());
  if (!signed_card.ok()) {
    return {};
  }
  auto card = std::move(signed_card.value());
  const auto valid = card.signatures(0);
  card.clear_signatures();
  for (std::size_t index = 1; index < a2a::core::kMaxAgentCardSignatures; ++index) {
    auto* unknown = card.add_signatures();
    *unknown = valid;
    unknown->set_protected_(std::string(kUnknownKidHeader));
  }
  *card.add_signatures() = valid;
  return card;
}

TEST(AgentCardSigningTest, AcceptsValidLastSignatureAtLimitAndRefusesFurtherSigning) {
  auto card = SignatureLimitCard();
  auto json = a2a::core::MessageToJson(card);
  ASSERT_TRUE(json.ok());
  std::size_t calls = 0;
  auto resolver = [&calls](std::string_view id) {
    ++calls;
    return Resolver(Key())(id);
  };
  EXPECT_TRUE(a2a::core::VerifyAgentCardJson(json.value(), resolver, kNow).ok());
  EXPECT_EQ(calls, a2a::core::kMaxAgentCardSignatures);
  EXPECT_FALSE(a2a::core::SignAgentCard(card, kKeyId, kPrivateKey).ok());
  card.mutable_signatures()->RemoveLast();
  EXPECT_TRUE(a2a::core::SignAgentCard(card, kKeyId, kPrivateKey).ok());
}

TEST(AgentCardSigningTest, RejectsOversizedSignatureArraysBeforeResolvingAnyKeys) {
  auto card = SignatureLimitCard();
  const auto valid = card.signatures(card.signatures_size() - 1);
  *card.add_signatures() = valid;
  std::size_t calls = 0;
  auto resolver = [&calls](std::string_view id) {
    ++calls;
    return Resolver(Key())(id);
  };
  for (const bool valid_first : {false, true}) {
    if (valid_first) {
      card.mutable_signatures()->SwapElements(0, card.signatures_size() - 1);
    }
    auto json = a2a::core::MessageToJson(card);
    ASSERT_TRUE(json.ok());
    EXPECT_FALSE(a2a::core::VerifyAgentCardJson(json.value(), resolver, kNow).ok());
    EXPECT_EQ(calls, 0U);
  }
}

TEST(AgentCardSigningTest, DiscoveryRejectsOversizedSignatureArraysWithoutCachingThem) {
  auto card = SignatureLimitCard();
  *card.add_signatures() = card.signatures(0);
  auto oversized = a2a::core::MessageToJson(card);
  ASSERT_TRUE(oversized.ok());
  std::string body = oversized.value();
  std::size_t calls = 0;
  a2a::client::DiscoveryClient discovery([&body](std::string_view) -> a2a::core::Result<a2a::client::HttpResponse> {
    return a2a::client::HttpResponse{.status_code = kHttpOk, .body = body};
  });
  discovery.SetAgentCardVerifier([&calls](std::string_view json) {
    return a2a::core::VerifyAgentCardJson(
        json,
        [&calls](std::string_view id) {
          ++calls;
          return Resolver(Key())(id);
        },
        kNow);
  });
  EXPECT_FALSE(discovery.Fetch(kBaseUrl).ok());
  EXPECT_EQ(calls, 0U);
  body = DiscoveryCardJson(kKeyId, kDescription);
  EXPECT_TRUE(discovery.Fetch(kBaseUrl).ok());
}

TEST(AgentCardSigningTest, AcceptsUnprotectedAnnotationsButRejectsOverlappingHeaders) {
  auto card = a2a::core::SignAgentCard(Card(), kKeyId, kPrivateKey);
  ASSERT_TRUE(card.ok());
  auto* header = card.value().mutable_signatures(0)->mutable_header();
  ASSERT_TRUE(a2a::core::JsonToMessage(kUnprotectedComment, header).ok());
  auto raw = a2a::core::MessageToJson(card.value());
  ASSERT_TRUE(raw.ok());
  EXPECT_TRUE(a2a::core::VerifyAgentCardJson(raw.value(), Resolver(Key()), kNow).ok());
  header->Clear();
  ASSERT_TRUE(a2a::core::JsonToMessage(kOverlappingUnprotected, header).ok());
  raw = a2a::core::MessageToJson(card.value());
  ASSERT_TRUE(raw.ok());
  EXPECT_FALSE(a2a::core::VerifyAgentCardJson(raw.value(), Resolver(Key()), kNow).ok());
}

constexpr std::string_view kVectorSignatures = "signatures";
constexpr std::string_view kHeaderVectors = "vectors";
constexpr std::string_view kHeaderVectorId = "id";
constexpr std::string_view kHeaderAccepted = "accepted";
constexpr int kHeaderVectorCount = 4;

TEST(AgentCardSigningTest, IndependentSignedJwsHeaderCases) {
  const auto corpus = ReadVectorFile(A2A_SIGNING_INTEROP_VECTOR_FILE);
  const auto resolver = VectorResolver(corpus);
  const auto& vectors = corpus.fields().at(std::string(kHeaderVectors)).list_value().values();
  ASSERT_EQ(vectors.size(), kHeaderVectorCount);
  for (const auto& vector : vectors) {
    const auto& fields = vector.struct_value().fields();
    SCOPED_TRACE(fields.at(std::string(kHeaderVectorId)).string_value());
    auto card = corpus.fields().at(std::string(kVectorCard));
    (*card.mutable_struct_value()->mutable_fields())[std::string(kVectorSignatures)] =
        fields.at(std::string(kVectorSignatures));
    auto raw = a2a::core::MessageToJson(card);
    ASSERT_TRUE(raw.ok());
    EXPECT_EQ(a2a::core::VerifyAgentCardJson(raw.value(), resolver, kNow).ok(),
              fields.at(std::string(kHeaderAccepted)).bool_value());
  }
}

}  // namespace
