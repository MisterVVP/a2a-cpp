#include "a2a/core/error.h"

#include <utility>

namespace a2a::core {

Error::Error(ErrorCode code, std::string message) : code_(code), message_(std::move(message)) {}

Error::Error(const Error& other) = default;

Error::Error(Error&& other) noexcept
    : code_(other.code_),
      message_(std::move(other.message_)),
      transport_(std::move(other.transport_)),
      protocol_code_(std::move(other.protocol_code_)),
      http_status_(other.http_status_) {}

Error& Error::operator=(const Error& other) {
  if (this == &other) {
    return *this;
  }
  code_ = other.code_;
  message_ = other.message_;
  transport_ = other.transport_;
  protocol_code_ = other.protocol_code_;
  http_status_ = other.http_status_;
  return *this;
}

Error& Error::operator=(Error&& other) noexcept {
  if (this == &other) {
    return *this;
  }
  code_ = other.code_;
  message_ = std::move(other.message_);
  transport_ = std::move(other.transport_);
  protocol_code_ = std::move(other.protocol_code_);
  http_status_ = other.http_status_;
  return *this;
}

Error Error::Validation(std::string message) {
  return {ErrorCode::kValidation, std::move(message)};
}

Error Error::UnsupportedVersion(std::string message) {
  return {ErrorCode::kUnsupportedVersion, std::move(message)};
}

Error Error::Network(std::string message) { return {ErrorCode::kNetwork, std::move(message)}; }

Error Error::RemoteProtocol(std::string message) {
  return {ErrorCode::kRemoteProtocol, std::move(message)};
}

Error Error::Serialization(std::string message) {
  return {ErrorCode::kSerialization, std::move(message)};
}

Error Error::Internal(std::string message) { return {ErrorCode::kInternal, std::move(message)}; }

Error Error::WithTransport(std::string transport) const {
  Error updated = *this;
  updated.transport_ = std::move(transport);
  return updated;
}

Error Error::WithProtocolCode(std::string protocol_code) const {
  Error updated = *this;
  updated.protocol_code_ = std::move(protocol_code);
  return updated;
}

Error Error::WithHttpStatus(int http_status) const {
  Error updated = *this;
  updated.http_status_ = http_status;
  return updated;
}

ErrorCode Error::code() const noexcept { return code_; }

std::string_view Error::message() const noexcept { return message_; }

const std::optional<std::string>& Error::transport() const noexcept { return transport_; }

const std::optional<std::string>& Error::protocol_code() const noexcept { return protocol_code_; }

const std::optional<int>& Error::http_status() const noexcept { return http_status_; }

}  // namespace a2a::core
