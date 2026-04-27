add_executable(example_discovery_only_client discovery_only_client.cpp)
target_include_directories(example_discovery_only_client PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(example_discovery_only_client PRIVATE a2a::client a2a::core)

add_executable(example_rest_client rest_client.cpp)
target_include_directories(example_rest_client PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(example_rest_client PRIVATE a2a::client a2a::server a2a::core)

add_executable(example_json_rpc_client json_rpc_client.cpp)
target_include_directories(example_json_rpc_client PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(example_json_rpc_client PRIVATE a2a::client a2a::server a2a::core)

add_executable(example_minimal_server_custom_executor minimal_server_custom_executor.cpp)
target_include_directories(example_minimal_server_custom_executor PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(example_minimal_server_custom_executor PRIVATE a2a::server a2a::core)

add_executable(example_streaming_client streaming_client.cpp)
target_include_directories(example_streaming_client PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(example_streaming_client PRIVATE a2a::client a2a::core)

add_executable(example_grpc_client grpc_client.cpp)
target_include_directories(example_grpc_client PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(example_grpc_client PRIVATE a2a::client a2a::core)
