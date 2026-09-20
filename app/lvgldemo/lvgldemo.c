/****************************************************************************
 * apps/examples/lvgldemo/lvgldemo.c
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
#include <sys/boardctl.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <fcntl.h>
#include <pthread.h>
#include <syslog.h>
#include <lvgl/lvgl.h>
#ifdef CONFIG_LV_USE_NUTTX_LIBUV
#include <uv.h>
#endif

#include "lvgldemo_common.h"
#include "lvgl_dispatch.h"
#include "xiaoq_page.h"
#include "wifi_page.h"
#include "recorder_page.h"
#include "player_page.h"
#include "ai_page.h"
#include "camera_page.h"
#include "voice_assistant.h"
#include "wakeup_detector.h"
#include "call_page.h"
#include "calling_page.h"
#include "bt_call_handler.h"
#include "meeting_page.h"
#include "menu_page.h"
#include "settings_page.h"
#include "i18n.h"

#ifdef CONFIG_WIRELESS_WAPI
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <nuttx/net/netconfig.h>
#include <netutils/netlib.h>
#include <wireless/wapi.h>
#ifdef CONFIG_NETDOWN_NOTIFIER
#include <nuttx/net/netdev.h>
#include <nuttx/wqueue.h>
#endif

/* External function from wifi_page.c */
extern int wifi_get_if_flags(FAR const char *ifname);

/* Network card name */
#define WIFI_NETCARD "wlan0"
#endif

#define TIME_STAMP_PATH "/emmc/time_stamp"
#define TIME_SAVE_INTERVAL_SEC 10

/* 提示音路径改由 i18n_get_prompt() 按当前语言返回，见 i18n.c 中 s_prompts 表 */

static volatile bool time_save_running = false;
static pthread_t time_save_thread;

#ifdef CONFIG_WIRELESS_WAPI
static lv_timer_t * network_monitor_timer = NULL;
#endif
static bool last_network_ready = false;
static volatile bool s_disconnect_handler_active = false;
#ifdef CONFIG_NETDOWN_NOTIFIER
static int s_netdown_notifier_key = 0;
#endif

static void persist_time_load(void)
{
  FILE *fp = fopen(TIME_STAMP_PATH, "r");
  if (!fp)
    {
      LV_LOG_USER("no saved timestamp, time starts from epoch");
      return;
    }

  time_t saved = 0;
  if (fscanf(fp, "%ld", (long *)&saved) != 1 || saved <= 0)
    {
      LV_LOG_USER("invalid timestamp file");
      fclose(fp);
      return;
    }
  fclose(fp);

  time_t now;
  time(&now);

  if (now < saved)
    {
      struct timespec ts;
      ts.tv_sec  = saved;
      ts.tv_nsec = 0;
      clock_settime(CLOCK_REALTIME, &ts);
      LV_LOG_USER("restored time to %ld (was %ld)", (long)saved, (long)now);
    }
  else
    {
      LV_LOG_USER("system time %ld >= saved %ld, keep system time", (long)now, (long)saved);
    }
}

static void persist_time_save(void)
{
  time_t now;
  time(&now);

  FILE *fp = fopen(TIME_STAMP_PATH, "w");
  if (!fp)
    {
      LV_LOG_USER("failed to open %s for writing", TIME_STAMP_PATH);
      return;
    }
  fprintf(fp, "%ld\n", (long)now);
  fclose(fp);
  LV_LOG_USER("saved timestamp %ld", (long)now);
}

static void * time_save_thread_func(void *arg)
{
  LV_UNUSED(arg);

  while (time_save_running)
    {
      sleep(TIME_SAVE_INTERVAL_SEC);
      if (time_save_running)
        {
          persist_time_save();
        }
    }

  return NULL;
}

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Should we perform board-specific driver initialization? There are two
 * ways that board initialization can occur:  1) automatically via
 * board_late_initialize() during bootupif CONFIG_BOARD_LATE_INITIALIZE
 * or 2).
 * via a call to boardctl() if the interface is enabled
 * (CONFIG_BOARDCTL=y).
 * If this task is running as an NSH built-in application, then that
 * initialization has probably already been performed otherwise we do it
 * here.
 */

