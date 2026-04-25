set(A2A_PROTO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
set(A2A_GENERATED_DIR "${CMAKE_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${A2A_GENERATED_DIR}")

set(A2A_PROTO_FILE "${A2A_PROTO_ROOT}/a2a/v1/a2a.proto")
set(A2A_PROTO_SRCS "${A2A_GENERATED_DIR}/a2a/v1/a2a.pb.cc")
set(A2A_PROTO_HDRS "${A2A_GENERATED_DIR}/a2a/v1/a2a.pb.h")
set(A2A_GRPC_SRCS "${A2A_GENERATED_DIR}/a2a/v1/a2a.grpc.pb.cc")
set(A2A_GRPC_HDRS "${A2A_GENERATED_DIR}/a2a/v1/a2a.grpc.pb.h")

add_custom_command(
  OUTPUT ${A2A_PROTO_SRCS} ${A2A_PROTO_HDRS} ${A2A_GRPC_SRCS} ${A2A_GRPC_HDRS}
  COMMAND protobuf::protoc
  ARGS
    --proto_path=${A2A_PROTO_ROOT}
    --proto_path=${Protobuf_INCLUDE_DIRS}
    --cpp_out=${A2A_GENERATED_DIR}
    --grpc_out=${A2A_GENERATED_DIR}
    --plugin=protoc-gen-grpc=$<TARGET_FILE:gRPC::grpc_cpp_plugin>
    ${A2A_PROTO_FILE}
  DEPENDS
    ${A2A_PROTO_FILE}
    protobuf::protoc
    gRPC::grpc_cpp_plugin
  COMMENT "Generating C++ sources from ${A2A_PROTO_FILE}"
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
