// SPDX-License-Identifier: Apache-2.0

#include <benchmark/benchmark.h>

#include <cstddef>
#include <string>
#include <string_view>

#include "a2a/server/transport_mux.h"
#include "bench_common.h"

namespace {

a2a::server::TransportMux BuildMux(std::size_t route_count) {
  a2a::server::TransportMux mux;
  for (std::size_t index = 0; index < route_count; ++index) {
    const std::string path = "/route/" + std::to_string(index);
    mux.RegisterRoute(
        {.name = "route-" + std::to_string(index),
         .matcher = [path](std::string_view method,
                           std::string_view request_path) { return method == "GET" && request_path == path; },
         .handler =
             [](const a2a::server::HttpServerRequest& request) {
               (void)request;
               return a2a::server::HttpServerResponse{.status_code = 200, .headers = {}, .body = "ok"};
             },
         .priority = static_cast<int>(index)});
  }
  return mux;
}

void BM_TransportMux_RouteJsonRpc(benchmark::State& state) {
  auto mux = BuildMux(2);
  const auto request = a2a::bench::BuildHttpRequest("GET", "/route/0");
  for (auto _ : state) {
    auto response = mux.RouteRequest(request);
    benchmark::DoNotOptimize(response);
  }
}
BENCHMARK(BM_TransportMux_RouteJsonRpc);

void BM_TransportMux_RouteRest(benchmark::State& state) {
  auto mux = BuildMux(2);
  const auto request = a2a::bench::BuildHttpRequest("GET", "/route/1");
  for (auto _ : state) {
    auto response = mux.RouteRequest(request);
    benchmark::DoNotOptimize(response);
  }
}
BENCHMARK(BM_TransportMux_RouteRest);

void BM_TransportMux_RouteRestWithQuery(benchmark::State& state) {
  auto mux = BuildMux(2);
  const auto request = a2a::bench::BuildHttpRequest("GET", "/route/1?historyLength=0");
  for (auto _ : state) {
    auto response = mux.RouteRequest(request);
    benchmark::DoNotOptimize(response);
  }
}
BENCHMARK(BM_TransportMux_RouteRestWithQuery);

void BM_TransportMux_RouteMiss(benchmark::State& state) {
  auto mux = BuildMux(2);
  const auto request = a2a::bench::BuildHttpRequest("GET", "/missing");
  for (auto _ : state) {
    auto response = mux.RouteRequest(request);
    benchmark::DoNotOptimize(response);
  }
}
BENCHMARK(BM_TransportMux_RouteMiss);

void BM_TransportMux_MethodMismatch(benchmark::State& state) {
  auto mux = BuildMux(2);
  const auto request = a2a::bench::BuildHttpRequest("POST", "/route/1");
  for (auto _ : state) {
    auto response = mux.RouteRequest(request);
    benchmark::DoNotOptimize(response);
  }
}
BENCHMARK(BM_TransportMux_MethodMismatch);

void BM_TransportMux_Route100Routes(benchmark::State& state) {
  auto mux = BuildMux(100);
  const auto request = a2a::bench::BuildHttpRequest("GET", "/route/99");
  for (auto _ : state) {
    auto response = mux.RouteRequest(request);
    benchmark::DoNotOptimize(response);
  }
}
BENCHMARK(BM_TransportMux_Route100Routes);

}  // namespace
