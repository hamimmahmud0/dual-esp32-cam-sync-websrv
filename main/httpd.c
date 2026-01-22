// 2023-01-03 httpd.c
// Copyright (C) 2023 Vino Fernando Crescini  <vfcrescini@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "config.h"
#include "httpd.h"
#include "camera.h"
#include "sclients.h"
#include "storage.h"
#include "vbytes.h"
#include "seqcap.h"

#include <stddef.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>

#include <esp_log.h>
#include <esp_http_server.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define _CAMWEBSRV_HTTPD_SERVER_PORT 80
#define _CAMWEBSRV_HTTPD_CONTROL_PORT 32768

#define _CAMWEBSRV_HTTPD_PATH_ROOT    "/"
#define _CAMWEBSRV_HTTPD_PATH_STYLE   "/style.css"
#define _CAMWEBSRV_HTTPD_PATH_SCRIPT  "/script.js"
#define _CAMWEBSRV_HTTPD_PATH_STATUS  "/status"
#define _CAMWEBSRV_HTTPD_PATH_RESET   "/reset"
#define _CAMWEBSRV_HTTPD_PATH_CONTROL "/control"
#define _CAMWEBSRV_HTTPD_PATH_CAPTURE "/capture"
#define _CAMWEBSRV_HTTPD_PATH_STREAM  "/stream"
#define _CAMWEBSRV_HTTPD_PATH_SEQ_CAP "/seq_cap"
#define _CAMWEBSRV_HTTPD_PATH_CAP_SEQ_INIT "/cap_seq_init"

