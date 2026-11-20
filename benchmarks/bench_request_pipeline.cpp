// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include <benchmark/benchmark.h>

#include <boost/asio/buffer.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/thread_pool.hpp>
#include <boost/asio/write.hpp>
#include <boost/system/system_error.hpp>
#include <chrono>
#include <cstddef>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include "a2a/core/extensions.h"
#include "a2a/core/http_constants.h"
#include "a2a/core/version.h"
#include "a2a/server/dispatcher.h"
#include "a2a/server/engine/async_dispatcher.h"
#include "a2a/server/http_adapter.h"
#include "a2a/server/transport_mux.h"
#include "server/engine/dispatch_pipeline.h"
#include "sut/pipeline_read_fixture.h"
#include "sut/tck_sut.h"

namespace {
namespace asio = boost::asio;
namespace http = a2a::core::http;
using namespace a2a::server;
using namespace a2a::server::engine;
using namespace a2a::tests::sut;
using a2a::core::Extensions;
using a2a::core::Version;
using Clock = std::chrono::steady_clock;
using Tcp = asio::ip::tcp;
using HttpDispatch = PreparedDispatch<HttpServerResponse>;
using DispatchResult = a2a::core::Result<DispatchResponse>;
constexpr std::size_t kWorkerCount = 1;
constexpr auto kLoopback = "127.0.0.1";
constexpr auto kRestTaskPrefix = "/a2a/tasks/";
constexpr auto kJsonRpcPrefix = R"({"jsonrpc":"2.0","id":1,"method":"GetTask","params":{"id":")";
constexpr auto kJsonRpcSuffix = R"("}})";
constexpr auto kInvalidFixture = "Invalid pipeline benchmark request or response";
constexpr auto kStartupCounter = "startup_ns";
constexpr auto kCompletionCounter = "dispatcher_completion_ns";

std::string RequestBytes(bool jsonrpc) {
  const std::string body = jsonrpc ? std::string(kJsonRpcPrefix).append(kPipelineTaskId).append(kJsonRpcSuffix) : "";
  std::string bytes;
  bytes.append(jsonrpc ? http::kMethodPost : http::kMethodGet).append(" ");
  if (jsonrpc) {
    bytes.append(kJsonRpcPath);
  } else {
    bytes.append(kRestTaskPrefix).append(kPipelineTaskId);
  }
  bytes.append(" ").append(http::kHttpVersion11).append(http::kLineTerminator);
  const auto header = [&bytes](std::string_view name, std::string_view value) {
    bytes.append(name).append(http::kHeaderNameValueSeparator).append(value).append(http::kLineTerminator);
  };
  header(http::kHostHeaderName, kLoopback);
  header(Version::kHeaderName, Version::kProtocolVersion);
  header(Extensions::kHeaderName, kRequiredExtensionUri);
  if (jsonrpc) {
    header(http::kContentTypeHeaderName, http::kContentTypeApplicationJson);
    header(http::kContentLengthHeaderName, std::to_string(body.size()));
  }
  return bytes.append(http::kLineTerminator).append(body);
}

class PipelineInput final {
 public:
  explicit PipelineInput(bool jsonrpc)
      : jsonrpc_(jsonrpc),
        bytes(RequestBytes(jsonrpc)),
        request(Parse()),
        dispatch(Prepare()),
        query(std::get<lf::a2a::v1::GetTaskRequest>(dispatch.request.payload)),
        task(executor->ReadTask(query).value()),
        response(Serialize()),
        framed(HttpAdapter::EncodeResponse(response).value()) {}

  HttpServerRequest Parse() const {
    std::size_t consumed = 0;
    auto parsed = HttpAdapter().ParseRequest(bytes, consumed, kLoopback);
    if (!parsed.ok() || consumed != bytes.size()) {
      throw std::runtime_error(kInvalidFixture);
    }
    auto& request = parsed.value();
    if (!request.has_value()) {
      throw std::runtime_error(kInvalidFixture);
    }
    return std::move(*request);
  }

  HttpDispatch Prepare() const {
    auto prepared = jsonrpc_ ? rpc_.Prepare(request) : rest_.Prepare(request);
    if (!prepared.ok() || !std::holds_alternative<HttpDispatch>(prepared.value())) {
      throw std::runtime_error(kInvalidFixture);
    }
    return std::get<HttpDispatch>(std::move(prepared.value()));
  }

 private:
  HttpServerResponse Serialize() const {
    auto plan = Prepare();
    auto result = plan.complete(DispatchResponse(task));
    if (!result.ok() || result.value().status_code != http::kStatusOk) {
      throw std::runtime_error(kInvalidFixture);
    }
    return std::move(result.value());
  }

  bool jsonrpc_;
  RestServerTransport rest_{nullptr,
                            {},
                            {.rest_api_base_path = std::string(kRestApiBasePath),
                             .include_legacy_transport_fields = false,
                             .agent_card_cache_settings = {},
                             .required_extensions = {std::string(kRequiredExtensionUri)}}};
  JsonRpcServerTransport rpc_{
      nullptr, {.rpc_path = std::string(kJsonRpcPath), .required_extensions = {std::string(kRequiredExtensionUri)}}};

