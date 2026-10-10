idf_component_register(
  SRC_DIRS "."
  INCLUDE_DIRS "."
  REQUIRES tmag5273 usb_device dispatcher logger i2c base_component freertos tinyusb qtpy hid-rp-mouse hid-rp omega_calibration omega_control xac
)
littlefs_create_partition_image(littlefs ../files FLASH_IN_PROJECT)