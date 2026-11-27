// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)
#include "event_http_listener.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <boost/asio/any_completion_handler.hpp>
#include <boost/asio/associated_cancellation_slot.hpp>
#include <boost/asio/associated_executor.hpp>
#include <boost/asio/async_result.hpp>
#include <boost/asio/bind_immediate_executor.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
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
using WriteCompletion = asio::any_completion_handler<void(core::Result<void>)>;
constexpr std::size_t kReadBytes = 8192;
constexpr std::size_t kHexBytes = sizeof(std::size_t) * 2U;
constexpr std::string_view kFinalChunk = "0\r\n\r\n";
constexpr std::string_view kHeartbeat = ": heartbeat\n\n";
constexpr int kNetworkConcurrency = 1;
constexpr std::string_view kStopped = "HTTP connection closed";
constexpr std::string_view kOutputLimit = "HTTP output exceeds configured bound";
constexpr std::string_view kThreadFailure = "HTTP worker startup failed";
constexpr std::string_view kAlreadyStarted = "HTTP server Start is single-use";
constexpr std::string_view kInvalidOptions = "HTTP server requires a handler, positive limits and idle timeout";
constexpr std::string_view kInvalidNetworkWorkers = "HTTP network worker count must be automatic, one, or two";
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

std::string ChunkOwned(std::string bytes) {
  std::array<char, kHexBytes + core::http::kLineTerminator.size()> prefix{};
  const auto converted = std::to_chars(prefix.data(), prefix.data() + kHexBytes, bytes.size(), 16);
  const auto* end = std::ranges::copy(core::http::kLineTerminator, converted.ptr).out;
  bytes.insert(0, prefix.data(), static_cast<std::size_t>(end - prefix.data()));
  bytes.append(core::http::kLineTerminator);
  return bytes;
}

