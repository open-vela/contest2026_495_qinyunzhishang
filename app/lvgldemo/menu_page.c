#include <nuttx/config.h>
#include <syslog.h>
#include <time.h>
#include <lvgl/lvgl.h>

#include "menu_page.h"
#include "lvgldemo_common.h"
#include "ai_page.h"
#include "xiaoq_page.h"
#include "meeting_page.h"
#include "camera_page.h"
#include "recorder_page.h"
#include "player_page.h"
#include "settings_page.h"
#include "agent_config.h"
#include "i18n.h"

/* 菜单项图标（32x32 RGB565 嵌入式位图，由 icons/*.c 提供） */
LV_IMAGE_DECLARE(menu_ai);
LV_IMAGE_DECLARE(menu_meeting);
LV_IMAGE_DECLARE(menu_capture);
LV_IMAGE_DECLARE(menu_recorder);
LV_IMAGE_DECLARE(menu_player);
LV_IMAGE_DECLARE(menu_setting);

/* UI 布局参数（参考同事的深色风格设计） */
#define MENU_BTN_HEIGHT     60          /* 按钮高度（增大触摸区防误触） */
#define MENU_BTN_GAP        8           /* 按钮间距（增大分隔防误触） */
#define MENU_START_Y        5           /* 在滚动容器内的起始位置 */
#define MENU_TITLE_HEIGHT   48          /* 顶部标题栏高度 */
#define MENU_SCROLL_TOP    MENU_TITLE_HEIGHT  /* 滚动容器起始 Y */
#define MENU_BOTTOM_MARGIN  50          /* 圆形屏底部留白50px（避开圆形边缘不可见区） */

/* 颜色定义（深色主题） */
#define COLOR_BG_DARK       0x000000
#define COLOR_TEXT_WHITE    0xFFFFFF
#define COLOR_BTN_NORMAL    0x000000
#define COLOR_BTN_PRESSED   0x222222

static lv_obj_t *menu_time_label = NULL;
static lv_timer_t *menu_time_timer = NULL;

/* ===== 页面跳转函数（复用 lvgldemo_common.h 全局 screen 指针）=====
 * 独立实现，不修改 lvgldemo.c 中 static 函数的可见性。
 */

static void menu_enter_ai_page(void)
{
    /* AI 页面异步清理进行中时拒绝进入，避免旧清理回调摧毁新页面 */
    if (ai_page_is_deinit_in_progress()) {
        syslog(LOG_INFO, "[menu] AI page deinit in progress, skip enter\n");
        xiaoq_update_status("请稍候...");
        return;
    }

    if (ai_screen == NULL) {
        ai_screen = lv_obj_create(NULL);
        create_ai_page(ai_screen);
    }
    lv_scr_load_anim(ai_screen, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 300, 0, false);
}

static void menu_enter_meeting_page(void)
{
    if (meeting_screen == NULL) {
        meeting_screen = lv_obj_create(NULL);
        create_meeting_page(meeting_screen);
    }
    lv_scr_load_anim(meeting_screen, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 300, 0, false);
}

static void menu_enter_camera_page(void)
{
    if (camera_screen == NULL) {
        camera_screen = lv_obj_create(NULL);
        create_camera_page(camera_screen);
    }
    lv_scr_load_anim(camera_screen, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 300, 0, false);
}

static void menu_enter_recorder_page(void)
{
    if (recorder_screen == NULL) {
        recorder_screen = lv_obj_create(NULL);
        create_recorder_page(recorder_screen);
    }
    lv_scr_load_anim(recorder_screen, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 300, 0, false);
}

static void menu_enter_player_page(void)
{
    if (player_screen == NULL) {
        player_screen = lv_obj_create(NULL);
        create_player_page(player_screen);
    }
    lv_scr_load_anim(player_screen, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 300, 0, false);
}

static void menu_back_to_main(void)
{
    if (main_screen) {
        lv_scr_load_anim(main_screen, LV_SCR_LOAD_ANIM_MOVE_BOTTOM, 300, 0, false);
    }
}

