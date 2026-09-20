/****************************************************************************
 * apps/examples/lvgldemo/player_page.c
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
#include <lvgl/lvgl.h>

#include "player_page.h"
#include "recorder_page.h"
#include "lvgldemo_common.h"
#include "lvgl_dispatch.h"
#include "i18n.h"

#ifdef CONFIG_MEDIA
#include <media_player.h>
#include <media_utils.h>
#include <media_policy.h>
#endif

/****************************************************************************
 * Player Page Data
 ****************************************************************************/

#define PLAYER_MAX_FILES  RECORDER_MAX_FILES
#define PLAYER_PATH_PREFIX RECORDER_PATH_PREFIX

/* 列表布局边距：左右各 50px，底部 100px（圆形屏适配，与 recorder_page 一致） */
#define PLAYER_ITEM_HEIGHT    50
#define PLAYER_ITEM_GAP       8
#define PLAYER_ITEM_START_Y   5
#define PLAYER_LIST_PAD_LR    50
#define PLAYER_LIST_PAD_BOT   100

typedef enum {
  PLAYER_STATE_IDLE,
  PLAYER_STATE_PLAYING,
  PLAYER_STATE_PAUSED
} player_state_t;

static lv_obj_t * player_title_label = NULL;
static lv_obj_t * player_time_cur_label = NULL;
static lv_obj_t * player_time_total_label = NULL;
static lv_obj_t * player_bar = NULL;
static lv_obj_t * player_play_btn = NULL;
static lv_obj_t * player_prev_btn = NULL;
static lv_obj_t * player_next_btn = NULL;
static lv_obj_t * player_file_list = NULL;
static lv_obj_t * player_file_count_label = NULL;

static player_state_t player_state = PLAYER_STATE_IDLE;
static lv_timer_t * player_timer = NULL;

/* Duration of the current file (ms), obtained from WAV header.
 * We cannot use media_player_get_duration() because the SMF
 * media server does not implement the "get_duration" command —
 * it falls through to an else branch, returns an empty response,
 * and media_proxy_once() crashes with a NULL pointer dereference
 * when it tries to strlcpy() the missing response string. */
static unsigned int player_current_duration = 0;

/* Playlist: scanned from /emmc/audio/ */
static char player_files[PLAYER_MAX_FILES][64];
static int player_file_count = 0;
static int player_total_count = 0;
static int player_current_idx = -1;

#ifdef CONFIG_MEDIA
static void * player_handle = NULL;

/****************************************************************************
 * Player Wrapper Functions (media_player API)
 ****************************************************************************/

/* Forward declarations */
static void player_play_file(int idx);
static void player_format_time(unsigned int ms, char * buf, size_t sz);
static void player_file_click_cb(lv_event_t * e);

/* Helper: dispatch a function to LVGL main thread via lv_async_call.
 * This is required because player_on_event runs on the media server
 * thread, and LVGL is NOT thread-safe — all UI operations must be
 * performed on the LVGL main thread. See BES Watch reference:
 *   audio_player_api.c audio_player_event_callback() */
static void player_dispatch_ui(lv_async_cb_t cb, void * data)
{
  lv_async_call(cb, data);
}

/* --- UI update helpers (run on LVGL main thread via lv_async_call) --- */

static void player_ui_set_play_icon(void * data)
{
  LV_UNUSED(data);
  if (player_play_btn)
    {
      lv_obj_t * lbl = lv_obj_get_child(player_play_btn, 0);
      if (lbl) lv_label_set_text(lbl, LV_SYMBOL_PLAY);
    }
}

static void player_ui_set_pause_icon(void * data)
{
  LV_UNUSED(data);
  if (player_play_btn)
    {
      lv_obj_t * lbl = lv_obj_get_child(player_play_btn, 0);
      if (lbl) lv_label_set_text(lbl, LV_SYMBOL_PAUSE);
    }
}

static void player_ui_on_stopped(void * data)
{
  LV_UNUSED(data);
  player_ui_set_play_icon(NULL);
  if (player_bar)
    lv_bar_set_value(player_bar, 0, LV_ANIM_OFF);
  if (player_time_cur_label)
    lv_label_set_text(player_time_cur_label, "0:00");
  if (player_timer)
    lv_timer_pause(player_timer);
}

static void player_ui_on_started(void * data)
{
  LV_UNUSED(data);
  player_ui_set_pause_icon(NULL);
  if (player_timer)
    lv_timer_resume(player_timer);
}

static void player_ui_on_paused(void * data)
{
  LV_UNUSED(data);
  player_ui_set_play_icon(NULL);
  if (player_timer)
    lv_timer_pause(player_timer);
}

/* Data for auto-play-next dispatch */
typedef struct
{
  int next_idx;
} player_next_data_t;

