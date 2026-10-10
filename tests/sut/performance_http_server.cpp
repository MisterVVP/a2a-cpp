// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "sut/performance_http_server.h"

#include <algorithm>
#include <array>
#include <asio/buffer.hpp>
#include <asio/error.hpp>
#include <asio/executor_work_guard.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/address.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/post.hpp>
#include <asio/signal_set.hpp>
#include <asio/socket_base.hpp>
#include <asio/steady_timer.hpp>
#include <asio/thread_pool.hpp>
#include <asio/version.hpp>
#include <charconv>
#include <csignal>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include "a2a/core/http_constants.h"
#include "a2a/server/http_adapter.h"
#include "a2a/server/http_stream_source.h"
#include "a2a/server/transport_mux.h"
#include "sut/sut_runtime.h"

namespace a2a::tests::sut {
namespace {
constexpr int kMinimumAsioVersion = 101800;
static_assert(ASIO_VERSION >= kMinimumAsioVersion, "Standalone Asio 1.18 or newer is required");
constexpr std::size_t kReadBufferBytes = 4096;
constexpr std::size_t kMaximumHeaderBytes = std::size_t{64} * 1024U;
constexpr std::size_t kMaximumHeaderCount = 128;
constexpr std::size_t kBatchEventLimit = 16;
constexpr std::size_t kBatchByteTarget = std::size_t{64} * 1024U;
constexpr std::string_view kUnsupportedReadiness = "Live stream lacks readiness support";
constexpr std::string_view kOutputLimitExceeded = "HTTP output buffer limit exceeded";
constexpr std::string_view kInvalidLimits = "Invalid performance HTTP limits";
constexpr std::string_view kRouteFailure = "HTTP route failed";
constexpr std::size_t kChunkSizeDigits = sizeof(std::size_t) * 2U;
constexpr std::size_t kMaximumWorkers = 64;
constexpr std::size_t kMaximumConnections = 4096;
constexpr std::size_t kMaximumBufferBytes = std::size_t{16} * 1024U * 1024U;
constexpr std::string_view kFinalChunk = "0\r\n\r\n";
constexpr std::string_view kRemoteAddress = "localhost";
constexpr std::string_view kResourcePrefix = "A2A_HTTP_IO_RESOURCES";
constexpr const char* kWorkersEnv = "A2A_PERF_HTTP_WORKERS";
constexpr const char* kConnectionsEnv = "A2A_PERF_HTTP_MAX_CONNECTIONS";
constexpr const char* kInputEnv = "A2A_PERF_HTTP_MAX_INPUT_BYTES";
constexpr const char* kOutputEnv = "A2A_PERF_HTTP_MAX_OUTPUT_BYTES";
constexpr const char* kWriteSliceEnv = "A2A_PERF_HTTP_WRITE_SLICE_BYTES";

std::size_t ReadLimit(const char* name, std::size_t fallback, std::size_t maximum) {
  const char* value = std::getenv(name);
  if (value == nullptr) {
    return fallback;
  }
  const std::string_view text(value);
  std::size_t limit = 0;
  const auto parsed = std::from_chars(text.data(), text.data() + text.size(), limit);
  if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || limit == 0 || limit > maximum) {
    throw std::invalid_argument(name);
  }
  return limit;
}

PerformanceHttpServer::Options CheckedOptions(const PerformanceHttpServer::Options& options) {
  if (options.application_workers == 0 || options.application_workers > kMaximumWorkers ||
      options.max_connections == 0 || options.max_connections > kMaximumConnections || options.max_input_bytes == 0 ||
      options.max_input_bytes > kMaximumBufferBytes || options.max_output_bytes == 0 ||
      options.max_output_bytes > kMaximumBufferBytes || options.write_slice_bytes == 0 ||
      options.write_slice_bytes > kMaximumBufferBytes) {
    throw std::invalid_argument(std::string(kInvalidLimits));
  }
  return options;
}

std::string FrameChunk(std::string_view bytes) {
  std::array<char, kChunkSizeDigits> digits{};
  const auto encoded = std::to_chars(digits.data(), digits.data() + digits.size(), bytes.size(), 16);
  std::string framed;
  framed.reserve(bytes.size() + kChunkSizeDigits + core::http::kHeaderDelimiter.size());
  framed.append(digits.data(), static_cast<std::size_t>(encoded.ptr - digits.data()));
  framed.append(core::http::kLineTerminator);
  framed.append(bytes);
  framed.append(core::http::kLineTerminator);
  return framed;
}
struct HttpResponseWork final {
  server::HttpServerResponse response;
  std::string encoded;
  bool close_connection = false;
  bool handled = false;
  bool stream_finished = false;
};

struct StreamBatch final {
  std::string bytes;
  bool finished = false;
};

core::Result<StreamBatch> ReadStreamBatch(server::HttpStreamSource& source, bool close_connection, std::size_t limit) {
  StreamBatch batch;
  for (std::size_t index = 0; index < kBatchEventLimit && batch.bytes.size() < kBatchByteTarget; ++index) {
    auto event = source.Next();
    if (!event.ok()) {
      return event.error();
    }
    auto& payload = event.value();
    if (!payload.has_value()) {
      batch.finished = !source.IsLive();
      if (batch.finished && !close_connection) {
        batch.bytes.append(kFinalChunk);
      }
      break;
    }
    auto bytes = close_connection ? std::move(*payload) : FrameChunk(*payload);
    if (bytes.size() > limit || batch.bytes.size() > limit - bytes.size()) {
      return core::Error::Internal(std::string(kOutputLimitExceeded));
    }
    batch.bytes.append(bytes);
  }
  return batch;
}
}  // namespace

