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
# stream_frame/dispatcher come from managed_components/ when an idf.py build
# has populated it, otherwise they are downloaded pinned to the version in
# dependencies.lock and verified by hash. The hid-rp headers used by the XAC
# report tests are always downloaded that way.

cmake_minimum_required(VERSION 3.20)
project(omega_stick_host_tests CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# ESP-IDF compiles with gnu++2b; the component relies on M_PI from that.
set(CMAKE_CXX_EXTENSIONS ON)
add_compile_definitions(_USE_MATH_DEFINES)

set(OMEGA_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")
set(ESPP_VERSION "v1.3.5") # espp/stream_frame, espp/dispatcher in dependencies.lock
set(ESPP_HID_RP_VERSION "1.3.3") # espp/hid-rp in dependencies.lock

function(fetch_pinned url dest sha256)
  if(NOT EXISTS "${dest}")
    message(STATUS "Downloading ${url}")
    file(DOWNLOAD "${url}" "${dest}" EXPECTED_HASH SHA256=${sha256} STATUS status)
    list(GET status 0 code)
    if(NOT code EQUAL 0)
      list(GET status 1 msg)
      file(REMOVE "${dest}")
      message(FATAL_ERROR "Could not fetch ${url}: ${msg}")
    endif()
  endif()
endfunction()

function(fetch_espp_header version component name sha256)
  fetch_pinned(
    "https://raw.githubusercontent.com/esp-cpp/espp/${version}/components/${component}/include/${name}"
    "${CMAKE_CURRENT_BINARY_DIR}/espp-${version}/${component}/${name}" ${sha256})
endfunction()

# ---- espp stream_frame / dispatcher ----------------------------------------
set(ESPP_MANAGED "${OMEGA_ROOT}/managed_components")
if(EXISTS "${ESPP_MANAGED}/espp__stream_frame/include/stream_frame.hpp"
   AND EXISTS "${ESPP_MANAGED}/espp__dispatcher/include/dispatcher.hpp")
  message(STATUS "Using espp stream_frame/dispatcher from managed_components/")
  set(ESPP_INCLUDE_DIRS
      "${ESPP_MANAGED}/espp__stream_frame/include"
      "${ESPP_MANAGED}/espp__dispatcher/include")
else()
  fetch_espp_header(${ESPP_VERSION} stream_frame stream_frame.hpp
    a67f593bbe21484806373ab05b43eb25e77bed98819780f1fc58d8014477deee)
  fetch_espp_header(${ESPP_VERSION} dispatcher dispatcher.hpp
    b0ca02a77cd3029428426cc8acae296b7e05495683d0fc2529118aa4c22c68dc)
  set(ESPP_INCLUDE_DIRS
      "${CMAKE_CURRENT_BINARY_DIR}/espp-${ESPP_VERSION}/stream_frame"
      "${CMAKE_CURRENT_BINARY_DIR}/espp-${ESPP_VERSION}/dispatcher")
endif()

# ---- espp hid-rp (XAC report) ----------------------------------------------
# The pinned component archive from the ESP component registry, the same one
# idf.py unpacks into managed_components/. It is always downloaded, never taken
# from managed_components/, so a local patch to the managed copy cannot make
# the report layout tests pass. The staged copy leaves out the fmt formatter
# headers; stubs/ provides empty stand-ins for those.
set(HID_RP_ARCHIVE "${CMAKE_CURRENT_BINARY_DIR}/espp__hid-rp-v${ESPP_HID_RP_VERSION}.zip")
set(HID_RP_DIR "${CMAKE_CURRENT_BINARY_DIR}/espp__hid-rp-v${ESPP_HID_RP_VERSION}")
fetch_pinned(
  "https://components-file.espressif.com/components/espp/hid-rp/${ESPP_HID_RP_VERSION}/espp__hid-rp-v${ESPP_HID_RP_VERSION}.zip"
  "${HID_RP_ARCHIVE}"
  986766286692d7a4029c3a7021f72b69d6f6aff161b8d096612ab97398490405)
if(NOT EXISTS "${HID_RP_DIR}/host-include/hid-rp-gamepad.hpp")
  file(ARCHIVE_EXTRACT INPUT "${HID_RP_ARCHIVE}" DESTINATION "${HID_RP_DIR}")
  file(COPY "${HID_RP_DIR}/include/" DESTINATION "${HID_RP_DIR}/host-include"
       PATTERN "*-formatters.hpp" EXCLUDE)
endif()

# ---- test binary ------------------------------------------------------------
add_executable(omega_host_tests
  test_main.cpp
  test_persistence.cpp
  test_store.cpp
  test_service.cpp
  test_xac_report.cpp)

target_include_directories(omega_host_tests PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}"
  "${CMAKE_CURRENT_SOURCE_DIR}/stubs"
  ${ESPP_INCLUDE_DIRS}
  "${OMEGA_ROOT}/components/omega_calibration/include"
  "${OMEGA_ROOT}/components/xac/include")
# Third-party headers: keep their warnings out of the -Wall -Wextra build.
target_include_directories(omega_host_tests SYSTEM PRIVATE
  "${HID_RP_DIR}/host-include"
  "${HID_RP_DIR}/detail/hid-rp/hid-rp")

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
