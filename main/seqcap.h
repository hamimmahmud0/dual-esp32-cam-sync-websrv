// 2026-01-16 seqcap.h
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#ifndef _CAMWEBSRV_SEQCAP_H
#define _CAMWEBSRV_SEQCAP_H

#include "camera.h"
#include <esp_camera.h>  // pixformat_t, framesize_t, PIXFORMAT_*, FRAMESIZE_*

#include <esp_err.h>
#include <stdbool.h>

// Forward declaration (httpd.h also defines this)
typedef void *camwebsrv_httpd_t;



typedef struct
{
  // Required
  pixformat_t pixformat;
  framesize_t framesize;
  char cap_seq_name[64];
  int cap_amount;

  // Optional: pass-through camera controls (set only if present)
  bool has_quality; int quality;
  bool has_brightness; int brightness;
  bool has_contrast; int contrast;
  bool has_saturation; int saturation;
  bool has_sharpness; int sharpness;
  bool has_special_effect; int special_effect;
  bool has_wb_mode; int wb_mode;
  bool has_aec; int aec;
  bool has_aec2; int aec2;
  bool has_aec_value; int aec_value;
  bool has_ae_level; int ae_level;
  bool has_agc; int agc;
  bool has_agc_gain; int agc_gain;
  bool has_gainceiling; int gainceiling;
  bool has_awb; int awb;
  bool has_awb_gain; int awb_gain;
  bool has_dcw; int dcw;
  bool has_bpc; int bpc;
  bool has_wpc; int wpc;
  bool has_hmirror; int hmirror;
  bool has_vflip; int vflip;
  bool has_lenc; int lenc;
  bool has_raw_gma; int raw_gma;
  bool has_colorbar; int colorbar;

  // Timing
  int slave_prepare_delay_ms; // master waits after init request
  int inter_frame_delay_ms;   // master waits between frames
} camwebsrv_seqcap_cfg_t;



typedef struct
{
  camwebsrv_camera_t cam;
  camwebsrv_httpd_t httpd;
  camwebsrv_seqcap_cfg_t *cfg;
  char slave_host[80];
  bool is_master;
} seqcap_task_arg_t;


extern camwebsrv_seqcap_cfg_t seqcap_cfg;
extern seqcap_task_arg_t seqcap_task_arg;

// Global "capture mode" gate used by main loop to pause ping/http servicing.
bool camwebsrv_seqcap_is_active(void);

// Master sequence: configure slave over HTTP, stop Wi-Fi/httpd, pulse GPIO, capture/write.
// 'slave_host' can be mDNS hostname (e.g., "cam-slave-<id>.local") or IP.
esp_err_t camwebsrv_seqcap_start_master(camwebsrv_camera_t cam, camwebsrv_httpd_t httpd, camwebsrv_seqcap_cfg_t *cfg, const char *slave_host);

// Slave prepares config; once started it stops Wi-Fi/httpd and waits for GPIO interrupts.
esp_err_t camwebsrv_seqcap_start_slave(camwebsrv_camera_t cam, camwebsrv_httpd_t httpd, camwebsrv_seqcap_cfg_t *cfg);

#endif