#undef NEED_BOARDINIT

#if defined(CONFIG_BOARDCTL) && !defined(CONFIG_NSH_ARCHINIT)
#  define NEED_BOARDINIT 1
#endif

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* Shared screen pointers */

lv_obj_t *main_screen = NULL;
lv_obj_t *menu_screen = NULL;
lv_obj_t *wifi_screen = NULL;
lv_obj_t *recorder_screen = NULL;
lv_obj_t *player_screen = NULL;
lv_obj_t *ai_screen = NULL;
lv_obj_t *camera_screen = NULL;
lv_obj_t *meeting_screen = NULL;
lv_obj_t *settings_screen = NULL;

/* ── System State Machine ───────────────────────────────────── */

typedef enum {
    SYSTEM_STATE_IDLE,           // 空闲状态（网络未就绪）
    SYSTEM_STATE_NETWORKING,     // 网络连接中
    SYSTEM_STATE_ONLINE_WAKEUP,  // 在线唤醒（网络就绪）
    SYSTEM_STATE_AI_RESPONSE,    // AI响应中
} system_state_t;

typedef struct {
    system_state_t state;
    bool network_ready;
} system_context_t;

static system_context_t system_ctx = {
    .state = SYSTEM_STATE_IDLE,
    .network_ready = false
};

/* Forward declarations for netdown notifier worker (defined after
 * on_network_disconnected).  The worker is registered in
 * on_network_ready() and fires immediately when the WiFi driver
 * calls netdev_carrier_off(). */
#ifdef CONFIG_NETDOWN_NOTIFIER
static void netdown_notify_worker(void *arg);
#endif

/* Network state callback */
static void on_network_ready(void)
{
  /* Wait for disconnect handler to finish if still running */
  if (s_disconnect_handler_active)
    {
      syslog(LOG_INFO, "[lvgldemo] Disconnect handler still active, deferring network ready\n");
      return;  /* last_network_ready not updated, will retry on next tick */
    }

  syslog(LOG_INFO, "[lvgldemo] Network ready, connecting cloud (idle mode)...\n");
  system_ctx.network_ready = true;
  system_ctx.state = SYSTEM_STATE_ONLINE_WAKEUP;

  /* 更新主界面状态 */
  xiaoq_update_status("🎤 你好小Q");

  /* 播放"wifi已连接"提示音（通过VAD线程播放，不阻塞） */
  wakeup_detector_request_prompt(i18n_get_prompt(PROMPT_WIFI_YI_LIANJIE));

  /* 启动云端连接（空闲模式，不发送mic，VAD继续运行） */
  voice_assistant_start();

  /* 重置语音对话界面连接状态。断连时 ai_status_label 被设为
   * "AI: 未连接"，重连后需恢复为"AI: 已连接"，否则界面会残留
   * "AI: 未连接"与"正在监听..."的不一致状态。 */
  ai_page_update_wakeup_status(i18n_get(STR_AI_CONNECTED));

  last_network_ready = true;

  /* Register for immediate network-down notification (zero polling delay).
   * netdown_notifier_setup() is one-shot: fires once when
   * netdev_carrier_off() is called by the WiFi driver, then auto-tears-down.
   * Re-registered each time WiFi connects.  The polling timer (1s) and
   * VA disconnect callback remain as fallbacks. */
#ifdef CONFIG_NETDOWN_NOTIFIER
  {
    FAR struct net_driver_s *dev = netdev_findbyname(WIFI_NETCARD);
    if (dev)
      {
        s_netdown_notifier_key = netdown_notifier_setup(netdown_notify_worker,
                                                         dev, NULL);
        if (s_netdown_notifier_key > 0)
          syslog(LOG_INFO, "[lvgldemo] Registered NETDEV_DOWN notifier (key=%d)\n",
                 s_netdown_notifier_key);
      }
  }
#endif
}

/* Background thread for network disconnection handling
 * Avoids blocking the LVGL timer with voice_assistant_stop() (pthread_join). */
