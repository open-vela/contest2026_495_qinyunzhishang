#include <nuttx/config.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <lvgl/lvgl.h>

#include "bt_debug_page.h"
#include "bt_call_handler.h"
#include "calling_page.h"
#include "call_page.h"
#include "lvgldemo_common.h"

lv_obj_t *bt_debug_screen = NULL;

static lv_obj_t *bt_status_label = NULL;
static lv_obj_t *call_status_label = NULL;
static lv_obj_t *detail_label = NULL;
static lv_obj_t *raw_file_label = NULL;
static lv_timer_t *refresh_timer = NULL;

static const char* get_call_state_str(bt_call_state_t state)
{
    switch (state)
    {
    case BT_CALL_IDLE:      return "IDLE";
    case BT_CALL_INCOMING:  return "INCOMING";
    case BT_CALL_DIALING:   return "DIALING";
    case BT_CALL_ALERTING:  return "ALERTING";
    case BT_CALL_ACTIVE:    return "ACTIVE";
    case BT_CALL_HELD:      return "HELD";
    default:                return "UNKNOWN";
    }
}

static void refresh_status_cb(lv_timer_t *timer)
{
    bool available = bt_call_handler_is_available();
    bt_call_state_t state = bt_call_handler_get_call_state();
    const char *number = bt_call_handler_get_call_number();
    const char *name = bt_call_handler_get_call_name();

    if (available)
    {
        lv_label_set_text(bt_status_label, "BT: ON");
        lv_obj_set_style_text_color(bt_status_label, lv_color_hex(0x00C853), 0);
    }
    else
    {
        lv_label_set_text(bt_status_label, "BT: OFF");
        lv_obj_set_style_text_color(bt_status_label, lv_color_hex(0xFF1744), 0);
    }

    lv_label_set_text(call_status_label, get_call_state_str(state));

    if (state == BT_CALL_INCOMING || state == BT_CALL_ACTIVE || state == BT_CALL_DIALING || state == BT_CALL_ALERTING)
    {
        lv_obj_set_style_text_color(call_status_label, lv_color_hex(0xFFD600), 0);

        char info[128];
        if (name && name[0] != '\0')
            snprintf(info, sizeof(info), "%s\n%s", number, name);
        else if (number && number[0] != '\0')
            snprintf(info, sizeof(info), "%s", number);
        else
            snprintf(info, sizeof(info), "No number info");

        lv_label_set_text(detail_label, info);
    }
    else
    {
        lv_obj_set_style_text_color(call_status_label, lv_color_hex(0x00C853), 0);
        lv_label_set_text(detail_label, "No active call");
    }

    int fd = open("/tmp/bt_call_state", O_RDONLY);
    if (fd >= 0)
    {
        char raw[128];
        int n = read(fd, raw, sizeof(raw) - 1);
        close(fd);
        if (n > 0)
        {
            raw[n] = '\0';
            if (raw[n - 1] == '\n') raw[n - 1] = '\0';
            char display[160];
            snprintf(display, sizeof(display), "File: %s", raw);
            lv_label_set_text(raw_file_label, display);
        }
        else
        {
            lv_label_set_text(raw_file_label, "File: (empty)");
        }
    }
    else
    {
        lv_label_set_text(raw_file_label, "File: not found");
    }
}

static void bt_debug_gesture_cb(lv_event_t *e)
{
    lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_active());

    if (dir == LV_DIR_RIGHT)
    {
        if (calling_screen)
        {
            lv_scr_load_anim(calling_screen, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 300, 0, false);
        }
        else if (call_screen)
        {
            lv_scr_load_anim(call_screen, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 300, 0, false);
        }
        else if (main_screen)
        {
            lv_scr_load_anim(main_screen, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 300, 0, false);
        }
    }
}

void create_bt_debug_page(lv_obj_t *parent)
{
    lv_obj_set_style_bg_color(parent, lv_color_hex(0x1A1A2E), 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);
    lv_obj_clear_flag(parent, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(parent);
    lv_obj_set_style_text_font(title, LV_FONT_DEFAULT, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_label_set_text(title, "BT Debug");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 20);

    bt_status_label = lv_label_create(parent);
    lv_obj_set_style_text_font(bt_status_label, LV_FONT_DEFAULT, 0);
    lv_obj_set_style_text_color(bt_status_label, lv_color_hex(0xFF1744), 0);
    lv_label_set_text(bt_status_label, "BT: OFF");
    lv_obj_align(bt_status_label, LV_ALIGN_TOP_MID, 0, 55);

    call_status_label = lv_label_create(parent);
    lv_obj_set_style_text_font(call_status_label, LV_FONT_DEFAULT, 0);
    lv_obj_set_style_text_color(call_status_label, lv_color_hex(0x00C853), 0);
    lv_label_set_text(call_status_label, "IDLE");
    lv_obj_align(call_status_label, LV_ALIGN_TOP_MID, 0, 85);

    detail_label = lv_label_create(parent);
    lv_obj_set_style_text_font(detail_label, LV_FONT_DEFAULT, 0);
    lv_obj_set_style_text_color(detail_label, lv_color_hex(0xBBBBBB), 0);
    lv_label_set_text(detail_label, "No active call");
    lv_obj_align(detail_label, LV_ALIGN_TOP_MID, 0, 120);

    raw_file_label = lv_label_create(parent);
    lv_obj_set_style_text_font(raw_file_label, LV_FONT_DEFAULT, 0);
    lv_obj_set_style_text_color(raw_file_label, lv_color_hex(0x888888), 0);
    lv_label_set_text(raw_file_label, "File: ---");
    lv_obj_align(raw_file_label, LV_ALIGN_TOP_MID, 0, 160);

    lv_obj_t *hint = lv_label_create(parent);
    lv_obj_set_style_text_font(hint, LV_FONT_DEFAULT, 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(0x666666), 0);
    lv_label_set_text(hint, "Swipe right to go back");
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -20);

    lv_obj_add_event_cb(parent, bt_debug_gesture_cb, LV_EVENT_GESTURE, NULL);

    refresh_timer = lv_timer_create(refresh_status_cb, 1000, NULL);
}

void bt_debug_page_deinit(void)
{
    if (refresh_timer != NULL)
    {
        lv_timer_del(refresh_timer);
        refresh_timer = NULL;
    }
    bt_status_label = NULL;
    call_status_label = NULL;
    detail_label = NULL;
    raw_file_label = NULL;
}