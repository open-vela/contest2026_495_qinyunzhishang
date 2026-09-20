/****************************************************************************
 * apps/examples/lvgldemo/settings_page.c
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

/* 设置页面：功能菜单页的子页面，提供系统设置入口。
 * 当前仅包含 WiFi 项。导航：
 *   - 右滑 → 返回功能菜单页
 *   - 点击 WiFi → 进入 wifi_page
 *   - wifi_page 右滑 → 返回设置页
 */

#include <nuttx/config.h>
#include <syslog.h>
#include <time.h>
#include <lvgl/lvgl.h>

#include "settings_page.h"
#include "lvgldemo_common.h"
#include "wifi_page.h"
#include "menu_page.h"
#include "lvgl_dispatch.h"
#include "agent_config.h"
#include "i18n.h"
#include "lang_dialog.h"

/* 设置项图标（32x32 RGB565 嵌入式位图，由 icons/*.c 提供） */
LV_IMAGE_DECLARE(setting_wifi);
LV_IMAGE_DECLARE(setting_language);

/* UI 布局参数（与 menu_page.c 一致） */
#define SETTINGS_BTN_HEIGHT     60
#define SETTINGS_BTN_GAP        8
#define SETTINGS_START_Y        5
#define SETTINGS_TITLE_HEIGHT   48
#define SETTINGS_SCROLL_TOP     SETTINGS_TITLE_HEIGHT
#define SETTINGS_BOTTOM_MARGIN  50

/* 颜色定义（深色主题，与 menu_page 一致） */
#define COLOR_TEXT_WHITE    0xFFFFFF
#define COLOR_BTN_NORMAL    0x000000
#define COLOR_BTN_PRESSED   0x222222

static lv_obj_t *settings_time_label = NULL;
static lv_timer_t *settings_time_timer = NULL;

/* ===== 页面跳转函数 ===== */

static void settings_enter_wifi_page(void)
{
    if (wifi_screen == NULL) {
        wifi_screen = lv_obj_create(NULL);
        create_wifi_page(wifi_screen);
    }
    lv_scr_load_anim(wifi_screen, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 300, 0, false);
}

static void settings_back_to_menu(void)
{
    /* 语言切换后 menu_screen 可能已被清理（menu_page_force_delete），
     * 此时右滑回菜单需要重建 menu_screen，否则无法返回 */
    if (!menu_screen) {
        menu_screen = lv_obj_create(NULL);
        lv_obj_set_style_bg_color(menu_screen, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(menu_screen, LV_OPA_COVER, 0);
        lv_obj_clear_flag(menu_screen, LV_OBJ_FLAG_SCROLLABLE);
        create_menu_page(menu_screen);
    }
    lv_scr_load_anim(menu_screen, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 300, 0, false);
}

/* ===== 按钮回调 ===== */

static void btn_wifi_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    settings_enter_wifi_page();
}

/* 语言项回调：进入语言设置页面 */
static void btn_language_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    lang_page_enter();
}

static void back_btn_click_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    settings_back_to_menu();
}

/* 屏幕级手势回调：右滑返回功能菜单页 */
static void settings_parent_gesture_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    lv_indev_t *indev = lv_indev_get_act();
    lv_dir_t dir = lv_indev_get_gesture_dir(indev);
    if (dir == LV_DIR_RIGHT) {
        syslog(LOG_INFO, "[settings] Swipe right -> back to menu\n");
        /* 等待手指释放后再处理，防止动画期间手指抬起的 CLICKED 事件
         * 误触发菜单页按钮。比 lv_indev_reset 副作用小，不会干扰屏幕加载动画。 */
        lv_indev_wait_release(indev);
        settings_back_to_menu();
    }
}

/* ===== 时间显示 ===== */

static void settings_update_time(void)
{
    struct tm time_info;
    char buf[16];

    time_info = agent_localtime();
    snprintf(buf, sizeof(buf), "%02d:%02d",
             time_info.tm_hour, time_info.tm_min);

    if (settings_time_label) {
        lv_label_set_text(settings_time_label, buf);
    }
}

