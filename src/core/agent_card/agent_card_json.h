// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

#include <simdjson.h>

#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "a2a/core/non_copyable.h"

namespace a2a::core::internal {
inline constexpr std::string_view kInvalidSigningJson = "invalid signing JSON";

template <typename T>
T ReadJson(simdjson::simdjson_result<T> result) {
  T value;
  if (std::move(result).get(value) != simdjson::SUCCESS) {
    throw std::invalid_argument(std::string(kInvalidSigningJson));
  }
  return value;
}

// Owns padded input, parser and JCS output. Consumers follow simdjson's
// forward-only cursor rules. Moving is forbidden while the document borrows storage.
class SigningJson final : private NonCopyableOrMovable {
 public:
  explicit SigningJson(std::string_view input, std::string_view excluded_root_member = {});
  [[nodiscard]] simdjson::ondemand::document& root() { return root_; }
  [[nodiscard]] const std::string& canonical() const { return canonical_; }

 private:
  simdjson::padded_string input_;
  simdjson::ondemand::parser parser_;
  simdjson::ondemand::document root_;
  std::string canonical_;
};
}  // namespace a2a::core::internal
