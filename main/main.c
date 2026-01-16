// 2023-01-03 main.c
// Copyright (C) 2023 Vino Fernando Crescini  <vfcrescini@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "config.h"
#include "cfgman.h"
#include "httpd.h"
#include "ping.h"
#include "storage.h"
#include "wifi.h"
#include "seqcap.h"

#include <esp_log.h>
#include <esp_err.h>
#include <esp_system.h>
#include <esp_event.h>

#include <mdns.h>

#include <ctype.h>
#include <stdbool.h>
#include <string.h>

#include <nvs_flash.h>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

static void camwebsrv_mdns_start(camwebsrv_cfgman_t cfgman)
{
  esp_err_t rv;
  const char *pair_id = "0";
  const char *role = "master";
  char hostname[64];

  // Optional config keys (fall back to defaults if missing)
  rv = camwebsrv_cfgman_get(cfgman, CAMWEBSRV_CFGMAN_KEY_PAIR_ID, &pair_id);
  if (rv != ESP_OK)
  {
    pair_id = "0";
  }

  rv = camwebsrv_cfgman_get(cfgman, CAMWEBSRV_CFGMAN_KEY_ROLE, &role);
  if (rv != ESP_OK)
  {
    role = "master";
  }

  // Build hostname: cam-master-<pair_id> or cam-slave-<pair_id>
  // mDNS hostnames should be letters/digits/hyphen; sanitize pair_id just in case.
  char pair_s[24];
  size_t j = 0;
  for (size_t i = 0; pair_id[i] != 0x00 && j < sizeof(pair_s) - 1; i++)
  {
    unsigned char c = (unsigned char)pair_id[i];
    if (isalnum(c))
    {
      pair_s[j++] = (char)tolower(c);
    }
    else if (c == '-' || c == '_')
    {
      pair_s[j++] = '-';
    }
    else
    {
      pair_s[j++] = '-';
    }
  }
  pair_s[j] = 0x00;
  if (pair_s[0] == 0x00)
  {
    strcpy(pair_s, "0");
  }

  bool is_master = (strcasecmp(role, "master") == 0);
  snprintf(hostname, sizeof(hostname), is_master ? "cam-master-%s" : "cam-slave-%s", pair_s);

  ESP_ERROR_CHECK(mdns_init());
  ESP_ERROR_CHECK(mdns_hostname_set(hostname));
  ESP_ERROR_CHECK(mdns_instance_name_set(hostname));

  // Advertise the HTTP server.
  // Note: ESP-IDF appends .local automatically when resolving the hostname.
  ESP_ERROR_CHECK(mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0));

  ESP_LOGI(CAMWEBSRV_TAG, "MDNS started: http://%s.local/", hostname);
}

