// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <chrono>
#include <cstdlib>
#include <future>
#include <memory>
#include <utility>

#include "a2a/server/engine/asio_agent_executor.h"
#include "a2a/server/engine/async_dispatcher.h"

namespace {
using namespace a2a::server;
using namespace a2a::server::engine;
constexpr auto kTaskId = "coroutine-example-task";
constexpr auto kDelay = std::chrono::milliseconds(1);

class TimerExecutor final : public CoroutineAgentExecutor {
 public:
  boost::asio::awaitable<a2a::core::Result<lf::a2a::v1::Task>> GetTask(lf::a2a::v1::GetTaskRequest request,
                                                                       RequestExecutionContext /*context*/) override {
    boost::asio::steady_timer timer(co_await boost::asio::this_coro::executor);
    timer.expires_after(kDelay);
    co_await timer.async_wait(boost::asio::use_awaitable);
    lf::a2a::v1::Task task;
    task.set_id(request.id());
    co_return task;
  }
};
}  // namespace

int main() {
  boost::asio::io_context loop;
  auto guard = boost::asio::make_work_guard(loop);
  const auto application = std::make_shared<TimerExecutor>();
  const auto executor = std::make_shared<AsioAgentExecutor>(application, loop.get_executor());
  AsyncDispatcher dispatcher(executor);
  lf::a2a::v1::GetTaskRequest payload;
  payload.set_id(kTaskId);
  DispatchRequest request{.operation = DispatcherOperation::kGetTask, .payload = std::move(payload)};
  std::promise<bool> completion;
  auto result = completion.get_future();
  const auto accepted = dispatcher.Dispatch(std::move(request), {}, [&](auto response, auto /*context*/) {
    completion.set_value(response.ok() && std::get<lf::a2a::v1::Task>(response.value().payload()).id() == kTaskId);
    guard.reset();
  });
  if (!accepted.ok()) {
    return EXIT_FAILURE;
  }
  loop.run();
  dispatcher.Shutdown();
  return result.get() ? EXIT_SUCCESS : EXIT_FAILURE;
}
