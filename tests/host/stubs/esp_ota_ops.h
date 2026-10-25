#pragma once

// Host-test stand-in for the slice of ESP-IDF's esp_ota_ops.h that
// CalibrationService uses (FW_INFO "previous slot" fields and REVERT). All
// behaviour comes from one global esp_ota_fake() that tests set up and inspect;
// OtaFixture resets it for every test. C++ only (the real API is C linkage).

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"

typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NOT_FOUND 0x105

inline const char *esp_err_to_name(esp_err_t err) {
  switch (err) {
  case ESP_OK:
    return "ESP_OK";
  case ESP_FAIL:
    return "ESP_FAIL";
  case ESP_ERR_NOT_FOUND:
    return "ESP_ERR_NOT_FOUND";
  default:
    return "UNKNOWN ERROR";
  }
}

struct esp_partition_t {
  char label[17];
  uint32_t address;
  uint32_t size;
};

typedef enum {
  ESP_OTA_IMG_NEW = 0,
  ESP_OTA_IMG_PENDING_VERIFY = 1,
  ESP_OTA_IMG_VALID = 2,
  ESP_OTA_IMG_INVALID = 3,
  ESP_OTA_IMG_ABORTED = 4,
  ESP_OTA_IMG_UNDEFINED = -1,
} esp_ota_img_states_t;

struct esp_ota_ops_fake {
  bool has_other_partition{true};
  esp_partition_t other{"ota_1", 0x1F0000, 0x1E0000};
  bool other_has_image{true};
  esp_app_desc_t other_desc{};
  esp_ota_img_states_t other_state{ESP_OTA_IMG_VALID};
  /// False models a slot holding the start of an image that never finished
  /// uploading: its descriptor reads, but esp_image_verify() fails.
  bool other_verifies{true};
  int verify_calls{0};
  esp_err_t set_boot_result{ESP_OK};
  int set_boot_calls{0};
  const esp_partition_t *last_set_boot{nullptr};

  esp_ota_ops_fake() { reset(); }

  void reset() {
    has_other_partition = true;
    memset(&other, 0, sizeof(other));
    snprintf(other.label, sizeof(other.label), "%s", "ota_1");
    other.address = 0x1F0000;
    other.size = 0x1E0000;
    other_has_image = true;
    memset(&other_desc, 0, sizeof(other_desc));
    other_desc.magic_word = 0xABCD5432u;
    snprintf(other_desc.version, sizeof(other_desc.version), "%s", "0.0.9-prev");
    snprintf(other_desc.project_name, sizeof(other_desc.project_name), "%s", "omega_stick");
    snprintf(other_desc.time, sizeof(other_desc.time), "%s", "01:02:03");
    snprintf(other_desc.date, sizeof(other_desc.date), "%s", "Jan  2 2026");
    other_state = ESP_OTA_IMG_VALID;
    other_verifies = true;
    verify_calls = 0;
    set_boot_result = ESP_OK;
    set_boot_calls = 0;
    last_set_boot = nullptr;
  }
};

inline esp_ota_ops_fake &esp_ota_fake() {
  static esp_ota_ops_fake fake;
  return fake;
}

inline const esp_partition_t *esp_ota_get_next_update_partition(const esp_partition_t *) {
  auto &f = esp_ota_fake();
  return f.has_other_partition ? &f.other : nullptr;
}

inline esp_err_t esp_ota_get_partition_description(const esp_partition_t *part,
                                                   esp_app_desc_t *desc) {
  auto &f = esp_ota_fake();
  if (part != &f.other || !f.other_has_image) {
    return ESP_ERR_NOT_FOUND;
  }
  *desc = f.other_desc;
  return ESP_OK;
}

inline esp_err_t esp_ota_get_state_partition(const esp_partition_t *part,
                                             esp_ota_img_states_t *state) {
  auto &f = esp_ota_fake();
  if (part != &f.other || !f.other_has_image) {
    return ESP_ERR_NOT_FOUND;
  }
  *state = f.other_state;
  return ESP_OK;
}

inline esp_err_t esp_ota_set_boot_partition(const esp_partition_t *part) {
  auto &f = esp_ota_fake();
  ++f.set_boot_calls;
  f.last_set_boot = part;
  return f.set_boot_result;
}
