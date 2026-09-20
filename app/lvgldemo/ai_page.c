/****************************************************************************
 * apps/examples/lvgldemo/ai_page.c
 *
 * AI chat page — connects to ai_agent WebSocket server on localhost:28789.
 * Displays AI responses on screen while ai_agent handles TTS voice playback.
 *
 * Protocol (JSON over WebSocket):
 *   Client → Server: {"type":"message","content":"<text>","chat_id":"<id>"}
 *   Server → Client: {"type":"response","content":"<text>","channel":"voice",...}
 *   Client → Server: {"type":"voice_start"} or {"type":"voice_stop"}
 *
 ****************************************************************************/

#include <nuttx/config.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <poll.h>
#include <time.h>
#include <pthread.h>
#include <sys/time.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <syslog.h>
#include <malloc.h>
#include <lvgl/lvgl.h>

#include "ai_page.h"
#include "lvgldemo_common.h"
#include "lvgl_dispatch.h"
#include "i18n.h"

/* When LV_USE_CLIB_MALLOC is enabled, lv_mem_monitor() is a no-op and
 * free_size is always 0. Fall back to mallinfo() to read the real system
 * heap free size so low-memory guards do not falsely skip UI updates. */
size_t lvgldemo_get_free_heap(void)
{
#if LV_USE_STDLIB_MALLOC == LV_STDLIB_CLIB
    struct mallinfo mi = mallinfo();
    return (size_t)mi.fordblks;
#else
    lv_mem_monitor_t mon;
    lv_mem_monitor(&mon);
    return (size_t)mon.free_size;
#endif
}
#include "menu_page.h"
#include "voice_assistant.h"
#include "wakeup_detector.h"
#include "agent_config.h"

#ifdef CONFIG_MEDIA
#include "voice/voice_tts.h"
#include "voice/audio_playback.h"
#endif

/* ── LED indicator (GPIO34=red, GPIO35=green) ───────────────────
 * BES HAL GPIO 控制：通过 extern 声明调用 hal_gpio_* 函数，避免引入
 * hal_gpio.h 及其依赖的 BES framework 头文件链（同 wifi_page.c 中
 * bwifi_reset() 的处理方式，因 lvgldemo 的 include 路径未包含
 * framework/services/platform/hal）。
 *
 * 引脚映射（best1700 hal_iomux_best1700.h 枚举值）：
 *   HAL_IOMUX_PIN_P3_4 → 红色指示灯 (硬件丝印 GPIO34)
 *   HAL_IOMUX_PIN_P3_5 → 绿色指示灯 (硬件丝印 GPIO35)
 *   HAL_GPIO_DIR_OUT   == 1
 *
 * 行为规范（产品需求）：
 *   开机默认         红 off, 绿 off  （由 lvgldemo main 调用 ai_page_led_boot_init）
 *   进入对话界面     红 off, 绿 on
 *   右滑禁止(halo亮) 红 on,  绿 off  （与红圈光晕同步）
 *   右滑允许(halo灭) 红 off, 绿 on   （与红圈光晕同步）
 *   退出对话界面     红 off, 绿 off
 */
#include "hal_iomux_best1700.h"   /* HAL_IOMUX_PIN_P3_4 / P3_5 枚举 */

extern void hal_gpio_pin_set_dir(int pin, int dir, uint8_t val_for_out);
extern void hal_gpio_pin_set(int pin);
extern void hal_gpio_pin_clr(int pin);

#define AI_LED_PIN_RED    HAL_IOMUX_PIN_P3_4
#define AI_LED_PIN_GREEN  HAL_IOMUX_PIN_P3_5
#define AI_LED_DIR_OUT    1    /* HAL_GPIO_DIR_OUT */

static void ai_led_set(bool red_on, bool green_on)
{
    if (red_on)  hal_gpio_pin_set(AI_LED_PIN_RED);
    else         hal_gpio_pin_clr(AI_LED_PIN_RED);
    if (green_on) hal_gpio_pin_set(AI_LED_PIN_GREEN);
    else          hal_gpio_pin_clr(AI_LED_PIN_GREEN);
}

/* Boot default: configure both LED GPIOs as output and drive low.
 * Called once from lvgldemo main() before UI is created. */
void ai_page_led_boot_init(void)
{
    hal_gpio_pin_set_dir(AI_LED_PIN_RED,   AI_LED_DIR_OUT, 0);
    hal_gpio_pin_set_dir(AI_LED_PIN_GREEN, AI_LED_DIR_OUT, 0);
}

#define AI_FONT_PATH "/emmc/font/MiSans-Normal.ttf"
#define AI_FONT_SIZE 20

/* ── Constants ────────────────────────────────────────────────── */

#define AI_WS_HOST       "127.0.0.1"
#define AI_WS_PORT       28789
#define AI_WS_PATH       "/"
#define AI_CHAT_ID       "lvgldemo"
#define AI_MSG_MAX       1024
#define AI_DISPLAY_LINES 8
#define AI_RECONNECT_MS  3000
#define AI_RECV_BUF_SIZE 4096
/* TTS 队列背压上限：超过此深度时丢弃新句，防止 WS 接收快于 TTS 合成
 * 导致队列无限增长与内存碎片化。每节点约 1KB，8 节点 ≈ 8KB。 */
#define AI_TTS_Q_MAX     8

/* 对话气泡布局（与 camera_ai_page 对齐）：宽度随内容自适应，
 * 超过最大宽度则按固定宽度换行；内边距与圆角加大以提升可读性。 */
#define AI_BUBBLE_MAX_WIDTH 300
#define AI_BUBBLE_PADDING   16
#define AI_BUBBLE_RADIUS    16

/* WebSocket magic GUID for handshake */
#define WS_GUID "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

/* ── Private Data ─────────────────────────────────────────────── */

lv_obj_t * ai_wakeup_label = NULL;
static lv_obj_t * ai_title_label = NULL;
static lv_obj_t * ai_status_label = NULL;
static lv_obj_t * ai_chat_area = NULL;
static lv_obj_t * ai_msg_labels[AI_DISPLAY_LINES];
static char ai_msg_texts[AI_DISPLAY_LINES][AI_MSG_MAX];
static lv_obj_t * ai_msg_bubbles[AI_DISPLAY_LINES];
static int ai_msg_index = 0;

/* Page alive flag: prevent background WS thread callbacks from accessing
 * released UI objects after page destruction (e.g. during language switch). */
static volatile bool s_page_alive = false;

/* 异步清理进行中标志：ai_screen_unloaded_cb 触发异步清理时置 true，
 * ai_deinit_lvgl_cb 完成清理后置 false。在此期间 create_ai_page 会拒绝
 * 创建新页面，避免旧页面异步清理回调摧毁新页面对象导致 MemFault。 */
static volatile bool s_deinit_in_progress = false;

#ifdef CONFIG_EXAMPLES_AI_AGENT_VELA
/* 右滑禁止状态指示器：禁止右滑时在最外圈显示红色圆形光晕（5px宽） */
static lv_obj_t * s_exit_block_halo = NULL;
static lv_timer_t * s_exit_block_timer = NULL;
static bool s_exit_block_last_state = false;
#define AI_EXIT_BLOCK_POLL_MS 500
#endif

static lv_font_t * ai_cjk_font = NULL;

static int ai_ws_fd = -1;
static volatile bool ai_ws_connected = false;
static volatile bool ai_ws_running = false;
static volatile int  ai_ws_generation = 0;  /* Incremented on each ai_page_start();
                                            * stale threads detect mismatch and exit */
static pthread_t ai_ws_thread;
static pthread_mutex_t ai_ws_lock = PTHREAD_MUTEX_INITIALIZER;

/* Pre-read buffer: ws_do_handshake() may over-read beyond HTTP headers,
 * capturing the start of a server-pushed WS frame (e.g. connect.challenge).
 * ws_recv_text() drains this buffer first before calling recv(). */
static unsigned char ai_ws_preread[512];
static int ai_ws_preread_len = 0;

#ifdef CONFIG_MEDIA
/* TTS playback state.
 *
 * A dedicated player thread consumes a sentence queue and reuses a single
 * audio_playback session across sentences. This avoids the per-sentence
 * open/close/prepare gap that caused stuttering on long multi-sentence
 * replies. The WS receive thread only enqueues text (non-blocking) so it
 * can keep draining subsequent sentences while TTS synthesis + playback
 * run in parallel on the player thread. */
