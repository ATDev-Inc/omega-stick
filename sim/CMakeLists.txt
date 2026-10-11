# omega_sim: desktop simulator of the Omega Stick firmware (see docs/simulator.md).
#
#   cmake -S sim -B build-sim
#   cmake --build build-sim
#   ctest --test-dir build-sim --output-on-failure
#
# Also built (and its scenarios run) from the host unit-test build, tests/host.
# Options are the ones in cmake/omega_host.cmake (OMEGA_HOST_WERROR, ...).

cmake_minimum_required(VERSION 3.20)
if(NOT TARGET espp_host)
  project(omega_sim CXX)
  include(${CMAKE_CURRENT_SOURCE_DIR}/../cmake/omega_host.cmake)
  enable_testing()
endif()

find_package(Threads REQUIRED)

set(OMEGA_SIM_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")

# The simulator reports "<version.txt>+sim" as the version it runs.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${OMEGA_SIM_ROOT}/version.txt")
file(READ "${OMEGA_SIM_ROOT}/version.txt" OMEGA_SIM_BASE_VERSION)
string(STRIP "${OMEGA_SIM_BASE_VERSION}" OMEGA_SIM_BASE_VERSION)

add_library(omega_sim_lib STATIC
  src/json.cpp
  src/ws_server.cpp)
target_include_directories(omega_sim_lib PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/src"
  # Stand-ins for the ESP-only OTA headers (espp/ota, esp_ota_ops.h,
  # esp_app_desc.h): a two-slot fake flash. Everything else is the real code.
  "${CMAKE_CURRENT_SOURCE_DIR}/stubs"
  "${OMEGA_SIM_ROOT}/components/omega_calibration/include"
  "${OMEGA_SIM_ROOT}/components/omega_control/include"
  "${OMEGA_SIM_ROOT}/components/tmag5273/include")
target_compile_definitions(omega_sim_lib PUBLIC
  OMEGA_SIM_FW_VERSION="${OMEGA_SIM_BASE_VERSION}+sim")
target_link_libraries(omega_sim_lib PUBLIC espp_host Threads::Threads)
if(WIN32)
  target_link_libraries(omega_sim_lib PUBLIC ws2_32)
endif()
omega_host_apply_flags(omega_sim_lib)

add_executable(omega_sim_tests
  tests/test_main.cpp
  tests/test_json.cpp
  tests/test_ws.cpp)
target_include_directories(omega_sim_tests PRIVATE "${OMEGA_SIM_ROOT}/tests/host")
target_link_libraries(omega_sim_tests PRIVATE omega_sim_lib)
omega_host_apply_flags(omega_sim_tests)
add_test(NAME sim_unit_tests COMMAND omega_sim_tests)
set_tests_properties(sim_unit_tests PROPERTIES TIMEOUT 60)
