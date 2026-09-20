#include <nuttx/config.h>
#include <stdio.h>
#include <string.h>
#include <lvgl/lvgl.h>

#include "call_page.h"
#include "calling_page.h"
#include "lvgldemo_common.h"
#include "i18n.h"
#include "bt_call_handler.h"

extern lv_obj_t *main_screen;

lv_obj_t *call_screen = NULL;
call_page_type_t current_page = PAGE_DIAL;

static lv_obj_t *top_number_txt = NULL;
static lv_obj_t *num_key[12];
static lv_obj_t *del_key = NULL;
static lv_obj_t *call_func_btn = NULL;
static lv_obj_t *nav_back_btn = NULL;
static char dial_buf[32] = {0};
static lv_obj_t *incoming_screen = NULL;

static const char *key_text[] = {
    "1", "2", "3",
    "4", "5", "6",
    "7", "8", "9",
    "*", "0", "#"
};

static void create_incoming_call_page(lv_obj_t *parent, const char *num_str, const char *name_str);

static void call_gesture_cb(lv_event_t *e)
{
    lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_active());

    if (dir == LV_DIR_RIGHT)
    {
        switch (current_page)
        {
            case PAGE_DIAL:
                if (main_screen)
                    lv_scr_load_anim(main_screen, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 300, 0, false);
                break;
            case PAGE_INCOMING:
                current_page = PAGE_DIAL;
                if (call_screen)
                    lv_scr_load_anim(call_screen, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 300, 0, false);
                break;
            case PAGE_CALLING:
                current_page = PAGE_DIAL;
                if (call_screen)
                    lv_scr_load_anim(call_screen, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 300, 0, false);
                calling_screen = NULL;
                break;
            default:
                break;
        }
    }
    else if (dir == LV_DIR_LEFT)
    {
        switch (current_page)
        {
            case PAGE_DIAL:
                current_page = PAGE_INCOMING;
                incoming_screen = lv_obj_create(NULL);
                create_incoming_call_page(incoming_screen, "13812345678", "Test");
                lv_scr_load_anim(incoming_screen, LV_SCR_LOAD_ANIM_MOVE_LEFT, 300, 0, false);
                break;
            case PAGE_INCOMING:
                current_page = PAGE_CALLING;
                show_calling_page();
                break;
            case PAGE_CALLING:
            default:
                break;
        }
    }
}

static void back_btn_click_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    if (main_screen)
        lv_scr_load_anim(main_screen, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 300, 0, false);
}

static void dial_key_click_cb(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);
    uint32_t idx = (uint32_t)(uintptr_t)lv_obj_get_user_data(btn);

    if (strlen(dial_buf) < sizeof(dial_buf) - 1)
    {
        strcat(dial_buf, key_text[idx]);
        lv_label_set_text(top_number_txt, dial_buf);
    }
}

static void del_key_click_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    int len = strlen(dial_buf);
    if (len > 0)
    {
        dial_buf[len - 1] = '\0';
        lv_label_set_text(top_number_txt, dial_buf);
    }
}

static void dial_call_btn_click_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    LV_LOG_USER("Dial: %s", dial_buf);
}

static void incoming_screen_unloaded_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    if (incoming_screen != NULL)
    {
        lv_obj_t *tmp = incoming_screen;
        incoming_screen = NULL;
        lv_obj_del(tmp);
    }
}

static void incoming_accept_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    LV_LOG_USER("Accept call");
    bt_call_accept();
    current_page = PAGE_CALLING;
    show_calling_page();
}

static void incoming_reject_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    LV_LOG_USER("Reject call");
    bt_call_reject();
    current_page = PAGE_DIAL;
    if (call_screen)
        lv_scr_load_anim(call_screen, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 300, 0, false);
    else if (main_screen)
        lv_scr_load_anim(main_screen, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 300, 0, false);
}

