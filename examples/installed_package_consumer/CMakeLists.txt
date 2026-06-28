cmake_minimum_required(VERSION 3.25)

project(a2a_cpp_installed_package_consumer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(a2a_cpp CONFIG REQUIRED)

set(A2A_EXAMPLE_APP "hello_agent" CACHE STRING "Example app to build")
set(A2A_EXAMPLE_APP_DIR "${CMAKE_CURRENT_LIST_DIR}/../apps/${A2A_EXAMPLE_APP}")
set(A2A_EXAMPLE_APP_SOURCE "${A2A_EXAMPLE_APP_DIR}/main.cpp")
if(NOT EXISTS "${A2A_EXAMPLE_APP_SOURCE}")
  message(FATAL_ERROR "Unknown A2A_EXAMPLE_APP='${A2A_EXAMPLE_APP}'. Expected ${A2A_EXAMPLE_APP_SOURCE} to exist.")
endif()

add_executable(a2a_example "${A2A_EXAMPLE_APP_SOURCE}")
target_link_libraries(a2a_example PRIVATE a2a::client a2a::server a2a::core)
