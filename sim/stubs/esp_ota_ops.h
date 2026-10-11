#pragma once

// Simulator stand-in for the slice of ESP-IDF's esp_ota_ops.h that
// CalibrationService and FirmwareHealthCheck use. The two-slot fake behind it
// lives in sim_flash.hpp.

// C++ only; the guard keeps tools that parse .h files as C (cppcheck in CI)
// from reading the C++ model.
#ifdef __cplusplus
#include "sim_flash.hpp"
#endif