static void player_ui_on_completed(void * data)
{
  player_next_data_t * nd = (player_next_data_t *)data;
  if (player_timer)
    lv_timer_pause(player_timer);

  if (nd->next_idx >= 0)
    {
      player_play_file(nd->next_idx);
    }
  else
    {
      player_state = PLAYER_STATE_IDLE;
      player_ui_set_play_icon(NULL);
      if (player_bar)
        lv_bar_set_value(player_bar, 0, LV_ANIM_OFF);
      if (player_time_cur_label)
        lv_label_set_text(player_time_cur_label, "0:00");
    }

  free(nd);
}

/* Player event callback — runs on media server thread!
 * MUST NOT touch LVGL objects directly; use lv_async_call(). */
static void player_on_event(void * cookie, int event, int result,
                            const char * extra)
{
  LV_UNUSED(cookie);
  LV_UNUSED(extra);

  LV_LOG_USER("player event: %s result=%d",
              media_event_get_name(event), result);

  if (result < 0)
    {
      LV_LOG_WARN("player event error: %s result=%d",
                  media_event_get_name(event), result);
      return;
    }

  switch (event)
    {
      case MEDIA_EVENT_PREPARED:
        media_player_start(player_handle);
        break;

      case MEDIA_EVENT_STARTED:
        player_state = PLAYER_STATE_PLAYING;
        media_player_set_volume(player_handle, 1.0f);
        player_dispatch_ui(player_ui_on_started, NULL);
        break;

      case MEDIA_EVENT_PAUSED:
        player_state = PLAYER_STATE_PAUSED;
        player_dispatch_ui(player_ui_on_paused, NULL);
        break;

      case MEDIA_EVENT_STOPPED:
        player_state = PLAYER_STATE_IDLE;
        player_dispatch_ui(player_ui_on_stopped, NULL);
        /* Re-open recorder after playback stops — it was closed in
         * player_play_file() to release shared SMF audio resources. */
        recorder_open();
        break;

      case MEDIA_EVENT_COMPLETED:
        {
          /* Single file playback: stop after completion, no auto-next */
          player_state = PLAYER_STATE_IDLE;
          player_dispatch_ui(player_ui_on_stopped, NULL);
          recorder_open();
        }
        break;

      case MEDIA_EVENT_SEEKED:
        break;

      default:
        break;
    }
}

int player_open(void)
{
  if (player_handle)
    {
      media_player_stop(player_handle);
      media_player_close(player_handle, 0);
      player_handle = NULL;
      usleep(100000); /* 100ms — let framework cleanup */
    }

  player_handle = media_player_open(MEDIA_STREAM_MUSIC);
  if (player_handle == NULL)
    {
      LV_LOG_ERROR("media_player_open error");
      return -1;
    }

  media_player_set_event_callback(player_handle, NULL, player_on_event);

  /* ---- Ensure MuteMode is off ----
   * If MuteMode is "on", PFW forces all volume filters to 0 regardless
   * of the stream volume index. Clear it first. */
  int mute = 0;
  media_policy_get_mute_mode(&mute);
  if (mute)
    {
      media_policy_set_mute_mode(0);
      LV_LOG_USER("MuteMode was on, cleared");
    }

  /* ---- Set ALL volume policy criteria to maximum ----
   *
   * The audio pipeline has MULTIPLE volume control layers (visible in
   * graph.conf), and each one can attenuate the output independently:
   *
   *   1) FFmpeg filter "volume@VolMedia0=precision=fixed"
   *      - Controlled by PFW criterion "MusicVolume" via MediaVolumeDomain
   *      - PFW formula: VolMedia,volume,10^(-4*(10-MusicVolume)/20)
   *      - MusicVolume=10 → volume=1.0 (0dB)
   *      - MusicVolume=5  → volume=0.1 (-20dB) — huge attenuation!
   *
   *   2) SMF stream volume (unnamed "volume=precision=fixed" filter)
   *      - Controlled by PFW criterion "MusicVolume" (range [0,10])
   *      - Read by smf_media_get_volume() at player start time
   *      - Initial value passed as vol=#%u in demuxer parameter
   *      - vol = (MusicVolume / 10) * SMF_VOLUME_MAX(32768)
   *
   *   3) Per-handle volume ratio (0.0~1.0)
   *      - Set via media_player_set_volume() AFTER the player starts
   *      - Calling before start has NO EFFECT (smf_media_id == 0)
   *
   * We must set BOTH MediaVolume AND MusicVolume to maximum, AND wait
   * for the PFW to apply before calling prepare/start, otherwise
   * smf_media_get_volume() may read a stale (low) value.
   */

  int vol_min = 0, vol_max = 10;
  media_policy_get_range(MEDIA_STREAM_MEDIA MEDIA_POLICY_VOLUME,
                         &vol_min, &vol_max);
  media_policy_set_stream_volume(MEDIA_STREAM_MEDIA, vol_max);
  LV_LOG_USER("MediaVolume set to max %d (range %d-%d)",
              vol_max, vol_min, vol_max);

  vol_min = 0; vol_max = 10;
  media_policy_get_range(MEDIA_STREAM_MUSIC MEDIA_POLICY_VOLUME,
                         &vol_min, &vol_max);
  media_policy_set_stream_volume(MEDIA_STREAM_MUSIC, vol_max);
  LV_LOG_USER("MusicVolume set to max %d (range %d-%d)",
              vol_max, vol_min, vol_max);

  /* Wait for PFW to propagate the volume change to FFmpeg filters.
   * Without this delay, smf_media_get_volume() in the SMF layer may
   * still read the old (low) MusicVolume value when the player starts,
   * causing the initial vol=#%u parameter to be too small. */
  usleep(200000);

  /* Verify the volume was actually applied */
  int cur_vol = 0;
  media_policy_get_stream_volume(MEDIA_STREAM_MUSIC, &cur_vol);
  LV_LOG_USER("MusicVolume after set: %d (expected %d)", cur_vol, vol_max);

  /* Note: media_player_set_volume(handle, 1.0f) is NOT called here because
   * the player hasn't started yet (smf_media_id == 0). The per-handle
   * volume ratio is set in the MEDIA_EVENT_STARTED callback instead. */

  return 0;
}