class PerformanceHttpServer::Impl final {
 public:
  class Connection;
  Impl(std::string_view host, int port, const server::TransportMux& mux, SutRuntimeObserver* observer, Options options)
      : host_(host),
        port_(port),
        mux_(mux),
        observer_(observer),
        options_(CheckedOptions(options)),
        acceptor_(io_),
        signals_(io_, SIGINT, SIGTERM) {
#ifdef _WIN32
    signals_.add(SIGBREAK);
#endif
    workers_.reserve(options_.application_workers);
    for (std::size_t index = 0; index < options_.application_workers; ++index) {
      workers_.push_back(std::make_unique<asio::thread_pool>(1));
    }
  }

  [[nodiscard]] bool Start();
  void Accept();
  void PumpRequests();
  void CompleteRequest(bool reset);
  void Shutdown();
  void EmitResources() const;
  asio::thread_pool& SelectWorker() {
    auto& worker = *workers_[next_worker_];
    next_worker_ = (next_worker_ + 1) % workers_.size();
    return worker;
  }
  void JoinWorkers() {
    for (auto& worker : workers_) {
      worker->join();
    }
  }

  std::string host_;
  int port_;
  const server::TransportMux& mux_;
  SutRuntimeObserver* observer_;
  Options options_;
  asio::io_context io_;
  std::vector<std::unique_ptr<asio::thread_pool>> workers_;
  std::size_t next_worker_ = 0;
  asio::ip::tcp::acceptor acceptor_;
  asio::signal_set signals_;
  std::unordered_set<std::shared_ptr<Connection>> connections_;
  std::size_t peak_connections_ = 0;
  std::size_t input_bytes_ = 0;
  std::size_t peak_input_bytes_ = 0;
  std::size_t output_bytes_ = 0;
  std::size_t peak_output_bytes_ = 0;
  bool stopping_ = false;
  bool accepting_ = false;
  std::deque<std::shared_ptr<Connection>> ready_requests_;
  std::size_t active_requests_ = 0;
  bool reset_running_ = false;
};

class PerformanceHttpServer::Impl::Connection final : public std::enable_shared_from_this<Connection> {
 public:
  Connection(Impl& owner, asio::ip::tcp::socket socket)
      : owner_(owner),
        worker_(owner.SelectWorker()),
        socket_(std::move(socket)),
        heartbeat_(owner.io_),
        guard_(asio::make_work_guard(owner.io_)),
        adapter_({.max_request_size = owner.options_.max_input_bytes,
                  .max_header_size = kMaximumHeaderBytes,
                  .max_header_count = kMaximumHeaderCount}),
        observer_(owner.observer_ == nullptr ? nullptr : owner.observer_->ObserveHttpConnection()) {}

  void Start();
  void Close();
  [[nodiscard]] bool IsReset() const;
  [[nodiscard]] bool IsClosed() const { return closed_; }
  void Dispatch();

