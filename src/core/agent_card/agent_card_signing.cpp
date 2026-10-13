// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "a2a/core/agent_card/agent_card_signing.h"

#include <openssl/evp.h>
#include <openssl/pem.h>

#include <algorithm>
#include <limits>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

#include "a2a/core/protojson.h"
#include "json_canonicalization.h"

namespace a2a::core {
namespace {
constexpr std::string_view kAlgorithm = "EdDSA";
constexpr char kKeyType[] = "ED25519";
constexpr std::string_view kSignatures = "signatures";
constexpr std::string_view kProtected = "protected";
constexpr std::string_view kSignature = "signature";
constexpr std::string_view kUnprotectedHeader = "header";
constexpr std::size_t kRequiredHeaderSize = 2;
constexpr std::string_view kAlg = "alg";
constexpr std::string_view kKid = "kid";
constexpr std::string_view kInvalidSignature = "invalid Agent Card signature or key policy";
constexpr std::size_t kEd25519SignatureSize = 64;
constexpr std::size_t kMaximumKeyIdSize = 1024;
constexpr std::size_t kMaximumKeySize = 16384;
constexpr std::size_t kBase64BlockSize = 4;
constexpr std::size_t kBase64InputBlockSize = 3;
using Key = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
using Bio = std::unique_ptr<BIO, decltype(&BIO_free)>;
using Digest = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;

[[noreturn]] void InvalidSignature() { throw std::invalid_argument(std::string(kInvalidSignature)); }

std::string Base64Url(std::string_view input) {
  const auto encoded_size = kBase64BlockSize * ((input.size() + kBase64InputBlockSize - 1) / kBase64InputBlockSize);
  std::string output(encoded_size + 1, '\0');
  const int size =
      EVP_EncodeBlock(reinterpret_cast<unsigned char*>(output.data()),
                      reinterpret_cast<const unsigned char*>(input.data()), static_cast<int>(input.size()));
  if (size < 0) {
    InvalidSignature();
  }
  output.resize(static_cast<std::size_t>(size));
  while (!output.empty() && output.back() == '=') {
    output.pop_back();
  }
  std::ranges::replace(output, '+', '-');
  std::ranges::replace(output, '/', '_');
  return output;
}

std::string DecodeBase64Url(std::string_view encoded) {
  if (encoded.empty() || encoded.size() % kBase64BlockSize == 1) {
    InvalidSignature();
  }
  for (const char character : encoded) {
    const auto byte = static_cast<unsigned char>(character);
    if ((byte < 'a' || byte > 'z') && (byte < 'A' || byte > 'Z') && (byte < '0' || byte > '9') && byte != '-' &&
        byte != '_') {
      InvalidSignature();
    }
  }
  std::string padded(encoded);
  std::ranges::replace(padded, '-', '+');
  std::ranges::replace(padded, '_', '/');
  const auto padding = (kBase64BlockSize - padded.size() % kBase64BlockSize) % kBase64BlockSize;
  padded.append(padding, '=');
  std::string decoded(padded.size(), '\0');
  const int size =
      EVP_DecodeBlock(reinterpret_cast<unsigned char*>(decoded.data()),
                      reinterpret_cast<const unsigned char*>(padded.data()), static_cast<int>(padded.size()));
  if (size < 0 || static_cast<std::size_t>(size) < padding) {
    InvalidSignature();
  }
  decoded.resize(static_cast<std::size_t>(size) - padding);
  if (Base64Url(decoded) != encoded) {
    InvalidSignature();
  }
  return decoded;
}

Key ReadKey(std::string_view pem, bool private_key) {
  if (pem.empty() || pem.size() > kMaximumKeySize) {
    InvalidSignature();
  }
  Bio bio(BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size())), BIO_free);
  if (bio == nullptr) {
    InvalidSignature();
  }
  // No interactive password prompt is permitted in SDK signing APIs.
  const auto no_password = [](char*, int, int, void*) { return 0; };
  Key key(private_key ? PEM_read_bio_PrivateKey(bio.get(), nullptr, no_password, nullptr)
                      : PEM_read_bio_PUBKEY(bio.get(), nullptr, nullptr, nullptr),
          EVP_PKEY_free);
  if (key == nullptr || EVP_PKEY_is_a(key.get(), kKeyType) != 1) {
    InvalidSignature();
  }
  return key;
}

