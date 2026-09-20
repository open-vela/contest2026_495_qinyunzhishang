/****************************************************************************
 * apps/examples/lvgldemo/camera_ai_page.h
 *
 * Camera AI analysis chat page
 *
 ****************************************************************************/

#ifndef CAMERA_AI_PAGE_H
#define CAMERA_AI_PAGE_H

#include <lvgl/lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Create AI analysis chat page, returns root screen object */
lv_obj_t *create_camera_ai_page(const char *image_path);

/* Destroy AI page and free resources, called when returning to camera */
void destroy_camera_ai_page(void);

/* Check if AI page is currently active */
bool camera_ai_page_is_active(void);

#ifdef __cplusplus
}
#endif

#endif /* CAMERA_AI_PAGE_H */
