// 2026-01-16 seqcap.c
// SPDX-License-Identifier: GPL-3.0-or-later

#include "config.h"
#include "seqcap.h"
#include "httpd.h"
#include "sdcard_utils.h"

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

camwebsrv_seqcap_cfg_t seqcap_cfg = {0};
seqcap_task_arg_t seqcap_task_arg = {0};

#define SANITY_CHECK_ENABLED

void log_sanity_check(int mark)
{
  #ifdef SANITY_CHECK_ENABLED
  
#if ESP_IDF_VERSION_MAJOR >= 5
    esp_rom_printf("Sanity:%d\n", mark);
#else
    ets_printf("Sanity:%d\n", mark);
#endif
  #endif
}

static inline void log_sanity_check_nolog(int mark)
{
#if ESP_IDF_VERSION_MAJOR >= 5
    esp_rom_printf("Sanity:%d\n", mark);
#else
    ets_printf("Sanity:%d\n", mark);
#endif
}

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
  switch (fs)
  {
  case FRAMESIZE_QQVGA:
    return "qqvga";
#ifdef FRAMESIZE_QQVGA2
  case FRAMESIZE_QQVGA2:
    return "qqvga2";
#endif
  case FRAMESIZE_QCIF:
    return "qcif";
  case FRAMESIZE_HQVGA:
    return "hqvga";
  case FRAMESIZE_240X240:
    return "240x240";
  case FRAMESIZE_QVGA:
    return "qvga";
  case FRAMESIZE_CIF:
    return "cif";
  case FRAMESIZE_HVGA:
    return "hvga";
  case FRAMESIZE_VGA:
    return "vga";
  case FRAMESIZE_SVGA:
    return "svga";
  case FRAMESIZE_XGA:
    return "xga";
  case FRAMESIZE_SXGA:
    return "sxga";
  case FRAMESIZE_UXGA:
    return "uxga";
#ifdef FRAMESIZE_HD
  case FRAMESIZE_HD:
    return "hd";
#endif
#ifdef FRAMESIZE_FHD
  case FRAMESIZE_FHD:
    return "fhd";
#endif
#ifdef FRAMESIZE_P_HD
  case FRAMESIZE_P_HD:
    return "p_hd";
#endif
#ifdef FRAMESIZE_P_3MP
  case FRAMESIZE_P_3MP:
    return "p_3mp";
#endif
#ifdef FRAMESIZE_QXGA
  case FRAMESIZE_QXGA:
    return "qxga";
#endif
#ifdef FRAMESIZE_QHD
  case FRAMESIZE_QHD:
    return "qhd";
#endif
#ifdef FRAMESIZE_WQXGA
  case FRAMESIZE_WQXGA:
    return "wqxga";
#endif
#ifdef FRAMESIZE_P_FHD
  case FRAMESIZE_P_FHD:
    return "p_fhd";
#endif
#ifdef FRAMESIZE_QSXGA
  case FRAMESIZE_QSXGA:
    return "qsxga";
#endif
  default:
    return "fs";
  }
}

