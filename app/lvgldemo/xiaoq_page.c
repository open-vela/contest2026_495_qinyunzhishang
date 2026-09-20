/****************************************************************************
 * apps/examples/lvgldemo/xiaoq_page.c
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
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <syslog.h>
#include <lvgl/lvgl.h>

#include "xiaoq_page.h"
#include "lvgldemo_common.h"
#include "call_page.h"
#include "agent_config.h"
#include "i18n.h"

/* 壁纸轮播配置（eMMC 动态加载，零 Flash 占用） */
#define WALLPAPER_COUNT      4
#define WALLPAPER_SWITCH_MS  30000  /* 30秒切换一次 */
#define WALLPAPER_DATA_SIZE  (454 * 454 * 2)  /* RGB565: 402KB per image */

static const char *wallpaper_paths[WALLPAPER_COUNT] = {
    "/emmc/wallpapers/wallpaper.bin",       /* 星空 */
    "/emmc/wallpapers/wallpaper2.bin",      /* 赛博朋克 */
    "/emmc/wallpapers/wallpaper3.bin",      /* 萌宠 */
    "/emmc/wallpapers/wallpaper4.bin",      /* 华勤技术 */
};

static int current_wallpaper_idx = 0;
static lv_obj_t *bg_img_obj = NULL;
static lv_timer_t *wallpaper_timer = NULL;
static uint8_t *current_wallpaper_data = NULL;  /* 当前加载的图片数据缓冲区 */

/* 底部上滑引导图标（提示上滑进入功能菜单） */
LV_IMAGE_DECLARE(up_up);

/****************************************************************************
 * 配置宏定义
 ****************************************************************************/

/* 字体路径 */
#define HOMEPAGE_FONT_PATH    "/emmc/font/MiSans-Normal.ttf"
#define HOMEPAGE_DATE_FONT_SIZE   39
#define HOMEPAGE_DATE_FONT_SIZE_EN 48   /* 英文日期字号（调大） */
#define HOMEPAGE_TIME_FONT_SIZE   120
#define HOMEPAGE_TIME_FONT_SIZE_EN 100  /* 英文时间字号（由80调大） */
#define HOMEPAGE_WEEKDAY_FONT_SIZE 39

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* 日期时间标签 */
static lv_obj_t *date_label = NULL;
static lv_obj_t *time_label = NULL;
static lv_obj_t *weekday_label = NULL;
static lv_font_t *date_font = NULL;
static lv_font_t *time_font = NULL;
static lv_font_t *weekday_font = NULL;
static lv_timer_t *time_update_timer = NULL;

/****************************************************************************
 * 字体加载（按语言加载，切语言时先删后建）
 ****************************************************************************/

/* 前向声明（homepage_lang_changed_cb 引用 homepage_update_datetime） */
static void homepage_update_datetime(void);

/* 壁纸轮播回调前向声明（create_xiaoq_page 引用 wallpaper_switch_cb，
 * wallpaper_switch_cb 引用 wallpaper_fade_in_ready_cb） */
static bool load_wallpaper_from_emmc(int idx);  /* eMMC加载函数 */
static void wallpaper_switch_cb(lv_timer_t *timer);
static void wallpaper_fade_in_ready_cb(lv_anim_t *a);

/* 按当前语言加载字体。中英文均用 MiSans FreeType（含拉丁字符），
 * 时间字号：中文 120px，英文 100px；日期字号：中文 39px，英文 48px；
 * 星期两者同 39px */
static void homepage_font_load(lang_t lang)
{
#if LV_USE_FREETYPE
    int time_size = (lang == LANG_EN) ? HOMEPAGE_TIME_FONT_SIZE_EN : HOMEPAGE_TIME_FONT_SIZE;
    int date_size = (lang == LANG_EN) ? HOMEPAGE_DATE_FONT_SIZE_EN : HOMEPAGE_DATE_FONT_SIZE;

    if (date_font == NULL) {
        date_font = lv_freetype_font_create(HOMEPAGE_FONT_PATH,
            LV_FREETYPE_FONT_RENDER_MODE_BITMAP, date_size,
            LV_FREETYPE_FONT_STYLE_NORMAL);
        if (!date_font) {
            syslog(LOG_WARNING, "[homepage] date font load failed: %s\n",
                   HOMEPAGE_FONT_PATH);
        }
    }
    if (time_font == NULL) {
        time_font = lv_freetype_font_create(HOMEPAGE_FONT_PATH,
            LV_FREETYPE_FONT_RENDER_MODE_BITMAP, time_size,
            LV_FREETYPE_FONT_STYLE_NORMAL);
        if (!time_font) {
            syslog(LOG_WARNING, "[homepage] time font load failed: %s\n",
                   HOMEPAGE_FONT_PATH);
        }
    }
    if (weekday_font == NULL) {
        weekday_font = lv_freetype_font_create(HOMEPAGE_FONT_PATH,
            LV_FREETYPE_FONT_RENDER_MODE_BITMAP, HOMEPAGE_WEEKDAY_FONT_SIZE,
            LV_FREETYPE_FONT_STYLE_NORMAL);
        if (!weekday_font) {
            syslog(LOG_WARNING, "[homepage] weekday font load failed: %s\n",
                   HOMEPAGE_FONT_PATH);
        }
    }
#else
    (void)lang;
#endif
}