#define _CAMWEBSRV_HTTPD_RESP_STATUS_STR "\
{\n\
  \"aec\": %u,\n\
  \"aec2\": %u,\n\
  \"aec_value\": %u,\n\
  \"ae_level\": %d,\n\
  \"agc\": %u,\n\
  \"agc_gain\": %u,\n\
  \"awb\": %u,\n\
  \"awb_gain\": %u,\n\
  \"bpc\": %u,\n\
  \"brightness\": %d,\n\
  \"colorbar\": %u,\n\
  \"contrast\": %d,\n\
  \"dcw\": %u,\n\
  \"flash\": %d,\n\
  \"fps\": %d,\n\
  \"framesize\": %u,\n\
  \"gainceiling\": %u,\n\
  \"hmirror\": %u,\n\
  \"lenc\": %u,\n\
  \"quality\": %u,\n\
  \"raw_gma\": %u,\n\
  \"saturation\": %d,\n\
  \"sharpness\": %d,\n\
  \"special_effect\": %u,\n\
  \"vflip\": %u,\n\
  \"wb_mode\": %u,\n\
  \"wpc\": %u\n\
}\n \
"

#define _CAMWEBSRV_HTTPD_PARAM_LEN 32

typedef struct
{
  httpd_handle_t handle;
  SemaphoreHandle_t sema;
  camwebsrv_camera_t cam;
  camwebsrv_sclients_t sclients;
  camwebsrv_cfgman_t cfgman;
} _camwebsrv_httpd_t;

typedef struct
{
  int sockfd;
  _camwebsrv_httpd_t *phttpd;
} _camwebsrv_httpd_worker_arg_t;

static esp_err_t _camwebsrv_httpd_handler_static(httpd_req_t *req);
static esp_err_t _camwebsrv_httpd_handler_status(httpd_req_t *req);
static esp_err_t _camwebsrv_httpd_handler_reset(httpd_req_t *req);
static esp_err_t _camwebsrv_httpd_handler_control(httpd_req_t *req);
static esp_err_t _camwebsrv_httpd_handler_capture(httpd_req_t *req);
static esp_err_t _camwebsrv_httpd_handler_stream(httpd_req_t *req);
static esp_err_t _camwebsrv_httpd_handler_seq_cap(httpd_req_t *req);
static esp_err_t _camwebsrv_httpd_handler_cap_seq_init(httpd_req_t *req);
static bool _camwebsrv_httpd_static_cb(const char *buf, size_t len, void *arg);
static void _camwebsrv_httpd_worker(void *arg);
static void _camwebsrv_httpd_noop(void *arg);

esp_err_t camwebsrv_httpd_init(camwebsrv_httpd_t *httpd, SemaphoreHandle_t sema, camwebsrv_cfgman_t cfgman)
{
  _camwebsrv_httpd_t *phttpd;
  esp_err_t rv;

  if (httpd == NULL || sema == NULL || cfgman == NULL)
  {
    return ESP_ERR_INVALID_ARG;
  }

  phttpd = (_camwebsrv_httpd_t *) malloc(sizeof(_camwebsrv_httpd_t));

  if (phttpd == NULL)
  {
    int e = errno;
    ESP_LOGE(CAMWEBSRV_TAG, "HTTPD camwebsrv_httpd_init(): malloc() failed: [%d]: %s", e, strerror(e));
    return ESP_FAIL;
  }

  memset(phttpd, 0x00, sizeof(_camwebsrv_httpd_t));

  phttpd->sema = sema;
  phttpd->cfgman = cfgman;

  rv = camwebsrv_camera_init(&(phttpd->cam));

  if (rv != ESP_OK)
  {
    ESP_LOGE(CAMWEBSRV_TAG, "HTTPD camwebsrv_httpd_init(): camwebsrv_camera_init() failed: [%d]: %s", rv, esp_err_to_name(rv));
    free(phttpd);
    return ESP_FAIL;
  }

  rv = camwebsrv_sclients_init(&(phttpd->sclients));

  if (rv != ESP_OK)
  {
    ESP_LOGE(CAMWEBSRV_TAG, "HTTPD camwebsrv_httpd_init(): camwebsrv_sclients_init() failed: [%d]: %s", rv, esp_err_to_name(rv));
    camwebsrv_camera_destroy(&(phttpd->cam));
    free(phttpd);
    return ESP_FAIL;
  }

  *httpd = (camwebsrv_httpd_t) phttpd;

  return ESP_OK;
}

esp_err_t camwebsrv_httpd_stop(camwebsrv_httpd_t httpd)
{
  if (httpd == NULL)
  {
    return ESP_ERR_INVALID_ARG;
  }

  _camwebsrv_httpd_t *phttpd = (_camwebsrv_httpd_t *)httpd;
  if (phttpd->handle == NULL)
  {
    return ESP_OK;
  }

  esp_err_t rv = httpd_stop(phttpd->handle);
  if (rv != ESP_OK)
  {
    ESP_LOGW(CAMWEBSRV_TAG, "HTTPD camwebsrv_httpd_stop(): httpd_stop() failed: [%d]: %s", rv, esp_err_to_name(rv));
  }
  phttpd->handle = NULL;
  return rv;
}

esp_err_t camwebsrv_httpd_destroy(camwebsrv_httpd_t *httpd)
{
  _camwebsrv_httpd_t *phttpd;
  esp_err_t rv;

  if (httpd == NULL)
  {
    return ESP_ERR_INVALID_ARG;
  }

  phttpd = (_camwebsrv_httpd_t *) *httpd;

  rv = camwebsrv_sclients_destroy(&(phttpd->sclients), phttpd->handle);

  if (rv != ESP_OK)
  {
    ESP_LOGE(CAMWEBSRV_TAG, "HTTPD camwebsrv_httpd_destroy(): camwebsrv_sclients_destroy() failed: [%d]: %s", rv, esp_err_to_name(rv));
  }

  rv = camwebsrv_camera_destroy(&(phttpd->cam));

  if (rv != ESP_OK)
  {
    ESP_LOGE(CAMWEBSRV_TAG, "HTTPD camwebsrv_httpd_destroy(): camwebsrv_camera_destroy() failed: [%d]: %s", rv, esp_err_to_name(rv));
  }

  if (phttpd->handle != NULL)
  {
    rv = httpd_stop(phttpd->handle);

    if (rv != ESP_OK)
    {
      ESP_LOGE(CAMWEBSRV_TAG, "HTTPD camwebsrv_httpd_destroy(): httpd_stop() failed: [%d]: %s", rv, esp_err_to_name(rv));
    }
  }

  free(phttpd);

  *httpd = NULL;

  return ESP_OK;
}

esp_err_t camwebsrv_httpd_start(camwebsrv_httpd_t httpd)
{
  _camwebsrv_httpd_t *phttpd;
  esp_err_t rv;
  httpd_uri_t uri;
  httpd_config_t c = HTTPD_DEFAULT_CONFIG();
  c.max_uri_handlers = 32;   // was default (often 8)

  if (httpd == NULL)
  {
    return ESP_ERR_INVALID_ARG;
  }

  phttpd = (_camwebsrv_httpd_t *) httpd;

  // configuration overrides

  c.server_port = _CAMWEBSRV_HTTPD_SERVER_PORT;
  c.ctrl_port = _CAMWEBSRV_HTTPD_CONTROL_PORT;
  c.global_user_ctx = (void *) phttpd;
  c.global_user_ctx_free_fn = _camwebsrv_httpd_noop;

  rv = httpd_start(&(phttpd->handle), &c);

  if (rv != ESP_OK)
  {
    ESP_LOGE(CAMWEBSRV_TAG, "HTTPD camwebsrv_httpd_start(): httpd_start() failed: [%d]: %s", rv, esp_err_to_name(rv));
    return rv;
  }

  // register root

  memset(&uri, 0x00, sizeof(uri));

  uri.uri     = _CAMWEBSRV_HTTPD_PATH_ROOT;
  uri.method  = HTTP_GET;
  uri.handler = _camwebsrv_httpd_handler_static;

  httpd_register_uri_handler(phttpd->handle, &uri);

  // register style

  memset(&uri, 0x00, sizeof(uri));

  uri.uri     = _CAMWEBSRV_HTTPD_PATH_STYLE;
  uri.method  = HTTP_GET;
  uri.handler = _camwebsrv_httpd_handler_static;

  httpd_register_uri_handler(phttpd->handle, &uri);

  // register script

  memset(&uri, 0x00, sizeof(uri));

  uri.uri     = _CAMWEBSRV_HTTPD_PATH_SCRIPT;
  uri.method  = HTTP_GET;
  uri.handler = _camwebsrv_httpd_handler_static;

  httpd_register_uri_handler(phttpd->handle, &uri);

  // register status

  memset(&uri, 0x00, sizeof(uri));

  uri.uri     = _CAMWEBSRV_HTTPD_PATH_STATUS;
  uri.method  = HTTP_GET;
  uri.handler = _camwebsrv_httpd_handler_status;

  httpd_register_uri_handler(phttpd->handle, &uri);

  // register reset

  memset(&uri, 0x00, sizeof(uri));

  uri.uri     = _CAMWEBSRV_HTTPD_PATH_RESET;
  uri.method  = HTTP_GET;
  uri.handler = _camwebsrv_httpd_handler_reset;

  httpd_register_uri_handler(phttpd->handle, &uri);

  // register control

  memset(&uri, 0x00, sizeof(uri));

  uri.uri     = _CAMWEBSRV_HTTPD_PATH_CONTROL;
  uri.method  = HTTP_GET;
  uri.handler = _camwebsrv_httpd_handler_control;

  httpd_register_uri_handler(phttpd->handle, &uri);

  // register capture

  memset(&uri, 0x00, sizeof(uri));

  uri.uri     = _CAMWEBSRV_HTTPD_PATH_CAPTURE;
  uri.method  = HTTP_GET;
  uri.handler = _camwebsrv_httpd_handler_capture;

  httpd_register_uri_handler(phttpd->handle, &uri);

  // register stream

  memset(&uri, 0x00, sizeof(uri));

  uri.uri     = _CAMWEBSRV_HTTPD_PATH_STREAM;
  uri.method  = HTTP_GET;
  uri.handler = _camwebsrv_httpd_handler_stream;

  httpd_register_uri_handler(phttpd->handle, &uri);

  // register sequence capture (master)

  memset(&uri, 0x00, sizeof(uri));

  uri.uri     = _CAMWEBSRV_HTTPD_PATH_SEQ_CAP;
  uri.method  = HTTP_GET;
  uri.handler = _camwebsrv_httpd_handler_seq_cap;

  httpd_register_uri_handler(phttpd->handle, &uri);

  // register sequence capture init (slave)

  memset(&uri, 0x00, sizeof(uri));

  uri.uri     = _CAMWEBSRV_HTTPD_PATH_CAP_SEQ_INIT;
  uri.method  = HTTP_GET;
  uri.handler = _camwebsrv_httpd_handler_cap_seq_init;

  httpd_register_uri_handler(phttpd->handle, &uri);

  ESP_LOGI(CAMWEBSRV_TAG, "HTTPD camwebsrv_httpd_start(): started server on port %d", _CAMWEBSRV_HTTPD_SERVER_PORT);

  return ESP_OK;
}

esp_err_t camwebsrv_httpd_process(camwebsrv_httpd_t httpd, uint16_t *nextevent)
{
  _camwebsrv_httpd_t *phttpd;
  esp_err_t rv;

  if (httpd == NULL)
  {
    return ESP_ERR_INVALID_ARG;
  }

  phttpd = (_camwebsrv_httpd_t *) httpd;

  // If the HTTP server is stopped (sequence capture mode), nothing to do.
  if (phttpd->handle == NULL)
  {
    return ESP_OK;
  }

  rv = camwebsrv_sclients_process(phttpd->sclients, phttpd->cam, phttpd->handle, nextevent);

  if (rv != ESP_OK)
  {
    ESP_LOGE(CAMWEBSRV_TAG, "HTTPD camwebsrv_httpd_process(): camwebsrv_sclients_process() failed: [%d]: %s", rv, esp_err_to_name(rv));
    return rv;
  }

  return ESP_OK;
}

static esp_err_t _camwebsrv_httpd_handler_static(httpd_req_t *req)
{
  esp_err_t rv;

  httpd_resp_set_status(req, "200 OK");

  // content and type depends on what the request was

  if (strcmp(req->uri, _CAMWEBSRV_HTTPD_PATH_STYLE) == 0)
  {
    httpd_resp_set_type(req, "text/css");
    rv = camwebsrv_storage_get("style.css", _camwebsrv_httpd_static_cb, (void *) req);
  }
  else if (strcmp(req->uri, _CAMWEBSRV_HTTPD_PATH_SCRIPT) == 0)
  {
    httpd_resp_set_type(req, "application/javascript");
    rv = camwebsrv_storage_get("script.js", _camwebsrv_httpd_static_cb, (void *) req);
  }
  else
  {
    _camwebsrv_httpd_t *phttpd;

    phttpd = (_camwebsrv_httpd_t *) httpd_get_global_user_ctx(req->handle);

    httpd_resp_set_type(req, "text/html");
    rv = camwebsrv_storage_get(camwebsrv_camera_is_ov3660(phttpd->cam) ? "ov3660.htm" : "ov2640.htm", _camwebsrv_httpd_static_cb, (void *) req);
  }

  if (rv != ESP_OK)
  {
    ESP_LOGE(CAMWEBSRV_TAG, "HTTPD _camwebsrv_httpd_handler_static(): camwebsrv_storage_get() failed: [%d]: %s", rv, esp_err_to_name(rv));
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, NULL);
    return rv;
  }

  ESP_LOGI(CAMWEBSRV_TAG, "HTTPD _camwebsrv_httpd_handler_static(%d): served %s", httpd_req_to_sockfd(req), req->uri);

  return ESP_OK;
}

