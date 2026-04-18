#include "a2a/core/error.h"

#include <utility>

namespace a2a::core {

Error::Error(ErrorCode code, std::string message)
    : code_(code), message_(std::move(message)) {}

Error Error::Validation(std::string message) {
  return Error(ErrorCode::kValidation, std::move(message));
}

Error Error::UnsupportedVersion(std::string message) {
  return Error(ErrorCode::kUnsupportedVersion, std::move(message));
}

Error Error::Network(std::string message) {
  return Error(ErrorCode::kNetwork, std::move(message));
}

Error Error::RemoteProtocol(std::string message) {
  return Error(ErrorCode::kRemoteProtocol, std::move(message));
}

Error Error::Serialization(std::string message) {
  return Error(ErrorCode::kSerialization, std::move(message));
}

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

const std::optional<std::string>& Error::transport() const noexcept {
  return transport_;
}

const std::optional<std::string>& Error::protocol_code() const noexcept {
  return protocol_code_;
}

const std::optional<int>& Error::http_status() const noexcept { return http_status_; }

}  // namespace a2a::core
