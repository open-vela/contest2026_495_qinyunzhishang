/****************************************************************************
 * apps/examples/lvgldemo/camera_ai_page.c
 *
 * Camera AI analysis chat page implementation
 *
 ****************************************************************************/

#include <nuttx/config.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <syslog.h>
#include <lvgl/lvgl.h>

#include "camera_ai_page.h"
#include "voice_assistant.h"
#include "lvgl_dispatch.h"
#include "camera_page.h"
#include "channels/feishu_bot.h"
#include "i18n.h"

/* 外部麦克风图标资源 - 三种状态分别对应不同颜色图标 */
extern const lv_image_dsc_t mic_white;  /* 空闲状态 */
extern const lv_image_dsc_t mic_blue;  /* AI回复中 */
extern const lv_image_dsc_t mic_green; /* 用户说话中 */

/* ── 样式常量 ───────────────────────────────────────────────────── */
#define CAI_FONT_PATH            "/emmc/font/MiSans-Normal.ttf"
#define CAI_TITLE_FONT_SIZE      28   /* 和camera页面标题字号一致 */
#define CAI_BUBBLE_FONT_SIZE     20   /* 和 ai_page 气泡字号一致 */
#define CAI_TITLE_MARGIN_TOP     40   /* 和camera页面标题位置一致 */
#define CAI_MIC_MARGIN_BOTTOM    5   /* 麦克风距底部边距，整体下移20px */
#define CAI_MIC_SIZE             48   /* 图标大小匹配mic_white 48x48 */
#define CAI_BUBBLE_MAX_WIDTH     300  /* 最大气泡宽度，小于行宽366px，留出左右间距 */
#define CAI_BUBBLE_PADDING       16
#define CAI_BUBBLE_RADIUS        16
#define CAI_BUBBLE_SPACING       12
#define CAI_SCREEN_PADDING_H     50   /* 左右边距50px，气泡不会贴到圆弧边缘 */
#define CAI_COLOR_AI_BG          lv_color_hex(0x2e2e2c)
#define CAI_COLOR_AI_TEXT        lv_color_white()
#define CAI_COLOR_USER_BG        lv_color_hex(0x98ee6b)
#define CAI_COLOR_USER_TEXT      lv_color_black()
#define CAI_COLOR_BG             lv_color_black()
#define CAI_COLOR_MIC_IDLE       lv_color_white()
#define CAI_COLOR_MIC_RECORDING  lv_color_hex(0x98ee6b)
#define CAI_COLOR_MIC_PLAYING    lv_color_hex(0x888888)

/* ── 消息结构 ────────────────────────────────────────────────────── */
typedef enum {
    CAI_MSG_AI = 0,
    CAI_MSG_USER = 1
} cai_msg_type_t;

typedef struct {
    cai_msg_type_t type;
    char *text;
    lv_obj_t *bubble;
    bool is_analyzing;
} cai_msg_t;

/* ── 页面上下文 ──────────────────────────────────────────────────── */
static lv_obj_t *g_cai_screen = NULL;
static lv_obj_t *g_cai_chat_scroll = NULL;
static lv_obj_t *g_cai_mic_icon = NULL;
static lv_font_t *g_cai_title_font = NULL;  /* 标题字体，单独加载28px */
static lv_font_t *g_cai_font = NULL;        /* 气泡字体，24px */
static cai_msg_t *g_cai_msgs = NULL;
static int g_cai_msg_count = 0;
static int g_cai_msg_cap = 0;
static bool g_cai_active = false;
static bool g_cai_mic_opened = false;  /* 首次 AI 回复后置 true，仅开一次 mic */
static char g_cai_image_path[256] = {0};

/* ── 字体加载辅助 ────────────────────────────────────────────────── */
#if LV_USE_FREETYPE
static lv_font_t *cai_load_font(const char *path, lv_coord_t size)
{
    lv_font_t *font = lv_freetype_font_create(path, LV_FREETYPE_FONT_RENDER_MODE_BITMAP,
                                               size, LV_FREETYPE_FONT_STYLE_NORMAL);
    if (!font) {
        syslog(LOG_WARNING, "[cai_page] Failed to load FreeType font: %s size=%ld\n", path, (long)size);
        /* 返回 NULL，让 cai_apply_font 走 fallback 链（CJK 字体），
         * 避免返回 montserrat 导致中文标题显示方格 */
        return NULL;
    }
    return font;
}
#endif

