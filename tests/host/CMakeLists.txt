# Host-side unit tests for components/omega_calibration.
#
# Builds the calibration headers against the REAL espp components (logger,
# format/fmt, base_component, math, file_system, joystick, stream_frame,
# dispatcher, hid-rp), pinned to the versions in dependencies.lock, so the
# tests run the same code the firmware does.
#
#   cmake -S tests/host -B build-host
#   cmake --build build-host
#   ctest --test-dir build-host --output-on-failure
#
# Options (all default OFF):
#   OMEGA_HOST_WERROR    warnings as errors (-Werror or /WX)
#   OMEGA_HOST_SANITIZE  AddressSanitizer + UBSan (GCC/Clang frontends only)
#   OMEGA_HOST_COVERAGE  --coverage instrumentation (GCC/Clang only)
#   OMEGA_HOST_FUZZ      libFuzzer target omega_frame_fuzz (Clang, needs OMEGA_HOST_SANITIZE)
#
# Each espp component comes from managed_components/ when an idf.py build has
# populated it, otherwise the pinned registry archive is downloaded and
# verified by hash. The hid-rp headers used by the XAC report tests are always
# downloaded, never taken from managed_components/.

cmake_minimum_required(VERSION 3.20)
project(omega_stick_host_tests CXX)

option(OMEGA_HOST_WERROR "Treat compiler warnings as errors" OFF)
option(OMEGA_HOST_SANITIZE "Build with AddressSanitizer and UBSan (not MSVC)" OFF)
option(OMEGA_HOST_COVERAGE "Build with --coverage instrumentation (GCC/Clang)" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# ESP-IDF compiles with gnu++2b; the component relies on M_PI from that.
set(CMAKE_CXX_EXTENSIONS ON)
add_compile_definitions(_USE_MATH_DEFINES)

set(OMEGA_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")
set(ESPP_HID_RP_VERSION "1.3.6") # espp/hid-rp in dependencies.lock

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

# espp_component(<name> <version> <sha256>)
# Sets ESPP_<NAME>_DIR to the component root: managed_components/espp__<name>
# when present, else the pinned registry archive extracted into the build dir.
set(ESPP_MANAGED "${OMEGA_ROOT}/managed_components")
function(espp_component name version sha256)
  set(managed "${ESPP_MANAGED}/espp__${name}")
  if(IS_DIRECTORY "${managed}/include")
    message(STATUS "Using espp ${name} from managed_components/")
    set(root "${managed}")
  else()
    set(archive "${CMAKE_CURRENT_BINARY_DIR}/espp__${name}-v${version}.zip")
    set(root "${CMAKE_CURRENT_BINARY_DIR}/espp__${name}-v${version}")
    fetch_pinned(
      "https://components-file.espressif.com/components/espp/${name}/${version}/espp__${name}-v${version}.zip"
      "${archive}" ${sha256})
    if(NOT IS_DIRECTORY "${root}/include")
      file(ARCHIVE_EXTRACT INPUT "${archive}" DESTINATION "${root}")
    endif()
  endif()
  string(TOUPPER "${name}" upper)
  set(ESPP_${upper}_DIR "${root}" PARENT_SCOPE)
endfunction()

# Versions and hashes match dependencies.lock.
espp_component(base_component 1.3.6 e16df411ad708e5b03af8a3d1e4134e10ca9f58877633f0b76a7f44e14d3c5b3)
espp_component(logger 1.3.6 440c97928209a67f53361f02ee0d9d040052d42062730f34e8adf55924fbe3c9)
espp_component(format 1.3.6 f7e9251b26e91e3d7e84b594fb64cabe52e8887151e9760987775873b8272b67)
espp_component(math 1.3.6 2a666a01c8c50edad8919357832ae65b6c7090a0e7e8eea250d1d1218d05908c)
espp_component(file_system 1.3.6 bf7b406429057e8cb10f0939bc7ed87b8486d9a522a86c03363d74439bab65b1)
espp_component(base_peripheral 1.3.6 456f6b2e2c3fa44d63de580e7cc6e44ccdacee01e7b00bcd5e3b639b68550c37)
espp_component(joystick 1.3.6 27525b1a6282de74e7ffa9dec445f09742fe6bcd975a9dc98665f6ab9f66eb4a)
espp_component(stream_frame 1.3.6 0a617522d51e017d9fe7c3dee3f77836455fccd35d3b69b6a1bdcef31623a015)
espp_component(dispatcher 1.3.6 114470a25298c1aaef56a832e7d964fa847218cc9c99d070c8b758f7b8f2026e)

set(ESPP_INCLUDE_DIRS
    "${ESPP_BASE_COMPONENT_DIR}/include"
    "${ESPP_LOGGER_DIR}/include"
    # The format component exposes the vendored fmt next to its own header.
    "${ESPP_FORMAT_DIR}/include"
    "${ESPP_FORMAT_DIR}/detail/fmt/include"
    "${ESPP_MATH_DIR}/include"
    "${ESPP_FILE_SYSTEM_DIR}/include"
    "${ESPP_BASE_PERIPHERAL_DIR}/include"
    "${ESPP_JOYSTICK_DIR}/include"
    "${ESPP_STREAM_FRAME_DIR}/include"
    "${ESPP_DISPATCHER_DIR}/include")

# ---- espp hid-rp (XAC report) ----------------------------------------------
# The pinned component archive from the ESP component registry, the same one
# idf.py unpacks into managed_components/. It is always downloaded, never taken
# from managed_components/, so a local patch to the managed copy cannot make
# the report layout tests pass.
set(HID_RP_ARCHIVE "${CMAKE_CURRENT_BINARY_DIR}/espp__hid-rp-v${ESPP_HID_RP_VERSION}.zip")
set(HID_RP_DIR "${CMAKE_CURRENT_BINARY_DIR}/espp__hid-rp-v${ESPP_HID_RP_VERSION}")
fetch_pinned(
  "https://components-file.espressif.com/components/espp/hid-rp/${ESPP_HID_RP_VERSION}/espp__hid-rp-v${ESPP_HID_RP_VERSION}.zip"
  "${HID_RP_ARCHIVE}"
  11ad7ca6a4d2030f3af76d771ba8857d32db7c94a48d42770c2256da6c667007)
if(NOT EXISTS "${HID_RP_DIR}/include/hid-rp-gamepad.hpp")
  file(ARCHIVE_EXTRACT INPUT "${HID_RP_ARCHIVE}" DESTINATION "${HID_RP_DIR}")
endif()

# ---- real espp sources ------------------------------------------------------
# The same files espp's own desktop build compiles. fmt is header-only here:
# the format component's format.hpp defines FMT_HEADER_ONLY itself, so there
# is nothing to build for it.
add_library(espp_host STATIC
  "${ESPP_LOGGER_DIR}/src/logger.cpp"
  "${ESPP_FILE_SYSTEM_DIR}/src/file_system.cpp"
  "${ESPP_JOYSTICK_DIR}/src/joystick.cpp")
target_include_directories(espp_host SYSTEM PUBLIC ${ESPP_INCLUDE_DIRS})
if(WIN32)
  # file_system.cpp calls std::localtime, which the Windows CRT flags as
  # deprecated. SYSTEM includes cannot hide a warning in a .cpp we compile.
  target_compile_definitions(espp_host PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

# ---- test binary ------------------------------------------------------------
add_executable(omega_host_tests
  test_main.cpp
  test_persistence.cpp
  test_store.cpp
  test_service.cpp
  test_xac_report.cpp
  test_sensor_fault.cpp
  test_reset_log.cpp
  test_health_check.cpp
  test_calibration_math.cpp
  test_drift.cpp
  test_tmag5273.cpp
  test_mouse_report.cpp
  test_hid_descriptor.cpp
  test_control.cpp
  test_golden_frames.cpp)

# Shared golden frames: generated by test_golden_frames.cpp, read by tests/web.
target_compile_definitions(omega_host_tests PRIVATE
  OMEGA_PROTOCOL_DIR="${OMEGA_ROOT}/tests/protocol")

target_include_directories(omega_host_tests PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}"
  "${OMEGA_ROOT}/components/omega_calibration/include"
  "${OMEGA_ROOT}/components/omega_control/include"
  "${OMEGA_ROOT}/components/xac/include"
  "${OMEGA_ROOT}/components/tmag5273/include"
  "${OMEGA_ROOT}/components/hid-rp-mouse/include"
  # Stand-ins for the ESP-only OTA headers (espp/ota, esp_ota_ops.h,
  # esp_app_desc.h); everything else is the real espp code.
  "${CMAKE_CURRENT_SOURCE_DIR}/stubs")
# Third-party headers: keep their warnings out of the warning-clean build.
target_include_directories(omega_host_tests SYSTEM PRIVATE
  "${HID_RP_DIR}/include"
  "${HID_RP_DIR}/detail/hid-rp/hid-rp")
target_link_libraries(omega_host_tests PRIVATE espp_host)

# ---- compiler flags ---------------------------------------------------------
# hid-rp's short_item.hpp uses std::bit_cast without including <bit>. libstdc++
# pulls it in transitively, MSVC's STL does not; force the include everywhere.
if(CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
  # fmt requires /utf-8 (espp_host passes it on to the tests as PUBLIC).
  target_compile_options(espp_host PUBLIC /utf-8)
  target_compile_options(omega_host_tests PRIVATE /W4 /FIbit)
  # hid-rp-mouse.hpp narrows the int button index to uint16_t (C4244); this is
  # the known bug mouse_set_button_rejects_indices_that_wrap_into_range. Only
  # the translation units that include that header are exempted.
  set_source_files_properties(test_mouse_report.cpp test_hid_descriptor.cpp
                              PROPERTIES COMPILE_OPTIONS "/wd4244")
  if(OMEGA_HOST_WERROR)
    target_compile_options(omega_host_tests PRIVATE /WX)
    target_compile_options(espp_host PRIVATE /WX)
  endif()
else()
  target_compile_options(omega_host_tests PRIVATE -Wall -Wextra -include bit)
  if(OMEGA_HOST_WERROR)
    target_compile_options(omega_host_tests PRIVATE -Werror)
    target_compile_options(espp_host PRIVATE -Werror)
  endif()
  if(OMEGA_HOST_SANITIZE)
    foreach(t omega_host_tests espp_host)
      target_compile_options(${t} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
      target_link_options(${t} PRIVATE -fsanitize=address,undefined)
    endforeach()
  endif()
  if(OMEGA_HOST_COVERAGE)
    foreach(t omega_host_tests espp_host)
      target_compile_options(${t} PRIVATE --coverage)
      target_link_options(${t} PRIVATE --coverage)
    endforeach()
  endif()
endif()
if(MINGW)
  # Let the binary run from shells that do not have the MinGW bin dir on PATH.
  target_link_options(omega_host_tests PRIVATE -static)
endif()

# ---- libFuzzer harness (tests/fuzz) ------------------------------------------
# Clang only; needs OMEGA_HOST_SANITIZE so espp_host carries the same
# instrumentation. Not part of the default build or of ctest.
option(OMEGA_HOST_FUZZ "Build the libFuzzer frame fuzzer (Clang, needs OMEGA_HOST_SANITIZE)" OFF)
if(OMEGA_HOST_FUZZ)
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang" OR
     CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    message(FATAL_ERROR "OMEGA_HOST_FUZZ needs the Clang GNU-style frontend (clang++)")
  endif()
  if(NOT OMEGA_HOST_SANITIZE)
    message(FATAL_ERROR "OMEGA_HOST_FUZZ requires OMEGA_HOST_SANITIZE=ON")
  endif()
  add_executable(omega_frame_fuzz "${OMEGA_ROOT}/tests/fuzz/frame_fuzz.cpp")
  target_include_directories(omega_frame_fuzz PRIVATE
    "${OMEGA_ROOT}/components/omega_calibration/include"
    # calibration_service.hpp includes the ESP-only OTA headers.
    "${CMAKE_CURRENT_SOURCE_DIR}/stubs")
  target_compile_options(omega_frame_fuzz PRIVATE
    -Wall -Wextra -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer)
  target_link_options(omega_frame_fuzz PRIVATE -fsanitize=fuzzer,address,undefined)
  target_link_libraries(omega_frame_fuzz PRIVATE espp_host)
endif()

enable_testing()
add_test(NAME omega_host_tests COMMAND omega_host_tests)
set_tests_properties(omega_host_tests PROPERTIES TIMEOUT 60)
