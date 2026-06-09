add_library(a2a_core STATIC
  core/agent_card_builder.cpp
  core/core.cpp
  core/error.cpp
  core/extensions.cpp
  core/protojson.cpp
  core/response_builders.cpp
  core/version.cpp
)
add_library(a2a::core ALIAS a2a_core)

set(A2A_PUBLIC_INCLUDE_DIR ${CMAKE_SOURCE_DIR}/include)

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
  client/grpc_transport.cpp
  client/http_json_transport.cpp
  client/json_rpc_transport.cpp
  client/sse_parser.cpp
)
add_library(a2a::client ALIAS a2a_client)

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
  server/grpc_server_transport.cpp
  server/http_adapter.cpp
  server/rest_server_transport.cpp
  server/rest_transport.cpp
  server/json_rpc_server_transport.cpp
  server/push_notification_delivery.cpp
  server/push_notification_service.cpp
  server/push_notification_store.cpp
  server/socket_utils.cpp
  server/task_id_generator.cpp
  server/transport_mux.cpp
  server/server.cpp
  server/sql_identifier.cpp
  server/stores/store_factory.cpp
)
add_library(a2a::server ALIAS a2a_server)

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
    server/stores/postgres_store.cpp
  )
  add_library(a2a::store_postgres ALIAS a2a_store_postgres)

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