static void cai_apply_font(lv_obj_t *obj, bool is_title)
{
    if (is_title) {
        /* 标题：优先用共享 28px（i18n_get_cjk_font），fallback 页面私有 */
        lv_font_t *cjk = i18n_get_cjk_font();
        if (cjk) {
            lv_obj_set_style_text_font(obj, cjk, 0);
            return;
        }
#if LV_USE_FREETYPE
        if (g_cai_title_font) {
            lv_obj_set_style_text_font(obj, g_cai_title_font, 0);
            return;
        }
#endif
    } else {
        /* 气泡：优先用页面私有字体（由 CAI_BUBBLE_FONT_SIZE 加载，与 ai_page 一致），
         * fallback 共享 24px。g_cai_font 为静态单例（销毁不删），不会被缓存淘汰变方格。 */
#if LV_USE_FREETYPE
        if (g_cai_font) {
            lv_obj_set_style_text_font(obj, g_cai_font, 0);
            return;
        }
#endif
        lv_font_t *cjk = i18n_get_cjk_font_small();
        if (cjk) {
            lv_obj_set_style_text_font(obj, cjk, 0);
            return;
        }
    }
    /* 最终 fallback */
#if LV_FONT_SIMSUN_16_CJK
    lv_obj_set_style_text_font(obj, &lv_font_simsun_16_cjk, 0);
#else
    lv_obj_set_style_text_font(obj, LV_FONT_DEFAULT, 0);
#endif
}

/* ── 气泡创建 ────────────────────────────────────────────────────── */
static void cai_remove_message(int index)
{
    if (index < 0 || index >= g_cai_msg_count) return;

    cai_msg_t *msg = &g_cai_msgs[index];
    if (msg->text) {
        free(msg->text);
        msg->text = NULL;
    }
    if (msg->bubble) {
        lv_obj_t *row = lv_obj_get_parent(msg->bubble);
        if (row) {
            lv_obj_delete(row);
        } else {
            lv_obj_delete(msg->bubble);
        }
        msg->bubble = NULL;
    }

    for (int i = index + 1; i < g_cai_msg_count; i++) {
        g_cai_msgs[i - 1] = g_cai_msgs[i];
    }
    g_cai_msg_count--;
}

/* 删除"正在分析图片..."提示气泡（按 is_analyzing 标记识别，不依赖语言文案）。
 * 幂等：找不到提示消息时返回 false，调用方可重复调用。
 * 调用时机：
 *   1) cai_on_dialog_state 收到 VA_STATE_AI_SPEAKING 时（TTS 开始播报即删，主路径）
 *   2) cai_on_ai_reply 首次回复时（兜底，防止状态回调丢失或时序异常）
 * 用例返回中文/英文文案都能被正确删除。 */
static bool cai_remove_analyzing_hint(void)
{
    for (int i = 0; i < g_cai_msg_count; i++) {
        if (g_cai_msgs[i].is_analyzing) {
            cai_remove_message(i);
            return true;
        }
    }
    return false;
}