void create_call_page(lv_obj_t *parent)
{
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

    lv_obj_add_event_cb(parent, call_gesture_cb, LV_EVENT_GESTURE, NULL);

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

    top_number_txt = lv_label_create(parent);
    lv_obj_remove_style_all(top_number_txt);
    lv_label_set_text(top_number_txt, "");
    lv_obj_set_style_text_font(top_number_txt, LV_FONT_DEFAULT, 0);
    lv_obj_set_style_text_color(top_number_txt, lv_color_black(), 0);
    lv_obj_align(top_number_txt, LV_ALIGN_TOP_MID, 0, 30);

    const int key_w = 50;
    const int key_gap = 12;
    int safe_width = LV_HOR_RES - 120;
    int start_x = 6 + (safe_width - (key_w * 3 + key_gap * 2)) / 2;
    int start_y = 60;

    for (int row = 0; row < 4; row++)
    {
        for (int col = 0; col < 3; col++)
        {
            int idx = row * 3 + col;
            num_key[idx] = lv_btn_create(parent);
            lv_obj_remove_style_all(num_key[idx]);
            lv_obj_set_size(num_key[idx], key_w, key_w);
            lv_obj_set_pos(num_key[idx],
                           start_x + col * (key_w + key_gap),
                           start_y + row * (key_w + key_gap));

            lv_obj_set_style_bg_color(num_key[idx], lv_color_white(), 0);
            lv_obj_set_style_radius(num_key[idx], key_w / 2, 0);
            lv_obj_set_style_border_width(num_key[idx], 0, 0);
            lv_obj_set_style_shadow_width(num_key[idx], 0, 0);

            lv_obj_t *key_lab = lv_label_create(num_key[idx]);
            lv_obj_remove_style_all(key_lab);
            lv_label_set_text(key_lab, key_text[idx]);
            lv_obj_center(key_lab);

            lv_obj_set_user_data(num_key[idx], (void *)(uintptr_t)idx);
            lv_obj_add_event_cb(num_key[idx], dial_key_click_cb, LV_EVENT_CLICKED, NULL);
        }
    }

    const int act_w = 56;
    const int act_gap = 20;
    int act_row_y = start_y + 4 * (key_w + key_gap);
    int act_total_w = act_w * 2 + act_gap;
    int act_start_x = (LV_HOR_RES - act_total_w) / 2 - 35;

    call_func_btn = lv_btn_create(parent);
    lv_obj_remove_style_all(call_func_btn);
    lv_obj_set_size(call_func_btn, act_w, act_w);
    lv_obj_set_pos(call_func_btn, act_start_x, act_row_y);
    lv_obj_set_style_bg_color(call_func_btn, lv_color_hex(0x00C853), 0);
    lv_obj_set_style_bg_opa(call_func_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(call_func_btn, act_w / 2, 0);
    lv_obj_set_style_border_width(call_func_btn, 0, 0);
    lv_obj_set_style_shadow_width(call_func_btn, 0, 0);

    lv_obj_t *call_icon = lv_label_create(call_func_btn);
    lv_obj_remove_style_all(call_icon);
    lv_label_set_text(call_icon, LV_SYMBOL_CALL);
    lv_obj_set_style_text_color(call_icon, lv_color_white(), 0);
    lv_obj_center(call_icon);
    lv_obj_add_event_cb(call_func_btn, dial_call_btn_click_cb, LV_EVENT_CLICKED, NULL);

    del_key = lv_btn_create(parent);
    lv_obj_remove_style_all(del_key);
    lv_obj_set_size(del_key, act_w, act_w);
    lv_obj_set_pos(del_key, act_start_x + act_w + act_gap, act_row_y);
    lv_obj_set_style_bg_color(del_key, lv_color_hex(0xE8E8E8), 0);
    lv_obj_set_style_bg_opa(del_key, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(del_key, act_w / 2, 0);
    lv_obj_set_style_border_width(del_key, 0, 0);
    lv_obj_set_style_shadow_width(del_key, 0, 0);

    lv_obj_t *del_lab = lv_label_create(del_key);
    lv_obj_remove_style_all(del_lab);
    lv_label_set_text(del_lab, "AC");
    lv_obj_center(del_lab);
    lv_obj_add_event_cb(del_key, del_key_click_cb, LV_EVENT_CLICKED, NULL);
}

void call_page_deinit(void)
{
    top_number_txt = NULL;
    del_key = NULL;
    call_func_btn = NULL;
    nav_back_btn = NULL;
    memset(dial_buf, 0, sizeof(dial_buf));

    for (int i = 0; i < 12; i++)
        num_key[i] = NULL;

    if (incoming_screen != NULL)
    {
        lv_obj_t *tmp = incoming_screen;
        incoming_screen = NULL;
        lv_obj_del(tmp);
    }

    if (call_screen != NULL)
    {
        lv_obj_t *tmp = call_screen;
        call_screen = NULL;
        lv_obj_del(tmp);
    }

    current_page = PAGE_DIAL;
}

static void create_incoming_call_page(lv_obj_t *parent, const char *num_str, const char *name_str)
{
    LV_UNUSED(name_str);
    lv_obj_set_style_bg_color(parent, lv_color_hex(0x1C1C1E), 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);
    lv_obj_clear_flag(parent, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_add_event_cb(parent, call_gesture_cb, LV_EVENT_GESTURE, NULL);
    lv_obj_add_event_cb(parent, incoming_screen_unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);

    lv_obj_t *phone_icon = lv_label_create(parent);
    lv_obj_remove_style_all(phone_icon);
    lv_label_set_text(phone_icon, LV_SYMBOL_CALL);
    lv_obj_set_style_text_font(phone_icon, LV_FONT_DEFAULT, 0);
    lv_obj_set_style_text_color(phone_icon, lv_color_hex(0x00C853), 0);
    lv_obj_align(phone_icon, LV_ALIGN_TOP_MID, 0, 40);

    lv_obj_t *title_txt = lv_label_create(parent);
    lv_obj_remove_style_all(title_txt);
    lv_label_set_text(title_txt, i18n_get(STR_CALL_INCOMING));
    i18n_apply_font(title_txt);
    lv_obj_set_style_text_color(title_txt, lv_color_white(), 0);
    lv_obj_align(title_txt, LV_ALIGN_TOP_MID, 0, 80);

    lv_obj_t *num_txt = lv_label_create(parent);
    lv_obj_remove_style_all(num_txt);
    if (num_str != NULL && num_str[0] != '\0')
        lv_label_set_text(num_txt, num_str);
    else
        lv_label_set_text(num_txt, i18n_get(STR_CALL_UNKNOWN));
    i18n_apply_font(num_txt);
    lv_obj_set_style_text_color(num_txt, lv_color_white(), 0);
    lv_obj_align(num_txt, LV_ALIGN_CENTER, 0, -10);

    lv_obj_t *reject_label = lv_label_create(parent);
    lv_obj_remove_style_all(reject_label);
    lv_label_set_text(reject_label, i18n_get(STR_CALL_DECLINE));
    i18n_apply_font(reject_label);
    lv_obj_set_style_text_color(reject_label, lv_color_hex(0xFF3B30), 0);
    lv_obj_align(reject_label, LV_ALIGN_BOTTOM_MID, -55, -80);

    lv_obj_t *reject_btn = lv_btn_create(parent);
    lv_obj_set_size(reject_btn, 56, 56);
    lv_obj_align(reject_btn, LV_ALIGN_BOTTOM_MID, -55, -25);
    lv_obj_set_style_bg_color(reject_btn, lv_color_hex(0xFF3B30), 0);
    lv_obj_set_style_radius(reject_btn, 28, 0);
    lv_obj_set_style_border_width(reject_btn, 0, 0);
    lv_obj_add_flag(reject_btn, LV_OBJ_FLAG_GESTURE_BUBBLE);

    lv_obj_t *rej_icon = lv_label_create(reject_btn);
    lv_obj_remove_style_all(rej_icon);
    lv_label_set_text(rej_icon, LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_color(rej_icon, lv_color_white(), 0);
    lv_obj_center(rej_icon);
    lv_obj_add_event_cb(reject_btn, incoming_reject_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *accept_label = lv_label_create(parent);
    lv_obj_remove_style_all(accept_label);
    lv_label_set_text(accept_label, i18n_get(STR_CALL_ACCEPT));
    i18n_apply_font(accept_label);
    lv_obj_set_style_text_color(accept_label, lv_color_hex(0x00C853), 0);
    lv_obj_align(accept_label, LV_ALIGN_BOTTOM_MID, 55, -80);

    lv_obj_t *accept_btn = lv_btn_create(parent);
    lv_obj_set_size(accept_btn, 56, 56);
    lv_obj_align(accept_btn, LV_ALIGN_BOTTOM_MID, 55, -25);
    lv_obj_set_style_bg_color(accept_btn, lv_color_hex(0x00C853), 0);
    lv_obj_set_style_radius(accept_btn, 28, 0);
    lv_obj_set_style_border_width(accept_btn, 0, 0);
    lv_obj_add_flag(accept_btn, LV_OBJ_FLAG_GESTURE_BUBBLE);

    lv_obj_t *acc_icon = lv_label_create(accept_btn);
    lv_obj_remove_style_all(acc_icon);
    lv_label_set_text(acc_icon, LV_SYMBOL_CALL);
    lv_obj_set_style_text_color(acc_icon, lv_color_white(), 0);
    lv_obj_center(acc_icon);
    lv_obj_add_event_cb(accept_btn, incoming_accept_cb, LV_EVENT_CLICKED, NULL);
}

void create_incoming_call_modal(const char *num_str, const char *name_str)
{
    if (incoming_screen != NULL)
        return;

    incoming_screen = lv_obj_create(NULL);
    create_incoming_call_page(incoming_screen,
                              (num_str != NULL && num_str[0] != '\0') ? num_str : i18n_get(STR_CALL_UNKNOWN),
                              name_str);
    current_page = PAGE_INCOMING;
    lv_scr_load_anim(incoming_screen, LV_SCR_LOAD_ANIM_MOVE_LEFT, 300, 0, false);
}

void hide_call_ui(void)
{
    current_page = PAGE_DIAL;

    if (main_screen != NULL)
        lv_scr_load_anim(main_screen, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 300, 0, false);
}