/* 释放所有 FreeType 字体（切语言前调用） */
static void homepage_font_unload(void)
{
#if LV_USE_FREETYPE
    if (date_font) {
        lv_freetype_font_delete(date_font);
        date_font = NULL;
    }
    if (time_font) {
        lv_freetype_font_delete(time_font);
        time_font = NULL;
    }
    if (weekday_font) {
        lv_freetype_font_delete(weekday_font);
        weekday_font = NULL;
    }
#endif
}

/* 语言变更回调：原地刷新主页字体和文字（方案 C） */
static void homepage_lang_changed_cb(lang_t lang)
{
    /* 1. 先把 label 字体设为安全值（montserrat_24），避免悬空指针 */
    if (time_label) {
        lv_obj_set_style_text_font(time_label, &lv_font_montserrat_24, 0);
    }
    if (date_label) {
        lv_obj_set_style_text_font(date_label, &lv_font_montserrat_24, 0);
    }
    if (weekday_label) {
        lv_obj_set_style_text_font(weekday_label, &lv_font_montserrat_24, 0);
    }

    /* 2. 删除旧 FreeType 字体 */
    homepage_font_unload();

    /* 3. 按新语言建新字体（中英文均建，仅时间字号不同） */
    homepage_font_load(lang);

    /* 4. 应用新字体到 label */
    if (time_label && time_font) {
        lv_obj_set_style_text_font(time_label, time_font, 0);
    }
    if (date_label && date_font) {
        lv_obj_set_style_text_font(date_label, date_font, 0);
    }
    if (weekday_label && weekday_font) {
        lv_obj_set_style_text_font(weekday_label, weekday_font, 0);
    }

    /* 5. 刷新日期/星期文字 */
    homepage_update_datetime();
}

/****************************************************************************
 * 日期时间更新
 ****************************************************************************/

static void homepage_update_datetime(void)
{
    struct tm time_info;
    char buf[64];

    /* 使用 agent_localtime() 获取正确的本地时间（NuttX localtime_r 不处理时区） */
    time_info = agent_localtime();

    /* 更新日期：中文 "7月21日"，英文 "Jul 21" */
    if (i18n_get_lang() == LANG_ZH_CN) {
        snprintf(buf, sizeof(buf), "%d月%d日",
                 time_info.tm_mon + 1, time_info.tm_mday);
    } else {
        /* 美式格式：月 日，如 "Jul 21" */
        snprintf(buf, sizeof(buf), "%s %d",
                 i18n_en_months[time_info.tm_mon], time_info.tm_mday);
    }
    if (date_label) {
        lv_label_set_text(date_label, buf);
    }

    /* 更新时间: "14:30" */
    snprintf(buf, sizeof(buf), "%02d:%02d",
             time_info.tm_hour, time_info.tm_min);
    if (time_label) {
        lv_label_set_text(time_label, buf);
    }

    /* 更新星期：查 i18n 表 */
    static const str_id_t week_ids[7] = {
        STR_HOME_WEEK_SUN, STR_HOME_WEEK_MON, STR_HOME_WEEK_TUE,
        STR_HOME_WEEK_WED, STR_HOME_WEEK_THU, STR_HOME_WEEK_FRI,
        STR_HOME_WEEK_SAT
    };
    if (weekday_label) {
        lv_label_set_text(weekday_label, i18n_get(week_ids[time_info.tm_wday]));
    }
}

static void time_timer_cb(lv_timer_t *timer)
{
    LV_UNUSED(timer);
    homepage_update_datetime();
}

