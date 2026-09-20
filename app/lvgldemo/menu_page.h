#ifndef MENU_PAGE_H
#define MENU_PAGE_H

#include <lvgl/lvgl.h>

/* menu_screen 全局变量（定义在 lvgldemo.c） */
extern lv_obj_t *menu_screen;

/* 创建菜单页面（统一的功能入口列表）
 * 包含7个菜单项：
 *   返回主界面 / 语音对话 / 会议模式 / 图文智录 / 录音 / 媒体播放器 / 设置
 */
void create_menu_page(lv_obj_t *parent);

/* 强制删除 menu_screen（menu_page 无 SCREEN_UNLOADED 回调，切语言时主动清理）
 * 删除后将 menu_screen 置 NULL，下次进入菜单时重新创建
 */
void menu_page_force_delete(void);

#endif /* MENU_PAGE_H */