static void cai_add_message(cai_msg_type_t type, const char *text)
{
    if (!g_cai_active || !text) return;

    /* 扩展消息数组 */
    if (g_cai_msg_count >= g_cai_msg_cap) {
        int new_cap = g_cai_msg_cap == 0 ? 8 : g_cai_msg_cap * 2;
        cai_msg_t *new_msgs = realloc(g_cai_msgs, new_cap * sizeof(cai_msg_t));
        if (!new_msgs) return;
        g_cai_msgs = new_msgs;
        g_cai_msg_cap = new_cap;
    }

    cai_msg_t *msg = &g_cai_msgs[g_cai_msg_count++];
    msg->type = type;
    msg->text = strdup(text);
    msg->is_analyzing = false;
    if (!msg->text) {
        g_cai_msg_count--;
        return;
    }

    /* 创建行容器，用于左/右对齐 */
    lv_obj_t *row = lv_obj_create(g_cai_chat_scroll);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);  /* 行高度自适应内容 */
    lv_obj_set_style_pad_top(row, CAI_BUBBLE_SPACING/2, 0);
    lv_obj_set_style_pad_bottom(row, CAI_BUBBLE_SPACING/2, 0);
    lv_obj_set_style_pad_left(row, 0, 0);
    lv_obj_set_style_pad_right(row, 0, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    if (type == CAI_MSG_AI) {
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
    } else {
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END);
    }
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    /* 创建气泡容器 - 宽高完全自适应内容 */
    msg->bubble = lv_obj_create(row);
    lv_obj_remove_style_all(msg->bubble);
    lv_obj_set_style_bg_opa(msg->bubble, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(msg->bubble, CAI_BUBBLE_RADIUS, 0);
    lv_obj_set_style_pad_all(msg->bubble, CAI_BUBBLE_PADDING, 0);
    lv_obj_set_width(msg->bubble, LV_SIZE_CONTENT);   /* 宽度随文字长度变化 */
    lv_obj_set_height(msg->bubble, LV_SIZE_CONTENT);  /* 高度随文字行数变化 */
    lv_obj_clear_flag(msg->bubble, LV_OBJ_FLAG_SCROLLABLE);

    /* 创建文字label - 宽度自适应内容，max_width限制换行阈值 */
    lv_obj_t *label = lv_label_create(msg->bubble);
    lv_label_set_text(label, text);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(label, LV_SIZE_CONTENT);  /* 宽度随文字长度自适应 */
    lv_obj_set_style_max_width(label, CAI_BUBBLE_MAX_WIDTH - CAI_BUBBLE_PADDING * 2, 0);  /* 超过最大宽度自动换行 */

    if (type == CAI_MSG_AI) {
        lv_obj_set_style_bg_color(msg->bubble, CAI_COLOR_AI_BG, 0);
        lv_obj_set_style_text_color(label, CAI_COLOR_AI_TEXT, 0);
    } else {
        lv_obj_set_style_bg_color(msg->bubble, CAI_COLOR_USER_BG, 0);
        lv_obj_set_style_text_color(label, CAI_COLOR_USER_TEXT, 0);
    }
    cai_apply_font(label, false);
    /* 更新布局确保坐标/高度准确后再计算滚动目标 */
    lv_obj_update_layout(msg->bubble);
    lv_obj_update_layout(g_cai_chat_scroll);
    /* 智能滚动：长消息(高度超过可视区)顶部对齐便于从头阅读；
     * 短消息滚到底部完整显示最新内容，保持聊天底部对齐体验。
     * 复用函数开头创建的 row 变量(msg->bubble 的父对象) */
    lv_coord_t row_h = lv_obj_get_height(row);
    lv_coord_t pad_top = lv_obj_get_style_pad_top(g_cai_chat_scroll, 0);
    lv_coord_t pad_bottom = lv_obj_get_style_pad_bottom(g_cai_chat_scroll, 0);
    lv_coord_t view_h = lv_obj_get_height(g_cai_chat_scroll) - pad_top - pad_bottom;
    lv_coord_t row_content_y = lv_obj_get_y(row) - pad_top;  /* row 在滚动内容中的逻辑纵坐标 */
    lv_coord_t target;
    if (row_h > view_h) {
        target = row_content_y;                   /* 长消息：顶部对齐，从头阅读 */
    } else {
        target = row_content_y + row_h - view_h;  /* 短消息：底部对齐，完整显示 */
    }
    if (target < 0) target = 0;
    lv_obj_scroll_to_y(g_cai_chat_scroll, target, LV_ANIM_OFF);  /* scroll_to_y 会自动 clamp 到合法范围 */
}

/* ── 状态回调 ────────────────────────────────────────────────────── */
static void cai_on_user_asr(const char *text)
{
    if (!g_cai_active) return;
    cai_add_message(CAI_MSG_USER, text);
}

static void cai_on_ai_reply(const char *text)
{
    if (!g_cai_active) return;

    /* 兜底：若状态回调未提前删除"正在分析图片..."提示，这里再删一次。
     * 正常情况下 cai_on_dialog_state(VA_STATE_AI_SPEAKING) 已删除，此处为 no-op。 */
    if (!g_cai_mic_opened) {
        cai_remove_analyzing_hint();
    }

    cai_add_message(CAI_MSG_AI, text);

    /* 首次 AI 回复后再开启 mic，进入多轮语音对话。
     * 进入 camera 页时已 suspend 监听（VAD+云端 mic 关闭），
     * 这里仅在首次回复时 enter_dialogue 打开云端 mic，避免图片分析期间录音干扰。
     * 同时 resume_listening 清除 suspend 标志：camera_ai_page 是聊天页，
     * 需让 30s 静音超时后 va_maybe_start_wakeup() 能正常重启本地 VAD，
     * 否则 suspend=true 会导致 VAD 重启被跳过，无法再次"小Q小Q"唤醒。 */
    if (!g_cai_mic_opened) {
        g_cai_mic_opened = true;
        voice_assistant_resume_listening();
        voice_assistant_enter_dialogue();
    }
}

/* 根据对话状态控制麦克风图标可见性：
 * 仅用户说话时显示白色 mic 图标；AI回复/空闲等均隐藏。 */
