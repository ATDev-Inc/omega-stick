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
espp_component(base_component 1.3.8 403fb20d320a209683e2daf37ceacaf450f7cdfb345d0d902aa431f505362b3e)
espp_component(logger 1.3.8 cf31ae78b3cbac2fbdaa6ff59e104af1cb41989e8c86b0c2dd3206f0bf786aca)
espp_component(format 1.3.8 f4408bbde3af07e39acc6ff533f1240f6d9ce9c59534672e7149c9af6edea2b9)
espp_component(math 1.3.8 ce8e27b11b5fea41282713721b9c70f713826877bb2738d5570cc4f187ae1552)
espp_component(file_system 1.3.8 4d42f77009d563dab9e397008e2b84be56b0414988eeda99f63de5136305a12b)
espp_component(base_peripheral 1.3.8 7e99e0d4effa9ba3c2fced34ecd0a3d3833222570baaf137ad01a20c0750563a)
espp_component(joystick 1.3.8 e35cf2a5e731af585d60757362ddc785cb205ea83fb77f252465e9688414b442)
espp_component(stream_frame 1.3.8 16a405339127d899cbebedd7bf3c1a7c482f74d7ec0abf48b46b22553335acc0)
espp_component(dispatcher 1.3.8 d51cdc60fece9854974876f0995ace2e941eca7af592fd0b1751071f89923443)

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