static void *disconnect_handler_thread(void *arg)
{
  (void)arg;

  syslog(LOG_INFO, "[lvgldemo] Disconnect handler started\n");

  /* 停止云端连接（如果在对话模式，会关闭mic并重启VAD） */
  voice_assistant_stop();

  /* voice_assistant_stop() 后 DashScope 线程已退出，此时更新
   * 语音对话界面状态不会被覆盖。否则界面会残留"已连接"和
   * "网络连接中..."等过期状态，误导用户。 */
  voice_assistant_update_status("网络已断开");
  ai_page_update_wakeup_status(i18n_get(STR_AI_DISCONNECTED));

  /* 仅当ai_page活跃时（suspended=false）才重启本地VAD；
   * 退出ai_page后suspended=true，VAD应保持关闭 */
  if (!voice_assistant_is_listening_suspended() &&
      !wakeup_detector_is_running())
    {
      wakeup_detector_start();
      usleep(500000);
    }

  /* 播放"wifi已断开"提示音（通过VAD线程播放，不阻塞） */
  wakeup_detector_request_prompt(i18n_get_prompt(PROMPT_WIFI_YI_DUANKAI));

  s_disconnect_handler_active = false;
  syslog(LOG_INFO, "[lvgldemo] Disconnect handler done\n");
  return NULL;
}

/* Forward declaration: on_va_disconnect_cb() calls on_network_disconnected()
 * which is defined below. */
static void on_network_disconnected(void);

/* Called from DashScope thread when ASR stream disconnects.
 * Checks if WiFi is actually down and triggers immediate disconnect
 * handling, bypassing the polling timer delay.
 * Thread-safe: s_disconnect_handler_active and last_network_ready
 * guard against duplicate calls from callback + polling. */
static void on_va_disconnect_cb(void)
{
#ifdef CONFIG_WIRELESS_WAPI
  if (!last_network_ready || s_disconnect_handler_active)
    return;

  int flags = wifi_get_if_flags(WIFI_NETCARD);
  if (flags & IFF_RUNNING)
    return;  /* WiFi still up, ASR disconnect was server-side */

  syslog(LOG_INFO, "[lvgldemo] VA disconnect callback: WiFi down, immediate reset\n");
  on_network_disconnected();
#endif
}

static void on_network_disconnected(void)
{
  /* Guard against concurrent calls from netdown notifier worker,
   * VA disconnect callback, and polling timer. */
  if (s_disconnect_handler_active || !last_network_ready)
    return;

  /* Set this FIRST to prevent concurrent callers from proceeding */
  s_disconnect_handler_active = true;

  syslog(LOG_INFO, "[lvgldemo] Network disconnected\n");
  system_ctx.network_ready = false;
  system_ctx.state = SYSTEM_STATE_IDLE;

  /* 更新主界面状态 */
  xiaoq_update_status("等待网络...");

  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, 8 * 1024);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

  pthread_t tid;
  if (pthread_create(&tid, &attr, disconnect_handler_thread, NULL) != 0)
    {
      syslog(LOG_ERR, "[lvgldemo] Failed to create disconnect handler thread\n");
      s_disconnect_handler_active = false;
      /* Fallback: do it inline */
      voice_assistant_stop();
      voice_assistant_update_status("网络已断开");
      ai_page_update_wakeup_status(i18n_get(STR_AI_DISCONNECTED));
      if (!wakeup_detector_is_running())
        wakeup_detector_start();
      wakeup_detector_request_prompt(i18n_get_prompt(PROMPT_WIFI_YI_DUANKAI));
    }

  pthread_attr_destroy(&attr);

  last_network_ready = false;
}

#ifdef CONFIG_NETDOWN_NOTIFIER
/* NuttX NETDEV_DOWN worker: fires immediately when netdev_carrier_off()
 * is called by the WiFi driver — zero polling delay.
 * Runs on LPWORK; on_network_disconnected() spawns a detached thread for
 * the heavy voice_assistant_stop() work, so this worker returns quickly.
 * One-shot: auto-torn-down after firing; re-registered in on_network_ready(). */
