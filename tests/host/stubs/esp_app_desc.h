#pragma once

// Host-test stand-in for ESP-IDF's esp_app_desc.h. Same layout and field
// names as the real esp_app_desc_t; esp_app_get_description() returns a fixed
// descriptor so tests can assert on the FW_INFO strings.

#include <stdint.h>

typedef struct {
  uint32_t magic_word;
  uint32_t secure_version;
  uint32_t reserv1[2];
  char version[32];
  char project_name[32];
  char time[16];
  char date[16];
  char idf_ver[32];
  uint8_t app_elf_sha256[32];
  uint16_t min_efuse_blk_rev_full;
  uint16_t max_efuse_blk_rev_full;
  uint8_t mmu_page_size;
  uint8_t reserv3[3];
  uint32_t reserv2[18];
} esp_app_desc_t;

static inline const esp_app_desc_t *esp_app_get_description(void) {
  static const esp_app_desc_t desc = {
      0xABCD5432u,
      0,
      {0, 0},
      "v9.9.9-test",
      "omega_stick",
      "12:34:56",
      "Oct 10 2026",
      "v6.0.2",
      {0xde, 0xad, 0xbe, 0xef},
      0,
      0,
      0,
      {0, 0, 0},
      {0},
  };
  return &desc;
}