/* ===== 按钮回调 ===== */

static void btn_ai_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    menu_enter_ai_page();
}

static void btn_meeting_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    menu_enter_meeting_page();
}

static void btn_camera_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    menu_enter_camera_page();
}

static void btn_recorder_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    menu_enter_recorder_page();
}

static void btn_home_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    menu_back_to_main();
}

static void btn_player_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    menu_enter_player_page();
}

static void btn_settings_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    if (settings_screen == NULL) {
        settings_screen = lv_obj_create(NULL);
        create_settings_page(settings_screen);
    }
    lv_scr_load_anim(settings_screen, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 300, 0, false);
}

static void back_btn_click_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    menu_back_to_main();
}

/* 屏幕级手势回调：下滑返回主界面
 * 
 * 设计原理：
 * 由于parent清除了SCROLLABLE/ELASTIC/MOMENTUM标志以解决历史滚动冲突，
 * 导致title_bar上的GESTURE_BUBBLE无法正常传播到手势处理器。
 * 因此改用在parent（屏幕根对象）上直接注册手势事件。
 * 
 * 行为规则：
 * - 下滑手势(LV_DIR_BOTTOM)：无条件返回主界面
 *   （scroll_cont的上滑不会被误判为下滑，所以安全）
 * - 其他方向：忽略（由LVGL原生处理滚动）*/
static void menu_parent_gesture_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_get_act());
    if (dir == LV_DIR_BOTTOM) {
        syslog(LOG_INFO, "[menu] Swipe down -> back to main\n");
        menu_back_to_main();
    }
}

/* ===== 创建单个菜单按钮（深色风格，参考同事的 UI 设计）=====
 * 样式特点：
 * - 深色背景 (#25253A)
 * - 白色文字
 * - 左侧图标 + 右侧文字
 * - 圆角矩形
 * - 点击时颜色变深
 */

/* 更新标题栏时间显示（与主界面同步） */
static void menu_update_time(void)
{
    struct tm time_info;
    char buf[16];

    time_info = agent_localtime();
    snprintf(buf, sizeof(buf), "%02d:%02d",
             time_info.tm_hour, time_info.tm_min);

    if (menu_time_label) {
        lv_label_set_text(menu_time_label, buf);
    }
}

static void menu_time_timer_cb(lv_timer_t *timer)
{
    LV_UNUSED(timer);
    menu_update_time();
}

/* 进入功能菜单界面时立即刷新时间，避免显示旧时间 */
static void menu_screen_load_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    menu_update_time();
}

