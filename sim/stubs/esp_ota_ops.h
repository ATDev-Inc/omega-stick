#pragma once

// Simulator stand-in for the slice of ESP-IDF's esp_ota_ops.h that
// CalibrationService and FirmwareHealthCheck use. The two-slot fake behind it
// lives in sim_flash.hpp.

#include "sim_flash.hpp"