static void netdown_notify_worker(void *arg)
{
  (void)arg;
  s_netdown_notifier_key = 0;
  syslog(LOG_INFO, "[lvgldemo] NETDEV_DOWN notifier: immediate reset\n");
  on_network_disconnected();
}
#endif

/* LVGL 主线程 drain 定时器回调：消费跨线程 UI 派发队列 */
static void dispatch_timer_cb(lv_timer_t * timer)
{
  LV_UNUSED(timer);
  lvgl_dispatch_drain();
}

/* Network state monitoring timer (LCD path) */
static void network_monitor_timer_cb(lv_timer_t * timer)
{
#ifdef CONFIG_WIRELESS_WAPI
  LV_UNUSED(timer);
  
  bool current_ready = false;
  int flags = wifi_get_if_flags(WIFI_NETCARD);
  if (flags & IFF_RUNNING) {
    /* IFF_RUNNING 在 WiFi 关联后即设置，早于 DHCP 完成。
     * 必须额外检查 IP 地址非零，确保 DHCP 已完成、DNS 已配置，
     * 否则 DashScope/TTS 连接会因 DNS 解析失败而 ECONNREFUSED (-111)。
     * 移动 WiFi 路由器 DHCP+DNS 配置延迟更长，尤其需要此检查。 */
    struct in_addr ipaddr;
    if (netlib_get_ipv4addr(WIFI_NETCARD, &ipaddr) == 0 && ipaddr.s_addr != 0) {
      current_ready = true;
    }
  }

  if (current_ready && !last_network_ready) {
    /* Network just became ready — on_network_ready updates last_network_ready
     * on success, or returns without updating if deferred */
    on_network_ready();
  } else if (!current_ready && last_network_ready) {
    /* Network just disconnected — on_network_disconnected updates last_network_ready */
    on_network_disconnected();
  }
#endif
}

/* Network state monitoring thread (no-LCD path).
 * Replaces the LVGL timer-based monitor when there is no display.
 * Periodically polls WiFi interface flags and triggers the same
 * on_network_ready / on_network_disconnected callbacks. */
static void *network_monitor_thread(void *arg)
{
  (void)arg;
  for (;;) {
    usleep(1000000);  /* 1s, same interval as the LVGL timer */

#ifdef CONFIG_WIRELESS_WAPI
    bool current_ready = false;
    int flags = wifi_get_if_flags(WIFI_NETCARD);
    if (flags & IFF_RUNNING) {
      /* IFF_RUNNING 在 WiFi 关联后即设置，早于 DHCP 完成。
       * 必须额外检查 IP 地址非零，确保 DHCP 已完成、DNS 已配置，
       * 否则 DashScope/TTS 连接会因 DNS 解析失败而 ECONNREFUSED (-111)。
       * 移动 WiFi 路由器 DHCP+DNS 配置延迟更长，尤其需要此检查。 */
      struct in_addr ipaddr;
      if (netlib_get_ipv4addr(WIFI_NETCARD, &ipaddr) == 0 && ipaddr.s_addr != 0) {
        current_ready = true;
      }
    }

    if (current_ready && !last_network_ready) {
      on_network_ready();
    } else if (!current_ready && last_network_ready) {
      on_network_disconnected();
    }
#endif
  }
  return NULL;
}

/* Forward declaration */
static void enter_ai_page(void);
static void enter_camera_page(void);
static void enter_meeting_page(void);
static void enter_menu_page(void);

static void enter_ai_page_async_cb(void * arg)
{
  (void)arg;
  enter_ai_page();
}

static void enter_camera_page_async_cb(void * arg)
{
  (void)arg;
  enter_camera_page();
}

static void enter_meeting_page_async_cb(void * arg)
{
  (void)arg;
  enter_meeting_page();
}

/* Local wake word callback — called from VAD thread.
 * Returns action: CONTINUE (keep VAD) or STOP (hand mic to cloud). */