static esp_err_t _camwebsrv_httpd_handler_status(httpd_req_t *req)
{
  esp_err_t rv = ESP_OK;
  _camwebsrv_httpd_t *phttpd;
  camwebsrv_vbytes_t vb;
  const uint8_t *buf;

  phttpd = (_camwebsrv_httpd_t *) httpd_get_global_user_ctx(req->handle);

  // response type/header status

  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_status(req, "200 OK");

  // initialise and compose response buffer

  rv = camwebsrv_vbytes_init(&vb);

  if (rv != ESP_OK)
  {
    ESP_LOGE(CAMWEBSRV_TAG, "HTTPD _camwebsrv_httpd_handler_status(): camwebsrv_vbytes_init() failed: [%d]: %s", rv, esp_err_to_name(rv));
    return rv;
  }

  rv = camwebsrv_vbytes_set_str(
    vb,
    _CAMWEBSRV_HTTPD_RESP_STATUS_STR,
    camwebsrv_camera_ctrl_get(phttpd->cam, "aec"),
    camwebsrv_camera_ctrl_get(phttpd->cam, "aec2"),
    camwebsrv_camera_ctrl_get(phttpd->cam, "aec_value"),
    camwebsrv_camera_ctrl_get(phttpd->cam, "ae_level"),
    camwebsrv_camera_ctrl_get(phttpd->cam, "agc"),
    camwebsrv_camera_ctrl_get(phttpd->cam, "agc_gain"),
    camwebsrv_camera_ctrl_get(phttpd->cam, "awb"),
    camwebsrv_camera_ctrl_get(phttpd->cam, "awb_gain"),
    camwebsrv_camera_ctrl_get(phttpd->cam, "bpc"),
    camwebsrv_camera_ctrl_get(phttpd->cam, "brightness"),
    camwebsrv_camera_ctrl_get(phttpd->cam, "colorbar"),
    camwebsrv_camera_ctrl_get(phttpd->cam, "contrast"),
    camwebsrv_camera_ctrl_get(phttpd->cam, "dcw"),
    camwebsrv_camera_ctrl_get(phttpd->cam, "flash"),
    camwebsrv_camera_ctrl_get(phttpd->cam, "fps"),
    camwebsrv_camera_ctrl_get(phttpd->cam, "framesize"),
    camwebsrv_camera_ctrl_get(phttpd->cam, "gainceiling"),
    camwebsrv_camera_ctrl_get(phttpd->cam, "hmirror"),
    camwebsrv_camera_ctrl_get(phttpd->cam, "lenc"),
    camwebsrv_camera_ctrl_get(phttpd->cam, "quality"),
    camwebsrv_camera_ctrl_get(phttpd->cam, "raw_gma"),
    camwebsrv_camera_ctrl_get(phttpd->cam, "saturation"),
    camwebsrv_camera_ctrl_get(phttpd->cam, "sharpness"),
    camwebsrv_camera_ctrl_get(phttpd->cam, "special_effect"),
    camwebsrv_camera_ctrl_get(phttpd->cam, "vflip"),
    camwebsrv_camera_ctrl_get(phttpd->cam, "wb_mode"),
    camwebsrv_camera_ctrl_get(phttpd->cam, "wpc")
  );

  if (rv != ESP_OK)
  {
    ESP_LOGE(CAMWEBSRV_TAG, "HTTPD _camwebsrv_httpd_handler_status(): camwebsrv_vbytes_set_str() failed: [%d]: %s", rv, esp_err_to_name(rv));
    camwebsrv_vbytes_destroy(&vb);
    return rv;
  }

  rv  = camwebsrv_vbytes_get_bytes(vb, &buf, NULL);

  if (rv != ESP_OK)
  {
    ESP_LOGE(CAMWEBSRV_TAG, "HTTPD _camwebsrv_httpd_handler_status(): camwebsrv_vbytes_get_bytes() failed: [%d]: %s", rv, esp_err_to_name(rv));
    camwebsrv_vbytes_destroy(&vb);
    return rv;
  }

  // send response

  rv = httpd_resp_sendstr(req, (char *) buf);

  if (rv != ESP_OK)
  {
    ESP_LOGE(CAMWEBSRV_TAG, "HTTPD _camwebsrv_httpd_handler_status(): httpd_resp_sendstr() failed: [%d]: %s", rv, esp_err_to_name(rv));
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, NULL);
    return rv;
  }

  camwebsrv_vbytes_destroy(&vb);

  ESP_LOGI(CAMWEBSRV_TAG, "HTTPD _camwebsrv_httpd_handler_status(%d): served %s", httpd_req_to_sockfd(req), req->uri);

  return ESP_OK;
}

