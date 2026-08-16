include(CheckCXXSourceCompiles)
check_cxx_source_compiles("
#include <thread>
int main() {
#ifndef __cpp_lib_jthread
#error std::jthread is unavailable
#endif
  std::jthread worker;
  return 0;
}
" A2A_STDLIB_HAS_JTHREAD)
if(A2A_STDLIB_HAS_JTHREAD)
  message(STATUS "StreamCancellationWatcher: std::jthread implementation selected for native build")
else()
  message(STATUS "StreamCancellationWatcher: interruptible std::thread fallback selected for native build")
endif()

add_library(a2a_core STATIC
  core/agent_card/agent_card_builder.cpp
  core/agent_card/agent_card_provider.cpp
  core/core.cpp
  core/error.cpp
  core/extensions.cpp
  core/http_utils.cpp
  core/json_value.cpp
  core/protojson.cpp
  core/response_builders.cpp
  core/string_utils.cpp
  core/version.cpp
)
add_library(a2a::core ALIAS a2a_core)
set_target_properties(a2a_core PROPERTIES EXPORT_NAME core)

set(A2A_PUBLIC_INCLUDE_DIR "${PROJECT_SOURCE_DIR}/include")

target_include_directories(a2a_core
  PUBLIC
    $<BUILD_INTERFACE:${A2A_PUBLIC_INCLUDE_DIR}>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_link_libraries(a2a_core
  PUBLIC
    a2a::proto_generated
    protobuf::libprotobuf
)

add_library(a2a_http STATIC
  http/http_client.cpp
)
add_library(a2a::http ALIAS a2a_http)
set_target_properties(a2a_http PROPERTIES EXPORT_NAME http)

target_include_directories(a2a_http
  PUBLIC
    $<BUILD_INTERFACE:${A2A_PUBLIC_INCLUDE_DIR}>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_link_libraries(a2a_http
  PUBLIC
    a2a::core
)

if(A2A_HAS_LIBCURL)
  target_link_libraries(a2a_http
    PRIVATE
      CURL::libcurl
  )
  target_compile_definitions(a2a_http
    PUBLIC
      A2A_HAS_LIBCURL=1
  )
endif()

add_library(a2a_client STATIC
  client/auth.cpp
  client/client.cpp
  client/discovery.cpp
  client/transport/grpc_transport.cpp
  client/transport/http_json_transport.cpp
  client/transport/json_rpc_transport.cpp
  client/transport/sse_parser.cpp
)
add_library(a2a::client ALIAS a2a_client)
set_target_properties(a2a_client PROPERTIES EXPORT_NAME client)

target_include_directories(a2a_client
  PUBLIC
    $<BUILD_INTERFACE:${A2A_PUBLIC_INCLUDE_DIR}>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_link_libraries(a2a_client
  PUBLIC
    a2a::core
    a2a::http
)

add_library(a2a_server STATIC
  server/agent_card/agent_card_serializer.cpp
  server/transport/grpc_server_transport.cpp
  server/transport/stream_cancellation_watcher.cpp
  server/transport/http_adapter.cpp
  server/transport/http_server_response_builder.cpp
  server/transport/rest_server_transport.cpp
  server/required_extensions_validator.cpp
  server/version_header_validator.cpp
  server/transport/rest_transport.cpp
  server/transport/json_rpc_server_transport.cpp
  server/push_notifications/push_notification_delivery.cpp
  server/push_notifications/push_notification_service.cpp
  server/push_notifications/push_notification_store.cpp
  server/network_utils.cpp
  server/stores/sql_identifier.cpp
  server/task_id_generator.cpp
  server/task_subscription_service.cpp
  server/transport/transport_mux.cpp
  server/dispatcher.cpp
  server/request_context.cpp
  server/tasks/in_memory_task_store.cpp
  server/tasks/list_tasks.cpp
  server/tasks/task_history.cpp
  server/tasks/task_lifecycle_service.cpp
  server/tasks/task_ordering.cpp
  server/stores/store_factory.cpp
)
add_library(a2a::server ALIAS a2a_server)
set_target_properties(a2a_server PROPERTIES EXPORT_NAME server)

target_include_directories(a2a_server
  PUBLIC
    $<BUILD_INTERFACE:${A2A_PUBLIC_INCLUDE_DIR}>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_link_libraries(a2a_server
  PUBLIC
    a2a::core
    a2a::http
)

if(A2A_ENABLE_POSTGRES_STORE)
  find_package(PostgreSQL REQUIRED)

  add_library(a2a_store_postgres STATIC
    server/stores/postgres_common.cpp
    server/stores/postgres_task_store.cpp
    server/stores/postgres_notification_store.cpp
  )
  add_library(a2a::store_postgres ALIAS a2a_store_postgres)
  set_target_properties(a2a_store_postgres
    PROPERTIES
      EXPORT_NAME store_postgres
  )

  target_include_directories(a2a_store_postgres
    PUBLIC
      $<BUILD_INTERFACE:${A2A_PUBLIC_INCLUDE_DIR}>
      $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  )

  target_link_libraries(a2a_store_postgres
    PUBLIC
      a2a::server
    PRIVATE
      PostgreSQL::PostgreSQL
  )

  target_compile_definitions(a2a_store_postgres
    PUBLIC
      A2A_ENABLE_POSTGRES_STORE=1
  )

  if(A2A_ENABLE_TESTING)
    target_compile_definitions(a2a_store_postgres
      PUBLIC
        A2A_POSTGRES_STORE_TESTING=1
    )
  endif()

  target_compile_definitions(a2a_server
    PRIVATE
      A2A_ENABLE_POSTGRES_STORE=1
  )
endif()

set(A2A_INSTALL_TARGETS a2a_core a2a_http a2a_client a2a_server)
if(A2A_ENABLE_POSTGRES_STORE)
  list(APPEND A2A_INSTALL_TARGETS a2a_store_postgres)
endif()

install(TARGETS ${A2A_INSTALL_TARGETS}
  EXPORT a2a_cppTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
