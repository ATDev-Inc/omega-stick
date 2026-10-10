# The following lines of boilerplate have to be in your project's CMakeLists
# in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.20)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# add the component directories that we want to use
set(EXTRA_COMPONENT_DIRS

  # Project-local components. Add new drivers as components/<name>/ and list
  # them in the COMPONENTS variable below.
  "components/"
)

set(
  COMPONENTS
  # Components pulled from the registry via `idf.py add-dependency` do not need
  # to be listed here; see main/idf_component.yml.
  "main esptool_py freertos tmag5273 usb_device logger i2c base_component hid-rp-mouse tinyusb qtpy hid-rp omega_calibration file_system joystick xac coredump stream_frame dispatcher"
  CACHE STRING
  "List of components to include"
  )

# Firmware version string, embedded in the app image (esp_app_desc_t::version),
# printed at boot and reported by the calibration console's GET_FW_INFO. Set
# before project() so ESP-IDF picks it up as PROJECT_VER.
#
# Numbering scheme (see docs/firmware-updates.md):
#   version.txt holds MAJOR.MINOR.PATCH, bumped by hand in the PR that warrants
#   it. MAJOR: saved calibration/settings or the console protocol become
#   incompatible. MINOR: new user-visible feature. PATCH: fixes only.
#
# What a build embeds:
#   "0.1.0"               release: clean tree, HEAD tagged exactly v0.1.0
#   "0.1.0+1a2b3c4"       any other build of commit 1a2b3c4
#   "0.1.0+1a2b3c4.dirty" built with uncommitted changes
#   "0.1.0+unknown"       git unavailable (e.g. a source tarball)
# `idf.py -DPROJECT_VER=...` overrides all of this.
#
# Keep the result under esp_app_desc_t's 32-byte limit (31 characters).
#
# Incremental builds: setting PROJECT_VER here skips ESP-IDF's own version.txt
# reader, which is what would otherwise make CMake re-run when the file
# changes, so the inputs are registered as configure dependencies below:
# version.txt, git's HEAD, and the branch ref HEAD points at. A commit or a
# version bump therefore re-runs CMake on the next `idf.py build`. The ".dirty"
# suffix is not tracked that way (it would mean re-running CMake on every
# edit), so it reflects the tree at the last configure; run `idf.py reconfigure`
# before a build whose dirty state matters.
if(NOT DEFINED PROJECT_VER)
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
               "${CMAKE_CURRENT_SOURCE_DIR}/version.txt")
  file(READ "${CMAKE_CURRENT_SOURCE_DIR}/version.txt" _base_version)
  string(STRIP "${_base_version}" _base_version)
  if(NOT _base_version MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
    message(FATAL_ERROR "version.txt must contain MAJOR.MINOR.PATCH, got '${_base_version}'")
  endif()

  set(_sha "")
  set(_dirty FALSE)
  set(_at_release_tag FALSE)
  find_package(Git QUIET)
  if(GIT_FOUND)
    execute_process(
      COMMAND ${GIT_EXECUTABLE} rev-parse --short=7 HEAD
      WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
      OUTPUT_VARIABLE _sha OUTPUT_STRIP_TRAILING_WHITESPACE
      ERROR_QUIET RESULT_VARIABLE _rc)
    if(NOT _rc EQUAL 0)
      set(_sha "")
    endif()
    # --git-path resolves worktrees (refs live in the common git dir) and
    # packed refs. A detached HEAD only needs HEAD itself.
    execute_process(
      COMMAND ${GIT_EXECUTABLE} rev-parse --symbolic-full-name HEAD
      WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
      OUTPUT_VARIABLE _head_ref OUTPUT_STRIP_TRAILING_WHITESPACE
      ERROR_QUIET RESULT_VARIABLE _rc)
    set(_git_paths HEAD packed-refs)
    if(_rc EQUAL 0 AND _head_ref MATCHES "^refs/")
      list(APPEND _git_paths "${_head_ref}")
    endif()
    foreach(_git_path IN LISTS _git_paths)
      execute_process(
        COMMAND ${GIT_EXECUTABLE} rev-parse --path-format=absolute --git-path ${_git_path}
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        OUTPUT_VARIABLE _git_file OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET RESULT_VARIABLE _rc)
      if(_rc EQUAL 0 AND EXISTS "${_git_file}")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_git_file}")
      endif()
    endforeach()
    execute_process(
      COMMAND ${GIT_EXECUTABLE} status --porcelain --untracked-files=no
      WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
      OUTPUT_VARIABLE _status OUTPUT_STRIP_TRAILING_WHITESPACE
      ERROR_QUIET RESULT_VARIABLE _rc)
    if(_rc EQUAL 0 AND NOT _status STREQUAL "")
      set(_dirty TRUE)
    endif()
    execute_process(
      COMMAND ${GIT_EXECUTABLE} describe --tags --exact-match HEAD
      WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
      OUTPUT_VARIABLE _tag OUTPUT_STRIP_TRAILING_WHITESPACE
      ERROR_QUIET RESULT_VARIABLE _rc)
    if(_rc EQUAL 0 AND _tag STREQUAL "v${_base_version}")
      set(_at_release_tag TRUE)
    endif()
  endif()
  # GitHub Actions: git may refuse to run in the checkout (ownership checks),
  # so fall back to the environment for the commit and the release tag.
  if(_sha STREQUAL "" AND DEFINED ENV{GITHUB_SHA})
    string(SUBSTRING "$ENV{GITHUB_SHA}" 0 7 _sha)
    if(DEFINED ENV{GITHUB_REF_TYPE} AND "$ENV{GITHUB_REF_TYPE}" STREQUAL "tag"
       AND "$ENV{GITHUB_REF_NAME}" STREQUAL "v${_base_version}")
      set(_at_release_tag TRUE)
    endif()
  endif()
  if(_sha STREQUAL "")
    set(_sha "unknown")
  endif()

  if(_at_release_tag AND NOT _dirty)
    set(PROJECT_VER "${_base_version}")
  else()
    set(PROJECT_VER "${_base_version}+${_sha}")
    if(_dirty)
      string(APPEND PROJECT_VER ".dirty")
    endif()
  endif()
endif()
message(STATUS "omega_stick firmware version: ${PROJECT_VER}")

project(omega_stick)

set(CMAKE_CXX_STANDARD 20)
