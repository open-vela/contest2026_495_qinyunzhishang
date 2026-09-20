#ifndef BT_DEBUG_PAGE_H
#define BT_DEBUG_PAGE_H

#include <lvgl/lvgl.h>

extern lv_obj_t *bt_debug_screen;

void create_bt_debug_page(lv_obj_t *parent);
void bt_debug_page_deinit(void);

#endif