/* 从其他页面返回主页时立即刷新时间 */
static void homepage_screen_load_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    homepage_update_datetime();
}



/****************************************************************************
 * Public Functions
 ****************************************************************************/

/* 创建主页：壁纸背景 + 日期时间星期 */
void create_xiaoq_page(lv_obj_t * parent)
{
    /* 设置壁纸背景（从 eMMC 动态加载） */
    bg_img_obj = lv_image_create(parent);
    
    /* 加载第一张壁纸 */
    if (load_wallpaper_from_emmc(current_wallpaper_idx)) {
        lv_obj_center(bg_img_obj);
    } else {
        LV_LOG_ERROR("Failed to load initial wallpaper from eMMC");
    }
    
    /* 启动壁纸轮播定时器（30秒后首次切换） */
    wallpaper_timer = lv_timer_create(wallpaper_switch_cb, WALLPAPER_SWITCH_MS, NULL);

    /* 按当前语言加载字体 */
    homepage_font_load(i18n_get_lang());

    /* 时间标签（大字体，表盘上方居中）
     * 中文用 MiSans 120px，英文用 MiSans 80px */
    time_label = lv_label_create(parent);
    if (time_font) {
        lv_obj_set_style_text_font(time_label, time_font, 0);
    } else {
        lv_obj_set_style_text_font(time_label, &lv_font_montserrat_24, 0);
    }
    lv_obj_set_style_text_color(time_label, lv_color_white(), 0);
    lv_label_set_text(time_label, "");
    lv_obj_align(time_label, LV_ALIGN_CENTER, 0, -90);

    /* 日期标签（小字体，时间下方偏左） */
    date_label = lv_label_create(parent);
    if (date_font) {
        lv_obj_set_style_text_font(date_label, date_font, 0);
    } else {
        lv_obj_set_style_text_font(date_label, &lv_font_montserrat_24, 0);
    }
    lv_obj_set_style_text_color(date_label, lv_color_white(), 0);
    lv_label_set_text(date_label, "");
    lv_obj_align(date_label, LV_ALIGN_CENTER, -55, 20);

    /* 星期标签（日期右侧） */
    weekday_label = lv_label_create(parent);
    if (weekday_font) {
        lv_obj_set_style_text_font(weekday_label, weekday_font, 0);
    } else {
        lv_obj_set_style_text_font(weekday_label, &lv_font_montserrat_24, 0);
    }
    lv_obj_set_style_text_color(weekday_label, lv_color_white(), 0);
    lv_label_set_text(weekday_label, "");
    lv_obj_align(weekday_label, LV_ALIGN_CENTER, 80, 20);

    /* 立即更新一次日期时间 */
    homepage_update_datetime();

    /* 每秒更新一次时间，确保 NTP 同步后立即反映到主页 */
    time_update_timer = lv_timer_create(time_timer_cb, 1000, NULL);

    /* 从其他页面返回主页时立即刷新时间（NTP 可能在离开期间完成同步） */
    lv_obj_add_event_cb(parent, homepage_screen_load_cb, LV_EVENT_SCREEN_LOAD_START, NULL);

    /* 底部上滑引导图标：提示用户上滑进入功能菜单 */
    lv_obj_t *guide_img = lv_image_create(parent);
    lv_image_set_src(guide_img, &up_up);
    /* 原图 200x200，缩放到 ~48x48 作为引导图标 */
    lv_image_set_scale(guide_img, 61);
    lv_obj_align(guide_img, LV_ALIGN_BOTTOM_MID, 0, 75);
    /* 染为白色，适配深色（黑色/蓝色）壁纸，提升可见性 */
    lv_obj_set_style_img_recolor(guide_img, lv_color_white(), 0);
    lv_obj_set_style_img_recolor_opa(guide_img, LV_OPA_COVER, 0);

    /* 注册语言变更回调（主页常驻，原地刷新，方案 C） */
    i18n_register_lang_cb(homepage_lang_changed_cb);
}

/****************************************************************************
 * 壁纸轮播：eMMC 动态加载 + 定时切换 + 淡入淡出动画
 ****************************************************************************/

