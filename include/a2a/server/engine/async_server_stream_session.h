// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

#include <boost/asio/awaitable.hpp>
#include <cstddef>
#include <vector>

#include "a2a/core/result.h"
#include "a2a/v1/a2a.pb.h"

namespace a2a::server::engine {
// A demand-driven, owned event sink. Await each Write before submitting the next
// event. Success acknowledges transport consumption, not peer application receipt.
// Cancellation wakes a suspended write. Returning from the executor ends the
// stream; the dispatcher arbitrates that result against request cancellation.
class AsyncServerStreamSession {
 public:
  static constexpr std::size_t kMaxBatchEvents = 8;
  virtual ~AsyncServerStreamSession() = default;
  virtual boost::asio::awaitable<core::Result<void>> Write(lf::a2a::v1::StreamResponse event) = 0;
  // Submit already available events in order. Await transport consumption of the whole batch
  // before writing again. Failure can follow delivery of a prefix. An empty batch is a no-op.
  virtual boost::asio::awaitable<core::Result<void>> WriteBatch(std::vector<lf::a2a::v1::StreamResponse> events);

 protected:
  static constexpr auto kBatchTooLarge = "Stream batch exceeds the event limit";
};
}  // namespace a2a::server::engine
