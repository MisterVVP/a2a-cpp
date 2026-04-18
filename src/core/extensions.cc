#include "a2a/core/extensions.h"

#include <algorithm>
#include <cctype>

#include "a2a/core/error.h"

namespace a2a::core {
namespace {

std::string_view Trim(std::string_view value) {
  std::size_t start = 0;
  std::size_t end = value.size();
  while (start < end && std::isspace(static_cast<unsigned char>(value[start])) != 0) {
    ++start;
  }
  while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
    --end;
  }
  return value.substr(start, end - start);
}

bool IsValidToken(std::string_view token) {
  if (token.empty()) {
    return false;
  }
  return std::all_of(token.begin(), token.end(), [](char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '-' || c == '_' ||
           c == '.';
  });
}

void Normalize(std::vector<std::string>* extensions) {
  std::sort(extensions->begin(), extensions->end());
  extensions->erase(std::unique(extensions->begin(), extensions->end()), extensions->end());
}

}  // namespace

std::string Extensions::Format(const std::vector<std::string>& extensions) {
  std::vector<std::string> normalized;
  normalized.reserve(extensions.size());
  for (const auto& extension : extensions) {
    if (!extension.empty()) {
      normalized.push_back(extension);
    }
  }

  Normalize(&normalized);

  std::string result;
  for (std::size_t i = 0; i < normalized.size(); ++i) {
    if (i > 0) {
      result += ',';
    }
    result += normalized[i];
  }
  return result;
}

Result<std::vector<std::string>> Extensions::Parse(std::string_view header_value) {
  std::vector<std::string> parsed;
  std::size_t start = 0;

  while (start <= header_value.size()) {
    const std::size_t delimiter = header_value.find(',', start);
    const std::size_t end = delimiter == std::string_view::npos ? header_value.size() : delimiter;
    const std::string_view token = Trim(header_value.substr(start, end - start));

    if (!token.empty()) {
      if (!IsValidToken(token)) {
        return Error::Validation("A2A-Extensions header contains invalid token: " +
                                 std::string(token));
      }
      parsed.emplace_back(token);
    }

    if (delimiter == std::string_view::npos) {
      break;
    }
    start = delimiter + 1;
  }

  Normalize(&parsed);
  return parsed;
}

}  // namespace a2a::core