static int player_close(void)
{
  if (player_handle == NULL) return -1;
  media_player_stop(player_handle);
  usleep(50000); /* 50ms — let AP process the stop */
  int ret = media_player_close(player_handle, 0);
  player_handle = NULL;
  player_state = PLAYER_STATE_IDLE;
  return ret;
}

static int player_prepare(const char * url)
{
  if (player_handle == NULL) return -1;
  int ret = media_player_prepare(player_handle, url, NULL);
  if (ret != 0)
    {
      LV_LOG_ERROR("media_player_prepare error %d", ret);
    }
  return ret;
}

static int player_start(void)
{
  if (player_handle == NULL) return -1;
  int ret = media_player_start(player_handle);
  if (ret != 0)
    {
      LV_LOG_ERROR("media_player_start error %d", ret);
    }
  return ret;
}

static int player_pause(void)
{
  if (player_handle == NULL) return -1;
  int ret = media_player_pause(player_handle);
  if (ret != 0)
    {
      LV_LOG_ERROR("media_player_pause error %d", ret);
    }
  return ret;
}

static int player_stop(void)
{
  if (player_handle == NULL) return -1;
  int ret = media_player_stop(player_handle);
  if (ret != 0)
    {
      LV_LOG_ERROR("media_player_stop error %d", ret);
    }
  return ret;
}

/* Parse WAV file header to get duration in milliseconds.
 * WAV layout: RIFF header (12 bytes) + fmt chunk (24+ bytes) + data chunk.
 * Duration = data_size / (sample_rate * channels * bits_per_sample / 8)
 * Returns 0 if the file is not a valid WAV or cannot be read. */
