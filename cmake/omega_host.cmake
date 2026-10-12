# Shared host-build setup for tests/host (unit tests) and sim (desktop
# simulator): the C++ settings, the pinned espp components, the espp_host
# static library built from the real espp sources, and the warning flags.
#
# Include it once from a project() that has CXX enabled:
#
#   include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/omega_host.cmake)
#
# Provides:
#   options  OMEGA_HOST_WERROR, OMEGA_HOST_SANITIZE, OMEGA_HOST_COVERAGE
#   fetch_pinned(url dest sha256), espp_component(name version sha256)
#   target   espp_host            (logger, file_system, joystick; PUBLIC espp includes)
#   function omega_host_apply_flags(<target>)   warnings, sanitizers, coverage
#
# Each espp component comes from managed_components/ when an idf.py build has
# populated it, otherwise the pinned registry archive is downloaded and
# verified by hash.

include_guard(GLOBAL)

option(OMEGA_HOST_WERROR "Treat compiler warnings as errors" OFF)
option(OMEGA_HOST_SANITIZE "Build with AddressSanitizer and UBSan (not MSVC)" OFF)
option(OMEGA_HOST_COVERAGE "Build with --coverage instrumentation (GCC/Clang)" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# ESP-IDF compiles with gnu++2b; the component relies on M_PI from that.
set(CMAKE_CXX_EXTENSIONS ON)
add_compile_definitions(_USE_MATH_DEFINES)

get_filename_component(OMEGA_HOST_REPO "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

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
set(ESPP_MANAGED "${OMEGA_HOST_REPO}/managed_components")
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

if(CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
  # fmt requires /utf-8 (espp_host passes it on to its users as PUBLIC).
  target_compile_options(espp_host PUBLIC /utf-8)
  if(OMEGA_HOST_WERROR)
    target_compile_options(espp_host PRIVATE /WX)
  endif()
else()
  if(OMEGA_HOST_WERROR)
    target_compile_options(espp_host PRIVATE -Werror)
  endif()
  if(OMEGA_HOST_SANITIZE)
    target_compile_options(espp_host PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(espp_host PRIVATE -fsanitize=address,undefined)
  endif()
  if(OMEGA_HOST_COVERAGE)
    target_compile_options(espp_host PRIVATE --coverage)
    target_link_options(espp_host PRIVATE --coverage)
  endif()
endif()

# omega_host_apply_flags(<target>): the warning, sanitizer and coverage flags
# every host executable of this repo is built with.
function(omega_host_apply_flags target)
  # hid-rp's short_item.hpp uses std::bit_cast without including <bit>.
  # libstdc++ pulls it in transitively, MSVC's STL does not; force the include
  # everywhere.
  if(CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    target_compile_options(${target} PRIVATE /W4 /FIbit)
    if(OMEGA_HOST_WERROR)
      target_compile_options(${target} PRIVATE /WX)
    endif()
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -include bit)
    if(OMEGA_HOST_WERROR)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
    if(OMEGA_HOST_SANITIZE)
      target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
      target_link_options(${target} PRIVATE -fsanitize=address,undefined)
    endif()
    if(OMEGA_HOST_COVERAGE)
      target_compile_options(${target} PRIVATE --coverage)
      target_link_options(${target} PRIVATE --coverage)
    endif()
  endif()
  if(MINGW)
    # Let the binary run from shells that do not have the MinGW bin dir on PATH.
    target_link_options(${target} PRIVATE -static)
  endif()
endfunction()
