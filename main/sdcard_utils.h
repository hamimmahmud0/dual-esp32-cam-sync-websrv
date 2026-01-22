#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "esp_vfs_fat.h"
#include "config.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *mount_point;                 // e.g. "/sdcard"
    bool format_if_mount_failed;             // usually false
    int max_files;                           // e.g. 5
    size_t allocation_unit_size;             // e.g. 16*1024 (0 = default)

    // SDMMC host settings
    int max_freq_khz;                        // e.g. SDMMC_FREQ_20M / SDMMC_FREQ_52M (0 = default)

    // Slot config
    int slot;                                // usually 1 on ESP32
    int width;                               // 1, 4, or 8 (most SD cards use 1 or 4)

    // Optional: set pins if using GPIO matrix (on some targets).
    // If you don't need to remap pins, leave these as -1.
    int pin_clk;
    int pin_cmd;
    int pin_d0;
    int pin_d1;
    int pin_d2;
    int pin_d3;
    int pin_d4;
    int pin_d5;
    int pin_d6;
    int pin_d7;

    bool internal_pullups;                   // adds SDMMC_SLOT_FLAG_INTERNAL_PULLUP
} sdcard_config_t;

// Mount / Unmount
esp_err_t sdcard_mount(const sdcard_config_t *cfg, sdmmc_card_t **out_card);
esp_err_t sdcard_unmount(const char *mount_point, sdmmc_card_t *card);

// File helpers
bool sdcard_exists(const char *path);
esp_err_t sdcard_write_file(const char *path, const void *data, size_t len, bool append);
esp_err_t sdcard_write_text(const char *path, const char *text, bool append);
esp_err_t sdcard_read_file(const char *path, void *out_buf, size_t max_len, size_t *out_len);
esp_err_t sdcard_read_text(const char *path, char *out_str, size_t max_len); // null-terminated
esp_err_t sdcard_remove(const char *path);
esp_err_t sdcard_rename(const char *from, const char *to);
esp_err_t sdcard_mkdir_p(const char *dir); // creates intermediate dirs if missing
esp_err_t sdcard_list_dir(const char *dir); // logs entries via ESP_LOGI

#ifdef __cplusplus
}
#endif

extern sdmmc_card_t *card;

extern sdcard_config_t sd_cfg;

extern char sd_write_buffer[SD_WRITE_BUFFER_SIZE_KB];