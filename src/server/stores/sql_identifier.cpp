// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "a2a/server/stores/sql_identifier.h"

#include <algorithm>
#include <cctype>
#include <ranges>
#include <string>

namespace a2a::server::stores {

bool IsAlphaOrUnderscore(unsigned char ch) noexcept { return std::isalpha(ch) != 0 || ch == '_'; }

bool IsAlnumOrUnderscore(unsigned char ch) noexcept { return std::isalnum(ch) != 0 || ch == '_'; }

bool IsValidSqlIdentifier(std::string_view identifier) {
  if (identifier.empty()) {
    return false;
  }
  if (!IsAlphaOrUnderscore(static_cast<unsigned char>(identifier.front()))) {
    return false;
  }
  return std::ranges::all_of(identifier.substr(1),
                             [](char ch) { return IsAlnumOrUnderscore(static_cast<unsigned char>(ch)); });
}

std::string QuoteSqlIdentifier(std::string_view identifier) {
  std::string quoted;
  quoted.reserve(identifier.size() + 2);
  quoted.push_back('"');
  for (const char ch : identifier) {
    if (ch == '"') {
      quoted.push_back('"');
    }
    quoted.push_back(ch);
  }
  quoted.push_back('"');
  return quoted;
}

std::string QualifiedSqlIdentifier(std::string_view schema, std::string_view identifier) {
  std::string qualified = QuoteSqlIdentifier(schema);
  qualified.reserve(qualified.size() + 1 + identifier.size() + 2);
  qualified.push_back('.');
  qualified.append(QuoteSqlIdentifier(identifier));
  return qualified;
}

}  // namespace a2a::server::stores
