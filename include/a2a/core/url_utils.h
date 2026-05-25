// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once
#include <algorithm>
#include <string>
#include <string_view>

namespace a2a::core {

constexpr std::string_view kSchemeDelimiter = "://";
constexpr std::string_view kNetworkPathPrefix = "//";
constexpr std::string_view kAuthoritySuffixDelimiters = "/?#";
constexpr char kPathSeparator = '/';
constexpr char kQuerySeparator = '?';
constexpr char kFragmentSeparator = '#';
constexpr std::string_view kDefaultPath = "/";

enum class UrlSuffixPolicy {
  kPathOnly,
  kPathAndQuery,
  kPathQueryAndFragment,
};

inline std::string ExtractTargetPath(std::string_view url, UrlSuffixPolicy suffix_policy = UrlSuffixPolicy::kPathOnly) {
  std::size_t path_start = 0;
  const std::size_t scheme_pos = url.find(kSchemeDelimiter);
  if (scheme_pos != std::string_view::npos) {
    const std::size_t authority_start = scheme_pos + kSchemeDelimiter.size();
    const std::size_t suffix_start = url.find_first_of(kAuthoritySuffixDelimiters, authority_start);
    path_start = suffix_start == std::string_view::npos ? url.size() : suffix_start;
  } else if (url.rfind(kNetworkPathPrefix, 0) == 0) {
    const std::size_t suffix_start = url.find_first_of(kAuthoritySuffixDelimiters, 2);
    path_start = suffix_start == std::string_view::npos ? url.size() : suffix_start;
  }

  std::string_view suffix = path_start >= url.size() ? std::string_view{} : url.substr(path_start);
  if (scheme_pos == std::string_view::npos && url.rfind(kNetworkPathPrefix, 0) != 0 && !url.empty() &&
      url.front() != kPathSeparator && url.front() != kQuerySeparator && url.front() != kFragmentSeparator) {
    suffix = url;
  }

  const std::size_t query_pos = suffix.find(kQuerySeparator);
  const std::size_t fragment_pos = suffix.find(kFragmentSeparator);
  const std::size_t path_end = std::min(query_pos, fragment_pos);
  std::string_view path = suffix.substr(0, path_end);

  std::string target;
  if (path.empty()) {
    target.assign(kDefaultPath);
  } else if (path.front() == kPathSeparator) {
    target.assign(path);
  } else {
    target.reserve(path.size() + 1);
    target.push_back(kPathSeparator);
    target.append(path);
  }

  if (suffix_policy == UrlSuffixPolicy::kPathOnly) {
    return target;
  }

  if (query_pos != std::string_view::npos) {
    const std::size_t query_end = fragment_pos == std::string_view::npos ? suffix.size() : fragment_pos;
    target.append(suffix.substr(query_pos, query_end - query_pos));
  }

  if (suffix_policy == UrlSuffixPolicy::kPathQueryAndFragment && fragment_pos != std::string_view::npos) {
    target.append(suffix.substr(fragment_pos));
  }

  return target;
}

}  // namespace a2a::core