static esp_err_t apply_cfg(camwebsrv_camera_t cam, const camwebsrv_seqcap_cfg_t *cfg)
{
  // pixformat and framesize first
  esp_err_t rv = camwebsrv_camera_ctrl_set(cam, "pixformat", (int)cfg->pixformat);
  if (rv != ESP_OK)
    return rv;
  rv = camwebsrv_camera_ctrl_set(cam, "framesize", (int)cfg->framesize);
  if (rv != ESP_OK)
    return rv;

  // Optional controls
  if (cfg->has_quality)
    camwebsrv_camera_ctrl_set(cam, "quality", cfg->quality);
  if (cfg->has_brightness)
    camwebsrv_camera_ctrl_set(cam, "brightness", cfg->brightness);
  if (cfg->has_contrast)
    camwebsrv_camera_ctrl_set(cam, "contrast", cfg->contrast);
  if (cfg->has_saturation)
    camwebsrv_camera_ctrl_set(cam, "saturation", cfg->saturation);
  if (cfg->has_sharpness)
    camwebsrv_camera_ctrl_set(cam, "sharpness", cfg->sharpness);
  if (cfg->has_special_effect)
    camwebsrv_camera_ctrl_set(cam, "special_effect", cfg->special_effect);
  if (cfg->has_wb_mode)
    camwebsrv_camera_ctrl_set(cam, "wb_mode", cfg->wb_mode);
  if (cfg->has_aec)
    camwebsrv_camera_ctrl_set(cam, "aec", cfg->aec);
  if (cfg->has_aec2)
    camwebsrv_camera_ctrl_set(cam, "aec2", cfg->aec2);
  if (cfg->has_aec_value)
    camwebsrv_camera_ctrl_set(cam, "aec_value", cfg->aec_value);
  if (cfg->has_ae_level)
    camwebsrv_camera_ctrl_set(cam, "ae_level", cfg->ae_level);
  if (cfg->has_agc)
    camwebsrv_camera_ctrl_set(cam, "agc", cfg->agc);
  if (cfg->has_agc_gain)
    camwebsrv_camera_ctrl_set(cam, "agc_gain", cfg->agc_gain);
  if (cfg->has_gainceiling)
    camwebsrv_camera_ctrl_set(cam, "gainceiling", cfg->gainceiling);
  if (cfg->has_awb)
    camwebsrv_camera_ctrl_set(cam, "awb", cfg->awb);
  if (cfg->has_awb_gain)
    camwebsrv_camera_ctrl_set(cam, "awb_gain", cfg->awb_gain);
  if (cfg->has_dcw)
    camwebsrv_camera_ctrl_set(cam, "dcw", cfg->dcw);
  if (cfg->has_bpc)
    camwebsrv_camera_ctrl_set(cam, "bpc", cfg->bpc);
  if (cfg->has_wpc)
    camwebsrv_camera_ctrl_set(cam, "wpc", cfg->wpc);
  if (cfg->has_hmirror)
    camwebsrv_camera_ctrl_set(cam, "hmirror", cfg->hmirror);
  if (cfg->has_vflip)
    camwebsrv_camera_ctrl_set(cam, "vflip", cfg->vflip);
  if (cfg->has_lenc)
    camwebsrv_camera_ctrl_set(cam, "lenc", cfg->lenc);
  if (cfg->has_raw_gma)
    camwebsrv_camera_ctrl_set(cam, "raw_gma", cfg->raw_gma);
  if (cfg->has_colorbar)
    camwebsrv_camera_ctrl_set(cam, "colorbar", cfg->colorbar);

  return ESP_OK;
}

esp_err_t ensure_capture_dir(const char *cap_seq_name)
{
  char dir_path[512];
  snprintf(dir_path, sizeof(dir_path),
           "%s/captures/%s",
           CAMWEBSRV_SDCARD_MOUNT_PATH,
           cap_seq_name);
  ESP_LOGI(CAMWEBSRV_TAG, "SEQCAP: ensuring capture dir: %s", dir_path);
  esp_err_t ret = sdcard_mkdir_p(dir_path);
  return ret;
}

char write_frame_to_sd_path[512];

static esp_err_t write_frame_to_sd(const uint8_t *buf, size_t len)
{
  // convert us -> ms, keep only 32-bit
  uint32_t ts_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);

  const char *fs = framesize_to_str(seqcap_cfg.framesize);
  if (!fs)
    fs = "UNK";

  const char *seq = seqcap_cfg.cap_seq_name;

  int n = snprintf(write_frame_to_sd_path, sizeof(write_frame_to_sd_path),
                   "%s/captures/%s/%" PRIu32 "-%s.raw",
                   CAMWEBSRV_SDCARD_MOUNT_PATH,
                   seq,
                   ts_ms,
                   fs);

  if (n < 0 || n >= (int)sizeof(write_frame_to_sd_path))
  {
    return ESP_ERR_INVALID_SIZE; // path too long
  }

  // log outpath
  log_sanity_check(235);
  //ESP_LOGI(CAMWEBSRV_TAG, "SEQCAP: writing frame to SD: %s", write_frame_to_sd_path);
  ets_printf("SEQCAP: writing frame to SD: %s\n", write_frame_to_sd_path);

  return sdcard_write_file(write_frame_to_sd_path, buf, len, false);
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

static SemaphoreHandle_t s_slave_trig = NULL;

static void IRAM_ATTR slave_isr(void *arg)
{
  BaseType_t hp = pdFALSE;
  if (s_slave_trig)
  {
    xSemaphoreGiveFromISR(s_slave_trig, &hp);
  }
  if (hp)
    portYIELD_FROM_ISR();
}
#include "esp_camera.h"

