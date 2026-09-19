#pragma once
#include <chrono>
#include <memory>
#include <string>
#include <string_view>

#include "a2a/core/result.h"

namespace support_tutorial {
class TextModel {
 public:
  virtual ~TextModel() = default;
  [[nodiscard]] virtual a2a::core::Result<std::string> Generate(std::string_view prompt) const = 0;
};
struct ModelConfig final {
  std::string provider;
  std::string base_url;
  std::string model;
  std::string api_key;
  std::chrono::milliseconds timeout{30000};
};
[[nodiscard]] a2a::core::Result<ModelConfig> LoadModelConfig(std::string_view role_prefix);
[[nodiscard]] a2a::core::Result<std::unique_ptr<TextModel>> CreateModel(const ModelConfig& config);
}  // namespace support_tutorial
