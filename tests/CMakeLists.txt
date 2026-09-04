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

add_executable(agent_card_builder_test
  unit/agent_card_builder_test.cpp
)

target_link_libraries(agent_card_builder_test
  PRIVATE
    a2a::core
    a2a::proto_generated
    GTest::gtest_main
)

gtest_discover_tests(agent_card_builder_test)

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

if(A2A_ENABLE_SUBSCRIPTION_DIAGNOSTICS)
  add_executable(subscription_diagnostics_test unit/subscription_diagnostics_test.cpp)
  target_include_directories(subscription_diagnostics_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
  target_compile_definitions(subscription_diagnostics_test PRIVATE A2A_ENABLE_SUBSCRIPTION_DIAGNOSTICS=1)
  target_link_libraries(subscription_diagnostics_test PRIVATE a2a::core GTest::gtest_main)
  gtest_discover_tests(subscription_diagnostics_test)
endif()

add_executable(json_value_test
  unit/json_value_test.cpp
)

target_link_libraries(json_value_test
  PRIVATE
    a2a::core
    GTest::gtest_main
)

gtest_discover_tests(json_value_test)

add_executable(non_copyable_test
  unit/non_copyable_test.cpp
)

target_link_libraries(non_copyable_test
  PRIVATE
    a2a::core
    GTest::gtest_main
)

gtest_discover_tests(non_copyable_test)

add_executable(response_builders_test
  unit/response_builders_test.cpp
)

target_link_libraries(response_builders_test
  PRIVATE
    a2a::core
    a2a::proto_generated
    GTest::gtest_main
)

gtest_discover_tests(response_builders_test)

add_executable(http_server_response_builder_test
  unit/http_server_response_builder_test.cpp
)

target_link_libraries(http_server_response_builder_test
  PRIVATE
    a2a::server
    GTest::gtest_main
)

gtest_discover_tests(http_server_response_builder_test)

add_executable(url_utils_test
  unit/url_utils_test.cpp
)

target_link_libraries(url_utils_test
  PRIVATE
    a2a::core
    GTest::gtest_main
)

gtest_discover_tests(url_utils_test)

add_executable(sql_identifier_test
  unit/sql_identifier_test.cpp
)

target_link_libraries(sql_identifier_test
  PRIVATE
    a2a::server
    GTest::gtest_main
)

gtest_discover_tests(sql_identifier_test)

add_executable(http_client_test
  unit/http_client_test.cpp
)

target_link_libraries(http_client_test
  PRIVATE
    a2a::client
    a2a::http
    a2a::core
    a2a::proto_generated
    GTest::gtest_main
)

gtest_discover_tests(http_client_test)

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
    ${CMAKE_SOURCE_DIR}/tests/support/example_support
)

target_link_libraries(examples_support_test
  PRIVATE
    a2a::server
    a2a::core
    a2a::proto_generated
    GTest::gtest_main
)

gtest_discover_tests(examples_support_test)

add_executable(task_id_generator_test
  unit/task_id_generator_test.cpp
)

target_link_libraries(task_id_generator_test
  PRIVATE
    a2a::server
    a2a::core
    a2a::proto_generated
    GTest::gtest_main
)

gtest_discover_tests(task_id_generator_test)

add_executable(task_subscription_service_test
  unit/task_subscription_service_test.cpp
)

target_link_libraries(task_subscription_service_test
  PRIVATE
    a2a::server
    a2a::core
    a2a::proto_generated
    GTest::gtest_main
)

gtest_discover_tests(task_subscription_service_test)

add_executable(store_factory_test
  unit/store_factory_test.cpp
)

target_link_libraries(store_factory_test
  PRIVATE
    a2a::server
    a2a::core
    a2a::proto_generated
    GTest::gtest_main
)

if(A2A_ENABLE_POSTGRES_STORE)
  target_link_libraries(store_factory_test
    PRIVATE
      a2a::store_postgres
  )
  target_compile_definitions(store_factory_test
    PRIVATE
      A2A_ENABLE_POSTGRES_STORE=1
  )
endif()

gtest_discover_tests(store_factory_test)

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

add_executable(stream_handle_lifecycle_test
  unit/stream_handle_lifecycle_test.cpp
)

target_link_libraries(stream_handle_lifecycle_test
  PRIVATE
    a2a::client
    a2a::core
    a2a::proto_generated
    GTest::gtest_main
)

gtest_discover_tests(stream_handle_lifecycle_test)


add_executable(auth_test
  unit/auth_test.cpp
)

target_link_libraries(auth_test
  PRIVATE
    a2a::client
    a2a::core
    a2a::proto_generated
    GTest::gtest_main
)

gtest_discover_tests(auth_test)

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


add_executable(http_json_transport_test
  unit/http_json_transport_test.cpp
)

target_link_libraries(http_json_transport_test
  PRIVATE
    a2a::client
    a2a::core
    a2a::proto_generated
    GTest::gtest_main
)

gtest_discover_tests(http_json_transport_test)

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

add_executable(list_tasks_client_parsing_test
  unit/list_tasks_client_parsing_test.cpp
)

