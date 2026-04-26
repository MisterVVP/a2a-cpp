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

add_executable(core_protojson_golden_test
  unit/core_protojson_golden_test.cpp
)

target_link_libraries(core_protojson_golden_test
  PRIVATE
    a2a::core
    a2a::proto_generated
    GTest::gtest_main
)

target_compile_definitions(core_protojson_golden_test
  PRIVATE
    A2A_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)

gtest_discover_tests(core_protojson_golden_test)

add_executable(examples_support_test
  unit/examples_support_test.cpp
)

target_include_directories(examples_support_test
  PRIVATE
    ${CMAKE_SOURCE_DIR}/examples
)

target_link_libraries(examples_support_test
  PRIVATE
    a2a::server
    a2a::core
    a2a::proto_generated
    GTest::gtest_main
)

gtest_discover_tests(examples_support_test)

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

add_executable(sse_parser_test
  unit/sse_parser_test.cpp
)

target_link_libraries(sse_parser_test
  PRIVATE
    a2a::client
    a2a::core
    a2a::proto_generated
    GTest::gtest_main
)

gtest_discover_tests(sse_parser_test)

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

add_executable(http_json_streaming_integration_test
  integration/http_json_streaming_integration_test.cpp
)

target_link_libraries(http_json_streaming_integration_test
  PRIVATE
    a2a::client
    a2a::core
    a2a::proto_generated
    GTest::gtest_main
)

gtest_discover_tests(http_json_streaming_integration_test)

add_executable(json_rpc_transport_test
  unit/json_rpc_transport_test.cpp
)

target_link_libraries(json_rpc_transport_test
  PRIVATE
    a2a::client
    a2a::core
    a2a::proto_generated
    GTest::gtest_main
)

gtest_discover_tests(json_rpc_transport_test)

add_executable(grpc_transport_test
  unit/grpc_transport_test.cpp
)

target_link_libraries(grpc_transport_test
  PRIVATE
    a2a::client
    a2a::core
    a2a::proto_generated
    GTest::gtest_main
)

gtest_discover_tests(grpc_transport_test)

add_executable(server_dispatcher_test
  unit/server_dispatcher_test.cpp
)

target_link_libraries(server_dispatcher_test
  PRIVATE
    a2a::server
    a2a::core
    a2a::proto_generated
    GTest::gtest_main
)

gtest_discover_tests(server_dispatcher_test)

add_executable(rest_transport_test
  unit/rest_transport_test.cpp
)

target_link_libraries(rest_transport_test
  PRIVATE
    a2a::server
    a2a::core
    a2a::proto_generated
    GTest::gtest_main
)

gtest_discover_tests(rest_transport_test)

add_executable(rest_server_transport_test
  unit/rest_server_transport_test.cpp
)

target_link_libraries(rest_server_transport_test
  PRIVATE
    a2a::server
    a2a::core
    a2a::proto_generated
    GTest::gtest_main
)

gtest_discover_tests(rest_server_transport_test)


add_executable(json_rpc_server_transport_test
  unit/json_rpc_server_transport_test.cpp
)

target_link_libraries(json_rpc_server_transport_test
  PRIVATE
    a2a::server
    a2a::core
    a2a::proto_generated
    GTest::gtest_main
)

gtest_discover_tests(json_rpc_server_transport_test)

add_executable(grpc_server_transport_test
  unit/grpc_server_transport_test.cpp
)

target_link_libraries(grpc_server_transport_test
  PRIVATE
    a2a::server
    a2a::core
    a2a::proto_generated
    GTest::gtest_main
)

gtest_discover_tests(grpc_server_transport_test)

add_executable(json_rpc_client_functional_test
  functional/json_rpc_client_functional_test.cpp
)

target_link_libraries(json_rpc_client_functional_test
  PRIVATE
    a2a::client
    a2a::core
    a2a::proto_generated
    GTest::gtest_main
)

gtest_discover_tests(json_rpc_client_functional_test)