static audio_playback_t *ai_tts_pb = NULL;
static pthread_mutex_t ai_tts_lock = PTHREAD_MUTEX_INITIALIZER;

typedef struct ai_tts_sentence {
    char text[AI_MSG_MAX];
    struct ai_tts_sentence *next;
} ai_tts_sentence_t;

static ai_tts_sentence_t *ai_tts_q_head = NULL;
static ai_tts_sentence_t *ai_tts_q_tail = NULL;
static pthread_mutex_t ai_tts_q_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ai_tts_q_cond = PTHREAD_COND_INITIALIZER;
static volatile bool ai_tts_player_running = false;
static volatile bool ai_tts_player_started = false;
static pthread_t ai_tts_player_thread;

/* TTS streaming callback - receives PCM chunks from TTS service.
 * Runs on the player thread (inside voice_tts_speak_stream). */
static void ai_tts_stream_cb(const unsigned char *pcm_data,
                              size_t pcm_len,
                              int is_last,
                              void *user_data)
{
    (void)user_data;
    (void)is_last;

    audio_playback_t *pb;
    pthread_mutex_lock(&ai_tts_lock);
    pb = ai_tts_pb;
    pthread_mutex_unlock(&ai_tts_lock);

    if (!pb || !pcm_data || pcm_len == 0) {
        return;
    }

    int wret = audio_playback_write(pb, pcm_data, pcm_len);
    if (wret < 0) {
        syslog(LOG_ERR, "[ai_page] TTS playback write failed: %d\n", wret);
    }
}

/* Open (or reuse) the shared playback session. Called on the player
 * thread only. */
static audio_playback_t *ai_tts_ensure_playback(void)
{
    if (ai_tts_pb) {
        return ai_tts_pb;
    }

    ai_tts_pb = audio_playback_open(AGENT_AUDIO_PLAYBACK_DEV,
                                     AGENT_TTS_WS_SAMPLE_RATE,
                                     AGENT_VOICE_CHANNELS,
                                     AGENT_VOICE_BITS);
    if (!ai_tts_pb) {
        syslog(LOG_ERR, "[ai_page] Failed to open TTS audio playback\n");
    }
    return ai_tts_pb;
}

/* Player thread: pulls sentences from the queue and synthesizes them
 * sequentially, reusing one playback session. Synthesis of sentence N+1
 * overlaps with playback of sentence N thanks to the ring buffer. */
static void *ai_tts_player_main(void *arg)
{
    (void)arg;

    while (ai_tts_player_running) {
        pthread_mutex_lock(&ai_tts_q_lock);
        while (ai_tts_player_running && ai_tts_q_head == NULL) {
            pthread_cond_wait(&ai_tts_q_cond, &ai_tts_q_lock);
        }
        if (!ai_tts_player_running) {
            pthread_mutex_unlock(&ai_tts_q_lock);
            break;
        }
        ai_tts_sentence_t *node = ai_tts_q_head;
        ai_tts_q_head = node->next;
        if (ai_tts_q_head == NULL) {
            ai_tts_q_tail = NULL;
        }
        pthread_mutex_unlock(&ai_tts_q_lock);

        pthread_mutex_lock(&ai_tts_lock);
        audio_playback_t *pb = ai_tts_ensure_playback();
        pthread_mutex_unlock(&ai_tts_lock);

        if (pb) {
            int ret = voice_tts_speak_stream(node->text, ai_tts_stream_cb, NULL);
            if (ret != 0) {
                syslog(LOG_ERR, "[ai_page] voice_tts_speak_stream failed: %d\n", ret);
            }
        }

        free(node);
    }

    return NULL;
}

/* Enqueue a sentence for TTS. Non-blocking; safe to call from the WS
 * receive thread.
 *
 * 背压：队列深度达到 AI_TTS_Q_MAX 时丢弃新句，防止 WS 接收快于
 * TTS 合成导致队列无限增长。丢新句而非旧句，避免打断正在合成的
 * 句子序列；被丢弃的句子已在屏幕上显示，用户仍可读到此文本。 */
static void ai_play_tts(const char *text)
{
    if (!text || text[0] == '\0') {
        return;
    }

    ai_tts_sentence_t *node = malloc(sizeof(ai_tts_sentence_t));
    if (!node) {
        syslog(LOG_WARNING, "[ai_page] TTS sentence alloc failed\n");
        return;
    }
    strncpy(node->text, text, AI_MSG_MAX - 1);
    node->text[AI_MSG_MAX - 1] = '\0';
    node->next = NULL;

    pthread_mutex_lock(&ai_tts_q_lock);

    /* 统计当前队列深度 */
    int depth = 0;
    for (ai_tts_sentence_t *p = ai_tts_q_head; p; p = p->next) {
        depth++;
    }

    if (depth >= AI_TTS_Q_MAX) {
        pthread_mutex_unlock(&ai_tts_q_lock);
        free(node);
        syslog(LOG_WARNING, "[ai_page] TTS queue full (depth=%d), drop sentence\n",
               depth);
        return;
    }

    if (ai_tts_q_tail) {
        ai_tts_q_tail->next = node;
    } else {
        ai_tts_q_head = node;
    }
    ai_tts_q_tail = node;
    pthread_cond_signal(&ai_tts_q_cond);
    pthread_mutex_unlock(&ai_tts_q_lock);
}

/* Stop the player thread, flush the queue, and release playback. */
static void ai_stop_tts(void)
{
    if (ai_tts_player_started) {
        ai_tts_player_running = false;
        pthread_mutex_lock(&ai_tts_q_lock);
        pthread_cond_broadcast(&ai_tts_q_cond);
        pthread_mutex_unlock(&ai_tts_q_lock);
        pthread_join(ai_tts_player_thread, NULL);
        ai_tts_player_started = false;
    }

    pthread_mutex_lock(&ai_tts_q_lock);
    ai_tts_sentence_t *node = ai_tts_q_head;
    ai_tts_q_head = NULL;
    ai_tts_q_tail = NULL;
    pthread_mutex_unlock(&ai_tts_q_lock);
    while (node) {
        ai_tts_sentence_t *next = node->next;
        free(node);
        node = next;
    }

    pthread_mutex_lock(&ai_tts_lock);
    if (ai_tts_pb) {
        audio_playback_stop(ai_tts_pb);
        audio_playback_close(ai_tts_pb);
        ai_tts_pb = NULL;
    }
    pthread_mutex_unlock(&ai_tts_lock);
}

/* Start the player thread. Called from ai_page_start(). */
static void ai_tts_player_start(void)
{
    if (ai_tts_player_running) {
        return;
    }

    ai_tts_player_running = true;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 16 * 1024);
    if (pthread_create(&ai_tts_player_thread, &attr, ai_tts_player_main, NULL) == 0) {
        ai_tts_player_started = true;
    } else {
        ai_tts_player_running = false;
        syslog(LOG_ERR, "[ai_page] Failed to create TTS player thread\n");
    }
    pthread_attr_destroy(&attr);
}
#endif /* CONFIG_MEDIA */

/* ── LVGL helpers (must run in LVGL thread via lv_async_call) ── */

typedef struct {
    char text[AI_MSG_MAX];
    bool is_user;
    char time_prefix[8];  /* "HH:MM" 本地时间前缀 */
} ai_async_msg_t;

static bool ai_is_valid_utf8(const char *str)
{
    if (!str) return false;
    const unsigned char *p = (const unsigned char *)str;
    while (*p) {
        unsigned char c = *p;
        int expected_bytes;
        if (c < 0x80) {
            p++;
            continue;
        } else if ((c & 0xE0) == 0xC0) {
            expected_bytes = 1;
        } else if ((c & 0xF0) == 0xE0) {
            expected_bytes = 2;
        } else if ((c & 0xF8) == 0xF0) {
            expected_bytes = 3;
        } else {
            return false;
        }
        p++;
        for (int i = 0; i < expected_bytes; i++) {
            if ((*p & 0xC0) != 0x80) return false;
            p++;
        }
    }
    return true;
}

