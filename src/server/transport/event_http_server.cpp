// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)
#include "a2a/server/event_http_server.h"

#include <array>
#include <atomic>
#include <boost/asio/associated_cancellation_slot.hpp>
#include <boost/asio/associated_executor.hpp>
#include <boost/asio/async_result.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/recycling_allocator.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/write.hpp>
#include <charconv>
#include <deque>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

#include "a2a/core/http_constants.h"

namespace a2a::server {
namespace {
namespace asio = boost::asio;
using Tcp = asio::ip::tcp;
using IoError = boost::system::error_code;
using WriteCompletion = std::function<void(core::Result<void>)>;
constexpr std::size_t kReadBytes = 8192;
constexpr std::size_t kHexBytes = sizeof(std::size_t) * 2U;
constexpr std::string_view kFinalChunk = "0\r\n\r\n";
constexpr std::string_view kHeartbeat = ": heartbeat\n\n";
constexpr std::string_view kStopped = "HTTP connection closed";
constexpr std::string_view kOutputLimit = "HTTP output exceeds configured bound";
constexpr std::string_view kThreadFailure = "HTTP worker startup failed";
constexpr std::string_view kAlreadyStarted = "HTTP server Start is single-use";
constexpr std::string_view kInvalidOptions = "HTTP server requires a handler, positive limits and idle timeout";
constexpr std::string_view kBlockingStream = "Legacy stream writers require the synchronous HTTP runtime";

std::string Chunk(std::string_view bytes) {
  std::array<char, kHexBytes> hex{};
  const auto converted = std::to_chars(hex.data(), hex.data() + hex.size(), bytes.size(), 16);
  std::string result;
  result.reserve(static_cast<std::size_t>(converted.ptr - hex.data()) + bytes.size() + 4U);
  result.append(hex.data(), converted.ptr);
  result.append(core::http::kLineTerminator);
  result.append(bytes);
  result.append(core::http::kLineTerminator);
  return result;
}

bool FitsOutput(const HttpServerResponse& response, std::size_t limit, bool streaming) {
  if (response.body.size() > limit) {
    return false;
  }
  // A stream prefix may be written separately from the response headers.
  std::size_t remaining = streaming ? limit : limit - response.body.size();
  for (const auto& [name, value] : response.headers) {
    if (name.size() > remaining) {
      return false;
    }
    remaining -= name.size();
    if (value.size() > remaining) {
      return false;
    }
    remaining -= value.size();
  }
  return true;
}

EventHttpServer::AsyncHandler LegacyHandler(EventHttpServer::Handler handler) {
  if (!handler) {
    return {};
  }
  return [handler = std::move(handler)](
             HttpServerRequest request,
             engine::RequestExecutionContext /*context*/) -> asio::awaitable<core::Result<EventHttpResponse>> {
    auto response = handler(request);
    if (!response.ok()) {
      co_return response.error();
    }
    co_return EventHttpResponse{.response = std::move(response.value()), .stream = {}};
  };
}

struct Runtime final {
  explicit Runtime(EventHttpServer::AsyncHandler callback, EventHttpServerOptions settings)
      : handler(std::move(callback)), options(std::move(settings)), acceptor(network) {}
  EventHttpServer::AsyncHandler handler;
  EventHttpServerOptions options;
  asio::io_context network;
  asio::io_context application;
  asio::executor_work_guard<asio::io_context::executor_type> application_work{application.get_executor()};
  asio::executor_work_guard<asio::io_context::executor_type> network_work{network.get_executor()};
  Tcp::acceptor acceptor;
  std::size_t pending = 0;
  std::atomic_bool stopping{false};
  bool finishing_shutdown = false;
};

void DrainRuntime(std::shared_ptr<Runtime> runtime) {
  if (runtime->finishing_shutdown) {
    return;
  }
  runtime->finishing_shutdown = true;
  auto& state = *runtime;
  state.network_work.reset();
  asio::post(state.application, [runtime = std::move(runtime)] {
    // Handler destruction may release blocking application objects.
    runtime->handler = {};
    runtime->application_work.reset();
  });
}

class Connection;
struct StreamGate final {};
class ConnectionStream final : public HttpEventStream {
 public:
  ConnectionStream(std::shared_ptr<Connection> connection, std::shared_ptr<StreamGate> gate)
      : connection_(std::move(connection)), gate_(std::move(gate)) {}
  asio::awaitable<core::Result<void>> Write(std::string bytes) override;

