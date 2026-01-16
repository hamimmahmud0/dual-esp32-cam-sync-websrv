// 2026-01-16 seqcap.c
// SPDX-License-Identifier: GPL-3.0-or-later

#include "config.h"
#include "seqcap.h"
#include "sdcard.h"
#include "httpd.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include <esp_log.h>
#include <esp_timer.h>
#include <esp_http_client.h>
#include <esp_wifi.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

#include <driver/gpio.h>
#include <rom/ets_sys.h>

static volatile bool s_active = false;

bool camwebsrv_seqcap_is_active(void)
{
  return s_active;
}

static void blink_pattern(void)
{
  // One long blink, then two short blinks.
  // NOTE: On ESP32-CAM, GPIO4 is often SD D1 when using 4-bit SDMMC.
  // We'll blink only after unmounting SD.
  gpio_set_direction(CAMWEBSRV_PIN_FLASH, GPIO_MODE_OUTPUT);

  gpio_set_level(CAMWEBSRV_PIN_FLASH, 1);
  vTaskDelay(pdMS_TO_TICKS(600));
  gpio_set_level(CAMWEBSRV_PIN_FLASH, 0);
  vTaskDelay(pdMS_TO_TICKS(300));

  for (int i = 0; i < 2; i++)
  {
    gpio_set_level(CAMWEBSRV_PIN_FLASH, 1);
    vTaskDelay(pdMS_TO_TICKS(180));
    gpio_set_level(CAMWEBSRV_PIN_FLASH, 0);
    vTaskDelay(pdMS_TO_TICKS(180));
  }
}

static const char *framesize_to_str(framesize_t fs)
{
  // esp32-camera uses these enum names; this is only for filenames.
  switch (fs)
  {
    case FRAMESIZE_QQVGA: return "qqvga";
#ifdef FRAMESIZE_QQVGA2
    case FRAMESIZE_QQVGA2: return "qqvga2";
#endif
    case FRAMESIZE_QCIF: return "qcif";
    case FRAMESIZE_HQVGA: return "hqvga";
    case FRAMESIZE_240X240: return "240x240";
    case FRAMESIZE_QVGA: return "qvga";
    case FRAMESIZE_CIF: return "cif";
    case FRAMESIZE_HVGA: return "hvga";
    case FRAMESIZE_VGA: return "vga";
    case FRAMESIZE_SVGA: return "svga";
    case FRAMESIZE_XGA: return "xga";
    case FRAMESIZE_HD: return "hd";
    case FRAMESIZE_SXGA: return "sxga";
    case FRAMESIZE_UXGA: return "uxga";
    case FRAMESIZE_FHD: return "fhd";
    case FRAMESIZE_P_HD: return "p_hd";
    case FRAMESIZE_P_3MP: return "p_3mp";
    case FRAMESIZE_QXGA: return "qxga";
    case FRAMESIZE_QHD: return "qhd";
    case FRAMESIZE_WQXGA: return "wqxga";
    case FRAMESIZE_P_FHD: return "p_fhd";
    case FRAMESIZE_QSXGA: return "qsxga";
    default: return "fs";
  }
}