static esp_err_t _camwebsrv_httpd_handler_reset(httpd_req_t *req)
{
  esp_err_t rv = ESP_OK;
  _camwebsrv_httpd_t *phttpd;

  phttpd = (_camwebsrv_httpd_t *) httpd_get_global_user_ctx(req->handle);

  // response type/header status

  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_status(req, "200 OK");

  // boot out all clients

  rv = camwebsrv_sclients_purge(phttpd->sclients, phttpd->handle);

  if (rv != ESP_OK)
  {
    ESP_LOGE(CAMWEBSRV_TAG, "HTTPD _camwebsrv_httpd_handler_reset(): camwebsrv_sclients_purge() failed: [%d]: %s", rv, esp_err_to_name(rv));
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, NULL);
    return rv;
  }

  // reset

  rv = camwebsrv_camera_reset(phttpd->cam);

  if (rv != ESP_OK)
  {
    ESP_LOGE(CAMWEBSRV_TAG, "HTTPD _camwebsrv_httpd_handler_reset(): camwebsrv_camera_reset() failed: [%d]: %s", rv, esp_err_to_name(rv));
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, NULL);
    return rv;
  }

  // send response

  rv = httpd_resp_send(req, NULL, 0);

  if (rv != ESP_OK)
  {
    ESP_LOGE(CAMWEBSRV_TAG, "HTTPD _camwebsrv_httpd_handler_reset(): httpd_resp_send() failed: [%d]: %s", rv, esp_err_to_name(rv));
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, NULL);
    return rv;
  }

  ESP_LOGI(CAMWEBSRV_TAG, "HTTPD _camwebsrv_httpd_handler_reset(%d): served %s", httpd_req_to_sockfd(req), req->uri);

  return ESP_OK;
}

