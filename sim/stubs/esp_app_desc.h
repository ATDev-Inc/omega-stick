#pragma once

// Simulator stand-in for ESP-IDF's esp_app_desc.h: the real esp_app_desc_t
// layout, with esp_app_get_description() answering for whichever image slot the
// simulated bootloader started (see sim_flash.hpp).

// C++ only; the guard keeps tools that parse .h files as C (cppcheck in CI)
// from reading the C++ model.
#ifdef __cplusplus
#include "sim_flash.hpp"
#endif
