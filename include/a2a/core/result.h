#pragma once

#include <cassert>
#include <optional>
#include <type_traits>
#include <utility>
#include <variant>

#include "error.h"

namespace a2a::core {

template <typename T>
class Result final {
 public:
  Result(const T& value) : value_(value) {}
  Result(T&& value) : value_(std::move(value)) {}
  Result(const Error& error) : value_(error) {}
  Result(Error&& error) : value_(std::move(error)) {}

  [[nodiscard]] bool ok() const noexcept { return std::holds_alternative<T>(value_); }

  [[nodiscard]] const T& value() const {
    assert(ok());
    return std::get<T>(value_);
  }

  [[nodiscard]] T& value() {
    assert(ok());
    return std::get<T>(value_);
  }

  [[nodiscard]] const Error& error() const {
    assert(!ok());
    return std::get<Error>(value_);
  }

 private:
  static_assert(!std::is_void_v<T>);
  std::variant<T, Error> value_;
};

template <>
class Result<void> final {
 public:
  Result() = default;
  Result(const Error& error) : error_(error) {}
  Result(Error&& error) : error_(std::move(error)) {}

  [[nodiscard]] bool ok() const noexcept { return !error_.has_value(); }

  [[nodiscard]] const Error& error() const {
    assert(error_.has_value());
    return *error_;
  }

 private:
  std::optional<Error> error_;
};

}  // namespace a2a::core
