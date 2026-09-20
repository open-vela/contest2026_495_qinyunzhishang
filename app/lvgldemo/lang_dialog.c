/****************************************************************************
 * apps/examples/lvgldemo/lang_dialog.c
 *
 * 语言设置页面实现（独立页面，参考 camera_page 布局）：
 *   - 顶部标题栏：返回按钮 + 标题文字 + 时间
 *   - 内容区：语言选项列表（圆点标记选中状态）
 *   - 结果提示区：选项和按钮之间（3秒自动消失）
 *   - 底部按钮区：确认 / 取消
 *   - 全程在 lang_screen 内操作，右滑才退出到设置页
 *
 ****************************************************************************/

#include "lang_dialog.h"
#include "i18n.h"
#include "bt_call_handler.h"
#include "recorder_page.h"
#include "meeting_page.h"
#include "settings_page.h"
#include "camera_page.h"

extern lv_obj_t *camera_screen;  /* 全局屏幕指针，定义于 lvgldemo.c */
#include "menu_page.h"
#include "lvgl_dispatch.h"
#include "agent_config.h"

#include <nuttx/config.h>
#include <syslog.h>
#include <stdio.h>
#include <time.h>

/* ===== 布局参数（参考 camera_page） ===== */
#define LANG_TITLE_H         48      /* 标题栏高度 */
#define LANG_TITLE_MARGIN_T  8       /* 标题距顶部边距 */
#define LANG_OPTION_H        52      /* 选项行高 */
#define LANG_OPTION_LEFT     60      /* 选项距左侧边框60px（避免圆点被遮挡） */
#define LANG_OPTION_START_Y  70      /* 选项起始 Y */
#define LANG_OPTION_GAP      12      /* 选项间距 */
#define LANG_RADIO_R         9       /* 单选圆点半径 */
#define LANG_RADIO_TEXT_GAP  15      /* 圆点与文字间距15px */
#define LANG_RESULT_H        40      /* 结果提示区高度 */
#define LANG_RESULT_W        75      /* 结果提示区宽度百分比 */
#define LANG_BTN_AREA_H      56      /* 底部按钮区高度 */
#define LANG_BTN_AREA_BOT    50      /* 底部按钮区距底部50px（上提20px） */
#define LANG_BTN_H           42      /* 按钮高度 */
#define LANG_BTN_GAP         20      /* 两按钮间距20px */

/* 颜色定义 */
#define COLOR_TEXT_WHITE     0xFFFFFF
#define COLOR_BG_OPTION      0x000000  /* 选项背景色（黑色，与页面一致） */
#define COLOR_RADIO_SEL      0x4CAF50  /* 选中圆点颜色 */
#define COLOR_RADIO_UNSEL    0x666666  /* 未选中圆点颜色 */
#define COLOR_TEXT_HINT      0xAAAAAA  /* 提示文字颜色 */
#define COLOR_BTN_CONFIRM    0x0078D7  /* 确认按钮蓝 */

/* ===== 页面对象 ===== */
static lv_obj_t *lang_screen       = NULL;
static lv_obj_t *lang_title_label  = NULL;
static lv_obj_t *lang_time_label   = NULL;
static lv_obj_t *lang_result_area  = NULL;  /* 结果提示容器 */
static lv_obj_t *lang_result_lbl   = NULL;  /* 结果文字 */
static lv_timer_t *lang_time_timer = NULL;
static lv_timer_t *result_timer    = NULL;

/* 当前选中的语言（可能尚未确认） */
static lang_t s_selected_lang = LANG_ZH_CN;

/* 防抖标志：切语言期间禁用 */
static bool s_switching = false;

/* 是否已执行过切换（用于判断是否需要重建） */
static bool s_language_changed = false;

/* 拦截弹窗 */
static lv_obj_t *s_block_modal = NULL;