static void settings_time_timer_cb(lv_timer_t *timer)
{
    LV_UNUSED(timer);
    settings_update_time();
}

static void settings_screen_load_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    settings_update_time();
}

/* ===== 创建单个设置项按钮（与 menu_page.c 风格一致）===== */
static void create_settings_item(lv_obj_t *parent, int index,
                                  const lv_image_dsc_t *icon, const char *text,
                                  lv_event_cb_t cb)
{
    /* 圆形屏适配：按钮宽度缩短50px并整体右移50px */
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, LV_HOR_RES * 80 / 100 - 50, SETTINGS_BTN_HEIGHT);
    lv_obj_align(btn, LV_ALIGN_TOP_MID, 50,
                 SETTINGS_START_Y + index * (SETTINGS_BTN_HEIGHT + SETTINGS_BTN_GAP));

    lv_obj_set_style_bg_color(btn, lv_color_hex(COLOR_BTN_NORMAL), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn, 10, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_shadow_opa(btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(COLOR_BTN_PRESSED), LV_STATE_PRESSED);

    /* 内容容器（移除 CLICKABLE 避免拦截按钮点击） */
    lv_obj_t *container = lv_obj_create(btn);
    lv_obj_remove_style_all(container);
    lv_obj_set_size(container, lv_pct(100), lv_pct(100));
    lv_obj_clear_flag(container, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_pad_all(container, 12, 0);
    lv_obj_set_style_pad_left(container, 15, 0);
    lv_obj_set_style_pad_right(container, 15, 0);
    lv_obj_set_style_border_width(container, 0, 0);
    lv_obj_set_style_bg_opa(container, LV_OPA_TRANSP, 0);

    /* 左侧图标（32x32 嵌入式位图，替代原 LV_SYMBOL 文字图标） */
    lv_obj_t *icon_img = lv_image_create(container);
    lv_image_set_src(icon_img, icon);
    lv_obj_align(icon_img, LV_ALIGN_LEFT_MID, 0, 0);

    /* 右侧文字标签（距图标右侧 20px） */
    lv_obj_t *text_label = lv_label_create(container);
    lv_obj_remove_style_all(text_label);
    lv_label_set_text(text_label, text);
    i18n_apply_font(text_label);
    lv_obj_set_style_text_color(text_label, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_align(text_label, LV_ALIGN_LEFT_MID, 32 + 20, 0);

    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
}

/* ===== 页面生命周期 ===== */

static void settings_deinit_async_cb(void *arg);

static void settings_screen_unloaded_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    LV_LOG_USER("settings screen unloaded, deinitializing...");
    lv_indev_reset(NULL, NULL);
    lvgl_dispatch_async(settings_deinit_async_cb, NULL);
}

void settings_page_deinit(void)
{
    if (settings_time_timer) {
        lv_timer_del(settings_time_timer);
        settings_time_timer = NULL;
    }

    settings_time_label = NULL;

    if (settings_screen) {
        lv_obj_del(settings_screen);
        settings_screen = NULL;
    }

    LV_LOG_USER("settings page deinit complete");
}

static void settings_deinit_async_cb(void *arg)
{
    LV_UNUSED(arg);
    settings_page_deinit();
}

/* ===== 主入口：创建设置页面 ===== */
void create_settings_page(lv_obj_t *parent)
{
    /* 深色背景 */
    lv_obj_set_style_bg_color(parent, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(parent, 0, 0);
    lv_obj_set_style_border_width(parent, 0, 0);
    lv_obj_clear_flag(parent, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(parent, LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_clear_flag(parent, LV_OBJ_FLAG_SCROLL_MOMENTUM);

    /* 顶部标题栏（固定不滚动） */
    lv_obj_t *title_bar = lv_obj_create(parent);
    lv_obj_remove_style_all(title_bar);
    lv_obj_set_size(title_bar, lv_pct(100), SETTINGS_TITLE_HEIGHT);
    lv_obj_align(title_bar, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_opa(title_bar, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(title_bar, 0, 0);
    lv_obj_clear_flag(title_bar, LV_OBJ_FLAG_SCROLLABLE);
    /* 不设置 GESTURE_BUBBLE：手势回调已同时在 title_bar 和 parent 上注册，
     * 若再冒泡会导致 settings_parent_gesture_cb 被触发两次，
     * settings_back_to_menu() 重复调用 lv_scr_load_anim 会打断屏幕加载动画，
     * 导致右滑后黑屏。 */
    lv_obj_add_event_cb(title_bar, settings_parent_gesture_cb, LV_EVENT_GESTURE, NULL);

    /* 返回按钮（左上角，点击返回功能菜单页） */
    lv_obj_t *back_btn = lv_btn_create(title_bar);
    lv_obj_remove_style_all(back_btn);
    lv_obj_set_size(back_btn, 36, 36);
    lv_obj_align(back_btn, LV_ALIGN_LEFT_MID, 10, 0);
    lv_obj_set_style_bg_opa(back_btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_radius(back_btn, 18, 0);

    lv_obj_t *back_icon = lv_label_create(back_btn);
    lv_obj_remove_style_all(back_icon);
    lv_label_set_text(back_icon, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(back_icon, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_set_style_text_font(back_icon, LV_FONT_DEFAULT, 0);
    lv_obj_center(back_icon);
    lv_obj_add_event_cb(back_btn, back_btn_click_cb, LV_EVENT_CLICKED, NULL);

    /* 标题栏时间显示 */
    settings_time_label = lv_label_create(title_bar);
    lv_obj_remove_style_all(settings_time_label);
    lv_label_set_text(settings_time_label, "");
    i18n_apply_font(settings_time_label);
    lv_obj_set_style_text_color(settings_time_label, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_align(settings_time_label, LV_ALIGN_CENTER, 0, 0);

    /* 滚动容器 */
    lv_obj_t *scroll_cont = lv_obj_create(parent);
    lv_obj_remove_style_all(scroll_cont);
    lv_obj_set_size(scroll_cont, lv_pct(100),
                    LV_VER_RES - SETTINGS_TITLE_HEIGHT - SETTINGS_BOTTOM_MARGIN);
    lv_obj_set_pos(scroll_cont, 0, SETTINGS_SCROLL_TOP);
    lv_obj_set_style_bg_opa(scroll_cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(scroll_cont, 0, 0);
    lv_obj_set_style_pad_all(scroll_cont, 0, 0);
    lv_obj_set_scroll_dir(scroll_cont, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(scroll_cont, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(scroll_cont, LV_OBJ_FLAG_CLICKABLE);

    /* 设置项列表：WiFi + 语言 */
    create_settings_item(scroll_cont, 0, &setting_wifi,
                         i18n_get(STR_SETTINGS_WIFI), btn_wifi_cb);
    create_settings_item(scroll_cont, 1, &setting_language,
                         i18n_get(STR_SETTINGS_LANGUAGE), btn_language_cb);

    /* 进入界面时立即刷新时间 */
    lv_obj_add_event_cb(parent, settings_screen_load_cb, LV_EVENT_SCREEN_LOADED, NULL);

    /* 屏幕级手势：右滑返回功能菜单页 */
    lv_obj_add_event_cb(parent, settings_parent_gesture_cb, LV_EVENT_GESTURE, NULL);

    lv_obj_add_event_cb(parent, settings_screen_unloaded_cb,
                        LV_EVENT_SCREEN_UNLOADED, NULL);

    /* 启动时间更新定时器（每60秒刷新一次） */
    settings_update_time();
    settings_time_timer = lv_timer_create(settings_time_timer_cb, 60000, NULL);
}
