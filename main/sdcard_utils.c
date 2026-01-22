#include "sdcard_utils.h"
#include "config.h"

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/unistd.h>
#include <dirent.h>
#include "config.h"

#include "esp_log.h"
#include <rom/ets_sys.h>

static const char *TAG = "sdcard_utils";

sdmmc_card_t *card = NULL;
sdcard_config_t sd_cfg = {
    .mount_point = "/sdcard",
    .format_if_mount_failed = false,
    .max_files = 5,
    .allocation_unit_size = 16 * 1024,
    .max_freq_khz = SDMMC_FREQ_52M,
    .slot = 1,
    .width = 4,                 // 4-bit mode
    .pin_clk = -1, .pin_cmd = -1, .pin_d0 = -1,
    .pin_d1 = -1, .pin_d2 = -1, .pin_d3 = -1,
    .pin_d4 = -1, .pin_d5 = -1, .pin_d6 = -1, .pin_d7 = -1,
    .internal_pullups = true,
};
char sd_write_buffer[SD_WRITE_BUFFER_SIZE_KB];

static bool is_pin_set(int pin) { return pin >= 0; }

esp_err_t sdcard_mount(const sdcard_config_t *sd_cfg, sdmmc_card_t **out_card)
{
    if (!sd_cfg || !sd_cfg->mount_point || !out_card) return ESP_ERR_INVALID_ARG;

    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = sd_cfg->format_if_mount_failed,
        .max_files = (sd_cfg->max_files > 0) ? sd_cfg->max_files : 5,
        .allocation_unit_size = sd_cfg->allocation_unit_size,
    };

    // Host
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    if (sd_cfg->max_freq_khz > 0) host.max_freq_khz = sd_cfg->max_freq_khz;

    // Slot config
    sdmmc_slot_config_t slot_cfg = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_cfg.cd = SDMMC_SLOT_NO_CD;
    slot_cfg.wp = SDMMC_SLOT_NO_WP;



    // Width
    if (sd_cfg->width == 1 || sd_cfg->width == 4 || sd_cfg->width == 8) {
        slot_cfg.width = sd_cfg->width;
    }

    // Optional internal pullups (still recommend external ~10k pullups!)
    if (sd_cfg->internal_pullups) {
        slot_cfg.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
    }

    // Optional GPIO matrix pin mapping (only if supported by SOC + you provided pins)
#if CONFIG_SOC_SDMMC_USE_GPIO_MATRIX
    if (is_pin_set(sd_cfg->pin_clk)) slot_cfg.clk = sd_cfg->pin_clk;
    if (is_pin_set(sd_cfg->pin_cmd)) slot_cfg.cmd = sd_cfg->pin_cmd;
    if (is_pin_set(sd_cfg->pin_d0))  slot_cfg.d0  = sd_cfg->pin_d0;

    if (sd_cfg->width >= 4) {
        if (is_pin_set(sd_cfg->pin_d1)) slot_cfg.d1 = sd_cfg->pin_d1;
        if (is_pin_set(sd_cfg->pin_d2)) slot_cfg.d2 = sd_cfg->pin_d2;
        if (is_pin_set(sd_cfg->pin_d3)) slot_cfg.d3 = sd_cfg->pin_d3;
    }
    if (sd_cfg->width >= 8) {
        if (is_pin_set(sd_cfg->pin_d4)) slot_cfg.d4 = sd_cfg->pin_d4;
        if (is_pin_set(sd_cfg->pin_d5)) slot_cfg.d5 = sd_cfg->pin_d5;
        if (is_pin_set(sd_cfg->pin_d6)) slot_cfg.d6 = sd_cfg->pin_d6;
        if (is_pin_set(sd_cfg->pin_d7)) slot_cfg.d7 = sd_cfg->pin_d7;
    }
#endif

    ESP_LOGI(TAG, "Mounting SD card at %s", sd_cfg->mount_point);

    sdmmc_card_t *card = NULL;
    esp_err_t ret = esp_vfs_fat_sdmmc_mount(sd_cfg->mount_point, &host, &slot_cfg, &mount_cfg, &card);
    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Mount failed (ESP_FAIL). format_if_mount_failed=%d", sd_cfg->format_if_mount_failed);
        } else {
            ESP_LOGE(TAG, "SDMMC init/mount failed: %s", esp_err_to_name(ret));
        }
        return ret;
    }

    sdmmc_card_print_info(stdout, card);
    *out_card = card;
    return ESP_OK;
}

