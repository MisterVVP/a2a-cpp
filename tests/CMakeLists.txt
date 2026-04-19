find_package(GTest CONFIG QUIET)
if(NOT GTest_FOUND)
  include(FetchContent)
  FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
  )
  # For Windows: Prevent overriding the parent project's compiler/linker settings.
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)
endif()

include(GoogleTest)

add_executable(generated_proto_smoke_test
  generated_proto_smoke_test.cpp
)

target_link_libraries(generated_proto_smoke_test
  PRIVATE
    a2a::proto_generated
    GTest::gtest_main
)

gtest_discover_tests(generated_proto_smoke_test)

add_executable(core_helpers_test
  unit/core_helpers_test.cpp
)

target_link_libraries(core_helpers_test
  PRIVATE
    a2a::core
    a2a::proto_generated
    GTest::gtest_main
)

gtest_discover_tests(core_helpers_test)

add_executable(discovery_test
  unit/discovery_test.cpp
)

target_link_libraries(discovery_test
  PRIVATE
    a2a::client
    a2a::core
    a2a::proto_generated
    GTest::gtest_main
)

target_compile_definitions(discovery_test
  PRIVATE
    A2A_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)

gtest_discover_tests(discovery_test)

add_executable(client_test
  unit/client_test.cpp
)

target_link_libraries(client_test
  PRIVATE
    a2a::client
    a2a::core
    a2a::proto_generated
    GTest::gtest_main
)

gtest_discover_tests(client_test)

add_executable(http_json_client_integration_test
  integration/http_json_client_integration_test.cpp
)

target_link_libraries(http_json_client_integration_test
  PRIVATE
    a2a::client
    a2a::core
    a2a::proto_generated
    GTest::gtest_main
)

gtest_discover_tests(http_json_client_integration_test)