static unsigned int player_get_wav_duration(const char * filepath)
{
  FILE * fp = fopen(filepath, "rb");
  if (fp == NULL)
    return 0;

  unsigned int duration = 0;
  uint8_t hdr[44];

  if (fread(hdr, 1, sizeof(hdr), fp) < 44)
    goto out;

  /* Check "RIFF" marker */
  if (hdr[0] != 'R' || hdr[1] != 'I' || hdr[2] != 'F' || hdr[3] != 'F')
    goto out;

  /* Check "WAVE" marker */
  if (hdr[8] != 'W' || hdr[9] != 'A' || hdr[10] != 'V' || hdr[11] != 'E')
    goto out;

  /* Find "data" sub-chunk — scan from byte 12 onwards */
  uint32_t data_size = 0;
  uint32_t sample_rate = 0;
  uint16_t channels = 0;
  uint16_t bits_per_sample = 0;
  int found_fmt = 0;
  int found_data = 0;
  uint32_t offset = 12;
  uint8_t chunk_hdr[8];

  while (offset + 8 <= 512 && !found_data)
    {
      fseek(fp, offset, SEEK_SET);
      if (fread(chunk_hdr, 1, 8, fp) < 8)
        break;

      uint32_t chunk_size = chunk_hdr[4] |
                            (chunk_hdr[5] << 8) |
                            (chunk_hdr[6] << 16) |
                            (chunk_hdr[7] << 24);

      if (chunk_hdr[0] == 'f' && chunk_hdr[1] == 'm' &&
          chunk_hdr[2] == 't' && chunk_hdr[3] == ' ')
        {
          /* Read fmt data: channels(2) + sample_rate(4) + byte_rate(4) + block_align(2) + bits_per_sample(2) */
          uint8_t fmt_data[16];
          if (fread(fmt_data, 1, 16, fp) >= 16)
            {
              channels = fmt_data[0] | (fmt_data[1] << 8);
              sample_rate = fmt_data[2] | (fmt_data[3] << 8) |
                            (fmt_data[4] << 16) | (fmt_data[5] << 24);
              bits_per_sample = fmt_data[14] | (fmt_data[15] << 8);
              found_fmt = 1;
            }
        }
      else if (chunk_hdr[0] == 'd' && chunk_hdr[1] == 'a' &&
               chunk_hdr[2] == 't' && chunk_hdr[3] == 'a')
        {
          data_size = chunk_size;
          found_data = 1;
        }

      /* Advance to next chunk (word-aligned) */
      offset += 8 + ((chunk_size + 1) & ~1u);
    }

  if (found_fmt && found_data && sample_rate > 0 && channels > 0 &&
      bits_per_sample > 0)
    {
      uint32_t byte_rate = sample_rate * channels * bits_per_sample / 8;
      if (byte_rate > 0)
        duration = (unsigned int)((uint64_t)data_size * 1000 / byte_rate);
    }

out:
  fclose(fp);
  return duration;
}

/* Play a file from the playlist by index */
static void player_play_file(int idx)
{
  if (idx < 0 || idx >= player_file_count) return;

  player_current_idx = idx;

  /* Close recorder before playing — the SMF shares audio pipeline
   * resources between recorder and player. If the recorder handle is
   * open (even if stopped), smf_stream_check() can fail when the
   * player tries to start, returning "audio player start failed".
   * Closing the recorder releases those shared resources. */
  recorder_close();

  /* Re-open player handle to ensure a clean SMF state. */
  player_open();

  /* Update title label */
  if (player_title_label)
    {
      lv_label_set_text(player_title_label, player_files[idx]);
    }

  /* Build full path and get duration from WAV header */
  char fullpath[128];
  snprintf(fullpath, sizeof(fullpath), "%s%s",
           PLAYER_PATH_PREFIX, player_files[idx]);

  player_current_duration = player_get_wav_duration(fullpath);

  /* Update total time label */
  if (player_time_total_label)
    {
      char buf[16];
      player_format_time(player_current_duration, buf, sizeof(buf));
      lv_label_set_text(player_time_total_label, buf);
    }

  /* Prepare and start (start is triggered from event callback
   * when MEDIA_EVENT_PREPARED fires) */
  int ret = player_prepare(fullpath);
  if (ret != 0)
    {
      LV_LOG_ERROR("player_prepare failed for %s: %d", fullpath, ret);
      if (player_title_label)
        lv_label_set_text(player_title_label, i18n_get(STR_PLAY_ERROR));
      /* Re-open recorder since playback failed and no STOPPED/COMPLETED
       * event will fire to re-open it. */
      recorder_open();
    }

  /* Reset progress bar */
  if (player_bar)
    lv_bar_set_value(player_bar, 0, LV_ANIM_OFF);
  if (player_time_cur_label)
    lv_label_set_text(player_time_cur_label, "0:00");
}

#else /* !CONFIG_MEDIA */

int player_open(void) { return 0; }

#endif /* CONFIG_MEDIA */

/****************************************************************************
 * Player file scanning (always available)
 ****************************************************************************/

/* Scan /emmc/audio/ for audio files to build playlist */
static int player_name_cmp_asc(const void * a, const void * b)
{
  return strcmp((const char *)a, (const char *)b);
}

/* 创建文件列表项（参考 recorder_page create_recorder_file_item 模式）
 * - 透明背景容器，图标+文字换行显示
 * - 文件名超长时换行显示，不滚动播报
 * - 圆形屏适配：宽度缩短50px+右移50px */
static void create_player_file_item(lv_obj_t *parent, int index,
                                      const char *filename, int idx)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, LV_HOR_RES - 2 * PLAYER_LIST_PAD_LR, PLAYER_ITEM_HEIGHT);
    lv_obj_align(btn, LV_ALIGN_TOP_LEFT, 0,
                 PLAYER_ITEM_START_Y + index * (PLAYER_ITEM_HEIGHT + PLAYER_ITEM_GAP));

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
    lv_obj_set_width(name_label, LV_HOR_RES - 2 * PLAYER_LIST_PAD_LR - 30 - 24);

    lv_obj_set_user_data(btn, (void *)(intptr_t)idx);
    lv_obj_add_event_cb(btn, player_file_click_cb, LV_EVENT_CLICKED, NULL);
}

