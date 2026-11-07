cmake_minimum_required(VERSION 3.25)

# Match the validated dependency when linting with Clang 19: Ubuntu 24.04's
# simdjson 3.0.1 contains invalid CRTP member lookup rejected by that parser.
set(A2A_SIMDJSON_VERSION 4.6.11)
set(A2A_SIMDJSON_SHA256 61d948fc24f0d793829ad658058e7597d064988a89b4607ea02e401a82df98ff)

if(NOT A2A_SIMDJSON_INSTALL_PREFIX)
  message(FATAL_ERROR "Set A2A_SIMDJSON_INSTALL_PREFIX to a writable dependency prefix")
endif()
if(NOT A2A_SIMDJSON_DOWNLOAD_DIR)
  set(A2A_SIMDJSON_DOWNLOAD_DIR "${A2A_SIMDJSON_INSTALL_PREFIX}/../simdjson-download")
endif()
file(MAKE_DIRECTORY "${A2A_SIMDJSON_DOWNLOAD_DIR}")
set(archive "${A2A_SIMDJSON_DOWNLOAD_DIR}/simdjson-${A2A_SIMDJSON_VERSION}.tar.gz")
set(source "${A2A_SIMDJSON_DOWNLOAD_DIR}/simdjson-${A2A_SIMDJSON_VERSION}")
set(build "${A2A_SIMDJSON_DOWNLOAD_DIR}/build-${A2A_SIMDJSON_VERSION}")

if(EXISTS "${archive}")
  file(SHA256 "${archive}" actual_hash)
endif()
if(NOT actual_hash STREQUAL A2A_SIMDJSON_SHA256)
  file(DOWNLOAD
    "https://github.com/simdjson/simdjson/archive/refs/tags/v${A2A_SIMDJSON_VERSION}.tar.gz"
    "${archive}" EXPECTED_HASH "SHA256=${A2A_SIMDJSON_SHA256}" TLS_VERIFY ON SHOW_PROGRESS)
endif()
if(NOT EXISTS "${source}/CMakeLists.txt")
  file(ARCHIVE_EXTRACT INPUT "${archive}" DESTINATION "${A2A_SIMDJSON_DOWNLOAD_DIR}")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" -S "${source}" -B "${build}"
  -DSIMDJSON_DEVELOPER_MODE=OFF -DSIMDJSON_INSTALL=ON -DBUILD_SHARED_LIBS=OFF
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_LIBDIR=lib
  "-DCMAKE_INSTALL_PREFIX=${A2A_SIMDJSON_INSTALL_PREFIX}"
  COMMAND_ERROR_IS_FATAL ANY)
execute_process(COMMAND "${CMAKE_COMMAND}" --build "${build}" --parallel 2
  COMMAND_ERROR_IS_FATAL ANY)
execute_process(COMMAND "${CMAKE_COMMAND}" --install "${build}"
  COMMAND_ERROR_IS_FATAL ANY)