// helper: grab+return (drop) frame safely
static inline void drop_one_frame(uint32_t delay_us)
{
    camera_fb_t *fb = esp_camera_fb_get();
    if (fb) {
        esp_camera_fb_return(fb);
    }
    if (delay_us) ets_delay_us(delay_us);
}

static void seqcap_task_master(void *arg)
{
  seqcap_task_arg_t *a = (seqcap_task_arg_t *)arg;

  log_sanity_check(295);

  ESP_LOGI(CAMWEBSRV_TAG, "SEQCAP master starting: pixformat=%d framesize=%d cap_seq_name=%s cap_amount=%d",
           (int)seqcap_cfg.pixformat,
           (int)seqcap_cfg.framesize,
           seqcap_cfg.cap_seq_name,
           seqcap_cfg.cap_amount);

  s_active = true;

  // 1) Tell slave to prepare while Wi-Fi + HTTPD are still running
  if (a->slave_host[0] != 0x00)
  {
    if (slave_http_prepare(a->cfg, a->slave_host) != ESP_OK)
    {
      ESP_LOGW(CAMWEBSRV_TAG, "SEQCAP master: slave prepare failed (continuing anyway)");
    }
  }

  log_sanity_check(315);

  if (seqcap_cfg.slave_prepare_delay_ms > 0)
  {
    vTaskDelay(pdMS_TO_TICKS(seqcap_cfg.slave_prepare_delay_ms));
  }

  // 2) Stop HTTP server and Wi-Fi ONCE to reduce jitter during capture
  if (a->httpd)
  {
    camwebsrv_httpd_stop(a->httpd);
    vTaskDelay(pdMS_TO_TICKS(50));
  }
  esp_wifi_stop();
  vTaskDelay(pdMS_TO_TICKS(50));

  log_sanity_check(331);

  // 3) Mount SD + ensure capture dir
  // ESP_ERROR_CHECK(sdcard_mount(&sd_cfg, &card));

  if (ensure_capture_dir(a->cfg->cap_seq_name) != ESP_OK)
  {
    ESP_LOGE(CAMWEBSRV_TAG, "SEQCAP master: failed to create capture dir");
    goto out_sd;
  }

  log_sanity_check(343);

  // 4) Apply camera settings
  if (apply_cfg(a->cam, a->cfg) != ESP_OK)
  {
    ESP_LOGE(CAMWEBSRV_TAG, "SEQCAP master: failed to apply camera cfg");
    goto out_sd;
  }

  log_sanity_check(352);

  // OPTIONAL but recommended: stop any streaming task before capture
  // (if you have one) to ensure only this task uses esp_camera_fb_get().

  // drop 5 frames to stabilize (RAW: fb_get + fb_return)
  for (int i = 0; i < 5; i++)
  {
    drop_one_frame(30000); // 30ms
  }

  log_sanity_check(367);
  vTaskDelay(pdMS_TO_TICKS(1000));

  // 5) Configure sync pin
  gpio_set_direction(CAMWEBSRV_PIN_SYNC, GPIO_MODE_OUTPUT);
  gpio_set_level(CAMWEBSRV_PIN_SYNC, 0);

  log_sanity_check(366);

  // 6) Capture loop (RAW fb ownership: get -> write -> return)
  for (int i = 0; i < a->cfg->cap_amount; i++)
  {
    log_sanity_check(380);

    gpio_set_level(CAMWEBSRV_PIN_SYNC, 1);

    camera_fb_t *fb = esp_camera_fb_get();   // <-- OWNERSHIP HERE

    ets_delay_us(5000);
    gpio_set_level(CAMWEBSRV_PIN_SYNC, 0);

    if (!fb)
    {
      ESP_LOGE(CAMWEBSRV_TAG, "SEQCAP master: esp_camera_fb_get failed");
      break;
    }

    // If you care about enforcing RAW/JPEG etc, you can sanity check:
    // if (fb->format != a->cfg->pixformat) { ... }

    vTaskDelay(pdMS_TO_TICKS(5));
    log_sanity_check_nolog(417);

    esp_err_t rv = write_frame_to_sd(fb->buf, fb->len);

    // IMPORTANT: return buffer no matter what
    esp_camera_fb_return(fb);
    fb = NULL;

    if (rv != ESP_OK)
    {
      ESP_LOGE(CAMWEBSRV_TAG, "SEQCAP master: write failed");
      break;
    }

    if (a->cfg->inter_frame_delay_ms > 0)
    {
      vTaskDelay(pdMS_TO_TICKS(a->cfg->inter_frame_delay_ms));
    }
  }

  // 7) Optional blink: unmount SD before blinking (GPIO4 conflict)
  ESP_ERROR_CHECK(sdcard_unmount(sd_cfg.mount_point, card));
  blink_pattern();
  ESP_ERROR_CHECK(sdcard_mount(&sd_cfg, &card));

  // 8) Restore Wi-Fi + HTTPD ONCE
  esp_wifi_start();
  esp_wifi_connect();
  if (a->httpd)
  {
    camwebsrv_httpd_start(a->httpd);
  }

  free(a);
  goto out;

out_sd:
  ESP_ERROR_CHECK(sdcard_unmount(sd_cfg.mount_point, card));
out:
  s_active = false;
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

  ESP_ERROR_CHECK(sdcard_mount(&sd_cfg, &card));

  if (ensure_capture_dir(a->cfg->cap_seq_name) != ESP_OK)
  {
    ESP_LOGE(CAMWEBSRV_TAG, "SEQCAP slave: failed to create capture dir");
    goto out_sd;
  }
  if (apply_cfg(a->cam, a->cfg) != ESP_OK)
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

  for (int i = 0; i < a->cfg->cap_amount; i++)
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
    rv = write_frame_to_sd(fbuf, flen);
    camwebsrv_camera_frame_dispose(a->cam);
    if (rv != ESP_OK)
    {
      ESP_LOGE(CAMWEBSRV_TAG, "SEQCAP slave: write failed");
      break;
    }
  }

  gpio_isr_handler_remove(CAMWEBSRV_PIN_SYNC);

  ESP_ERROR_CHECK(sdcard_unmount(sd_cfg.mount_point, card));
  blink_pattern();
  ESP_ERROR_CHECK(sdcard_mount(&sd_cfg, &card));

  // Bring Wi-Fi + web server back
  esp_wifi_start();
  esp_wifi_connect();
  if (a->httpd)
  {
    camwebsrv_httpd_start(a->httpd);
  }

  goto out;