 private:
  std::shared_ptr<Connection> connection_;
  std::shared_ptr<StreamGate> gate_;
};

class Connection final : public std::enable_shared_from_this<Connection> {
 public:
  Connection(std::shared_ptr<Runtime> runtime, Tcp::socket socket,
             std::function<void(std::shared_ptr<Connection>)> remove)
      : runtime_(std::move(runtime)),
        socket_(std::move(socket)),
        idle_(runtime_->network),
        heartbeat_(runtime_->network),
        remove_(std::move(remove)),
        codec_(runtime_->options.codec),
        max_output_bytes_(runtime_->options.max_output_bytes) {}

  void Start() {
    IoError error;
    error = socket_.set_option(Tcp::no_delay(true), error);
    const auto endpoint = socket_.remote_endpoint(error);
    if (!error) {
      remote_ = endpoint.address().to_string();
    }
    Touch();
    WaitForIdle();
    Read();
  }

  void Close(engine::RequestCancellationReason reason = engine::RequestCancellationReason::kClientCancelled) {
    {
      std::lock_guard lock(write_mutex_);
      if (closed_) {
        return;
      }
      closed_ = true;
    }
    IoError ignored;
    ignored = socket_.shutdown(Tcp::socket::shutdown_both, ignored);
    ignored = socket_.close(ignored);
    (void)idle_.cancel();
    (void)heartbeat_.cancel();
    active_stream_.reset();
    if (execution_) {
      // Stop callbacks are application code, including dispatcher cancellation.
      asio::post(runtime_->application, [execution = execution_, reason] { (void)execution->Cancel(reason); });
    }
    FailQueued();
    remove_(shared_from_this());
  }

  static asio::awaitable<core::Result<void>> WriteEvent(std::shared_ptr<Connection> self, std::string bytes,
                                                        std::shared_ptr<StreamGate> gate) {
    if (bytes.empty()) {
      co_return co_await self->Send({}, std::move(gate));
    }
    if (bytes.size() > self->max_output_bytes_ - kHexBytes - 4U) {
      co_return core::Error::Validation(std::string(kOutputLimit));
    }
    co_return co_await self->Send(Chunk(bytes), std::move(gate));
  }

 private:
  struct Output final {
    std::shared_ptr<std::string> bytes;
    WriteCompletion completion;
  };

  void CompleteApplication(const std::shared_ptr<engine::RequestExecution>& execution, bool failed) {
    std::unique_lock lock(write_mutex_);
    if (closed_) {
      return;
    }
    if (failed) {
      lock.unlock();
      (void)execution->Cancel();
      return;
    }
    (void)execution->TryComplete(engine::RequestCompletion::kSucceeded);
  }

