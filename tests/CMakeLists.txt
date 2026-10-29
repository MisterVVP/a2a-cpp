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

if(WIN32)
  # Discover after linking and app-local DLL deployment have finished for every
  # target, rather than starting test processes during the parallel MSVC build.
  set(CMAKE_GTEST_DISCOVER_TESTS_DISCOVERY_MODE PRE_TEST)
endif()

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
  target_link_libraries(subscription_diagnostics_test PRIVATE a2a_core_diagnostics GTest::gtest_main)
  gtest_discover_tests(subscription_diagnostics_test)
endif()

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

gtest_discover_tests(http_client_test PROPERTIES LABELS "tsan")
if(A2A_HAS_LIBCURL)
  target_include_directories(http_client_test PRIVATE ${PROJECT_SOURCE_DIR}/src/http)
  target_link_libraries(http_client_test PRIVATE CURL::libcurl)
endif()

add_executable(mcp_client_test
  unit/mcp_client_test.cpp
  ${PROJECT_SOURCE_DIR}/examples/tutorials/common/mcp_client.cpp
)

target_include_directories(mcp_client_test PRIVATE ${PROJECT_SOURCE_DIR}/examples/tutorials/common)
target_link_libraries(mcp_client_test PRIVATE a2a::client a2a::http a2a::core GTest::gtest_main)
if(WIN32)
  target_link_libraries(mcp_client_test PRIVATE ws2_32)
endif()
gtest_discover_tests(mcp_client_test)

add_executable(core_protojson_golden_test
  unit/core_protojson_golden_test.cpp
)

target_include_directories(core_protojson_golden_test PRIVATE ${PROJECT_SOURCE_DIR}/src)

