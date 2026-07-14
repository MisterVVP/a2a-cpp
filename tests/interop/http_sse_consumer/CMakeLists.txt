cmake_minimum_required(VERSION 3.24)
project(a2a_http_sse_interop LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(A2A_ENABLE_TESTING ON CACHE BOOL "" FORCE)
set(A2A_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(A2A_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
set(A2A_ENABLE_POSTGRES_STORE OFF CACHE BOOL "" FORCE)

if(NOT DEFINED A2A_SOURCE_DIR)
  message(FATAL_ERROR "A2A_SOURCE_DIR must point to the a2a-cpp repository")
endif()

find_package(GTest CONFIG REQUIRED)

add_subdirectory("${A2A_SOURCE_DIR}" a2a-cpp-build)

add_executable(http_sse_interop_client
  "${A2A_SOURCE_DIR}/tests/interop/http_sse_interop_client.cpp"
)

target_link_libraries(http_sse_interop_client
  PRIVATE
    a2a::client
    a2a::core
    a2a::proto_generated
)

add_executable(http_streaming_gap_test
  "${A2A_SOURCE_DIR}/tests/interop/http_streaming_gap_test.cpp"
)

target_link_libraries(http_streaming_gap_test
  PRIVATE
    a2a::client
    a2a::core
    a2a::proto_generated
    GTest::gtest_main
)