 private:
  void Read();
  void ProcessRequest();
  void ParseAndRoute();
  void OnParsedInput(std::size_t consumed, std::size_t parked);
  void DeferReset(server::HttpServerRequest request, std::size_t consumed, std::size_t parked);
  void UpdateInput(std::size_t consumed, std::size_t parked);
  void Route(server::HttpServerRequest request, std::size_t consumed = 0, std::size_t parked = 0);
  [[nodiscard]] core::Result<HttpResponseWork> PrepareResponse(const server::HttpServerRequest& request,
                                                               server::HttpServerResponse response, bool handled);
  [[nodiscard]] core::Result<StreamBatch> ProduceBatch(server::HttpStreamSource& source, bool close_connection);
  void OnResponse(core::Result<HttpResponseWork> work, std::size_t consumed, std::size_t parked);
  void QueueOutput(std::string bytes);
  void Write();
  void OnWritten();
  void NextEvent();
  void OnBatch(core::Result<StreamBatch> batch);
  void CompleteResponse();
  void OnReady();
  void WaitForEvent();
  void FinishResponse();
  void Dispose();

  Impl& owner_;
  asio::thread_pool& worker_;
  asio::ip::tcp::socket socket_;
  asio::steady_timer heartbeat_;
  asio::executor_work_guard<asio::io_context::executor_type> guard_;
  server::HttpAdapter adapter_;
  server::HttpConnectionState parser_input_;
  std::string input_;
  std::string worker_input_;
  std::size_t parked_bytes_ = 0;
  std::size_t inflight_input_bytes_ = 0;
  std::optional<server::HttpServerRequest> pending_request_;
  std::array<char, kReadBufferBytes> read_buffer_{};
  std::unique_ptr<SutHttpConnectionObserver> observer_;
  std::optional<server::HttpServerResponse> response_;
  std::string output_;
  std::size_t write_offset_ = 0;
  bool job_busy_ = false;
  bool response_active_ = false;
  bool closed_ = false;
  bool close_after_response_ = false;
  bool handled_ = false;
  bool stream_ready_ = false;
  bool stream_finished_ = false;
  bool ready_registered_ = false;
  bool input_eof_ = false;
  bool read_pending_ = false;
  bool counted_ = false;
  bool reset_request_ = false;
};

void PerformanceHttpServer::Impl::Connection::Start() {
  asio::error_code error;
  socket_.set_option(asio::ip::tcp::no_delay(true), error);
  if (error) {
    Close();
    return;
  }
  Read();
}

void PerformanceHttpServer::Impl::Connection::Read() {
  if (closed_ || read_pending_ || input_eof_) {
    return;
  }
  const auto available = owner_.options_.max_input_bytes - input_.size() - parked_bytes_ - inflight_input_bytes_;
  if (available == 0) {
    return;
  }
  read_pending_ = true;
  socket_.async_read_some(asio::buffer(read_buffer_.data(), std::min(available, read_buffer_.size())),
                          [self = shared_from_this()](asio::error_code error, std::size_t bytes) {
                            self->read_pending_ = false;
                            if (self->closed_) {
                              return;
                            }
                            if (error == asio::error::eof) {
                              self->input_eof_ = true;
                              if (self->response_ && self->response_->stream_source) {
                                self->Close();
                              } else {
                                self->ProcessRequest();
                              }
                              return;
                            }
                            if (error) {
                              self->Close();
                              return;
                            }
                            self->input_.append(self->read_buffer_.data(), bytes);
                            self->owner_.input_bytes_ += bytes;
                            self->owner_.peak_input_bytes_ =
                                std::max(self->owner_.peak_input_bytes_, self->owner_.input_bytes_);
                            self->ProcessRequest();
                            if (!self->closed_) {
                              self->Read();
                            }
                          });
}

void PerformanceHttpServer::Impl::Connection::ProcessRequest() {
  if (closed_ || response_active_ || job_busy_) {
    return;
  }
  if (input_.empty() && parked_bytes_ == 0) {
    if (input_eof_) {
      Close();
    }
    return;
  }
  response_active_ = true;
  owner_.ready_requests_.push_back(shared_from_this());
  owner_.PumpRequests();
}

bool PerformanceHttpServer::Impl::Connection::IsReset() const {
  return pending_request_.has_value() && owner_.observer_ != nullptr &&
         owner_.observer_->IsHttpMeasurementReset(*pending_request_);
}