 public:
  const std::shared_ptr<PipelineReadFixture> executor = std::make_shared<PipelineReadFixture>();
  const std::string bytes;
  const HttpServerRequest request;
  const HttpDispatch dispatch;
  const lf::a2a::v1::GetTaskRequest query;
  const lf::a2a::v1::Task task;
  const HttpServerResponse response;
  const std::string framed;
};

void BM_RequestFraming(benchmark::State& state, const PipelineInput& input) {
  while (state.KeepRunning()) {
    auto request = input.Parse();
    benchmark::DoNotOptimize(request);
  }
}

template <bool Async>
void BM_Routing(benchmark::State& state, const PipelineInput& input) {
  const auto handler = [](const HttpServerRequest& /*request*/) {
    HttpServerResponse response;
    response.status_code = http::kStatusOk;
    return response;
  };
  TransportMux mux;
  const TransportMux::JsonRpcRouteOptions rpc;
  const TransportMux::RestRouteOptions rest;
  mux.RegisterRoute({.name = rpc.route_name,
                     .matcher = [&rpc](std::string_view method,
                                       std::string_view path) { return method == rpc.method && path == rpc.rpc_path; },
                     .handler = handler,
                     .priority = rpc.priority});
  mux.RegisterRoute({.name = rest.route_name,
                     .matcher = [&rest](std::string_view /*method*/,
                                        std::string_view path) { return path.starts_with(rest.rest_api_prefix); },
                     .handler = handler,
                     .priority = rest.priority});
  while (state.KeepRunning()) {
    if constexpr (Async) {
      auto routed = mux.PrepareRequest(input.request);
      benchmark::DoNotOptimize(routed);
    } else {
      auto routed = mux.RouteRequest(input.request);
      benchmark::DoNotOptimize(routed);
    }
  }
}

void BM_Validation(benchmark::State& state, const PipelineInput& input) {
  while (state.KeepRunning()) {
    auto plan = input.Prepare();
    benchmark::DoNotOptimize(plan);
  }
}

void BM_Interceptors(benchmark::State& state, const PipelineInput& input) {
  const DispatchPipeline pipeline{
      .security = {}, .interceptors = {std::make_shared<PipelinePolicy>()}, .agent_card_provider = {}};
  const DispatchResult result(DispatchResponse(input.task));
  auto context = input.dispatch.context;
  while (state.KeepRunning()) {
    DispatchSession session(pipeline, input.dispatch.request, context);
    auto before = session.Before();
    session.After(result);
    benchmark::DoNotOptimize(before);
  }
}

void BM_TaskStore(benchmark::State& state, const PipelineInput& input) {
  while (state.KeepRunning()) {
    auto result = input.executor->ReadTask(input.query);
    benchmark::DoNotOptimize(result);
  }
}

class DispatchTiming final : public PipelinePolicy {
 public:
  a2a::core::Result<void> BeforeDispatch(const DispatchRequest& request, RequestContext& context) override {
    before = Clock::now();
    return PipelinePolicy::BeforeDispatch(request, context);
  }

  void AfterDispatch(const DispatchRequest& /*request*/, RequestContext& /*context*/,
                     const DispatchResult& /*result*/) override {
    after = Clock::now();
  }

  Clock::time_point before;
  Clock::time_point after;
};

template <bool Async, bool SharedIngress = false>
void BM_Dispatch(benchmark::State& state, const PipelineInput& input) {
  auto interceptor = std::make_shared<DispatchTiming>();
  asio::thread_pool workers(kWorkerCount);
  Dispatcher sync(input.executor.get(), {interceptor});
  AsyncDispatcherOptions options;
  options.interceptors = {interceptor};
  AsyncDispatcher async(input.executor, workers.get_executor(), std::move(options));
  Clock::duration startup{};
  Clock::duration completion{};
  while (state.KeepRunning()) {
    auto context = input.dispatch.context;
    Clock::time_point start;
    Clock::time_point end;
    bool ok = false;
    if constexpr (Async) {
      auto delivered = std::make_shared<std::promise<std::pair<Clock::time_point, bool>>>();
      auto reply = delivered->get_future();
      auto submit = [&async, &input, &start, context = std::move(context), delivered]() mutable {
        start = Clock::now();
        const auto admitted =
            async.Dispatch(input.dispatch.request, std::move(context),
                           [delivered](DispatchResult result, const RequestExecutionContext& /*context*/) {
                             delivered->set_value({Clock::now(), result.ok()});
                           });
        if (!admitted.ok()) {
          delivered->set_value({Clock::now(), false});
        }
      };
      if constexpr (SharedIngress) {
        asio::post(workers, std::move(submit));
      } else {
        submit();
      }
      const auto result = reply.get();
      end = result.first;
      ok = result.second;
    } else {
      start = Clock::now();
      const auto result = sync.Dispatch(input.dispatch.request, context);
      end = Clock::now();
      ok = result.ok();
    }
    if (!ok) {
      state.SkipWithError(kInvalidFixture);
      break;
    }
    startup += interceptor->before - start;
    completion += end - interceptor->after;
    state.SetIterationTime(std::chrono::duration<double>(end - start).count());
  }
  using Nanoseconds = std::chrono::duration<double, std::nano>;
  state.counters[kStartupCounter] = {Nanoseconds(startup).count(), benchmark::Counter::kAvgIterations};
  state.counters[kCompletionCounter] = {Nanoseconds(completion).count(), benchmark::Counter::kAvgIterations};
}

