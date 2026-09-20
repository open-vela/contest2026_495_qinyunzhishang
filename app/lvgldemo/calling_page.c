#include <nuttx/config.h>
#include <string.h>
#include <stdio.h>
#include <lvgl/lvgl.h>

#include "calling_page.h"
#include "call_page.h"
#include "lvgldemo_common.h"
#include "i18n.h"
#include "bt_debug_page.h"
#include "bt_call_handler.h"

lv_obj_t *calling_screen = NULL;
static lv_obj_t *calling_num_txt = NULL;
static lv_obj_t *calling_status_txt = NULL;
static lv_obj_t *calling_time_txt = NULL;
static lv_obj_t *hangup_btn = NULL;
static lv_obj_t *nav_back_btn = NULL;
static lv_timer_t *call_timer = NULL;
static uint32_t call_sec = 0;

static void calling_gesture_cb(lv_event_t *e);
static void calling_screen_unloaded_cb(lv_event_t *e);
static void call_time_timer_cb(lv_timer_t *timer);
static void hangup_btn_click_cb(lv_event_t *e);
static void back_btn_click_cb(lv_event_t *e);

static void call_time_timer_cb(lv_timer_t *timer)
{
    LV_UNUSED(timer);
    call_sec++;
    uint32_t min = call_sec / 60;
    uint32_t sec = call_sec % 60;

    char time_buf[16] = {0};
    snprintf(time_buf, sizeof(time_buf), "%02d:%02d", (int)min, (int)sec);
    lv_label_set_text(calling_time_txt, time_buf);
}

static void calling_gesture_cb(lv_event_t *e)
{
    lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_active());
    if (dir == LV_DIR_RIGHT && call_screen)
    {
        current_page = PAGE_DIAL;
        lv_scr_load_anim(call_screen, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 300, 0, false);
        calling_screen = NULL;
    }
    else if (dir == LV_DIR_LEFT)
    {
        if (bt_debug_screen == NULL)
        {
            bt_debug_screen = lv_obj_create(NULL);
            create_bt_debug_page(bt_debug_screen);
        }
        lv_scr_load_anim(bt_debug_screen, LV_SCR_LOAD_ANIM_MOVE_LEFT, 300, 0, false);
    }
}

static void calling_screen_unloaded_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    if (call_timer != NULL)
    {
        lv_timer_del(call_timer);
        call_timer = NULL;
    }
    calling_num_txt = NULL;
    calling_status_txt = NULL;
    calling_time_txt = NULL;
    hangup_btn = NULL;
    nav_back_btn = NULL;
    call_sec = 0;

    if (calling_screen != NULL)
    {
        lv_obj_t *tmp = calling_screen;
        calling_screen = NULL;
        lv_obj_del(tmp);
    }
}

static void back_btn_click_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    if (call_screen)
    {
        current_page = PAGE_DIAL;
        lv_scr_load_anim(call_screen, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 300, 0, false);
        calling_screen = NULL;
    }
    else
    {
        /* call_screen 未创建时（已移除手动拨号入口），直接返回主界面 */
        current_page = PAGE_DIAL;
        if (main_screen)
        {
            lv_scr_load_anim(main_screen, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 300, 0, false);
        }
        calling_screen = NULL;
    }
}

static void hangup_btn_click_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    LV_LOG_USER("Hang up");
    bt_call_terminate();
    
    if (call_screen)
    {
        current_page = PAGE_DIAL;
        lv_scr_load_anim(call_screen, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 300, 0, false);
        calling_screen = NULL;
    }
    else
    {
        /* call_screen 未创建时，立即返回主界面（避免等待2秒轮询） */
        current_page = PAGE_DIAL;
        if (main_screen)
        {
            lv_scr_load_anim(main_screen, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 300, 0, false);
        }
        calling_screen = NULL;
    }
}