static esp_err_t _camwebsrv_httpd_handler_control(httpd_req_t *req)
{
  esp_err_t rv;
  size_t len;
  char *buf;
  char bvar[_CAMWEBSRV_HTTPD_PARAM_LEN];
  char bval[_CAMWEBSRV_HTTPD_PARAM_LEN];
  _camwebsrv_httpd_t *phttpd;

  phttpd = (_camwebsrv_httpd_t *) httpd_get_global_user_ctx(req->handle);

  // response type/header status

  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_type(req, "text/plain");
  httpd_resp_set_status(req, "200 OK");

  // how long is the query string?

  len = httpd_req_get_url_query_len(req) + 1;

  if (len == 0)
  {
    ESP_LOGE(CAMWEBSRV_TAG, "HTTPD _camwebsrv_httpd_handler_control(): failed; zero-length query string");
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, NULL);
    return ESP_FAIL;
  }

  // initialise buffer for query string

  buf = (char *) malloc(len + 1);

  if (buf == NULL)
  {
    int e = errno;
    ESP_LOGE(CAMWEBSRV_TAG, "HTTPD _camwebsrv_httpd_handler_control(): malloc() failed: [%d]: %s", e, strerror(e));
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, NULL);
    return ESP_FAIL;
  }

  memset(buf, 0x00, len + 1);
  memset(bvar, 0x00, sizeof(bvar));
  memset(bval, 0x00, sizeof(bval));

  // retrieve query string

  rv = httpd_req_get_url_query_str(req, buf, len);

  if (rv != ESP_OK)
  {
    ESP_LOGE(CAMWEBSRV_TAG, "HTTPD _camwebsrv_httpd_handler_control(): httpd_req_get_url_query_str() failed: [%d]: %s", rv, esp_err_to_name(rv));
    free(buf);
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, NULL);
    return rv;
  }

  // get variable name from query string

  rv = httpd_query_key_value(buf, "var", bvar, sizeof(bvar) - 1);

  if (rv != ESP_OK)
  {
    free(buf);
    ESP_LOGE(CAMWEBSRV_TAG, "HTTPD _camwebsrv_httpd_handler_control(): httpd_query_key_value(\"var\") failed: [%d]: %s", rv, esp_err_to_name(rv));
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, NULL);
    return rv;
  }

  // get variable value from query string

  rv = httpd_query_key_value(buf, "val", bval, sizeof(bval) - 1);

  if (rv != ESP_OK)
  {
    free(buf);
    ESP_LOGE(CAMWEBSRV_TAG, "HTTPD _camwebsrv_httpd_handler_control(): httpd_query_key_value(\"val\") failed: [%d]: %s", rv, esp_err_to_name(rv));
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, NULL);
    return rv;
  }

  // we don't need the query string buffer anymore

  free(buf);

  // set camera variable

  rv = camwebsrv_camera_ctrl_set(phttpd->cam, bvar, atoi(bval));

  if (rv != ESP_OK)
  {
    ESP_LOGE(CAMWEBSRV_TAG, "HTTPD _camwebsrv_httpd_handler_control(): camwebsrv_camera_ctrl_set(\"%s\", %s) failed", bvar, bval);

    if (rv == ESP_ERR_INVALID_ARG)
    {
      httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, NULL);
    }
    else
    {
      httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, NULL);
    }

    return rv;
  }

  // send response

  rv = httpd_resp_send(req, NULL, 0);

  if (rv != ESP_OK)
  {
    ESP_LOGE(CAMWEBSRV_TAG, "HTTPD _camwebsrv_httpd_handler_control(): httpd_resp_send() failed: [%d]: %s", rv, esp_err_to_name(rv));
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, NULL);
    return rv;
  }

  ESP_LOGI(CAMWEBSRV_TAG, "HTTPD _camwebsrv_httpd_handler_control(%d): served %s", httpd_req_to_sockfd(req), req->uri);

  return ESP_OK;
}

static esp_err_t _camwebsrv_httpd_handler_capture(httpd_req_t *req)
{
  esp_err_t rv;
  uint8_t *fbuf = NULL;
  size_t flen = 0;
  _camwebsrv_httpd_t *phttpd;

  phttpd = (_camwebsrv_httpd_t *) httpd_get_global_user_ctx(req->handle);

  // response type/header status

  httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=capture.jpg");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_type(req, "image/jpeg");
  httpd_resp_set_status(req, "200 OK");

  rv = camwebsrv_camera_frame_grab(phttpd->cam, &fbuf, &flen, NULL);

  if (rv != ESP_OK)
  {
    ESP_LOGE(CAMWEBSRV_TAG, "HTTPD _camwebsrv_httpd_handler_capture(): camwebsrv_camera_frame_grab() failed: [%d]: %s", rv, esp_err_to_name(rv));
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, NULL);
    return rv;
  }

  rv = httpd_resp_send(req, (const char *) fbuf, (ssize_t) flen);

  camwebsrv_camera_frame_dispose(phttpd->cam);

  if (rv != ESP_OK)
  {
    ESP_LOGE(CAMWEBSRV_TAG, "HTTPD _camwebsrv_httpd_handler_capture(): httpd_resp_send() failed: [%d]: %s", rv, esp_err_to_name(rv));
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, NULL);
    return rv;
  }

  ESP_LOGI(CAMWEBSRV_TAG, "HTTPD _camwebsrv_httpd_handler_capture(%d): served %s", httpd_req_to_sockfd(req), req->uri);

  return ESP_OK;
}