std::string SigningInput(std::string_view protected_header, const nlohmann::json& payload) {
  const auto encoded = Base64Url(internal::Canonicalize(payload));
  std::string input;
  input.reserve(protected_header.size() + encoded.size() + 1);
  input.append(protected_header);
  input.push_back('.');
  input.append(encoded);
  return input;
}

void VerifySignature(const nlohmann::json& signature, const nlohmann::json& payload,
                     const AgentCardKeyResolver& resolver, std::chrono::system_clock::time_point now) {
  const auto protected_header = signature.at(kProtected).get<std::string>();
  const auto header = internal::ParseSigningJson(DecodeBase64Url(protected_header));
  if (!header.is_object() || header.size() != kRequiredHeaderSize || header.at(kAlg) != kAlgorithm) {
    InvalidSignature();
  }
  const auto kid = header.at(kKid).get<std::string>();
  if (kid.empty()) {
    InvalidSignature();
  }
  auto resolved = resolver(kid);
  if (!resolved.ok() || resolved.value().revoked || now < resolved.value().valid_from ||
      now >= resolved.value().valid_until) {
    InvalidSignature();
  }
  if (signature.contains(kUnprotectedHeader)) {
    const auto& unprotected = signature.at(kUnprotectedHeader);
    if (!unprotected.is_object() || !unprotected.empty()) {
      InvalidSignature();
    }
  }
  auto key = ReadKey(resolved.value().public_key_pem, false);
  const auto bytes = DecodeBase64Url(signature.at(kSignature).get<std::string>());
  if (bytes.size() != kEd25519SignatureSize) {
    InvalidSignature();
  }
  const auto input = SigningInput(protected_header, payload);
  Digest digest(EVP_MD_CTX_new(), EVP_MD_CTX_free);
  if (digest == nullptr || EVP_DigestVerifyInit(digest.get(), nullptr, nullptr, nullptr, key.get()) != 1 ||
      EVP_DigestVerify(digest.get(), reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size(),
                       reinterpret_cast<const unsigned char*>(input.data()), input.size()) != 1) {
    InvalidSignature();
  }
}
}  // namespace

Result<lf::a2a::v1::AgentCard> SignAgentCard(const lf::a2a::v1::AgentCard& card, std::string_view key_id,
                                             std::string_view private_key_pem) {
  try {
    if (key_id.empty() || key_id.size() > kMaximumKeyIdSize) {
      InvalidSignature();
    }
    auto unsigned_card = card;
    unsigned_card.clear_signatures();
    auto json = MessageToJson(unsigned_card);
    if (!json.ok()) {
      return json.error();
    }
    const auto payload = internal::ParseSigningJson(json.value());
    const nlohmann::json header{{kAlg, kAlgorithm}, {kKid, key_id}};
    const auto protected_header = Base64Url(internal::Canonicalize(header));
    const auto input = SigningInput(protected_header, payload);
    auto key = ReadKey(private_key_pem, true);
    Digest digest(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    std::string signature(kEd25519SignatureSize, '\0');
    std::size_t size = signature.size();
    if (digest == nullptr || EVP_DigestSignInit(digest.get(), nullptr, nullptr, nullptr, key.get()) != 1 ||
        EVP_DigestSign(digest.get(), reinterpret_cast<unsigned char*>(signature.data()), &size,
                       reinterpret_cast<const unsigned char*>(input.data()), input.size()) != 1 ||
        size != signature.size()) {
      InvalidSignature();
    }
    auto signed_card = card;
    auto* added = signed_card.add_signatures();
    added->set_protected_(protected_header);
    added->set_signature(Base64Url(signature));
    return signed_card;
  } catch (const std::exception&) {
    return Error::Validation(std::string(kInvalidSignature));
  }
}

Result<void> VerifyAgentCardJson(std::string_view json, const AgentCardKeyResolver& resolver,
                                 std::chrono::system_clock::time_point now) {
  try {
    if (!resolver) {
      InvalidSignature();
    }
    auto payload = internal::ParseSigningJson(json);
    auto signatures = payload.at(kSignatures);
    if (!signatures.is_array() || signatures.empty()) {
      InvalidSignature();
    }
    payload.erase(std::string(kSignatures));
    for (const auto& signature : signatures) {
      VerifySignature(signature, payload, resolver, now);
    }
    return {};
  } catch (const std::exception&) {
    return Error::Validation(std::string(kInvalidSignature));
  }
}
}  // namespace a2a::core
