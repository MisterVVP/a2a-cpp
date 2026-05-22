add_library(a2a_core STATIC
  core/core.cpp
  core/error.cpp
  core/extensions.cpp
  core/protojson.cpp
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
)

add_library(a2a_server STATIC
  server/grpc_server_transport.cpp
  server/rest_server_transport.cpp
  server/rest_transport.cpp
  server/json_rpc_server_transport.cpp
  server/server.cpp
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
)

install(TARGETS a2a_core a2a_client a2a_server
  EXPORT a2a_cppTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