  asio::awaitable<core::Result<void>> Send(std::string bytes, std::shared_ptr<StreamGate> gate = {}) {
    auto token = asio::use_awaitable;
    co_return co_await asio::async_initiate<decltype(token), void(core::Result<void>)>(
        [self = shared_from_this(), bytes = std::move(bytes), gate = std::move(gate)](auto complete) mutable {
          struct PendingWrite final {
            decltype(complete) handler;
            std::atomic_bool pending{true};
          };
          auto executor = asio::get_associated_executor(complete);
          auto slot = asio::get_associated_cancellation_slot(complete);
          auto completion =
              std::allocate_shared<PendingWrite>(asio::recycling_allocator<PendingWrite>{}, std::move(complete));
          if (slot.is_connected()) {
            slot.assign([weak = std::weak_ptr(self), operation = std::weak_ptr(completion),
                         gate](asio::cancellation_type type) {
              const auto connection = weak.lock();
              if (!connection || type == asio::cancellation_type::none) {
                return;
              }
              std::lock_guard lock(connection->write_mutex_);
              if (!connection->closed_) {
                asio::post(connection->runtime_->network, [connection, operation, gate] {
                  const auto pending = operation.lock();
                  if (pending && pending->pending && (!gate || gate == connection->active_stream_)) {
                    // A partially written chunk cannot be reused safely.
                    connection->Close();
                  }
                });
              }
            });
          }
          auto finish = [executor, completion](core::Result<void> result) mutable {
            completion->pending = false;
            asio::post(executor, [completion, result = std::move(result)]() mutable {
              asio::get_associated_cancellation_slot(completion->handler).clear();
              completion->handler(std::move(result));
            });
          };
          // Serialize posting with Close so retained sinks cannot revive a stopped loop.
          std::lock_guard lock(self->write_mutex_);
          if (self->closed_) {
            finish(core::Error::Network(std::string(kStopped)));
            return;
          }
          asio::post(self->runtime_->network,
                     [self, bytes = std::move(bytes), gate = std::move(gate), finish = std::move(finish)]() mutable {
                       self->Queue(std::move(bytes), std::move(finish), gate);
                     });
        },
        token);
  }

  void Queue(std::string bytes, WriteCompletion completion, const std::shared_ptr<StreamGate>& gate = {}) {
    if (closed_) {
      completion(core::Error::Network(std::string(kStopped)));
      return;
    }
    if (gate && gate != active_stream_) {
      completion(core::Error::Network(std::string(kStopped)));
      return;
    }
    if (bytes.size() > runtime_->options.max_output_bytes - queued_bytes_) {
      Close();
      completion(core::Error::Validation(std::string(kOutputLimit)));
      return;
    }
    queued_bytes_ += bytes.size();
    output_.push_back({std::allocate_shared<std::string>(asio::recycling_allocator<std::string>{}, std::move(bytes)),
                       std::move(completion)});
    if (output_.size() == 1U) {
      Flush();
    }
  }

  void Flush() {
    auto bytes = output_.front().bytes;
    asio::async_write(socket_, asio::buffer(*bytes), [self = shared_from_this(), bytes](IoError error, std::size_t) {
      if (self->closed_) {
        return;
      }
      auto item = std::move(self->output_.front());
      self->output_.pop_front();
      self->queued_bytes_ -= bytes->size();
      if (error) {
        self->Close();
        item.completion(core::Error::Network(error.message()));
        return;
      }
      self->Touch();
      item.completion({});
      if (!self->output_.empty()) {
        self->Flush();
      }
    });
  }

  void FailQueued() {
    auto output = std::move(output_);
    queued_bytes_ = 0;
    for (auto& item : output) {
      item.completion(core::Error::Network(std::string(kStopped)));
    }
  }

  void Touch() { idle_deadline_ = asio::steady_timer::clock_type::now() + runtime_->options.idle_timeout; }

  void WaitForIdle() {
    // Activity only moves the deadline. Keep one wait instead of cancelling and
    // allocating another timer handler for every read and write.
    idle_.expires_at(idle_deadline_);
    idle_.async_wait([self = shared_from_this()](IoError error) {
      if (error || self->closed_) {
        return;
      }
      if (self->idle_deadline_ <= asio::steady_timer::clock_type::now()) {
        self->Close(engine::RequestCancellationReason::kDeadlineExceeded);
      } else {
        self->WaitForIdle();
      }
    });
  }

