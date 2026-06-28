cmake_minimum_required(VERSION 3.25)

project(a2a_cpp_fetch_content_consumer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)

set(A2A_CPP_GIT_REPOSITORY "https://github.com/MisterVVP/a2a-cpp.git" CACHE STRING "a2a-cpp Git repository")
set(A2A_CPP_GIT_TAG "main" CACHE STRING "a2a-cpp Git tag, branch, or commit")

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

add_executable(a2a_cpp_fetch_content_consumer main.cpp)
target_link_libraries(a2a_cpp_fetch_content_consumer
  PRIVATE
    a2a::client
    a2a::server
    a2a::core
)
