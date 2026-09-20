#ifndef CALL_PAGE_H
#define CALL_PAGE_H

#include <lvgl/lvgl.h>

typedef enum {
    PAGE_DIAL = 0,
    PAGE_INCOMING,
    PAGE_CALLING
} call_page_type_t;

extern call_page_type_t current_page;
extern lv_obj_t *call_screen;

void create_call_page(lv_obj_t *parent);
void call_page_deinit(void);
void create_incoming_call_modal(const char *num_str, const char *name_str);
void hide_call_ui(void);

#endif