target_link_libraries(list_tasks_client_parsing_test
  PRIVATE
    a2a::client
    a2a::core
    a2a::proto_generated
    GTest::gtest_main
)

gtest_discover_tests(list_tasks_client_parsing_test)

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


add_executable(http_adapter_test
  unit/http_adapter_test.cpp
)

target_link_libraries(http_adapter_test
  PRIVATE
    a2a::server
    a2a::core
    a2a::proto_generated
    GTest::gtest_main
)

gtest_discover_tests(http_adapter_test)
add_executable(version_header_validator_test
  unit/version_header_validator_test.cpp
)

target_link_libraries(version_header_validator_test
  PRIVATE
    a2a::server
    GTest::gtest_main
)

gtest_discover_tests(version_header_validator_test)

add_executable(rest_server_transport_test
  unit/rest_server_transport_test.cpp
)

target_include_directories(rest_server_transport_test PRIVATE "${PROJECT_SOURCE_DIR}/src")

target_link_libraries(rest_server_transport_test
  PRIVATE
    a2a::server
    a2a::core
    a2a::proto_generated
    GTest::gtest_main
)

gtest_discover_tests(rest_server_transport_test)

add_executable(rest_push_notification_transport_test
  unit/rest_push_notification_transport_test.cpp
)

target_link_libraries(rest_push_notification_transport_test
  PRIVATE
    a2a::server
    a2a::core
    a2a::proto_generated
    GTest::gtest_main
)

gtest_discover_tests(rest_push_notification_transport_test)


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

add_executable(transport_mux_test
  unit/transport_mux_test.cpp
)

target_link_libraries(transport_mux_test
  PRIVATE
    a2a::server
    a2a::core
    a2a::proto_generated
    GTest::gtest_main
)

gtest_discover_tests(transport_mux_test)

add_executable(server_utils_test
  unit/server_utils_test.cpp
)

target_link_libraries(server_utils_test
  PRIVATE
    a2a::server
    a2a::core
    a2a::proto_generated
    GTest::gtest_main
)

gtest_discover_tests(server_utils_test)

add_executable(network_utils_test
  unit/network_utils_test.cpp
)

target_link_libraries(network_utils_test
  PRIVATE
    a2a::server
    a2a::core
    GTest::gtest_main
)

gtest_discover_tests(network_utils_test)

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

add_executable(stream_cancellation_watcher_test
  unit/stream_cancellation_watcher_test.cpp
)

target_include_directories(stream_cancellation_watcher_test
  PRIVATE
    ${PROJECT_SOURCE_DIR}/src
)

target_link_libraries(stream_cancellation_watcher_test
  PRIVATE
    a2a::server
    a2a::core
    a2a::proto_generated
    GTest::gtest_main
)

gtest_discover_tests(stream_cancellation_watcher_test)

add_executable(stream_cancellation_watcher_fallback_test
  unit/stream_cancellation_watcher_test.cpp
  ${PROJECT_SOURCE_DIR}/src/server/transport/stream_cancellation_watcher.cpp
)

target_include_directories(stream_cancellation_watcher_fallback_test
  PRIVATE
    ${PROJECT_SOURCE_DIR}/src
)

target_compile_definitions(stream_cancellation_watcher_fallback_test
  PRIVATE
    A2A_FORCE_STREAM_CANCELLATION_WATCHER_THREAD_FALLBACK=1
)

target_link_libraries(stream_cancellation_watcher_fallback_test
  PRIVATE
    a2a::core
    a2a::proto_generated
    GTest::gtest_main
)

gtest_discover_tests(stream_cancellation_watcher_fallback_test)


add_executable(push_notification_store_test
  unit/push_notification_store_test.cpp
)

target_link_libraries(push_notification_store_test
  PRIVATE
    a2a::server
    a2a::core
    a2a::proto_generated
    GTest::gtest_main
)

gtest_discover_tests(push_notification_store_test)

add_executable(push_notification_service_test
  unit/push_notification_service_test.cpp
)

target_link_libraries(push_notification_service_test
  PRIVATE
    a2a::server
    a2a::core
    a2a::proto_generated
    GTest::gtest_main
)

gtest_discover_tests(push_notification_service_test)

add_executable(push_notification_delivery_test
  unit/push_notification_delivery_test.cpp
)

target_link_libraries(push_notification_delivery_test
  PRIVATE
    a2a::server
    a2a::core
    a2a::proto_generated
    GTest::gtest_main
)

gtest_discover_tests(push_notification_delivery_test)

add_executable(server_task_store_unit_test
  unit/server_task_store_unit_test.cpp
)

target_link_libraries(server_task_store_unit_test
  PRIVATE
    a2a::server
    a2a::core
    a2a::proto_generated
    GTest::gtest_main
)

gtest_discover_tests(server_task_store_unit_test)

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
    ${CMAKE_SOURCE_DIR}/tests/support/example_support
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
    ${CMAKE_SOURCE_DIR}/tests/support/example_support
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

