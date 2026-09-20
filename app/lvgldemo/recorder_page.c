/****************************************************************************
 * apps/examples/lvgldemo/recorder_page.c
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.  The
 * ASF licenses this file to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance with the
 * License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <dirent.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <errno.h>
#include <time.h>
#include <lvgl/lvgl.h>

#include "recorder_page.h"
#include "player_page.h"
#include "wifi_page.h"
#include "lvgldemo_common.h"
#include "lvgl_dispatch.h"
#include "wakeup_detector.h"
#include "voice_assistant.h"
#include "i18n.h"
#include "menu_page.h"

#ifdef CONFIG_MEDIA
#include <media_recorder.h>
#include <media_policy.h>
#include <media_defs.h>

/* BES audioflinger API for hardware mic gain control.
 * The default per-channel ADC gain is CODEC_SADC_VOL=5 → 8dB.
 * Setting it to 15 → 36dB (maximum) gives ~28dB more gain.
 * This function is provided by the BES audio framework and is
 * safe to call — if the stream is not open, it just returns error.
 * A weak fallback is provided so the build succeeds even if the
 * BES audioflinger is not linked. */
extern uint32_t af_stream_set_chan_vol(uint32_t id, uint32_t stream,
                                       uint32_t ch_map, uint8_t vol)
                                       __attribute__((weak));

/* BES audioflinger stream constants (raw values from hal_aud.h) */
#define BES_AUD_STREAM_ID_0     0   /* AUD_STREAM_ID_0 */
#define BES_AUD_STREAM_CAPTURE  1   /* AUD_STREAM_CAPTURE */
#define BES_AUD_CHANNEL_MAP_CH0 1   /* AUD_CHANNEL_MAP_CH0 = mic1 */
#define BES_MIC_GAIN_MAX        15  /* Maximum mic gain index (36dB) */

#endif /* CONFIG_MEDIA */

/****************************************************************************
 * Recorder Page Data
 ****************************************************************************/

typedef enum {
  RECORDER_STATE_IDLE,
  RECORDER_STATE_RECORDING,
  RECORDER_STATE_PAUSED
} recorder_state_t;

static lv_obj_t * recorder_time_label = NULL;
static lv_obj_t * recorder_start_btn = NULL;
static lv_obj_t * recorder_stop_btn = NULL;
static lv_obj_t * recorder_file_list = NULL;
static lv_obj_t * recorder_file_count_label = NULL;
static lv_obj_t * recorder_status_dot = NULL;  /* 录音状态指示红点 */
static recorder_state_t recorder_state = RECORDER_STATE_IDLE;
static uint32_t recorder_time = 0;
static lv_timer_t * recorder_timer = NULL;
static char recorder_path[64];

/* 列表项样式常量（参考 menu_page）
 * 高度 50px：montserrat_24 字体单行显示 + padding */
#define REC_ITEM_HEIGHT    50
#define REC_ITEM_GAP       8
#define REC_ITEM_START_Y   5
/* 列表布局边距：左右各 50px，底部 100px（圆形屏适配） */
#define REC_LIST_PAD_LR     50
#define REC_LIST_PAD_BOT    100

/* File list data: indexed by list position, holds filenames for delete dialog */
static char recorder_files[RECORDER_MAX_FILES][32];
static int recorder_file_count = 0;

/* Delete confirmation dialog state */
static lv_obj_t * recorder_delete_dialog = NULL;
static int recorder_delete_pending_idx = -1;

/* Custom long-press timer (bypasses LVGL's slow LV_INDEV_MODE_EVENT detection) */
static lv_timer_t * recorder_long_press_timer = NULL;
static int recorder_long_press_pending_idx = -1;

/* Forward declarations */
static void recorder_scan_files(void);
static void recorder_cancel_long_press_timer(void);

#ifdef CONFIG_MEDIA
static void * recorder_handle = NULL;

/****************************************************************************
 * Recorder Functions (media_recorder API)
 ****************************************************************************/

int recorder_open(void)
{
  /* Ensure mic is not muted: mute=1 means mute mode is OFF (mic active).
   * If the mic is left in muted state, recordings will be silent. */
  media_policy_set_mic_mute(1);

  /* ---- Set recording-related volume policy criteria to maximum ----
   *
   * The audio capture pipeline has volume controls similar to playback:
   *
   *   1) RecordVolume — controls the recording input gain at the policy
   *      level. Default is 5 (range [0,10]), causing 50% attenuation.
   *
   *   2) MediaVolume — even though this is primarily for playback, the
   *      PFW MediaVolumeDomain also affects shared audio routing. Setting
   *      it to maximum ensures no attenuation in the shared path.
   */

  int vol_min = 0, vol_max = 10;
  media_policy_get_range(MEDIA_SCENARIO_RECORD MEDIA_POLICY_VOLUME,
                         &vol_min, &vol_max);
  media_policy_set_stream_volume(MEDIA_SCENARIO_RECORD, vol_max);
  LV_LOG_USER("RecordVolume set to max %d (range %d-%d)",
              vol_max, vol_min, vol_max);

  /* Also set MediaVolume to maximum for consistent audio routing. */
  vol_min = 0; vol_max = 15;
  media_policy_get_range(MEDIA_STREAM_MEDIA MEDIA_POLICY_VOLUME,
                         &vol_min, &vol_max);
  media_policy_set_stream_volume(MEDIA_STREAM_MEDIA, vol_max);
  LV_LOG_USER("MediaVolume set to max %d (range %d-%d)",
              vol_max, vol_min, vol_max);

  media_policy_include("SelCap", "mic1", 1);
  recorder_handle = media_recorder_open(MEDIA_SOURCE_MIC);
  if (recorder_handle == NULL) {
    LV_LOG_ERROR("media_recorder_open error");
    media_policy_exclude("SelCap", "mic1", 1);
    return -1;
  }
  return 0;
}