static esp_err_t _camwebsrv_httpd_handler_stream(httpd_req_t *req)
{
  esp_err_t rv;
  _camwebsrv_httpd_t *phttpd;
  _camwebsrv_httpd_worker_arg_t *parg;

  phttpd = (_camwebsrv_httpd_t *) httpd_get_global_user_ctx(req->handle);

  parg = (_camwebsrv_httpd_worker_arg_t *) malloc(sizeof(_camwebsrv_httpd_worker_arg_t));

  if (parg == NULL)
  {
    int e = errno;
    ESP_LOGE(CAMWEBSRV_TAG, "HTTPD _camwebsrv_httpd_handler_stream(): malloc() failed: [%d]: %s", e, strerror(e));
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, NULL);
    return ESP_FAIL;
  }

  parg->phttpd = phttpd;
  parg->sockfd = httpd_req_to_sockfd(req);

  rv = httpd_queue_work(req->handle, _camwebsrv_httpd_worker, parg);

  if (rv != ESP_OK)
  {
    ESP_LOGE(CAMWEBSRV_TAG, "HTTPD _camwebsrv_httpd_handler_stream(): httpd_queue_work() failed: [%d]: %s", rv, esp_err_to_name(rv));
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, NULL);
    free(parg);
    return ESP_FAIL;
  }

  ESP_LOGI(CAMWEBSRV_TAG, "HTTPD _camwebsrv_httpd_handler_stream(%d): served %s", httpd_req_to_sockfd(req), req->uri);

  return ESP_OK;
}

// ---------------- Sequence capture endpoints ----------------

static bool _qv_int(const char *qs, const char *key, int *out)
{
  char tmp[_CAMWEBSRV_HTTPD_PARAM_LEN];
  memset(tmp, 0x00, sizeof(tmp));
  if (httpd_query_key_value(qs, key, tmp, sizeof(tmp) - 1) != ESP_OK)
  {
    return false;
  }
  *out = atoi(tmp);
  return true;
}

static bool _qv_str(const char *qs, const char *key, char *out, size_t outlen)
{
  if (out == NULL || outlen == 0) return false;
  memset(out, 0x00, outlen);
  if (httpd_query_key_value(qs, key, out, outlen - 1) != ESP_OK)
  {
    return false;
  }
  return true;
}

static pixformat_t _parse_pixformat(const char *s)
{
  if (s == NULL || s[0] == 0x00) return PIXFORMAT_JPEG;
  // numeric?
  if (s[0] >= '0' && s[0] <= '9') return (pixformat_t)atoi(s);
  if (strcasecmp(s, "jpeg") == 0) return PIXFORMAT_JPEG;
  if (strcasecmp(s, "rgb565") == 0) return PIXFORMAT_RGB565;
  if (strcasecmp(s, "yuv422") == 0) return PIXFORMAT_YUV422;
  if (strcasecmp(s, "grayscale") == 0) return PIXFORMAT_GRAYSCALE;
  if (strcasecmp(s, "rgb888") == 0) return PIXFORMAT_RGB888;
  if (strcasecmp(s, "raw") == 0) return PIXFORMAT_RAW;
  return PIXFORMAT_JPEG;
}

static framesize_t _parse_framesize(const char *s)
{
  if (s == NULL || s[0] == 0x00) return FRAMESIZE_UXGA;
  // numeric?
  if (s[0] >= '0' && s[0] <= '9') return (framesize_t)atoi(s);

  // Accept common enum names, case-insensitive.
  #define FS(name) if (strcasecmp(s, #name) == 0) return FRAMESIZE_##name
  FS(QQVGA);
  #ifdef FRAMESIZE_QQVGA2    
  FS(QQVGA2);
#endif
  FS(QCIF);
  FS(HQVGA);
  FS(240X240);
  FS(QVGA);
  FS(CIF);
  FS(HVGA);
  FS(VGA);
  FS(SVGA);
  FS(XGA);
  FS(HD);
  FS(SXGA);
  FS(UXGA);
  FS(FHD);
  FS(P_HD);
  FS(P_3MP);
  FS(QXGA);
  FS(QHD);
  FS(WQXGA);
  FS(P_FHD);
  FS(QSXGA);
  #undef FS
  return FRAMESIZE_UXGA;
}

static void _cfg_try_int(const char *qs, const char *key, bool *has, int *val)
{
  int tmp;
  if (_qv_int(qs, key, &tmp))
  {
    *has = true;
    *val = tmp;
  }
}