/* 从 eMMC 加载指定索引的壁纸到内存缓冲区 */
static bool load_wallpaper_from_emmc(int idx)
{
    FILE *fp = NULL;
    size_t bytes_read = 0;
    const char *path = wallpaper_paths[idx];
    
    /* 参数检查 */
    if (idx < 0 || idx >= WALLPAPER_COUNT || !path) {
        LV_LOG_ERROR("Invalid wallpaper index: %d", idx);
        return false;
    }
    
    /* 打开文件 */
    fp = fopen(path, "rb");
    if (!fp) {
        LV_LOG_ERROR("Cannot open wallpaper file: %s", path);
        return false;
    }
    
    /* 分配内存缓冲区（如果尚未分配或需要重新分配） */
    if (!current_wallpaper_data) {
        current_wallpaper_data = (uint8_t *)lv_malloc(WALLPAPER_DATA_SIZE);
        if (!current_wallpaper_data) {
            LV_LOG_ERROR("Failed to allocate wallpaper buffer (%d bytes)", WALLPAPER_DATA_SIZE);
            fclose(fp);
            return false;
        }
    }
    
    /* 读取文件数据 */
    bytes_read = fread(current_wallpaper_data, 1, WALLPAPER_DATA_SIZE, fp);
    fclose(fp);
    
    if (bytes_read != WALLPAPER_DATA_SIZE) {
        LV_LOG_ERROR("Wallpaper file size mismatch: expected %d, got %d", 
                     WALLPAPER_DATA_SIZE, (int)bytes_read);
        return false;
    }
    
    /* 创建临时描述符并设置到 image 对象 */
    static lv_image_dsc_t dsc;
    dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    dsc.header.w = 454;
    dsc.header.h = 454;
    dsc.data_size = WALLPAPER_DATA_SIZE;
    dsc.data = current_wallpaper_data;
    
    lv_image_set_src(bg_img_obj, &dsc);
    
    LV_LOG_INFO("Successfully loaded wallpaper: %s (%d bytes)", path, (int)bytes_read);
    return true;
}

static void wallpaper_switch_cb(lv_timer_t *timer)
{
    LV_UNUSED(timer);
    
    if (!bg_img_obj) return;
    
    /* 切换到下一张壁纸（循环） */
    current_wallpaper_idx = (current_wallpaper_idx + 1) % WALLPAPER_COUNT;
    
    /* 淡出效果（300ms） */
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, bg_img_obj);
    lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_TRANSP);
    lv_anim_set_time(&a, 300);
    lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)lv_obj_set_style_opa);
    lv_anim_set_ready_cb(&a, wallpaper_fade_in_ready_cb);
    lv_anim_start(&a);
}

/* 淡出完成后：从 eMMC 加载新图片并淡入 */
static void wallpaper_fade_in_ready_cb(lv_anim_t *a)
{
    LV_UNUSED(a);
    
    /* 从 eMMC 加载新壁纸 */
    if (load_wallpaper_from_emmc(current_wallpaper_idx)) {
        /* 淡入效果（300ms） */
        lv_anim_t anim_in;
        lv_anim_init(&anim_in);
        lv_anim_set_var(&anim_in, bg_img_obj);
        lv_anim_set_values(&anim_in, LV_OPA_TRANSP, LV_OPA_COVER);
        lv_anim_set_time(&anim_in, 300);
        lv_anim_set_exec_cb(&anim_in, (lv_anim_exec_xcb_t)lv_obj_set_style_opa);
        lv_anim_start(&anim_in);
    } else {
        LV_LOG_ERROR("Failed to load wallpaper %d, keeping previous image", current_wallpaper_idx);
        /* 即使加载失败也要恢复透明度，否则会一直透明 */
        lv_obj_set_style_opa(bg_img_obj, LV_OPA_COVER, 0);
    }
}

void xiaoq_update_status(const char *status)
{
    LV_UNUSED(status);
}

void xiaoq_cleanup(void)
{
    /* 注销语言变更回调 */
    i18n_unregister_lang_cb(homepage_lang_changed_cb);

    /* 清理壁纸轮播定时器和内存 */
    if (wallpaper_timer) {
        lv_timer_del(wallpaper_timer);
        wallpaper_timer = NULL;
    }
    
    /* 释放 eMMC 加载的壁纸数据缓冲区 */
    if (current_wallpaper_data) {
        lv_free(current_wallpaper_data);
        current_wallpaper_data = NULL;
    }
    bg_img_obj = NULL;  /* 防止悬空指针 */

    if (time_update_timer) {
        lv_timer_del(time_update_timer);
        time_update_timer = NULL;
    }
    homepage_font_unload();
}
