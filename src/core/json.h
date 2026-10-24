// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

#include <google/protobuf/struct.pb.h>
#include <simdjson.h>

#include <cstddef>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "a2a/core/non_copyable.h"
#include "a2a/core/result.h"

namespace a2a::core::internal {

inline constexpr std::size_t kMaximumJsonBytes = 64U * 1024U * 1024U;
inline constexpr std::size_t kMaximumJsonDepth = 128U;
inline constexpr std::size_t kMaximumCachedJsonBytes = 1024U * 1024U;

// A document leases a parser from this thread's single-slot cache. Reentrant
// parses acquire another parser; no document shares mutable storage. parse()
// copies/pads the input, and all DOM views expire with this object. Copy owning
// results before calling application callbacks or destroying the document.
class JsonDocument final : private NonCopyableOrMovable {
 public:
  JsonDocument();
  ~JsonDocument();
  [[nodiscard]] Result<void> Parse(std::string_view input);
  [[nodiscard]] Result<void> ParseObject(std::string_view input);
  [[nodiscard]] simdjson::dom::element root() const { return root_; }
  [[nodiscard]] std::size_t capacity() const { return parser_->capacity(); }

 private:
  std::unique_ptr<simdjson::dom::parser> parser_;
  simdjson::dom::element root_;
};

template <typename T>
T JsonRead(simdjson::simdjson_result<T> result) {
  T value;
  const auto error = std::move(result).get(value);
  if (error != simdjson::SUCCESS) {
    throw std::invalid_argument(simdjson::error_message(error));
  }
  return value;
}

// Use only after JsonDocument has validated the complete input. Raw views keep
// original number tokens intact instead of round-tripping through binary64.
class JsonCursor final : private NonCopyableOrMovable {
 public:
  explicit JsonCursor(std::string_view input);
  ~JsonCursor();
  [[nodiscard]] simdjson::ondemand::document& root() { return root_; }
  [[nodiscard]] std::size_t capacity() const { return parser_->capacity(); }

 private:
  simdjson::padded_string input_;
  std::unique_ptr<simdjson::ondemand::parser> parser_;
  simdjson::ondemand::document root_;
};

[[nodiscard]] std::string JsonMember(std::string_view input, std::string_view name);

[[nodiscard]] google::protobuf::Value JsonToValue(simdjson::dom::element value);

}  // namespace a2a::core::internal