add_executable(grpc_client_functional_test
  functional/grpc_client_functional_test.cpp
)

target_link_libraries(grpc_client_functional_test
  PRIVATE
    a2a::client
    a2a::core
    a2a::proto_generated
    GTest::gtest_main
)

gtest_discover_tests(grpc_client_functional_test)

add_executable(server_task_store_functional_test
  functional/server_task_store_functional_test.cpp
)

target_link_libraries(server_task_store_functional_test
  PRIVATE
    a2a::server
    a2a::core
    a2a::proto_generated
    GTest::gtest_main
)

gtest_discover_tests(server_task_store_functional_test)

add_executable(examples_functional_test
  functional/examples_functional_test.cpp
)

target_include_directories(examples_functional_test
  PRIVATE
    ${CMAKE_SOURCE_DIR}/examples
)

target_link_libraries(examples_functional_test
  PRIVATE
    a2a::server
    a2a::core
    a2a::proto_generated
    GTest::gtest_main
)

gtest_discover_tests(examples_functional_test)

add_executable(rest_server_transport_functional_test
  functional/rest_server_transport_functional_test.cpp
)

target_link_libraries(rest_server_transport_functional_test
  PRIVATE
    a2a::server
    a2a::core
    a2a::proto_generated
    GTest::gtest_main
)

gtest_discover_tests(rest_server_transport_functional_test)


add_executable(json_rpc_server_transport_functional_test
  functional/json_rpc_server_transport_functional_test.cpp
)

target_link_libraries(json_rpc_server_transport_functional_test
  PRIVATE
    a2a::server
    a2a::core
    a2a::proto_generated
    GTest::gtest_main
)

gtest_discover_tests(json_rpc_server_transport_functional_test)

add_executable(json_rpc_client_integration_test
  integration/json_rpc_client_integration_test.cpp
)

target_link_libraries(json_rpc_client_integration_test
  PRIVATE
    a2a::client
    a2a::core
    a2a::proto_generated
    GTest::gtest_main
)

gtest_discover_tests(json_rpc_client_integration_test)


add_executable(json_rpc_server_transport_integration_test
  integration/json_rpc_server_transport_integration_test.cpp
)

target_link_libraries(json_rpc_server_transport_integration_test
  PRIVATE
    a2a::server
    a2a::client
    a2a::core
    a2a::proto_generated
    GTest::gtest_main
)

gtest_discover_tests(json_rpc_server_transport_integration_test)

add_executable(server_dispatcher_task_store_integration_test
  integration/server_dispatcher_task_store_integration_test.cpp
)

target_link_libraries(server_dispatcher_task_store_integration_test
  PRIVATE
    a2a::server
    a2a::core
    a2a::proto_generated
    GTest::gtest_main
)

gtest_discover_tests(server_dispatcher_task_store_integration_test)

add_executable(rest_server_transport_integration_test
  integration/rest_server_transport_integration_test.cpp
)

target_link_libraries(rest_server_transport_integration_test
  PRIVATE
    a2a::server
    a2a::client
    a2a::core
    a2a::proto_generated
    GTest::gtest_main
)

gtest_discover_tests(rest_server_transport_integration_test)

add_executable(grpc_transport_integration_test
  integration/grpc_transport_integration_test.cpp
)

target_link_libraries(grpc_transport_integration_test
  PRIVATE
    a2a::server
    a2a::client
    a2a::core
    a2a::proto_generated
    GTest::gtest_main
)

gtest_discover_tests(grpc_transport_integration_test)

add_executable(examples_interop_integration_test
  integration/examples_interop_integration_test.cpp
)

target_include_directories(examples_interop_integration_test
  PRIVATE
    ${CMAKE_SOURCE_DIR}/examples
)

target_link_libraries(examples_interop_integration_test
  PRIVATE
    a2a::server
    a2a::client
    a2a::core
    a2a::proto_generated
    GTest::gtest_main
)

gtest_discover_tests(examples_interop_integration_test)
