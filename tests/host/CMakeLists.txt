# Host-side unit tests for components/omega_calibration.
#
# Builds the calibration headers against small stand-ins for the espp pieces
# that need ESP-IDF (logger, base_component, joystick, file_system) and
# against the REAL espp stream_frame / dispatcher headers, so the wire
# protocol under test is the one the device speaks.
#
#   cmake -S tests/host -B build-host
#   cmake --build build-host
#   ctest --test-dir build-host --output-on-failure
#
# The espp headers come from managed_components/ when an idf.py build has
# populated it, otherwise the two files are downloaded pinned to the version
# in dependencies.lock and verified by hash.

cmake_minimum_required(VERSION 3.20)
project(omega_stick_host_tests CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# ESP-IDF compiles with gnu++2b; the component relies on M_PI from that.
set(CMAKE_CXX_EXTENSIONS ON)
add_compile_definitions(_USE_MATH_DEFINES)

set(OMEGA_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")
set(ESPP_VERSION "v1.3.5") # keep in step with espp/* in dependencies.lock

# ---- espp headers -----------------------------------------------------------
set(ESPP_MANAGED "${OMEGA_ROOT}/managed_components")
if(EXISTS "${ESPP_MANAGED}/espp__stream_frame/include/stream_frame.hpp"
   AND EXISTS "${ESPP_MANAGED}/espp__dispatcher/include/dispatcher.hpp")
  message(STATUS "Using espp headers from managed_components/")
  set(ESPP_INCLUDE_DIRS
      "${ESPP_MANAGED}/espp__stream_frame/include"
      "${ESPP_MANAGED}/espp__dispatcher/include")
else()
  set(ESPP_DL_DIR "${CMAKE_CURRENT_BINARY_DIR}/espp-${ESPP_VERSION}/include")
  file(MAKE_DIRECTORY "${ESPP_DL_DIR}")
  function(fetch_espp_header component name sha256)
    set(dest "${ESPP_DL_DIR}/${name}")
    if(NOT EXISTS "${dest}")
      set(url "https://raw.githubusercontent.com/esp-cpp/espp/${ESPP_VERSION}/components/${component}/include/${name}")
      message(STATUS "Downloading ${url}")
      file(DOWNLOAD "${url}" "${dest}" EXPECTED_HASH SHA256=${sha256} STATUS status)
      list(GET status 0 code)
      if(NOT code EQUAL 0)
        list(GET status 1 msg)
        file(REMOVE "${dest}")
        message(FATAL_ERROR "Could not fetch ${name}: ${msg}")
      endif()
    endif()
  endfunction()
  fetch_espp_header(stream_frame stream_frame.hpp
    a67f593bbe21484806373ab05b43eb25e77bed98819780f1fc58d8014477deee)
  fetch_espp_header(dispatcher dispatcher.hpp
    b0ca02a77cd3029428426cc8acae296b7e05495683d0fc2529118aa4c22c68dc)
  set(ESPP_INCLUDE_DIRS "${ESPP_DL_DIR}")
endif()

# ---- test binary ------------------------------------------------------------
add_executable(omega_host_tests
  test_main.cpp
  test_persistence.cpp
  test_store.cpp
  test_service.cpp)

target_include_directories(omega_host_tests PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}"
  "${CMAKE_CURRENT_SOURCE_DIR}/stubs"
  ${ESPP_INCLUDE_DIRS}
  "${OMEGA_ROOT}/components/omega_calibration/include")

if(MSVC)
  target_compile_options(omega_host_tests PRIVATE /W4)
else()
  target_compile_options(omega_host_tests PRIVATE -Wall -Wextra)
endif()
if(MINGW)
  # Let the binary run from shells that do not have the MinGW bin dir on PATH.
  target_link_options(omega_host_tests PRIVATE -static)
endif()

enable_testing()
add_test(NAME omega_host_tests COMMAND omega_host_tests)
set_tests_properties(omega_host_tests PROPERTIES TIMEOUT 60)