void PerformanceHttpServer::Impl::Connection::Dispatch() {
  reset_request_ = IsReset();
  counted_ = true;
  job_busy_ = true;
  if (pending_request_) {
    asio::post(worker_, [self = shared_from_this(), request = std::move(*pending_request_)]() mutable {
      self->Route(std::move(request), 0, self->parked_bytes_);
    });
    pending_request_.reset();
    return;
  }
  inflight_input_bytes_ = input_.size();
  input_.swap(worker_input_);
  asio::post(worker_, [self = shared_from_this()] { self->ParseAndRoute(); });
}

void PerformanceHttpServer::Impl::Connection::ParseAndRoute() {
  const auto buffered = parser_input_.BufferedSize() + worker_input_.size();
  auto appended = adapter_.AppendInput(parser_input_, worker_input_);
  worker_input_.clear();
  auto request = appended.ok() ? adapter_.TryReadRequest(parser_input_, std::string(kRemoteAddress))
                               : core::Result<std::optional<server::HttpServerRequest>>(appended.error());
  const auto parked = parser_input_.BufferedSize();
  const auto consumed = buffered - parked;
  if (!request.ok()) {
    asio::post(owner_.io_, [self = shared_from_this(), consumed, parked] {
      self->UpdateInput(consumed, parked);
      self->job_busy_ = false;
      self->Close();
    });
    return;
  }
  auto& parsed_request = request.value();
  if (!parsed_request) {
    asio::post(owner_.io_, [self = shared_from_this(), consumed, parked] { self->OnParsedInput(consumed, parked); });
    return;
  }
  if (owner_.observer_ != nullptr && owner_.observer_->IsHttpMeasurementReset(*parsed_request)) {
    asio::post(owner_.io_, [self = shared_from_this(), request = std::move(*parsed_request), consumed,
                            parked]() mutable { self->DeferReset(std::move(request), consumed, parked); });
    return;
  }
  Route(std::move(*parsed_request), consumed, parked);
}

void PerformanceHttpServer::Impl::Connection::UpdateInput(std::size_t consumed, std::size_t parked) {
  owner_.input_bytes_ -= consumed;
  parked_bytes_ = parked;
  inflight_input_bytes_ = 0;
}

void PerformanceHttpServer::Impl::Connection::OnParsedInput(std::size_t consumed, std::size_t parked) {
  UpdateInput(consumed, parked);
  job_busy_ = false;
  if (closed_) {
    Dispose();
    return;
  }
  counted_ = false;
  owner_.CompleteRequest(false);
  response_active_ = false;
  if (!input_.empty()) {
    ProcessRequest();
  } else if (input_eof_ || parked_bytes_ == owner_.options_.max_input_bytes) {
    Close();
    return;
  }
  Read();
}

void PerformanceHttpServer::Impl::Connection::DeferReset(server::HttpServerRequest request, std::size_t consumed,
                                                         std::size_t parked) {
  UpdateInput(consumed, parked);
  job_busy_ = false;
  if (closed_) {
    Dispose();
    return;
  }
  pending_request_ = std::move(request);
  owner_.ready_requests_.push_front(shared_from_this());
  counted_ = false;
  owner_.CompleteRequest(false);
}

void PerformanceHttpServer::Impl::Connection::Route(server::HttpServerRequest request, std::size_t consumed,
                                                    std::size_t parked) {
  core::Result<HttpResponseWork> work = core::Error::Internal(std::string(kRouteFailure));
  try {
    server::HttpServerResponse observed;
    const bool handled = observer_ != nullptr && observer_->BeginRequest(request, observed);
    auto response =
        handled ? core::Result<server::HttpServerResponse>(std::move(observed)) : owner_.mux_.RouteRequest(request);
    work = response.ok() ? PrepareResponse(request, std::move(response.value()), handled)
                         : core::Result<HttpResponseWork>(response.error());
  } catch (const std::exception& error) {
    work = core::Error::Internal(error.what());
  }
  asio::post(owner_.io_, [self = shared_from_this(), work = std::move(work), consumed, parked]() mutable {
    self->OnResponse(std::move(work), consumed, parked);
  });
}