void player_scan_files(void)
{
  player_file_count = 0;
  player_current_idx = -1;
  player_total_count = 0;

  DIR * dir = opendir(PLAYER_PATH_PREFIX);
  if (dir == NULL)
    {
      LV_LOG_USER("player dir not found: %s", PLAYER_PATH_PREFIX);
      return;
    }

  int total = 0;
  struct dirent * entry;
  while ((entry = readdir(dir)) != NULL)
    {
      const char * name = entry->d_name;
      size_t len = strlen(name);
      if (len > 4 && len < 64 &&
          (strcmp(name + len - 4, ".wav") == 0 ||
           strcmp(name + len - 4, ".mp3") == 0 ||
           strcmp(name + len - 4, ".pcm") == 0))
        {
          total++;
        }
    }
  rewinddir(dir);

  player_total_count = total;

  if (total == 0)
    {
      closedir(dir);
      LV_LOG_USER("player scanned 0 files");
      return;
    }

  char (*names)[64] = malloc(total * 64);
  if (names == NULL)
    {
      closedir(dir);
      LV_LOG_USER("player malloc failed");
      return;
    }

  int count = 0;
  while ((entry = readdir(dir)) != NULL && count < total)
    {
      const char * name = entry->d_name;
      size_t len = strlen(name);
      if (len > 4 && len < 64 &&
          (strcmp(name + len - 4, ".wav") == 0 ||
           strcmp(name + len - 4, ".mp3") == 0 ||
           strcmp(name + len - 4, ".pcm") == 0))
        {
          strncpy(names[count], name, 63);
          names[count][63] = '\0';
          count++;
        }
    }
  closedir(dir);

  qsort(names, count, sizeof(names[0]), player_name_cmp_asc);

  int start = count > PLAYER_MAX_FILES ? count - PLAYER_MAX_FILES : 0;
  player_file_count = 0;
  for (int i = start; i < count; i++)
    {
      strncpy(player_files[player_file_count], names[i],
              sizeof(player_files[player_file_count]) - 1);
      player_files[player_file_count][sizeof(player_files[player_file_count]) - 1] = '\0';
      player_file_count++;
    }

  free(names);

  LV_LOG_USER("player scanned %d/%d files", player_file_count, player_total_count);
}

/* Format milliseconds to m:ss */
static void player_format_time(unsigned int ms, char * buf, size_t sz)
{
  unsigned int sec = ms / 1000;
  unsigned int min = sec / 60;
  sec = sec % 60;
  snprintf(buf, sz, "%u:%02u", min, sec);
}

/****************************************************************************
 * Private Functions (UI callbacks)
 ****************************************************************************/

/* Player file list item click callback */
static void player_file_click_cb(lv_event_t * e)
{
  lv_obj_t * btn = lv_event_get_target(e);
  int idx = (int)(intptr_t)lv_obj_get_user_data(btn);
  if (idx < 0 || idx >= player_file_count) return;

#ifdef CONFIG_MEDIA
  player_play_file(idx);
#endif
}

/* Player back button — return to recorder screen */
static void player_back_btn_cb(lv_event_t * e)
{
  LV_UNUSED(e);
  if (recorder_screen == NULL)
    {
      recorder_screen = lv_obj_create(NULL);
      create_recorder_page(recorder_screen);
    }
  lv_scr_load_anim(recorder_screen, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 300, 0, false);
}

/* Play/Pause button callback */
static void player_play_btn_cb(lv_event_t * e)
{
  LV_UNUSED(e);
#ifdef CONFIG_MEDIA
  if (player_current_idx < 0)
    {
      /* No file selected yet — play first file if available */
      if (player_file_count > 0)
        player_play_file(0);
      return;
    }

  if (player_state == PLAYER_STATE_PLAYING)
    {
      player_pause();
    }
  else if (player_state == PLAYER_STATE_PAUSED)
    {
      player_start();
    }
  else
    {
      /* IDLE — replay current file */
      player_play_file(player_current_idx);
    }
#endif
}

/* Previous track button callback */
static void player_prev_btn_cb(lv_event_t * e)
{
  LV_UNUSED(e);
  if (player_file_count == 0) return;

  if (player_current_idx > 0)
    player_current_idx--;
  else
    player_current_idx = player_file_count - 1; /* wrap around */

#ifdef CONFIG_MEDIA
  player_play_file(player_current_idx);
#endif
}

/* Next track button callback */
static void player_next_btn_cb(lv_event_t * e)
{
  LV_UNUSED(e);
  if (player_file_count == 0) return;

  if (player_current_idx + 1 < player_file_count)
    player_current_idx++;
  else
    player_current_idx = 0; /* wrap around */

#ifdef CONFIG_MEDIA
  player_play_file(player_current_idx);
#endif
}