bool FitsOutput(const HttpServerResponse& response, std::size_t limit, bool streaming) {
  if (response.body.size() > limit) {
    return false;
  }
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

class Connection;

struct NetworkLoop final {
  asio::io_context context{kNetworkConcurrency};
  asio::executor_work_guard<asio::io_context::executor_type> work{context.get_executor()};
  std::unordered_set<std::shared_ptr<Connection>> connections;
  std::size_t pending = 0;
  bool stopping = false;
};

struct Runtime final {
  Runtime(EventHttpServer::AsyncHandler callback, EventHttpServerOptions settings,
          http_runtime::SelectNativeLane select_lane, bool native_binding)
      : handler(std::move(callback)),
        options(std::move(settings)),
        select_lane(std::move(select_lane)),
        secondary(UseSecondLoop(options, native_binding) ? std::make_unique<NetworkLoop>() : nullptr),
        acceptor(network.context),
        drains(NetworkWorkers()) {}

  static bool UseSecondLoop(const EventHttpServerOptions& options, bool native_binding) {
    if (options.network_workers != 0) {
      return options.network_workers == EventHttpServerOptions::kMaxNetworkWorkers;
    }
    return native_binding && std::thread::hardware_concurrency() >= options.network_auto_min_cpus;
  }

  [[nodiscard]] std::size_t NetworkWorkers() const noexcept { return 1U + (secondary ? 1U : 0U); }

  bool ReserveRequest() noexcept {
    auto count = pending.load(std::memory_order_relaxed);
    while (count < options.max_pending_requests) {
      if (pending.compare_exchange_weak(count, count + 1, std::memory_order_relaxed)) {
        return true;
      }
    }
    return false;
  }

  EventHttpServer::AsyncHandler handler;
  std::shared_ptr<const http_runtime::NativeHttpHandler> native_handler;
  EventHttpServerOptions options;
  const http_runtime::SelectNativeLane select_lane;
  NetworkLoop network;
  const std::unique_ptr<NetworkLoop> secondary;
  asio::io_context application;
  asio::executor_work_guard<asio::io_context::executor_type> application_work{application.get_executor()};
  Tcp::acceptor acceptor;
  std::atomic_size_t active_connections{0};
  std::atomic_size_t pending{0};
  std::atomic_bool stopping{false};
  std::atomic_size_t drains;
};

void ReleaseRuntime(const std::shared_ptr<Runtime>& runtime) {
  if (runtime->drains.fetch_sub(1, std::memory_order_acq_rel) == 1) {
    asio::post(runtime->application, [runtime] {
      runtime->handler = {};
      runtime->native_handler.reset();
      runtime->application_work.reset();
    });
    runtime->network.work.reset();
    if (runtime->secondary) {
      runtime->secondary->work.reset();
    }
  }
}

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

class NativeConnectionStream final : public http_runtime::NativeHttpStream {
 public:
  NativeConnectionStream(std::shared_ptr<Connection> connection, std::shared_ptr<engine::RequestExecution> execution,
                         bool reusable)
      : connection_(std::move(connection)), execution_(std::move(execution)), reusable_(reusable) {}
  asio::awaitable<core::Result<void>> Start(HttpServerResponse response, std::string first_event) override;
  asio::awaitable<core::Result<void>> Write(std::string bytes) override;
  [[nodiscard]] http_runtime::NativeHttpBatchLimits BatchLimits() const noexcept override;
  void Finish(std::string error_event) override;
  void FinishEmpty(HttpServerResponse response) override;
  void Abort() override;

 private:
  std::shared_ptr<Connection> connection_;
  std::shared_ptr<engine::RequestExecution> execution_;
  const std::shared_ptr<StreamGate> gate_ = std::make_shared<StreamGate>();
  const bool reusable_;
  bool close_ = true;
  bool headers_queued_ = false;
};

class Connection final : public std::enable_shared_from_this<Connection> {
 public:
  Connection(std::shared_ptr<Runtime> runtime, NetworkLoop& loop, Tcp::socket socket)
      : runtime_(std::move(runtime)),
        native_lane_(runtime_->select_lane ? runtime_->select_lane() : http_runtime::NativeLane{}),
        loop_(loop),
        socket_(std::move(socket)),
        idle_(loop_.context),
        heartbeat_(loop_.context),
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
      asio::post(runtime_->application, [execution = execution_, reason] { (void)execution->Cancel(reason); });
    }
    FailQueued();
    loop_.connections.erase(shared_from_this());
    runtime_->active_connections.fetch_sub(1, std::memory_order_relaxed);
  }

  asio::awaitable<core::Result<void>> WriteEvent(std::string bytes, std::shared_ptr<StreamGate> gate) {
    if (bytes.size() > max_output_bytes_ - kHexBytes - 4U) {
      return OutputLimitError();
    }
    if (!bytes.empty()) {
      bytes = ChunkOwned(std::move(bytes));
    }
    return Send(std::move(bytes), std::move(gate));
  }

 private:
  static asio::awaitable<core::Result<void>> OutputLimitError() {
    co_return core::Error::Validation(std::string(kOutputLimit));
  }

  friend class NativeConnectionStream;

  struct Output final {
    std::string bytes;
    WriteCompletion completion;
    std::size_t id = 0;
  };

  asio::awaitable<core::Result<void>> Send(std::string bytes, std::shared_ptr<StreamGate> gate = {},
                                           bool starts_stream = false) {
    auto token = asio::use_awaitable;
    return asio::async_initiate<decltype(token), void(core::Result<void>)>(
        [self = shared_from_this(), bytes = std::move(bytes), gate = std::move(gate),
         starts_stream](auto complete) mutable {
          auto executor = asio::get_associated_executor(complete);
          auto slot = asio::get_associated_cancellation_slot(complete);
          auto finish = [executor, complete = std::move(complete)](core::Result<void> result) mutable {
            asio::post(executor, [complete = std::move(complete), result = std::move(result)]() mutable {
              complete(std::move(result));
            });
          };
          // Serialize posting with Close so retained sinks cannot revive a stopped loop.
          std::lock_guard lock(self->write_mutex_);
          if (self->closed_) {
            finish(core::Error::Network(std::string(kStopped)));
            return;
          }
          const auto id = ++self->next_write_id_;
          if (slot.is_connected()) {
            slot.assign([weak = std::weak_ptr(self), id, gate](asio::cancellation_type type) {
              if (type != asio::cancellation_type::none) {
                if (const auto connection = weak.lock()) {
                  connection->CancelWrite(id, gate);
                }
              }
            });
          }
          asio::post(self->loop_.context, [self, bytes = std::move(bytes), gate = std::move(gate),
                                           finish = std::move(finish), id, starts_stream]() mutable {
            if (starts_stream && !self->closed_) {
              self->ActivateStream(gate);
            }
            self->Queue(std::move(bytes), std::move(finish), gate, id);
          });
        },
        token);
  }

  void CancelWrite(std::size_t id, const std::shared_ptr<StreamGate>& gate) {
    std::lock_guard lock(write_mutex_);
    if (!closed_) {
      asio::post(loop_.context, [self = shared_from_this(), id, gate] {
        if (gate && gate != self->active_stream_) {
          return;
        }
        if (self->active_output_.id == id ||
            std::ranges::any_of(self->output_, [id](const Output& queued) { return queued.id == id; })) {
          self->Close();
        }
      });
    }
  }

  void Queue(std::string bytes, WriteCompletion completion, const std::shared_ptr<StreamGate>& gate = {},
             std::size_t id = 0) {
    if (closed_ || (gate && gate != active_stream_)) {
      completion(core::Error::Network(std::string(kStopped)));
      return;
    }
    if (bytes.size() > max_output_bytes_ - queued_bytes_) {
      Close();
      completion(core::Error::Validation(std::string(kOutputLimit)));
      return;
    }
    queued_bytes_ += bytes.size();
    Output item{.bytes = std::move(bytes), .completion = std::move(completion), .id = id};
    if (writing_) {
      output_.push_back(std::move(item));
    } else {
      writing_ = true;
      Flush(std::move(item));
    }
  }

  void Flush(Output item) {
    active_output_ = std::move(item);
    auto finished = [self = shared_from_this()](IoError error, std::size_t) {
      if (self->closed_) {
        self->active_output_ = {};
        return;
      }
      self->queued_bytes_ -= self->active_output_.bytes.size();
      auto completion = std::move(self->active_output_.completion);
      self->active_output_ = {};
      if (error) {
        self->Close();
        completion(core::Error::Network(error.message()));
        return;
      }
      self->Touch();
      completion(core::Result<void>{});
      if (!self->closed_ && !self->output_.empty()) {
        auto next = std::move(self->output_.front());
        self->output_.pop_front();
        self->Flush(std::move(next));
      } else {
        self->writing_ = false;
      }
    };
    asio::async_write(socket_, asio::buffer(active_output_.bytes),
                      asio::bind_immediate_executor(loop_.context.get_executor(), std::move(finished)));
  }

  void FailQueued() {
    auto active = std::move(active_output_.completion);
    if (active) {
      active(core::Error::Network(std::string(kStopped)));
    }
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
        if (!self->writing_) {
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
    if (runtime_->stopping.load(std::memory_order_relaxed)) {
      Close(engine::RequestCancellationReason::kServerShutdown);
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
    if (!runtime_->ReserveRequest()) {
      ErrorResponse(core::http::kStatusServiceUnavailable);
      return;
    }
    input_.erase(0, consumed);
    processing_ = true;
    ++loop_.pending;
    execution_ = std::make_shared<engine::RequestExecution>();
    if (auto native_handler = runtime_->native_handler) {
      StartNative(std::move(parsed.value()), std::move(native_handler));
      return;
    }
    auto self = shared_from_this();
    auto execution = execution_;
    asio::co_spawn(runtime_->application, RunApplication(self, std::move(parsed.value()), execution_),
                   [self, execution](const std::exception_ptr& exception, TerminalResponse response) {
                     self->PostTerminalResponse(execution, exception, std::move(response));
                   });
  }

  void StartNative(HttpServerRequest&& request, std::shared_ptr<const http_runtime::NativeHttpHandler> native_handler) {
    const bool borrowed = static_cast<bool>(native_lane_.first);
    auto invoke = [self = shared_from_this(), execution = execution_, handler = std::move(native_handler),
                   request = std::move(request), borrowed]() mutable {
      const auto callback = std::move(handler);
      RunNative(self, *callback, std::move(request), execution, borrowed);
    };
    if (!borrowed) {
      asio::post(runtime_->application, std::move(invoke));
      return;
    }
    runtime_->drains.fetch_add(1, std::memory_order_relaxed);
    asio::post(native_lane_.first, [runtime = runtime_, invoke = std::move(invoke)]() mutable {
      invoke();
      ReleaseRuntime(runtime);
    });
  }

  static void RunNative(const std::shared_ptr<Connection>& self, const http_runtime::NativeHttpHandler& handler,
                        HttpServerRequest&& request, const std::shared_ptr<engine::RequestExecution>& execution,
                        bool borrowed) {
    if (self->runtime_->stopping.load() || execution->Context().StopToken().stop_requested()) {
      if (!borrowed) {
        (void)execution->Cancel(engine::RequestCancellationReason::kServerShutdown);
      }
      self->PostTerminalResponse(execution, {}, {});
      return;
    }
    const bool reusable = HttpAdapter::IsConnectionReusable(request);
    auto complete = [self, execution, reusable](core::Result<HttpServerResponse> result) {
      try {
        auto event = GetResponse(std::move(result));
        self->PostTerminalResponse(execution, {}, self->EncodeResponse(event.response, reusable, false));
      } catch (...) {
        self->PostTerminalResponse(execution, std::current_exception(), {});
      }
    };
    try {
      handler(
          std::move(request), execution->Context(), std::move(complete),
          [self, execution, reusable] { return std::make_shared<NativeConnectionStream>(self, execution, reusable); },
          {.executor = self->runtime_->application.get_executor(), .lane = self->native_lane_.second});
    } catch (...) {
      self->PostTerminalResponse(execution, std::current_exception(), {});
    }
  }

  struct TerminalResponse final {
    std::string bytes;
    bool close = true;
    std::string preceding_chunk;
  };

  void PostTerminalResponse(const std::shared_ptr<engine::RequestExecution>& execution,
                            const std::exception_ptr& exception, TerminalResponse response) {
    if (exception) {
      (void)execution->Cancel();
    }
    asio::post(loop_.context, [self = shared_from_this(), response = std::move(response)]() mutable {
      self->WriteTerminalResponse(std::move(response));
    });
  }

  void FinishRequest(bool close) {
    runtime_->pending.fetch_sub(1, std::memory_order_relaxed);
    --loop_.pending;
    execution_.reset();
    if (close || closed_) {
      Close();
    } else {
      processing_ = false;
      Process();
    }
    if (loop_.stopping && loop_.pending == 0) {
      ReleaseRuntime(runtime_);
    }
  }

  void WriteTerminalResponse(TerminalResponse response) {
    const bool stream = streaming_;
    if (stream) {
      streaming_ = false;
      active_stream_.reset();
      (void)heartbeat_.cancel();
    }
    if (closed_ || response.bytes.empty()) {
      FinishRequest(true);
      return;
    }
    if (!response.preceding_chunk.empty()) {
      auto chunk = std::move(response.preceding_chunk);
      Queue(std::move(chunk),
            [self = shared_from_this(), response = std::move(response)](const core::Result<void>& result) mutable {
              if (result.ok()) {
                self->WriteTerminalBytes(std::move(response));
              } else {
                self->FinishRequest(true);
              }
            });
      return;
    }
    WriteTerminalBytes(std::move(response));
  }

  void WriteTerminalBytes(TerminalResponse response) {
    Queue(std::move(response.bytes),
          [self = shared_from_this(), close = response.close](const core::Result<void>& result) {
            if (result.ok()) {
              (void)self->execution_->TryComplete(engine::RequestCompletion::kSucceeded);
            }
            self->FinishRequest(close || !result.ok());
          });
  }

  void ActivateStream(const std::shared_ptr<StreamGate>& gate) {
    active_stream_ = gate;
    streaming_ = true;
    Heartbeat();
  }

  static asio::awaitable<TerminalResponse> RunApplication(std::shared_ptr<Connection> self, HttpServerRequest request,
                                                          std::shared_ptr<engine::RequestExecution> execution) {
    const auto context = execution->Context();
    if (self->runtime_->stopping.load()) {
      (void)execution->Cancel(engine::RequestCancellationReason::kServerShutdown);
      co_return TerminalResponse{};
    }
    const bool reusable = HttpAdapter::IsConnectionReusable(request);
    auto event = GetResponse(co_await self->runtime_->handler(std::move(request), context));
    if (event.stream) {
      co_return co_await SendStream(std::move(self), std::move(execution), reusable, std::move(event));
    }
    co_return self->EncodeResponse(event.response, reusable, false);
  }

  static EventHttpResponse GetResponse(core::Result<HttpServerResponse> response) {
    if (!response.ok()) {
      return GetResponse(core::Result<EventHttpResponse>(response.error()));
    }
    return GetResponse(EventHttpResponse{.response = std::move(response.value()), .stream = {}});
  }

  static EventHttpResponse GetResponse(core::Result<EventHttpResponse> response) {
    if (!response.ok()) {
      EventHttpResponse error;
      error.response.status_code = core::http::kStatusInternalServerError;
      return error;
    }
    auto event = std::move(response.value());
    if (event.response.stream_writer) {
      event = {};
      event.response.status_code = core::http::kStatusNotImplemented;
      event.response.body = kBlockingStream;
    }
    return event;
  }

  TerminalResponse EncodeResponse(const HttpServerResponse& response, bool reusable, bool streaming) const {
    const bool close = !reusable || HttpAdapter::ShouldCloseConnection({}, response);
    if (!FitsOutput(response, runtime_->options.max_output_bytes, streaming)) {
      throw std::runtime_error(std::string(kOutputLimit));
    }
    auto bytes = streaming ? HttpAdapter::EncodeResponseHead(response, close, true)
                           : HttpAdapter::EncodeResponse(response, close);
    if (!bytes.ok()) {
      throw std::runtime_error(std::string(bytes.error().message()));
    }
    return {.bytes = std::move(bytes.value()), .close = close, .preceding_chunk = {}};
  }

  static asio::awaitable<TerminalResponse> SendStream(std::shared_ptr<Connection> self,
                                                      std::shared_ptr<engine::RequestExecution> execution,
                                                      bool reusable, EventHttpResponse event) {
    auto head = self->EncodeResponse(event.response, reusable, true);
    const auto context = execution->Context();
    bool initial_sent = false;
    if (!event.response.body.empty()) {
      auto initial = Chunk(event.response.body);
      const auto limit = self->runtime_->options.max_output_bytes;
      if (head.bytes.size() <= limit && initial.size() <= limit - head.bytes.size()) {
        head.bytes.append(initial);
        event.response.body.clear();
        initial_sent = true;
      }
    }
    const auto sent = co_await self->Send(std::move(head.bytes));
    if (!sent.ok()) {
      co_return TerminalResponse{};
    }
    auto gate = std::make_shared<StreamGate>();
    auto sink = std::make_shared<ConnectionStream>(self, gate);
    asio::post(self->loop_.context, [self, gate = std::move(gate)] { self->ActivateStream(gate); });
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
    co_return TerminalResponse{.bytes = std::string(kFinalChunk), .close = head.close, .preceding_chunk = {}};
  }

  std::shared_ptr<Runtime> runtime_;
  const http_runtime::NativeLane native_lane_;
  NetworkLoop& loop_;
  Tcp::socket socket_;
  asio::steady_timer idle_;
  asio::steady_timer::time_point idle_deadline_;
  asio::steady_timer heartbeat_;
  HttpAdapter codec_;
  HttpConnectionState codec_state_;
  std::array<char, kReadBytes> read_{};
  std::string input_;
  std::string remote_;
  std::deque<Output> output_;
  Output active_output_;
  std::shared_ptr<engine::RequestExecution> execution_;
  std::shared_ptr<StreamGate> active_stream_;
  const std::size_t max_output_bytes_;
  std::mutex write_mutex_;
  std::size_t next_write_id_ = 0;
  std::size_t queued_bytes_ = 0;
  bool writing_ = false;
  bool closed_ = false;
  bool processing_ = false;
  bool streaming_ = false;
  bool eof_ = false;
};
asio::awaitable<core::Result<void>> ConnectionStream::Write(std::string bytes) {
  return connection_->WriteEvent(std::move(bytes), gate_);
}

asio::awaitable<core::Result<void>> NativeConnectionStream::Start(HttpServerResponse response,
                                                                  std::string first_event) {
  auto head = connection_->EncodeResponse(response, reusable_, true);
  close_ = head.close;
  if (!first_event.empty()) {
    if (first_event.size() > connection_->max_output_bytes_ - kHexBytes - 4U) {
      co_return core::Error::Validation(std::string(kOutputLimit));
    }
    first_event = ChunkOwned(std::move(first_event));
    if (head.bytes.size() <= connection_->max_output_bytes_ &&
        first_event.size() <= connection_->max_output_bytes_ - head.bytes.size()) {
      head.bytes += first_event;
      first_event.clear();
    }
  }
  headers_queued_ = true;
  auto written = co_await connection_->Send(std::move(head.bytes), gate_, true);
  if (written.ok() && !first_event.empty()) {
    written = co_await connection_->Send(std::move(first_event), gate_);
  }

  co_return written;
}

asio::awaitable<core::Result<void>> NativeConnectionStream::Write(std::string bytes) {
  return connection_->WriteEvent(std::move(bytes), gate_);
}

http_runtime::NativeHttpBatchLimits NativeConnectionStream::BatchLimits() const noexcept {
  const auto& options = connection_->runtime_->options;
  return {
      .payload_bytes = std::min(options.max_sse_batch_payload_bytes, connection_->max_output_bytes_ - kHexBytes - 4U),
      .id_bytes = options.max_sse_batch_id_bytes};
}

void NativeConnectionStream::Finish(std::string error_event) {
  if (!headers_queued_) {
    Abort();
    return;
  }
  std::string chunk;
  if (!error_event.empty()) {
    if (error_event.size() > connection_->max_output_bytes_ - kHexBytes - 4U) {
      Abort();
      return;
    }
    chunk = ChunkOwned(std::move(error_event));
  }
  connection_->PostTerminalResponse(
      execution_, {}, {.bytes = std::string(kFinalChunk), .close = close_, .preceding_chunk = std::move(chunk)});
}

void NativeConnectionStream::FinishEmpty(HttpServerResponse response) {
  auto head = connection_->EncodeResponse(response, reusable_, true);
  connection_->PostTerminalResponse(
      execution_, {},
      {.bytes = std::string(kFinalChunk), .close = head.close, .preceding_chunk = std::move(head.bytes)});
}

void NativeConnectionStream::Abort() {
  connection_->PostTerminalResponse(execution_, std::make_exception_ptr(std::runtime_error(std::string(kStopped))), {});
}

}  // namespace