void create_calling_page(lv_obj_t *parent)
{
    call_sec = 0;

    lv_obj_set_style_bg_color(parent, lv_color_hex(0xF0F8FF), 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_left(parent, 60, 0);
    lv_obj_set_style_pad_right(parent, 60, 0);
    lv_obj_set_style_pad_top(parent, 10, 0);
    lv_obj_set_style_pad_bottom(parent, 10, 0);

    lv_obj_set_scrollbar_mode(parent, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(parent, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(parent, LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_clear_flag(parent, LV_OBJ_FLAG_SCROLL_MOMENTUM);

    lv_obj_add_event_cb(parent, calling_gesture_cb, LV_EVENT_GESTURE, NULL);
    lv_obj_add_event_cb(parent, calling_screen_unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);

    nav_back_btn = lv_btn_create(parent);
    lv_obj_remove_style_all(nav_back_btn);
    lv_obj_set_size(nav_back_btn, 30, 30);
    lv_obj_set_pos(nav_back_btn, 10, 12);
    lv_obj_set_style_bg_opa(nav_back_btn, LV_OPA_TRANSP, 0);

    lv_obj_t *back_icon = lv_label_create(nav_back_btn);
    lv_obj_remove_style_all(back_icon);
    lv_label_set_text(back_icon, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_font(back_icon, LV_FONT_DEFAULT, 0);
    lv_obj_set_style_text_color(back_icon, lv_color_hex(0x0088FF), 0);
    lv_obj_center(back_icon);
    lv_obj_add_event_cb(nav_back_btn, back_btn_click_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *phone_icon = lv_label_create(parent);
    lv_obj_remove_style_all(phone_icon);
    lv_label_set_text(phone_icon, LV_SYMBOL_CALL);
    lv_obj_set_style_text_font(phone_icon, LV_FONT_DEFAULT, 0);
    lv_obj_set_style_text_color(phone_icon, lv_color_hex(0x00C853), 0);
    lv_obj_align(phone_icon, LV_ALIGN_TOP_MID, 0, 35);

    calling_num_txt = lv_label_create(parent);
    lv_obj_remove_style_all(calling_num_txt);
    lv_label_set_text(calling_num_txt, "");
    i18n_apply_font(calling_num_txt);
    lv_obj_set_style_text_color(calling_num_txt, lv_color_black(), 0);
    lv_obj_align(calling_num_txt, LV_ALIGN_TOP_MID, 0, 80);

    calling_status_txt = lv_label_create(parent);
    lv_obj_remove_style_all(calling_status_txt);
    lv_label_set_text(calling_status_txt, i18n_get(STR_CALLING_CALLING));
    i18n_apply_font(calling_status_txt);
    lv_obj_set_style_text_color(calling_status_txt, lv_color_hex(0x999999), 0);
    lv_obj_align(calling_status_txt, LV_ALIGN_CENTER, 0, -25);

    calling_time_txt = lv_label_create(parent);
    lv_obj_remove_style_all(calling_time_txt);
    lv_label_set_text(calling_time_txt, "00:00");
    i18n_apply_font(calling_time_txt);
    lv_obj_set_style_text_color(calling_time_txt, lv_color_hex(0x0088FF), 0);
    lv_obj_align(calling_time_txt, LV_ALIGN_CENTER, 0, 10);

    call_timer = lv_timer_create(call_time_timer_cb, 1000, NULL);

    hangup_btn = lv_btn_create(parent);
    lv_obj_remove_style_all(hangup_btn);
    const int btn_size = 68;
    lv_obj_set_size(hangup_btn, btn_size, btn_size);
    lv_obj_align(hangup_btn, LV_ALIGN_BOTTOM_MID, 0, -40);
    lv_obj_set_style_bg_color(hangup_btn, lv_color_hex(0xFF3B30), 0);
    lv_obj_set_style_bg_opa(hangup_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(hangup_btn, btn_size / 2, 0);
    lv_obj_set_style_border_width(hangup_btn, 0, 0);

    lv_obj_t *hang_icon = lv_label_create(hangup_btn);
    lv_obj_remove_style_all(hang_icon);
    lv_label_set_text(hang_icon, LV_SYMBOL_CALL);
    lv_obj_set_style_text_color(hang_icon, lv_color_white(), 0);
    lv_obj_set_style_text_font(hang_icon, LV_FONT_DEFAULT, 0);
    lv_obj_center(hang_icon);
    lv_obj_add_event_cb(hangup_btn, hangup_btn_click_cb, LV_EVENT_CLICKED, NULL);
}

void show_calling_page(void)
{
    if (calling_screen == NULL)
    {
        calling_screen = lv_obj_create(NULL);
        create_calling_page(calling_screen);
    }
    lv_scr_load_anim(calling_screen, LV_SCR_LOAD_ANIM_MOVE_LEFT, 300, 0, false);
}

void hide_calling_page(void)
{
    if (calling_screen != NULL)
    {
        lv_obj_t *tmp = calling_screen;
        calling_screen = NULL;
        lv_obj_del(tmp);
    }
}

void update_calling_info(const char *num_str, const char *name_str)
{
    LV_UNUSED(name_str);
    if (calling_num_txt != NULL && num_str != NULL && num_str[0] != '\0')
        lv_label_set_text(calling_num_txt, num_str);
}

void reset_calling_timer(void)
{
    call_sec = 0;
    if (calling_time_txt != NULL)
        lv_label_set_text(calling_time_txt, "00:00");
}

void update_calling_status(int state)
{
    if (calling_status_txt == NULL)
        return;

    const char *txt;
    switch (state)
    {
        case 4:
            txt = i18n_get(STR_CALLING_ON_CALL);
            break;
        case 2:
            txt = i18n_get(STR_CALLING_DIALING);
            break;
        case 3:
            txt = i18n_get(STR_CALLING_RINGING);
            break;
        default:
            txt = i18n_get(STR_CALLING_CALLING);
            break;
    }
    lv_label_set_text(calling_status_txt, txt);
}


void calling_page_deinit(void)
{
    if (call_timer != NULL)
    {
        lv_timer_del(call_timer);
        call_timer = NULL;
    }

    calling_num_txt = NULL;
    calling_status_txt = NULL;
    calling_time_txt = NULL;
    hangup_btn = NULL;
    nav_back_btn = NULL;
    call_sec = 0;

    if (calling_screen != NULL)
    {
        lv_obj_t *tmp = calling_screen;
        calling_screen = NULL;
        lv_obj_del(tmp);
    }
}