/* ===== 圆点指示器对象 ===== */
static lv_obj_t *s_radio_zh = NULL;
static lv_obj_t *s_radio_en = NULL;

/* ===== 前向声明 ===== */
static void lang_option_update_visual(void);
static void lang_do_confirm(void);
static void lang_show_inline_result(bool ok, const char *msg);
static void lang_clear_result(void);
static void lang_gesture_cb(lv_event_t *e);

/****************************************************************************
 * 语言选项点击回调（仅更新选中状态，不立即切换）
 ****************************************************************************/

static void lang_option_click_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    s_selected_lang = (lang_t)(intptr_t)lv_event_get_user_data(e);
    lang_option_update_visual();
}

/****************************************************************************
 * 圆点选中状态更新
 ****************************************************************************/

static void lang_option_update_visual(void)
{
    if (s_radio_zh)
    {
        bool sel = (s_selected_lang == LANG_ZH_CN);
        lv_obj_set_style_bg_color(s_radio_zh,
            sel ? lv_color_hex(COLOR_RADIO_SEL)
                : lv_color_hex(COLOR_RADIO_UNSEL), 0);
        lv_obj_set_style_bg_opa(s_radio_zh, LV_OPA_COVER, 0);
    }
    if (s_radio_en)
    {
        bool sel = (s_selected_lang == LANG_EN);
        lv_obj_set_style_bg_color(s_radio_en,
            sel ? lv_color_hex(COLOR_RADIO_SEL)
                : lv_color_hex(COLOR_RADIO_UNSEL), 0);
        lv_obj_set_style_bg_opa(s_radio_en, LV_OPA_COVER, 0);
    }
}

/****************************************************************************
 * 创建单个语言选项行（圆点 + 文字）
 ****************************************************************************/