namespace http_runtime {

struct EventHttpListener::Impl final {
  Impl(EventHttpServer::AsyncHandler handler, NativeHttpHandler native_handler, EventHttpServerOptions options,
       http_runtime::SelectNativeLane select_lane)
      : runtime(std::make_shared<Runtime>(std::move(handler), std::move(options), std::move(select_lane),
                                          static_cast<bool>(native_handler))) {
    if (native_handler) {
      runtime->native_handler = std::make_shared<const NativeHttpHandler>(std::move(native_handler));
    }
  }
  static void StopLoop(const std::shared_ptr<Runtime>& runtime, NetworkLoop& loop) {
    while (!loop.connections.empty()) {
      const auto connection = *loop.connections.begin();
      connection->Close(engine::RequestCancellationReason::kServerShutdown);
    }
    loop.stopping = true;
    if (loop.pending == 0) {
      ReleaseRuntime(runtime);
    }
  }
  void Stop() {
    if (runtime->stopping.exchange(true)) {
      return;
    }
    IoError ignored;
    ignored = runtime->acceptor.close(ignored);
    if (runtime->secondary) {
      asio::post(runtime->secondary->context, [state = runtime] { StopLoop(state, *state->secondary); });
    }
    StopLoop(runtime, runtime->network);
  }
  void StartConnection(NetworkLoop& loop, Tcp::socket socket) {
    runtime->active_connections.fetch_add(1, std::memory_order_relaxed);
    auto start = [state = runtime, &loop, socket = std::move(socket)]() mutable {
      if (state->stopping.load(std::memory_order_relaxed)) {
        state->active_connections.fetch_sub(1, std::memory_order_relaxed);
        return;
      }
      auto connection = std::make_shared<Connection>(state, loop, std::move(socket));
      loop.connections.insert(connection);
      connection->Start();
    };
    if (&loop == &runtime->network) {
      start();
    } else {
      asio::post(loop.context, std::move(start));
    }
  }
  void Accept() {
    const auto active = runtime->active_connections.load(std::memory_order_relaxed);
    auto& loop =
        runtime->secondary && active >= runtime->options.second_network_connection_threshold && accept_to_secondary
            ? *runtime->secondary
            : runtime->network;
    accept_to_secondary = !accept_to_secondary;
    runtime->acceptor.async_accept(loop.context, [this, &loop](IoError error, Tcp::socket socket) {
      if (error) {
        Stop();
        return;
      }
      if (!runtime->stopping) {
        if (runtime->active_connections.load(std::memory_order_relaxed) < runtime->options.max_connections) {
          StartConnection(loop, std::move(socket));
        }
        Accept();
      }
    });
  }
  std::shared_ptr<Runtime> runtime;
  std::thread network_thread;
  std::thread secondary_thread;
  std::vector<std::thread> application_threads;
  std::mutex lifecycle;
  std::atomic<std::uint16_t> bound_port{0};
  bool started = false;
  bool shutdown_posted = false;
  bool accept_to_secondary = false;
};

EventHttpListener::EventHttpListener(EventHttpServer::AsyncHandler handler, NativeHttpHandler native_handler,
                                     EventHttpServerOptions options, http_runtime::SelectNativeLane select_lane)
    : impl_(std::make_unique<Impl>(std::move(handler), std::move(native_handler), std::move(options),
                                   std::move(select_lane))) {}

std::size_t EventHttpListener::network_workers() const noexcept { return impl_->runtime->NetworkWorkers(); }

EventHttpListener::~EventHttpListener() {
  try {
    Shutdown();
    Join();
  } catch (...) {
    std::terminate();
  }
}

core::Result<void> EventHttpListener::Start() {
  std::lock_guard lock(impl_->lifecycle);
  auto& rt = *impl_->runtime;
  const auto& options = rt.options;
  if (impl_->started) {
    return core::Error::Validation(std::string(kAlreadyStarted));
  }
  if (options.network_workers > EventHttpServerOptions::kMaxNetworkWorkers) {
    return core::Error::Validation(std::string(kInvalidNetworkWorkers));
  }
  if ((!rt.handler && !rt.native_handler) || options.max_connections == 0 || options.max_pending_requests == 0 ||
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
    if (rt.secondary) {
      impl_->secondary_thread = std::thread([&rt] { (void)rt.secondary->context.run(); });
    }
    impl_->network_thread = std::thread([&rt] { (void)rt.network.context.run(); });
  } catch (const std::system_error&) {
    rt.stopping = true;
    rt.application.stop();
    rt.network.context.stop();
    if (rt.secondary) {
      rt.secondary->context.stop();
    }
    IoError ignored;
    ignored = rt.acceptor.close(ignored);
    Join();
    return core::Error::Internal(std::string(kThreadFailure));
  }
  return {};
}
std::uint16_t EventHttpListener::port() const noexcept { return impl_->bound_port.load(); }
void EventHttpListener::Shutdown() {
  std::lock_guard lock(impl_->lifecycle);
  if (!impl_->started || impl_->shutdown_posted) {
    return;
  }
  impl_->shutdown_posted = true;
  asio::post(impl_->runtime->network.context, [this] { impl_->Stop(); });
}
void EventHttpListener::Join() {
  if (impl_->network_thread.joinable()) {
    impl_->network_thread.join();
  }
  if (impl_->secondary_thread.joinable()) {
    impl_->secondary_thread.join();
  }
  for (auto& worker : impl_->application_threads) {
    if (worker.joinable()) {
      worker.join();
    }
  }
}
}  // namespace http_runtime
}  // namespace a2a::server
