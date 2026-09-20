/****************************************************************************
 * apps/examples/lvgldemo/camera_page.h
 *
 * Camera page — remote capture via UART to Lubancat 2N.
 *
 ****************************************************************************/

#ifndef CAMERA_PAGE_H
#define CAMERA_PAGE_H

#include <lvgl/lvgl.h>
#include <stdbool.h>

void create_camera_page(lv_obj_t *parent);
void camera_page_deinit(void);

/* 获取camera页面对象，用于页面切换 */
lv_obj_t *camera_page_get_screen(void);

/* 设置按钮启用/禁用状态 */
void camera_page_set_buttons_enabled(bool enabled);

/* 设置忙碌状态 */
void camera_page_set_busy(bool busy);

/* 从 AI 分析界面返回 camera 时调用：保留缩略图，仅恢复按钮状态 */
void camera_page_notify_return_from_ai(void);

#endif /* CAMERA_PAGE_H */
