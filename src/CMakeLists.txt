add_library(a2a_core STATIC
  core/core.cpp
  core/error.cpp
  core/extensions.cpp
  core/protojson.cpp
  core/version.cpp
)
add_library(a2a::core ALIAS a2a_core)

target_include_directories(a2a_core
  PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(a2a_core
  PUBLIC
    a2a::proto_generated
    protobuf::libprotobuf
)

add_library(a2a_client STATIC
  client/client.cpp
  client/discovery.cpp
  client/http_json_transport.cpp
  client/sse_parser.cpp
)
add_library(a2a::client ALIAS a2a_client)

target_include_directories(a2a_client
  PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(a2a_client
  PUBLIC
    a2a::core
)

add_library(a2a_server STATIC
  server/server.cpp
)
add_library(a2a::server ALIAS a2a_server)

target_include_directories(a2a_server
  PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(a2a_server
  PUBLIC
    a2a::core
)