void app_main()
{
  esp_err_t rv;
  SemaphoreHandle_t sema;
  camwebsrv_cfgman_t cfgman = NULL;
  camwebsrv_httpd_t httpd = NULL;
  camwebsrv_ping_t ping = NULL;
  camwebsrv_wifi_t wifi = NULL;

  // initialise NVS

  rv = nvs_flash_init();

  if (rv == ESP_ERR_NVS_NO_FREE_PAGES || rv == ESP_ERR_NVS_NEW_VERSION_FOUND)
  {
    rv = nvs_flash_erase();

    if (rv != ESP_OK)
    {
      ESP_LOGE(CAMWEBSRV_TAG, "MAIN app_main(): nvs_flash_erase() failed: [%d]: %s", rv, esp_err_to_name(rv));
      goto camwebsrv_main_error;
    }

    rv = nvs_flash_init();
  }

  if (rv != ESP_OK)
  {
    ESP_LOGE(CAMWEBSRV_TAG, "MAIN app_main(): nvs_flash_init() failed: [%d]: %s", rv, esp_err_to_name(rv));
    goto camwebsrv_main_error;
  }

  // create default event loop

  rv = esp_event_loop_create_default();
  
  if (rv != ESP_OK)
  {
    ESP_LOGE(CAMWEBSRV_TAG, "WIFI app_main(): esp_event_loop_create_default() failed: [%d]: %s", rv, esp_err_to_name(rv));
    goto camwebsrv_main_error;
  }

  // initialise storage

  rv = camwebsrv_storage_init();

  if (rv != ESP_OK)
  {
    ESP_LOGE(CAMWEBSRV_TAG, "MAIN app_main(): camwebsrv_storage_init() failed: [%d]: %s", rv, esp_err_to_name(rv));
    goto camwebsrv_main_error;
  }

  // initialise config manager

  rv = camwebsrv_cfgman_init(&cfgman);

  if (rv != ESP_OK)
  {
    ESP_LOGE(CAMWEBSRV_TAG, "MAIN app_main(): camwebsrv_cfgman_init() failed: [%d]: %s", rv, esp_err_to_name(rv));
    goto camwebsrv_main_error;
  }

  // load config

  rv = camwebsrv_cfgman_load(cfgman, CAMWEBSRV_CFGMAN_FILENAME);

  if (rv != ESP_OK)
  {
    ESP_LOGE(CAMWEBSRV_TAG, "MAIN app_main(): camwebsrv_cfgman_load(%s) failed: [%d]: %s", CAMWEBSRV_CFGMAN_FILENAME, rv, esp_err_to_name(rv));
    goto camwebsrv_main_error;
  }

  // initialise wifi

  rv = camwebsrv_wifi_init(&wifi, cfgman);

  if (rv != ESP_OK)
  {
    ESP_LOGE(CAMWEBSRV_TAG, "MAIN app_main(): camwebsrv_wifi_init() failed: [%d]: %s", rv, esp_err_to_name(rv));
    goto camwebsrv_main_error;
  }

  // Start mDNS after Wi-Fi is connected (camwebsrv_wifi_init blocks until got IP).
  camwebsrv_mdns_start(cfgman);

  // initialise ping

  rv = camwebsrv_ping_init(&ping, cfgman);
  
  if (rv != ESP_OK)
  {
    ESP_LOGE(CAMWEBSRV_TAG, "MAIN app_main(): camwebsrv_ping_init() failed: [%d]: %s", rv, esp_err_to_name(rv));
    goto camwebsrv_main_error;
  }

  // initialise sema

  sema = xSemaphoreCreateBinary();

  if (sema == NULL)
  {
    ESP_LOGE(CAMWEBSRV_TAG, "MAIN app_main(): xSemaphoreCreateBinary() failed: [%d]: %s", rv, esp_err_to_name(rv));
    goto camwebsrv_main_error;
  }

  // initialise web server

  rv = camwebsrv_httpd_init(&httpd, sema, cfgman);

  if (rv != ESP_OK)
  {
    ESP_LOGE(CAMWEBSRV_TAG, "MAIN app_main(): camwebsrv_httpd_init() failed: [%d]: %s", rv, esp_err_to_name(rv));
    goto camwebsrv_main_error;
  }

  // start web server

  rv = camwebsrv_httpd_start(httpd);

  if (rv != ESP_OK)
  {
    ESP_LOGE(CAMWEBSRV_TAG, "MAIN app_main(): camwebsrv_httpd_start() failed: [%d]: %s", rv, esp_err_to_name(rv));
    goto camwebsrv_main_error;
  }

  // process stream requests indefinitely

  while(1)
  {
    uint16_t nextevent = UINT16_MAX;

    // During synchronized sequence capture we pause normal HTTP/ping processing.
    if (camwebsrv_seqcap_is_active())
    {
      vTaskDelay(pdMS_TO_TICKS(50));
      continue;
    }

    // ping

    rv = camwebsrv_ping_process(ping, &nextevent);

    if (rv != ESP_OK)
    {
      ESP_LOGW(CAMWEBSRV_TAG, "MAIN app_main(): camwebsrv_ping_process() failed: [%d]: %s", rv, esp_err_to_name(rv));
      goto camwebsrv_main_error;
    }

    // httpd

    rv = camwebsrv_httpd_process(httpd, &nextevent);

    if (rv != ESP_OK)
    {
      ESP_LOGE(CAMWEBSRV_TAG, "MAIN app_main(): camwebsrv_httpd_process() failed: [%d]: %s", rv, esp_err_to_name(rv));
      goto camwebsrv_main_error;
    }

    // block until there is actually something to do

    xSemaphoreTake(sema, (nextevent == UINT16_MAX) ? portMAX_DELAY : pdMS_TO_TICKS(nextevent));
  }

  camwebsrv_main_error:

  ESP_LOGE(CAMWEBSRV_TAG, "MAIN app_main(): Rebooting in %d seconds", CAMWEBSRV_MAIN_REBOOT_DELAY_MSEC / 1000);
  vTaskDelay(pdMS_TO_TICKS(CAMWEBSRV_MAIN_REBOOT_DELAY_MSEC));
  esp_restart();
  return;
}
