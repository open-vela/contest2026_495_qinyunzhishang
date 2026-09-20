/****************************************************************************
 * apps/examples/lvgldemo/wifi_page.c
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
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/stat.h>
#include <syslog.h>
#include <lvgl/lvgl.h>

#include "wifi_page.h"
#include "lvgldemo_common.h"
#include "lvgl_dispatch.h"
#include "settings_page.h"

#ifdef CONFIG_WIRELESS_WAPI
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <nuttx/net/netconfig.h>
#include <netutils/netlib.h>
#include <netutils/dhcpc.h>
#include <wireless/wapi.h>

/* Declared here to avoid pulling in bwifi_interface.h, which brings in a
 * large chain of BES framework headers (plat_types.h, cmsis_os.h,
 * wifi_def.h, bwifi_hal.h, bwifi_event.h) that are not part of the
 * lvgldemo include paths. bwifi_reset() is the correct WiFi recovery
 * API: unlike netlib_ifdown/ifup (which only toggles IFF_UP), it also
 * restarts the WPA supplicant task via wpa_agent_remove_vif/add_vif.
 * Without WPA task restart, scan/connect ioctls silently fail and the
 * driver enters an 82-second reset loop that never recovers. */
extern int bwifi_reset(void);
#endif

/****************************************************************************
 * WiFi Settings Page Data
 ****************************************************************************/

#ifdef CONFIG_WIRELESS_WAPI

#define WIFI_NETCARD "wlan0"
#define WIFI_MAX_SCAN_RESULTS 64

/* WiFi config file for auto-connect credentials */
#define WIFI_CONFIG_DIR    "/emmc/wifi"
#define WIFI_CONFIG_FILE   "/emmc/wifi/wifi_config.txt"
#define WIFI_MAX_SAVED     3
#define WIFI_AUTO_SCAN_INTERVAL_SEC  8

/* AP 列表布局常量（参考 recorder_page 圆形屏适配，与 menu_page 一致风格）*/
#define WIFI_LIST_PAD_LR    50    /* 列表左右各预留 50px（圆形屏内切圆适配）*/
#define WIFI_LIST_PAD_BOT   100   /* 列表底部预留 100px（避开圆形不可见区）*/
#define WIFI_ITEM_HEIGHT    50    /* 列表项高度（与 recorder_page 一致）*/
#define WIFI_ITEM_GAP       8     /* 列表项间距 */

typedef struct {
  char ssid[128];
  char password[64];
} wifi_credential_t;

static lv_obj_t * wifi_ap_list = NULL;
static lv_obj_t * wifi_status_label = NULL;
static lv_obj_t * wifi_ip_label = NULL;
static lv_obj_t * wifi_scan_btn = NULL;
static lv_obj_t * wifi_pwd_textarea = NULL;
static lv_obj_t * wifi_pwd_kb = NULL;
static lv_obj_t * wifi_pwd_modal = NULL;
static lv_obj_t * wifi_pwd_eye_btn = NULL;
static char wifi_selected_ssid[128] = {0};
static char wifi_connecting_ssid[128] = {0};
static bool wifi_is_on = true;
static bool wifi_is_connected = false;
static lv_timer_t * wifi_status_timer = NULL;

/* DHCP handle for the persistent renewal thread started by
 * dhcpc_request_async(). The dhcpc_run() worker automatically re-requests
 * the lease at lease_time/2 intervals, preventing AP-side lease expiry
 * (typically 8h on 4G routers) from disconnecting us with reason_code=1.
 * This replaces the one-shot netlib_obtain_ipv4addr() which never renews
 * (see its own comment: "there is no logic for renewing the IP address"). */
static void *g_dhcpc_handle = NULL;

/* Apply DHCP result to the netif (IP/netmask/router/DNS).
 * Mirrors netlib_obtainipv4addr.c:dhcp_setup_result() but runs in the
 * dhcpc_run worker context, so we call it each time the lease is obtained
 * or renewed.
 *
 * 注意：dhcpc_run 在 dhcpc_request 失败（如超时未收到 DHCP Offer）时会
 * 以 NULL 调用本回调。必须做 NULL 保护，否则 pcd->ipaddr 解引用 NULL
 * 触发 MemFault 崩溃（CFSR=0x82, MMFAR=0x4）。 */
static void wifi_dhcpc_callback(FAR struct dhcpc_state *pcd)
{
  int ret;

  if (pcd == NULL)
    {
      syslog(LOG_WARNING, "[wifi_page] DHCP callback got NULL state (request failed)\n");
      return;
    }

  ret = netlib_set_ipv4addr(WIFI_NETCARD, &pcd->ipaddr);
  if (ret < 0)
    {
      syslog(LOG_WARNING, "[wifi_page] set_ipv4addr failed\n");
    }

  if (pcd->netmask.s_addr != 0)
    {
      ret = netlib_set_ipv4netmask(WIFI_NETCARD, &pcd->netmask);
      if (ret < 0)
        {
          syslog(LOG_WARNING, "[wifi_page] set_ipv4netmask failed\n");
        }
    }

  if (pcd->default_router.s_addr != 0)
    {
      ret = netlib_set_dripv4addr(WIFI_NETCARD, &pcd->default_router);
      if (ret < 0)
        {
          syslog(LOG_WARNING, "[wifi_page] set_dripv4addr failed\n");
        }
    }

  if (pcd->dnsaddr.s_addr != 0)
    {
#ifdef CONFIG_NETDB_DNSCLIENT
      netlib_set_ipv4dnsaddr(&pcd->dnsaddr);
#endif
    }

  syslog(LOG_INFO, "[wifi_page] DHCP lease obtained (lease=%lus)\n",
         (unsigned long)pcd->lease_time);
}

/* Start persistent DHCP renewal. Closes any previous handle first so
 * repeated calls (e.g. on reconnect) are safe. */
static void wifi_dhcpc_start(void)
{
  uint8_t mac[IFHWADDRLEN];
  int ret;

  if (g_dhcpc_handle != NULL)
    {
      dhcpc_cancel(g_dhcpc_handle);
      dhcpc_close(g_dhcpc_handle);
      g_dhcpc_handle = NULL;
    }

  /* dhcpc_open requires the interface MAC address (same pattern as
   * netlib_obtainipv4addr.c:dhcp_obtain_statefuladdr). */
  ret = netlib_getmacaddr(WIFI_NETCARD, mac);
  if (ret < 0)
    {
      syslog(LOG_WARNING, "[wifi_page] getmacaddr failed (%d)\n", ret);
      return;
    }

  g_dhcpc_handle = dhcpc_open(WIFI_NETCARD, mac, IFHWADDRLEN);
  if (g_dhcpc_handle == NULL)
    {
      syslog(LOG_WARNING, "[wifi_page] dhcpc_open failed\n");
      return;
    }

  /* dhcpc_request_async starts dhcpc_run thread which loops:
   *   dhcpc_request → callback → sleep(lease_time/2) → repeat
   * The thread stays alive until dhcpc_cancel/dhcpc_close. */
  ret = dhcpc_request_async(g_dhcpc_handle, wifi_dhcpc_callback);
  if (ret < 0)
    {
      syslog(LOG_WARNING, "[wifi_page] dhcpc_request_async failed (%d)\n",
             ret);
      dhcpc_close(g_dhcpc_handle);
      g_dhcpc_handle = NULL;
      return;
    }

  syslog(LOG_INFO, "[wifi_page] DHCP renewal thread started\n");
}