static void cai_set_mic_icon(va_dialog_state_t state)
{
    if (!g_cai_mic_icon) return;
    if (state == VA_STATE_USER_SPEAKING) {
        lv_img_set_src(g_cai_mic_icon, &mic_white);
        lv_obj_clear_flag(g_cai_mic_icon, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(g_cai_mic_icon, LV_OBJ_FLAG_HIDDEN);
    }
}

static void cai_on_dialog_state(va_dialog_state_t state)
{
    if (!g_cai_active) return;
    /* AI 开始 TTS 播报即删除"正在分析图片..."提示，避免语音已响但提示仍留屏的违和感。
     * 函数幂等，状态多次进入也只会删一次。 */
    if (state == VA_STATE_AI_SPEAKING) {
        cai_remove_analyzing_hint();
    }
    cai_set_mic_icon(state);
}

/* 图片分析状态回调 */
static void cai_on_image_status(const char *status, bool done)
{
    (void)done;
    if (!g_cai_active) return;
    /* 分析开始已经显示"正在分析图片..."，后续状态如果是结果会通过AI reply回调追加 */
    if (status && strstr(status, "完成") == NULL && strstr(status, "done") == NULL) {
        syslog(LOG_INFO, "[cai_page] Image status: %s\n", status);
    }
}

/* ── 右滑返回手势 ────────────────────────────────────────────────── */
static void cai_gesture_cb(lv_event_t *e)
{
    (void)e;
    lv_indev_t *indev = lv_indev_get_act();
    if (!indev) return;
    lv_dir_t dir = lv_indev_get_gesture_dir(indev);
    if (dir == LV_DIR_RIGHT) {
        if (indev) lv_indev_wait_release(indev);
        /* C状态：只切屏回 camera，不销毁 AI 页，保留聊天记录与对话状态。
         * AI 页的销毁由 camera 页在重新拍照或从 menu 进入时负责。 */
        camera_page_notify_return_from_ai();
        lv_scr_load_anim(camera_page_get_screen(), LV_SCR_LOAD_ANIM_MOVE_RIGHT, 300, 0, false);
    }
}

/* ── 页面创建 ────────────────────────────────────────────────────── */
lv_obj_t *create_camera_ai_page(const char *image_path)
{
    if (g_cai_active) return g_cai_screen;

    if (image_path) {
        strncpy(g_cai_image_path, image_path, sizeof(g_cai_image_path) - 1);
    }

    g_cai_screen = lv_obj_create(NULL);
    lv_obj_remove_style_all(g_cai_screen);
    lv_obj_set_style_bg_color(g_cai_screen, CAI_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(g_cai_screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(g_cai_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(g_cai_screen, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(g_cai_screen, cai_gesture_cb, LV_EVENT_GESTURE, NULL);

    /* 屏幕刚创建时get_width不准，直接使用固定分辨率(圆形屏466x466) */
    lv_coord_t screen_w = LV_HOR_RES;
    lv_coord_t screen_h = LV_VER_RES;
    syslog(LOG_INFO, "[cai_page] Create AI page, screen: %ldx%ld\n", (long)screen_w, (long)screen_h);

    /* 加载字体：标题单独加载28px，和camera页面一致；气泡加载24px */
#if LV_USE_FREETYPE
    if (!g_cai_title_font) {
        g_cai_title_font = cai_load_font(CAI_FONT_PATH, CAI_TITLE_FONT_SIZE);
    }
    if (!g_cai_font) {
        g_cai_font = cai_load_font(CAI_FONT_PATH, CAI_BUBBLE_FONT_SIZE);
    }
#endif

    /* 顶部标题："图文智录" 和camera页面完全一致 */
    lv_obj_t *title = lv_label_create(g_cai_screen);
    lv_label_set_text(title, i18n_get(STR_CAI_TITLE));
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    cai_apply_font(title, true);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, CAI_TITLE_MARGIN_TOP);

    /* 对话滚动区域 */
    lv_coord_t chat_top = CAI_TITLE_MARGIN_TOP + CAI_TITLE_FONT_SIZE + 20;
    lv_coord_t chat_bottom = CAI_MIC_MARGIN_BOTTOM + CAI_MIC_SIZE + 20;
    lv_coord_t chat_h = screen_h - chat_top - chat_bottom;
    g_cai_chat_scroll = lv_obj_create(g_cai_screen);
    lv_obj_remove_style_all(g_cai_chat_scroll);
    lv_obj_set_size(g_cai_chat_scroll, screen_w - CAI_SCREEN_PADDING_H * 2, chat_h);
    lv_obj_align(g_cai_chat_scroll, LV_ALIGN_TOP_MID, 0, chat_top);
    lv_obj_set_style_bg_opa(g_cai_chat_scroll, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_top(g_cai_chat_scroll, CAI_BUBBLE_SPACING/2, 0);
    lv_obj_set_style_pad_bottom(g_cai_chat_scroll, CAI_BUBBLE_SPACING/2, 0);
    lv_obj_set_flex_flow(g_cai_chat_scroll, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(g_cai_chat_scroll, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
    lv_obj_set_scroll_dir(g_cai_chat_scroll, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(g_cai_chat_scroll, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(g_cai_chat_scroll, LV_OBJ_FLAG_GESTURE_BUBBLE);
    /* 对话区域也注册右滑手势回调，这样在屏幕中间滑动也能退出 */
    lv_obj_add_event_cb(g_cai_chat_scroll, cai_gesture_cb, LV_EVENT_GESTURE, NULL);

    /* 底部麦克风图标 - 仅在用户说话时显示，初始隐藏 */
    g_cai_mic_icon = lv_img_create(g_cai_screen);
    lv_img_set_src(g_cai_mic_icon, &mic_white);
    lv_obj_set_size(g_cai_mic_icon, CAI_MIC_SIZE, CAI_MIC_SIZE);
    lv_obj_align(g_cai_mic_icon, LV_ALIGN_BOTTOM_MID, 0, -CAI_MIC_MARGIN_BOTTOM);
    lv_obj_clear_flag(g_cai_mic_icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(g_cai_mic_icon, LV_OBJ_FLAG_HIDDEN);

    /* 初始欢迎消息：正在分析图片... */
    g_cai_active = true;
    cai_add_message(CAI_MSG_AI, i18n_get(STR_CAI_ANALYZING));
    if (g_cai_msg_count > 0) {
        g_cai_msgs[g_cai_msg_count - 1].is_analyzing = true;
    }

    /* 注册回调 */
    voice_assistant_set_user_asr_callback(cai_on_user_asr);
    voice_assistant_set_ai_reply_callback(cai_on_ai_reply);
    voice_assistant_set_dialog_state_callback(cai_on_dialog_state);
    voice_assistant_set_image_status_callback(cai_on_image_status);

    /* mic 延迟到首次 AI 回复后再开启（见 cai_on_ai_reply），
     * 避免图片分析期间 mic 录音干扰拍照/分析流程。 */
    g_cai_mic_opened = false;

    syslog(LOG_INFO, "[cai_page] AI page created, mic will open on first AI reply\n");
    return g_cai_screen;
}

/* ── 页面销毁 ────────────────────────────────────────────────────── */
void destroy_camera_ai_page(void)
{
    if (!g_cai_active) return;

    g_cai_active = false;
    g_cai_mic_opened = false;

    /* 恢复 suspend 状态：camera_ai_page 活跃期间会 resume_listening 清除标志，
     * 销毁时须 suspend_listening 恢复为 camera_page/menu 期望的 VAD 关闭状态。
     * suspend_listening 内部会 exit_dialogue（关闭云端 mic）+ wakeup_detector_stop，
     * 且先置 suspend=true，使 dashscope 线程的 va_maybe_start_wakeup() 跳过 VAD 重启。 */
    voice_assistant_suspend_listening();
    voice_assistant_disable_feishu_forward();

    /* 注销回调 */
    voice_assistant_set_user_asr_callback(NULL);
    voice_assistant_set_ai_reply_callback(NULL);
    voice_assistant_set_dialog_state_callback(NULL);
    voice_assistant_set_image_status_callback(NULL);

    /* 释放所有消息 */
    for (int i = 0; i < g_cai_msg_count; i++) {
        free(g_cai_msgs[i].text);
    }
    free(g_cai_msgs);
    g_cai_msgs = NULL;
    g_cai_msg_count = 0;
    g_cai_msg_cap = 0;

    /* 销毁UI对象 */
    if (g_cai_screen) {
        lv_obj_delete(g_cai_screen);
        g_cai_screen = NULL;
    }

    g_cai_chat_scroll = NULL;
    g_cai_mic_icon = NULL;

/* Note: Keep font loaded across page creates/deletes to avoid reload overhead, will be cleaned up on app exit */
#if 0
#if LV_USE_FREETYPE
    if (g_cai_font) {
        lv_freetype_font_delete(g_cai_font);
        g_cai_font = NULL;
    }
#endif
#endif

    syslog(LOG_INFO, "[cai_page] AI page destroyed\n");
}

bool camera_ai_page_is_active(void)
{
    return g_cai_active;
}