target_link_libraries(core_protojson_golden_test
  PRIVATE
    a2a::core
    a2a::proto_generated
    simdjson::simdjson
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

gtest_discover_tests(task_subscription_service_test PROPERTIES LABELS "tsan")

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

gtest_discover_tests(stream_handle_lifecycle_test PROPERTIES LABELS "tsan")


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

gtest_discover_tests(http_json_transport_test PROPERTIES LABELS "tsan")

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

gtest_discover_tests(http_json_streaming_integration_test PROPERTIES LABELS "tsan")

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

gtest_discover_tests(json_rpc_transport_test PROPERTIES LABELS "tsan")

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

gtest_discover_tests(server_dispatcher_test PROPERTIES LABELS "tsan")

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

gtest_discover_tests(stream_cancellation_watcher_test PROPERTIES LABELS "tsan")

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

gtest_discover_tests(server_task_store_unit_test PROPERTIES LABELS "tsan")

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

# DLL startup/unload can exceed CMake's five-second discovery default on Windows.
# This limit only enumerates tests; it does not change their execution timeout.
gtest_discover_tests(json_rpc_server_transport_integration_test DISCOVERY_TIMEOUT 30)

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

gtest_discover_tests(server_dispatcher_task_store_integration_test PROPERTIES LABELS "tsan")

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

add_library(sut_runtime STATIC
  sut/sut_runtime.cpp
  sut/sut_agent.cpp
  sut/sut_application.cpp
  sut/sut_server_runtime.cpp
  sut/tck_sut_http_server.cpp
  sut/tck_sut_store.cpp
)
target_include_directories(sut_runtime
  PUBLIC ${CMAKE_SOURCE_DIR}/tests
  PRIVATE ${CMAKE_SOURCE_DIR}/tests/support/example_support
)
target_link_libraries(sut_runtime PUBLIC a2a::server a2a::core a2a::proto_generated)
if(A2A_ENABLE_SIGNING)
  target_link_libraries(sut_runtime PUBLIC a2a::signing)
endif()

if(A2A_ENABLE_SUBSCRIPTION_DIAGNOSTICS)
  add_library(sut_runtime_diagnostics STATIC
    sut/sut_runtime.cpp
    sut/sut_agent.cpp
    sut/sut_application.cpp
    sut/sut_server_runtime.cpp
    sut/tck_sut_http_server.cpp
    sut/tck_sut_store.cpp
  )
  if(A2A_ENABLE_SIGNING)
    target_link_libraries(sut_runtime_diagnostics PUBLIC a2a::signing)
  endif()
  target_include_directories(sut_runtime_diagnostics
    PUBLIC ${CMAKE_SOURCE_DIR}/tests
    PRIVATE ${CMAKE_SOURCE_DIR}/tests/support/example_support
  )
  target_link_libraries(sut_runtime_diagnostics
    PUBLIC a2a_server_diagnostics a2a_core_diagnostics a2a::proto_generated
  )
endif()

add_executable(tck_sut sut/tck_sut.cpp)
target_link_libraries(tck_sut PRIVATE sut_runtime)

add_executable(performance_sut
  sut/performance_sut.cpp
  sut/performance_sut_diagnostics.cpp
)

if(A2A_ENABLE_SUBSCRIPTION_DIAGNOSTICS)
  target_link_libraries(performance_sut PRIVATE sut_runtime_diagnostics)
  target_include_directories(performance_sut PRIVATE ${PROJECT_SOURCE_DIR}/src)
  target_compile_definitions(performance_sut PRIVATE A2A_ENABLE_SUBSCRIPTION_DIAGNOSTICS=1)
else()
  target_link_libraries(performance_sut PRIVATE sut_runtime)
endif()

if(A2A_ENABLE_POSTGRES_STORE)
  target_link_libraries(sut_runtime
    PRIVATE
      a2a::store_postgres
  )
  target_compile_definitions(sut_runtime
    PRIVATE
      A2A_ENABLE_POSTGRES_STORE=1
  )
  if(A2A_ENABLE_SUBSCRIPTION_DIAGNOSTICS)
    target_link_libraries(sut_runtime_diagnostics PRIVATE a2a::store_postgres)
    target_compile_definitions(sut_runtime_diagnostics PRIVATE A2A_ENABLE_POSTGRES_STORE=1)
  endif()
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
  add_test(
    NAME subscription_diagnostics_symbol_command_test
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/subscription_diagnostics_build_test_test.py
  )
  if(NOT A2A_ENABLE_SUBSCRIPTION_DIAGNOSTICS)
    if(MSVC)
      find_program(A2A_SYMBOL_INSPECTOR NAMES dumpbin REQUIRED)
      set(A2A_SYMBOL_INSPECTOR_ARGUMENT /symbols)
    elseif(CMAKE_NM)
      set(A2A_SYMBOL_INSPECTOR ${CMAKE_NM})
      set(A2A_SYMBOL_INSPECTOR_ARGUMENT -C)
    else()
      find_program(A2A_SYMBOL_INSPECTOR NAMES llvm-nm nm REQUIRED)
      set(A2A_SYMBOL_INSPECTOR_ARGUMENT -C)
    endif()
    add_test(
      NAME subscription_diagnostics_normal_build_test
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/subscription_diagnostics_build_test.py
              --symbol-tool ${A2A_SYMBOL_INSPECTOR}
              --symbol-argument=${A2A_SYMBOL_INSPECTOR_ARGUMENT}
              $<TARGET_FILE:a2a_core> $<TARGET_FILE:a2a_client> $<TARGET_FILE:a2a_server>
    )
  endif()
  add_test(
    NAME http_persistence_test
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/http_persistence_test.py $<TARGET_FILE:performance_sut>
  )
  if(A2A_ENABLE_SUBSCRIPTION_DIAGNOSTICS)
    add_test(
      NAME http_diagnostics_reset_test
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/http_diagnostics_reset_test.py
              $<TARGET_FILE:performance_sut>
    )
  endif()
  add_test(
    NAME performance_runner_script_test
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/performance_runner_test.py
  )
  set(SUT_ENTRYPOINTS_TEST_DIAGNOSTICS_ARGUMENT)
  if(A2A_ENABLE_SUBSCRIPTION_DIAGNOSTICS)
    set(SUT_ENTRYPOINTS_TEST_DIAGNOSTICS_ARGUMENT --expect-subscription-diagnostics)
  endif()
  set(SUT_ENTRYPOINTS_TEST_POSTGRES_ARGUMENT)
  if(A2A_ENABLE_POSTGRES_STORE)
    set(SUT_ENTRYPOINTS_TEST_POSTGRES_ARGUMENT --expect-postgres)
  endif()
  add_test(
    NAME sut_entrypoints_test
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/sut_entrypoints_test.py
            --compile-commands ${CMAKE_BINARY_DIR}/compile_commands.json
            --tck-sut $<TARGET_FILE:tck_sut>
            --performance-sut $<TARGET_FILE:performance_sut>
            ${SUT_ENTRYPOINTS_TEST_DIAGNOSTICS_ARGUMENT}
            ${SUT_ENTRYPOINTS_TEST_POSTGRES_ARGUMENT}
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
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/support
)

target_link_libraries(a2a_performance_driver
  PRIVATE
    a2a::server
    a2a::core
    a2a::proto_generated
)

if(A2A_ENABLE_SUBSCRIPTION_DIAGNOSTICS)
  target_link_libraries(a2a_wire_performance_driver
    PRIVATE a2a_client_diagnostics a2a_core_diagnostics a2a::proto_generated
  )
  target_include_directories(a2a_wire_performance_driver PRIVATE ${PROJECT_SOURCE_DIR}/src)
  target_compile_definitions(a2a_wire_performance_driver PRIVATE A2A_ENABLE_SUBSCRIPTION_DIAGNOSTICS=1)
else()
  target_link_libraries(a2a_wire_performance_driver
    PRIVATE a2a::client a2a::core a2a::proto_generated
  )
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

# Real SUT scenarios use public SDK clients for every ordered binding pair.
add_executable(shared_binding_test integration/shared_binding_test.cpp)
target_include_directories(shared_binding_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(shared_binding_test PRIVATE a2a::client a2a::server GTest::gtest_main)
if(Python3_Interpreter_FOUND AND A2A_HAS_LIBCURL)
  foreach(binding_backend IN ITEMS inmemory postgres)
    if(binding_backend STREQUAL "postgres" AND NOT A2A_ENABLE_POSTGRES_STORE)
      continue()
    endif()
    add_test(NAME shared_binding_${binding_backend}
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/run_binding_scenarios.py
        --sut $<TARGET_FILE:tck_sut> --scenarios $<TARGET_FILE:shared_binding_test>
        --backend ${binding_backend})
    set_tests_properties(shared_binding_${binding_backend}
      PROPERTIES LABELS "binding_equivalence" TIMEOUT 180 SKIP_RETURN_CODE 77)
  endforeach()
endif()

add_executable(protocol_version_integration_test integration/protocol_version_integration_test.cpp)
target_include_directories(protocol_version_integration_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/support/example_support)
target_link_libraries(protocol_version_integration_test PRIVATE a2a::client a2a::server GTest::gtest_main)
gtest_discover_tests(protocol_version_integration_test PROPERTIES LABELS "protocol_version")

add_executable(sut_agent_test unit/sut_agent_test.cpp)
target_link_libraries(sut_agent_test PRIVATE sut_runtime GTest::gtest_main)
gtest_discover_tests(sut_agent_test)

add_executable(sdk_security_test unit/sdk_security_test.cpp)
target_include_directories(sdk_security_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/support/example_support)
target_link_libraries(sdk_security_test PRIVATE a2a::server GTest::gtest_main)
gtest_discover_tests(sdk_security_test PROPERTIES LABELS "security")

if(A2A_ENABLE_SIGNING)
add_executable(agent_card_signing_test unit/agent_card_signing_test.cpp)
target_include_directories(agent_card_signing_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(agent_card_signing_test PRIVATE a2a::client a2a::signing simdjson::simdjson GTest::gtest_main)
target_compile_definitions(agent_card_signing_test PRIVATE
  A2A_SIGNING_INTEROP_VECTOR_FILE="${CMAKE_CURRENT_SOURCE_DIR}/support/security/sdk_signing_vector.json"
  A2A_JCS_VECTOR_FILE="${CMAKE_CURRENT_SOURCE_DIR}/support/security/jcs_vectors.json")
gtest_discover_tests(agent_card_signing_test DISCOVERY_MODE PRE_TEST
  PROPERTIES LABELS "security\;card_signing")

add_executable(security_scenarios_test integration/security_scenarios_test.cpp)
target_include_directories(security_scenarios_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/support/example_support)
target_link_libraries(security_scenarios_test PRIVATE a2a::client a2a::server a2a::signing GTest::gtest_main)
find_program(A2A_SECURITY_NGINX_EXECUTABLE nginx)
find_program(A2A_SECURITY_OPENSSL_EXECUTABLE openssl)
if(UNIX AND Python3_Interpreter_FOUND AND A2A_HAS_LIBCURL AND A2A_SECURITY_NGINX_EXECUTABLE AND A2A_SECURITY_OPENSSL_EXECUTABLE)
  foreach(security_backend IN ITEMS inmemory postgres)
    if(security_backend STREQUAL "postgres" AND NOT A2A_ENABLE_POSTGRES_STORE)
      continue()
    endif()
    add_test(NAME security_scenarios_${security_backend}
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/run_security_scenarios.py
        --sut $<TARGET_FILE:tck_sut> --scenarios $<TARGET_FILE:security_scenarios_test>
        --independent-sdk $<TARGET_FILE:security_transport_integration_test>
        --backend ${security_backend})
    set_tests_properties(security_scenarios_${security_backend}
      PROPERTIES LABELS "security" TIMEOUT 240 SKIP_RETURN_CODE 77)
  endforeach()
endif()

endif()

add_executable(task_authorization_service_test unit/task_authorization_service_test.cpp)
if(UNIX AND Python3_Interpreter_FOUND)
  add_test(NAME security_launcher_test
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/security_launcher_test.py)
  set_tests_properties(security_launcher_test PROPERTIES LABELS "security")
endif()
target_include_directories(task_authorization_service_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(task_authorization_service_test PRIVATE a2a::server GTest::gtest_main)
gtest_discover_tests(task_authorization_service_test PROPERTIES LABELS "security")

add_executable(security_transport_integration_test integration/security_transport_integration_test.cpp)
target_include_directories(security_transport_integration_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/support/example_support)
target_link_libraries(security_transport_integration_test PRIVATE a2a::client a2a::server GTest::gtest_main)
gtest_discover_tests(security_transport_integration_test PROPERTIES LABELS "security")

if(A2A_ENABLE_ASIO)
  add_library(sut_async_runtime STATIC
    sut/async_sut_runtime.cpp
    sut/async_sut_agent.cpp
    sut/async_sut_executor.cpp
    sut/sut_work_queue.cpp
  )
  target_link_libraries(sut_async_runtime PUBLIC sut_runtime a2a::asio)
  add_executable(async_sut_test unit/async_sut_test.cpp)
  target_link_libraries(async_sut_test PRIVATE sut_async_runtime GTest::gtest_main)
  gtest_discover_tests(async_sut_test PROPERTIES LABELS tsan TIMEOUT 30)
  add_executable(tck_async_sut sut/tck_async_sut.cpp)
  target_link_libraries(tck_async_sut PRIVATE sut_async_runtime)
  if(Python3_Interpreter_FOUND)
    add_test(NAME async_sut_runtime_test
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/async_sut_runtime_test.py
        --sut $<TARGET_FILE:tck_async_sut>)
    set_tests_properties(async_sut_runtime_test PROPERTIES LABELS "grpc_async" TIMEOUT 60)
  endif()
  add_executable(async_dispatcher_test unit/async_dispatcher_test.cpp)
  target_include_directories(async_dispatcher_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(async_dispatcher_test PRIVATE a2a::asio GTest::gtest_main)
  gtest_discover_tests(async_dispatcher_test PROPERTIES LABELS "tsan")
  add_executable(async_grpc_server_transport_test integration/async_grpc_server_transport_test.cpp)
  target_include_directories(async_grpc_server_transport_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(async_grpc_server_transport_test PRIVATE a2a::asio GTest::gtest_main)
  gtest_discover_tests(async_grpc_server_transport_test PROPERTIES LABELS "grpc_async" TIMEOUT 30)
  add_executable(async_grpc_example_smoke ${PROJECT_SOURCE_DIR}/examples/apps/async_grpc_server/main.cpp)
  target_link_libraries(async_grpc_example_smoke PRIVATE a2a::asio)
  add_test(NAME async_grpc_example_smoke COMMAND async_grpc_example_smoke --smoke)
  add_executable(async_grpc_execution_evidence async_grpc_execution_evidence.cpp)
  target_include_directories(async_grpc_execution_evidence PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(async_grpc_execution_evidence PRIVATE a2a::asio)
  add_test(NAME async_grpc_execution_evidence COMMAND async_grpc_execution_evidence)
  set_tests_properties(async_grpc_execution_evidence PROPERTIES LABELS "grpc_async" TIMEOUT 60)
endif()

if(Python3_Interpreter_FOUND)
  add_test(NAME installed_async_grpc_consumer_test
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/installed_async_grpc_consumer_test.py
      --source ${PROJECT_SOURCE_DIR} --build ${PROJECT_BINARY_DIR} --config $<CONFIG>)
  set_tests_properties(installed_async_grpc_consumer_test PROPERTIES LABELS "installed;grpc_async" TIMEOUT 300 SKIP_RETURN_CODE 77)
endif()

add_executable(dispatch_pipeline_test unit/dispatch_pipeline_test.cpp)
target_include_directories(dispatch_pipeline_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(dispatch_pipeline_test PRIVATE a2a::server GTest::gtest_main)
gtest_discover_tests(dispatch_pipeline_test)

add_executable(grpc_helpers_test unit/grpc_helpers_test.cpp)
target_include_directories(grpc_helpers_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(grpc_helpers_test PRIVATE a2a::server GTest::gtest_main)
gtest_discover_tests(grpc_helpers_test)

add_executable(grpc_request_context_test unit/grpc_request_context_test.cpp)
target_include_directories(grpc_request_context_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(grpc_request_context_test PRIVATE a2a::server GTest::gtest_main)
gtest_discover_tests(grpc_request_context_test)

if(A2A_ENABLE_ASIO)
  add_executable(coroutine_dispatcher_test unit/coroutine_dispatcher_test.cpp)
  target_include_directories(coroutine_dispatcher_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(coroutine_dispatcher_test PRIVATE a2a::asio GTest::gtest_main)
  gtest_discover_tests(coroutine_dispatcher_test PROPERTIES LABELS tsan TIMEOUT 30)

  add_executable(asio_grpc_server_test integration/asio_grpc_server_test.cpp)
  target_include_directories(asio_grpc_server_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(asio_grpc_server_test PRIVATE a2a::asio GTest::gtest_main)
  gtest_discover_tests(asio_grpc_server_test PROPERTIES LABELS grpc_async TIMEOUT 30)
endif()

if(A2A_ENABLE_ASIO AND Python3_Interpreter_FOUND)
  add_test(NAME installed_asio_consumer_test
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/installed_async_grpc_consumer_test.py
      --source ${PROJECT_SOURCE_DIR} --build ${PROJECT_BINARY_DIR} --config $<CONFIG> --app asio_executor)
  set_tests_properties(installed_asio_consumer_test PROPERTIES LABELS installed TIMEOUT 300)
endif()

if(A2A_STDLIB_HAS_STOP_TOKEN)
  add_executable(request_execution_test unit/request_execution_test.cpp)
  target_link_libraries(request_execution_test PRIVATE a2a::server GTest::gtest_main)
  gtest_discover_tests(request_execution_test PROPERTIES LABELS tsan)
endif()