static void player_deinit_async_cb(void * arg);

static void player_screen_unloaded_cb(lv_event_t * e)
{
  LV_UNUSED(e);
  LV_LOG_USER("player screen unloaded, deinitializing...");
  lv_indev_reset(NULL, NULL);
  lvgl_dispatch_async(player_deinit_async_cb, NULL);
}

/* Gesture event handler for player screen */
static void player_gesture_cb(lv_event_t * e)
{
  lv_indev_t *indev = lv_indev_active();
  lv_dir_t dir = lv_indev_get_gesture_dir(indev);
  if (dir == LV_DIR_RIGHT) {
    /* 右滑返回功能菜单页（与 ai_page/camera_page/meeting_page 一致） */
    /* 等待手指释放后再处理，防止动画期间手指抬起的 CLICKED 事件
     * 误触发菜单页按钮。比 lv_indev_reset 副作用小，不会干扰屏幕加载动画。 */
    lv_indev_wait_release(indev);
    if (menu_screen == NULL)
      {
        menu_screen = lv_obj_create(NULL);
        create_menu_page(menu_screen);
      }
    lv_scr_load_anim(menu_screen, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 300, 0, false);
  }
}

#ifdef CONFIG_MEDIA
/* Timer callback: update progress bar and time labels */
static void player_timer_cb(lv_timer_t * timer)
{
  LV_UNUSED(timer);
  if (player_state != PLAYER_STATE_PLAYING || !player_handle)
    return;

  unsigned int pos = 0;
  unsigned int dur = player_current_duration;
  media_player_get_position(player_handle, &pos);

  /* Update progress bar */
  if (player_bar && dur > 0)
    {
      lv_bar_set_value(player_bar, (int32_t)((uint64_t)pos * 100 / dur),
                        LV_ANIM_OFF);
    }

  /* Update time labels */
  char buf[16];
  if (player_time_cur_label)
    {
      player_format_time(pos, buf, sizeof(buf));
      lv_label_set_text(player_time_cur_label, buf);
    }
  /* Total time label is already set when player_play_file() is called
   * and does not need to be updated every tick. */
}
#endif

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/* Re-scan files and rebuild the playlist UI widget.
 * Called on init and every time the player screen is swiped to.
 * We delete the old lv_list and create a new one because
 * lv_obj_clean() on an lv_list destroys its internal scroll
 * container, after which lv_list_add_button() creates items
 * that don't receive click events. */
