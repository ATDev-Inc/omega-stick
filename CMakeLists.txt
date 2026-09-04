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
  "main esptool_py freertos tmag5273 usb_device logger i2c base_component"
  CACHE STRING
  "List of components to include"
  )

project(omega_stick)

set(CMAKE_CXX_STANDARD 20)