static void create_lang_option_row(lv_obj_t *parent, int index,
                                    const char *text, lang_t lang,
                                    lv_obj_t **out_radio)
{
    int y = LANG_OPTION_START_Y + index * (LANG_OPTION_H + LANG_OPTION_GAP);

    /* 行容器 */
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_HOR_RES - 2 * LANG_OPTION_LEFT, LANG_OPTION_H);
    lv_obj_align(row, LV_ALIGN_TOP_LEFT, LANG_OPTION_LEFT, y);
    lv_obj_set_style_bg_color(row, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(row, 0, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    /* 手势冒泡到 lang_screen，确保选项行上的右滑能触发返回 */
    lv_obj_add_flag(row, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_set_style_layout(row, LV_LAYOUT_FLEX, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(row, LANG_RADIO_TEXT_GAP, 0);

    /* 点击整行触发选择 */
    lv_obj_add_event_cb(row, lang_option_click_cb, LV_EVENT_CLICKED,
                        (void *)(intptr_t)lang);
    lv_obj_add_event_cb(row, lang_gesture_cb, LV_EVENT_GESTURE, NULL);

    /* 左侧：单选圆点 */
    lv_obj_t *radio = lv_obj_create(row);
    lv_obj_remove_style_all(radio);
    lv_obj_set_size(radio, LANG_RADIO_R * 2, LANG_RADIO_R * 2);
    lv_obj_set_style_radius(radio, LANG_RADIO_R, 0);
    lv_obj_set_style_bg_color(radio, lv_color_hex(COLOR_RADIO_UNSEL), 0);
    lv_obj_set_style_bg_opa(radio, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(radio, 0, 0);
    lv_obj_clear_flag(radio, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    *out_radio = radio;

    /* 右侧：文字标签（强制 CJK 字体，确保英文模式下"简体中文"正常显示）*/
    lv_obj_t *label = lv_label_create(row);
    lv_obj_remove_style_all(label);
    lv_label_set_text(label, text);
    {
        lv_font_t *cjk = i18n_get_cjk_font();
        if (cjk) lv_obj_set_style_text_font(label, cjk, 0);
        else i18n_apply_font(label);
    }
    lv_obj_set_style_text_color(label, lv_color_hex(COLOR_TEXT_WHITE), 0);
}

/****************************************************************************
 * 结果提示（内嵌在页面中，3秒自动消失）
 ****************************************************************************/

static void result_timeout_cb(lv_timer_t *t)
{
    lv_timer_del(t);
    result_timer = NULL;
    lang_clear_result();
}

static void lang_clear_result(void)
{
    if (lang_result_lbl)
    {
        lv_label_set_text(lang_result_lbl, "");
    }
    if (lang_result_area)
    {
        lv_obj_add_flag(lang_result_area, LV_OBJ_FLAG_HIDDEN);
    }
}

static void lang_show_inline_result(bool ok, const char *msg)
{
    if (!lang_result_area || !lang_result_lbl)
    {
        return;
    }

    /* 显示结果区域 */
    lv_obj_clear_flag(lang_result_area, LV_OBJ_FLAG_HIDDEN);

    /* 设置结果文字 */
    lv_label_set_text(lang_result_lbl, msg);
    /* 成功：白字；失败：红字 */
    lv_obj_set_style_text_color(lang_result_lbl,
        ok ? lv_color_hex(COLOR_TEXT_WHITE) : lv_color_hex(0xFF5555), 0);
    /* 用 i18n_apply_font 保持和页面文字一致的字体大小（不强制 28px CJK） */
    i18n_apply_font(lang_result_lbl);

    /* 清除旧的定时器 */
    if (result_timer)
    {
        lv_timer_del(result_timer);
        result_timer = NULL;
    }

    /* 3秒后自动消失 */
    result_timer = lv_timer_create(result_timeout_cb, 3000, NULL);
    lv_timer_set_repeat_count(result_timer, 1);
}

/****************************************************************************
 * 确认按钮逻辑（核心修改：不离开 lang_screen）
 ****************************************************************************/

static void lang_do_confirm(void)
{
    if (s_switching)
    {
        return;
    }

    lang_t target = s_selected_lang;
    lang_t current = i18n_get_lang();

    /* 若未改变：提示当前已是该语言，无需重复设置 */
    if (target == current)
    {
        const char *hint = (target == LANG_ZH_CN)
            ? i18n_get(STR_LANG_ALREADY_ZH)
            : i18n_get(STR_LANG_ALREADY_EN);
        lang_show_inline_result(true, hint);
        return;
    }

    /* 状态拦截 */
    if (bt_call_handler_get_call_state() != BT_CALL_IDLE)
    {
        lang_show_block(STR_BLOCK_CALL);
        return;
    }
    if (recorder_is_recording())
    {
        lang_show_block(STR_BLOCK_RECORDER);
        return;
    }
    if (meeting_is_active())
    {
        lang_show_block(STR_BLOCK_MEETING);
        return;
    }

    s_switching = true;

    /* 1. 执行语言切换（保存配置 + 广播给主页刷新字体）
     * 注意：i18n_set_lang 内部不会删除 CJK 字体（见下方说明），
     *       避免 lang_screen 上的 label 引用已释放的字体导致死机 */
    bool ok = i18n_set_lang(target);

    /* 2. 在 lang_screen 内显示结果提示（不离开当前页面！）*/
    lang_show_inline_result(ok,
        ok ? i18n_get(STR_LANG_SWITCH_OK) : i18n_get(STR_LANG_SWITCH_FAIL));

    /* 3. 标记语言已变更（退出时再重建其他页面） */
    if (ok)
    {
        s_language_changed = true;
    }

    /* 4. 解除防抖（允许再次切换） */
    s_switching = false;
}

static void lang_confirm_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    lang_do_confirm();
}

/****************************************************************************
 * 底部按钮区创建
 ****************************************************************************/

static void create_bottom_buttons(lv_obj_t *parent)
{
    /* 按钮区容器：距底30px，水平居中 */
    lv_obj_t *btn_area = lv_obj_create(parent);
    lv_obj_remove_style_all(btn_area);
    lv_obj_set_height(btn_area, LANG_BTN_H);
    lv_obj_set_width(btn_area, LV_SIZE_CONTENT);
    lv_obj_align(btn_area, LV_ALIGN_BOTTOM_MID, 0, -LANG_BTN_AREA_BOT);
    lv_obj_set_style_bg_opa(btn_area, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(btn_area, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(btn_area, LV_OBJ_FLAG_GESTURE_BUBBLE);  /* 手势冒泡到 lang_screen */
    lv_obj_set_style_layout(btn_area, LV_LAYOUT_FLEX, 0);
    lv_obj_set_flex_flow(btn_area, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn_area, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(btn_area, LANG_BTN_GAP, 0);
    lv_obj_add_event_cb(btn_area, lang_gesture_cb, LV_EVENT_GESTURE, NULL);

    /* 确认按钮：宽度自适应文字 */
    lv_obj_t *confirm_btn = lv_button_create(btn_area);
    lv_obj_remove_style_all(confirm_btn);
    lv_obj_set_height(confirm_btn, LANG_BTN_H);
    lv_obj_set_width(confirm_btn, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(confirm_btn, 20, 0);
    lv_obj_set_style_bg_color(confirm_btn, lv_color_hex(COLOR_BTN_CONFIRM), 0);
    lv_obj_set_style_bg_opa(confirm_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(confirm_btn, 8, 0);
    lv_obj_add_event_cb(confirm_btn, lang_confirm_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(confirm_btn, lang_gesture_cb, LV_EVENT_GESTURE, NULL);
    lv_obj_add_flag(confirm_btn, LV_OBJ_FLAG_GESTURE_BUBBLE);  /* 手势冒泡 */

    lv_obj_t *confirm_lbl = lv_label_create(confirm_btn);
    lv_label_set_text(confirm_lbl, i18n_get(STR_LANG_CONFIRM));
    i18n_apply_font(confirm_lbl);
    lv_obj_set_style_text_color(confirm_lbl, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_center(confirm_lbl);
}

/****************************************************************************
 * 结果提示区创建（位于选项和按钮之间）
 ****************************************************************************/

static void create_result_area(lv_obj_t *parent)
{
    /* 计算Y位置：固定在底部按钮区上方 20px 处 */
    int btn_top = LV_VER_RES - LANG_BTN_AREA_H - LANG_BTN_AREA_BOT;
    int result_y = btn_top - 20 - LANG_RESULT_H;

    lang_result_area = lv_obj_create(parent);
    lv_obj_remove_style_all(lang_result_area);
    lv_obj_set_size(lang_result_area, LV_PCT(LANG_RESULT_W), LANG_RESULT_H);
    lv_obj_align(lang_result_area, LV_ALIGN_TOP_MID, 0, result_y);
    lv_obj_set_style_bg_color(lang_result_area, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(lang_result_area, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(lang_result_area, 8, 0);
    lv_obj_set_style_border_width(lang_result_area, 0, 0);
    lv_obj_clear_flag(lang_result_area,
                      LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    /* 默认隐藏 */
    lv_obj_add_flag(lang_result_area, LV_OBJ_FLAG_HIDDEN);

    lang_result_lbl = lv_label_create(lang_result_area);
    lv_obj_remove_style_all(lang_result_lbl);
    lv_label_set_text(lang_result_lbl, "");
    i18n_apply_font(lang_result_lbl);
    lv_obj_set_style_text_color(lang_result_lbl, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_center(lang_result_lbl);
}

/****************************************************************************
 * 导航：右滑返回设置页（退出时检查是否需要重建）
 ****************************************************************************/

static void lang_exit_to_settings(void)
{
    bool need_rebuild = s_language_changed;
    s_language_changed = false;

    if (need_rebuild)
    {
        /* bug3/4 根因修复：
         * 1. 不用异步重建（lang_rebuild_async_cb），异步在 timer handler 中
         *    执行会导致 LVGL 状态不一致崩溃（见 2800串口.log 崩溃栈）。
         * 2. 不调 i18n_rebuild_current_page()，因为它会 menu_page_force_delete()
         *    删除 menu_screen 并置 NULL，导致 settings_back_to_menu() 中
         *    if(menu_screen) 为 false 无法右滑回菜单。
         * 方案：同步只重建 settings_screen 和 menu_screen（文字会随语言刷新），
         *       meeting_page 从 menu 进入时会重建。
         */
        settings_page_deinit();
        settings_screen = lv_obj_create(NULL);
        lv_obj_set_style_bg_color(settings_screen, lv_color_hex(0x000000), 0);
        lv_obj_set_style_bg_opa(settings_screen, LV_OPA_COVER, 0);
        lv_obj_clear_flag(settings_screen, LV_OBJ_FLAG_SCROLLABLE);
        create_settings_page(settings_screen);

        /* bug3: 重建 menu_screen，让菜单页文字随语言刷新。
         * menu_page 和 meeting_page 没注册 lang_changed_cb，需重建才刷新。 */
        if (menu_screen) {
            menu_page_force_delete();
        }
        menu_screen = lv_obj_create(NULL);
        lv_obj_set_style_bg_color(menu_screen, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(menu_screen, LV_OPA_COVER, 0);
        lv_obj_clear_flag(menu_screen, LV_OBJ_FLAG_SCROLLABLE);
        create_menu_page(menu_screen);

        /* Bug1/2/3: 销毁各功能页屏幕，使其下次进入时重建以刷新语言文案。
         * ai/meeting 的 deinit 会删除屏幕并置空全局指针；
         * camera_page_deinit 删除 cam_screen_obj 但无法置空全局 camera_screen，需手动置空。 */
        ai_page_deinit();
        meeting_page_deinit();
        camera_page_deinit();
        camera_screen = NULL; lv_scr_load_anim(settings_screen, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 300, 0, false);
        /* 广播语言变更，通知 main_screen 刷新字体（延迟广播机制） */
        i18n_flush_lang_changed();
    }
    else
    {
        /* 未变更语言：直接返回设置页。
         * Bug1 修复：进入 lang 页时 settings_screen 被卸载回调异步销毁置空，
         *           此处需重建，否则右滑返回无响应（无论是否设置语言都可返回）。 */
        if (settings_screen == NULL)
        {
            settings_screen = lv_obj_create(NULL);
            lv_obj_set_style_bg_color(settings_screen, lv_color_hex(0x000000), 0);
            lv_obj_set_style_bg_opa(settings_screen, LV_OPA_COVER, 0);
            lv_obj_clear_flag(settings_screen, LV_OBJ_FLAG_SCROLLABLE);
            create_settings_page(settings_screen);
        }
        lv_scr_load_anim(settings_screen, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 300, 0, false);
    }
}

static void lang_back_btn_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    lang_exit_to_settings();
}

static void lang_gesture_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    lv_indev_t *indev = lv_indev_get_act();
    if (!indev) return;
    lv_dir_t dir = lv_indev_get_gesture_dir(indev);
    if (dir == LV_DIR_RIGHT)
    {
        /* 不调 lv_indev_wait_release，它会阻塞事件循环导致手势处理异常。
         * 直接切换屏幕，参考 camera_page 的 cam_gesture_cb 做法。 */
        lang_exit_to_settings();
    }
}

/****************************************************************************
 * 时间显示
 ****************************************************************************/

static void lang_update_time(void)
{
    struct tm time_info;
    char buf[16];

    time_info = agent_localtime();
    snprintf(buf, sizeof(buf), "%02d:%02d",
             time_info.tm_hour, time_info.tm_min);

    if (lang_time_label)
    {
        lv_label_set_text(lang_time_label, buf);
    }
}

static void lang_time_timer_cb(lv_timer_t *timer)
{
    LV_UNUSED(timer);
    lang_update_time();
}

/****************************************************************************
 * 拦截提示弹窗（手动关闭）
 ****************************************************************************/

static void lang_block_ok_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    if (s_block_modal)
    {
        lv_obj_del(s_block_modal);
        s_block_modal = NULL;
    }
}

void lang_show_block(int msg_id)
{
    if (s_block_modal)
    {
        lv_obj_del(s_block_modal);
        s_block_modal = NULL;
    }

    s_block_modal = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(s_block_modal);
    lv_obj_set_size(s_block_modal, LV_PCT(100), LV_PCT(100));
    lv_obj_center(s_block_modal);
    lv_obj_set_style_bg_color(s_block_modal, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_block_modal, LV_OPA_50, 0);
    lv_obj_clear_flag(s_block_modal, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *card = lv_obj_create(s_block_modal);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, LV_PCT(75), LV_PCT(45));
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x1A1A2E), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 12, 0);
    lv_obj_set_style_pad_all(card, 15, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_layout(card, LV_LAYOUT_FLEX, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *msg = lv_label_create(card);
    lv_label_set_text(msg, i18n_get(msg_id));
    i18n_apply_font(msg);
    lv_obj_set_style_text_color(msg, lv_color_white(), 0);
    lv_label_set_long_mode(msg, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(msg, LV_PCT(90));

    lv_obj_t *ok_btn = lv_button_create(card);
    lv_obj_set_size(ok_btn, LV_PCT(60), 45);
    lv_obj_set_style_radius(ok_btn, 22, 0);
    lv_obj_set_style_bg_color(ok_btn, lv_color_hex(0x4CAF50), 0);
    lv_obj_set_style_bg_opa(ok_btn, LV_OPA_COVER, 0);
    lv_obj_add_event_cb(ok_btn, lang_block_ok_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *ok_label = lv_label_create(ok_btn);
    lv_label_set_text(ok_label, i18n_get(STR_BLOCK_OK));
    i18n_apply_font(ok_label);
    lv_obj_set_style_text_color(ok_label, lv_color_white(), 0);
    lv_obj_center(ok_label);
}

/****************************************************************************
 * 页面生命周期
 ****************************************************************************/

static void lang_deinit_async_cb(void *arg)
{
    LV_UNUSED(arg);
    lang_page_deinit();
}

static void lang_screen_unloaded_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    lv_indev_reset(NULL, NULL);
    lvgl_dispatch_async(lang_deinit_async_cb, NULL);
}

void lang_page_deinit(void)
{
    /* 清理定时器 */
    if (lang_time_timer)
    {
        lv_timer_del(lang_time_timer);
        lang_time_timer = NULL;
    }
    if (result_timer)
    {
        lv_timer_del(result_timer);
        result_timer = NULL;
    }

    /* 清理引用 */
    lang_title_label = NULL;
    lang_time_label = NULL;
    lang_result_area = NULL;
    lang_result_lbl = NULL;
    s_radio_zh = NULL;
    s_radio_en = NULL;

    /* 销毁屏幕 */
    if (lang_screen)
    {
        lv_obj_del(lang_screen);
        lang_screen = NULL;
    }

    /* 重置状态 */
    s_switching = false;
    s_language_changed = false;
}

/****************************************************************************
 * 主入口：创建语言设置页面（参考 camera_page 布局）
 ****************************************************************************/

void lang_page_enter(void)
{
    if (lang_screen || s_switching)
    {
        return;
    }

    /* 重置状态 */
    s_selected_lang = i18n_get_lang();
    s_switching = false;
    s_language_changed = false;

    /* 创建新屏幕 */
    lang_screen = lv_obj_create(NULL);

    /* 背景色：深色主题（与 camera_page 一致） */
    lv_obj_set_style_bg_color(lang_screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(lang_screen, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(lang_screen, 0, 0);
    lv_obj_set_style_border_width(lang_screen, 0, 0);
    lv_obj_clear_flag(lang_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(lang_screen, LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_clear_flag(lang_screen, LV_OBJ_FLAG_SCROLL_MOMENTUM);
    /* 设置可点击，确保屏幕级手势能触发（右滑返回） */
    lv_obj_add_flag(lang_screen, LV_OBJ_FLAG_CLICKABLE);

    /* ===== 1. 顶部标题栏（参考 camera_page） ===== */
    lv_obj_t *title_bar = lv_obj_create(lang_screen);
    lv_obj_remove_style_all(title_bar);
    lv_obj_set_size(title_bar, LV_PCT(100), LANG_TITLE_H);
    lv_obj_align(title_bar, LV_ALIGN_TOP_MID, 0, LANG_TITLE_MARGIN_T);
    lv_obj_set_style_bg_opa(title_bar, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(title_bar, 0, 0);
    lv_obj_clear_flag(title_bar, LV_OBJ_FLAG_SCROLLABLE);
    /* 手势冒泡到 lang_screen，确保标题栏区域的右滑能触发返回（参考 menu_page 做法） */
    lv_obj_add_flag(title_bar, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(title_bar, lang_gesture_cb, LV_EVENT_GESTURE, NULL);

    /* 返回按钮（左上角） */
    lv_obj_t *back_btn = lv_button_create(title_bar);
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
    lv_obj_add_event_cb(back_btn, lang_back_btn_cb, LV_EVENT_CLICKED, NULL);

    /* 标题文字（居中） */
    lang_title_label = lv_label_create(title_bar);
    lv_obj_remove_style_all(lang_title_label);
    lv_label_set_text(lang_title_label, i18n_get(STR_LANG_DIALOG_TITLE));
    i18n_apply_font(lang_title_label);
    lv_obj_set_style_text_color(lang_title_label, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_align(lang_title_label, LV_ALIGN_CENTER, 0, 0);

    /* 时间显示（右上角） */
    lang_time_label = lv_label_create(title_bar);
    lv_obj_remove_style_all(lang_time_label);
    lv_label_set_text(lang_time_label, "");
    i18n_apply_font(lang_time_label);
    lv_obj_set_style_text_color(lang_time_label, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_align(lang_time_label, LV_ALIGN_RIGHT_MID, -10, 0);

    /* ===== 2. 语言选项列表（圆点 + 文字） ===== */
    create_lang_option_row(lang_screen, 0, i18n_get(STR_LANG_ZH_CN_LABEL),
                           LANG_ZH_CN, &s_radio_zh);
    create_lang_option_row(lang_screen, 1, i18n_get(STR_LANG_EN_LABEL),
                           LANG_EN, &s_radio_en);

    /* 设置初始选中状态 */
    lang_option_update_visual();

    /* ===== 3. 结果提示区（默认隐藏，选项和按钮之间） ===== */
    create_result_area(lang_screen);

    /* ===== 4. 底部确认/取消按钮 ===== */
    create_bottom_buttons(lang_screen);

    /* ===== 屏幕级手势：右滑返回 ===== */
    lv_obj_add_event_cb(lang_screen, lang_gesture_cb, LV_EVENT_GESTURE, NULL);
    lv_obj_add_event_cb(lang_screen, lang_screen_unloaded_cb,
                        LV_EVENT_SCREEN_UNLOADED, NULL);

    /* ===== 启动时间定时器 ===== */
    lang_update_time();
    lang_time_timer = lv_timer_create(lang_time_timer_cb, 60000, NULL);

    /* 加载动画进入 */
    lv_scr_load_anim(lang_screen, LV_SCR_LOAD_ANIM_MOVE_LEFT, 300, 0, false);
}
