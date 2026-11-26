// SPDX-License-Identifier: Apache-2.0

#include "a2a/server/engine/async_server_stream_session.h"

#include <utility>

namespace a2a::server::engine {
boost::asio::awaitable<core::Result<void>> AsyncServerStreamSession::WriteBatch(
    std::vector<lf::a2a::v1::StreamResponse> events) {
  if (events.size() > kMaxBatchEvents) {
    co_return core::Error::Validation(kBatchTooLarge);
  }
  for (auto& event : events) {
    auto result = co_await Write(std::move(event));
    if (!result.ok()) {
      co_return result;
    }
  }
  co_return core::Result<void>{};
}
}  // namespace a2a::server::engine