static void ai_sanitize_utf8(char *str)
{
    if (!str) return;
    unsigned char *dst = (unsigned char *)str;
    const unsigned char *src = dst;
    while (*src) {
        unsigned char c = *src;
        int expected_bytes;
        if (c < 0x80) {
            *dst++ = *src++;
            continue;
        } else if ((c & 0xE0) == 0xC0) {
            expected_bytes = 1;
        } else if ((c & 0xF0) == 0xE0) {
            expected_bytes = 2;
        } else if ((c & 0xF8) == 0xF0) {
            expected_bytes = 3;
        } else {
            src++;
            continue;
        }
        int skip = 1;
        for (int i = 0; i < expected_bytes; i++) {
            if ((src[1 + i] & 0xC0) != 0x80) {
                skip = 0;
                break;
            }
        }
        if (skip) {
            *dst++ = *src++;
            for (int i = 0; i < expected_bytes; i++)
                *dst++ = *src++;
        } else {
            src++;
        }
    }
    *dst = '\0';
}

static void ai_add_message_async_cb(void * data)
{
    ai_async_msg_t * msg = (ai_async_msg_t *)data;
    if (!msg) return;

    ai_sanitize_utf8(msg->text);

    /* 删除空行：保留正常换行，将空白行（仅含空格/制表符/换行）折叠为单个换行，
     * 并去除首尾换行/空格。避免飞书多消息合并 / Omni transcript
     * 残留换行在 UI 上出现空行。
     * 示例: "你好\n\t\n\r\n\t\n\t\t世界" → "你好\n世界" */
    size_t r = 0, w = 0;
    bool last_was_nl = false;
    /* 跳过首部空白 */
    while (msg->text[r] == ' ' || msg->text[r] == '\t' ||
           msg->text[r] == '\n' || msg->text[r] == '\r') {
        r++;
    }
    while (msg->text[r]) {
        char c = msg->text[r];
        if (c == '\n' || c == '\r') {
            /* 统一为 \n，连续换行只保留一个（删除空行） */
            if (w > 0 && !last_was_nl) {
                msg->text[w++] = '\n';
                last_was_nl = true;
            }
        } else if (c == ' ' || c == '\t') {
            /* 跨行空白：若换行后只有空格/制表符再换行，按空行处理 */
            size_t peek = r;
            while (msg->text[peek] == ' ' || msg->text[peek] == '\t') peek++;
            if (msg->text[peek] == '\n' || msg->text[peek] == '\r') {
                /* 跳过空白 + 换行，按单个换行处理 */
                r = peek;
                if (w > 0 && !last_was_nl) {
                    msg->text[w++] = '\n';
                    last_was_nl = true;
                }
            } else {
                msg->text[w++] = c;
                last_was_nl = false;
            }
        } else {
            msg->text[w++] = c;
            last_was_nl = false;
        }
        r++;
    }
    /* 去除尾部换行/空格 */
    while (w > 0 && (msg->text[w - 1] == '\n' || msg->text[w - 1] == ' ' ||
                     msg->text[w - 1] == '\t')) {
        w--;
    }
    msg->text[w] = '\0';
    /* 全为空白则跳过显示 */
    if (w == 0) {
        free(msg);
        return;
    }

    size_t free_heap = lvgldemo_get_free_heap();
    size_t text_len = strlen(msg->text);
    size_t estimated_need = text_len + 4096;
    syslog(LOG_INFO, "[ai_page] add msg: heap=%u need=%u user=%d text=%.40s\n",
           (unsigned)free_heap, (unsigned)estimated_need, msg->is_user, msg->text);
    if (free_heap < estimated_need) {
        syslog(LOG_WARNING, "[ai_page] heap low (%u bytes free, need ~%u), skip message update\n",
               (unsigned)free_heap, (unsigned)estimated_need);
        free(msg);
        return;
    }

    /* 滚动：满员时复用最旧 slot，不再 lv_obj_del / lv_obj_create */
    if (ai_msg_index >= AI_DISPLAY_LINES) {
        /* 保存最旧的 row + label 指针，移到末尾 slot 复用，
         * 避免删除/创建 LVGL 对象造成内存碎片。
         * 原实现仅前移数组指针但未同步 LVGL 子对象位置，
         * 导致旧 row 残留在 ai_chat_area 中越积越多。 */
        lv_obj_t *oldest_row   = ai_msg_bubbles[0];
        lv_obj_t *oldest_label = ai_msg_labels[0];
        for (int i = 0; i < AI_DISPLAY_LINES - 1; i++) {
            strncpy(ai_msg_texts[i], ai_msg_texts[i + 1], AI_MSG_MAX - 1);
            ai_msg_texts[i][AI_MSG_MAX - 1] = '\0';
            ai_msg_labels[i] = ai_msg_labels[i + 1];
            ai_msg_bubbles[i] = ai_msg_bubbles[i + 1];
        }
        /* 复用最旧的 row：放到末尾 slot，并在父容器中移到最后位置，
         * 保持 UI 显示顺序（旧→新）与数组索引一致。 */
        ai_msg_bubbles[AI_DISPLAY_LINES - 1] = oldest_row;
        ai_msg_labels[AI_DISPLAY_LINES - 1] = oldest_label;
        if (oldest_row && ai_chat_area) {
            uint32_t child_cnt = lv_obj_get_child_count(ai_chat_area);
            if (child_cnt > 0) {
                lv_obj_move_to_index(oldest_row, (int32_t)child_cnt - 1);
            }
        }
        ai_msg_index = AI_DISPLAY_LINES - 1;
    }

    int idx = ai_msg_index;
    /* 在消息文本前添加本地时间前缀 "HH:MM"，时间戳后换行，内容从下一行展示 */
    if (msg->time_prefix[0]) {
        snprintf(ai_msg_texts[idx], AI_MSG_MAX, "%s\n%s",
                 msg->time_prefix, msg->text);
    } else {
        strncpy(ai_msg_texts[idx], msg->text, AI_MSG_MAX - 1);
        ai_msg_texts[idx][AI_MSG_MAX - 1] = '\0';
    }

    lv_color_t bg_color = msg->is_user ? lv_color_hex(0x98ee6b) : lv_color_hex(0x2e2e2c);
    lv_color_t txt_color = msg->is_user ? lv_color_hex(0x000000) : lv_color_hex(0xFFFFFF);

    /* 复用已有 row + bubble + label，避免频繁创建/销毁导致内存碎片。
     * ai_msg_bubbles[idx] 现存储 row(对齐容器)，内层 bubble 为其首个子节点。 */
    if (ai_msg_bubbles[idx] && ai_msg_labels[idx]) {
        lv_obj_t *row = ai_msg_bubbles[idx];
        lv_obj_t *bubble = lv_obj_get_child(row, 0);
        /* 已有 widget：仅更新文本、颜色与左右对齐 */
        lv_obj_set_style_bg_color(bubble, bg_color, 0);
        lv_label_set_text(ai_msg_labels[idx], ai_msg_texts[idx]);
        lv_obj_set_style_text_color(ai_msg_labels[idx], txt_color, 0);
        if (msg->is_user) {
            lv_obj_set_flex_align(row, LV_FLEX_ALIGN_END,
                                  LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END);
        } else {
            lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START,
                                  LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
        }
    } else {
        /* 首次创建：row(对齐容器) + bubble + label。
         * row 全宽、内容高，负责用户气泡右对齐 / AI 气泡左对齐；
         * bubble 宽高随内容自适应，label 通过 max_width 限制换行阈值，
         * 超过最大宽度按固定宽度换行，小于则按文字宽度收紧。 */
        lv_obj_t *row = lv_obj_create(ai_chat_area);
        lv_obj_remove_style_all(row);
        lv_obj_set_width(row, LV_PCT(100));
        lv_obj_set_height(row, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        if (msg->is_user) {
            lv_obj_set_flex_align(row, LV_FLEX_ALIGN_END,
                                  LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END);
        } else {
            lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START,
                                  LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
        }
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *bubble = lv_obj_create(row);
        lv_obj_remove_style_all(bubble);
        lv_obj_set_style_bg_color(bubble, bg_color, 0);
        lv_obj_set_style_bg_opa(bubble, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(bubble, AI_BUBBLE_RADIUS, 0);
        lv_obj_set_style_pad_all(bubble, AI_BUBBLE_PADDING, 0);
        lv_obj_set_width(bubble, LV_SIZE_CONTENT);
        lv_obj_set_height(bubble, LV_SIZE_CONTENT);
        lv_obj_clear_flag(bubble, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t * label = lv_label_create(bubble);
        lv_label_set_text(label, ai_msg_texts[idx]);
        lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(label, LV_SIZE_CONTENT);
        lv_obj_set_style_max_width(label,
            AI_BUBBLE_MAX_WIDTH - AI_BUBBLE_PADDING * 2, 0);
        lv_obj_set_style_text_color(label, txt_color, 0);
#if LV_USE_FREETYPE
        if (ai_cjk_font)
            lv_obj_set_style_text_font(label, ai_cjk_font, 0);
        else
#endif
#if LV_FONT_SIMSUN_16_CJK
        lv_obj_set_style_text_font(label, &lv_font_simsun_16_cjk, 0);
#endif
        ai_msg_labels[idx] = label;
        ai_msg_bubbles[idx] = row;
    }

    ai_msg_index++;

    if (ai_chat_area) {
        lv_obj_update_layout(ai_chat_area);
        lv_obj_t *latest = ai_msg_bubbles[idx];
        if (latest) {
            int32_t bubble_h = lv_obj_get_height(latest);
            int32_t view_h   = lv_obj_get_content_height(ai_chat_area);
            if (bubble_h > view_h) {
                /* 气泡高度超过可视区域，滚动到气泡顶部，让用户从头阅读 */
                int32_t pad_top = lv_obj_get_style_pad_top(ai_chat_area, 0);
                int32_t target_y = lv_obj_get_y(latest) - pad_top;
                if (target_y < 0) target_y = 0;
                lv_obj_scroll_to_y(ai_chat_area, target_y, LV_ANIM_OFF);
            } else {
                /* 正常情况：滚动到底部显示最新消息 */
                lv_obj_scroll_to_y(ai_chat_area, LV_COORD_MAX, LV_ANIM_OFF);
            }
        } else {
            lv_obj_scroll_to_y(ai_chat_area, LV_COORD_MAX, LV_ANIM_OFF);
        }
    }

    free(msg);
}

static void ai_show_message(const char * text, bool is_user)
{
    ai_async_msg_t * msg = malloc(sizeof(ai_async_msg_t));
    if (!msg) return;
    strncpy(msg->text, text, AI_MSG_MAX - 1);
    msg->text[AI_MSG_MAX - 1] = '\0';
    msg->is_user = is_user;

    /* 获取本地时间作为气泡前缀 */
    struct tm tm_now = agent_localtime();
    snprintf(msg->time_prefix, sizeof(msg->time_prefix), "%02d:%02d",
             tm_now.tm_hour, tm_now.tm_min);

    if (!lvgl_dispatch_async(ai_add_message_async_cb, msg)) {
        /* 队列满，释放 msg 避免泄漏 */
        free(msg);
    }
}

static void ai_set_status_async_cb(void * data)
{
    char * text = (char *)data;
    if (!text) return;

    /* Discard if the page has been destroyed (e.g. language switch rebuild) */
    if (!s_page_alive) {
        free(text);
        return;
    }

    ai_sanitize_utf8(text);

    size_t free_heap = lvgldemo_get_free_heap();
    size_t text_len = strlen(text);
    size_t estimated_need = text_len + 4096;
    if (free_heap < estimated_need) {
        syslog(LOG_WARNING, "[ai_page] heap low (%u bytes free, need ~%u), skip status update\n",
               (unsigned)free_heap, (unsigned)estimated_need);
        free(text);
        return;
    }

    if (ai_status_label)
        lv_label_set_text(ai_status_label, text);
    free(text);
}

static void ai_set_status(const char * text)
{
    char * buf = strdup(text);
    if (buf) {
        if (!lvgl_dispatch_async(ai_set_status_async_cb, buf)) {
            syslog(LOG_WARNING, "[ai_page] dispatch_async failed (queue full) for '%s'\n", text);
            free(buf);
        }
    } else {
        syslog(LOG_WARNING, "[ai_page] set_status strdup failed for '%s'\n", text);
    }
}

/* ── WebSocket client (minimal implementation) ─────────────────── */

/* Base64 encode for WS handshake (minimal, no padding handling needed) */
static const char b64_table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int base64_encode(const unsigned char *src, size_t len,
                         char *dst, size_t dst_size)
{
    size_t i, j;
    for (i = 0, j = 0; i < len && j + 4 < dst_size; i += 3) {
        uint32_t a = src[i];
        uint32_t b = (i + 1 < len) ? src[i + 1] : 0;
        uint32_t c = (i + 2 < len) ? src[i + 2] : 0;
        uint32_t triple = (a << 16) | (b << 8) | c;

        dst[j++] = b64_table[(triple >> 18) & 0x3F];
        dst[j++] = b64_table[(triple >> 12) & 0x3F];
        dst[j++] = (i + 1 < len) ? b64_table[(triple >> 6) & 0x3F] : '=';
        dst[j++] = (i + 2 < len) ? b64_table[triple & 0x3F] : '=';
    }
    dst[j] = '\0';
    return (int)j;
}

/* SHA-1 for WS handshake — use a simple implementation
 * since NuttX may not have mbedtls available in lvgldemo context.
 * We use a hardcoded key for simplicity (the server accepts any key). */
static int ws_do_handshake(int fd)
{
    /* Clear pre-read buffer */
    ai_ws_preread_len = 0;

    /* Generate a simple Sec-WebSocket-Key (16 bytes base64 encoded) */
    unsigned char key_bytes[16];
    for (int i = 0; i < 16; i++)
        key_bytes[i] = (unsigned char)(rand() & 0xFF);
    char ws_key[32];
    base64_encode(key_bytes, 16, ws_key, sizeof(ws_key));

    /* Send HTTP upgrade request */
    char req[512];
    int rlen = snprintf(req, sizeof(req),
        "GET %s HTTP/1.1\r\n"
        "Host: %s:%d\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: %s\r\n"
        "Sec-WebSocket-Version: 13\r\n\r\n",
        AI_WS_PATH, AI_WS_HOST, AI_WS_PORT, ws_key);

    if (send(fd, req, rlen, 0) != rlen) {
        syslog(LOG_WARNING, "[ai_page] WS send upgrade req failed (rlen=%d)\n", rlen);
        return -1;
    }

    char resp[512];
    int total = 0;
    while (total < (int)sizeof(resp) - 1) {
        struct pollfd pfd_h = { .fd = fd, .events = POLLIN };
        int pr = poll(&pfd_h, 1, 10000);
        if (pr <= 0) {
            syslog(LOG_WARNING, "[ai_page] WS handshake poll timeout/fail (pr=%d,errno=%d,total=%d)\n",
                pr, errno, total);
            return -1;
        }
        int n = recv(fd, resp + total, sizeof(resp) - 1 - total, 0);
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            continue;
        if (n <= 0) {
            syslog(LOG_WARNING, "[ai_page] WS recv response failed (n=%d, errno=%d, total=%d)\n",
                        n, errno, total);
            return -1;
        }
        total += n;
        resp[total] = '\0';
        if (strstr(resp, "\r\n\r\n")) break;
    }

    /* Check for 101 response */
    if (total < 4 || strncmp(resp, "HTTP/1.1 101", 12) != 0) {
        syslog(LOG_WARNING, "[ai_page] WS bad response (%d bytes): %.80s\n", total, resp);
        return -1;
    }

    /* Save any data beyond the HTTP headers into pre-read buffer */
    char *hdr_end = strstr(resp, "\r\n\r\n");
    if (hdr_end) {
        int hdr_len = (int)(hdr_end - resp) + 4; /* include \r\n\r\n */
        int extra = total - hdr_len;
        if (extra > 0) {
            if (extra <= (int)sizeof(ai_ws_preread)) {
                memcpy(ai_ws_preread, resp + hdr_len, extra);
                ai_ws_preread_len = extra;
            } else {
                syslog(LOG_WARNING, "[ai_page] WS preread overflow: %d bytes > %zu, truncating\n",
                    extra, sizeof(ai_ws_preread));
                memcpy(ai_ws_preread, resp + hdr_len, sizeof(ai_ws_preread));
                ai_ws_preread_len = (int)sizeof(ai_ws_preread);
            }
        }
    }

    return 0;
}

static int ws_send_ping(int fd)
{
    unsigned char ping_frame[6] = { 0x89, 0x80, 0x00, 0x00, 0x00, 0x00 };
    return send(fd, ping_frame, 6, 0) == 6 ? 0 : -1;
}

/* Send a WebSocket text frame (client → server, masked) */
static int ws_send_text(int fd, const char * payload, size_t len)
{
    unsigned char mask[4];
    for (int i = 0; i < 4; i++)
        mask[i] = (unsigned char)(rand() & 0xFF);

    unsigned char hdr[14];
    int hdr_len = 0;

    hdr[0] = 0x81; /* FIN + text opcode */
    if (len < 126) {
        hdr[1] = (unsigned char)(0x80 | len); /* MASK + length */
        memcpy(hdr + 2, mask, 4);
        hdr_len = 6;
    } else if (len < 65536) {
        hdr[1] = 0x80 | 126;
        hdr[2] = (unsigned char)(len >> 8);
        hdr[3] = (unsigned char)(len & 0xFF);
        memcpy(hdr + 4, mask, 4);
        hdr_len = 8;
    } else {
        return -1; /* Too large */
    }

    if (send(fd, hdr, hdr_len, 0) != hdr_len)
        return -1;

    /* Mask and send payload */
    unsigned char *masked = malloc(len);
    if (!masked) return -1;
    for (size_t i = 0; i < len; i++)
        masked[i] = ((const unsigned char *)payload)[i] ^ mask[i % 4];
    int rc = (send(fd, masked, len, 0) == (ssize_t)len) ? 0 : -1;
    free(masked);
    return rc;
}

/* Receive a WebSocket text frame (server → client, unmasked)
 * Drains ai_ws_preread buffer first (leftover from HTTP handshake).
 * Uses poll() for reliable blocking wait (avoids MSG_WAITALL issues on NuttX). */
static int ws_recv_text(int fd, char * buf, size_t buf_size)
{
    unsigned char b0, b1;

    #define READ_BYTE(dst) do { \
        if (ai_ws_preread_len > 0) { \
            (dst) = ai_ws_preread[0]; \
            memmove(ai_ws_preread, ai_ws_preread + 1, --ai_ws_preread_len); \
        } else { \
            int _got = 0; \
            for (int _attempt = 0; _attempt < 120 && !_got; _attempt++) { \
                struct pollfd _pfd = { .fd = fd, .events = POLLIN }; \
                int _pr = poll(&_pfd, 1, 30000); \
                if (_pr == 0) { \
                    ws_send_ping(fd); \
                    continue; \
                } \
                if (_pr < 0) { \
                    syslog(LOG_WARNING, "[ai_page] READ_BYTE poll error (errno=%d)\n", errno); \
                    break; \
                } \
                int _n = recv(fd, &(dst), 1, 0); \
                if (_n == 1) { _got = 1; break; } \
                if (_n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) { \
                    continue; \
                } \
                syslog(LOG_WARNING, "[ai_page] READ_BYTE recv failed (n=%d,errno=%d)\n", _n, errno); \
                break; \
            } \
            if (!_got) return -1; \
        } \
    } while(0)

    READ_BYTE(b0);
    READ_BYTE(b1);

    int opcode = b0 & 0x0F;
    size_t plen = b1 & 0x7F;

    if (opcode == 0x8) {
        return 0;
    }

    /* Handle ping/pong */
    if (opcode == 0x9) {
        if (plen == 126) {
            unsigned char ext[2];
            READ_BYTE(ext[0]);
            READ_BYTE(ext[1]);
            plen = ((size_t)ext[0] << 8) | ext[1];
        }
        unsigned char pp[128];
        if (plen > 0 && plen <= 125) {
            for (size_t i = 0; i < plen; i++) READ_BYTE(pp[i]);
        }
        /* Send pong */
        unsigned char pong[2] = { 0x8A, (unsigned char)plen };
        send(fd, pong, 2, 0);
        if (plen > 0 && plen <= 125)
            send(fd, pp, plen, 0);
        return -2; /* Continue reading */
    }

    if (opcode != 0x1) return -2; /* Skip non-text frames */

    if (plen == 126) {
        unsigned char ext[2];
        READ_BYTE(ext[0]);
        READ_BYTE(ext[1]);
        plen = ((size_t)ext[0] << 8) | ext[1];
    } else if (plen == 127) {
        return -1;
    }

    if (plen >= buf_size) return -1;

    /* Read payload: drain preread first, then socket with poll */
    int received = 0;
    while ((size_t)received < plen) {
        if (ai_ws_preread_len > 0) {
            int want = (int)plen - received;
            int avail = ai_ws_preread_len < want ? ai_ws_preread_len : want;
            memcpy(buf + received, ai_ws_preread, avail);
            received += avail;
            ai_ws_preread_len -= avail;
            if (ai_ws_preread_len > 0)
                memmove(ai_ws_preread, ai_ws_preread + avail, ai_ws_preread_len);
        } else {
            struct pollfd pfd_r = { .fd = fd, .events = POLLIN };
            int pr = poll(&pfd_r, 1, 30000);
            if (pr == 0) {
                ws_send_ping(fd);
                continue;
            }
            if (pr < 0) {
                syslog(LOG_WARNING, "[ai_page] payload poll error (errno=%d)\n", errno);
                return -1;
            }
            int n = recv(fd, buf + received, plen - received, 0);
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                continue;
            }
            if (n <= 0) {
                syslog(LOG_WARNING, "[ai_page] payload recv failed (n=%d,errno=%d)\n", n, errno);
                return -1;
            }
            received += n;
        }
    }
    buf[plen] = '\0';
    return (int)plen;

    #undef READ_BYTE
}

/* Send a chat message to ai_agent via WebSocket */
static int ai_ws_send_message(const char * text)
{
    pthread_mutex_lock(&ai_ws_lock);
    if (ai_ws_fd < 0 || !ai_ws_connected) {
        pthread_mutex_unlock(&ai_ws_lock);
        return -1;
    }

    /* Build JSON: {"type":"message","content":"<text>","chat_id":"lvgldemo"} */
    char json[AI_MSG_MAX + 128];
    snprintf(json, sizeof(json),
        "{\"type\":\"message\",\"content\":\"%s\",\"chat_id\":\"%s\"}",
        text, AI_CHAT_ID);

    int rc = ws_send_text(ai_ws_fd, json, strlen(json));
    pthread_mutex_unlock(&ai_ws_lock);
    return rc;
}

/* Send a voice control command via WebSocket */
static int __attribute__((unused)) ai_ws_send_command(const char * cmd_type)
{
    pthread_mutex_lock(&ai_ws_lock);
    if (ai_ws_fd < 0 || !ai_ws_connected) {
        syslog(LOG_WARNING, "[ai_page] WS send '%s' skipped: fd=%d connected=%d\n",
            cmd_type, ai_ws_fd, ai_ws_connected);
        pthread_mutex_unlock(&ai_ws_lock);
        return -1;
    }

    char json[256];
    snprintf(json, sizeof(json),
        "{\"type\":\"%s\",\"chat_id\":\"%s\"}", cmd_type, AI_CHAT_ID);

    syslog(LOG_INFO, "[ai_page] WS sending '%s' (%zu bytes)\n",
        cmd_type, strlen(json));

    int rc = ws_send_text(ai_ws_fd, json, strlen(json));

    pthread_mutex_unlock(&ai_ws_lock);
    return rc;
}

/* ── WebSocket client thread ───────────────────────────────────── */

static void ai_ws_interruptible_sleep(int seconds, int my_generation)
{
    for (int i = 0; i < seconds * 10 &&
         ai_ws_running && my_generation == ai_ws_generation; i++) {
        usleep(100000);
    }
}

static void * ai_ws_client_thread(void * arg)
{
    (void)arg;
    char recv_buf[AI_RECV_BUF_SIZE];
    int my_generation = ai_ws_generation;

    while (ai_ws_running && my_generation == ai_ws_generation) {
        if (my_generation == ai_ws_generation)
            ai_set_status(i18n_get(STR_AI_CONNECTING));

        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) {
            if (my_generation == ai_ws_generation)
                ai_set_status(i18n_get(STR_AI_SOCKET_ERROR));
            ai_ws_interruptible_sleep(3, my_generation);
            continue;
        }

        struct sockaddr_in addr = {
            .sin_family = AF_INET,
            .sin_port = htons(AI_WS_PORT),
            .sin_addr.s_addr = inet_addr(AI_WS_HOST),
        };

        /* Use blocking connect — localhost is instantaneous and avoids
         * fcntl(O_NONBLOCK) toggle which may silently fail on NuttX,
         * leaving the socket in non-blocking mode and causing recv()
         * to return EAGAIN immediately. */
        if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
            syslog(LOG_WARNING, "[ai_page] WS connect failed (errno=%d)\n", errno);
            close(fd);
            if (my_generation == ai_ws_generation)
                ai_set_status(i18n_get(STR_AI_CONNECT_FAIL));
            ai_ws_interruptible_sleep(AI_RECONNECT_MS / 1000, my_generation);
            continue;
        }

        syslog(LOG_INFO, "[ai_page] WS TCP connected to %s:%d\n", AI_WS_HOST, AI_WS_PORT);

        /* WebSocket handshake — no SO_RCVTIMEO; we use poll() for all
         * timeout control. SO_RCVTIMEO can interfere with poll() on some
         * NuttX socket backends and cause spurious EAGAIN. */

        /* WebSocket handshake */
        if (ws_do_handshake(fd) != 0) {
            syslog(LOG_WARNING, "[ai_page] WS handshake failed\n");
            close(fd);
            if (my_generation == ai_ws_generation)
                ai_set_status(i18n_get(STR_AI_HANDSHAKE_FAIL));
            ai_ws_interruptible_sleep(AI_RECONNECT_MS / 1000, my_generation);
            continue;
        }

        pthread_mutex_lock(&ai_ws_lock);
        ai_ws_fd = fd;
        ai_ws_connected = true;
        pthread_mutex_unlock(&ai_ws_lock);

        if (my_generation == ai_ws_generation)
            ai_set_status(i18n_get(STR_AI_CONNECTED));

        while (ai_ws_running && ai_ws_connected &&
               my_generation == ai_ws_generation) {
            int n = ws_recv_text(fd, recv_buf, sizeof(recv_buf));
            if (n == 0) {
                break;
            }
            if (n < 0) {
                if (n == -2) continue;
                syslog(LOG_WARNING, "[ai_page] WS recv error (n=%d,errno=%d)\n", n, errno);
                break;
            }

            bool is_user_msg = (strstr(recv_buf, "\"type\":\"asr\"") != NULL);

            char *content_start = strstr(recv_buf, "\"content\":\"");
            if (content_start) {
                content_start += 11; /* Skip "content":" */
                char *content_end = strchr(content_start, '"');
                if (content_end) {
                    size_t clen = (size_t)(content_end - content_start);
                    if (clen >= AI_MSG_MAX) clen = AI_MSG_MAX - 1;
                    char display_text[AI_MSG_MAX];
                    memcpy(display_text, content_start, clen);
                    display_text[clen] = '\0';

                    /* Unescape \\n to real newlines for display */
                    char *p = display_text;
                    while ((p = strstr(p, "\\n")) != NULL) {
                        *p = '\n';
                        memmove(p + 1, p + 2, strlen(p + 2) + 1);
                        p++;
                    }

                    /* Unescape \\\" to " */
                    p = display_text;
                    while ((p = strstr(p, "\\\"")) != NULL) {
                        *p = '"';
                        memmove(p + 1, p + 2, strlen(p + 2) + 1);
                        p++;
                    }

                    ai_show_message(display_text, is_user_msg);

                    /* Play TTS for AI response (not user ASR transcript.
                     * Skip TTS for "response_silent" type — those are
                     * voice-channel replies already spoken by
                     * voice_channel_speak() in agent_main.c.  Playing
                     * them here too would cause duplicate/overlapping
                     * audio. */
                    bool is_silent = (strstr(recv_buf,
                        "\"type\":\"response_silent\"") != NULL);
                    if (!is_user_msg && !is_silent) {
#ifdef CONFIG_MEDIA
                        ai_play_tts(display_text);
#endif
                    }

                    if (my_generation == ai_ws_generation)
                        ai_set_status(i18n_get(STR_AI_CONNECTED));
                }
            }
        }

        /* Disconnected — only clear globals if we still own them.
         * A stale thread (generation mismatch) must not clobber the
         * new thread's fd / connected flag. */
        pthread_mutex_lock(&ai_ws_lock);
        if (ai_ws_fd == fd) {
            ai_ws_connected = false;
            ai_ws_fd = -1;
        }
        pthread_mutex_unlock(&ai_ws_lock);
        close(fd);

        if (my_generation == ai_ws_generation)
            ai_set_status(i18n_get(STR_AI_DISCONNECTED));

        if (ai_ws_running && my_generation == ai_ws_generation)
            ai_ws_interruptible_sleep(AI_RECONNECT_MS / 1000, my_generation);
    }

    syslog(LOG_INFO, "[ai_page] WS client thread exiting (gen=%d)\n", my_generation);
    return NULL;
}

