vcpkg_check_linkage(ONLY_STATIC_LIBRARY)

vcpkg_check_features(
    OUT_FEATURE_OPTIONS FEATURE_OPTIONS
    FEATURES
        postgres-store A2A_ENABLE_POSTGRES_STORE
)

vcpkg_from_git(
    OUT_SOURCE_PATH SOURCE_PATH
    URL "https://github.com/MisterVVP/a2a-cpp.git"
    REF v0.2.0
)

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -DA2A_ENABLE_TESTING=OFF
        -DA2A_BUILD_EXAMPLES=OFF
        -DA2A_BUILD_BENCHMARKS=OFF
        -DA2A_ENABLE_LIBCURL=ON
        ${FEATURE_OPTIONS}
)

vcpkg_cmake_install()
vcpkg_cmake_config_fixup(PACKAGE_NAME a2a_cpp CONFIG_PATH lib/cmake/a2a_cpp)

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/share")

file(INSTALL "${CMAKE_CURRENT_LIST_DIR}/usage" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}")
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
