cmake_minimum_required(VERSION 3.25)
include("${CMAKE_CURRENT_LIST_DIR}/BoostVersion.cmake")

if(NOT A2A_BOOST_INSTALL_PREFIX)
  message(FATAL_ERROR "Set A2A_BOOST_INSTALL_PREFIX to a writable dependency prefix")
endif()
if(NOT A2A_BOOST_DOWNLOAD_DIR)
  set(A2A_BOOST_DOWNLOAD_DIR "${A2A_BOOST_INSTALL_PREFIX}/../boost-download")
endif()
file(MAKE_DIRECTORY "${A2A_BOOST_DOWNLOAD_DIR}")
set(archive "${A2A_BOOST_DOWNLOAD_DIR}/boost-${A2A_BOOST_VERSION}-cmake.tar.xz")
set(source "${A2A_BOOST_DOWNLOAD_DIR}/boost-${A2A_BOOST_VERSION}")
set(build "${A2A_BOOST_DOWNLOAD_DIR}/build-${A2A_BOOST_VERSION}")

if(EXISTS "${archive}")
  file(SHA256 "${archive}" actual_hash)
endif()
if(NOT actual_hash STREQUAL A2A_BOOST_SHA256)
  file(DOWNLOAD
    "https://github.com/boostorg/boost/releases/download/boost-${A2A_BOOST_VERSION}/boost-${A2A_BOOST_VERSION}-cmake.tar.xz"
    "${archive}" EXPECTED_HASH "SHA256=${A2A_BOOST_SHA256}" TLS_VERIFY ON SHOW_PROGRESS)
endif()
if(NOT EXISTS "${source}/CMakeLists.txt")
  file(ARCHIVE_EXTRACT INPUT "${archive}" DESTINATION "${A2A_BOOST_DOWNLOAD_DIR}")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" -S "${source}" -B "${build}"
  -DBOOST_INCLUDE_LIBRARIES=asio -DBUILD_TESTING=OFF -DCMAKE_BUILD_TYPE=Release
  "-DCMAKE_INSTALL_PREFIX=${A2A_BOOST_INSTALL_PREFIX}" -DCMAKE_INSTALL_LIBDIR=lib
  COMMAND_ERROR_IS_FATAL ANY)
execute_process(COMMAND "${CMAKE_COMMAND}" --build "${build}" --parallel 2
  COMMAND_ERROR_IS_FATAL ANY)
execute_process(COMMAND "${CMAKE_COMMAND}" --install "${build}"
  COMMAND_ERROR_IS_FATAL ANY)