static esp_err_t apply_cfg(camwebsrv_camera_t cam, const camwebsrv_seqcap_cfg_t *cfg)
{
  // pixformat and framesize first
  esp_err_t rv = camwebsrv_camera_ctrl_set(cam, "pixformat", (int)cfg->pixformat);
  if (rv != ESP_OK) return rv;
  rv = camwebsrv_camera_ctrl_set(cam, "framesize", (int)cfg->framesize);
  if (rv != ESP_OK) return rv;

  // Optional controls
  if (cfg->has_quality) camwebsrv_camera_ctrl_set(cam, "quality", cfg->quality);
  if (cfg->has_brightness) camwebsrv_camera_ctrl_set(cam, "brightness", cfg->brightness);
  if (cfg->has_contrast) camwebsrv_camera_ctrl_set(cam, "contrast", cfg->contrast);
  if (cfg->has_saturation) camwebsrv_camera_ctrl_set(cam, "saturation", cfg->saturation);
  if (cfg->has_sharpness) camwebsrv_camera_ctrl_set(cam, "sharpness", cfg->sharpness);
  if (cfg->has_special_effect) camwebsrv_camera_ctrl_set(cam, "special_effect", cfg->special_effect);
  if (cfg->has_wb_mode) camwebsrv_camera_ctrl_set(cam, "wb_mode", cfg->wb_mode);
  if (cfg->has_aec) camwebsrv_camera_ctrl_set(cam, "aec", cfg->aec);
  if (cfg->has_aec2) camwebsrv_camera_ctrl_set(cam, "aec2", cfg->aec2);
  if (cfg->has_aec_value) camwebsrv_camera_ctrl_set(cam, "aec_value", cfg->aec_value);
  if (cfg->has_ae_level) camwebsrv_camera_ctrl_set(cam, "ae_level", cfg->ae_level);
  if (cfg->has_agc) camwebsrv_camera_ctrl_set(cam, "agc", cfg->agc);
  if (cfg->has_agc_gain) camwebsrv_camera_ctrl_set(cam, "agc_gain", cfg->agc_gain);
  if (cfg->has_gainceiling) camwebsrv_camera_ctrl_set(cam, "gainceiling", cfg->gainceiling);
  if (cfg->has_awb) camwebsrv_camera_ctrl_set(cam, "awb", cfg->awb);
  if (cfg->has_awb_gain) camwebsrv_camera_ctrl_set(cam, "awb_gain", cfg->awb_gain);
  if (cfg->has_dcw) camwebsrv_camera_ctrl_set(cam, "dcw", cfg->dcw);
  if (cfg->has_bpc) camwebsrv_camera_ctrl_set(cam, "bpc", cfg->bpc);
  if (cfg->has_wpc) camwebsrv_camera_ctrl_set(cam, "wpc", cfg->wpc);
  if (cfg->has_hmirror) camwebsrv_camera_ctrl_set(cam, "hmirror", cfg->hmirror);
  if (cfg->has_vflip) camwebsrv_camera_ctrl_set(cam, "vflip", cfg->vflip);
  if (cfg->has_lenc) camwebsrv_camera_ctrl_set(cam, "lenc", cfg->lenc);
  if (cfg->has_raw_gma) camwebsrv_camera_ctrl_set(cam, "raw_gma", cfg->raw_gma);
  if (cfg->has_colorbar) camwebsrv_camera_ctrl_set(cam, "colorbar", cfg->colorbar);

  return ESP_OK;
}

static esp_err_t ensure_capture_dir(const char *cap_seq_name)
{
  char dir[192];
  snprintf(dir, sizeof(dir), "%s/captures/%s", CAMWEBSRV_SDCARD_MOUNT_PATH, cap_seq_name);
  return camwebsrv_sdcard_mkdirs(dir);
}

static esp_err_t write_frame_to_sd(const camwebsrv_seqcap_cfg_t *cfg, const uint8_t *buf, size_t len)
{
  int64_t ts_us = esp_timer_get_time();
  const char *fs = framesize_to_str(cfg->framesize);
  char path[256];
  snprintf(path, sizeof(path), "%s/captures/%s/%lld-%s.raw", CAMWEBSRV_SDCARD_MOUNT_PATH, cfg->cap_seq_name, (long long)ts_us, fs);
  return camwebsrv_sdcard_write_file(path, buf, len);
}