void player_refresh_playlist(void * unused)
{
  LV_UNUSED(unused);
  lv_obj_t * parent = player_screen;
  if (parent == NULL)
    return;

  /* Delete old list widget if it exists */
  if (player_file_list != NULL)
    {
      lv_obj_del(player_file_list);
      player_file_list = NULL;
    }

  /* 创建透明滚动容器（参考 recorder_page：删除白色背景+边框+圆角）
   * 左右各预留 50px，底部预留 100px（圆形屏适配）
   * 顶部从 y=195 开始（控制按钮 y=120+40=160、文件计数 y=170 下方） */
  player_file_list = lv_obj_create(parent);
  lv_obj_remove_style_all(player_file_list);
  lv_obj_set_size(player_file_list, LV_HOR_RES - 2 * PLAYER_LIST_PAD_LR,
                  LV_VER_RES - 195 - PLAYER_LIST_PAD_BOT);
  lv_obj_align(player_file_list, LV_ALIGN_TOP_LEFT, PLAYER_LIST_PAD_LR, 205);
  lv_obj_set_style_bg_opa(player_file_list, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(player_file_list, 0, 0);
  lv_obj_set_style_pad_left(player_file_list, 0, 0);
  lv_obj_set_style_pad_right(player_file_list, 0, 0);
  lv_obj_set_style_pad_top(player_file_list, 0, 0);
  lv_obj_set_style_pad_bottom(player_file_list, 0, 0);
  lv_obj_set_scroll_dir(player_file_list, LV_DIR_VER);
  lv_obj_clear_flag(player_file_list, LV_OBJ_FLAG_SCROLL_CHAIN);
  lv_obj_add_flag(player_file_list, LV_OBJ_FLAG_GESTURE_BUBBLE);
  lv_obj_set_scrollbar_mode(player_file_list, LV_SCROLLBAR_MODE_OFF);

  /* Re-scan directory */
  player_scan_files();

  /* Update file count label */
  if (player_file_count_label)
    {
      lv_label_set_text_fmt(player_file_count_label, "Total: %d",
                            player_total_count);
    }

  /* Populate list */
  if (player_file_count == 0)
    {
      lv_obj_t * empty = lv_label_create(player_file_list);
      lv_obj_remove_style_all(empty);
      lv_label_set_text(empty, i18n_get(STR_PLAY_EMPTY));
      lv_obj_set_style_text_color(empty, lv_color_hex(0xCCCCCC), 0);
      i18n_apply_font_small(empty);
      lv_obj_align(empty, LV_ALIGN_TOP_LEFT, 0, PLAYER_ITEM_START_Y);
    }
  else
    {
      for (int i = 0; i < player_file_count; i++)
        {
          create_player_file_item(player_file_list, i, player_files[i], i);
        }
    }

  LV_LOG_USER("playlist refreshed, %d files", player_file_count);
}

/* Create Player page */
void create_player_page(lv_obj_t * parent)
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

  /* ── Back button (透明标题栏，无蓝色背景) ── */
  lv_obj_t * back_btn = lv_button_create(parent);
  lv_obj_set_size(back_btn, 40, 40);
  lv_obj_align(back_btn, LV_ALIGN_TOP_LEFT, 8, 0);
  lv_obj_set_style_bg_opa(back_btn, LV_OPA_TRANSP, 0);
  lv_obj_set_style_shadow_opa(back_btn, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(back_btn, 0, 0);
  lv_obj_t * back_label = lv_label_create(back_btn);
  lv_label_set_text(back_label, LV_SYMBOL_LEFT);
  lv_obj_set_style_text_color(back_label, lv_color_hex(0xFFFFFF), 0);
  i18n_apply_font_small(back_label);
  lv_obj_center(back_label);
  lv_obj_add_event_cb(back_btn, player_back_btn_cb, LV_EVENT_CLICKED, NULL);

  /* ── Title "Player" ── */
  lv_obj_t * title = lv_label_create(parent);
  lv_label_set_text(title, i18n_get(STR_PLAY_TITLE));
  lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
  i18n_apply_font(title);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

  /* ── Song title (file name) — 换行显示，不滚动播报 ── */
  player_title_label = lv_label_create(parent);
  lv_label_set_text(player_title_label, "");
  lv_obj_set_style_text_color(player_title_label, lv_color_hex(0xFFFFFF), 0);
  i18n_apply_font_small(player_title_label);
  /* 单行显示，超长时省略号截断（避免2行遮盖进度条）
   * 宽度与文件列表项文件名一致（减去图标30px+右侧padding 24px），避免圆形屏边缘遮挡 */
  lv_label_set_long_mode(player_title_label, LV_LABEL_LONG_DOT);
  lv_obj_set_style_text_align(player_title_label, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_width(player_title_label, LV_HOR_RES - 2 * PLAYER_LIST_PAD_LR - 30 - 24);
  lv_obj_align(player_title_label, LV_ALIGN_TOP_MID, 0, 46);

  /* ── Progress bar ── */
  player_bar = lv_bar_create(parent);
  lv_obj_set_size(player_bar, LV_HOR_RES - 2 * PLAYER_LIST_PAD_LR, 8);
  lv_obj_align(player_bar, LV_ALIGN_TOP_MID, 0, 90);
  lv_bar_set_range(player_bar, 0, 100);
  lv_bar_set_value(player_bar, 0, LV_ANIM_OFF);
  lv_obj_set_style_bg_color(player_bar, lv_color_hex(0x333333), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(player_bar, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_bg_color(player_bar, lv_color_hex(0x0088FF), LV_PART_INDICATOR);
  lv_obj_set_style_bg_opa(player_bar, LV_OPA_COVER, LV_PART_INDICATOR);

  /* ── Time labels ── */
  player_time_cur_label = lv_label_create(parent);
  lv_label_set_text(player_time_cur_label, "0:00");
  lv_obj_set_style_text_color(player_time_cur_label, lv_color_hex(0xAAAAAA), 0);
  i18n_apply_font_small(player_time_cur_label);
  lv_obj_align_to(player_time_cur_label, player_bar, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 4);

  player_time_total_label = lv_label_create(parent);
  lv_label_set_text(player_time_total_label, "0:00");
  lv_obj_set_style_text_color(player_time_total_label, lv_color_hex(0xAAAAAA), 0);
  i18n_apply_font_small(player_time_total_label);
  lv_obj_align_to(player_time_total_label, player_bar, LV_ALIGN_OUT_BOTTOM_RIGHT, 0, 4);

  /* ── Control buttons row ── */
  lv_obj_t * ctrl_cont = lv_obj_create(parent);
  lv_obj_remove_style_all(ctrl_cont);
  lv_obj_set_size(ctrl_cont, LV_HOR_RES - 2 * PLAYER_LIST_PAD_LR, 40);
  lv_obj_align(ctrl_cont, LV_ALIGN_TOP_MID, 0, 130);
  lv_obj_set_flex_flow(ctrl_cont, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(ctrl_cont, LV_FLEX_ALIGN_SPACE_EVENLY,
                         LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_clear_flag(ctrl_cont, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_clear_flag(ctrl_cont, LV_OBJ_FLAG_CLICKABLE);

  /* Previous button */
  player_prev_btn = lv_button_create(ctrl_cont);
  lv_obj_set_size(player_prev_btn, 50, 40);
  lv_obj_set_style_bg_color(player_prev_btn, lv_color_hex(0x444444), 0);
  lv_obj_set_style_radius(player_prev_btn, 8, 0);
  lv_obj_t * prev_label = lv_label_create(player_prev_btn);
  lv_label_set_text(prev_label, LV_SYMBOL_PREV);
  lv_obj_set_style_text_color(prev_label, lv_color_hex(0xFFFFFF), 0);
  lv_obj_center(prev_label);
  lv_obj_add_event_cb(player_prev_btn, player_prev_btn_cb, LV_EVENT_CLICKED, NULL);

  /* Play/Pause button */
  player_play_btn = lv_button_create(ctrl_cont);
  lv_obj_set_size(player_play_btn, 60, 40);
  lv_obj_set_style_bg_color(player_play_btn, lv_color_hex(0x0088FF), 0);
  lv_obj_set_style_radius(player_play_btn, 8, 0);
  lv_obj_t * play_label = lv_label_create(player_play_btn);
  lv_label_set_text(play_label, LV_SYMBOL_PLAY);
  lv_obj_set_style_text_color(play_label, lv_color_hex(0xFFFFFF), 0);
  lv_obj_center(play_label);
  lv_obj_add_event_cb(player_play_btn, player_play_btn_cb, LV_EVENT_CLICKED, NULL);

  /* Next button */
  player_next_btn = lv_button_create(ctrl_cont);
  lv_obj_set_size(player_next_btn, 50, 40);
  lv_obj_set_style_bg_color(player_next_btn, lv_color_hex(0x444444), 0);
  lv_obj_set_style_radius(player_next_btn, 8, 0);
  lv_obj_t * next_label = lv_label_create(player_next_btn);
  lv_label_set_text(next_label, LV_SYMBOL_NEXT);
  lv_obj_set_style_text_color(next_label, lv_color_hex(0xFFFFFF), 0);
  lv_obj_center(next_label);
  lv_obj_add_event_cb(player_next_btn, player_next_btn_cb, LV_EVENT_CLICKED, NULL);

  /* ── File count label ── */
  player_file_count_label = lv_label_create(parent);
  lv_label_set_text_fmt(player_file_count_label, i18n_get(STR_PLAY_TOTAL_FMT), 0);
  lv_obj_set_style_text_color(player_file_count_label, lv_color_hex(0xCCCCCC), 0);
  i18n_apply_font_small(player_file_count_label);
  lv_obj_align(player_file_count_label, LV_ALIGN_TOP_MID, 0, 170);

  /* Populate playlist (also creates the list widget) */
  player_refresh_playlist(NULL);

  /* Add gesture handler */
  lv_obj_add_event_cb(parent, player_gesture_cb, LV_EVENT_GESTURE, NULL);

  lv_obj_add_event_cb(parent, player_screen_unloaded_cb,
      LV_EVENT_SCREEN_UNLOADED, NULL);

#ifdef CONFIG_MEDIA
  /* Create progress update timer (paused by default) */
  player_timer = lv_timer_create(player_timer_cb, 500, NULL);
  lv_timer_pause(player_timer);

  /* Open player handle */
  player_open();
#endif
}

void player_page_deinit(void)
{
#ifdef CONFIG_MEDIA
  if (player_timer)
    {
      lv_timer_del(player_timer);
      player_timer = NULL;
    }

  player_close();
#endif

  player_title_label = NULL;
  player_time_cur_label = NULL;
  player_time_total_label = NULL;
  player_bar = NULL;
  player_play_btn = NULL;
  player_prev_btn = NULL;
  player_next_btn = NULL;
  player_file_list = NULL;
  player_file_count_label = NULL;

  player_state = PLAYER_STATE_IDLE;
  player_current_duration = 0;
  player_file_count = 0;
  player_total_count = 0;
  player_current_idx = -1;

  if (player_screen)
    {
      lv_obj_del(player_screen);
      player_screen = NULL;
    }

  LV_LOG_USER("player page deinit complete");
}

static void player_deinit_async_cb(void * arg)
{
  LV_UNUSED(arg);
  player_page_deinit();
}