int recorder_close(void)
{
  if (recorder_handle == NULL) return -1;
  int ret = media_recorder_close(recorder_handle);
  if (ret != 0) {
    LV_LOG_ERROR("media_recorder_close error %d", ret);
    return -1;
  }
  recorder_handle = NULL;
#ifdef CONFIG_MEDIA
  media_policy_exclude("SelCap", "mic1", 1);
#endif
  return 0;
}

static int recorder_prepare(const char * url)
{
  if (recorder_handle == NULL) return -1;
  int ret = media_recorder_prepare(recorder_handle, url,
    "mux=[keys=wav],fmt=[rate=#16000,ch=#1,bits=#16,width=#2],enc=[keys=pcm,imin=#1920]");
  if (ret != 0) {
    LV_LOG_ERROR("media_recorder_prepare error %d", ret);
    return -1;
  }
  return 0;
}

static int recorder_start(void)
{
  if (recorder_handle == NULL) return -1;
  int ret = media_recorder_start(recorder_handle);
  if (ret != 0) {
    LV_LOG_ERROR("media_recorder_start error %d", ret);
    return -1;
  }

  /* Set hardware mic gain to maximum AFTER the audio stream has started.
   *
   * The BES platform configures per-channel ADC gain from CODEC_SADC_VOL
   * during codec stream setup. On glass_demo (best1700), this defaults to
   * CODEC_SADC_VOL=15 → codec_adc_vol[15]=36dB (maximum).
   *
   * af_stream_set_chan_vol() directly programs the ADC digital gain
   * register, overriding the board default. It requires the audio
   * capture stream to be open (which it is after recorder_start).
   */
  if (af_stream_set_chan_vol) {
    uint32_t af_ret = af_stream_set_chan_vol(
        BES_AUD_STREAM_ID_0,
        BES_AUD_STREAM_CAPTURE,
        BES_AUD_CHANNEL_MAP_CH0,
        BES_MIC_GAIN_MAX);
    LV_LOG_USER("mic gain set to max (36dB), af_ret=%u", af_ret);
  } else {
    LV_LOG_WARN("af_stream_set_chan_vol not available, mic gain unchanged");
  }

  return 0;
}

static int recorder_stop(void)
{
  if (recorder_handle == NULL) return -1;
  int ret = media_recorder_stop(recorder_handle);
  if (ret != 0) {
    LV_LOG_ERROR("media_recorder_stop error %d", ret);
    return -1;
  }
  return 0;
}

static int recorder_pause(void)
{
  if (recorder_handle == NULL) return -1;
  int ret = media_recorder_pause(recorder_handle);
  if (ret != 0) {
    LV_LOG_ERROR("media_recorder_pause error %d", ret);
    return -1;
  }
  return 0;
}

#else /* !CONFIG_MEDIA */

int recorder_open(void) { return 0; }
int recorder_close(void) { return 0; }
static int recorder_prepare(const char * url) { (void)url; return 0; }
static int recorder_start(void) { return 0; }
static int recorder_stop(void) { return 0; }
static int recorder_pause(void) { return 0; }

#endif /* CONFIG_MEDIA */

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/* Back button click handler */
static void recorder_back_btn_cb(lv_event_t * e)
{
  LV_UNUSED(e);
  if (wifi_screen == NULL) {
    wifi_screen = lv_obj_create(NULL);
    create_wifi_page(wifi_screen);
  }
  lv_scr_load_anim(wifi_screen, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 300, 0, false);
}

/* Recorder file list item click — navigate to player and play */
static void recorder_file_click_cb(lv_event_t * e)
{
  LV_UNUSED(e);
  /* Ignore click if delete dialog is showing (long-press just fired) */
  if (recorder_delete_dialog != NULL) return;

  player_scan_files();
  player_open();
  if (player_screen == NULL) {
    player_screen = lv_obj_create(NULL);
    create_player_page(player_screen);
  }
  lv_scr_load_anim(player_screen, LV_SCR_LOAD_ANIM_MOVE_LEFT, 300, 0, false);
}

/* ── Recorder file list long-press: delete confirmation dialog ── */

static void recorder_delete_dialog_close(void)
{
  if (recorder_delete_dialog) {
    lv_obj_del(recorder_delete_dialog);
    recorder_delete_dialog = NULL;
  }
  recorder_delete_pending_idx = -1;
}