esp_err_t sdcard_unmount(const char *mount_point, sdmmc_card_t *card)
{
    if (!mount_point || !card) return ESP_ERR_INVALID_ARG;
    esp_vfs_fat_sdcard_unmount(mount_point, card);
    ESP_LOGI(TAG, "Unmounted %s", mount_point);
    return ESP_OK;
}

bool sdcard_exists(const char *path)
{
    struct stat st;
    return (path && stat(path, &st) == 0);
}

esp_err_t sdcard_write_file(const char *path, const void *data, size_t len, bool append)
{
    if (!path || (!data && len > 0)) return ESP_ERR_INVALID_ARG;

    const char *mode = append ? "ab" : "wb";
    FILE *f = fopen(path, mode);
    if (!f) {
        ets_printf("%s: fopen(%s) failed: errno=%d (%s)\n",TAG, path, errno, strerror(errno));
        return ESP_FAIL;
    }

    size_t written = 0;
    if (len > 0) written = fwrite(data, 1, len, f);

    fclose(f);

    if (written != len) {
        ESP_LOGE(TAG, "Short write %s: %u/%u", path, (unsigned)written, (unsigned)len);
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t sdcard_write_text(const char *path, const char *text, bool append)
{
    if (!path || !text) return ESP_ERR_INVALID_ARG;
    return sdcard_write_file(path, text, strlen(text), append);
}

esp_err_t sdcard_read_file(const char *path, void *out_buf, size_t max_len, size_t *out_len)
{
    if (!path || !out_buf || max_len == 0) return ESP_ERR_INVALID_ARG;

    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "fopen(%s) failed: errno=%d (%s)", path, errno, strerror(errno));
        return ESP_FAIL;
    }

    size_t n = fread(out_buf, 1, max_len, f);
    fclose(f);

    if (out_len) *out_len = n;
    return ESP_OK;
}

esp_err_t sdcard_read_text(const char *path, char *out_str, size_t max_len)
{
    if (!out_str || max_len < 2) return ESP_ERR_INVALID_ARG;

    size_t n = 0;
    esp_err_t ret = sdcard_read_file(path, out_str, max_len - 1, &n);
    if (ret != ESP_OK) return ret;

    out_str[n] = '\0';
    return ESP_OK;
}

esp_err_t sdcard_remove(const char *path)
{
    if (!path) return ESP_ERR_INVALID_ARG;
    if (unlink(path) != 0) {
        ESP_LOGE(TAG, "unlink(%s) failed: errno=%d (%s)", path, errno, strerror(errno));
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t sdcard_rename(const char *from, const char *to)
{
    if (!from || !to) return ESP_ERR_INVALID_ARG;

    // If destination exists, remove it (optional convenience)
    if (sdcard_exists(to)) {
        if (unlink(to) != 0) {
            ESP_LOGE(TAG, "unlink(%s) failed: errno=%d (%s)", to, errno, strerror(errno));
            return ESP_FAIL;
        }
    }

    if (rename(from, to) != 0) {
        ESP_LOGE(TAG, "rename(%s -> %s) failed: errno=%d (%s)", from, to, errno, strerror(errno));
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t sdcard_mkdir_p(const char *dir)
{
    if (!dir || dir[0] == '\0') return ESP_ERR_INVALID_ARG;

    char tmp[256];
    size_t len = strnlen(dir, sizeof(tmp));
    if (len >= sizeof(tmp)) return ESP_ERR_INVALID_SIZE;

    memcpy(tmp, dir, len);
    tmp[len] = '\0';

    // Create intermediate directories
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (!sdcard_exists(tmp)) {
                if (mkdir(tmp, 0775) != 0 && errno != EEXIST) {
                    ESP_LOGE(TAG, "mkdir(%s) failed: errno=%d (%s)", tmp, errno, strerror(errno));
                    return ESP_FAIL;
                }
            }
            *p = '/';
        }
    }

    if (!sdcard_exists(tmp)) {
        if (mkdir(tmp, 0775) != 0 && errno != EEXIST) {
            ESP_LOGE(TAG, "mkdir(%s) failed: errno=%d (%s)", tmp, errno, strerror(errno));
            return ESP_FAIL;
        }
    }
    return ESP_OK;
}

esp_err_t sdcard_list_dir(const char *dir)
{
    if (!dir) return ESP_ERR_INVALID_ARG;

    DIR *d = opendir(dir);
    if (!d) {
        ESP_LOGE(TAG, "opendir(%s) failed: errno=%d (%s)", dir, errno, strerror(errno));
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Listing dir: %s", dir);
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        ESP_LOGI(TAG, "  %s", e->d_name);
    }
    closedir(d);
    return ESP_OK;
}