static esp_err_t slave_http_prepare(const camwebsrv_seqcap_cfg_t *cfg, const char *slave_host)
{
  // Call: http://<slave_host>/cap_seq_init?...query...
  char url[512];
  // Keep it short; only send required + key camera controls.
  // NOTE: Query string already URL-safe if cap_seq_name has no spaces; user should keep it simple.
  snprintf(url, sizeof(url),
           "http://%s/cap_seq_init?pixformat=%d&framesize=%d&cap_seq_name=%s&cap_amount=%d",
           slave_host,
           (int)cfg->pixformat,
           (int)cfg->framesize,
           cfg->cap_seq_name,
           cfg->cap_amount);

  esp_http_client_config_t c = {
    .url = url,
    .method = HTTP_METHOD_GET,
    .timeout_ms = 5000,
  };
  esp_http_client_handle_t client = esp_http_client_init(&c);
  if (client == NULL)
  {
    return ESP_FAIL;
  }
  esp_err_t rv = esp_http_client_perform(client);
  if (rv == ESP_OK)
  {
    int code = esp_http_client_get_status_code(client);
    if (code < 200 || code >= 300)
    {
      rv = ESP_FAIL;
      ESP_LOGW(CAMWEBSRV_TAG, "SEQCAP slave prepare HTTP status %d", code);
    }
  }
  esp_http_client_cleanup(client);
  return rv;
}

typedef struct
{
  camwebsrv_camera_t cam;
  camwebsrv_httpd_t httpd;
  camwebsrv_seqcap_cfg_t cfg;
  char slave_host[80];
  bool is_master;
} seqcap_task_arg_t;

static SemaphoreHandle_t s_slave_trig = NULL;

static void IRAM_ATTR slave_isr(void *arg)
{
  BaseType_t hp = pdFALSE;
  if (s_slave_trig)
  {
    xSemaphoreGiveFromISR(s_slave_trig, &hp);
  }
  if (hp) portYIELD_FROM_ISR();
}

static void seqcap_task_master(void *arg)
{
  seqcap_task_arg_t *a = (seqcap_task_arg_t *)arg;
  s_active = true;

  // Stop HTTP server and Wi-Fi to reduce jitter during capture.
  camwebsrv_httpd_stop(a->httpd);
  esp_wifi_stop();
  vTaskDelay(pdMS_TO_TICKS(50));

  bool sd4 = false;
  if (camwebsrv_sdcard_mount(&sd4) != ESP_OK)
  {
    ESP_LOGE(CAMWEBSRV_TAG, "SEQCAP master: SD mount failed");
    goto out;
  }

  if (ensure_capture_dir(a->cfg.cap_seq_name) != ESP_OK)
  {
    ESP_LOGE(CAMWEBSRV_TAG, "SEQCAP master: failed to create capture dir");
    goto out_sd;
  }

  // Tell slave to prepare
  if (a->slave_host[0] != 0x00)
  {
    if (slave_http_prepare(&a->cfg, a->slave_host) != ESP_OK)
    {
      ESP_LOGW(CAMWEBSRV_TAG, "SEQCAP master: slave prepare failed (continuing anyway)");
    }
  }

  if (a->cfg.slave_prepare_delay_ms > 0)
  {
    vTaskDelay(pdMS_TO_TICKS(a->cfg.slave_prepare_delay_ms));
  }

  // Stop web server and Wi-Fi to reduce jitter during capture
  if (a->httpd)
  {
    camwebsrv_httpd_stop(a->httpd);
  }
  esp_wifi_stop();
  vTaskDelay(pdMS_TO_TICKS(50));

  // Apply camera settings (while Wi-Fi still running, to keep things simple)
  if (apply_cfg(a->cam, &a->cfg) != ESP_OK)
  {
    ESP_LOGE(CAMWEBSRV_TAG, "SEQCAP master: failed to apply camera cfg");
    goto out_sd;
  }

  // Configure sync pin
  gpio_set_direction(CAMWEBSRV_PIN_SYNC, GPIO_MODE_OUTPUT);
  gpio_set_level(CAMWEBSRV_PIN_SYNC, 0);

  for (int i = 0; i < a->cfg.cap_amount; i++)
  {
    uint8_t *fbuf = NULL;
    size_t flen = 0;

    // Pulse high, start capture immediately.
    gpio_set_level(CAMWEBSRV_PIN_SYNC, 1);
    esp_err_t rv = camwebsrv_camera_frame_grab(a->cam, &fbuf, &flen, NULL);
    ets_delay_us(5000);
    gpio_set_level(CAMWEBSRV_PIN_SYNC, 0);

    if (rv != ESP_OK)
    {
      ESP_LOGE(CAMWEBSRV_TAG, "SEQCAP master: frame_grab failed: %s", esp_err_to_name(rv));
      break;
    }

    rv = write_frame_to_sd(&a->cfg, fbuf, flen);
    camwebsrv_camera_frame_dispose(a->cam);

    if (rv != ESP_OK)
    {
      ESP_LOGE(CAMWEBSRV_TAG, "SEQCAP master: write failed");
      break;
    }

    if (a->cfg.inter_frame_delay_ms > 0)
    {
      vTaskDelay(pdMS_TO_TICKS(a->cfg.inter_frame_delay_ms));
    }
  }

  // Optional blink (unmount SD first if using 4-bit, because GPIO4 is SD D1)
  camwebsrv_sdcard_unmount();
  blink_pattern();
  camwebsrv_sdcard_mount(NULL);

  // Restore Wi-Fi and web server
  esp_wifi_start();
  esp_wifi_connect();
  if (a->httpd)
  {
    camwebsrv_httpd_start(a->httpd);
  }

  // Restore Wi-Fi and web server
  esp_wifi_start();
  esp_wifi_connect();
  if (a->httpd)
  {
    camwebsrv_httpd_start(a->httpd);
  }

  // Bring Wi-Fi + web server back
  esp_wifi_start();
  esp_wifi_connect();
  if (a->httpd)
  {
    camwebsrv_httpd_start(a->httpd);
  }

  // Restart Wi-Fi + HTTP server
  esp_wifi_start();
  esp_wifi_connect();
  vTaskDelay(pdMS_TO_TICKS(200));
  camwebsrv_httpd_start(a->httpd);

  goto out;

out_sd:
  camwebsrv_sdcard_unmount();
out:
  s_active = false;
  free(a);
  vTaskDelete(NULL);
}