core::Result<HttpResponseWork> PerformanceHttpServer::Impl::Connection::PrepareResponse(
    const server::HttpServerRequest& request, server::HttpServerResponse response, bool handled) {
  HttpResponseWork work;
  work.close_connection = server::HttpAdapter::ShouldCloseConnection(request, response);
  work.handled = handled;
  if (response.body.size() > owner_.options_.max_output_bytes || (response.stream_writer && !response.stream_source)) {
    return core::Error::Internal(std::string(kOutputLimitExceeded));
  }
  auto encoded = server::HttpAdapter::EncodeResponse(response, work.close_connection);
  if (!encoded.ok()) {
    return encoded.error();
  }
  work.encoded = std::move(encoded.value());
  if (response.stream_source) {
    auto batch = ProduceBatch(*response.stream_source, work.close_connection);
    if (!batch.ok()) {
      return batch.error();
    }
    work.stream_finished = batch.value().finished;
    work.encoded.append(batch.value().bytes);
  }
  if (work.encoded.size() > owner_.options_.max_output_bytes) {
    return core::Error::Internal(std::string(kOutputLimitExceeded));
  }
  work.response = std::move(response);
  return work;
}

void PerformanceHttpServer::Impl::Connection::OnResponse(core::Result<HttpResponseWork> work, std::size_t consumed,
                                                         std::size_t parked) {
  UpdateInput(consumed, parked);
  job_busy_ = false;
  if (!work.ok()) {
    Close();
    return;
  }
  response_ = std::move(work.value().response);
  close_after_response_ = work.value().close_connection;
  handled_ = work.value().handled;
  stream_finished_ = work.value().stream_finished;
  if (closed_) {
    Dispose();
    return;
  }
  if (input_eof_ && response_->stream_source) {
    Close();
    return;
  }
  QueueOutput(std::move(work.value().encoded));
}

void PerformanceHttpServer::Impl::Connection::QueueOutput(std::string bytes) {
  if (bytes.size() > owner_.options_.max_output_bytes) {
    Close();
    return;
  }
  owner_.output_bytes_ += bytes.size();
  owner_.peak_output_bytes_ = std::max(owner_.peak_output_bytes_, owner_.output_bytes_);
  output_ = std::move(bytes);
  write_offset_ = 0;
  if (output_.empty()) {
    OnWritten();
    return;
  }
  Write();
}

void PerformanceHttpServer::Impl::Connection::Write() {
  const auto count = std::min(output_.size() - write_offset_, owner_.options_.write_slice_bytes);
  socket_.async_write_some(asio::buffer(output_.data() + write_offset_, count),
                           [self = shared_from_this()](asio::error_code error, std::size_t bytes) {
                             if (self->closed_) {
                               return;
                             }
                             if (error || bytes == 0) {
                               self->Close();
                               return;
                             }
                             self->write_offset_ += bytes;
                             if (self->write_offset_ < self->output_.size()) {
                               self->Write();
                               return;
                             }
                             self->owner_.output_bytes_ -= self->output_.size();
                             self->output_.clear();
                             self->OnWritten();
                           });
}

void PerformanceHttpServer::Impl::Connection::OnWritten() {
  if (closed_ || !response_.has_value()) {
    return;
  }
  if (response_->stream_source) {
    response_->stream_source->CompleteDelivery();
  }
  if (!response_->stream_source || stream_finished_) {
    FinishResponse();
    return;
  }
  NextEvent();
}

void PerformanceHttpServer::Impl::Connection::NextEvent() {
  if (closed_ || job_busy_ || !output_.empty() || !response_.has_value()) {
    return;
  }
  heartbeat_.cancel();
  stream_ready_ = false;
  job_busy_ = true;
  asio::post(worker_, [self = shared_from_this()] {
    core::Result<StreamBatch> batch = core::Error::Internal(std::string(kUnsupportedReadiness));
    try {
      if (self->response_.has_value()) {
        batch = self->ProduceBatch(*self->response_->stream_source, self->close_after_response_);
      }
    } catch (const std::exception& error) {
      batch = core::Error::Internal(error.what());
    }
    asio::post(self->owner_.io_, [self, batch = std::move(batch)]() mutable { self->OnBatch(std::move(batch)); });
  });
}

core::Result<StreamBatch> PerformanceHttpServer::Impl::Connection::ProduceBatch(server::HttpStreamSource& source,
                                                                                bool close_connection) {
  const std::weak_ptr<Connection> weak = shared_from_this();
  if (!ready_registered_) {
    source.SetPendingEventByteLimit(owner_.options_.max_input_bytes);
  }
  const bool notified = ready_registered_ || source.SetReadyCallback([weak] {
    if (auto connection = weak.lock()) {
      asio::post(connection->owner_.io_, [connection] { connection->OnReady(); });
    }
  });
  ready_registered_ = notified;
  if (source.IsLive() && !notified) {
    return core::Error::Internal(std::string(kUnsupportedReadiness));
  }
  return ReadStreamBatch(source, close_connection, owner_.options_.max_output_bytes);
}

