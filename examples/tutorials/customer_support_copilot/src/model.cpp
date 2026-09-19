#include "model.h"

#include <cstdlib>
#include <memory>
#include <string>

#include "a2a/core/protojson.h"
#include "a2a/http/http_client.h"
#include "google/protobuf/struct.pb.h"

namespace support_tutorial {
namespace {
constexpr std::string_view kDeterministic = "deterministic";
constexpr std::string_view kOpenAi = "openai_compatible";
constexpr std::string_view kGemini = "gemini";
constexpr std::string_view kProvider = "A2A_TUTORIAL_MODEL_PROVIDER";
constexpr std::string_view kBaseUrl = "A2A_TUTORIAL_MODEL_BASE_URL";
constexpr std::string_view kName = "A2A_TUTORIAL_MODEL_NAME";
constexpr std::string_view kKey = "A2A_TUTORIAL_MODEL_API_KEY";
constexpr std::string_view kTimeout = "A2A_TUTORIAL_MODEL_TIMEOUT_MS";
constexpr std::string_view kGeminiApiKey = "GEMINI_API_KEY";
constexpr std::string_view kGeminiBaseUrl = "https://generativelanguage.googleapis.com/v1beta/openai/";
constexpr std::string_view kGeminiModel = "gemini-3.8-flash";
constexpr int kHttpSuccessMinimum = 200;
constexpr int kHttpSuccessMaximum = 300;
std::string Env(std::string_view role, std::string_view suffix, std::string_view fallback = {}) {
  std::string name("A2A_TUTORIAL_");
  name.append(role);
  name.push_back('_');
  name.append(suffix);
  if (const char* value = std::getenv(name.c_str()); value != nullptr) {
    return value;
  }
  if (const char* value = std::getenv(std::string(fallback).c_str()); value != nullptr) {
    return value;
  }
  return {};
}
class Deterministic final : public TextModel {
 public:
  [[nodiscard]] a2a::core::Result<std::string> Generate(std::string_view /*prompt*/) const override {
    return std::string{};
  }
};
class OpenAi final : public TextModel {
 public:
  explicit OpenAi(ModelConfig config) : config_(std::move(config)) {}
  a2a::core::Result<std::string> Generate(std::string_view prompt) const override {
    google::protobuf::Struct root;
    (*root.mutable_fields())["model"].set_string_value(config_.model);
    auto* messages = (*root.mutable_fields())["messages"].mutable_list_value();
    auto* message = messages->add_values()->mutable_struct_value();
    (*message->mutable_fields())["role"].set_string_value("user");
    (*message->mutable_fields())["content"].set_string_value(std::string(prompt));
    auto body = a2a::core::MessageToJson(root);
    if (!body.ok()) {
      return body.error();
    }
    std::string url = config_.base_url;
    if (!url.empty() && url.back() == '/') {
      url.pop_back();
    }
    url.append("/chat/completions");
    a2a::http::Request request{.method = "POST",
                               .url = std::move(url),
                               .headers = {{"Content-Type", "application/json"}},
                               .body = std::move(body.value()),
                               .timeout = config_.timeout};
    if (!config_.api_key.empty()) {
      request.headers.push_back({"Authorization", std::string("Bearer ").append(config_.api_key)});
    }
    auto response = client_.SendRequest(request);
    if (!response.ok()) {
      return response.error();
    }
    if (response.value().status_code < kHttpSuccessMinimum || response.value().status_code >= kHttpSuccessMaximum) {
      return a2a::core::Error::Internal("model endpoint returned HTTP " + std::to_string(response.value().status_code));
    }
    google::protobuf::Struct parsed;
    auto status = a2a::core::JsonToMessage(response.value().body, &parsed);
    if (!status.ok()) {
      return a2a::core::Error::Validation("model endpoint returned malformed JSON");
    }
    const auto choices = parsed.fields().find("choices");
    if (choices == parsed.fields().end() || choices->second.list_value().values().empty()) {
      return a2a::core::Error::Validation("model response has no choices");
    }
    const auto& fields = choices->second.list_value().values(0).struct_value().fields();
    const auto found_message = fields.find("message");
    if (found_message == fields.end()) {
      return a2a::core::Error::Validation("model response has no message");
    }
    const auto found_content = found_message->second.struct_value().fields().find("content");
    if (found_content == found_message->second.struct_value().fields().end()) {
      return a2a::core::Error::Validation("model response has no content");
    }
    return found_content->second.string_value();
  }

 private:
  ModelConfig config_;
  mutable a2a::http::Client client_;
};
}  // namespace
a2a::core::Result<ModelConfig> LoadModelConfig(std::string_view role) {
  ModelConfig config;
  config.provider = Env(role, "MODEL_PROVIDER", kProvider);
  if (config.provider.empty()) {
    config.provider = kDeterministic;
  }
  config.base_url = Env(role, "MODEL_BASE_URL", kBaseUrl);
  config.model = Env(role, "MODEL_NAME", kName);
  config.api_key = Env(role, "MODEL_API_KEY", kKey);
  const auto timeout = Env(role, "MODEL_TIMEOUT_MS", kTimeout);
  if (!timeout.empty()) {
    try {
      config.timeout = std::chrono::milliseconds(std::stoll(timeout));
    } catch (...) {
      return a2a::core::Error::Validation("model timeout must be a positive integer");
    }
  }
  if (config.timeout.count() <= 0) {
    return a2a::core::Error::Validation("model timeout must be positive");
  }
  if (config.provider == kGemini) {
    if (config.base_url.empty()) {
      config.base_url = kGeminiBaseUrl;
    }
    if (config.model.empty()) {
      config.model = kGeminiModel;
    }
    if (config.api_key.empty()) {
      if (const char* value = std::getenv(kGeminiApiKey.data()); value != nullptr) {
        config.api_key = value;
      }
    }
  }
  if (config.provider != kDeterministic && config.provider != kOpenAi && config.provider != kGemini) {
    return a2a::core::Error::Validation("unsupported model provider: " + config.provider);
  }
  if (config.provider == kOpenAi && (config.base_url.empty() || config.model.empty())) {
    return a2a::core::Error::Validation("openai_compatible requires model base URL and model name");
  }
  if (config.provider == kGemini && config.api_key.empty()) {
    return a2a::core::Error::Validation("gemini requires GEMINI_API_KEY or a model API key");
  }
  return config;
}
a2a::core::Result<std::unique_ptr<TextModel>> CreateModel(const ModelConfig& config) {
  if (config.provider == kDeterministic) {
    return std::unique_ptr<TextModel>(std::make_unique<Deterministic>());
  }
  return std::unique_ptr<TextModel>(std::make_unique<OpenAi>(config));
}
}  // namespace support_tutorial