/* Wakeup status update (called from lvgldemo) */
void ai_page_update_wakeup_status(const char *status)
{
    if (!status) return;
    char *buf = strdup(status);
    if (!buf) {
        syslog(LOG_WARNING, "[ai_page] update_wakeup_status strdup failed\n");
        return;
    }
    if (!lvgl_dispatch_async(ai_set_status_async_cb, buf)) {
        free(buf);
    }
}

/* Public wrapper to send a user message from voice_assistant */
int ai_page_send_message(const char *text)
{
    return ai_ws_send_message(text);
}

/* Display Omni multimodal response text directly on screen */
void ai_page_show_omni_response(const char *text)
{
    if (!text || text[0] == '\0') {
        return;
    }
    ai_show_message(text, false);
}

/* Display user speech transcript on screen (Omni mode) */
void ai_page_show_user_message(const char *text)
{
    if (!text || text[0] == '\0') {
        return;
    }
    ai_show_message(text, true);
}

/* Display feishu conversation message with sender-based styling:
 * - 小Q助手 (bot): green bg, black text (is_user=true style)
 * - Others: gray bg, white text (is_user=false style) */
void ai_page_show_feishu_msg(const char *text, const char *sender_name)
{
    if (!text || text[0] == '\0') {
        return;
    }

    /* 判断是否为小Q助手(bot)的消息：包含"小Q"、"xiaoq"、"助手"等关键词 */
    bool is_bot = false;
    if (sender_name && sender_name[0]) {
        if (strstr(sender_name, "小Q") || strstr(sender_name, "xiaoq") ||
            strstr(sender_name, "XiaoQ") || strstr(sender_name, "助手")) {
            is_bot = true;
        }
    }

    /* bot消息使用绿底黑字(is_user=true)，其他人消息使用灰底白字(is_user=false) */
    ai_show_message(text, is_bot);
}

