// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)
#include <boost/asio.hpp>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>

#include "a2a/server/engine/async_runtime.h"
#include "a2a/server/event_http_dispatcher.h"

namespace {
constexpr std::string_view kSmoke = "--smoke";
constexpr std::string_view kRequest =
    "GET /a2a/tasks/example HTTP/1.1\r\nA2A-Version: 1.0\r\nConnection: close\r\n\r\n";
constexpr std::string_view kTaskId = "example";
class Executor final : public a2a::server::engine::CoroutineAgentExecutor {
 public:
  boost::asio::awaitable<a2a::core::Result<lf::a2a::v1::Task>> GetTask(
      lf::a2a::v1::GetTaskRequest request, a2a::server::engine::RequestExecutionContext /*context*/) override {
    lf::a2a::v1::Task task;
    task.set_id(request.id());
    co_return task;
  }
};
bool Smoke(const a2a::server::EventHttpServer& server) {
  boost::asio::io_context io;
  boost::asio::ip::tcp::socket socket(io);
  socket.connect({boost::asio::ip::address_v4::loopback(), server.port()});
  boost::asio::write(socket, boost::asio::buffer(kRequest));
  std::string response;
  boost::system::error_code error;
  boost::asio::read(socket, boost::asio::dynamic_buffer(response), error);
  return error == boost::asio::error::eof && response.find(kTaskId) != std::string::npos;
}
}  // namespace

int Run(int argc, char** argv) {
  a2a::server::engine::AsyncRuntime application(std::make_shared<Executor>());
  a2a::server::EventHttpServer listener(a2a::server::MakeEventHttpHandler(application.dispatcher(), {}));
  const auto started = listener.Start();
  if (!started.ok()) {
    std::cerr << started.error().message() << '\n';
    return EXIT_FAILURE;
  }
  bool success = true;
  if (argc > 1 && argv[1] == kSmoke) {
    success = Smoke(listener);
  } else {
    std::cout << "Listening on 127.0.0.1:" << listener.port() << "; press Enter to stop\n";
    std::cin.get();
  }
  listener.Shutdown();
  listener.Join();
  application.Shutdown();
  application.Join();
  return success ? EXIT_SUCCESS : EXIT_FAILURE;
}

int main(int argc, char** argv) noexcept {
  try {
    return Run(argc, argv);
  } catch (...) {
    return EXIT_FAILURE;
  }
}