add_executable(cpp_grpc_interop_client
  interop/cpp_grpc_interop_client.cpp
)
target_link_libraries(cpp_grpc_interop_client
  PRIVATE
    a2a::client
    a2a::core
    a2a::proto_generated
)

add_executable(cpp_grpc_interop_server
  interop/cpp_grpc_interop_server.cpp
)
target_link_libraries(cpp_grpc_interop_server
  PRIVATE
    a2a::server
    a2a::core
    a2a::proto_generated
)

add_executable(tck_sut sut/tck_sut.cpp)
target_include_directories(tck_sut PRIVATE ${CMAKE_SOURCE_DIR}/tests ${CMAKE_SOURCE_DIR}/tests/support/example_support)
target_link_libraries(tck_sut PRIVATE a2a::server a2a::core a2a::proto_generated)

if(A2A_ENABLE_SUBSCRIPTION_DIAGNOSTICS)
  target_include_directories(tck_sut PRIVATE ${PROJECT_SOURCE_DIR}/src)
  target_compile_definitions(tck_sut PRIVATE A2A_ENABLE_SUBSCRIPTION_DIAGNOSTICS=1)
endif()

if(A2A_ENABLE_POSTGRES_STORE)
  target_link_libraries(tck_sut
    PRIVATE
      a2a::store_postgres
  )
  target_compile_definitions(tck_sut
    PRIVATE
      A2A_ENABLE_POSTGRES_STORE=1
  )
endif()

add_executable(store_conformance_test
  store_conformance/store_conformance_test.cpp
)

target_include_directories(store_conformance_test
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(store_conformance_test
  PRIVATE
    a2a::server
    a2a::core
    a2a::proto_generated
    GTest::gtest_main
)

if(A2A_ENABLE_POSTGRES_STORE)
  find_package(PostgreSQL REQUIRED)
  target_link_libraries(store_conformance_test
    PRIVATE
      a2a::store_postgres
      PostgreSQL::PostgreSQL
  )
  target_compile_definitions(store_conformance_test
    PRIVATE
      A2A_ENABLE_POSTGRES_STORE=1
  )
endif()

gtest_discover_tests(store_conformance_test)

add_executable(a2a_performance_driver_test
  performance/a2a_performance_common.cpp
  performance/a2a_performance_driver_test.cpp
)

target_include_directories(a2a_performance_driver_test
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/support
)

target_link_libraries(a2a_performance_driver_test
  PRIVATE
    a2a::server
    a2a::core
    a2a::proto_generated
    GTest::gtest_main
)

if(A2A_ENABLE_POSTGRES_STORE)
  target_link_libraries(a2a_performance_driver_test
    PRIVATE
      a2a::store_postgres
  )
  target_compile_definitions(a2a_performance_driver_test
    PRIVATE
      A2A_ENABLE_POSTGRES_STORE=1
  )
endif()

gtest_discover_tests(a2a_performance_driver_test)

find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
  if(NOT A2A_ENABLE_SUBSCRIPTION_DIAGNOSTICS)
    add_test(
      NAME subscription_diagnostics_normal_build_test
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/subscription_diagnostics_build_test.py
              $<TARGET_FILE:a2a_core> $<TARGET_FILE:a2a_client> $<TARGET_FILE:a2a_server>
    )
  endif()
  add_test(
    NAME http_persistence_test
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/http_persistence_test.py $<TARGET_FILE:tck_sut>
  )
  add_test(
    NAME performance_runner_script_test
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/performance_runner_test.py
  )
  add_test(
    NAME run_examples_script_test
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/run_examples_script_test.py
            ${CMAKE_SOURCE_DIR}/scripts/run_examples.sh
            ${CMAKE_SOURCE_DIR}/scripts/install_build_deps.sh
  )
endif()

add_executable(a2a_performance_driver
  performance/a2a_performance_common.cpp
  performance/a2a_performance_driver.cpp
)

add_executable(a2a_wire_performance_driver
  performance/a2a_performance_common.cpp
  performance/a2a_wire_performance_driver.cpp
)

target_include_directories(a2a_performance_driver
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/support
)

target_include_directories(a2a_wire_performance_driver
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/support
)

target_link_libraries(a2a_performance_driver
  PRIVATE
    a2a::server
    a2a::core
    a2a::proto_generated
)

target_link_libraries(a2a_wire_performance_driver
  PRIVATE
    a2a::client
    a2a::core
    a2a::proto_generated
)

if(A2A_ENABLE_SUBSCRIPTION_DIAGNOSTICS)
  target_include_directories(a2a_wire_performance_driver PRIVATE ${PROJECT_SOURCE_DIR}/src)
  target_compile_definitions(a2a_wire_performance_driver PRIVATE A2A_ENABLE_SUBSCRIPTION_DIAGNOSTICS=1)
endif()

if(A2A_ENABLE_POSTGRES_STORE)
  target_link_libraries(a2a_performance_driver
    PRIVATE
      a2a::store_postgres
  )
  target_compile_definitions(a2a_performance_driver
    PRIVATE
      A2A_ENABLE_POSTGRES_STORE=1
  )
endif()