static void recorder_delete_cancel_cb(lv_event_t * e)
{
  LV_UNUSED(e);
  recorder_delete_dialog_close();
}

static void recorder_delete_confirm_cb(lv_event_t * e)
{
  LV_UNUSED(e);
  int idx = recorder_delete_pending_idx;
  recorder_delete_dialog_close();

  if (idx < 0 || idx >= recorder_file_count) return;

  char fullpath[128];
  snprintf(fullpath, sizeof(fullpath), "%s%s",
           RECORDER_PATH_PREFIX, recorder_files[idx]);

  if (remove(fullpath) == 0) {
    LV_LOG_USER("recorder deleted: %s", fullpath);
  } else {
    LV_LOG_ERROR("recorder delete failed: %s (errno=%d)", fullpath, errno);
  }

  /* Sync filesystem, then refresh the file list */
  sync();
  recorder_scan_files();
}

/*
 * Custom 3-second long-press via a oneshot LVGL timer.
 * We bypass LV_EVENT_LONG_PRESSED because LV_INDEV_MODE_EVENT
 * uses a 3000ms-period timer with strict '>' comparison,
 * resulting in ~6000ms effective delay.
 */

static void recorder_long_press_timer_cb(lv_timer_t * t)
{
  LV_UNUSED(t);
  recorder_long_press_timer = NULL;

  int idx = recorder_long_press_pending_idx;
  recorder_long_press_pending_idx = -1;

  if (idx < 0 || idx >= recorder_file_count) return;

  /* Close any existing dialog first */
  recorder_delete_dialog_close();
  recorder_delete_pending_idx = idx;

  /* Semi-transparent overlay */
  recorder_delete_dialog = lv_obj_create(lv_layer_top());
  lv_obj_set_size(recorder_delete_dialog, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_bg_color(recorder_delete_dialog, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(recorder_delete_dialog, LV_OPA_50, 0);
  lv_obj_clear_flag(recorder_delete_dialog, LV_OBJ_FLAG_SCROLLABLE);

  /* Dialog panel */
  lv_obj_t * panel = lv_obj_create(recorder_delete_dialog);
  lv_obj_set_size(panel, 280, 140);
  lv_obj_center(panel);
  lv_obj_set_style_bg_color(panel, lv_color_hex(0x2A2A2A), 0);
  lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(panel, 12, 0);
  lv_obj_set_style_border_width(panel, 1, 0);
  lv_obj_set_style_border_color(panel, lv_color_hex(0x555555), 0);
  lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

  /* Title */
  lv_obj_t * title = lv_label_create(panel);
  lv_label_set_text(title, i18n_get(STR_REC_DELETE_TITLE));
  lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
  i18n_apply_font(title);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 15);

  /* Filename */
  lv_obj_t * fname = lv_label_create(panel);
  lv_label_set_text(fname, recorder_files[idx]);
  lv_label_set_long_mode(fname, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(fname, 240);
  lv_obj_set_style_text_color(fname, lv_color_hex(0xCCCCCC), 0);
  i18n_apply_font_small(fname);
  lv_obj_align(fname, LV_ALIGN_TOP_MID, 0, 40);

  /* Cancel button */
  lv_obj_t * cancel_btn = lv_button_create(panel);
  lv_obj_set_size(cancel_btn, 100, 36);
  lv_obj_align(cancel_btn, LV_ALIGN_BOTTOM_LEFT, 20, -20);
  lv_obj_set_style_bg_color(cancel_btn, lv_color_hex(0x555555), 0);
  lv_obj_set_style_radius(cancel_btn, 6, 0);
  lv_obj_t * cancel_lbl = lv_label_create(cancel_btn);
  lv_label_set_text(cancel_lbl, i18n_get(STR_REC_CANCEL));
  i18n_apply_font_small(cancel_lbl);
  lv_obj_center(cancel_lbl);
  lv_obj_add_event_cb(cancel_btn, recorder_delete_cancel_cb, LV_EVENT_CLICKED, NULL);

  /* Delete button */
  lv_obj_t * del_btn = lv_button_create(panel);
  lv_obj_set_size(del_btn, 100, 36);
  lv_obj_align(del_btn, LV_ALIGN_BOTTOM_RIGHT, -20, -20);
  lv_obj_set_style_bg_color(del_btn, lv_color_hex(0xFF3333), 0);
  lv_obj_set_style_radius(del_btn, 6, 0);
  lv_obj_t * del_lbl = lv_label_create(del_btn);
  lv_label_set_text(del_lbl, i18n_get(STR_REC_DELETE));
  i18n_apply_font_small(del_lbl);
  lv_obj_center(del_lbl);
  lv_obj_add_event_cb(del_btn, recorder_delete_confirm_cb, LV_EVENT_CLICKED, NULL);
}

/* On press: start a 3-second oneshot timer for long-press detection */
static void recorder_file_press_cb(lv_event_t * e)
{
  lv_obj_t * btn = lv_event_get_target(e);
  int idx = (int)(intptr_t)lv_obj_get_user_data(btn);
  if (idx < 0 || idx >= recorder_file_count) return;

  /* Cancel any previous timer first */
  recorder_cancel_long_press_timer();

  recorder_long_press_pending_idx = idx;
  recorder_long_press_timer = lv_timer_create(
      recorder_long_press_timer_cb, 3000, NULL);
  lv_timer_set_repeat_count(recorder_long_press_timer, 1);
}

/* On release: cancel the pending long-press timer */
static void recorder_file_release_cb(lv_event_t * e)
{
  LV_UNUSED(e);
  recorder_cancel_long_press_timer();
}

static void recorder_cancel_long_press_timer(void)
{
  if (recorder_long_press_timer) {
    lv_timer_del(recorder_long_press_timer);
    recorder_long_press_timer = NULL;
  }
  recorder_long_press_pending_idx = -1;
}

/* Recorder timer callback */
static void recorder_timer_cb(lv_timer_t * timer)
{
  LV_UNUSED(timer);
  recorder_time++;
  if (recorder_time_label) {
    lv_label_set_text_fmt(recorder_time_label, "%02"LV_PRIu32":%02"LV_PRIu32":%02"LV_PRIu32,
                          recorder_time / 3600, (recorder_time % 3600) / 60, recorder_time % 60);
  }
  /* 录音中：红点闪烁（每秒切换显隐） */
  if (recorder_status_dot && recorder_state == RECORDER_STATE_RECORDING) {
    lv_obj_set_style_bg_opa(recorder_status_dot,
        (recorder_time % 2 == 0) ? LV_OPA_COVER : LV_OPA_40, 0);
  }
}

static void recorder_generate_path(void)
{
  time_t raw_time;
  struct tm time_info;
  time(&raw_time);
  localtime_r(&raw_time, &time_info);
  char time_str[20];
  strftime(time_str, sizeof(time_str), "%Y%m%d_%H%M%S", &time_info);
  lv_snprintf(recorder_path, sizeof(recorder_path), "%s%s.wav", RECORDER_PATH_PREFIX, time_str);
  LV_LOG_USER("recorder path: %s", recorder_path);
}

static void recorder_update_btn_states(void)
{
  switch (recorder_state) {
    case RECORDER_STATE_IDLE:
      lv_obj_set_style_bg_color(recorder_start_btn, lv_color_hex(0x0088FF), 0);
      lv_label_set_text(lv_obj_get_child(recorder_start_btn, 0), LV_SYMBOL_PLAY);
      lv_obj_clear_state(recorder_start_btn, LV_STATE_DISABLED);
      lv_obj_add_state(recorder_stop_btn, LV_STATE_DISABLED);
      break;
    case RECORDER_STATE_RECORDING:
      lv_obj_set_style_bg_color(recorder_start_btn, lv_color_hex(0x999999), 0);
      lv_label_set_text(lv_obj_get_child(recorder_start_btn, 0), LV_SYMBOL_PLAY);
      lv_obj_add_state(recorder_start_btn, LV_STATE_DISABLED);
      lv_obj_clear_state(recorder_stop_btn, LV_STATE_DISABLED);
      break;
    case RECORDER_STATE_PAUSED:
      lv_obj_set_style_bg_color(recorder_start_btn, lv_color_hex(0x0088FF), 0);
      lv_label_set_text(lv_obj_get_child(recorder_start_btn, 0), LV_SYMBOL_PLAY);
      lv_obj_clear_state(recorder_start_btn, LV_STATE_DISABLED);
      lv_obj_clear_state(recorder_stop_btn, LV_STATE_DISABLED);
      break;
  }
}

static int name_cmp_asc(const void * a, const void * b)
{
  return strcmp((const char *)a, (const char *)b);
}

/* 创建文件列表项（参考 menu_page create_menu_item 模式）
 * - 透明背景容器，图标+文字换行显示
 * - 文件名超长时换行显示，不滚动播报
 * - 圆形屏适配：宽度缩短50px+右移50px，与 menu_page 一致 */
static void create_recorder_file_item(lv_obj_t *parent, int index,
                                       const char *filename, int idx)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, LV_HOR_RES - 2 * REC_LIST_PAD_LR, REC_ITEM_HEIGHT);
    lv_obj_align(btn, LV_ALIGN_TOP_LEFT, 0,
                 REC_ITEM_START_Y + index * (REC_ITEM_HEIGHT + REC_ITEM_GAP));

    lv_obj_set_style_bg_color(btn, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn, 10, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_shadow_opa(btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x222222), LV_STATE_PRESSED);

    /* 内容容器 */
    lv_obj_t *container = lv_obj_create(btn);
    lv_obj_remove_style_all(container);
    lv_obj_set_size(container, lv_pct(100), lv_pct(100));
    lv_obj_clear_flag(container, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_pad_all(container, 8, 0);
    lv_obj_set_style_pad_left(container, 12, 0);
    lv_obj_set_style_pad_right(container, 12, 0);
    lv_obj_set_style_border_width(container, 0, 0);
    lv_obj_set_style_bg_opa(container, LV_OPA_TRANSP, 0);

    /* 左侧图标 */
    lv_obj_t *icon_label = lv_label_create(container);
    lv_obj_remove_style_all(icon_label);
    lv_label_set_text(icon_label, LV_SYMBOL_AUDIO);
    lv_obj_set_style_text_color(icon_label, lv_color_hex(0x0088FF), 0);
    lv_obj_set_style_text_font(icon_label, LV_FONT_DEFAULT, 0);
    lv_obj_align(icon_label, LV_ALIGN_LEFT_MID, 0, 0);

    /* 文件名标签：换行显示，不滚动播报 */
    lv_obj_t *name_label = lv_label_create(container);
    lv_obj_remove_style_all(name_label);
    lv_label_set_text(name_label, filename);
    lv_label_set_long_mode(name_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(name_label, lv_color_hex(0xFFFFFF), 0);
    i18n_apply_font_small(name_label);
    lv_obj_align(name_label, LV_ALIGN_LEFT_MID, 30, 0);
    /* 允许文字换行宽度自适应容器剩余空间：btn宽度 - 图标偏移30 - 左右pad 24 */
    lv_obj_set_width(name_label, LV_HOR_RES - 2 * REC_LIST_PAD_LR - 30 - 24);

    lv_obj_set_user_data(btn, (void *)(intptr_t)idx);
    lv_obj_add_event_cb(btn, recorder_file_click_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(btn, recorder_file_press_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(btn, recorder_file_release_cb, LV_EVENT_RELEASED, NULL);
}

static void recorder_scan_files(void)
{
  if (recorder_file_list == NULL) return;
  lv_obj_clean(recorder_file_list);

  DIR * dir = opendir(RECORDER_PATH_PREFIX);
  if (dir == NULL) {
    LV_LOG_USER("recorder dir not found: %s", RECORDER_PATH_PREFIX);
    lv_obj_t *empty = lv_label_create(recorder_file_list);
    lv_obj_remove_style_all(empty);
    lv_label_set_text(empty, i18n_get(STR_REC_EMPTY));
    lv_obj_set_style_text_color(empty, lv_color_hex(0x666666), 0);
    i18n_apply_font_small(empty);
    lv_obj_align(empty, LV_ALIGN_TOP_LEFT, 0, REC_ITEM_START_Y);
    if (recorder_file_count_label) {
      lv_label_set_text_fmt(recorder_file_count_label, "Total: 0");
    }
    return;
  }

  int total = 0;
  struct dirent * entry;
  while ((entry = readdir(dir)) != NULL) {
    const char * name = entry->d_name;
    size_t len = strlen(name);
    if (len > 4 && len < 32 && strcmp(name + len - 4, ".wav") == 0) {
      total++;
    }
  }
  rewinddir(dir);

  if (recorder_file_count_label) {
    lv_label_set_text_fmt(recorder_file_count_label, "Total: %d", total);
  }

  if (total == 0) {
    closedir(dir);
    lv_obj_t *empty = lv_label_create(recorder_file_list);
    lv_obj_remove_style_all(empty);
    lv_label_set_text(empty, i18n_get(STR_REC_EMPTY));
    lv_obj_set_style_text_color(empty, lv_color_hex(0x666666), 0);
    i18n_apply_font_small(empty);
    lv_obj_align(empty, LV_ALIGN_TOP_LEFT, 0, REC_ITEM_START_Y);
    return;
  }

  char (*names)[32] = malloc(total * 32);
  if (names == NULL) {
    closedir(dir);
    lv_obj_t *empty = lv_label_create(recorder_file_list);
    lv_obj_remove_style_all(empty);
    lv_label_set_text(empty, i18n_get(STR_REC_EMPTY));
    lv_obj_set_style_text_color(empty, lv_color_hex(0x666666), 0);
    i18n_apply_font_small(empty);
    lv_obj_align(empty, LV_ALIGN_TOP_LEFT, 0, REC_ITEM_START_Y);
    return;
  }

  int count = 0;
  while ((entry = readdir(dir)) != NULL && count < total) {
    const char * name = entry->d_name;
    size_t len = strlen(name);
    if (len > 4 && len < 32 && strcmp(name + len - 4, ".wav") == 0) {
      strncpy(names[count], name, 31);
      names[count][31] = '\0';
      count++;
    }
  }
  closedir(dir);

  qsort(names, count, sizeof(names[0]), name_cmp_asc);

  int start = count > RECORDER_MAX_FILES ? count - RECORDER_MAX_FILES : 0;
  recorder_file_count = 0;
  for (int i = count - 1; i >= start; i--) {
    int idx = count - 1 - i;
    strncpy(recorder_files[idx], names[i], 31);
    recorder_files[idx][31] = '\0';

    create_recorder_file_item(recorder_file_list, idx, names[i], idx);
    recorder_file_count++;
  }

  if (count == 0) {
    lv_obj_t *empty = lv_label_create(recorder_file_list);
    lv_obj_remove_style_all(empty);
    lv_label_set_text(empty, i18n_get(STR_REC_EMPTY));
    lv_obj_set_style_text_color(empty, lv_color_hex(0x666666), 0);
    i18n_apply_font_small(empty);
    lv_obj_align(empty, LV_ALIGN_TOP_LEFT, 0, REC_ITEM_START_Y);
  }

  free(names);
}

static void recorder_start_btn_cb(lv_event_t * e)
{
  LV_UNUSED(e);
  if (recorder_state == RECORDER_STATE_IDLE) {
    mkdir(RECORDER_PATH_PREFIX, 0777);
    recorder_generate_path();
    recorder_prepare(recorder_path);
    recorder_start();
    recorder_time = 0;
    lv_label_set_text(recorder_time_label, "00:00:00");
    if (recorder_timer) lv_timer_resume(recorder_timer);
    recorder_state = RECORDER_STATE_RECORDING;
  } else if (recorder_state == RECORDER_STATE_PAUSED) {
    recorder_start();
    if (recorder_timer) lv_timer_resume(recorder_timer);
    recorder_state = RECORDER_STATE_RECORDING;
  }
  recorder_update_btn_states();
}

static lv_timer_t * recorder_scan_timer = NULL;

static void recorder_scan_timer_cb(lv_timer_t * timer)
{
  LV_UNUSED(timer);
  recorder_close();
  sync();
  usleep(200000);
  sync();
  int fd = open(recorder_path, O_WRONLY);
  if (fd >= 0) {
    fsync(fd);
    close(fd);
  }
  sync();
  usleep(100000);
  sync();
  struct stat st;
  if (stat(recorder_path, &st) == 0) {
    LV_LOG_USER("recorder file: size=%ld", (long)st.st_size);
  } else {
    LV_LOG_USER("recorder file: stat failed");
  }
  recorder_scan_files();
  recorder_open();
  if (recorder_scan_timer) {
    lv_timer_del(recorder_scan_timer);
    recorder_scan_timer = NULL;
  }
}

static void recorder_stop_btn_cb(lv_event_t * e)
{
  LV_UNUSED(e);
  if (recorder_state == RECORDER_STATE_RECORDING || recorder_state == RECORDER_STATE_PAUSED) {
    recorder_stop();
    if (recorder_timer) lv_timer_pause(recorder_timer);
    lv_label_set_text(recorder_time_label, "00:00:00");
    recorder_state = RECORDER_STATE_IDLE;
    recorder_update_btn_states();
    if (recorder_scan_timer) lv_timer_del(recorder_scan_timer);
    recorder_scan_timer = lv_timer_create(recorder_scan_timer_cb, 1500, NULL);
    lv_timer_set_repeat_count(recorder_scan_timer, 1);
  } else {
    recorder_update_btn_states();
  }
}

/* Gesture event handler for recorder screen */
static void recorder_gesture_cb(lv_event_t * e)
{
  lv_indev_t *indev = lv_indev_active();
  lv_dir_t dir = lv_indev_get_gesture_dir(indev);
  if (dir == LV_DIR_RIGHT) {
    /* 右滑返回功能菜单页（与 ai_page/camera_page/meeting_page 一致） */
    /* 等待手指释放后再处理，防止动画期间手指抬起的 CLICKED 事件
     * 误触发菜单页按钮。比 lv_indev_reset 副作用小，不会干扰屏幕加载动画。 */
    lv_indev_wait_release(indev);
    if (menu_screen == NULL) {
      menu_screen = lv_obj_create(NULL);
      create_menu_page(menu_screen);
    }
    lv_scr_load_anim(menu_screen, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 300, 0, false);
  }
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

static void recorder_screen_unloaded_cb(lv_event_t * e);
static void recorder_screen_loaded_cb(lv_event_t * e);

/* Create Recorder page */
void create_recorder_page(lv_obj_t * parent)
{
  lv_obj_set_style_bg_color(parent, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);
  lv_obj_set_style_pad_left(parent, 0, 0);
  lv_obj_set_style_pad_right(parent, 0, 0);
  lv_obj_set_style_pad_top(parent, 0, 0);
  lv_obj_set_style_pad_bottom(parent, 0, 0);
  lv_obj_set_scrollbar_mode(parent, LV_SCROLLBAR_MODE_OFF);
  lv_obj_clear_flag(parent, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_clear_flag(parent, LV_OBJ_FLAG_SCROLL_ELASTIC);
  lv_obj_clear_flag(parent, LV_OBJ_FLAG_SCROLL_MOMENTUM);

  /* Title bar：透明背景，无圆角，无边框 */
  lv_obj_t * title_bar = lv_obj_create(parent);
  lv_obj_remove_style_all(title_bar);
  lv_obj_set_size(title_bar, LV_PCT(100), 36);
  lv_obj_align(title_bar, LV_ALIGN_TOP_MID, 0, 0);
  lv_obj_set_style_bg_opa(title_bar, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(title_bar, 0, 0);
  lv_obj_set_style_radius(title_bar, 0, 0);
  lv_obj_set_style_pad_all(title_bar, 3, 0);
  lv_obj_clear_flag(title_bar, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_clear_flag(title_bar, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_flag(title_bar, LV_OBJ_FLAG_GESTURE_BUBBLE);

  /* Back button：透明背景 */
  lv_obj_t * back_btn = lv_button_create(title_bar);
  lv_obj_set_size(back_btn, 30, 30);
  lv_obj_align(back_btn, LV_ALIGN_LEFT_MID, 3, 0);
  lv_obj_set_style_bg_opa(back_btn, LV_OPA_TRANSP, 0);
  lv_obj_set_style_shadow_opa(back_btn, LV_OPA_TRANSP, 0);
  lv_obj_t * back_label = lv_label_create(back_btn);
  lv_label_set_text(back_label, LV_SYMBOL_LEFT);
  lv_obj_set_style_text_color(back_label, lv_color_hex(0xFFFFFF), 0);
  lv_obj_center(back_label);
  lv_obj_add_event_cb(back_btn, recorder_back_btn_cb, LV_EVENT_CLICKED, NULL);

  /* Title text */
  lv_obj_t * title = lv_label_create(title_bar);
  lv_label_set_text(title, i18n_get(STR_REC_TITLE));
  lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
  i18n_apply_font(title);
  lv_obj_align(title, LV_ALIGN_CENTER, 0, 0);

  /* Time display：使用大号字体突出显示，左侧带录音状态指示红点 */
  recorder_time_label = lv_label_create(parent);
  lv_label_set_text(recorder_time_label, "00:00:00");
  lv_obj_set_style_text_color(recorder_time_label, lv_color_hex(0xFFFFFF), 0);
  i18n_apply_font_small(recorder_time_label);
  lv_obj_align(recorder_time_label, LV_ALIGN_TOP_MID, 0, 45);

  /* 录音状态指示红点：位于时间显示左侧，仅录音时显示 */
  recorder_status_dot = lv_obj_create(parent);
  lv_obj_remove_style_all(recorder_status_dot);
  lv_obj_set_size(recorder_status_dot, 12, 12);
  lv_obj_set_style_radius(recorder_status_dot, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(recorder_status_dot, lv_color_hex(0xFF3333), 0);
  lv_obj_set_style_bg_opa(recorder_status_dot, LV_OPA_TRANSP, 0);  /* 初始隐藏 */
  lv_obj_align_to(recorder_status_dot, recorder_time_label, LV_ALIGN_OUT_LEFT_MID, -8, 0);
  lv_obj_clear_flag(recorder_status_dot, LV_OBJ_FLAG_CLICKABLE);

  /* Button container */
  lv_obj_t * btn_cont = lv_obj_create(parent);
  lv_obj_remove_style_all(btn_cont);
  lv_obj_set_size(btn_cont, LV_PCT(100), 40);
  lv_obj_align(btn_cont, LV_ALIGN_TOP_MID, 0, 70);
  lv_obj_set_flex_flow(btn_cont, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(btn_cont, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_clear_flag(btn_cont, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_clear_flag(btn_cont, LV_OBJ_FLAG_CLICKABLE);

  /* Start button */
  recorder_start_btn = lv_button_create(btn_cont);
  lv_obj_set_size(recorder_start_btn, 70, 36);
  lv_obj_set_style_bg_color(recorder_start_btn, lv_color_hex(0x0088FF), 0);
  lv_obj_set_style_radius(recorder_start_btn, 8, 0);
  lv_obj_t * start_label = lv_label_create(recorder_start_btn);
  lv_label_set_text(start_label, LV_SYMBOL_PLAY);
  lv_obj_set_style_text_color(start_label, lv_color_hex(0xFFFFFF), 0);
  lv_obj_center(start_label);
  lv_obj_add_event_cb(recorder_start_btn, recorder_start_btn_cb, LV_EVENT_CLICKED, NULL);

  /* Stop button */
  recorder_stop_btn = lv_button_create(btn_cont);
  lv_obj_set_size(recorder_stop_btn, 70, 36);
  lv_obj_set_style_bg_color(recorder_stop_btn, lv_color_hex(0xFF4444), 0);
  lv_obj_set_style_radius(recorder_stop_btn, 8, 0);
  lv_obj_t * stop_label = lv_label_create(recorder_stop_btn);
  lv_label_set_text(stop_label, LV_SYMBOL_STOP);
  lv_obj_set_style_text_color(stop_label, lv_color_hex(0xFFFFFF), 0);
  lv_obj_center(stop_label);
  lv_obj_add_event_cb(recorder_stop_btn, recorder_stop_btn_cb, LV_EVENT_CLICKED, NULL);

  /* File count label */
  recorder_file_count_label = lv_label_create(parent);
  lv_label_set_text_fmt(recorder_file_count_label, i18n_get(STR_REC_TOTAL_FMT), 0);
  lv_obj_set_style_text_color(recorder_file_count_label, lv_color_hex(0xCCCCCC), 0);
  i18n_apply_font_small(recorder_file_count_label);
  lv_obj_align(recorder_file_count_label, LV_ALIGN_TOP_MID, 0, 115);

  /* File list：透明滚动容器（参考 menu_page scroll_cont 模式）
   * - 无背景色、无边框、无圆角
   * - 列表项按 menu_item 模式逐个添加（图标+文字换行）
   * - 左右各预留 50px，底部预留 100px（圆形屏适配） */
  recorder_file_list = lv_obj_create(parent);
  lv_obj_remove_style_all(recorder_file_list);
  lv_obj_set_size(recorder_file_list, LV_HOR_RES - 2 * REC_LIST_PAD_LR,
                  LV_VER_RES - 145 - REC_LIST_PAD_BOT);
  lv_obj_align(recorder_file_list, LV_ALIGN_TOP_LEFT, REC_LIST_PAD_LR, 145);
  lv_obj_set_style_bg_opa(recorder_file_list, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(recorder_file_list, 0, 0);
  lv_obj_set_style_radius(recorder_file_list, 0, 0);
  lv_obj_set_style_pad_all(recorder_file_list, 0, 0);
  lv_obj_set_scroll_dir(recorder_file_list, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(recorder_file_list, LV_SCROLLBAR_MODE_OFF);
  lv_obj_clear_flag(recorder_file_list, LV_OBJ_FLAG_SCROLL_CHAIN);
  lv_obj_clear_flag(recorder_file_list, LV_OBJ_FLAG_SCROLL_ELASTIC);
  lv_obj_clear_flag(recorder_file_list, LV_OBJ_FLAG_SCROLL_MOMENTUM);
  lv_obj_add_flag(recorder_file_list, LV_OBJ_FLAG_GESTURE_BUBBLE);

  /* Add gesture handler */
  lv_obj_add_event_cb(parent, recorder_gesture_cb, LV_EVENT_GESTURE, NULL);

  /* Create timer */
  recorder_timer = lv_timer_create(recorder_timer_cb, 1000, NULL);
  lv_timer_pause(recorder_timer);

  /* Initialize button states only. All blocking work (stop wakeup/voice +
   * open recorder + scan files) deferred to SCREEN_LOADED to avoid stalling
   * the screen-load animation. voice_assistant_stop() calls pthread_join
   * which can block for seconds when the cloud thread is waiting on network. */
  recorder_update_btn_states();

  lv_obj_add_event_cb(parent, recorder_screen_loaded_cb,
      LV_EVENT_SCREEN_LOADED, NULL);
  lv_obj_add_event_cb(parent, recorder_screen_unloaded_cb,
      LV_EVENT_SCREEN_UNLOADED, NULL);
}

void recorder_page_deinit(void)
{
#ifdef CONFIG_MEDIA
  if (recorder_state == RECORDER_STATE_RECORDING) {
    recorder_stop();
    recorder_state = RECORDER_STATE_IDLE;
  }
#endif
  recorder_close();

  if (recorder_timer) {
    lv_timer_del(recorder_timer);
    recorder_timer = NULL;
  }

  if (recorder_scan_timer) {
    lv_timer_del(recorder_scan_timer);
    recorder_scan_timer = NULL;
  }

  /* Close delete confirmation dialog if open */
  if (recorder_delete_dialog) {
    lv_obj_del(recorder_delete_dialog);
    recorder_delete_dialog = NULL;
  }
  recorder_delete_pending_idx = -1;

  /* Cancel pending long-press timer */
  recorder_cancel_long_press_timer();

  recorder_file_list = NULL;
  recorder_file_count_label = NULL;
  recorder_time_label = NULL;
  recorder_status_dot = NULL;
  recorder_start_btn = NULL;
  recorder_stop_btn = NULL;
  recorder_time = 0;
  recorder_state = RECORDER_STATE_IDLE;
  recorder_file_count = 0;

  if (recorder_screen) {
    lv_obj_del(recorder_screen);
    recorder_screen = NULL;
  }
}

static void recorder_deinit_async_cb(void * arg)
{
  (void)arg;
  recorder_page_deinit();
}

bool recorder_is_recording(void)
{
  return recorder_state == RECORDER_STATE_RECORDING ||
         recorder_state == RECORDER_STATE_PAUSED;
}

static void recorder_screen_unloaded_cb(lv_event_t * e)
{
  (void)e;
  lv_indev_reset(NULL, NULL);
  lvgl_dispatch_async(recorder_deinit_async_cb, NULL);
  /* 新需求：VAD 由 ai_page 生命周期控制，退出 recorder_page 不启动 VAD/云端 */
}

/* 切屏动画完成后异步执行所有阻塞操作：
 * - wakeup_detector_stop() / voice_assistant_stop()（含 pthread_join，可能阻塞数秒）
 * - recorder_open()（open Capture）
 * - recorder_scan_files()（opendir + readdir）
 *
 * 用 lvgl_dispatch_async 推迟到下一个 LVGL tick 执行，SCREEN_LOADED 事件本身
 * 立即返回不阻塞后续事件分发；切屏动画已在动画期间完成，界面已显示。 */
static void recorder_loaded_async_cb(void * arg)
{
  (void)arg;
  wakeup_detector_stop();
  voice_assistant_stop();
  recorder_open();
  recorder_scan_files();
  recorder_update_btn_states();
}

static void recorder_screen_loaded_cb(lv_event_t * e)
{
  (void)e;
  lvgl_dispatch_async(recorder_loaded_async_cb, NULL);
}