  void Heartbeat() {
    if (closed_ || !streaming_ || runtime_->options.heartbeat_interval.count() == 0) {
      return;
    }
    heartbeat_.expires_after(runtime_->options.heartbeat_interval);
    heartbeat_.async_wait([self = shared_from_this()](IoError error) {
      if (!error && self->streaming_ && !self->closed_) {
        if (self->output_.empty()) {
          self->Queue(Chunk(kHeartbeat), [](const core::Result<void>& /*result*/) {});
        }
        self->Heartbeat();
      }
    });
  }

  void Read() {
    socket_.async_read_some(asio::buffer(read_), [self = shared_from_this()](IoError error, std::size_t size) {
      if (self->closed_) {
        return;
      }
      if (error) {
        if (error == asio::error::eof) {
          self->eof_ = true;
          if (!self->processing_) {
            self->Process();
          }
        } else {
          self->Close();
        }
        return;
      }
      self->Touch();
      if (size > self->runtime_->options.max_input_bytes - self->input_.size()) {
        self->Close();
        return;
      }
      self->input_.append(self->read_.data(), size);
      self->Process();
      self->Read();
    });
  }

  void ErrorResponse(int status) {
    processing_ = true;
    HttpServerResponse response;
    response.status_code = status;
    const auto bytes = HttpAdapter::EncodeResponse(response, true);
    if (!bytes.ok()) {
      Close();
      return;
    }
    Queue(bytes.value(), [self = shared_from_this()](const core::Result<void>& /*result*/) { self->Close(); });
  }

  void Process() {
    if (closed_ || processing_) {
      return;
    }
    std::size_t consumed = 0;
    auto request = codec_.ParseRequest(input_, codec_state_, consumed, remote_);
    if (!request.ok()) {
      ErrorResponse(core::http::kStatusBadRequest);
      return;
    }
    auto& parsed = request.value();
    if (!parsed.has_value()) {
      if (eof_) {
        Close();
      }
      return;
    }
    if (runtime_->pending >= runtime_->options.max_pending_requests) {
      ErrorResponse(core::http::kStatusServiceUnavailable);
      return;
    }
    input_.erase(0, consumed);
    processing_ = true;
    ++runtime_->pending;
    execution_ = std::make_shared<engine::RequestExecution>();
    auto self = shared_from_this();
    auto execution = execution_;
    asio::co_spawn(runtime_->application, RunApplication(self, std::move(parsed.value()), execution_),
                   [self, execution](const std::exception_ptr& exception, bool close) {
                     self->CompleteApplication(execution, static_cast<bool>(exception));
                     asio::post(self->runtime_->network, [self, exception, close] {
                       --self->runtime_->pending;
                       if (self->runtime_->stopping && self->runtime_->pending == 0) {
                         DrainRuntime(self->runtime_);
                       }
                       self->streaming_ = false;
                       self->active_stream_.reset();
                       (void)self->heartbeat_.cancel();
                       self->execution_.reset();
                       if (exception || close || self->closed_) {
                         self->Close();
                       } else {
                         self->processing_ = false;
                         self->Process();
                       }
                     });
                   });
  }

