#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace a2a::core {

enum class ErrorCode : std::uint8_t {
  kValidation,
  kUnsupportedVersion,
  kNetwork,
  kRemoteProtocol,
  kSerialization,
  kInternal,
};

class Error final {
 public:
  Error(ErrorCode code, std::string message);
  Error(const Error&) = default;
  Error(Error&&) noexcept = default;
  Error& operator=(const Error&) = default;
  Error& operator=(Error&&) noexcept = default;
  ~Error() = default;

  [[nodiscard]] static Error Validation(std::string message);
  [[nodiscard]] static Error UnsupportedVersion(std::string message);
  [[nodiscard]] static Error Network(std::string message);
  [[nodiscard]] static Error RemoteProtocol(std::string message);
  [[nodiscard]] static Error Serialization(std::string message);

  [[nodiscard]] Error WithTransport(std::string transport) const;
  [[nodiscard]] Error WithProtocolCode(std::string protocol_code) const;
  [[nodiscard]] Error WithHttpStatus(int http_status) const;

  [[nodiscard]] ErrorCode code() const noexcept;
  [[nodiscard]] std::string_view message() const noexcept;
  [[nodiscard]] const std::optional<std::string>& transport() const noexcept;
  [[nodiscard]] const std::optional<std::string>& protocol_code() const noexcept;
  [[nodiscard]] const std::optional<int>& http_status() const noexcept;

 private:
  ErrorCode code_ = ErrorCode::kInternal;
  std::string message_;
  std::optional<std::string> transport_ = std::nullopt;
  std::optional<std::string> protocol_code_ = std::nullopt;
  std::optional<int> http_status_ = std::nullopt;
};

}  // namespace a2a::core