static wakeup_action_t on_local_wakeup(void)
{
  syslog(LOG_INFO, "[lvgldemo] Local wake word detected\n");

  if (system_ctx.network_ready)
    {
      /* WiFi connected: enter cloud dialogue mode */
      syslog(LOG_INFO, "[lvgldemo] WiFi connected, entering cloud dialogue\n");
      system_ctx.state = SYSTEM_STATE_AI_RESPONSE;
      xiaoq_update_status("👂 唤醒中...");

      /* 显示交互气泡：用户唤醒词气泡 + AI回复气泡
       * 仅在语音对话界面显示，避免其他页面唤醒时操作已释放的UI对象 */
      if (ai_screen != NULL && lv_scr_act() == ai_screen)
        {
          ai_page_show_user_message(i18n_get(STR_AI_WAKEWORD_USER));
          ai_page_show_omni_response(i18n_get(STR_AI_WAKEWORD_REPLY));
        }

      /* Play "你好，请说！" prompt (stops VAD recorder, plays, doesn't reopen) */
      wakeup_detector_play_prompt(i18n_get_prompt(PROMPT_NIHAO_QING_SHUO));

      /* Switch to cloud dialogue mode (stops VAD, opens cloud mic) */
      voice_assistant_enter_dialogue();

      return WAKEUP_ACTION_STOP;
    }
  else
    {
      /* No WiFi: prompt user to connect */
      syslog(LOG_INFO, "[lvgldemo] No WiFi, prompting to connect\n");

      /* Play "请连接wifi" prompt (stops VAD recorder, plays, doesn't reopen) */
      wakeup_detector_play_prompt(i18n_get_prompt(PROMPT_QING_LIANJIE_WIFI));

      return WAKEUP_ACTION_CONTINUE;
    }
}

static void on_meeting_keyword_detected(void)
{
  syslog(LOG_INFO, "[lvgldemo] Meeting keyword detected, entering meeting mode...\n");
  lv_async_call(enter_meeting_page_async_cb, NULL);
}

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/* Gesture event handler for main screen */
static void main_gesture_cb(lv_event_t * e)
{
  lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_active());
  if (dir == LV_DIR_TOP) {
    enter_menu_page();
  }
}

static void enter_ai_page(void)
{
  if (ai_screen == NULL) {
    ai_screen = lv_obj_create(NULL);
    create_ai_page(ai_screen);
  }
  lv_scr_load_anim(ai_screen, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 300, 0, false);
}

static void enter_camera_page(void)
{
  if (camera_screen == NULL) {
    camera_screen = lv_obj_create(NULL);
    create_camera_page(camera_screen);
  }
  lv_scr_load_anim(camera_screen, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 300, 0, false);
}

static void enter_meeting_page(void)
{
  if (meeting_screen == NULL) {
    meeting_screen = lv_obj_create(NULL);
    create_meeting_page(meeting_screen);
  }
  lv_scr_load_anim(meeting_screen, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 300, 0, false);
}

static void enter_menu_page(void)
{
  if (menu_screen == NULL) {
    menu_screen = lv_obj_create(NULL);
    create_menu_page(menu_screen);
  }
  lv_scr_load_anim(menu_screen, LV_SCR_LOAD_ANIM_MOVE_TOP, 300, 0, false);
}



#ifdef CONFIG_LV_USE_NUTTX_LIBUV
static void lv_nuttx_uv_loop(uv_loop_t *loop, lv_nuttx_result_t *result)
{
  lv_nuttx_uv_t uv_info;
  void *data;

  uv_loop_init(loop);

  lv_memset(&uv_info, 0, sizeof(uv_info));
  uv_info.loop = loop;
  uv_info.disp = result->disp;
  uv_info.indev = result->indev;
#ifdef CONFIG_UINPUT_TOUCH
  uv_info.uindev = result->utouch_indev;
#endif

  data = lv_nuttx_uv_init(&uv_info);
  uv_run(loop, UV_RUN_DEFAULT);
  lv_nuttx_uv_deinit(&data);
}
#endif

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: main or lv_demos_main
 *
 * Description:
 *
 * Input Parameters:
 *   Standard argc and argv
 *
 * Returned Value:
 *   Zero on success; a positive, non-zero value on failure.
 *
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  lv_nuttx_dsc_t info;
  lv_nuttx_result_t result;