/* ── Back button ───────────────────────────────────────────────── */

static void ai_back_btn_cb(lv_event_t * e)
{
    (void)e;
    if (main_screen)
        lv_scr_load_anim(main_screen, LV_SCR_LOAD_ANIM_MOVE_LEFT, 300, 0, false);
}

/* ── Gesture handler ───────────────────────────────────────────── */

#ifdef CONFIG_EXAMPLES_AI_AGENT_VELA
/* 轮询右滑禁止状态，动态显示/隐藏红色圆形光晕，并同步切换红绿指示灯 */
static void ai_exit_block_timer_cb(lv_timer_t * t)
{
    (void)t;
    if (!s_page_alive || !s_exit_block_halo) return;
    bool blocked = voice_assistant_is_exit_blocked();
    if (blocked == s_exit_block_last_state) return;  /* 状态未变，跳过 */
    s_exit_block_last_state = blocked;
    if (blocked) {
        lv_obj_clear_flag(s_exit_block_halo, LV_OBJ_FLAG_HIDDEN);
        ai_led_set(true, false);   /* halo亮：红灯亮、绿灯灭 */
    } else {
        lv_obj_add_flag(s_exit_block_halo, LV_OBJ_FLAG_HIDDEN);
        ai_led_set(false, true);   /* halo灭：红灯灭、绿灯亮 */
    }
}
#endif