  static asio::awaitable<bool> RunApplication(std::shared_ptr<Connection> self, HttpServerRequest request,
                                              std::shared_ptr<engine::RequestExecution> execution) {
    const auto context = execution->Context();
    if (self->runtime_->stopping.load()) {
      (void)execution->Cancel(engine::RequestCancellationReason::kServerShutdown);
      co_return true;
    }
    // Retain connection policy while transferring the parsed request to the handler.
    const bool reusable = HttpAdapter::IsConnectionReusable(request);
    auto response = co_await self->runtime_->handler(std::move(request), context);
    if (!response.ok()) {
      response = EventHttpResponse{};
      response.value().response.status_code = core::http::kStatusInternalServerError;
    }
    auto& event = response.value();
    if (event.response.stream_writer) {
      event.response = {};
      event.response.status_code = core::http::kStatusNotImplemented;
      event.response.body = kBlockingStream;
      event.stream = {};
    }
    const bool close = !reusable || HttpAdapter::ShouldCloseConnection({}, event.response);
    if (!FitsOutput(event.response, self->runtime_->options.max_output_bytes, static_cast<bool>(event.stream))) {
      throw std::runtime_error(std::string(kOutputLimit));
    }
    auto bytes = event.stream ? HttpAdapter::EncodeResponseHead(event.response, close, true)
                              : HttpAdapter::EncodeResponse(event.response, close);
    if (!bytes.ok()) {
      throw std::runtime_error(std::string(bytes.error().message()));
    }
    bool initial_sent = false;
    if (event.stream && !event.response.body.empty()) {
      auto initial = Chunk(event.response.body);
      const auto limit = self->runtime_->options.max_output_bytes;
      if (bytes.value().size() <= limit && initial.size() <= limit - bytes.value().size()) {
        bytes.value().append(initial);
        event.response.body.clear();
        initial_sent = true;
      }
    }
    const auto sent = co_await self->Send(std::move(bytes.value()));
    if (!sent.ok()) {
      co_return true;
    }
    if (event.stream) {
      auto gate = std::make_shared<StreamGate>();
      auto sink = std::make_shared<ConnectionStream>(self, gate);
      asio::post(self->runtime_->network, [self, gate = std::move(gate)] {
        self->active_stream_ = gate;
        self->streaming_ = true;
        self->Heartbeat();
      });
      if (!initial_sent && !event.response.body.empty()) {
        const auto initial = co_await sink->Write(std::move(event.response.body));
        if (!initial.ok()) {
          throw std::runtime_error(std::string(initial.error().message()));
        }
      }
      const auto streamed = co_await event.stream(std::move(sink), context);
      if (!streamed.ok()) {
        throw std::runtime_error(std::string(streamed.error().message()));
      }
      asio::post(self->runtime_->network, [self] {
        self->streaming_ = false;
        self->active_stream_.reset();
        (void)self->heartbeat_.cancel();
      });
      const auto finished = co_await self->Send(std::string(kFinalChunk));
      if (!finished.ok()) {
        co_return true;
      }
    }
    co_return close;
  }