void BM_Serialization(benchmark::State& state, const PipelineInput& input) {
  while (state.KeepRunning()) {
    auto plan = input.Prepare();
    DispatchResult value{DispatchResponse(input.task)};
    const auto start = Clock::now();
    auto result = plan.complete(std::move(value));
    state.SetIterationTime(std::chrono::duration<double>(Clock::now() - start).count());
    benchmark::DoNotOptimize(result);
  }
}

void BM_ResponseFraming(benchmark::State& state, const PipelineInput& input) {
  while (state.KeepRunning()) {
    auto result = HttpAdapter::EncodeResponse(input.response);
    benchmark::DoNotOptimize(result);
  }
}

template <bool Async>
void BM_ResponseCompletion(benchmark::State& state, const PipelineInput& input) {
  asio::io_context receiver;
  asio::thread_pool network(kWorkerCount);
  const Tcp::endpoint local(asio::ip::make_address_v4(kLoopback), 0);
  Tcp::acceptor acceptor(receiver, local);
  Tcp::socket writer(network);
  writer.connect(acceptor.local_endpoint());
  auto reader = acceptor.accept();
  writer.set_option(Tcp::no_delay(true));
  const auto buffer = asio::buffer(input.framed);
  std::string bytes(input.framed.size(), '\0');
  while (state.KeepRunning()) {
    Clock::time_point start;
    Clock::time_point end;
    if constexpr (Async) {
      auto completed = std::make_shared<std::promise<Clock::time_point>>();
      auto reply = completed->get_future();
      start = Clock::now();
      asio::post(network, [&writer, buffer, completed] {
        asio::async_write(writer, buffer, [completed](const boost::system::error_code& error, std::size_t /*written*/) {
          if (error) {
            completed->set_exception(std::make_exception_ptr(boost::system::system_error(error)));
          } else {
            completed->set_value(Clock::now());
          }
        });
      });
      end = reply.get();
    } else {
      start = Clock::now();
      asio::write(writer, buffer);
      end = Clock::now();
    }
    asio::read(reader, asio::buffer(bytes));
    if (bytes != input.framed) {
      state.SkipWithError(kInvalidFixture);
      break;
    }
    state.SetIterationTime(std::chrono::duration<double>(end - start).count());
  }
}

auto* RegisterCost(const char* stage, bool jsonrpc, void (*measure)(benchmark::State&, const PipelineInput&)) {
  constexpr auto kPrefix = "RequestPipeline/";
  constexpr auto kJsonRpc = "jsonrpc/";
  constexpr auto kRest = "rest/";
  const std::string name = std::string(kPrefix).append(jsonrpc ? kJsonRpc : kRest).append(stage);
  return benchmark::RegisterBenchmark(name, [=](benchmark::State& state) { measure(state, PipelineInput(jsonrpc)); });
}

[[maybe_unused]] const bool kRegistered = [] {
  for (const bool jsonrpc : {false, true}) {
    RegisterCost("RequestFraming/shared", jsonrpc, BM_RequestFraming)->UseRealTime();
    RegisterCost("Routing/sync", jsonrpc, BM_Routing<false>)->UseRealTime();
    RegisterCost("Routing/async", jsonrpc, BM_Routing<true>)->UseRealTime();
    RegisterCost("Validation/shared", jsonrpc, BM_Validation)->UseRealTime();
    RegisterCost("Interceptors/shared", jsonrpc, BM_Interceptors)->UseRealTime();
    RegisterCost("TaskStore/shared", jsonrpc, BM_TaskStore)->UseRealTime();
    RegisterCost("Dispatch/sync", jsonrpc, BM_Dispatch<false>)->UseManualTime();
    RegisterCost("Dispatch/async-separate", jsonrpc, BM_Dispatch<true>)->UseManualTime();
    RegisterCost("Dispatch/async-shared", jsonrpc, BM_Dispatch<true, true>)->UseManualTime();
    RegisterCost("Serialization/shared", jsonrpc, BM_Serialization)->UseManualTime();
    RegisterCost("ResponseFraming/shared", jsonrpc, BM_ResponseFraming)->UseRealTime();
    RegisterCost("ResponseCompletion/sync", jsonrpc, BM_ResponseCompletion<false>)->UseManualTime();
    RegisterCost("ResponseCompletion/async", jsonrpc, BM_ResponseCompletion<true>)->UseManualTime();
  }
  return true;
}();
}  // namespace
