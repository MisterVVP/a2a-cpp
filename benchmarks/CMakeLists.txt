include(FetchContent)

option(A2A_BENCHMARK_USE_SYSTEM_BENCHMARK "Use an installed Google Benchmark package instead of FetchContent" ON)

set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_WERROR OFF CACHE BOOL "" FORCE)

if(A2A_BENCHMARK_USE_SYSTEM_BENCHMARK)
  find_package(benchmark CONFIG QUIET)
endif()

if(NOT benchmark_FOUND)
  FetchContent_Declare(
    google_benchmark
    URL https://github.com/google/benchmark/archive/refs/tags/v1.9.1.tar.gz
    URL_HASH SHA256=32131c08ee31eeff2c8968d7e874f3cb648034377dfc32a4c377fa8796d84981
  )
  FetchContent_MakeAvailable(google_benchmark)
endif()

add_executable(a2a_benchmarks
  benchmark_main.cpp
  bench_agent_card.cpp
  bench_http_adapter.cpp
  bench_json_rpc_transport.cpp
  bench_protojson.cpp
  bench_push_notifications.cpp
  bench_rest_transport.cpp
  bench_task_lifecycle.cpp
  bench_task_store.cpp
  bench_transport_mux.cpp
  bench_uuidv7_task_id.cpp
)

target_link_libraries(a2a_benchmarks
  PRIVATE
    a2a::core
    a2a::server
    benchmark::benchmark
)

target_compile_definitions(a2a_benchmarks PRIVATE A2A_BENCHMARK_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
