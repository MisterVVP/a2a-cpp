cmake_minimum_required(VERSION 3.25)

project(a2a_cpp VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(A2A_ENABLE_TESTING "Build tests" ON)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  add_compile_options(-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion)
endif()

find_package(Protobuf REQUIRED)
find_package(gRPC REQUIRED)

add_subdirectory(proto)
add_subdirectory(src)

if(A2A_ENABLE_TESTING)
  include(CTest)
  enable_testing()
  add_subdirectory(tests)
endif()
