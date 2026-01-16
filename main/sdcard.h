// 2026-01-16 sdcard.h
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef _CAMWEBSRV_SDCARD_H
#define _CAMWEBSRV_SDCARD_H

#include <esp_err.h>
#include <stdbool.h>
#include <stddef.h>

// Mount SD card at CAMWEBSRV_SDCARD_MOUNT_PATH (see config.h).
// Tries 4-bit SDMMC; if that fails, falls back to 1-bit.
esp_err_t camwebsrv_sdcard_mount(bool *used_4bit);
esp_err_t camwebsrv_sdcard_unmount(void);

// Ensure a directory exists (mkdir -p style).
esp_err_t camwebsrv_sdcard_mkdirs(const char *path);

// Write a binary file (overwrites if exists).
esp_err_t camwebsrv_sdcard_write_file(const char *path, const void *data, size_t len);

#endif
