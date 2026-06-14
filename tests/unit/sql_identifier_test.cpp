// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "a2a/server/stores/sql_identifier.h"

#include <gtest/gtest.h>

#include <array>
#include <string_view>

namespace {

constexpr unsigned char kAlpha = 'a';
constexpr unsigned char kUpperAlpha = 'Z';
constexpr unsigned char kDigit = '7';
constexpr unsigned char kUnderscore = '_';
constexpr unsigned char kDash = '-';
constexpr std::string_view kSimpleIdentifier = "schema_1";
constexpr std::string_view kStartsWithDigit = "1schema";
constexpr std::string_view kContainsDash = "bad-schema";
constexpr std::string_view kContainsDot = "bad.schema";
constexpr std::string_view kContainsSemicolon = "bad;schema";
constexpr std::string_view kNeedsQuoting = "tenant_schema";
constexpr std::string_view kContainsQuote = "tenant\"schema";
constexpr std::string_view kQuotedIdentifier = "\"tenant_schema\"";
constexpr std::string_view kEscapedQuotedIdentifier = R"("tenant""schema")";
constexpr std::string_view kTableName = "a2a_tasks";
constexpr std::string_view kQualifiedIdentifier = R"("tenant_schema"."a2a_tasks")";

TEST(SqlIdentifierTest, ClassifiesIdentifierCharacters) {
  EXPECT_TRUE(a2a::server::IsAlphaOrUnderscore(kAlpha));
  EXPECT_TRUE(a2a::server::IsAlphaOrUnderscore(kUpperAlpha));
  EXPECT_TRUE(a2a::server::IsAlphaOrUnderscore(kUnderscore));
  EXPECT_FALSE(a2a::server::IsAlphaOrUnderscore(kDigit));
  EXPECT_FALSE(a2a::server::IsAlphaOrUnderscore(kDash));

  EXPECT_TRUE(a2a::server::IsAlnumOrUnderscore(kAlpha));
  EXPECT_TRUE(a2a::server::IsAlnumOrUnderscore(kUpperAlpha));
  EXPECT_TRUE(a2a::server::IsAlnumOrUnderscore(kDigit));
  EXPECT_TRUE(a2a::server::IsAlnumOrUnderscore(kUnderscore));
  EXPECT_FALSE(a2a::server::IsAlnumOrUnderscore(kDash));
}

TEST(SqlIdentifierTest, ValidatesSimpleSqlIdentifiers) {
  constexpr std::array<std::string_view, 5> kInvalidIdentifiers = {std::string_view{}, kStartsWithDigit, kContainsDash,
                                                                   kContainsDot, kContainsSemicolon};

  EXPECT_TRUE(a2a::server::IsValidSqlIdentifier(kSimpleIdentifier));
  for (const std::string_view invalid_identifier : kInvalidIdentifiers) {
    EXPECT_FALSE(a2a::server::IsValidSqlIdentifier(invalid_identifier));
  }
}

TEST(SqlIdentifierTest, QuotesAndQualifiesSqlIdentifiers) {
  EXPECT_EQ(a2a::server::QuoteSqlIdentifier(kNeedsQuoting), kQuotedIdentifier);
  EXPECT_EQ(a2a::server::QuoteSqlIdentifier(kContainsQuote), kEscapedQuotedIdentifier);
  EXPECT_EQ(a2a::server::QualifiedSqlIdentifier(kNeedsQuoting, kTableName), kQualifiedIdentifier);
}

}  // namespace