out_sd:
  ESP_ERROR_CHECK(sdcard_unmount(sd_cfg.mount_point, card));
out:
  s_active = false;
  free(a);
  vTaskDelete(NULL);
}

esp_err_t camwebsrv_seqcap_start_master(camwebsrv_camera_t cam, camwebsrv_httpd_t httpd, camwebsrv_seqcap_cfg_t *cfg, const char *slave_host)
{
  if (cam == NULL || cfg == NULL)
  {
    return ESP_ERR_INVALID_ARG;
  }
  if (s_active)
  {
    return ESP_ERR_INVALID_STATE;
  }
  memset(&seqcap_task_arg, 0x00, sizeof(seqcap_task_arg));
  seqcap_task_arg_t *a = &seqcap_task_arg;
  if (!a)
    return ESP_ERR_NO_MEM;
  a->cam = cam;
  a->httpd = httpd;
  a->cfg = cfg;
  a->is_master = true;
  if (slave_host)
    strncpy(a->slave_host, slave_host, sizeof(a->slave_host) - 1);

  log_sanity_check(472);

  if (xTaskCreate(seqcap_task_master, "seqcap_master", 1024 * 40, a, 5, NULL) != pdPASS)
    return ESP_FAIL;

  log_sanity_check(485);
  return ESP_OK;
}

esp_err_t camwebsrv_seqcap_start_slave(camwebsrv_camera_t cam, camwebsrv_httpd_t httpd, camwebsrv_seqcap_cfg_t *cfg)
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
  if (!a)
    return ESP_ERR_NO_MEM;
  a->cam = cam;
  a->httpd = httpd;
  a->cfg = cfg;
  a->is_master = false;

  if (xTaskCreate(seqcap_task_slave, "seqcap_slave", 8192, a, 5, NULL) != pdPASS)
  {
    free(a);
    return ESP_FAIL;
  }
  return ESP_OK;
}
