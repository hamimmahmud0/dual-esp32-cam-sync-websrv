// 2026-01-16 sdcard.c
// SPDX-License-Identifier: GPL-3.0-or-later

#include "config.h"
#include "sdcard.h"

#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/unistd.h>
#include <fcntl.h>

#include <esp_log.h>
#include <esp_vfs_fat.h>

#include <driver/sdmmc_host.h>
#include <driver/sdmmc_defs.h>
#include <sdmmc_cmd.h>
#include <driver/gpio.h>

static sdmmc_card_t *s_card = NULL;
static bool s_mounted = false;

static void sdcard_configure_pullups(void)
{
  // Recommended pull-ups for SDMMC lines
  gpio_set_pull_mode(CAMWEBSRV_SDMMC_PIN_CMD, GPIO_PULLUP_ONLY);
  gpio_set_pull_mode(CAMWEBSRV_SDMMC_PIN_D0, GPIO_PULLUP_ONLY);
  gpio_set_pull_mode(CAMWEBSRV_SDMMC_PIN_D1, GPIO_PULLUP_ONLY);
  gpio_set_pull_mode(CAMWEBSRV_SDMMC_PIN_D2, GPIO_PULLUP_ONLY);
  gpio_set_pull_mode(CAMWEBSRV_SDMMC_PIN_D3, GPIO_PULLUP_ONLY);
}

static esp_err_t sdcard_mount_impl(int width)
{
  esp_vfs_fat_sdmmc_mount_config_t mount_config = {
    .format_if_mount_failed = false,
    .max_files = 5,
    .allocation_unit_size = 0,
    .disk_status_check_enable = false,
  };

  sdmmc_host_t host = SDMMC_HOST_DEFAULT();
  sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
  slot_config.width = width;
  slot_config.clk = CAMWEBSRV_SDMMC_PIN_CLK;
  slot_config.cmd = CAMWEBSRV_SDMMC_PIN_CMD;
  slot_config.d0 = CAMWEBSRV_SDMMC_PIN_D0;
  slot_config.d1 = CAMWEBSRV_SDMMC_PIN_D1;
  slot_config.d2 = CAMWEBSRV_SDMMC_PIN_D2;
  slot_config.d3 = CAMWEBSRV_SDMMC_PIN_D3;

  sdcard_configure_pullups();

  return esp_vfs_fat_sdmmc_mount(CAMWEBSRV_SDCARD_MOUNT_PATH, &host, &slot_config, &mount_config, &s_card);
}

esp_err_t camwebsrv_sdcard_mount(bool *used_4bit)
{
  esp_err_t rv;

  if (s_mounted)
  {
    if (used_4bit) *used_4bit = true;
    return ESP_OK;
  }

  // Try 4-bit first
  rv = sdcard_mount_impl(4);
  if (rv == ESP_OK)
  {
    s_mounted = true;
    if (used_4bit) *used_4bit = true;
    ESP_LOGI(CAMWEBSRV_TAG, "SDCARD mounted (4-bit) at %s", CAMWEBSRV_SDCARD_MOUNT_PATH);
    return ESP_OK;
  }

  ESP_LOGW(CAMWEBSRV_TAG, "SDCARD 4-bit mount failed (%s). Falling back to 1-bit.", esp_err_to_name(rv));

  // If 4-bit failed, try 1-bit
  rv = sdcard_mount_impl(1);
  if (rv == ESP_OK)
  {
    s_mounted = true;
    if (used_4bit) *used_4bit = false;
    ESP_LOGI(CAMWEBSRV_TAG, "SDCARD mounted (1-bit) at %s", CAMWEBSRV_SDCARD_MOUNT_PATH);
    return ESP_OK;
  }

  ESP_LOGE(CAMWEBSRV_TAG, "SDCARD mount failed: [%d] %s", rv, esp_err_to_name(rv));
  return rv;
}

esp_err_t camwebsrv_sdcard_unmount(void)
{
  if (!s_mounted)
  {
    return ESP_OK;
  }

  esp_err_t rv = esp_vfs_fat_sdcard_unmount(CAMWEBSRV_SDCARD_MOUNT_PATH, s_card);
  if (rv != ESP_OK)
  {
    ESP_LOGW(CAMWEBSRV_TAG, "SDCARD unmount failed: [%d] %s", rv, esp_err_to_name(rv));
  }
  s_card = NULL;
  s_mounted = false;
  return rv;
}

esp_err_t camwebsrv_sdcard_mkdirs(const char *path)
{
  if (path == NULL || path[0] == 0x00)
  {
    return ESP_ERR_INVALID_ARG;
  }

  // Make a writable copy and create each segment.
  char tmp[256];
  snprintf(tmp, sizeof(tmp), "%s", path);

  for (char *p = tmp + 1; *p; p++)
  {
    if (*p == '/')
    {
      *p = 0;
      if (mkdir(tmp, 0775) != 0 && errno != EEXIST)
      {
        int e = errno;
        ESP_LOGE(CAMWEBSRV_TAG, "SDCARD mkdir(%s) failed: [%d] %s", tmp, e, strerror(e));
        return ESP_FAIL;
      }
      *p = '/';
    }
  }

  if (mkdir(tmp, 0775) != 0 && errno != EEXIST)
  {
    int e = errno;
    ESP_LOGE(CAMWEBSRV_TAG, "SDCARD mkdir(%s) failed: [%d] %s", tmp, e, strerror(e));
    return ESP_FAIL;
  }

  return ESP_OK;
}

esp_err_t camwebsrv_sdcard_write_file(const char *path, const void *data, size_t len)
{
  if (path == NULL || data == NULL)
  {
    return ESP_ERR_INVALID_ARG;
  }

  int fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0664);
  if (fd < 0)
  {
    int e = errno;
    ESP_LOGE(CAMWEBSRV_TAG, "SDCARD open(%s) failed: [%d] %s", path, e, strerror(e));
    return ESP_FAIL;
  }

  const uint8_t *p = (const uint8_t *)data;
  size_t remaining = len;
  while (remaining > 0)
  {
    ssize_t n = write(fd, p, remaining);
    if (n < 0)
    {
      int e = errno;
      ESP_LOGE(CAMWEBSRV_TAG, "SDCARD write(%s) failed: [%d] %s", path, e, strerror(e));
      close(fd);
      return ESP_FAIL;
    }
    p += (size_t)n;
    remaining -= (size_t)n;
  }

  close(fd);
  return ESP_OK;
}