void PerformanceHttpServer::Impl::Connection::OnBatch(core::Result<StreamBatch> batch) {
  job_busy_ = false;
  if (closed_) {
    Dispose();
    return;
  }
  if (!batch.ok()) {
    Close();
    return;
  }
  stream_finished_ = batch.value().finished;
  if (!batch.value().bytes.empty()) {
    QueueOutput(std::move(batch.value().bytes));
    return;
  }
  if (stream_finished_) {
    FinishResponse();
    return;
  }
  WaitForEvent();
}

void PerformanceHttpServer::Impl::Connection::OnReady() {
  stream_ready_ = true;
  if (!closed_ && response_.has_value() && response_->stream_source) {
    NextEvent();
  }
}

void PerformanceHttpServer::Impl::Connection::WaitForEvent() {
  if (stream_ready_) {
    NextEvent();
    return;
  }
  heartbeat_.expires_after(core::http::kSseHeartbeatInterval);
  heartbeat_.async_wait([self = shared_from_this()](asio::error_code error) {
    // An expired completion can already be queued when cancellation runs.
    // Never emit a stale heartbeat after stream completion or during reuse.
    if (!error && !self->closed_ && !self->job_busy_ && self->output_.empty() && self->response_.has_value() &&
        self->response_->stream_source && !self->stream_finished_) {
      self->QueueOutput(self->close_after_response_ ? std::string(core::http::kSseHeartbeat)
                                                    : FrameChunk(core::http::kSseHeartbeat));
    }
  });
}

void PerformanceHttpServer::Impl::Connection::FinishResponse() {
  if (!response_.has_value()) {
    Close();
    return;
  }
  if (observer_ != nullptr) {
    observer_->FinishRequest(*response_, close_after_response_, handled_);
  }
  if (!response_->stream_source) {
    response_.reset();
    CompleteResponse();
    return;
  }
  // Cleanup is queued before any subsequent request on the same worker. It
  // need not make an extra round trip through the I/O loop before reuse.
  asio::post(worker_, [response = std::move(response_)]() mutable { response.reset(); });
  response_.reset();
  CompleteResponse();
}

void PerformanceHttpServer::Impl::Connection::CompleteResponse() {
  counted_ = false;
  owner_.CompleteRequest(reset_request_);
  response_active_ = false;
  stream_finished_ = false;
  ready_registered_ = false;
  if (closed_) {
    Dispose();
    return;
  }
  if (close_after_response_) {
    Close();
    return;
  }
  ProcessRequest();
  Read();
}

void PerformanceHttpServer::Impl::Connection::Close() {
  if (!closed_) {
    closed_ = true;
    asio::error_code ignored;
    socket_.close(ignored);
    heartbeat_.cancel();
    std::erase(owner_.ready_requests_, shared_from_this());
    owner_.PumpRequests();
  }
  if (!job_busy_) {
    Dispose();
  }
}

void PerformanceHttpServer::Impl::Connection::Dispose() {
  // Socket buffers remain alive until canceled I/O handlers are destroyed.
  // Source cancellation and observer cleanup can synchronize with publishers
  // or diagnostic reset; run them on the application pool, never the I/O loop.
  job_busy_ = true;
  asio::post(worker_, [self = shared_from_this()] {
    self->response_.reset();
    self->observer_.reset();
    asio::post(self->owner_.io_, [self] {
      self->owner_.output_bytes_ -= self->output_.size();
      self->owner_.input_bytes_ -= self->input_.size() + self->parked_bytes_;
      self->owner_.connections_.erase(self);
      if (self->counted_) {
        self->counted_ = false;
        self->owner_.CompleteRequest(self->reset_request_);
      }
      self->owner_.Accept();
      self->guard_.reset();
    });
  });
}