#ifdef CONFIG_LV_USE_NUTTX_LIBUV
  uv_loop_t ui_loop;
  lv_memzero(&ui_loop, sizeof(ui_loop));
#endif

  if (lv_is_initialized())
    {
      LV_LOG_ERROR("LVGL already initialized! aborting.");
      return -1;
    }

#ifdef NEED_BOARDINIT
  /* Perform board-specific driver initialization */

  boardctl(BOARDIOC_INIT, 0);

#endif

  lv_init();

  lv_nuttx_dsc_init(&info);

#ifdef CONFIG_LV_USE_NUTTX_LCD
  info.fb_path = "/dev/lcd0";
#endif

#ifdef CONFIG_INPUT_TOUCHSCREEN
  info.input_path = CONFIG_EXAMPLES_LVGLDEMO_INPUT_DEVPATH;
#endif

  lv_nuttx_init(&info, &result);
  if (result.indev)
    {
      void *drv_data = lv_indev_get_driver_data(result.indev);
      if (drv_data)
        {
          int touch_fd = *(int *)drv_data;
          if (touch_fd >= 0 && touch_fd < 20)
            {
              int new_fd = fcntl(touch_fd, F_DUPFD, 20);
              if (new_fd >= 0)
                {
                  /* 不关闭 touch_fd！保留它打开作为占位符，防止飞书 socket 复用。 */
                  *(int *)drv_data = new_fd;
                  fcntl(new_fd, F_SETFD, FD_CLOEXEC);
                  syslog(LOG_INFO,
                    "[TOUCH] uv_poll fd=%d -> fd=%d (kept fd=%d open as guard)\n",
                    touch_fd, new_fd, touch_fd);
                }
              else
                {
                  syslog(LOG_ERR, "[TOUCH] F_DUPFD failed: errno=%d\n", errno);
                }
            }
        }
    }

  bool has_display = (result.disp != NULL);
  if (!has_display)
    {
      LV_LOG_ERROR("Display init failed (no LCD?), continuing without UI...");
    }

  /* Boot default for LED indicators: red (GPIO34) off, green (GPIO35) off.
   * Configures both pins as output and drives low. Must happen before any
   * ai_page lifecycle sets the green LED on (create_ai_page). */
  ai_page_led_boot_init();

  persist_time_load();

  if (has_display)
    {
      /* 初始化 i18n（必须在 UI 创建之前，读取 /emmc/lang_config） */
      i18n_init();

      /* Create main screen with xiaoq animation */
      main_screen = lv_scr_act();
      lv_obj_set_style_bg_color(main_screen, lv_color_hex(0x000000), 0);
      lv_obj_set_style_bg_opa(main_screen, LV_OPA_COVER, 0);
      lv_obj_clear_flag(main_screen, LV_OBJ_FLAG_SCROLLABLE);
      lv_obj_clear_flag(main_screen, LV_OBJ_FLAG_SCROLL_ELASTIC);
      lv_obj_clear_flag(main_screen, LV_OBJ_FLAG_SCROLL_MOMENTUM);

      create_xiaoq_page(main_screen);
    }

  time_save_running = true;
  {
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 2048);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&time_save_thread, &attr, time_save_thread_func, NULL) != 0)
      {
        LV_LOG_USER("failed to create time save thread");
        time_save_running = false;
      }
    pthread_attr_destroy(&attr);
  }

  /* Add gesture handler on main screen for left swipe */
  if (has_display)
    lv_obj_add_event_cb(main_screen, main_gesture_cb, LV_EVENT_GESTURE, NULL);

  /* Start WiFi auto-connect and network state monitoring.
   * WiFi auto-connect loads saved credentials and connects in the
   * background, independent of whether the user navigates to the
   * WiFi page. */
