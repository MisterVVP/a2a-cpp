// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "a2a/core/error.h"
#include "a2a/core/http_constants.h"
#include "a2a/core/result.h"

namespace a2a::http {

struct Header final {
  std::string name;
  std::string value;
};

struct Request final {
  std::string method;
  std::string url;
  std::vector<Header> headers;
  std::string body;
  std::chrono::milliseconds timeout{0};
  std::string http_version = std::string(core