// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "a2a/core/agent_card/agent_card_signing.h"

#include <openssl/evp.h>
#include <openssl/pem.h>

#include <algorithm>
#include <array>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

#include "a2a/core/protojson.h"
#include "agent_card_json.h"

namespace a2a::core {
namespace {
constexpr std::string_view kAlgorithm = "EdDSA";
constexpr char kKeyType[] = "ED25519";
constexpr std::string_view kSignatures = "signatures";
constexpr std::string_view kProtected = "protected";
constexpr std::string_view kSignature = "signature";
constexpr std::string_view kUnprotectedHeader = "header";
constexpr std::string_view kAlg = "alg";
constexpr std::string_view kKid = "kid";
constexpr std::string_view kTyp = "typ";
constexpr std::string_view kJose = "JOSE";
constexpr std::string_view kJku = "jku";
constexpr std::string_view kCritical = "crit";
constexpr std::string_view kEncodedPayload = "b64";
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

std::span<const unsigned char> Bytes(std::string_view text) {
  // C++ permits unsigned char to inspect any object's representation. This view
  // borrows the original text without changing its alignment, lifetime or size.
  return {reinterpret_cast<const unsigned char*>(text.data()), text.size()};
}

std::string Base64Url(std::span<const unsigned char> input) {
  // EVP_EncodeBlock returns an int and also writes a terminating NUL.
  constexpr auto kMaximumInputSize =
      (static_cast<std::size_t>(std::numeric_limits<int>::max()) - 1) / kBase64BlockSize * kBase64InputBlockSize;
  if (input.size() > kMaximumInputSize) {
    InvalidSignature();
  }
  const auto encoded_size = kBase64BlockSize * ((input.size() + kBase64InputBlockSize - 1) / kBase64InputBlockSize);
  std::string output(encoded_size + 1, '\0');
  // unsigned char may also write char storage; capacity includes OpenSSL's NUL.
  const int size =
      EVP_EncodeBlock(reinterpret_cast<unsigned char*>(output.data()), input.data(), static_cast<int>(input.size()));
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

std::string Base64Url(std::string_view input) { return Base64Url(Bytes(input)); }

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
  if (padded.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    InvalidSignature();
  }
  std::string decoded(padded.size(), '\0');
  // Byte access to char storage is permitted; decoding writes at most 3/4 of
  // padded.size() bytes, including bytes later removed for '=' padding.
  const int size = EVP_DecodeBlock(reinterpret_cast<unsigned char*>(decoded.data()), Bytes(padded).data(),
                                   static_cast<int>(padded.size()));
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

using internal::ReadJson;

// Decoded strings borrow the parent signing document until verification finishes.
struct JwsSignature {
  std::string_view protected_header;
  std::string_view encoded_signature;
  std::vector<std::string_view> unprotected_names;
};

JwsSignature ReadSignature(simdjson::ondemand::value value) {
  JwsSignature signature;
  for (auto field_result : ReadJson(value.get_object())) {
    const auto name = ReadJson(field_result.unescaped_key());
    auto field = ReadJson(std::move(field_result));
    if (name == kProtected) {
      signature.protected_header = ReadJson(field.value().get_string());
    } else if (name == kSignature) {
      signature.encoded_signature = ReadJson(field.value().get_string());
    } else if (name == kUnprotectedHeader) {
      for (auto annotation_result : ReadJson(field.value().get_object())) {
        const auto key = ReadJson(annotation_result.unescaped_key());
        if (key == kAlg || key == kKid || key == kCritical || key == kEncodedPayload) {
          InvalidSignature();
        }
        signature.unprotected_names.emplace_back(key);
      }
    }
  }
  std::ranges::sort(signature.unprotected_names);
  return signature;
}

std::string_view ReadKeyId(internal::SigningJson& protected_header, const JwsSignature& signature) {
  std::string_view algorithm;
  std::string_view kid;
  for (auto field_result : ReadJson(protected_header.root().get_object())) {
    const auto name = ReadJson(field_result.unescaped_key());
    auto field = ReadJson(std::move(field_result));
    if (std::ranges::binary_search(signature.unprotected_names, name) || name == kCritical) {
      InvalidSignature();
    }
    if (name == kAlg) {
      algorithm = ReadJson(field.value().get_string());
    } else if (name == kKid) {
      kid = ReadJson(field.value().get_string());
    } else if (name == kEncodedPayload) {
      if (!ReadJson(field.value().get_bool())) {
        InvalidSignature();
      }
    } else if (name == kTyp || name == kJku) {
      (void)ReadJson(field.value().get_string());
    }
  }
  if (algorithm != kAlgorithm || kid.empty() || kid.size() > kMaximumKeyIdSize) {
    InvalidSignature();
  }
  return kid;
}

std::vector<unsigned char> SigningInput(std::string_view protected_header, std::string_view encoded_payload) {
  std::vector<unsigned char> input;
  input.reserve(protected_header.size() + encoded_payload.size() + 1);
  input.insert(input.end(), protected_header.begin(), protected_header.end());
  input.push_back('.');
  input.insert(input.end(), encoded_payload.begin(), encoded_payload.end());
  return input;
}

void VerifySignature(simdjson::ondemand::value signature_value, std::string_view encoded_payload,
                     const AgentCardKeyResolver& resolver, std::chrono::system_clock::time_point now) {
  const auto signature = ReadSignature(signature_value);
  internal::SigningJson parsed_header(DecodeBase64Url(signature.protected_header));
  const auto kid = ReadKeyId(parsed_header, signature);
  const auto bytes = DecodeBase64Url(signature.encoded_signature);
  if (bytes.size() != kEd25519SignatureSize) {
    InvalidSignature();
  }
  auto resolved = resolver(kid);
  if (!resolved.ok() || resolved.value().revoked || now < resolved.value().valid_from ||
      now >= resolved.value().valid_until) {
    InvalidSignature();
  }
  auto key = ReadKey(resolved.value().public_key_pem, false);
  const auto input = SigningInput(signature.protected_header, encoded_payload);
  Digest digest(EVP_MD_CTX_new(), EVP_MD_CTX_free);
  if (digest == nullptr || EVP_DigestVerifyInit(digest.get(), nullptr, nullptr, nullptr, key.get()) != 1 ||
      EVP_DigestVerify(digest.get(), Bytes(bytes).data(), bytes.size(), input.data(), input.size()) != 1) {
    InvalidSignature();
  }
}
}  // namespace

Result<lf::a2a::v1::AgentCard> SignAgentCard(const lf::a2a::v1::AgentCard& card, std::string_view key_id,
                                             std::string_view private_key_pem) {
  try {
    if (key_id.empty() || key_id.size() > kMaximumKeyIdSize ||
        static_cast<std::size_t>(card.signatures_size()) >= kMaxAgentCardSignatures) {
      InvalidSignature();
    }
    auto unsigned_card = card;
    unsigned_card.clear_signatures();
    auto json = MessageToJson(unsigned_card);
    if (!json.ok()) {
      return json.error();
    }
    const internal::SigningJson payload(json.value());
    google::protobuf::Value header;
    auto* fields = header.mutable_struct_value()->mutable_fields();
    (*fields)[std::string(kAlg)].set_string_value(std::string(kAlgorithm));
    (*fields)[std::string(kKid)].set_string_value(std::string(key_id));
    (*fields)[std::string(kTyp)].set_string_value(std::string(kJose));
    auto header_json = MessageToJson(header);
    if (!header_json.ok()) {
      return header_json.error();
    }
    const auto protected_header = Base64Url(internal::SigningJson(header_json.value()).canonical());
    const auto input = SigningInput(protected_header, Base64Url(payload.canonical()));
    auto key = ReadKey(private_key_pem, true);
    Digest digest(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    std::array<unsigned char, kEd25519SignatureSize> signature{};
    std::size_t size = signature.size();
    if (digest == nullptr || EVP_DigestSignInit(digest.get(), nullptr, nullptr, nullptr, key.get()) != 1 ||
        EVP_DigestSign(digest.get(), signature.data(), &size, input.data(), input.size()) != 1 ||
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
    internal::SigningJson payload(json, kSignatures);
    const auto encoded_payload = Base64Url(payload.canonical());
    for (auto field_result : ReadJson(payload.root().get_object())) {
      if (ReadJson(field_result.unescaped_key()) != kSignatures) {
        continue;
      }
      auto field = ReadJson(std::move(field_result));
      auto signatures = ReadJson(field.value().get_array());
      const auto count = ReadJson(signatures.count_elements());
      if (count == 0 || count > kMaxAgentCardSignatures) {
        InvalidSignature();
      }
      for (auto signature : signatures) {
        try {
          VerifySignature(ReadJson(signature), encoded_payload, resolver, now);
          return {};
        } catch (const std::exception&) {
          // Unknown, expired or invalid signatures do not prevent key rotation.
          continue;
        }
      }
    }
    InvalidSignature();
  } catch (const std::exception&) {
    return Error::Validation(std::string(kInvalidSignature));
  }
}
}  // namespace a2a::core