static esp_err_t _camwebsrv_httpd_handler_seq_cap(httpd_req_t *req)
{
  _camwebsrv_httpd_t *phttpd = (_camwebsrv_httpd_t *)httpd_get_global_user_ctx(req->handle);

  // Only master should accept /seq_cap
  const char *role = "master";
  camwebsrv_cfgman_get(phttpd->cfgman, CAMWEBSRV_CFGMAN_KEY_ROLE, &role);
  if (strcasecmp(role, "master") != 0)
  {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Not master");
    return ESP_FAIL;
  }

  size_t len = httpd_req_get_url_query_len(req) + 1;
  if (len <= 1)
  {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing query");
    return ESP_FAIL;
  }

  char *qs = (char *)calloc(1, len + 1);
  if (!qs)
  {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, NULL);
    return ESP_FAIL;
  }

  esp_err_t rv = httpd_req_get_url_query_str(req, qs, len);
  if (rv != ESP_OK)
  {
    free(qs);
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, NULL);
    return rv;
  }

  memset(&seqcap_cfg, 0x00, sizeof(seqcap_cfg));

  char pf[_CAMWEBSRV_HTTPD_PARAM_LEN];
  char sz[_CAMWEBSRV_HTTPD_PARAM_LEN];
  char name[sizeof(seqcap_cfg.cap_seq_name)];
  memset(pf, 0x00, sizeof(pf));
  memset(sz, 0x00, sizeof(sz));
  memset(name, 0x00, sizeof(name));

  // required args
  _qv_str(qs, "pixformat", pf, sizeof(pf));
  // size can be provided as 'size' (requested) or 'framesize'
  if (!_qv_str(qs, "size", sz, sizeof(sz)))
  {
    _qv_str(qs, "framesize", sz, sizeof(sz));
  }
  if (!_qv_str(qs, "cap_seq_name", name, sizeof(name)))
  {
    free(qs);
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing cap_seq_name");
    return ESP_FAIL;
  }
  int cap_amount = 0;
  if (!_qv_int(qs, "cap_amount", &cap_amount) || cap_amount <= 0)
  {
    free(qs);
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing cap_amount");
    return ESP_FAIL;
  }

  seqcap_cfg.pixformat = _parse_pixformat(pf);
  seqcap_cfg.framesize = _parse_framesize(sz);
  strncpy(seqcap_cfg.cap_seq_name, name, sizeof(seqcap_cfg.cap_seq_name) - 1);
  seqcap_cfg.cap_amount = cap_amount;

  // optional timing
  seqcap_cfg.slave_prepare_delay_ms = 200;
  seqcap_cfg.inter_frame_delay_ms = 0;
  _qv_int(qs, "slave_prepare_delay_ms", &seqcap_cfg.slave_prepare_delay_ms);
  _qv_int(qs, "inter_frame_delay_ms", &seqcap_cfg.inter_frame_delay_ms);

  // optional camera settings
  _cfg_try_int(qs, "quality", &seqcap_cfg.has_quality, &seqcap_cfg.quality);
  _cfg_try_int(qs, "brightness", &seqcap_cfg.has_brightness, &seqcap_cfg.brightness);
  _cfg_try_int(qs, "contrast", &seqcap_cfg.has_contrast, &seqcap_cfg.contrast);
  _cfg_try_int(qs, "saturation", &seqcap_cfg.has_saturation, &seqcap_cfg.saturation);
  _cfg_try_int(qs, "sharpness", &seqcap_cfg.has_sharpness, &seqcap_cfg.sharpness);
  _cfg_try_int(qs, "special_effect", &seqcap_cfg.has_special_effect, &seqcap_cfg.special_effect);
  _cfg_try_int(qs, "wb_mode", &seqcap_cfg.has_wb_mode, &seqcap_cfg.wb_mode);
  _cfg_try_int(qs, "aec", &seqcap_cfg.has_aec, &seqcap_cfg.aec);
  _cfg_try_int(qs, "aec2", &seqcap_cfg.has_aec2, &seqcap_cfg.aec2);
  _cfg_try_int(qs, "aec_value", &seqcap_cfg.has_aec_value, &seqcap_cfg.aec_value);
  _cfg_try_int(qs, "ae_level", &seqcap_cfg.has_ae_level, &seqcap_cfg.ae_level);
  _cfg_try_int(qs, "agc", &seqcap_cfg.has_agc, &seqcap_cfg.agc);
  _cfg_try_int(qs, "agc_gain", &seqcap_cfg.has_agc_gain, &seqcap_cfg.agc_gain);
  _cfg_try_int(qs, "gainceiling", &seqcap_cfg.has_gainceiling, &seqcap_cfg.gainceiling);
  _cfg_try_int(qs, "awb", &seqcap_cfg.has_awb, &seqcap_cfg.awb);
  _cfg_try_int(qs, "awb_gain", &seqcap_cfg.has_awb_gain, &seqcap_cfg.awb_gain);
  _cfg_try_int(qs, "dcw", &seqcap_cfg.has_dcw, &seqcap_cfg.dcw);
  _cfg_try_int(qs, "bpc", &seqcap_cfg.has_bpc, &seqcap_cfg.bpc);
  _cfg_try_int(qs, "wpc", &seqcap_cfg.has_wpc, &seqcap_cfg.wpc);
  _cfg_try_int(qs, "hmirror", &seqcap_cfg.has_hmirror, &seqcap_cfg.hmirror);
  _cfg_try_int(qs, "vflip", &seqcap_cfg.has_vflip, &seqcap_cfg.vflip);
  _cfg_try_int(qs, "lenc", &seqcap_cfg.has_lenc, &seqcap_cfg.lenc);
  _cfg_try_int(qs, "raw_gma", &seqcap_cfg.has_raw_gma, &seqcap_cfg.raw_gma);
  _cfg_try_int(qs, "colorbar", &seqcap_cfg.has_colorbar, &seqcap_cfg.colorbar);

  // Determine slave host
  char slave_host[96] = {0};
  if (!_qv_str(qs, "slave_host", slave_host, sizeof(slave_host)))
  {
    const char *pair_id = "0";
    camwebsrv_cfgman_get(phttpd->cfgman, CAMWEBSRV_CFGMAN_KEY_PAIR_ID, &pair_id);
    snprintf(slave_host, sizeof(slave_host), "cam-slave-%s.local", pair_id);
  }

  free(qs);

  // Respond immediately so the HTTP client doesn't time out
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_sendstr(req, "{\"ok\":true,\"started\":true}");

  // Start capture task (it will stop Wi-Fi/httpd and restore them when done)

  // print seqcap_cfg for debugging
  ESP_LOGI(CAMWEBSRV_TAG, "HTTPD /seq_cap: Starting sequence capture with config:");
  ESP_LOGI(CAMWEBSRV_TAG, "  pixformat: %d", seqcap_cfg.pixformat);
  ESP_LOGI(CAMWEBSRV_TAG, "  framesize: %d", seqcap_cfg.framesize);
  ESP_LOGI(CAMWEBSRV_TAG, "  cap_seq_name: %s", seqcap_cfg.cap_seq_name);
  ESP_LOGI(CAMWEBSRV_TAG, "  cap_amount: %d", seqcap_cfg.cap_amount);
  ESP_LOGI(CAMWEBSRV_TAG, "  slave_prepare_delay_ms: %d", seqcap_cfg.slave_prepare_delay_ms);
  ESP_LOGI(CAMWEBSRV_TAG, "  inter_frame_delay_ms: %d", seqcap_cfg.inter_frame_delay_ms);
  if (seqcap_cfg.has_quality) ESP_LOGI(CAMWEBSRV_TAG, "  quality: %d", seqcap_cfg.quality);
  if (seqcap_cfg.has_brightness) ESP_LOGI(CAMWEBSRV_TAG, "  brightness: %d", seqcap_cfg.brightness);
  if (seqcap_cfg.has_contrast) ESP_LOGI(CAMWEBSRV_TAG, "  contrast: %d", seqcap_cfg.contrast);
  if (seqcap_cfg.has_saturation) ESP_LOGI(CAMWEBSRV_TAG, "  saturation: %d", seqcap_cfg.saturation);

  rv = camwebsrv_seqcap_start_master(phttpd->cam, (camwebsrv_httpd_t)phttpd, &seqcap_cfg, slave_host);
  if (rv != ESP_OK)
  {
    ESP_LOGE(CAMWEBSRV_TAG, "HTTPD /seq_cap: camwebsrv_seqcap_start_master failed: %s", esp_err_to_name(rv));
  }

  return ESP_OK;
}