#ifdef CONFIG_WIRELESS_WAPI
  wifi_auto_connect_start();
  last_network_ready = false;

  /* Register VA disconnect callback for immediate WiFi disconnect detection.
   * When the DashScope thread detects ASR stream ECONNRESET, it calls this
   * callback, which checks wifi_get_if_flags() and triggers
   * on_network_disconnected() without waiting for the polling timer. */
  voice_assistant_set_disconnect_callback(on_va_disconnect_cb);

  if (has_display)
    {
      network_monitor_timer = lv_timer_create(network_monitor_timer_cb, 1000, NULL);
    }
  else
    {
      /* No LCD: the LVGL network_monitor_timer is never created, so
       * system_ctx.network_ready stays false forever and wake-word
       * callbacks always play "please connect WiFi".  Run a background
       * thread that polls the interface flags and calls the same
       * on_network_ready / on_network_disconnected callbacks. */
      pthread_attr_t attr;
      pthread_attr_init(&attr);
      pthread_attr_setstacksize(&attr, 4096);
      pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
      pthread_t tid;
      pthread_create(&tid, &attr, network_monitor_thread, NULL);
      pthread_attr_destroy(&attr);
    }
#endif

  /* 跨线程 UI 派发器的消费端：在 LVGL 主线程周期性 drain 队列。
   * 5ms 周期 (200Hz) 对人眼足够流畅，空队列时仅一次锁+判断，开销可忽略。
   * 所有其他线程的 UI 更新都通过 lvgl_dispatch_async 投递到此消费。
   * 无 LCD 时跳过 UI 派发器（队列项将被丢弃，不影响核心功能）。 */
  if (has_display)
    {
      lv_timer_t *dispatch_timer = lv_timer_create(dispatch_timer_cb, 5, NULL);
      if (dispatch_timer) {
        lv_timer_set_repeat_count(dispatch_timer, -1);
      }
    }

#ifdef CONFIG_EXAMPLES_LVGLDEMO_WAKEUP_ENABLE
  wakeup_detector_init();
  /* 开机不默认启动本地VAD：VAD由ai_page生命周期控制
   *   进入ai_page → 启动本地VAD
   *   退出ai_page → 关闭本地VAD + 关闭云端mic + 复位所有标记
   */

  /* Register local wake word callback */
  wakeup_detector_set_callback(on_local_wakeup);

#ifdef CONFIG_EXAMPLES_AI_AGENT_VELA
  voice_assistant_init();
  voice_assistant_set_meeting_callback(on_meeting_keyword_detected);
#endif
#endif
  bt_call_handler_init();

  /* NOTE: Do NOT add LV_OBJ_FLAG_GESTURE_BUBBLE to screen objects!
   * LVGL walks UP from the touched child until it finds an object WITHOUT
   * GESTURE_BUBBLE, and sends the gesture event there. If the screen has
   * this flag, the walk continues to NULL and the gesture is lost. */

  /* All sub-screens (wifi, recorder, player, ai) are created lazily
   * when the user swipes to them, and destroyed on screen unload.
   * This saves boot time and LVGL heap memory. */

#ifdef CONFIG_LV_USE_NUTTX_LIBUV
  if (has_display)
    lv_nuttx_uv_loop(&ui_loop, &result);
  else
    while (1)
      usleep(1000000);
#else
  if (has_display)
    {
      while (1)
        {
          uint32_t idle;
          idle = lv_timer_handler();

          /* Minimum sleep of 1ms */

          idle = idle ? idle : 1;
          usleep(idle * 1000);
        }
    }
  else
    while (1)
      usleep(1000000);
#endif

time_save_running = false;

#ifdef CONFIG_WIRELESS_WAPI
  if (network_monitor_timer) {
    lv_timer_del(network_monitor_timer);
    network_monitor_timer = NULL;
  }
#endif

#ifdef CONFIG_EXAMPLES_LVGLDEMO_WAKEUP_ENABLE
  wakeup_detector_deinit();
#ifdef CONFIG_EXAMPLES_AI_AGENT_VELA
  voice_assistant_deinit();
#endif
#endif
  ai_page_stop();
  camera_page_deinit();
  wifi_page_deinit();
  recorder_page_deinit();
  player_page_deinit();
  call_page_deinit();
  calling_page_deinit();
  settings_page_deinit();
  bt_call_handler_deinit();
  xiaoq_cleanup();

  lv_nuttx_deinit(&result);
  lv_deinit();

  return 0;
}
