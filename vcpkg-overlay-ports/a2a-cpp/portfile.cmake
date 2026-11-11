vcpkg_check_linkage(ONLY_STATIC_LIBRARY)

vcpkg_check_features(
    OUT_FEATURE_OPTIONS FEATURE_OPTIONS
    FEATURES
        postgres-store A2A_ENABLE_POSTGRES_STORE
        asio A2A_ENABLE_ASIO
)

vcpkg_from_git(
    OUT_SOURCE_PATH SOURCE_PATH
    URL "https://github.com/MisterVVP/a2a-cpp.git"
    # Integrated v0.6.0 candidate; no unpublished release tag is required.
    REF 2e92a3d082b0a1e1eb0cdff48aa835919b2e1ddf
    PATCHES fix-async-deadline-wait.patch
)

# The pinned candidate predates the release metadata bump. Stamp the installed
# CMake package with the overlay version too. Replace the pin with the final
# release commit and remove this stamp and its integrated patch when publishing.
vcpkg_replace_string("${SOURCE_PATH}/CMakeLists.txt"
    "project(a2a_cpp VERSION 0.5.2 LANGUAGES CXX)"
    "project(a2a_cpp VERSION ${VERSION} LANGUAGES CXX)")

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
