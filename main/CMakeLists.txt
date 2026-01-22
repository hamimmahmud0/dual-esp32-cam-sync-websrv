# 2023-01-03 CMakeLists.txt
# Copyright (C) 2023 Vino Fernando Crescini  <vfcrescini@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later



idf_component_register(
  SRCS "sd_bench.c" "sdcard_utils.c" "main.c" "camera.c" "cfgman.c" "httpd.c" "ping.c" "sclients.c" "storage.c" "vbytes.c" "wifi.c" "sdcard.c" "seqcap.c"
  PRIV_REQUIRES "esp_event" "esp_http_client" "esp_http_server" "esp_timer" "esp_wifi" "fatfs" "freertos" "lwip" "mdns" "nvs_flash" "vfs" "sdmmc" "driver"
  PRIV_INCLUDE_DIRS "."
)

fatfs_create_rawflash_image("storage" "../storage" FLASH_IN_PROJECT PRESERVE_TIME)