/* Stop persistent DHCP renewal. Safe to call when not running. */
static void wifi_dhcpc_stop(void)
{
  if (g_dhcpc_handle != NULL)
    {
      dhcpc_cancel(g_dhcpc_handle);
      dhcpc_close(g_dhcpc_handle);
      g_dhcpc_handle = NULL;
      syslog(LOG_INFO, "[wifi_page] DHCP renewal thread stopped\n");
    }
}

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/* Forward declarations */
static void wifi_ap_click_cb(lv_event_t * e);
static void wifi_update_status(void);

/* Get interface flags helper */
int wifi_get_if_flags(FAR const char *ifname)
{
  struct ifreq req;
  memset(&req, 0, sizeof(req));
  if (ifname) {
    int sockfd = socket(NET_SOCK_FAMILY, NET_SOCK_TYPE, NET_SOCK_PROTOCOL);
    if (sockfd >= 0) {
      strlcpy(req.ifr_name, ifname, IFNAMSIZ);
      ioctl(sockfd, SIOCGIFFLAGS, (unsigned long)&req);
      close(sockfd);
    }
  }
  return req.ifr_flags;
}

/* Update WiFi status display */
static void wifi_update_status(void)
{
  if (!wifi_status_label) return;

  if (!wifi_is_on) {
    lv_label_set_text(wifi_status_label, "WiFi: OFF");
    lv_obj_set_style_text_color(wifi_status_label, lv_color_hex(0xCCCCCC), 0);
    if (wifi_ip_label) lv_label_set_text(wifi_ip_label, "");
    wifi_is_connected = false;
    return;
  }

  int flags = wifi_get_if_flags(WIFI_NETCARD);
  if (flags & IFF_RUNNING) {
    wifi_is_connected = true;

    /* Line 1: SSID name in blue */
    if (wifi_selected_ssid[0] != '\0') {
      lv_label_set_text_fmt(wifi_status_label, LV_SYMBOL_WIFI " %s", wifi_selected_ssid);
      lv_obj_set_style_text_color(wifi_status_label, lv_color_hex(0x0088FF), 0);
    } else {
      lv_label_set_text(wifi_status_label, "Connected");
      lv_obj_set_style_text_color(wifi_status_label, lv_color_hex(0x0088FF), 0);
    }

    /* Line 2: status + IP in gray */
    int sock = wapi_make_socket();
    if (sock >= 0) {
      struct in_addr addr;
      if (wapi_get_ip(sock, WIFI_NETCARD, &addr) == 0) {
        char ip_str[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &addr, ip_str, sizeof(ip_str));
        if (wifi_ip_label)
          lv_label_set_text_fmt(wifi_ip_label, "IP: %s", ip_str);
      } else {
        if (wifi_ip_label) lv_label_set_text(wifi_ip_label, "IP: getting...");
      }
      close(sock);
    } else {
      if (wifi_ip_label) lv_label_set_text(wifi_ip_label, "Connected");
    }
  } else {
    wifi_is_connected = false;
    lv_label_set_text(wifi_status_label, "Disconnected");
    lv_obj_set_style_text_color(wifi_status_label, lv_color_hex(0xCCCCCC), 0);
    if (wifi_ip_label) lv_label_set_text(wifi_ip_label, "");
  }
}

/* 在 wifi_ap_list 容器内添加一条深色风格文本提示（替代 lv_list_add_text）。
 * 参考 recorder_page 空状态 "No recordings" 风格：灰色文字、montserrat_24 字体、
 * 左对齐 LV_ALIGN_TOP_LEFT。每次调用都新建一个 label，调用方需先 lv_obj_clean。 */
static void wifi_list_add_text_label(const char *text)
{
  if (!wifi_ap_list || !text) return;
  lv_obj_t *lbl = lv_label_create(wifi_ap_list);
  lv_obj_remove_style_all(lbl);
  lv_label_set_text(lbl, text);
  lv_obj_set_style_text_color(lbl, lv_color_hex(0xCCCCCC), 0);
  lv_obj_set_style_text_font(lbl, &lv_font_montserrat_24, 0);
  lv_obj_align(lbl, LV_ALIGN_TOP_LEFT, 0, 0);
}

/* 创建 WiFi AP 列表项（参考 recorder_page create_recorder_file_item 模式）
 * - 黑色背景按钮 + 圆角 + 按压缩放效果
 * - 左侧 WiFi 图标（蓝色）+ 右侧 SSID 文字（白色，montserrat_24，换行不滚动）
 * - 圆形屏适配：宽度 = LV_HOR_RES - 2*WIFI_LIST_PAD_LR，左对齐 TOP_LEFT
 * - SSID 存入 user_data（lv_malloc 分配），点击触发 wifi_ap_click_cb */
