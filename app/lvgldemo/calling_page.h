#ifndef CALLING_PAGE_H
#define CALLING_PAGE_H

#include <lvgl/lvgl.h>

extern lv_obj_t *calling_screen;

void create_calling_page(lv_obj_t *parent);
void calling_page_deinit(void);
void show_calling_page(void);
void hide_calling_page(void);
void update_calling_info(const char *num_str, const char *name_str);
void reset_calling_timer(void);
void update_calling_status(int state);

#endif