static void ai_gesture_cb(lv_event_t * e)
{
    lv_indev_t *indev = lv_indev_active();
    lv_dir_t dir = lv_indev_get_gesture_dir(indev);
    if (dir == LV_DIR_RIGHT) {
        /* 右滑返回功能菜单页（与 camera_page/meeting_page 一致） */

        /* 状态保护：以下场景禁止右滑退出
         * 1. 普通对话：AI回复中
         * 2. 今日消息：消息归纳中
         * 3. 飞书查询：文档查询中
         * 4. 飞书对话：TTS播报中 */
#ifdef CONFIG_EXAMPLES_AI_AGENT_VELA
        if (voice_assistant_is_exit_blocked()) {
            return;
        }
#endif
        /* 等待手指释放后再处理，防止动画期间手指抬起的 CLICKED 事件
         * 误触发菜单页按钮。比 lv_indev_reset 副作用小，不会干扰屏幕加载动画。 */
        lv_indev_wait_release(indev);
        if (menu_screen == NULL) {
            menu_screen = lv_obj_create(NULL);
            create_menu_page(menu_screen);
        }
        lv_scr_load_anim(menu_screen,
            LV_SCR_LOAD_ANIM_MOVE_RIGHT, 300, 0, false);
    }
}

/* ── Public Functions ──────────────────────────────────────────── */