static void create_wifi_ap_item(int index, const char *ssid,
                                 int rssi_dbm, bool has_rssi)
{
  lv_obj_t *btn = lv_btn_create(wifi_ap_list);
  lv_obj_remove_style_all(btn);
  lv_obj_set_size(btn, LV_HOR_RES - 2 * WIFI_LIST_PAD_LR, WIFI_ITEM_HEIGHT);
  lv_obj_align(btn, LV_ALIGN_TOP_LEFT, 0,
               index * (WIFI_ITEM_HEIGHT + WIFI_ITEM_GAP));

  lv_obj_set_style_bg_color(btn, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(btn, 10, 0);
  lv_obj_set_style_border_width(btn, 0, 0);
  lv_obj_set_style_shadow_opa(btn, LV_OPA_TRANSP, 0);
  lv_obj_set_style_bg_color(btn, lv_color_hex(0x222222), LV_STATE_PRESSED);

  /* 内容容器（不可点击，避免拦截按钮事件）*/
  lv_obj_t *container = lv_obj_create(btn);
  lv_obj_remove_style_all(container);
  lv_obj_set_size(container, lv_pct(100), lv_pct(100));
  lv_obj_clear_flag(container, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_pad_all(container, 8, 0);
  lv_obj_set_style_pad_left(container, 12, 0);
  lv_obj_set_style_pad_right(container, 12, 0);
  lv_obj_set_style_border_width(container, 0, 0);
  lv_obj_set_style_bg_opa(container, LV_OPA_TRANSP, 0);

  /* 左侧 WiFi 图标 */
  lv_obj_t *icon_label = lv_label_create(container);
  lv_obj_remove_style_all(icon_label);
  lv_label_set_text(icon_label, LV_SYMBOL_WIFI);
  lv_obj_set_style_text_color(icon_label, lv_color_hex(0x0088FF), 0);
  lv_obj_set_style_text_font(icon_label, LV_FONT_DEFAULT, 0);
  lv_obj_align(icon_label, LV_ALIGN_LEFT_MID, 0, 0);

  /* SSID + RSSI 文字：换行显示，不滚动播报 */
  char item_text[WAPI_ESSID_MAX_SIZE + 32];
  if (has_rssi) {
    snprintf(item_text, sizeof(item_text), "%s  (%d dBm)", ssid, rssi_dbm);
  } else {
    snprintf(item_text, sizeof(item_text), "%s", ssid);
  }
  lv_obj_t *name_label = lv_label_create(container);
  lv_obj_remove_style_all(name_label);
  lv_label_set_text(name_label, item_text);
  lv_label_set_long_mode(name_label, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_color(name_label, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_text_font(name_label, &lv_font_montserrat_24, 0);
  /* 图标与文字间距35px，垂直居中适配1行或2行 */
  lv_obj_align(name_label, LV_ALIGN_LEFT_MID, 35, 0);
  /* 文字换行宽度 = 按钮宽 - 图标偏移35 - 左右pad 24 */
  lv_obj_set_width(name_label, LV_HOR_RES - 2 * WIFI_LIST_PAD_LR - 35 - 24);
  /* 先自适应高度，再限制最多2行：1行时居中，2行时完整显示，超过2行裁切 */
  lv_obj_update_layout(name_label);
  if (lv_obj_get_self_height(name_label) > 2 * 24) {
    lv_obj_set_height(name_label, 2 * 24);
  }

  /* SSID 存入 user_data，点击时用于密码弹窗 */
  char *ssid_copy = lv_malloc(strlen(ssid) + 1);
  if (ssid_copy) {
    strcpy(ssid_copy, ssid);
    lv_obj_set_user_data(btn, ssid_copy);
  }
  lv_obj_add_event_cb(btn, wifi_ap_click_cb, LV_EVENT_CLICKED, NULL);
}

/* WiFi scan and populate AP list */
static void wifi_do_scan(void)
{
  if (!wifi_ap_list) return;

  lv_obj_clean(wifi_ap_list);

  if (!wifi_is_on) return;

  int sock = wapi_make_socket();
  if (sock < 0) {
    wifi_list_add_text_label("Scan failed: socket error");
    return;
  }

  /* Set mode before scan */
  wapi_set_mode(sock, WIFI_NETCARD, IW_MODE_INFRA);

  /* Start scan */
  int ret = wapi_scan_init(sock, WIFI_NETCARD, NULL);
  if (ret < 0) {
    wifi_list_add_text_label("Scan init failed");
    close(sock);
    return;
  }

  /* Wait for scan to complete - give driver time to start scan first */
  usleep(500000);  /* 500ms initial wait */
  int retry = 0;
  while (retry < 80) {  /* up to 80 * 200ms = 16s */
    int stat = wapi_scan_stat(sock, WIFI_NETCARD);
    if (stat == 0) break;       /* data ready */
    if (stat < 0) {             /* error */
      wifi_list_add_text_label("Scan error");
      close(sock);
      return;
    }
    usleep(200000);
    retry++;
  }

  if (retry >= 80) {
    wifi_list_add_text_label("Scan timeout");
    close(sock);
    return;
  }

  /* Collect scan results */
  struct wapi_list_s scan_list;
  memset(&scan_list, 0, sizeof(scan_list));
  ret = wapi_scan_coll(sock, WIFI_NETCARD, &scan_list);
  if (ret < 0) {
    wifi_list_add_text_label("Scan collect failed");
    close(sock);
    return;
  }

  /* Add scan results to list */
  int count = 0;
  struct wapi_scan_info_s *info = scan_list.head.scan;
  while (info && count < WIFI_MAX_SCAN_RESULTS) {
    if (info->has_essid && info->essid[0] != '\0') {
      /* Filter out APs with SSID starting with "HQ_" */
      if (strncmp(info->essid, "HQ_", 3) == 0) {
        info = info->next;
        count++;
        continue;
      }
      /* Safety check: stop if memory is running low to prevent crash */
      if (lvgldemo_get_free_heap() < 4096) {
        wifi_list_add_text_label("... more APs (memory limit)");
        break;
      }
      /* 创建 AP 列表项（参考 recorder_page 风格：图标+SSID换行，深色主题）*/
      create_wifi_ap_item(count, info->essid, info->rssi, info->has_rssi);
    }
    info = info->next;
    count++;
  }

  if (count == 0) {
    wifi_list_add_text_label("No AP found");
  }

  wapi_scan_coll_free(&scan_list);
  close(sock);
}

/* WiFi connect */
static void wifi_do_connect(const char *ssid, const char *passwd)
{
  int sock = wapi_make_socket();
  if (sock < 0) return;

  netlib_ifup(WIFI_NETCARD);
  wapi_set_mode(sock, WIFI_NETCARD, IW_MODE_INFRA);

  /* Set WPA2 authentication */
  wpa_driver_wext_set_auth_param(sock, WIFI_NETCARD,
    IW_AUTH_WPA_VERSION, IW_AUTH_WPA_VERSION_WPA2);
  wpa_driver_wext_set_auth_param(sock, WIFI_NETCARD,
    IW_AUTH_CIPHER_PAIRWISE, IW_AUTH_CIPHER_CCMP);

  /* Set PSK */
  int passlen = strlen(passwd);
  if (passlen >= 8 && passlen <= 63) {
    wpa_driver_wext_set_key_ext(sock, WIFI_NETCARD, WPA_ALG_CCMP,
                                passwd, passlen);
  }

  /* Set ESSID and connect */
  wapi_set_essid(sock, WIFI_NETCARD, ssid, WAPI_ESSID_ON);

  /* Wait for connection */
  int loop_cnt = 0;
  while (!(wifi_get_if_flags(WIFI_NETCARD) & IFF_RUNNING) && loop_cnt < 60) {
    loop_cnt++;
    usleep(50000);
  }

  /* DHCP — start persistent renewal thread (auto re-requests at
   * lease_time/2). Replaces one-shot netlib_obtain_ipv4addr() which
   * never renews and causes AP-side lease-expiry disconnects. */
  if (wifi_get_if_flags(WIFI_NETCARD) & IFF_RUNNING) {
    wifi_dhcpc_start();
  }

  close(sock);
  wifi_update_status();
}

/* WiFi disconnect */
static void wifi_do_disconnect(void)
{
  int sock = wapi_make_socket();
  if (sock < 0) return;
  wpa_driver_wext_disconnect(sock, WIFI_NETCARD);
  close(sock);
  wifi_dhcpc_stop();
  wifi_is_connected = false;
  wifi_update_status();
}

/* Password modal close handler */
static void wifi_pwd_cancel_cb(lv_event_t * e)
{
  LV_UNUSED(e);
  if (wifi_pwd_modal) {
    lv_obj_del(wifi_pwd_modal);
    wifi_pwd_modal = NULL;
    wifi_pwd_textarea = NULL;
    wifi_pwd_kb = NULL;
    wifi_pwd_eye_btn = NULL;
  }
}

/* Password modal connect handler */
static void wifi_pwd_connect_cb(lv_event_t * e)
{
  LV_UNUSED(e);
  const char *pwd = lv_textarea_get_text(wifi_pwd_textarea);
  if (wifi_connecting_ssid[0] != '\0' && pwd && strlen(pwd) >= 8) {
    wifi_do_connect(wifi_connecting_ssid, pwd);
    /* Save credentials for auto-connect if connection succeeded */
    if (wifi_get_if_flags(WIFI_NETCARD) & IFF_RUNNING) {
      wifi_save_config(wifi_connecting_ssid, pwd);
    }
    /* Update connected SSID only after initiating connection */
    strncpy(wifi_selected_ssid, wifi_connecting_ssid, sizeof(wifi_selected_ssid) - 1);
    wifi_selected_ssid[sizeof(wifi_selected_ssid) - 1] = '\0';
  }
  /* Close modal */
  if (wifi_pwd_modal) {
    lv_obj_del(wifi_pwd_modal);
    wifi_pwd_modal = NULL;
    wifi_pwd_textarea = NULL;
    wifi_pwd_kb = NULL;
    wifi_pwd_eye_btn = NULL;
  }
}

/* Custom keyboard event handler - replaces default to fix mode switching.
 * Keyboard icon (LV_SYMBOL_KEYBOARD) toggles between NUMBER and TEXT:
 *   - On NUMBER pad (top-right icon) -> TEXT mode
 *   - On TEXT/SPECIAL pad (bottom-left icon) -> NUMBER mode
 * All other keys: delegate to default handler */
static void wifi_kb_custom_event_cb(lv_event_t * e)
{
  lv_obj_t * kb = lv_event_get_current_target(e);
  uint32_t btn_id = lv_buttonmatrix_get_selected_button(kb);
  if (btn_id == LV_BUTTONMATRIX_BUTTON_NONE) return;

  const char * txt = lv_buttonmatrix_get_button_text(kb, btn_id);
  if (txt == NULL) return;

  /* Keyboard icon toggles between NUMBER and TEXT modes */
  if (lv_strcmp(txt, LV_SYMBOL_KEYBOARD) == 0 ||
      lv_strcmp(txt, LV_SYMBOL_CLOSE) == 0) {
    lv_keyboard_mode_t mode = lv_keyboard_get_mode(kb);
    if (mode == LV_KEYBOARD_MODE_NUMBER) {
      lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_TEXT_LOWER);
    } else {
      lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_NUMBER);
    }
    return;
  }

  /* All other keys: let the default LVGL handler process them */
  lv_keyboard_def_event_cb(e);
}

/* Toggle password visibility */
static void wifi_pwd_eye_cb(lv_event_t * e)
{
  LV_UNUSED(e);
  if (!wifi_pwd_textarea || !wifi_pwd_eye_btn) return;
  bool pwd_mode = lv_textarea_get_password_mode(wifi_pwd_textarea);
  lv_textarea_set_password_mode(wifi_pwd_textarea, !pwd_mode);
  lv_obj_t * eye_label = lv_obj_get_child(wifi_pwd_eye_btn, 0);
  if (eye_label) {
    lv_label_set_text(eye_label, pwd_mode ? LV_SYMBOL_EYE_OPEN : LV_SYMBOL_EYE_CLOSE);
  }
}

/* AP list item click handler - show password dialog */
static void wifi_ap_click_cb(lv_event_t * e)
{
  lv_obj_t * btn = lv_event_get_target(e);
  char *ssid = (char *)lv_obj_get_user_data(btn);
  if (!ssid) return;

  /* Save to temporary buffer - only update wifi_selected_ssid on successful connect */
  strncpy(wifi_connecting_ssid, ssid, sizeof(wifi_connecting_ssid) - 1);
  wifi_connecting_ssid[sizeof(wifi_connecting_ssid) - 1] = '\0';

  /* Create full-screen password input modal for circular display */
  wifi_pwd_modal = lv_obj_create(lv_scr_act());
  lv_obj_remove_style_all(wifi_pwd_modal);
  lv_obj_set_size(wifi_pwd_modal, LV_PCT(100), LV_PCT(100));
  lv_obj_center(wifi_pwd_modal);
  lv_obj_set_style_bg_color(wifi_pwd_modal, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(wifi_pwd_modal, LV_OPA_COVER, 0);
  lv_obj_set_style_pad_all(wifi_pwd_modal, 8, 0);
  /* Less horizontal padding for wider keyboard */
  lv_obj_set_style_pad_left(wifi_pwd_modal, 15, 0);
  lv_obj_set_style_pad_right(wifi_pwd_modal, 15, 0);

  /* Cancel button */
  lv_obj_t * cancel_btn = lv_button_create(wifi_pwd_modal);
  lv_obj_set_size(cancel_btn, 139, 32);
  lv_obj_set_style_bg_color(cancel_btn, lv_color_hex(0x555555), 0);
  lv_obj_set_style_radius(cancel_btn, 6, 0);
  lv_obj_t * cancel_label = lv_label_create(cancel_btn);
  lv_label_set_text(cancel_label, "Cancel");
  lv_obj_set_style_text_color(cancel_label, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_text_font(cancel_label, &lv_font_montserrat_24, 0);
  lv_obj_center(cancel_label);
  lv_obj_add_event_cb(cancel_btn, wifi_pwd_cancel_cb, LV_EVENT_CLICKED, NULL);

  /* Connect button */
  lv_obj_t * connect_btn = lv_button_create(wifi_pwd_modal);
  lv_obj_set_size(connect_btn, 139, 32);
  lv_obj_set_style_bg_color(connect_btn, lv_color_hex(0x0088FF), 0);
  lv_obj_set_style_radius(connect_btn, 6, 0);
  lv_obj_t * conn_label = lv_label_create(connect_btn);
  lv_label_set_text(conn_label, "Connect");
  lv_obj_set_style_text_color(conn_label, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_text_font(conn_label, &lv_font_montserrat_24, 0);
  lv_obj_center(conn_label);
  lv_obj_add_event_cb(connect_btn, wifi_pwd_connect_cb, LV_EVENT_CLICKED, NULL);

  /* Keyboard - 紧挨密码框下方，宽度354px，居中 */
  wifi_pwd_kb = lv_keyboard_create(wifi_pwd_modal);
  lv_obj_set_width(wifi_pwd_kb, 314);
  lv_obj_set_height(wifi_pwd_kb, LV_PCT(55));
  lv_obj_align(wifi_pwd_kb, LV_ALIGN_BOTTOM_MID, 0, -98);
  lv_keyboard_set_mode(wifi_pwd_kb, LV_KEYBOARD_MODE_NUMBER);

  /* Buttons 紧挨键盘下方，各压缩50px宽度，向中心移动25px */
  lv_obj_align_to(cancel_btn, wifi_pwd_kb, LV_ALIGN_OUT_BOTTOM_LEFT, 10, 4);
  lv_obj_align_to(connect_btn, wifi_pwd_kb, LV_ALIGN_OUT_BOTTOM_RIGHT, -20, 4);

  /* Password textarea - 宽度354px，紧挨键盘上方 */
  wifi_pwd_textarea = lv_textarea_create(wifi_pwd_modal);
  lv_obj_set_width(wifi_pwd_textarea, 314);
  lv_obj_set_height(wifi_pwd_textarea, 36);
  lv_obj_align_to(wifi_pwd_textarea, wifi_pwd_kb, LV_ALIGN_OUT_TOP_MID, 0, -6);
  lv_textarea_set_placeholder_text(wifi_pwd_textarea, "Password");
  lv_textarea_set_password_mode(wifi_pwd_textarea, true);
  lv_textarea_set_one_line(wifi_pwd_textarea, true);
  /* Add right padding so text doesn't overlap eye icon */
  lv_obj_set_style_pad_right(wifi_pwd_textarea, 32, 0);

  /* Now link textarea to keyboard AFTER textarea is created */
  lv_keyboard_set_textarea(wifi_pwd_kb, wifi_pwd_textarea);

  /* Replace default keyboard handler with custom one that supports mode switching
   * via keyboard icon (NUMBER<->TEXT) instead of default CANCEL/SPECIAL behavior */
  lv_obj_remove_event_cb(wifi_pwd_kb, lv_keyboard_def_event_cb);
  lv_obj_add_event_cb(wifi_pwd_kb, wifi_kb_custom_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

  /* Eye icon button - toggles password visibility, overlaid on right side of textarea */
  wifi_pwd_eye_btn = lv_button_create(wifi_pwd_modal);
  lv_obj_set_size(wifi_pwd_eye_btn, 28, 28);
  lv_obj_align_to(wifi_pwd_eye_btn, wifi_pwd_textarea, LV_ALIGN_RIGHT_MID, -4, 0);
  lv_obj_set_style_bg_opa(wifi_pwd_eye_btn, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(wifi_pwd_eye_btn, 0, 0);
  lv_obj_set_style_shadow_width(wifi_pwd_eye_btn, 0, 0);
  lv_obj_set_style_outline_width(wifi_pwd_eye_btn, 0, 0);
  lv_obj_t * eye_label = lv_label_create(wifi_pwd_eye_btn);
  lv_label_set_text(eye_label, LV_SYMBOL_EYE_CLOSE);
  lv_obj_set_style_text_color(eye_label, lv_color_hex(0xCCCCCC), 0);
  lv_obj_center(eye_label);
  lv_obj_add_event_cb(wifi_pwd_eye_btn, wifi_pwd_eye_cb, LV_EVENT_CLICKED, NULL);

  /* Title - SSID name，宽度300px超长显示...，居中 */
  lv_obj_t * title = lv_label_create(wifi_pwd_modal);
  lv_label_set_text_fmt(title, "%s", wifi_connecting_ssid);
  lv_obj_set_style_text_color(title, lv_color_hex(0x0088FF), 0);
  lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
  lv_obj_set_width(title, 260);
  lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
  /* 固定定位到modal顶部，居中 */
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 25);
}

/* Scan result dispatch: update AP list on LVGL main thread after background scan.
 * This runs on LVGL thread via lvgl_dispatch_async. The scan data is passed
 * through a dynamically allocated struct to avoid touching LVGL objects from
 * the background thread. */
typedef struct {
  int count;
  char ssids[WIFI_MAX_SCAN_RESULTS][WAPI_ESSID_MAX_SIZE + 1];
  int rssi[WIFI_MAX_SCAN_RESULTS];
  bool has_rssi[WIFI_MAX_SCAN_RESULTS];
  bool error;
  char error_msg[64];
} wifi_scan_data_t;

static void wifi_scan_result_dispatch_cb(void *arg)
{
  wifi_scan_data_t *data = (wifi_scan_data_t *)arg;
  lv_obj_clean(wifi_ap_list);

  if (!data) return;

  if (data->error) {
    wifi_list_add_text_label(data->error_msg);
    /* Restore scan button text */
    if (wifi_scan_btn) {
      lv_obj_t * lbl = lv_obj_get_child(wifi_scan_btn, 0);
      if (lbl) lv_label_set_text(lbl, "Scan WiFi Networks");
    }
    free(data);
    return;
  }

  for (int i = 0; i < data->count; i++) {
    create_wifi_ap_item(i, data->ssids[i], data->rssi[i], data->has_rssi[i]);
  }

  if (data->count == 0) {
    wifi_list_add_text_label("No AP found");
  }

  /* Restore scan button text */
  if (wifi_scan_btn) {
    lv_obj_t * lbl = lv_obj_get_child(wifi_scan_btn, 0);
    if (lbl) lv_label_set_text(lbl, "Scan WiFi Networks");
  }

  free(data);
}

/* Background scan thread: performs blocking I/O, then dispatches UI update */
static void *wifi_scan_thread(void *arg)
{
  LV_UNUSED(arg);

  wifi_scan_data_t *data = (wifi_scan_data_t *)calloc(1, sizeof(wifi_scan_data_t));
  if (!data) return NULL;

  if (!wifi_is_on) {
    free(data);
    return NULL;
  }

  int sock = wapi_make_socket();
  if (sock < 0) {
    data->error = true;
    snprintf(data->error_msg, sizeof(data->error_msg), "Scan failed: socket error");
    lvgl_dispatch_async(wifi_scan_result_dispatch_cb, data);
    return NULL;
  }

  wapi_set_mode(sock, WIFI_NETCARD, IW_MODE_INFRA);

  int ret = wapi_scan_init(sock, WIFI_NETCARD, NULL);
  if (ret < 0) {
    data->error = true;
    snprintf(data->error_msg, sizeof(data->error_msg), "Scan init failed");
    close(sock);
    lvgl_dispatch_async(wifi_scan_result_dispatch_cb, data);
    return NULL;
  }

  usleep(500000);
  int retry = 0;
  while (retry < 80) {
    int stat = wapi_scan_stat(sock, WIFI_NETCARD);
    if (stat == 0) break;
    if (stat < 0) {
      data->error = true;
      snprintf(data->error_msg, sizeof(data->error_msg), "Scan error");
      close(sock);
      lvgl_dispatch_async(wifi_scan_result_dispatch_cb, data);
      return NULL;
    }
    usleep(200000);
    retry++;
  }

  if (retry >= 80) {
    data->error = true;
    snprintf(data->error_msg, sizeof(data->error_msg), "Scan timeout");
    close(sock);
    lvgl_dispatch_async(wifi_scan_result_dispatch_cb, data);
    return NULL;
  }

  struct wapi_list_s scan_list;
  memset(&scan_list, 0, sizeof(scan_list));
  ret = wapi_scan_coll(sock, WIFI_NETCARD, &scan_list);
  if (ret < 0) {
    data->error = true;
    snprintf(data->error_msg, sizeof(data->error_msg), "Scan collect failed");
    close(sock);
    lvgl_dispatch_async(wifi_scan_result_dispatch_cb, data);
    return NULL;
  }

  int count = 0;
  struct wapi_scan_info_s *info = scan_list.head.scan;
  while (info && count < WIFI_MAX_SCAN_RESULTS) {
    if (info->has_essid && info->essid[0] != '\0') {
      if (strncmp(info->essid, "HQ_", 3) != 0) {
        strncpy(data->ssids[data->count], info->essid, WAPI_ESSID_MAX_SIZE);
        data->ssids[data->count][WAPI_ESSID_MAX_SIZE] = '\0';
        data->rssi[data->count] = info->rssi;
        data->has_rssi[data->count] = info->has_rssi;
        data->count++;
      }
    }
    info = info->next;
    count++;
  }

  wapi_scan_coll_free(&scan_list);
  close(sock);

  /* Dispatch UI update to LVGL main thread */
  lvgl_dispatch_async(wifi_scan_result_dispatch_cb, data);
  return NULL;
}

/* Scan button click handler - non-blocking: spawn background thread */
static void wifi_scan_btn_cb(lv_event_t * e)
{
  LV_UNUSED(e);

  lv_obj_clean(wifi_ap_list);
  wifi_list_add_text_label("Scanning...");

  /* Update scan button text */
  if (wifi_scan_btn) {
    lv_obj_t * lbl = lv_obj_get_child(wifi_scan_btn, 0);
    if (lbl) lv_label_set_text(lbl, "Scanning...");
  }

  /* Launch background thread for blocking scan I/O */
  pthread_t tid;
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, 4096);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  pthread_create(&tid, &attr, wifi_scan_thread, NULL);
  pthread_attr_destroy(&attr);
}

/* Back button click handler */
static void wifi_back_btn_cb(lv_event_t * e)
{
  LV_UNUSED(e);
  if (main_screen) {
    lv_scr_load_anim(main_screen, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 300, 0, false);
  }
}

/* Gesture event handler for wifi screen */
static void wifi_gesture_cb(lv_event_t * e)
{
  LV_UNUSED(e);
  lv_indev_t *indev = lv_indev_get_act();
  lv_dir_t dir = lv_indev_get_gesture_dir(indev);
  if (dir == LV_DIR_RIGHT) {
    /* 右滑返回设置页（设置页是 wifi_page 的父级）
     * 等待手指释放后再处理，防止动画期间手指抬起的 CLICKED 事件
     * 误触发设置页按钮。比 lv_indev_reset 副作用小，不会干扰屏幕加载动画。 */
    lv_indev_wait_release(indev);
    if (settings_screen == NULL)
      {
        settings_screen = lv_obj_create(NULL);
        create_settings_page(settings_screen);
      }
    lv_scr_load_anim(settings_screen, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 300, 0, false);
  }
}

static void wifi_deinit_async_cb(void * arg);

static void wifi_screen_unloaded_cb(lv_event_t * e)
{
  LV_UNUSED(e);
  LV_LOG_USER("wifi screen unloaded, deinitializing...");
  lvgl_dispatch_async(wifi_deinit_async_cb, NULL);
}

/* WiFi status refresh timer */
static void wifi_status_timer_cb(lv_timer_t * timer)
{
  LV_UNUSED(timer);
  if (wifi_is_on) {
    wifi_update_status();
  }
}

/* ========================================================================
 * WiFi config save / load / auto-connect
 * ===================================================================== */

/* Save WiFi credentials to /emmc/wifi/wifi_config.txt.
 * Format: SSID=PASSWORD per line. New entry inserted at top (most recent first).
 * Same SSID overwrites with new password and moves to top. Max WIFI_MAX_SAVED entries. */
void wifi_save_config(const char *ssid, const char *password)
{
  if (!ssid || !password || ssid[0] == '\0') return;

  /* Ensure directory exists */
  mkdir(WIFI_CONFIG_DIR, 0777);

  /* Read existing entries, skipping the one with the same SSID */
  wifi_credential_t creds[WIFI_MAX_SAVED];
  int ncount = 0;
  FILE *fr = fopen(WIFI_CONFIG_FILE, "r");
  if (fr) {
    char line[256];
    while (fgets(line, sizeof(line), fr) && ncount < WIFI_MAX_SAVED - 1) {
      char *eq = strchr(line, '=');
      if (!eq) continue;
      *eq = '\0';
      /* Skip if same SSID (will be re-added at top with new password) */
      if (strcmp(line, ssid) == 0) continue;
      strlcpy(creds[ncount].ssid, line, sizeof(creds[0].ssid));
      strlcpy(creds[ncount].password, eq + 1, sizeof(creds[0].password));
      /* Strip trailing newline from password */
      char *nl = strchr(creds[ncount].password, '\n');
      if (nl) *nl = '\0';
      ncount++;
    }
    fclose(fr);
  }

  /* Shift existing entries down, insert new entry at top (index 0) */
  if (ncount >= WIFI_MAX_SAVED - 1)
    ncount = WIFI_MAX_SAVED - 1;  /* Cap to leave room for new entry */
  for (int i = ncount; i > 0; i--) {
    creds[i] = creds[i - 1];
  }
  strlcpy(creds[0].ssid, ssid, sizeof(creds[0].ssid));
  strlcpy(creds[0].password, password, sizeof(creds[0].password));
  ncount++;

  /* Write all entries back */
  FILE *fw = fopen(WIFI_CONFIG_FILE, "w");
  if (fw) {
    for (int i = 0; i < ncount; i++) {
      fprintf(fw, "%s=%s\n", creds[i].ssid, creds[i].password);
    }
    fclose(fw);
    syslog(LOG_INFO, "[wifi_page] Saved WiFi credential: %s\n", ssid);
  } else {
    syslog(LOG_ERR, "[wifi_page] Failed to write WiFi config file\n");
  }
}

/* Load saved WiFi credentials. Returns count, fills creds array. */
static int wifi_load_configs(wifi_credential_t *creds, int max)
{
  int ncount = 0;
  FILE *fr = fopen(WIFI_CONFIG_FILE, "r");
  if (!fr) return 0;

  char line[256];
  while (fgets(line, sizeof(line), fr) && ncount < max) {
    char *eq = strchr(line, '=');
    if (!eq) continue;
    *eq = '\0';
    strlcpy(creds[ncount].ssid, line, sizeof(creds[0].ssid));
    strlcpy(creds[ncount].password, eq + 1, sizeof(creds[0].password));
    /* Strip trailing newline */
    char *nl = strchr(creds[ncount].password, '\n');
    if (nl) *nl = '\0';
    ncount++;
  }
  fclose(fr);
  return ncount;
}

/* Background auto-connect thread.
 * Loads saved credentials, scans every 20s, auto-connects if a saved
 * SSID is found and WiFi is not yet connected. */
static bool wifi_auto_connect_running = false;
static pthread_t wifi_auto_connect_thread_id;

static void *wifi_auto_connect_thread(void *arg)
{
  LV_UNUSED(arg);

  syslog(LOG_INFO, "[wifi_page] Auto-connect thread started\n");

  /* Initial delay to let system finish boot */
  sleep(3);

  /* Consecutive scan failure counter. When the driver appears stuck
   * (scan completes but reports 0 APs, or scan never completes), we
   * reset the interface via ifdown/ifup after WIFI_IF_RESET_FAIL_COUNT
   * consecutive failures. Connection successes reset the counter. */
  #define WIFI_IF_RESET_FAIL_COUNT  10
  int fail_count = 0;

  while (wifi_auto_connect_running) {
    /* Check if already connected */
    if (wifi_get_if_flags(WIFI_NETCARD) & IFF_RUNNING) {
      fail_count = 0;
      sleep(WIFI_AUTO_SCAN_INTERVAL_SEC);
      continue;
    }

    /* Reload credentials each iteration — user may have saved new networks */
    wifi_credential_t creds[WIFI_MAX_SAVED];
    int ncount = wifi_load_configs(creds, WIFI_MAX_SAVED);
    if (ncount == 0) {
      sleep(WIFI_AUTO_SCAN_INTERVAL_SEC);
      continue;
    }

    /* Reset the WiFi driver when consecutive failures accumulate. A stuck
     * driver (e.g. after beacon loss overnight) won't recover from a plain
     * ifup on an already-up interface. Use bwifi_reset() to force a full
     * re-initialization of the RWNX firmware/mac state AND restart the WPA
     * supplicant task. netlib_ifdown/ifup only toggles IFF_UP and leaves
     * the WPA task exited, causing permanent reconnection failure. */
    if (fail_count >= WIFI_IF_RESET_FAIL_COUNT) {
      syslog(LOG_WARNING,
             "[wifi_page] %d consecutive scan failures, resetting WiFi driver\n",
             fail_count);
      int reset_ret = bwifi_reset();
      if (reset_ret == 0) {
        syslog(LOG_INFO,
               "[wifi_page] bwifi_reset success, WPA task restarted\n");
      } else {
        syslog(LOG_WARNING,
               "[wifi_page] bwifi_reset failed (ret=%d)\n", reset_ret);
      }
      /* Allow WPA task and firmware to fully initialize before scanning */
      sleep(2);
      fail_count = 0;
    } else {
      /* Bring up interface (no-op if already up) */
      netlib_ifup(WIFI_NETCARD);
    }

    /* Scan */
    int sock = wapi_make_socket();
    if (sock < 0) {
      fail_count++;
      sleep(WIFI_AUTO_SCAN_INTERVAL_SEC);
      continue;
    }

    wapi_set_mode(sock, WIFI_NETCARD, IW_MODE_INFRA);
    int ret = wapi_scan_init(sock, WIFI_NETCARD, NULL);
    if (ret < 0) {
      close(sock);
      fail_count++;
      sleep(WIFI_AUTO_SCAN_INTERVAL_SEC);
      continue;
    }

    /* Wait for scan to complete */
    usleep(500000);
    int retry = 0;
    while (retry < 80) {
      int stat = wapi_scan_stat(sock, WIFI_NETCARD);
      if (stat == 0) break;
      if (stat < 0) break;
      usleep(200000);
      retry++;
    }

    if (retry >= 80) {
      close(sock);
      fail_count++;
      sleep(WIFI_AUTO_SCAN_INTERVAL_SEC);
      continue;
    }

    /* Collect scan results */
    struct wapi_list_s scan_list;
    memset(&scan_list, 0, sizeof(scan_list));
    ret = wapi_scan_coll(sock, WIFI_NETCARD, &scan_list);
    if (ret < 0) {
      close(sock);
      fail_count++;
      sleep(WIFI_AUTO_SCAN_INTERVAL_SEC);
      continue;
    }

    /* Find a matching saved SSID */
    const char *matched_ssid = NULL;
    const char *matched_pwd = NULL;
    struct wapi_scan_info_s *info = scan_list.head.scan;
    int ap_count = 0;
    while (info) {
      ap_count++;
      if (info->has_essid && info->essid[0] != '\0') {
        for (int i = 0; i < ncount; i++) {
          if (strcmp(info->essid, creds[i].ssid) == 0) {
            matched_ssid = creds[i].ssid;
            matched_pwd = creds[i].password;
            break;
          }
        }
        if (matched_ssid) break;
      }
      info = info->next;
    }

    /* If scan returned 0 APs the driver is likely stuck — count as failure
     * so the interface eventually gets reset. Also count when no saved
     * SSID matched (normal case, but still a "no connection" outcome). */
    if (ap_count == 0) {
      syslog(LOG_WARNING, "[wifi_page] scan returned 0 APs (fail_count=%d)\n",
             fail_count + 1);
      fail_count++;
      wapi_scan_coll_free(&scan_list);
      close(sock);
      sleep(WIFI_AUTO_SCAN_INTERVAL_SEC);
      continue;
    }

    if (matched_ssid && matched_pwd) {
      syslog(LOG_INFO, "[wifi_page] Auto-connecting to: %s\n", matched_ssid);

      /* Set WPA2 */
      wpa_driver_wext_set_auth_param(sock, WIFI_NETCARD,
        IW_AUTH_WPA_VERSION, IW_AUTH_WPA_VERSION_WPA2);
      wpa_driver_wext_set_auth_param(sock, WIFI_NETCARD,
        IW_AUTH_CIPHER_PAIRWISE, IW_AUTH_CIPHER_CCMP);

      int passlen = strlen(matched_pwd);
      if (passlen >= 8 && passlen <= 63) {
        wpa_driver_wext_set_key_ext(sock, WIFI_NETCARD, WPA_ALG_CCMP,
                                    matched_pwd, passlen);
      }

      wapi_set_essid(sock, WIFI_NETCARD, matched_ssid, WAPI_ESSID_ON);

      /* Wait for connection */
      int loop_cnt = 0;
      while (!(wifi_get_if_flags(WIFI_NETCARD) & IFF_RUNNING) && loop_cnt < 60) {
        loop_cnt++;
        usleep(50000);
      }

      if (wifi_get_if_flags(WIFI_NETCARD) & IFF_RUNNING) {
        /* Start persistent DHCP renewal (handles re-connect safely:
         * closes any stale handle from a previous session first). */
        wifi_dhcpc_start();
        /* Update static state so WiFi page shows correct status */
        strncpy(wifi_selected_ssid, matched_ssid, sizeof(wifi_selected_ssid) - 1);
        wifi_selected_ssid[sizeof(wifi_selected_ssid) - 1] = '\0';
        wifi_is_on = true;
        wifi_is_connected = true;
        fail_count = 0;
        syslog(LOG_INFO, "[wifi_page] Auto-connected to: %s\n", matched_ssid);
      } else {
        syslog(LOG_WARNING, "[wifi_page] Auto-connect failed for: %s\n", matched_ssid);
        fail_count++;
      }
    }

    wapi_scan_coll_free(&scan_list);
    close(sock);

    sleep(WIFI_AUTO_SCAN_INTERVAL_SEC);
  }

  return NULL;
}

void wifi_auto_connect_start(void)
{
  if (wifi_auto_connect_running) return;
  wifi_auto_connect_running = true;

  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, 8192);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

  if (pthread_create(&wifi_auto_connect_thread_id, &attr,
                     wifi_auto_connect_thread, NULL) != 0) {
    syslog(LOG_ERR, "[wifi_page] Failed to create auto-connect thread\n");
    wifi_auto_connect_running = false;
  }

  pthread_attr_destroy(&attr);
}

#endif /* CONFIG_WIRELESS_WAPI */

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/* Create WiFi settings page */
void create_wifi_page(lv_obj_t * parent)
{
#ifdef CONFIG_WIRELESS_WAPI
  /* 深色背景（参考 recorder_page 风格）*/
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

  /* Title bar：透明背景（参考 recorder_page）*/
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

  /* Back button：透明背景（参考 recorder_page）*/
  lv_obj_t * back_btn = lv_button_create(title_bar);
  lv_obj_set_size(back_btn, 30, 30);
  lv_obj_align(back_btn, LV_ALIGN_LEFT_MID, 3, 0);
  lv_obj_set_style_bg_opa(back_btn, LV_OPA_TRANSP, 0);
  lv_obj_set_style_shadow_opa(back_btn, LV_OPA_TRANSP, 0);
  lv_obj_t * back_label = lv_label_create(back_btn);
  lv_label_set_text(back_label, LV_SYMBOL_LEFT);
  lv_obj_set_style_text_color(back_label, lv_color_hex(0xFFFFFF), 0);
  lv_obj_center(back_label);
  lv_obj_add_event_cb(back_btn, wifi_back_btn_cb, LV_EVENT_CLICKED, NULL);

  /* Title text：montserrat_24 白色（参考 recorder_page）*/
  lv_obj_t * title = lv_label_create(title_bar);
  lv_label_set_text(title, "WiFi");
  lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
  lv_obj_align(title, LV_ALIGN_CENTER, 0, 0);

  /* Status area：透明背景容器，SSID（蓝色）+ IP（浅灰），montserrat_24
   * 右移 50px 避开圆形屏左侧不可见区 */
  lv_obj_t * status_cont = lv_obj_create(parent);
  lv_obj_remove_style_all(status_cont);
  lv_obj_set_size(status_cont, LV_HOR_RES - 2 * WIFI_LIST_PAD_LR, LV_SIZE_CONTENT);
  lv_obj_align(status_cont, LV_ALIGN_TOP_LEFT, WIFI_LIST_PAD_LR + 40, 42);
  lv_obj_set_style_pad_left(status_cont, 8, 0);
  lv_obj_set_style_pad_right(status_cont, 8, 0);
  lv_obj_set_style_pad_top(status_cont, 6, 0);
  lv_obj_set_style_pad_bottom(status_cont, 6, 0);
  lv_obj_set_style_bg_opa(status_cont, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(status_cont, 0, 0);
  lv_obj_clear_flag(status_cont, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_clear_flag(status_cont, LV_OBJ_FLAG_CLICKABLE);

  /* Line 1: SSID name (blue when connected) */
  wifi_status_label = lv_label_create(status_cont);
  lv_label_set_text(wifi_status_label, "WiFi: OFF");
  lv_obj_set_style_text_font(wifi_status_label, &lv_font_montserrat_24, 0);
  lv_obj_set_style_text_color(wifi_status_label, lv_color_hex(0xCCCCCC), 0);
  lv_obj_align(wifi_status_label, LV_ALIGN_TOP_LEFT, 0, 0);
  lv_label_set_long_mode(wifi_status_label, LV_LABEL_LONG_SCROLL_CIRCULAR);
  lv_obj_set_width(wifi_status_label, LV_PCT(100));

  /* Line 2: Connected + IP (gray) */
  wifi_ip_label = lv_label_create(status_cont);
  lv_label_set_text(wifi_ip_label, "");
  lv_obj_set_style_text_font(wifi_ip_label, &lv_font_montserrat_24, 0);
  lv_obj_set_style_text_color(wifi_ip_label, lv_color_hex(0xCCCCCC), 0);
  lv_obj_align(wifi_ip_label, LV_ALIGN_TOP_LEFT, 0, 28);
  lv_label_set_long_mode(wifi_ip_label, LV_LABEL_LONG_SCROLL_CIRCULAR);
  lv_obj_set_width(wifi_ip_label, LV_PCT(100));

  /* Scan button：蓝色强调，montserrat_24 白色文字
   * Y=125，宽度缩短60px */
  wifi_scan_btn = lv_button_create(parent);
  lv_obj_set_size(wifi_scan_btn, LV_HOR_RES - 2 * WIFI_LIST_PAD_LR - 60, 36);
  lv_obj_align(wifi_scan_btn, LV_ALIGN_TOP_LEFT, WIFI_LIST_PAD_LR, 115);
  lv_obj_set_style_bg_color(wifi_scan_btn, lv_color_hex(0x0088FF), 0);
  lv_obj_set_style_radius(wifi_scan_btn, 8, 0);
  lv_obj_t * scan_label = lv_label_create(wifi_scan_btn);
  lv_label_set_text(scan_label, "Scan WiFi Networks");
  lv_obj_set_style_text_color(scan_label, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_text_font(scan_label, &lv_font_montserrat_24, 0);
  lv_obj_center(scan_label);
  lv_obj_add_event_cb(wifi_scan_btn, wifi_scan_btn_cb, LV_EVENT_CLICKED, NULL);

  /* AP list：透明滚动容器（参考 recorder_page recorder_file_list 模式）
   * - 无背景色、无边框、无圆角
   * - 列表项按 create_wifi_ap_item 模式逐个添加（图标+SSID换行）
   * - 左右各预留 50px，底部预留 100px（圆形屏适配）
   * - Y=165，与 scan 按钮保持间距 */
  wifi_ap_list = lv_obj_create(parent);
  lv_obj_remove_style_all(wifi_ap_list);
  lv_obj_set_size(wifi_ap_list, LV_HOR_RES - 2 * WIFI_LIST_PAD_LR,
                  LV_VER_RES - 155 - WIFI_LIST_PAD_BOT);
  lv_obj_align(wifi_ap_list, LV_ALIGN_TOP_LEFT, WIFI_LIST_PAD_LR, 155);
  lv_obj_set_style_bg_opa(wifi_ap_list, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(wifi_ap_list, 0, 0);
  lv_obj_set_style_radius(wifi_ap_list, 0, 0);
  lv_obj_set_style_pad_all(wifi_ap_list, 0, 0);
  lv_obj_set_scroll_dir(wifi_ap_list, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(wifi_ap_list, LV_SCROLLBAR_MODE_OFF);
  lv_obj_clear_flag(wifi_ap_list, LV_OBJ_FLAG_SCROLL_CHAIN);
  lv_obj_clear_flag(wifi_ap_list, LV_OBJ_FLAG_SCROLL_ELASTIC);
  lv_obj_clear_flag(wifi_ap_list, LV_OBJ_FLAG_SCROLL_MOMENTUM);
  lv_obj_add_flag(wifi_ap_list, LV_OBJ_FLAG_GESTURE_BUBBLE);
  /* No placeholder text - the scan button already tells user what to do */

  /* Add gesture handler - do NOT add GESTURE_BUBBLE to screen,
   * it must be the terminal target for gesture bubbling from children */
  lv_obj_add_event_cb(parent, wifi_gesture_cb, LV_EVENT_GESTURE, NULL);

  lv_obj_add_event_cb(parent, wifi_screen_unloaded_cb,
      LV_EVENT_SCREEN_UNLOADED, NULL);

  /* WiFi 默认开启：拉起接口 + 更新状态 */
  netlib_ifup(WIFI_NETCARD);
  wifi_update_status();

  /* Start status refresh timer */
  wifi_status_timer = lv_timer_create(wifi_status_timer_cb, 3000, NULL);

  /* WiFi auto-connect is now started at boot in lvgldemo.c main(),
   * no longer tied to WiFi page creation. */
#else
  lv_obj_t * no_wifi_label = lv_label_create(parent);
  lv_label_set_text(no_wifi_label, "WiFi not supported\n(CONFIG_WIRELESS_WAPI not enabled)");
  lv_obj_center(no_wifi_label);
#endif /* CONFIG_WIRELESS_WAPI */
}

void wifi_page_deinit(void)
{
#ifdef CONFIG_WIRELESS_WAPI
  if (wifi_status_timer)
    {
      lv_timer_del(wifi_status_timer);
      wifi_status_timer = NULL;
    }

  wifi_ap_list = NULL;
  wifi_status_label = NULL;
  wifi_ip_label = NULL;
  wifi_scan_btn = NULL;
  wifi_pwd_textarea = NULL;
  wifi_pwd_kb = NULL;
  wifi_pwd_modal = NULL;
  wifi_pwd_eye_btn = NULL;
  wifi_connecting_ssid[0] = '\0';
#endif

  if (wifi_screen)
    {
      lv_obj_del(wifi_screen);
      wifi_screen = NULL;
    }

  LV_LOG_USER("wifi page deinit complete");
}

static void wifi_deinit_async_cb(void * arg)
{
  LV_UNUSED(arg);
  wifi_page_deinit();
}
