include(FetchContent)

FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
)
# For Windows: Prevent overriding the parent project's compiler/linker settings.
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

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
