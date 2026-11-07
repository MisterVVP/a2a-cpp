set(A2A_PROTO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
set(A2A_GENERATED_DIR "${PROJECT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${A2A_GENERATED_DIR}")

set(A2A_GOOGLEAPIS_PROTO_ROOT "${PROJECT_SOURCE_DIR}/third_party/googleapis")
set(A2A_PROTO_FILE "${A2A_PROTO_ROOT}/a2a/v1/a2a.proto")
set(A2A_GOOGLE_API_PROTO_FILES
  "${A2A_GOOGLEAPIS_PROTO_ROOT}/google/api/annotations.proto"
  "${A2A_GOOGLEAPIS_PROTO_ROOT}/google/api/client.proto"
  "${A2A_GOOGLEAPIS_PROTO_ROOT}/google/api/field_behavior.proto"
  "${A2A_GOOGLEAPIS_PROTO_ROOT}/google/api/http.proto"
)

set(A2A_PROTO_INCLUDE_ARGS
  "--proto_path=${A2A_PROTO_ROOT}"
  "--proto_path=${A2A_GOOGLEAPIS_PROTO_ROOT}"
)

foreach(PROTOBUF_INCLUDE_DIR IN LISTS Protobuf_INCLUDE_DIRS)
  list(APPEND A2A_PROTO_INCLUDE_ARGS "--proto_path=${PROTOBUF_INCLUDE_DIR}")
endforeach()

set(A2A_PROTO_SRCS
  "${A2A_GENERATED_DIR}/a2a/v1/a2a.pb.cc"
  "${A2A_GENERATED_DIR}/google/api/annotations.pb.cc"
  "${A2A_GENERATED_DIR}/google/api/client.pb.cc"
  "${A2A_GENERATED_DIR}/google/api/field_behavior.pb.cc"
  "${A2A_GENERATED_DIR}/google/api/http.pb.cc"
)

set(A2A_PROTO_HDRS
  "${A2A_GENERATED_DIR}/a2a/v1/a2a.pb.h"
  "${A2A_GENERATED_DIR}/google/api/annotations.pb.h"
  "${A2A_GENERATED_DIR}/google/api/client.pb.h"
  "${A2A_GENERATED_DIR}/google/api/field_behavior.pb.h"
  "${A2A_GENERATED_DIR}/google/api/http.pb.h"
)

set(A2A_GRPC_SRCS "${A2A_GENERATED_DIR}/a2a/v1/a2a.grpc.pb.cc")
set(A2A_GRPC_HDRS "${A2A_GENERATED_DIR}/a2a/v1/a2a.grpc.pb.h")

add_custom_command(
  OUTPUT ${A2A_PROTO_SRCS} ${A2A_PROTO_HDRS}
  COMMAND protobuf::protoc
  ARGS
    ${A2A_PROTO_INCLUDE_ARGS}
    --experimental_allow_proto3_optional
    --cpp_out=${A2A_GENERATED_DIR}
    ${A2A_PROTO_FILE}
    ${A2A_GOOGLE_API_PROTO_FILES}
  DEPENDS
    ${A2A_PROTO_FILE}
    ${A2A_GOOGLE_API_PROTO_FILES}
    protobuf::protoc
  COMMENT "Generating protobuf C++ sources"
  VERBATIM
)

add_custom_command(
  OUTPUT ${A2A_GRPC_SRCS} ${A2A_GRPC_HDRS}
  COMMAND protobuf::protoc
  ARGS
    ${A2A_PROTO_INCLUDE_ARGS}
    --experimental_allow_proto3_optional
    --grpc_out=${A2A_GENERATED_DIR}
    --plugin=protoc-gen-grpc=$<TARGET_FILE:gRPC::grpc_cpp_plugin>
    ${A2A_PROTO_FILE}
  COMMAND ${CMAKE_COMMAND}
    "-DA2A_GRPC_SOURCE=${A2A_GRPC_SRCS}"
    "-DA2A_GRPC_HEADER=${A2A_GRPC_HDRS}"
    -P "${PROJECT_SOURCE_DIR}/cmake/NormalizeGrpcWindowsMacros.cmake"
  DEPENDS
    ${A2A_PROTO_FILE}
    "${PROJECT_SOURCE_DIR}/cmake/NormalizeGrpcWindowsMacros.cmake"
    protobuf::protoc
    gRPC::grpc_cpp_plugin
    ${A2A_PROTO_SRCS}
  COMMENT "Generating gRPC C++ sources from ${A2A_PROTO_FILE}"
  VERBATIM
)

add_custom_target(a2a_proto_codegen
  DEPENDS ${A2A_PROTO_SRCS} ${A2A_PROTO_HDRS} ${A2A_GRPC_SRCS} ${A2A_GRPC_HDRS}
)

add_library(a2a_proto_generated STATIC
  ${A2A_PROTO_SRCS}
  ${A2A_GRPC_SRCS}
)
add_library(a2a::proto_generated ALIAS a2a_proto_generated)
set_target_properties(a2a_proto_generated PROPERTIES EXPORT_NAME proto_generated)

# gRPC 1.33 requires this opt-in for the stable callback service/client names.
# Export it so installed consumers see the same generated API as the SDK.
if(gRPC_VERSION VERSION_LESS 1.38)
  target_compile_definitions(a2a_proto_generated PUBLIC GRPC_CALLBACK_API_NONEXPERIMENTAL=1)
endif()

target_link_libraries(a2a_proto_generated
  PUBLIC
    protobuf::libprotobuf
    gRPC::grpc++
)

target_include_directories(a2a_proto_generated
  PUBLIC
    $<BUILD_INTERFACE:${A2A_GENERATED_DIR}>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

add_dependencies(a2a_proto_generated a2a_proto_codegen)

install(TARGETS a2a_proto_generated
  EXPORT a2a_cppTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
