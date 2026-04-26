cmake_minimum_required(VERSION 3.25)

project(a2a_cpp VERSION 0.1.0 LANGUAGES CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(A2A_ENABLE_TESTING "Build tests" ON)
option(A2A_BUILD_EXAMPLES "Build example binaries" ON)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  add_compile_options(-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion)
endif()

find_package(Protobuf REQUIRED)
find_package(gRPC REQUIRED)

add_subdirectory(proto)
add_subdirectory(src)

if(A2A_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()

if(A2A_ENABLE_TESTING)
  include(CTest)
  enable_testing()
  add_subdirectory(tests)
endif()

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(DIRECTORY ${CMAKE_BINARY_DIR}/generated/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(EXPORT a2a_cppTargets
  FILE a2a_cppTargets.cmake
  NAMESPACE a2a::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/a2a_cpp
)

configure_package_config_file(
  cmake/a2a_cppConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/a2a_cppConfig.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/a2a_cpp
)

write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/a2a_cppConfigVersion.cmake
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion
)

install(FILES
  ${CMAKE_CURRENT_BINARY_DIR}/a2a_cppConfig.cmake
  ${CMAKE_CURRENT_BINARY_DIR}/a2a_cppConfigVersion.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/a2a_cpp
)