  std::shared_ptr<Runtime> runtime_;
  Tcp::socket socket_;
  asio::steady_timer idle_;
  asio::steady_timer::time_point idle_deadline_;
  asio::steady_timer heartbeat_;
  std::function<void(std::shared_ptr<Connection>)> remove_;
  HttpAdapter codec_;
  HttpConnectionState codec_state_;
  std::array<char, kReadBytes> read_{};
  std::string input_;
  std::string remote_;
  std::deque<Output> output_;
  std::shared_ptr<engine::RequestExecution> execution_;
  std::shared_ptr<StreamGate> active_stream_;
  const std::size_t max_output_bytes_;
  std::mutex write_mutex_;
  std::size_t queued_bytes_ = 0;
  bool closed_ = false;
  bool processing_ = false;
  bool streaming_ = false;
  bool eof_ = false;
};
asio::awaitable<core::Result<void>> ConnectionStream::Write(std::string bytes) {
  return Connection::WriteEvent(connection_, std::move(bytes), gate_);
}

}  // namespace

struct EventHttpServer::Impl final {
  Impl(AsyncHandler handler, EventHttpServerOptions options)
      : runtime(std::make_shared<Runtime>(std::move(handler), std::move(options))) {}
  void Stop() {
    if (runtime->stopping) {
      return;
    }
    runtime->stopping = true;
    IoError ignored;
    ignored = runtime->acceptor.close(ignored);
    auto active = connections;
    for (const auto& connection : active) {
      connection->Close(engine::RequestCancellationReason::kServerShutdown);
    }
    if (runtime->pending == 0) {
      DrainRuntime(runtime);
    }
  }
  void Accept() {
    runtime->acceptor.async_accept([this](IoError error, Tcp::socket socket) {
      if (error) {
        Stop();
        return;
      }
      if (!runtime->stopping && connections.size() < runtime->options.max_connections) {
        auto connection = std::make_shared<Connection>(runtime, std::move(socket),
                                                       [this](const auto& item) { connections.erase(item); });
        connections.insert(connection);
        connection->Start();
      }
      if (!runtime->stopping) {
        Accept();
      }
    });
  }
  std::shared_ptr<Runtime> runtime;
  std::unordered_set<std::shared_ptr<Connection>> connections;
  std::thread network_thread;
  std::vector<std::thread> application_threads;
  std::mutex lifecycle;
  std::atomic<std::uint16_t> bound_port{0};
  bool started = false;
  bool shutdown_posted = false;
};

EventHttpServer::EventHttpServer(Handler handler, EventHttpServerOptions options)
    : EventHttpServer(LegacyHandler(std::move(handler)), std::move(options)) {}
EventHttpServer::EventHttpServer(AsyncHandler handler, EventHttpServerOptions options)
    : impl_(std::make_unique<Impl>(std::move(handler), std::move(options))) {}
EventHttpServer::~EventHttpServer() {
  try {
    Shutdown();
    Join();
  } catch (...) {
    std::terminate();
  }
}

core::Result<void> EventHttpServer::Start() {
  std::lock_guard lock(impl_->lifecycle);
  auto& rt = *impl_->runtime;
  const auto& options = rt.options;
  if (impl_->started) {
    return core::Error::Validation(std::string(kAlreadyStarted));
  }
  if (!rt.handler || options.max_connections == 0 || options.max_pending_requests == 0 ||
      options.application_threads == 0 || options.max_input_bytes == 0 || options.max_output_bytes <= kHexBytes + 4U ||
      options.codec.max_request_size == 0 || options.idle_timeout.count() <= 0 ||
      options.heartbeat_interval.count() < 0) {
    return core::Error::Validation(std::string(kInvalidOptions));
  }
  IoError error;
  const auto address = asio::ip::make_address(options.address, error);
  if (error) {
    return core::Error::Network(error.message());
  }
  const Tcp::endpoint endpoint(address, options.port);
  error = rt.acceptor.open(endpoint.protocol(), error);
  if (!error) {
    error = rt.acceptor.set_option(Tcp::acceptor::reuse_address(true), error);
  }
  if (!error) {
    error = rt.acceptor.bind(endpoint, error);
  }
  if (!error) {
    error = rt.acceptor.listen(asio::socket_base::max_listen_connections, error);
  }
  if (error) {
    IoError ignored;
    ignored = rt.acceptor.close(ignored);
    return core::Error::Network(error.message());
  }
  impl_->bound_port.store(rt.acceptor.local_endpoint().port());
  impl_->started = true;
  impl_->Accept();
  try {
    for (std::size_t index = 0; index < options.application_threads; ++index) {
      impl_->application_threads.emplace_back([this] { (void)impl_->runtime->application.run(); });
    }
    impl_->network_thread = std::thread([this] { (void)impl_->runtime->network.run(); });
  } catch (const std::system_error&) {
    rt.stopping = true;
    rt.application.stop();
    rt.network.stop();
    IoError ignored;
    ignored = rt.acceptor.close(ignored);
    for (auto& thread : impl_->application_threads) {
      if (thread.joinable()) {
        thread.join();
      }
    }
    return core::Error::Internal(std::string(kThreadFailure));
  }
  return {};
}
std::uint16_t EventHttpServer::port() const noexcept { return impl_->bound_port.load(); }
void EventHttpServer::Shutdown() {
  std::lock_guard lock(impl_->lifecycle);
  if (!impl_->started || impl_->shutdown_posted) {
    return;
  }
  impl_->shutdown_posted = true;
  asio::post(impl_->runtime->network, [this] { impl_->Stop(); });
}
void EventHttpServer::Join() {
  if (impl_->network_thread.joinable()) {
    impl_->network_thread.join();
  }
  for (auto& worker : impl_->application_threads) {
    if (worker.joinable()) {
      worker.join();
    }
  }
}
}  // namespace a2a::server