static void seqcap_task_slave(void *arg)
{
  seqcap_task_arg_t *a = (seqcap_task_arg_t *)arg;
  s_active = true;

  // Stop HTTP server and Wi-Fi to reduce jitter during capture.
  camwebsrv_httpd_stop(a->httpd);
  esp_wifi_stop();
  vTaskDelay(pdMS_TO_TICKS(50));

  bool sd4 = false;
  if (camwebsrv_sdcard_mount(&sd4) != ESP_OK)
  {
    ESP_LOGE(CAMWEBSRV_TAG, "SEQCAP slave: SD mount failed");
    goto out;
  }
  if (ensure_capture_dir(a->cfg.cap_seq_name) != ESP_OK)
  {
    ESP_LOGE(CAMWEBSRV_TAG, "SEQCAP slave: failed to create capture dir");
    goto out_sd;
  }
  if (apply_cfg(a->cam, &a->cfg) != ESP_OK)
  {
    ESP_LOGE(CAMWEBSRV_TAG, "SEQCAP slave: failed to apply camera cfg");
    goto out_sd;
  }

  // Prepare GPIO interrupt on sync pin
  gpio_config_t io = {
    .pin_bit_mask = 1ULL << CAMWEBSRV_PIN_SYNC,
    .mode = GPIO_MODE_INPUT,
    .pull_up_en = GPIO_PULLUP_DISABLE,
    .pull_down_en = GPIO_PULLDOWN_DISABLE,
    .intr_type = GPIO_INTR_POSEDGE,
  };
  gpio_config(&io);

  if (s_slave_trig == NULL)
  {
    s_slave_trig = xSemaphoreCreateBinary();
  }
  xSemaphoreTake(s_slave_trig, 0); // clear

  esp_err_t isr_rv = gpio_install_isr_service(0);
  if (isr_rv != ESP_OK && isr_rv != ESP_ERR_INVALID_STATE)
  {
    ESP_LOGW(CAMWEBSRV_TAG, "SEQCAP slave: gpio_install_isr_service failed: %s", esp_err_to_name(isr_rv));
  }
  gpio_isr_handler_add(CAMWEBSRV_PIN_SYNC, slave_isr, NULL);

  for (int i = 0; i < a->cfg.cap_amount; i++)
  {
    if (xSemaphoreTake(s_slave_trig, portMAX_DELAY) != pdTRUE)
    {
      continue;
    }
    uint8_t *fbuf = NULL;
    size_t flen = 0;
    esp_err_t rv = camwebsrv_camera_frame_grab(a->cam, &fbuf, &flen, NULL);
    if (rv != ESP_OK)
    {
      ESP_LOGE(CAMWEBSRV_TAG, "SEQCAP slave: frame_grab failed: %s", esp_err_to_name(rv));
      break;
    }
    rv = write_frame_to_sd(&a->cfg, fbuf, flen);
    camwebsrv_camera_frame_dispose(a->cam);
    if (rv != ESP_OK)
    {
      ESP_LOGE(CAMWEBSRV_TAG, "SEQCAP slave: write failed");
      break;
    }
  }

  gpio_isr_handler_remove(CAMWEBSRV_PIN_SYNC);

  camwebsrv_sdcard_unmount();
  blink_pattern();
  camwebsrv_sdcard_mount(NULL);

  // Bring Wi-Fi + web server back
  esp_wifi_start();
  esp_wifi_connect();
  if (a->httpd)
  {
    camwebsrv_httpd_start(a->httpd);
  }

  // Restart Wi-Fi + HTTP server
  esp_wifi_start();
  esp_wifi_connect();
  vTaskDelay(pdMS_TO_TICKS(200));
  camwebsrv_httpd_start(a->httpd);

  goto out;

out_sd:
  camwebsrv_sdcard_unmount();
out:
  s_active = false;
  free(a);
  vTaskDelete(NULL);
}