static void ai_screen_unloaded_cb(lv_event_t * e);

bool ai_page_is_deinit_in_progress(void)
{
    return s_deinit_in_progress;
}

void create_ai_page(lv_obj_t * parent)
{
    /* 异步清理进行中时拒绝创建新页面：
     * 旧页面的 ai_deinit_lvgl_cb 还未执行，此时创建新页面会被旧回调摧毁。 */
    if (s_deinit_in_progress) {
        syslog(LOG_WARNING, "[ai_page] create_ai_page rejected: deinit in progress\n");
        return;
    }

    lv_mem_monitor_t mon0;
    lv_mem_monitor(&mon0);
    syslog(LOG_INFO, "[ai_page] create_ai_page start: LVGL heap free=%u frag=%u%%\n",
           (unsigned)mon0.free_size, (unsigned)mon0.frag_pct);

    /* Mark page as alive so background WS callbacks can update UI safely */
    s_page_alive = true;

#if LV_USE_FREETYPE
    if (!ai_cjk_font) {
        ai_cjk_font = lv_freetype_font_create(AI_FONT_PATH,
            LV_FREETYPE_FONT_RENDER_MODE_BITMAP, AI_FONT_SIZE,
            LV_FREETYPE_FONT_STYLE_NORMAL);
        if (ai_cjk_font) {
            lv_mem_monitor_t mon1;
            lv_mem_monitor(&mon1);
            syslog(LOG_INFO, "[ai_page] after freetype font create: free=%u frag=%u%%\n",
                   (unsigned)mon1.free_size, (unsigned)mon1.frag_pct);
        } else {
            syslog(LOG_WARNING, "[ai_page] FreeType font load failed: %s\n", AI_FONT_PATH);
        }
    }
#endif

    lv_obj_set_style_bg_color(parent, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_left(parent, 60, 0);
    lv_obj_set_style_pad_right(parent, 60, 0);
    lv_obj_set_style_pad_top(parent, 25, 0);
    lv_obj_set_style_pad_bottom(parent, 5, 0);
    lv_obj_set_scrollbar_mode(parent, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(parent, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(parent, LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_clear_flag(parent, LV_OBJ_FLAG_SCROLL_MOMENTUM);

    /* Title text only — no back button */
    ai_title_label = lv_label_create(parent);
    lv_label_set_text(ai_title_label, i18n_get(STR_AI_TITLE));
    lv_obj_set_style_text_color(ai_title_label,
        lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(ai_title_label, LV_ALIGN_TOP_MID, 0, -9);
    /* 设置标题字体：优先用共享 CJK 字体（避免私有字体被 FreeType 缓存淘汰），
     * fallback 到 ai_cjk_font，再 fallback 到 simsun_16_cjk。
     * 之前标题未设字体导致中文显示方格。 */
    {
        lv_font_t *cjk = i18n_get_cjk_font();
        if (cjk) {
            lv_obj_set_style_text_font(ai_title_label, cjk, 0);
        } else if (ai_cjk_font) {
            lv_obj_set_style_text_font(ai_title_label, ai_cjk_font, 0);
        }
#if LV_FONT_SIMSUN_16_CJK
        else {
            lv_obj_set_style_text_font(ai_title_label, &lv_font_simsun_16_cjk, 0);
        }
#endif
    }

    /* Chat area (scrollable, no border/bg — bubble style) */
    ai_chat_area = lv_obj_create(parent);
    lv_obj_set_size(ai_chat_area, 400, 300);
    lv_obj_align(ai_chat_area, LV_ALIGN_TOP_MID, 0, 36);
    lv_obj_set_style_bg_opa(ai_chat_area, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ai_chat_area, 0, 0);
    lv_obj_set_style_radius(ai_chat_area, 0, 0);
    lv_obj_set_style_pad_all(ai_chat_area, 10, 0);
    lv_obj_set_flex_flow(ai_chat_area, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(ai_chat_area,
        LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(ai_chat_area, 8, 0);
    lv_obj_set_scroll_dir(ai_chat_area, LV_DIR_VER);

    /* Wakeup status label (displayed above chat area) */
    ai_wakeup_label = lv_label_create(parent);
    lv_label_set_text(ai_wakeup_label, i18n_get(STR_AI_WAITING));
    lv_obj_set_style_text_color(ai_wakeup_label,
        lv_color_hex(0x0088FF), 0);
#if LV_USE_FREETYPE
    if (ai_cjk_font)
        lv_obj_set_style_text_font(ai_wakeup_label, ai_cjk_font, 0);
    else
#endif
#if LV_FONT_SIMSUN_16_CJK
    lv_obj_set_style_text_font(ai_wakeup_label,
        &lv_font_simsun_16_cjk, 0);
#endif
    lv_obj_align(ai_wakeup_label, LV_ALIGN_BOTTOM_MID, 0, -35);

    /* Create message labels (bubble objects created dynamically) */
    for (int i = 0; i < AI_DISPLAY_LINES; i++) {
        ai_msg_texts[i][0] = '\0';
        ai_msg_labels[i] = NULL;
        ai_msg_bubbles[i] = NULL;
    }

    /* Status label */
    ai_status_label = lv_label_create(parent);
    lv_label_set_text(ai_status_label, i18n_get(STR_AI_NOT_CONNECTED));
    lv_obj_set_style_text_color(ai_status_label,
        lv_color_hex(0x888888), 0);
#if LV_USE_FREETYPE
    if (ai_cjk_font)
        lv_obj_set_style_text_font(ai_status_label, ai_cjk_font, 0);
    else
#endif
#if LV_FONT_SIMSUN_16_CJK
    lv_obj_set_style_text_font(ai_status_label,
        &lv_font_simsun_16_cjk, 0);
#endif
    lv_obj_align(ai_status_label, LV_ALIGN_BOTTOM_MID, 0, -55);

    /* Gesture handler */
    lv_obj_add_event_cb(parent, ai_gesture_cb, LV_EVENT_GESTURE, NULL);

    lv_obj_add_event_cb(parent, ai_screen_unloaded_cb,
        LV_EVENT_SCREEN_UNLOADED, NULL);

    ai_msg_index = 0;

#ifdef CONFIG_EXAMPLES_AI_AGENT_VELA
    /* 右滑禁止状态红色光晕指示器：在最外圈画5px宽红色圆环。
     * 禁止右滑退出时显示，允许时隐藏。通过定时器轮询状态动态切换。
     *
     * 定位：先 lv_obj_update_layout(lv_layer_top()) 强制刷新父对象坐标，
     * 确保 content_width 已就绪，再用 LV_ALIGN_CENTER 让 LVGL 精确计算居中
     * 坐标（避免 layer_top 未 layout 时 content_width=0 导致 CENTER 偏移）。
     *
     * 尺寸：LV_HOR_RES-10=444，外框=444×444，相对屏幕中心居中。
     * 外径 444，距屏幕边缘 5px，留出余量避免 VGLite GPU 抗锯齿扩展超出。
     *
     * deinit 时需手动删除（lv_layer_top() 上的对象不会被 lv_obj_del(ai_screen)
     * 自动回收）。清除 CLICKABLE 避免拦截手势事件。 */
  {
    const int32_t halo_size = LV_HOR_RES - 10;  /* 444 */
    lv_obj_t * top = lv_layer_top();
    lv_obj_update_layout(top);  /* 确保 layer_top 的 content_width/height 就绪 */
    s_exit_block_halo = lv_obj_create(top);
    lv_obj_remove_style_all(s_exit_block_halo);
    lv_obj_set_size(s_exit_block_halo, halo_size, halo_size);
    lv_obj_align(s_exit_block_halo, LV_ALIGN_CENTER, -5, 0);
  }
    lv_obj_set_style_radius(s_exit_block_halo, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(s_exit_block_halo, 5, 0);
    lv_obj_set_style_border_color(s_exit_block_halo, lv_color_hex(0xFF0000), 0);
    lv_obj_set_style_bg_opa(s_exit_block_halo, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(s_exit_block_halo,
        LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_exit_block_halo, LV_OBJ_FLAG_HIDDEN);
    s_exit_block_last_state = false;
    s_exit_block_timer = lv_timer_create(ai_exit_block_timer_cb,
        AI_EXIT_BLOCK_POLL_MS, NULL);
#endif

    /* 进入对话界面：红灯灭、绿灯亮（初始状态，halo未亮时） */
    ai_led_set(false, true);

    lv_mem_monitor_t mon2;
    lv_mem_monitor(&mon2);
    syslog(LOG_INFO, "[ai_page] create_ai_page done: LVGL heap free=%u frag=%u%%\n",
           (unsigned)mon2.free_size, (unsigned)mon2.frag_pct);

    /* 新需求：进入ai_page时启动本地VAD，关闭云端mic
     * - 确保云端线程运行（幂等，已启动则返回0）
     * - 退出对话模式（关闭云端mic，如在对话中）
     * - 清除suspend标志，允许后续VAD重启
     * - 启动本地VAD，监听唤醒词"小Q小Q" */
#ifdef CONFIG_EXAMPLES_AI_AGENT_VELA
    voice_assistant_start();
    voice_assistant_exit_dialogue();
    voice_assistant_resume_listening();
#endif
#ifdef CONFIG_EXAMPLES_LVGLDEMO_WAKEUP_ENABLE
    wakeup_detector_start();
#endif

    ai_page_start();
}

void ai_page_start(void)
{
    if (ai_ws_running) return;

    ai_ws_running = true;
    ai_ws_generation++;  /* New generation: any stale thread from a previous
                          * ai_page_stop()/ai_page_start() cycle will detect
                          * the mismatch and exit instead of reconnecting. */
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 16 * 1024);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_create(&ai_ws_thread, &attr, ai_ws_client_thread, NULL);
    pthread_attr_destroy(&attr);

#ifdef CONFIG_MEDIA
    ai_tts_player_start();
#endif

    LV_LOG_USER("AI page: WS client thread started");
}

void ai_page_stop(void)
{
    ai_ws_running = false;

    pthread_mutex_lock(&ai_ws_lock);
    if (ai_ws_fd >= 0) {
        close(ai_ws_fd);
        ai_ws_fd = -1;
    }
    ai_ws_connected = false;
    pthread_mutex_unlock(&ai_ws_lock);

#ifdef CONFIG_MEDIA
    ai_stop_tts();
#endif

    /* 新需求：退出ai_page时关闭云端mic + 关闭本地VAD + 复位所有标记
     * 顺序：
     *   1. 退出飞书对话模式（如在飞书对话中）
     *   2. 复位飞书快速路径标志
     *   3. suspend_listening：设置suspend标志 + 关闭云端mic + 停止本地VAD
     *      之后 on_network_disconnected/对话超时等回调检测到suspend=true
     *      不会自动重启VAD，保持"开机默认关闭"语义 */
#ifdef CONFIG_EXAMPLES_AI_AGENT_VELA
  #ifdef CONFIG_AI_AGENT_FEISHU
    if (voice_assistant_is_feishu_conversation_active()) {
        voice_assistant_exit_feishu_conversation();
    }
  #endif
    voice_assistant_reset_feishu_fast_path();
    voice_assistant_suspend_listening();
#endif

    LV_LOG_USER("AI page: stop requested");
}

void ai_page_deinit(void)
{
    /* Mark page as dead first so pending background callbacks get discarded */
    s_page_alive = false;

    /* 退出对话界面：红灯灭、绿灯灭 */
    ai_led_set(false, false);

#ifdef CONFIG_EXAMPLES_AI_AGENT_VELA
    if (s_exit_block_timer) {
        lv_timer_del(s_exit_block_timer);
        s_exit_block_timer = NULL;
    }
    /* 光晕创建在 lv_layer_top() 上，不会被 lv_obj_del(ai_screen) 回收，需手动删除 */
    if (s_exit_block_halo) {
        lv_obj_del(s_exit_block_halo);
        s_exit_block_halo = NULL;
    }
    s_exit_block_last_state = false;
#endif

    ai_page_stop();

#if LV_USE_FREETYPE
    if (ai_cjk_font) {
        lv_freetype_font_delete(ai_cjk_font);
        ai_cjk_font = NULL;
    }
#endif

    ai_title_label = NULL;
    ai_status_label = NULL;
    ai_chat_area = NULL;
    ai_wakeup_label = NULL;
    for (int i = 0; i < AI_DISPLAY_LINES; i++) {
        ai_msg_labels[i] = NULL;
        ai_msg_bubbles[i] = NULL;
        ai_msg_texts[i][0] = '\0';
    }
    ai_msg_index = 0;
    ai_ws_preread_len = 0;

    if (ai_screen) {
        lv_obj_del(ai_screen);
        ai_screen = NULL;
    }
}

static void ai_deinit_async_cb(void * arg)
{
    (void)arg;
    ai_page_deinit();
}

/* LVGL 线程执行：仅清理 LVGL 对象（屏幕、字体、光晕），不阻塞 */
static void ai_deinit_lvgl_cb(void * arg)
{
    (void)arg;

#ifdef CONFIG_EXAMPLES_AI_AGENT_VELA
    /* 光晕创建在 lv_layer_top() 上，不会被 lv_obj_del(ai_screen) 回收，
     * 必须在此手动删除。timer 也需在 LVGL 线程中删除。 */
    if (s_exit_block_timer) {
        lv_timer_del(s_exit_block_timer);
        s_exit_block_timer = NULL;
    }
    if (s_exit_block_halo) {
        lv_obj_del(s_exit_block_halo);
        s_exit_block_halo = NULL;
    }
    s_exit_block_last_state = false;
#endif

#if LV_USE_FREETYPE
    if (ai_cjk_font) {
        lv_freetype_font_delete(ai_cjk_font);
        ai_cjk_font = NULL;
    }
#endif

    ai_title_label = NULL;
    ai_status_label = NULL;
    ai_chat_area = NULL;
    ai_wakeup_label = NULL;
    for (int i = 0; i < AI_DISPLAY_LINES; i++) {
        ai_msg_labels[i] = NULL;
        ai_msg_bubbles[i] = NULL;
        ai_msg_texts[i][0] = '\0';
    }
    ai_msg_index = 0;
    ai_ws_preread_len = 0;

    if (ai_screen) {
        lv_obj_del(ai_screen);
        ai_screen = NULL;
    }

    /* 清理完成，允许 create_ai_page 创建新页面 */
    s_deinit_in_progress = false;
}

/* 独立线程执行：ai_page_stop() 包含 pthread_join 等阻塞操作，
 * 必须在 LVGL 线程之外执行，否则会卡住 UI。 */
static void *ai_deinit_thread(void * arg)
{
    (void)arg;
    ai_page_stop();
    /* LVGL 对象清理必须回到 LVGL 线程 */
    lvgl_dispatch_async(ai_deinit_lvgl_cb, NULL);
    return NULL;
}

static void ai_screen_unloaded_cb(lv_event_t * e)
{
    (void)e;
    /* 立即标记页面已死，阻止 timer 回调继续操作光晕 */
    s_page_alive = false;

    /* 标记异步清理进行中，create_ai_page 会拒绝创建新页面直到清理完成 */
    s_deinit_in_progress = true;

    /* ai_page_stop() 内部会 pthread_join TTS 播放线程，
     * TTS 线程可能阻塞在网络 I/O 上长达数秒，
     * 因此必须在独立线程中执行，避免卡住 LVGL 主线程。 */
    pthread_t tid;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 8192);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_create(&tid, &attr, ai_deinit_thread, NULL);
    pthread_attr_destroy(&attr);
}