bool PerformanceHttpServer::Impl::Start() {
  asio::error_code error;
  const auto address = asio::ip::make_address(host_, error);
  if (error) {
    return false;
  }
  const asio::ip::tcp::endpoint endpoint(address, static_cast<unsigned short>(port_));
  acceptor_.open(endpoint.protocol(), error);
  if (error) {
    return false;
  }
  acceptor_.set_option(asio::ip::tcp::acceptor::reuse_address(true), error);
  if (error) {
    return false;
  }
  acceptor_.bind(endpoint, error);
  if (error) {
    return false;
  }
  acceptor_.listen(asio::socket_base::max_listen_connections, error);
  if (error) {
    return false;
  }
  signals_.async_wait([this](asio::error_code signal_error, int signal_number) {
    (void)signal_number;
    if (!signal_error) {
      Shutdown();
    }
  });
  Accept();
  return true;
}

void PerformanceHttpServer::Impl::Accept() {
  if (stopping_ || accepting_ || connections_.size() >= options_.max_connections) {
    return;
  }
  accepting_ = true;
  acceptor_.async_accept([this](asio::error_code error, asio::ip::tcp::socket socket) {
    accepting_ = false;
    if (stopping_) {
      return;
    }
    if (error) {
      Shutdown();
      return;
    }
    auto connection = std::make_shared<Connection>(*this, std::move(socket));
    connections_.insert(connection);
    peak_connections_ = std::max(peak_connections_, connections_.size());
    connection->Start();
    Accept();
  });
}

void PerformanceHttpServer::Impl::PumpRequests() {
  while (!stopping_ && !reset_running_ && !ready_requests_.empty()) {
    auto connection = ready_requests_.front();
    if (connection->IsClosed()) {
      ready_requests_.pop_front();
      continue;
    }
    const bool reset = connection->IsReset();
    if (reset && active_requests_ != 0) {
      return;
    }
    ready_requests_.pop_front();
    ++active_requests_;
    reset_running_ = reset;
    connection->Dispatch();
  }
}

void PerformanceHttpServer::Impl::CompleteRequest(bool reset) {
  --active_requests_;
  if (reset) {
    reset_running_ = false;
  }
  PumpRequests();
}

void PerformanceHttpServer::Impl::Shutdown() {
  if (stopping_) {
    return;
  }
  stopping_ = true;
  asio::error_code ignored;
  acceptor_.close(ignored);
  signals_.cancel(ignored);
  for (const auto& connection : connections_) {
    connection->Close();
  }
  ready_requests_.clear();
}

void PerformanceHttpServer::Impl::EmitResources() const {
  std::cout << kResourcePrefix << " io_workers=1 application_workers=" << options_.application_workers
            << " max_connections=" << options_.max_connections << " peak_connections=" << peak_connections_
            << " max_input_bytes=" << options_.max_input_bytes << " max_output_bytes=" << options_.max_output_bytes
            << " max_global_input_bytes=" << options_.max_connections * options_.max_input_bytes
            << " max_global_output_bytes=" << options_.max_connections * options_.max_output_bytes
            << " peak_queued_input_bytes=" << peak_input_bytes_ << " peak_queued_output_bytes=" << peak_output_bytes_
            << '\n';
}

PerformanceHttpServer::PerformanceHttpServer(std::string_view host, int port, const server::TransportMux& mux,
                                             SutRuntimeObserver* observer, Options options)
    : impl_(std::make_unique<Impl>(host, port, mux, observer, options)) {}
PerformanceHttpServer::~PerformanceHttpServer() { impl_->JoinWorkers(); }
bool PerformanceHttpServer::Start() { return impl_->Start(); }
void PerformanceHttpServer::Run() { impl_->io_.run(); }
void PerformanceHttpServer::Stop() {
  asio::post(impl_->io_, [this] { impl_->Shutdown(); });
}
void PerformanceHttpServer::Join() {
  impl_->JoinWorkers();
  impl_->EmitResources();
}
PerformanceHttpServer::Options PerformanceHttpServer::OptionsFromEnvironment() {
  Options options;
  options.application_workers = ReadLimit(kWorkersEnv, options.application_workers, kMaximumWorkers);
  options.max_connections = ReadLimit(kConnectionsEnv, options.max_connections, kMaximumConnections);
  options.max_input_bytes = ReadLimit(kInputEnv, options.max_input_bytes, kMaximumBufferBytes);
  options.max_output_bytes = ReadLimit(kOutputEnv, options.max_output_bytes, kMaximumBufferBytes);
  options.write_slice_bytes = ReadLimit(kWriteSliceEnv, options.write_slice_bytes, kMaximumBufferBytes);
  return options;
}
}  // namespace a2a::tests::sut
