cmake_minimum_required(VERSION 3.25)

project(a2a_cpp_fetch_content_consumer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)

set(A2A_CPP_GIT_REPOSITORY "https://github.com/MisterVVP/a2a-cpp.git" CACHE STRING "a2a-cpp Git repository")
set(A2A_CPP_GIT_TAG "main" CACHE STRING "a2a-cpp Git tag, branch, or commit")
set(A2A_EXAMPLE_APP "hello_agent" CACHE STRING "Example app to build")

set(A2A_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(A2A_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(A2A_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
set(A2A_ENABLE_POSTGRES_STORE OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
  a2a_cpp
  GIT_REPOSITORY "${A2A_CPP_GIT_REPOSITORY}"
  GIT_TAG "${A2A_CPP_GIT_TAG}"
)
FetchContent_MakeAvailable(a2a_cpp)

set(A2A_EXAMPLE_APP_DIR "${CMAKE_CURRENT_LIST_DIR}/../apps/${A2A_EXAMPLE_APP}")
set(A2A_EXAMPLE_APP_SOURCE "${A2A_EXAMPLE_APP_DIR}/main.cpp")
if(NOT EXISTS "${A2A_EXAMPLE_APP_SOURCE}")
  message(FATAL_ERROR "Unknown A2A_EXAMPLE_APP='${A2A_EXAMPLE_APP}'. Expected ${A2A_EXAMPLE_APP_SOURCE} to exist.")
endif()

add_executable(a2a_example "${A2A_EXAMPLE_APP_SOURCE}")
target_link_libraries(a2a_example PRIVATE a2a::client a2a::server a2a::core)

if(A2A_EXAMPLE_APP STREQUAL "asio_executor" OR A2A_EXAMPLE_APP STREQUAL "blocking_executor")
  if(NOT TARGET a2a::asio)
    message(FATAL_ERROR "This coroutine example requires an SDK built with A2A_ENABLE_ASIO=ON")
  endif()
  target_link_libraries(a2a_example PRIVATE a2a::asio)
endif()

if(A2A_EXAMPLE_APP STREQUAL "async_grpc_server" AND TARGET a2a::asio)
  target_link_libraries(a2a_example PRIVATE a2a::asio)
endif()
