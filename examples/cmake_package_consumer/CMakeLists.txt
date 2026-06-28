cmake_minimum_required(VERSION 3.25)

project(a2a_cpp_package_consumer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(a2a_cpp CONFIG REQUIRED)

add_executable(a2a_cpp_package_consumer main.cpp)
target_link_libraries(a2a_cpp_package_consumer
  PRIVATE
    a2a::client
    a2a::server
    a2a::core
)