static void create_menu_item(lv_obj_t *parent, int index,
                              const lv_image_dsc_t *icon, const char *text,
                              lv_event_cb_t cb)
{
    /* 创建按钮容器：圆形屏适配——按钮宽度缩短50px并整体右移50px，
     * 让按钮内容避开左下/右下圆形不可见区域 */
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, LV_HOR_RES * 80 / 100 - 50, MENU_BTN_HEIGHT);  /* 原 lv_pct(80) 缩短50px */
    lv_obj_align(btn, LV_ALIGN_TOP_MID, 50,                            /* 右移50px */
                 MENU_START_Y + index * (MENU_BTN_HEIGHT + MENU_BTN_GAP));

    /* 深色背景样式 */
    lv_obj_set_style_bg_color(btn, lv_color_hex(COLOR_BTN_NORMAL), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn, 10, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_shadow_opa(btn, LV_OPA_TRANSP, 0);

    /* 点击状态：背景变深 */
    lv_obj_set_style_bg_color(btn, lv_color_hex(COLOR_BTN_PRESSED), LV_STATE_PRESSED);

    /* 内容容器（紧凑布局：图标+文字靠左排列）
     * 关键修复：使用 lv_obj_clear_flag() 移除 CLICKABLE 属性，防止 container 拦截点击事件！
     */
    lv_obj_t *container = lv_obj_create(btn);
    lv_obj_remove_style_all(container);
    lv_obj_set_size(container, lv_pct(100), lv_pct(100));
    lv_obj_clear_flag(container, LV_OBJ_FLAG_CLICKABLE);  // ← LVGL 8.x 兼容写法
    lv_obj_set_style_pad_all(container, 12, 0);           /* 减小内边距 */
    lv_obj_set_style_pad_left(container, 15, 0);
    lv_obj_set_style_pad_right(container, 15, 0);
    lv_obj_set_style_border_width(container, 0, 0);
    lv_obj_set_style_bg_opa(container, LV_OPA_TRANSP, 0);

    /* 左侧图标（32x32 嵌入式位图，替代原 LV_SYMBOL 文字图标） */
    lv_obj_t *icon_img = lv_image_create(container);
    lv_image_set_src(icon_img, icon);
    lv_obj_align(icon_img, LV_ALIGN_LEFT_MID, 0, 0);

    /* 右侧文字标签（距图标右侧 20px，使用 i18n 统一字体） */
    lv_obj_t *text_label = lv_label_create(container);
    lv_obj_remove_style_all(text_label);
    lv_label_set_text(text_label, text);
    i18n_apply_font(text_label);
    lv_obj_set_style_text_color(text_label, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_align(text_label, LV_ALIGN_LEFT_MID, 32 + 20, 0);  /* 图标宽32px + 间距20px */

    /* 绑定点击事件 */
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
}

/* ===== 主入口：创建菜单页面 =====
 * UI 设计参考：
 * - 深色背景（#1A1A2E）
 * - 白色文字和图标
 * - 简洁的垂直列表
 * - 顶部标题区域
 * - 底部返回提示
 */

void create_menu_page(lv_obj_t *parent)
{
    /* 深色背景 */
    lv_obj_set_style_bg_color(parent, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);
    /* 清除默认padding和边框，让内容铺满整个屏幕 */
    lv_obj_set_style_pad_all(parent, 0, 0);
    lv_obj_set_style_border_width(parent, 0, 0);
    /* 清除所有滚动相关标志，参考 wifi_page.c 和 ai_page.c 的成功做法 */
    /* 关键修复：必须同时清除 SCROLL_ELASTIC 和 SCROLL_MOMENTUM！
    /* 只清 SCROLLABLE 不够，ELASTIC 和 MOMENTUM 会让 LVGL 仍把手势当滚动处理，
     * 从而拦截手势事件不让其触发 LV_EVENT_GESTURE */
    lv_obj_clear_flag(parent, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(parent, LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_clear_flag(parent, LV_OBJ_FLAG_SCROLL_MOMENTUM);

    /* 顶部标题栏（固定不滚动） */
    lv_obj_t *title_bar = lv_obj_create(parent);
    lv_obj_remove_style_all(title_bar);
    lv_obj_set_size(title_bar, lv_pct(100), MENU_TITLE_HEIGHT);
    lv_obj_align(title_bar, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_opa(title_bar, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(title_bar, 0, 0);
    lv_obj_clear_flag(title_bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(title_bar, LV_OBJ_FLAG_GESTURE_BUBBLE);  /* 标题栏也冒泡手势 */
    lv_obj_add_event_cb(title_bar, menu_parent_gesture_cb, LV_EVENT_GESTURE, NULL);  /* 下滑返回（title_bar层级）*/

    /* 返回按钮（左上角） */
    lv_obj_t *back_btn = lv_btn_create(title_bar);
    lv_obj_remove_style_all(back_btn);
    lv_obj_set_size(back_btn, 36, 36);
    lv_obj_align(back_btn, LV_ALIGN_LEFT_MID, 10, 0);
    lv_obj_set_style_bg_opa(back_btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_radius(back_btn, 18, 0);

    lv_obj_t *back_icon = lv_label_create(back_btn);
    lv_obj_remove_style_all(back_icon);
    lv_label_set_text(back_icon, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(back_icon, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_set_style_text_font(back_icon, LV_FONT_DEFAULT, 0);
    lv_obj_center(back_icon);
    lv_obj_add_event_cb(back_btn, back_btn_click_cb, LV_EVENT_CLICKED, NULL);

    /* 标题栏时间显示（与主界面同步） */
    menu_time_label = lv_label_create(title_bar);
    lv_obj_remove_style_all(menu_time_label);
    lv_label_set_text(menu_time_label, "");
    i18n_apply_font(menu_time_label);
    lv_obj_set_style_text_color(menu_time_label, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_align(menu_time_label, LV_ALIGN_CENTER, 0, 0);

    /* 滚动容器：高度 = 实际屏幕高(LV_VER_RES) - 标题栏 - 底部留白
     * 之前硬编码 SCREEN_HEIGHT=320 导致 454 屏下方约 136px 空白未利用 */
    lv_obj_t *scroll_cont = lv_obj_create(parent);
    lv_obj_remove_style_all(scroll_cont);
    lv_obj_set_size(scroll_cont, lv_pct(100), LV_VER_RES - MENU_TITLE_HEIGHT - MENU_BOTTOM_MARGIN);
    lv_obj_set_pos(scroll_cont, 0, MENU_SCROLL_TOP);
    lv_obj_set_style_bg_opa(scroll_cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(scroll_cont, 0, 0);
    lv_obj_set_style_pad_all(scroll_cont, 0, 0);
    /* 双向滚动：用户可以自由上下滑动查看所有菜单项 */
    lv_obj_set_scroll_dir(scroll_cont, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(scroll_cont, LV_SCROLLBAR_MODE_OFF);
    /* 移除 CLICKABLE 避免容器拦截按钮点击事件 */
    lv_obj_clear_flag(scroll_cont, LV_OBJ_FLAG_CLICKABLE);

    /* 菜单按钮列表（6项）：纯功能入口，通过标题栏下滑返回主界面 */
    create_menu_item(scroll_cont, 0, &menu_ai,       i18n_get(STR_MENU_AI),       btn_ai_cb);
    create_menu_item(scroll_cont, 1, &menu_meeting,  i18n_get(STR_MENU_MEETING),  btn_meeting_cb);
    create_menu_item(scroll_cont, 2, &menu_capture,  i18n_get(STR_MENU_CAMERA),   btn_camera_cb);
    create_menu_item(scroll_cont, 3, &menu_recorder, i18n_get(STR_MENU_RECORDER), btn_recorder_cb);
    create_menu_item(scroll_cont, 4, &menu_player,   i18n_get(STR_MENU_PLAYER),   btn_player_cb);
    create_menu_item(scroll_cont, 5, &menu_setting,  i18n_get(STR_MENU_SETTINGS), btn_settings_cb);

    /* 进入界面时立即刷新时间（动画开始即触发，比 SCREEN_LOADED 更早） */
    lv_obj_add_event_cb(parent, menu_screen_load_cb, LV_EVENT_SCREEN_LOAD_START, NULL);

    /* 注册屏幕级手势：作为title_bar手势的备用方案
     * 当title_bar因parent清除滚动标志导致手势传播失败时，
     * 此处仍能捕获到下滑手势并触发返回操作 */
    lv_obj_add_event_cb(parent, menu_parent_gesture_cb, LV_EVENT_GESTURE, NULL);  /* 下滑返回（parent层级）*/

    /* 启动时间更新定时器（每秒刷新一次） */
    menu_update_time();
    menu_time_timer = lv_timer_create(menu_time_timer_cb, 1000, NULL);
}

/* 强制删除 menu_screen（切语言时调用，menu_page 无 SCREEN_UNLOADED 回调） */
void menu_page_force_delete(void)
{
    if (menu_time_timer) {
        lv_timer_del(menu_time_timer);
        menu_time_timer = NULL;
    }
    if (menu_screen) {
        lv_obj_del(menu_screen);
        menu_screen = NULL;
    }
    menu_time_label = NULL;
}