static esp_err_t _camwebsrv_httpd_handler_cap_seq_init(httpd_req_t *req)
{
  _camwebsrv_httpd_t *phttpd = (_camwebsrv_httpd_t *)httpd_get_global_user_ctx(req->handle);

  // Only slave should accept /cap_seq_init
  const char *role = "slave";
  camwebsrv_cfgman_get(phttpd->cfgman, CAMWEBSRV_CFGMAN_KEY_ROLE, &role);
  if (strcasecmp(role, "slave") != 0)
  {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Not slave");
    return ESP_FAIL;
  }

  size_t len = httpd_req_get_url_query_len(req) + 1;
  if (len <= 1)
  {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing query");
    return ESP_FAIL;
  }

  char *qs = (char *)calloc(1, len + 1);
  if (!qs)
  {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, NULL);
    return ESP_FAIL;
  }
  esp_err_t rv = httpd_req_get_url_query_str(req, qs, len);
  if (rv != ESP_OK)
  {
    free(qs);
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, NULL);
    return rv;
  }

  camwebsrv_seqcap_cfg_t cfg;
  memset(&cfg, 0x00, sizeof(cfg));

  int pf_i = (int)PIXFORMAT_JPEG;
  int fs_i = (int)FRAMESIZE_UXGA;
  _qv_int(qs, "pixformat", &pf_i);
  _qv_int(qs, "framesize", &fs_i);
  cfg.pixformat = (pixformat_t)pf_i;
  cfg.framesize = (framesize_t)fs_i;

  if (!_qv_str(qs, "cap_seq_name", cfg.cap_seq_name, sizeof(cfg.cap_seq_name)))
  {
    free(qs);
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing cap_seq_name");
    return ESP_FAIL;
  }

  if (!_qv_int(qs, "cap_amount", &cfg.cap_amount) || cfg.cap_amount <= 0)
  {
    free(qs);
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing cap_amount");
    return ESP_FAIL;
  }

  free(qs);

  // Ack immediately, then start slave capture task
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_sendstr(req, "{\"ok\":true,\"prepared\":true}");

  rv = camwebsrv_seqcap_start_slave(phttpd->cam, (camwebsrv_httpd_t)phttpd, &cfg);
  if (rv != ESP_OK)
  {
    ESP_LOGE(CAMWEBSRV_TAG, "HTTPD /cap_seq_init: camwebsrv_seqcap_start_slave failed: %s", esp_err_to_name(rv));
  }

  return ESP_OK;
}

static bool _camwebsrv_httpd_static_cb(const char *buf, size_t len, void *arg)
{
  esp_err_t rv;
  httpd_req_t *req = (httpd_req_t *) arg;

  rv = httpd_resp_send(req, buf, len);

  if (rv != ESP_OK)
  {
    ESP_LOGE(CAMWEBSRV_TAG, "HTTPD _camwebsrv_httpd_static_cb(): httpd_resp_send() failed: [%d]: %s", rv, esp_err_to_name(rv));
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, NULL);
  }

  return true;
}

static void _camwebsrv_httpd_worker(void *arg)
{
  _camwebsrv_httpd_worker_arg_t *parg;
  esp_err_t rv;

  parg = (_camwebsrv_httpd_worker_arg_t *) arg;

  rv = camwebsrv_sclients_add(parg->phttpd->sclients, parg->sockfd);

  if (rv != ESP_OK)
  {
    ESP_LOGE(CAMWEBSRV_TAG, "HTTPD _camwebsrv_httpd_worker(): camwebsrv_sclients_add() failed: [%d]: %s", rv, esp_err_to_name(rv));
    httpd_sess_trigger_close(parg->phttpd->handle, parg->sockfd);
  }

  // trigger new event
  
  xSemaphoreGive(parg->phttpd->sema);

  free(parg);
}

static void _camwebsrv_httpd_noop(void *arg)
{
}