esp_err_t camwebsrv_seqcap_start_master(camwebsrv_camera_t cam, camwebsrv_httpd_t httpd, const camwebsrv_seqcap_cfg_t *cfg, const char *slave_host)
{
  if (cam == NULL || cfg == NULL)
  {
    return ESP_ERR_INVALID_ARG;
  }
  if (s_active)
  {
    return ESP_ERR_INVALID_STATE;
  }

  seqcap_task_arg_t *a = (seqcap_task_arg_t *)calloc(1, sizeof(*a));
  if (!a) return ESP_ERR_NO_MEM;
  a->cam = cam;
  a->httpd = httpd;
  a->cfg = *cfg;
  a->is_master = true;
  if (slave_host) strncpy(a->slave_host, slave_host, sizeof(a->slave_host) - 1);

  if (xTaskCreate(seqcap_task_master, "seqcap_master", 8192, a, 5, NULL) != pdPASS)
  {
    free(a);
    return ESP_FAIL;
  }
  return ESP_OK;
}

esp_err_t camwebsrv_seqcap_start_slave(camwebsrv_camera_t cam, camwebsrv_httpd_t httpd, const camwebsrv_seqcap_cfg_t *cfg)
{
  if (cam == NULL || cfg == NULL)
  {
    return ESP_ERR_INVALID_ARG;
  }
  if (s_active)
  {
    return ESP_ERR_INVALID_STATE;
  }

  seqcap_task_arg_t *a = (seqcap_task_arg_t *)calloc(1, sizeof(*a));
  if (!a) return ESP_ERR_NO_MEM;
  a->cam = cam;
  a->httpd = httpd;
  a->cfg = *cfg;
  a->is_master = false;

  if (xTaskCreate(seqcap_task_slave, "seqcap_slave", 8192, a, 5, NULL) != pdPASS)
  {
    free(a);
    return ESP_FAIL;
  }
  return ESP_OK;
}
