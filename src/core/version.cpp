// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "a2a/core/version.h"

#include <algorithm>

namespace a2a::core {
namespace {

bool IsNumericComponent(std::string_view component) noexcept {
  return !component.empty() && (component.size() == 1 || component.front() != '0') &&
         std::ranges::all_of(component, [](char ch) { return ch >= '0' && ch <= '9'; });
}

}  // namespace

std::string Version::HeaderValue() { return std::string(kProtocolVersion); }

std::optional<std::string_view> Version::MajorMinor(std::string_view version) noexcept {
  if (version.empty()) {
    return kEmptyVersionFallback;
  }
  const auto minor_start = version.find('.');
  if (minor_start == std::string_view::npos || !IsNumericComponent(version.substr(0, minor_start))) {
    return std::nullopt;
  }
  const auto patch_start = version.find('.', minor_start + 1);
  const auto major_minor = version.substr(0, patch_start);
  if (!IsNumericComponent(major_minor.substr(minor_start + 1))) {
    return std::nullopt;
  }
  if (patch_start != std::string_view::npos && !IsNumericComponent(version.substr(patch_start + 1))) {
    return std::nullopt;
  }
  return major_minor;
}

bool Version::IsSupported(std::string_view version) noexcept { return MajorMinor(version) == kProtocolVersion; }

}  // namespace a2a::core
