#pragma once

// Simulator stand-in for the slice of ESP-IDF's esp_image_format.h that
// CalibrationService uses (esp_image_verify() on the other OTA slot). The
// two-slot fake behind it lives in sim_flash.hpp.

// C++ only; the guard keeps tools that parse .h files as C (cppcheck in CI)
// from reading the C++ model.
#ifdef __cplusplus
#include "sim_flash.hpp"
#endif
