#pragma once

// Host-test stand-in for the slice of ESP-IDF's esp_image_format.h that
// CalibrationService uses: esp_image_verify() on the other OTA slot, to tell a
// complete image from the start of an upload that never finished (FW_INFO's
// "partial"). Backed by the same esp_ota_fake() as esp_ota_ops.h. C++ only.

#include <stdint.h>

#include "esp_ota_ops.h"

#define ESP_ERR_IMAGE_INVALID 0x2002

typedef struct {
  uint32_t offset;
  uint32_t size;
} esp_partition_pos_t;

typedef enum {
  ESP_IMAGE_VERIFY,
  ESP_IMAGE_VERIFY_SILENT,
} esp_image_load_mode_t;

typedef struct {
  uint32_t start_addr;
  uint32_t image_len;
} esp_image_metadata_t;

inline esp_err_t esp_image_verify(esp_image_load_mode_t, const esp_partition_pos_t *part,
                                  esp_image_metadata_t *data) {
  auto &f = esp_ota_fake();
  ++f.verify_calls;
  if (!part || part->offset != f.other.address || !f.other_has_image || !f.other_verifies) {
    return ESP_ERR_IMAGE_INVALID;
  }
  if (data) {
    data->start_addr = part->offset;
    data->image_len = part->size;
  }
  return ESP_OK;
}
