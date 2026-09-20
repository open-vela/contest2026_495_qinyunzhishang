/****************************************************************************
 * apps/examples/lvgldemo/camera_page.c
 *
 * Camera page — remote capture via UART to Lubancat 2N.
 * UI: image preview area + info log area + capture/get buttons.
 * UART: /dev/ttyS1 @ 921600, sends CAPTURE_REQ (CMD=0x01).
 *
 ****************************************************************************/

#include <nuttx/config.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <termios.h>
#include <pthread.h>
#include <syslog.h>
#include <stdarg.h>
#include <poll.h>
#include <lvgl/lvgl.h>

#include <jpeglib.h>
#include <setjmp.h>

#include <sys/stat.h>
#include "camera_page.h"
#include "lvgldemo_common.h"
#include "ai_page.h"
#include "voice_assistant.h"
#include "camera_ai_page.h"
#include "camera_feishu_doc.h"
#include "i18n.h"
#include "channels/feishu_bot.h"
#include "agent_config.h"
#include "lvgl_dispatch.h"
#include "menu_page.h"
#include "wakeup_detector.h"

/* 声明外部转换的图片资源 */
LV_IMAGE_DECLARE(image_preview);
LV_IMAGE_DECLARE(camera_page_photo_on);
LV_IMAGE_DECLARE(camera_page_photo_off);
LV_IMAGE_DECLARE(camera_page_ai_on);
LV_IMAGE_DECLARE(camera_page_ai_off);

/* camera页面根对象保存，用于返回 */
static lv_obj_t *cam_screen_obj = NULL;

#define CAM_UART_DEV      "/dev/ttyS1"
#define CAM_UART_BAUD     921600

#define CAM_UART_SYNC     0xAA55
#define CAM_CMD_CAPTURE_REQ    0x01
#define CAM_CMD_CAPTURE_ACK    0x02
#define CAM_CMD_CAPTURE_FAIL   0x03
#define CAM_CMD_GET_IMAGE_REQ  0x04
#define CAM_CMD_GET_IMAGE_ACK  0x05
#define CAM_CMD_SPI_READY      0x07
#define CAM_CMD_FRAME_ACK      0x08
#define CAM_CMD_SPI_READY_BIG  0x09

#define CAM_SPI_DEV           "/dev/spislv0"
#define CAM_SPI_MAGIC         0x494D4731
#define CAM_SPI_HDR_SIZE      19
#define CAM_SPI_MAX_PAYLOAD   256
#define CAM_SPI_FRAME_SIZE    (CAM_SPI_HDR_SIZE + CAM_SPI_MAX_PAYLOAD + 2)
#define CAM_SPI_TYPE_THUMB_DATA  1
#define CAM_SPI_TYPE_THUMB_END   2
#define CAM_SPI_TYPE_BIG_DATA    3
#define CAM_SPI_TYPE_BIG_END     4

#define CAM_MAX_LOG_LINES  8
#define CAM_LOG_MAX_LEN    128

#define CAM_LOG_AREA_HEIGHT      28
#define CAM_LOG_AREA_MARGIN_TOP  28
#define CAM_BTN_AREA_HEIGHT      120
#define CAM_BTN_AREA_MARGIN_BOT  5  /* 整体下移15px */
#define CAM_BTN_SIZE_W           120
#define CAM_BTN_SIZE_H           48
#define CAM_BTN_CONTAINER_WIDTH  360
#define CAM_TITLE_MARGIN_TOP     40
#define CAM_FONT_PATH "/emmc/font/MiSans-Normal.ttf"
#define CAM_SHUTTER_WAV "/emmc/xiaoqxiaoq/camera_shutter.wav"
#define CAM_TITLE_FONT_SIZE      28
#define CAM_BTN_FONT_SIZE        24
#define CAM_COUNTDOWN_FONT_SIZE  96
#define CAM_COUNTDOWN_START      5
#define CAM_COUNTDOWN_PERIOD_MS  1000
#define CAM_PREVIEW_MAX_W        360  /* 16:9预览区域最大宽度 */
#define CAM_PREVIEW_RATIO_W      16
#define CAM_PREVIEW_RATIO_H      9

static lv_font_t *cam_title_font = NULL;
static lv_font_t *cam_btn_font = NULL;
static lv_font_t *cam_countdown_font = NULL;

/* 字体应用函数 - 优先FreeType完整字体(所有中文都有)，回退到内置宋体 */
static void cam_apply_font(lv_obj_t *obj, lv_font_t *ft_font)
{
#if LV_USE_FREETYPE
    if (ft_font) {
        lv_obj_set_style_text_font(obj, ft_font, 0);
        return;
    }
#endif
#if LV_FONT_SIMSUN_16_CJK
    lv_obj_set_style_text_font(obj, &lv_font_simsun_16_cjk, 0);
#else
    lv_obj_set_style_text_font(obj, LV_FONT_DEFAULT, 0);
#endif
}

static lv_obj_t *cam_preview_img = NULL;
static lv_obj_t *cam_thumb_img = NULL;
static lv_obj_t *cam_capture_btn = NULL;
static lv_obj_t *cam_ai_btn = NULL;
static lv_obj_t *cam_placeholder_icon = NULL;  /* 拍照前占位图标容器 */
static lv_obj_t *cam_log_labels[CAM_MAX_LOG_LINES];
static char cam_log_texts[CAM_MAX_LOG_LINES][CAM_LOG_MAX_LEN];
static int cam_log_index = 0;

static bool cam_thumb_fullscreen = false;
static int32_t cam_thumb_off_x = 0;
static int32_t cam_thumb_off_y = 0;
static bool cam_thumb_dragged = false;

static lv_obj_t *cam_status_label = NULL;       /* 预览区下方状态文字（拍照中/失败提示） */
static lv_obj_t *cam_countdown_label = NULL;    /* 预览区中央大字号倒计时 */
static lv_timer_t *cam_countdown_timer = NULL;  /* 倒计时定时器（1s 周期） */
static int cam_countdown_value = 0;             /* 当前倒计时值 */
static lv_timer_t *cam_fail_hint_timer = NULL;  /* 失败提示3s定时器 */
static bool cam_has_photo = false;              /* 是否已成功拍照（控制按钮图标 photo_on/off） */
static bool cam_return_from_ai = false;         /* 从 AI 分析界面返回标志，用于保留缩略图 */
static bool cam_ai_chat_active = false;         /* AI 分析聊天页已存在，C状态：左右滑只切屏不重建 */

static int cam_uart_fd = -1;
static pthread_t cam_uart_rx_thread;
static volatile bool cam_uart_running = false;
static pthread_mutex_t cam_uart_tx_lock = PTHREAD_MUTEX_INITIALIZER;

static int cam_spi_fd = -1;
static pthread_t cam_spi_rx_thread;
static volatile bool cam_spi_running = false;
static volatile bool cam_spi_recv_active = false;
static volatile bool cam_spi_is_big_image = false;
static uint32_t cam_session_id = 0;
static uint8_t *cam_thumb_buf = NULL;
static uint32_t cam_thumb_total = 0;
static uint32_t cam_thumb_offset = 0;
static uint16_t cam_thumb_w = 0;
static uint16_t cam_thumb_h = 0;
static lv_image_dsc_t cam_thumb_dsc;
static lv_timer_t *cam_spi_timeout_timer = NULL;
static volatile uint32_t cam_spi_last_recv_tick = 0;  /* SPI 最后收到有效帧的时刻，用于空闲超时检测 */

static int cam_big_fd = -1;
static char cam_big_filename[64] = {0};
static uint32_t cam_big_total = 0;
static uint32_t cam_big_offset = 0;
static uint32_t cam_big_session = 0;

static volatile bool cam_busy = false;

/* ── 飞书发送相关 ──────────────────────────────────────────── */
static char s_feishu_receive_id[64] = {0};
static const char *s_feishu_id_type = "chat_id";  /* "chat_id" 或 "open_id" */

typedef struct {
    char image_path[64];
    bool start_ai_after;  /* true: 飞书上传成功后启动 AI 分析 */
} cam_feishu_arg_t;

#define CAM_RECV_BITMAP_SIZE 1024
static uint32_t cam_ack_next_offset = 0;
static uint16_t cam_recv_nframes = 0;
static uint8_t cam_recv_bitmap[CAM_RECV_BITMAP_SIZE];

static uint16_t cam_crc16_ccitt(const uint8_t *data, uint16_t len)
{
    uint16_t crc = 0xFFFF;
    for (uint16_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int j = 0; j < 8; j++) {
            if (crc & 0x8000)
                crc = (crc << 1) ^ 0x1021;
            else
                crc <<= 1;
        }
    }
    return crc;
}

static int cam_uart_open(void)
{
    int fd = open(CAM_UART_DEV, O_RDWR | O_NOCTTY);
    if (fd < 0) {
        syslog(LOG_ERR, "[camera_page] open %s failed: %d\n", CAM_UART_DEV, errno);
        return -1;
    }

    struct termios tio;
    tcgetattr(fd, &tio);
    cfsetispeed(&tio, B921600);
    cfsetospeed(&tio, B921600);
    tio.c_cflag &= ~(PARENB | CSTOPB | CSIZE | CRTSCTS);
    tio.c_cflag |= CS8 | CLOCAL | CREAD;
    tio.c_iflag &= ~(IXON | IXOFF | IXANY | ICRNL | INLCR);
    tio.c_oflag &= ~OPOST;
    tio.c_lflag &= ~(ICANON | ECHO | ISIG);
    tio.c_cc[VMIN] = 1;
    tio.c_cc[VTIME] = 0;
    tcsetattr(fd, TCSANOW, &tio);
    tcflush(fd, TCIOFLUSH);

    return fd;
}

static int cam_uart_send_frame(uint8_t cmd, const uint8_t *payload, uint16_t payload_len)
{
    uint16_t frame_len = 2 + payload_len;
    uint16_t total = 2 + 2 + 1 + 1 + payload_len + 2;
    uint8_t *buf = malloc(total);
    if (!buf) return -1;

    buf[0] = 0xAA;
    buf[1] = 0x55;
    buf[2] = (frame_len >> 8) & 0xFF;
    buf[3] = frame_len & 0xFF;
    buf[4] = cmd;
    buf[5] = 0;
    if (payload_len > 0 && payload)
        memcpy(buf + 6, payload, payload_len);

    uint16_t crc = cam_crc16_ccitt(buf + 2, 4 + payload_len);
    buf[6 + payload_len] = (crc >> 8) & 0xFF;
    buf[6 + payload_len + 1] = crc & 0xFF;

    pthread_mutex_lock(&cam_uart_tx_lock);
    ssize_t written = write(cam_uart_fd, buf, total);
    pthread_mutex_unlock(&cam_uart_tx_lock);
    usleep(5000);

    free(buf);
    return (written == total) ? 0 : -1;
}

typedef struct {
    char text[CAM_LOG_MAX_LEN];
} cam_async_log_t;

static void cam_add_log_async_cb(void *data)
{
    cam_async_log_t *msg = (cam_async_log_t *)data;
    if (!msg) return;
    if (cam_log_labels[0]) {
        lv_label_set_text(cam_log_labels[0], msg->text);
    }
    free(msg);
}

static void cam_add_log(const char *fmt, ...)
{
    cam_async_log_t *msg = malloc(sizeof(cam_async_log_t));
    if (!msg) return;

    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg->text, CAM_LOG_MAX_LEN, fmt, ap);
    va_end(ap);

    if (!lvgl_dispatch_async(cam_add_log_async_cb, msg)) {
        free(msg);
    }
}

typedef struct {
    bool enabled;
} cam_async_btn_t;

static void cam_set_buttons_async_cb(void *data)
{
    cam_async_btn_t *p = (cam_async_btn_t *)data;
    if (!p) return;
    /* 仅控制拍照按钮；AI 按钮由 cam_set_ai_btn_enabled_async 独立控制。
     * 纯 image 对象无 DISABLED 状态，用透明度区分可用性：
     *   enabled  → OPA_COVER   + photo_on  （可拍照）
     *   disabled → OPA_50      + photo_off （拍照中） */
    if (cam_capture_btn) {
        if (p->enabled) {
            lv_obj_set_style_img_opa(cam_capture_btn, LV_OPA_COVER, 0);
            lv_image_set_src(cam_capture_btn, &camera_page_photo_on);
        } else {
            lv_obj_set_style_img_opa(cam_capture_btn, LV_OPA_50, 0);
            lv_image_set_src(cam_capture_btn, &camera_page_photo_off);
        }
    }
    free(p);
}

static void cam_set_buttons_enabled(bool enabled)
{
    cam_async_btn_t *p = malloc(sizeof(cam_async_btn_t));
    if (!p) return;
    p->enabled = enabled;
    if (!lvgl_dispatch_async(cam_set_buttons_async_cb, p)) {
        free(p);
    }
}

/* ── AI 按钮独立可用性控制（纯 image 用透明度表示状态） ── */
static void cam_set_ai_btn_async_cb(void *data)
{
    bool enabled = (bool)(intptr_t)data;
    if (cam_ai_btn) {
        if (enabled) {
            lv_obj_set_style_img_opa(cam_ai_btn, LV_OPA_COVER, 0);
            lv_image_set_src(cam_ai_btn, &camera_page_ai_on);
        } else {
            lv_obj_set_style_img_opa(cam_ai_btn, LV_OPA_50, 0);
            lv_image_set_src(cam_ai_btn, &camera_page_ai_off);
        }
    }
}

static void cam_set_ai_btn_enabled_async(bool enabled)
{
    lvgl_dispatch_async(cam_set_ai_btn_async_cb, (void *)(intptr_t)enabled);
}

/* ── 预览区下方状态文字显示控制（拍照中/失败提示） ────────── */
typedef struct {
    char text[48];
    bool show;
    bool is_fail;  /* true=浅红 0xFF6666, false=白色 */
} cam_status_msg_t;

static void cam_show_status_async_cb(void *data)
{
    cam_status_msg_t *p = (cam_status_msg_t *)data;
    if (!p) return;
    if (cam_status_label) {
        if (p->show) {
            lv_label_set_text(cam_status_label, p->text);
            lv_obj_set_style_text_color(cam_status_label,
                p->is_fail ? lv_color_hex(0xFF6666) : lv_color_white(), 0);
            lv_obj_clear_flag(cam_status_label, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(cam_status_label, LV_OBJ_FLAG_HIDDEN);
        }
    }
    free(p);
}

static void cam_show_status_async(const char *text, bool show, bool is_fail)
{
    cam_status_msg_t *p = malloc(sizeof(cam_status_msg_t));
    if (!p) return;
    if (text) {
        strncpy(p->text, text, sizeof(p->text) - 1);
        p->text[sizeof(p->text) - 1] = '\0';
    } else {
        p->text[0] = '\0';
    }
    p->show = show;
    p->is_fail = is_fail;
    if (!lvgl_dispatch_async(cam_show_status_async_cb, p)) {
        free(p);
    }
}

/* ── 失败提示 3s 定时器（LVGL 线程） ──────────────────────── */
static void cam_fail_hint_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    cam_fail_hint_timer = NULL;
    /* 状态文字始终 3s 后隐藏（适用于所有临时提示：拍照失败 / AI 启动中 / AI 连接中等） */
    if (cam_status_label) lv_obj_add_flag(cam_status_label, LV_OBJ_FLAG_HIDDEN);
    /* 仅在空闲且未拍照时恢复占位图标，避免覆盖新一轮拍照/已拍照状态 */
    if (!cam_busy && !cam_has_photo) {
        if (cam_placeholder_icon) lv_obj_clear_flag(cam_placeholder_icon, LV_OBJ_FLAG_HIDDEN);
    }
}

static void cam_do_start_fail_hint_timer(void *data)
{
    (void)data;
    if (cam_fail_hint_timer) {
        lv_timer_del(cam_fail_hint_timer);
        cam_fail_hint_timer = NULL;
    }
    if (cam_countdown_timer) {
        lv_timer_del(cam_countdown_timer);
        cam_countdown_timer = NULL;
    }
    if (cam_countdown_label) {
        lv_obj_add_flag(cam_countdown_label, LV_OBJ_FLAG_HIDDEN);
    }
    cam_countdown_value = 0;
    cam_fail_hint_timer = lv_timer_create(cam_fail_hint_timer_cb, 3000, NULL);
    if (cam_fail_hint_timer)
        lv_timer_set_repeat_count(cam_fail_hint_timer, 1);
}

static void cam_start_fail_hint_async(void)
{
    lvgl_dispatch_async(cam_do_start_fail_hint_timer, NULL);
}

/* ── 预览区中央倒计时（点击拍照后立即启动，图片就绪/失败时取消） ── */
static void cam_countdown_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (!cam_countdown_label) return;

    cam_countdown_value--;
    if (cam_countdown_value > 0) {
        char buf[8];
        snprintf(buf, sizeof(buf), "%d", cam_countdown_value);
        lv_label_set_text(cam_countdown_label, buf);
    } else {
        /* 倒计时归零但图片未就绪：不判失败，切换为“处理中”文案继续等待。
         * 文案用小字号(24px)，与倒计时大数字(96px)区分 */
        lv_label_set_text(cam_countdown_label, i18n_get(STR_CAM_PROCESSING));
        cam_apply_font(cam_countdown_label, cam_btn_font);
        if (cam_countdown_timer) {
            lv_timer_del(cam_countdown_timer);
            cam_countdown_timer = NULL;
        }
    }
}

/* LVGL 线程执行：启动倒计时（重拍时安全重置） */
static void cam_do_start_countdown(void *data)
{
    (void)data;
    if (cam_countdown_timer) {
        lv_timer_del(cam_countdown_timer);
        cam_countdown_timer = NULL;
    }
    cam_countdown_value = CAM_COUNTDOWN_START;
    if (cam_countdown_label) {
        char buf[8];
        snprintf(buf, sizeof(buf), "%d", cam_countdown_value);
        lv_label_set_text(cam_countdown_label, buf);
        /* 重拍时恢复大字号（上次归零可能改成了小字号“处理中”） */
#if LV_USE_FREETYPE
        if (cam_countdown_font) {
            lv_obj_set_style_text_font(cam_countdown_label, cam_countdown_font, 0);
        } else
#endif
        if (cam_title_font) {
            lv_obj_set_style_text_font(cam_countdown_label, cam_title_font, 0);
        } else {
            cam_apply_font(cam_countdown_label, NULL);
        }
        lv_obj_clear_flag(cam_countdown_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(cam_countdown_label);
    }
    /* 隐藏占位图标，避免与倒计时数字重叠 */
    if (cam_placeholder_icon) {
        lv_obj_add_flag(cam_placeholder_icon, LV_OBJ_FLAG_HIDDEN);
    }
    cam_countdown_timer = lv_timer_create(cam_countdown_timer_cb,
                                          CAM_COUNTDOWN_PERIOD_MS, NULL);
}

/* LVGL 线程执行：停止并隐藏倒计时 */
static void cam_do_stop_countdown(void *data)
{
    (void)data;
    if (cam_countdown_timer) {
        lv_timer_del(cam_countdown_timer);
        cam_countdown_timer = NULL;
    }
    if (cam_countdown_label) {
        lv_obj_add_flag(cam_countdown_label, LV_OBJ_FLAG_HIDDEN);
    }
}

static void cam_start_countdown(void)
{
    lvgl_dispatch_async(cam_do_start_countdown, NULL);
}

static void cam_stop_countdown(void)
{
    lvgl_dispatch_async(cam_do_stop_countdown, NULL);
}

/* ── 统一拍照失败处理（须在 LVGL 线程调用） ──────────────────
 * stage: 0=CAPTURE 阶段, 1=缩略图/衔接阶段, 2=大图阶段
 * 清理缓冲/半成品文件、显示失败提示、恢复按钮与文案 */
static void cam_handle_capture_failure(int stage)
{
    if (stage >= 1 && cam_thumb_buf) {
        free(cam_thumb_buf);
        cam_thumb_buf = NULL;
    }
    cam_thumb_offset = 0;
    cam_thumb_total = 0;

    if (stage == 2) {
        if (cam_big_fd >= 0) {
            close(cam_big_fd);
            cam_big_fd = -1;
        }
        if (cam_big_filename[0] != '\0') {
            unlink(cam_big_filename);  /* 删除半成品，避免堆积坏文件 */
            cam_big_filename[0] = '\0';
        }
    } else {
        cam_big_filename[0] = '\0';
    }

    cam_spi_is_big_image = false;
    cam_spi_recv_active = false;

    cam_show_status_async(i18n_get(STR_CAM_CAPTURE_FAIL), true, true);
    cam_start_fail_hint_async();
    cam_stop_countdown();

    cam_has_photo = false;
    cam_busy = false;
    cam_set_buttons_enabled(true);           /* 拍照按钮恢复可用 → photo_on */
    cam_set_ai_btn_enabled_async(false);     /* AI 按钮保持/恢复 disabled */
}

static void cam_handle_capture_failure_async_cb(void *data)
{
    int stage = (int)(intptr_t)data;
    cam_handle_capture_failure(stage);
}

static void cam_handle_capture_failure_async(int stage)
{
    lvgl_dispatch_async(cam_handle_capture_failure_async_cb, (void *)(intptr_t)stage);
}

/* ── Timeout watchdog (5s) ────────────────────────────────────── */
static lv_timer_t *cam_timeout_timer = NULL;

static void cam_timeout_cb(lv_timer_t *timer)
{
    (void)timer;
    syslog(LOG_INFO, "[camera_page] timeout fired\n");
    cam_timeout_timer = NULL;
    /* 缩略图已收到但大图未开传 → 阶段1（等 GET_IMAGE_ACK 超时）；否则阶段0 */
    int stage = (cam_thumb_buf != NULL && cam_big_filename[0] == '\0') ? 1 : 0;
    cam_add_log(stage == 1 ? "GET_IMAGE timeout" : "Capture timeout");
    cam_handle_capture_failure(stage);
}

/* LVGL 线程执行：实际删除 capture 超时定时器。
 * cam_timeout_timer 的读写与 lv_timer_del 都只在本回调（LVGL 线程）中进行，
 * 避免与 lv_timer_handler 并发修改定时器链表导致死机。 */
static void cam_do_stop_timeout(void *data)
{
    (void)data;
    if (cam_timeout_timer) {
        lv_timer_del(cam_timeout_timer);
        cam_timeout_timer = NULL;
    }
}

/* 可在 UART RX 线程调用：派发到 LVGL 线程执行 lv_timer_del。 */
static void cam_stop_timeout(void)
{
    lvgl_dispatch_async(cam_do_stop_timeout, NULL);
}

static void cam_start_timeout(uint32_t ms)
{
    if (cam_timeout_timer) {
        lv_timer_del(cam_timeout_timer);
        cam_timeout_timer = NULL;
    }
    syslog(LOG_INFO, "[camera_page] start %ums timeout\n", (unsigned)ms);
    cam_timeout_timer = lv_timer_create(cam_timeout_cb, ms, NULL);
    if (cam_timeout_timer) {
        lv_timer_set_repeat_count(cam_timeout_timer, 1);
    } else {
        syslog(LOG_ERR, "[camera_page] failed to create timeout timer\n");
    }
}

/* ── SPI thumbnail timeout (30s) ─────────────────────────────── */
static void cam_spi_reopen(void)
{
    if (cam_spi_fd >= 0) {
        close(cam_spi_fd);
        cam_spi_fd = -1;
    }
    usleep(50000);
    cam_spi_fd = open(CAM_SPI_DEV, O_RDWR | O_NONBLOCK);
    if (cam_spi_fd >= 0) {
        syslog(LOG_INFO, "[camera_page] SPI device reopened\n");
    } else {
        syslog(LOG_ERR, "[camera_page] SPI reopen failed: %d\n", errno);
    }
}

static void cam_spi_timeout_cb(lv_timer_t *timer)
{
    (void)timer;
    /* 空闲超时检查：距上次收到有效帧不足 20s 则继续等待 */
    uint32_t now = lv_tick_get();
    if (now - cam_spi_last_recv_tick < 20000) {
        return;  /* 数据仍在流动，定时器周期性续命 */
    }
    syslog(LOG_ERR, "[camera_page] SPI idle timeout fired (idle=%lums)\n",
           (unsigned long)(now - cam_spi_last_recv_tick));
    /* 真正超时：删除周期性定时器并失败处理 */
    if (cam_spi_timeout_timer) {
        lv_timer_del(cam_spi_timeout_timer);
        cam_spi_timeout_timer = NULL;
    }
    int stage = cam_spi_is_big_image ? 2 : 1;
    cam_add_log(stage == 2 ? "Big image timeout" : "Thumbnail timeout");
    cam_handle_capture_failure(stage);
    cam_spi_reopen();
}

/* LVGL 线程执行：重建 SPI 超时定时器。
 * cam_spi_timeout_timer 的读写与 lv_timer 操作都只在本回调（LVGL 线程）中进行。 */
static void cam_do_start_spi_timeout(void *data)
{
    (void)data;
    if (cam_spi_timeout_timer) {
        lv_timer_del(cam_spi_timeout_timer);
        cam_spi_timeout_timer = NULL;
    }
    cam_spi_last_recv_tick = lv_tick_get();  /* 初始化为启动时刻 */
    /* 周期性 5s 检查空闲时间，由 cam_spi_timeout_cb 判断是否真正超时 */
    cam_spi_timeout_timer = lv_timer_create(cam_spi_timeout_cb, 5000, NULL);
    /* 不设 repeat_count，周期性运行，超时后在回调内自行 del */
}

/* LVGL 线程执行：删除 SPI 超时定时器。 */
static void cam_do_stop_spi_timeout(void *data)
{
    (void)data;
    if (cam_spi_timeout_timer) {
        lv_timer_del(cam_spi_timeout_timer);
        cam_spi_timeout_timer = NULL;
    }
}

/* 可在 UART/SPI RX 线程调用：派发到 LVGL 线程执行，
 * 避免与 lv_timer_handler 并发修改定时器链表导致死机。 */
static void cam_start_spi_timeout(void)
{
    lvgl_dispatch_async(cam_do_start_spi_timeout, NULL);
}

static void cam_stop_spi_timeout(void)
{
    lvgl_dispatch_async(cam_do_stop_spi_timeout, NULL);
}

/* ── Send FRAME_ACK via UART ─────────────────────────────────── */
static void cam_send_frame_ack(uint32_t session, uint32_t offset, uint8_t ok)
{
    uint8_t payload[9];
    payload[0] = (session >> 24) & 0xFF;
    payload[1] = (session >> 16) & 0xFF;
    payload[2] = (session >> 8) & 0xFF;
    payload[3] = session & 0xFF;
    payload[4] = (offset >> 24) & 0xFF;
    payload[5] = (offset >> 16) & 0xFF;
    payload[6] = (offset >> 8) & 0xFF;
    payload[7] = offset & 0xFF;
    payload[8] = ok;
    cam_uart_send_frame(CAM_CMD_FRAME_ACK, payload, 9);
}

/* ── Show thumbnail on UI (called from LVGL thread) ──────────── */
typedef struct {
    uint16_t w;
    uint16_t h;
} cam_thumb_show_t;

static void cam_draw_placeholder_icon(lv_obj_t *parent)
{
    /* 绘制带笑脸的图片框占位图标 - 白色线条 */
    lv_color_t white = lv_color_white();
    
    /* 左上角圆角弧线 */
    static const lv_point_t pts_tl[] = {{0, 40}, {0, 10}, {10, 0}, {60, 0}};
    lv_obj_t *l_tl = lv_line_create(parent);
    lv_line_set_points(l_tl, pts_tl, 4);
    lv_obj_set_style_line_color(l_tl, white, 0);
    lv_obj_set_style_line_width(l_tl, 8, 0);
    lv_obj_set_style_line_rounded(l_tl, true, 0);
    lv_obj_align(l_tl, LV_ALIGN_CENTER, -90, -90);

    /* 右上角圆角弧线 */
    static const lv_point_t pts_tr[] = {{120, 0}, {170, 0}, {180, 10}, {180, 40}};
    lv_obj_t *l_tr = lv_line_create(parent);
    lv_line_set_points(l_tr, pts_tr, 4);
    lv_obj_set_style_line_color(l_tr, white, 0);
    lv_obj_set_style_line_width(l_tr, 8, 0);
    lv_obj_set_style_line_rounded(l_tr, true, 0);
    lv_obj_align(l_tr, LV_ALIGN_CENTER, -90, -90);

    /* 左下角圆角弧线 */
    static const lv_point_t pts_bl[] = {{0, 140}, {0, 170}, {10, 180}, {60, 180}};
    lv_obj_t *l_bl = lv_line_create(parent);
    lv_line_set_points(l_bl, pts_bl, 4);
    lv_obj_set_style_line_color(l_bl, white, 0);
    lv_obj_set_style_line_width(l_bl, 8, 0);
    lv_obj_set_style_line_rounded(l_bl, true, 0);
    lv_obj_align(l_bl, LV_ALIGN_CENTER, -90, -90);

    /* 右下角圆角弧线 */
    static const lv_point_t pts_br[] = {{120, 180}, {170, 180}, {180, 170}, {180, 140}};
    lv_obj_t *l_br = lv_line_create(parent);
    lv_line_set_points(l_br, pts_br, 4);
    lv_obj_set_style_line_color(l_br, white, 0);
    lv_obj_set_style_line_width(l_br, 8, 0);
    lv_obj_set_style_line_rounded(l_br, true, 0);
    lv_obj_align(l_br, LV_ALIGN_CENTER, -90, -90);

    /* 山峰线 */
    static const lv_point_t pts_mountain[] = {{0, 120}, {50, 70}, {90, 110}, {130, 60}, {180, 120}};
    lv_obj_t *l_mountain = lv_line_create(parent);
    lv_line_set_points(l_mountain, pts_mountain, 5);
    lv_obj_set_style_line_color(l_mountain, white, 0);
    lv_obj_set_style_line_width(l_mountain, 8, 0);
    lv_obj_set_style_line_rounded(l_mountain, true, 0);
    lv_obj_align(l_mountain, LV_ALIGN_CENTER, -90, -90);

    /* 笑脸 - 左眼 */
    lv_obj_t *eye_l = lv_obj_create(parent);
    lv_obj_remove_style_all(eye_l);
    lv_obj_set_size(eye_l, 14, 20);
    lv_obj_set_style_radius(eye_l, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(eye_l, white, 0);
    lv_obj_set_style_bg_opa(eye_l, LV_OPA_COVER, 0);
    lv_obj_align(eye_l, LV_ALIGN_CENTER, -25, -20);

    /* 笑脸 - 右眼 */
    lv_obj_t *eye_r = lv_obj_create(parent);
    lv_obj_remove_style_all(eye_r);
    lv_obj_set_size(eye_r, 14, 20);
    lv_obj_set_style_radius(eye_r, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(eye_r, white, 0);
    lv_obj_set_style_bg_opa(eye_r, LV_OPA_COVER, 0);
    lv_obj_align(eye_r, LV_ALIGN_CENTER, 25, -20);

    /* 笑脸 - 微笑嘴 */
    static const lv_point_t pts_smile[] = {{-20, 0}, {0, 15}, {20, 0}};
    lv_obj_t *smile = lv_line_create(parent);
    lv_line_set_points(smile, pts_smile, 3);
    lv_obj_set_style_line_color(smile, white, 0);
    lv_obj_set_style_line_width(smile, 8, 0);
    lv_obj_set_style_line_rounded(smile, true, 0);
    lv_obj_align(smile, LV_ALIGN_CENTER, 0, 15);
}

/* ── libjpeg-turbo 错误处理（避免损坏 JPG 调用 exit） ──────── */
struct cam_jpg_error_mgr {
    struct jpeg_error_mgr pub;
    jmp_buf setjmp_buffer;
};

static void cam_jpg_error_exit(j_common_ptr cinfo)
{
    struct cam_jpg_error_mgr *myerr = (struct cam_jpg_error_mgr *)cinfo->err;
    longjmp(myerr->setjmp_buffer, 1);
}

/* 用 libjpeg-turbo 将 JPG 解码为 RGB565 小端 buffer（与 2N 缩略图格式完全一致）。
 * 利用 libjpeg 内置 scale_denom 粗缩放 + 逐行采样精缩放到 target_w×target_h。
 * 成功返回 malloc 的 buffer（调用者 free），失败返回 NULL。 */
static uint8_t *cam_decode_jpg_to_rgb565(const char *path,
                                         uint16_t target_w,
                                         uint16_t target_h)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        syslog(LOG_ERR, "[camera_page] jpg open fail: %s err=%d\n", path, errno);
        return NULL;
    }

    struct jpeg_decompress_struct cinfo;
    struct cam_jpg_error_mgr jerr;
    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = cam_jpg_error_exit;

    /* 必须用 volatile：setjmp/longjmp 后非 volatile 局部变量值未定义（C99），
     * 会导致错误处理路径 free(垃圾值) 触发 mm_malloc_size.c assert */
    volatile uint8_t *out = NULL;
    volatile uint8_t *rgb_row = NULL;

    if (setjmp(jerr.setjmp_buffer)) {
        syslog(LOG_ERR, "[camera_page] jpg decode error: %s (img=%ux%u scale=%u out=%ux%u scanline=%u)\n",
               path, cinfo.image_width, cinfo.image_height, cinfo.scale_denom,
               cinfo.output_width, cinfo.output_height, cinfo.output_scanline);
        /* 正式方案：安全释放内存。读完所有行后 finish 不报错，longjmp 仅在
         * 极端情况触发，此时堆元数据未被破坏，free/destroy 安全 */
        if (rgb_row) { free((void *)rgb_row); rgb_row = NULL; }
        if (out) { free((void *)out); out = NULL; }
        jpeg_destroy_decompress(&cinfo);
        fclose(fp);
        return NULL;
    }

    jpeg_create_decompress(&cinfo);
    jpeg_stdio_src(&cinfo, fp);
    jpeg_read_header(&cinfo, TRUE);

    /* scale_denom=4：1280×720 → 320×180，减少 16 倍数据量加速解码
     * 280×158 目标尺寸，320×180 已足够，无需 1:1 */
    cinfo.scale_denom = 4;
    cinfo.out_color_space = JCS_RGB;
    syslog(LOG_INFO, "[camera_page] jpg hdr: img=%ux%u scale_denom=%u\n",
           cinfo.image_width, cinfo.image_height, cinfo.scale_denom);
    jpeg_start_decompress(&cinfo);
    syslog(LOG_INFO, "[camera_page] jpg start: out=%ux%u components=%u\n",
           cinfo.output_width, cinfo.output_height, cinfo.output_components);

    uint16_t src_w = cinfo.output_width;
    uint16_t src_h = cinfo.output_height;
    uint32_t buf_size = (uint32_t)target_w * target_h * 2;
    out = (volatile uint8_t *)malloc(buf_size);
    if (!out) {
        syslog(LOG_ERR, "[camera_page] rgb565 malloc %u fail\n", (unsigned)buf_size);
        longjmp(jerr.setjmp_buffer, 1);
    }
    rgb_row = (volatile uint8_t *)malloc((uint32_t)src_w * 3);
    if (!rgb_row) {
        syslog(LOG_ERR, "[camera_page] rgb_row malloc %u fail\n", (unsigned)(src_w * 3));
        longjmp(jerr.setjmp_buffer, 1);
    }

    /* 逐行解码，按比例采样到 target_w×target_h，转 RGB565 小端。
     * 用非 volatile 临时指针避免每个像素访问都走 volatile（性能损耗） */
    uint8_t *row_buf = (uint8_t *)rgb_row;
    uint8_t *out_buf = (uint8_t *)out;
    uint32_t last_read = 0;
    for (uint16_t dy = 0; dy < target_h; dy++) {
        uint32_t src_y = (uint32_t)dy * src_h / target_h;
        if (src_y >= src_h) src_y = src_h - 1;
        /* 读到目标行为止 */
        while (last_read <= src_y) {
            jpeg_read_scanlines(&cinfo, &row_buf, 1);
            last_read++;
        }
        uint8_t *dst = out_buf + (uint32_t)dy * target_w * 2;
        for (uint16_t dx = 0; dx < target_w; dx++) {
            uint32_t src_x = (uint32_t)dx * src_w / target_w;
            if (src_x >= src_w) src_x = src_w - 1;
            uint8_t r = row_buf[src_x * 3 + 0];
            uint8_t g = row_buf[src_x * 3 + 1];
            uint8_t b = row_buf[src_x * 3 + 2];
            uint16_t c = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
            dst[dx * 2 + 0] = c & 0xFF;        /* low byte */
            dst[dx * 2 + 1] = (c >> 8) & 0xFF; /* high byte */
        }
    }
    syslog(LOG_INFO, "[camera_page] jpg sampled %u/%u rows, reading remaining\n",
           (unsigned)last_read, (unsigned)src_h);

    /* 关键：读完所有剩余行（采样只读了部分行），让 jpeg_finish_decompress
     * 不触发 longjmp。验证阶段崩溃根因是 finish 强制读剩余行时 NEON 报错 */
    while (cinfo.output_scanline < cinfo.output_height) {
        jpeg_read_scanlines(&cinfo, &row_buf, 1);
    }

    /* 正常 finish + destroy：无剩余行不会报错，destroy 释放内部 buffer 无泄漏 */
    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    syslog(LOG_INFO, "[camera_page] jpg decoded %s -> %ux%u RGB565 (finish+destroy ok)\n",
           path, target_w, target_h);
    free((void *)rgb_row);
    rgb_row = NULL;
    fclose(fp);
    return (uint8_t *)out;
}

static void cam_show_thumbnail_async_cb(void *data)
{
    cam_thumb_show_t *p = (cam_thumb_show_t *)data;
    if (!p) return;

    if (cam_thumb_buf && cam_thumb_img) {
        cam_thumb_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
        cam_thumb_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
        cam_thumb_dsc.header.flags = LV_IMAGE_FLAGS_MODIFIABLE;
        cam_thumb_dsc.header.w = p->w;
        cam_thumb_dsc.header.h = p->h;
        cam_thumb_dsc.header.stride = p->w * 2;
        cam_thumb_dsc.data_size = p->w * p->h * 2;
        cam_thumb_dsc.data = cam_thumb_buf;

        lv_image_set_src(cam_thumb_img, &cam_thumb_dsc);
        /* 缩放适配16:9矩形预览区域，保持图片比例 */
        lv_coord_t preview_w = lv_obj_get_width(cam_preview_img);
        lv_coord_t preview_h = lv_obj_get_height(cam_preview_img);
        uint16_t scale_x = (uint16_t)((uint32_t)preview_w * LV_SCALE_NONE / p->w);
        uint16_t scale_y = (uint16_t)((uint32_t)preview_h * LV_SCALE_NONE / p->h);
        uint16_t scale = (scale_x < scale_y) ? scale_x : scale_y;
        lv_image_set_scale(cam_thumb_img, scale);
        lv_obj_align(cam_thumb_img, LV_ALIGN_CENTER, 0, 0);
        lv_obj_clear_flag(cam_thumb_img, LV_OBJ_FLAG_HIDDEN);

        /* 拍照成功后隐藏占位图标 */
        if (cam_placeholder_icon) {
            lv_obj_add_flag(cam_placeholder_icon, LV_OBJ_FLAG_HIDDEN);
        }
        /* 隐藏状态文字（拍照中/失败提示） */
        if (cam_status_label) {
            lv_obj_add_flag(cam_status_label, LV_OBJ_FLAG_HIDDEN);
        }
        /* 图片就绪，取消倒计时 */
        cam_stop_countdown();

        cam_add_log("Thumbnail shown (%dx%d)", p->w, p->h);
    }

    free(p);
}

/* ── SPI RX thread ───────────────────────────────────────────── */
static void cam_spi_rx_thread_func(void *arg)
{
    (void)arg;
    uint8_t stream_buf[CAM_SPI_FRAME_SIZE * 12];
    int stream_len = 0;

    syslog(LOG_INFO, "[camera_page] SPI RX thread started\n");

    while (cam_spi_running) {
        if (!cam_spi_recv_active) {
            usleep(50000);
            continue;
        }

        /* 直接非阻塞 read：驱动 read() 内部调用 qpoll 把下半部 RX 数据搬到
         * 上半部再返回，绕开 poll/notify 单等待者机制（该机制在 reopen 期间
         * priv->fds 易残留致 poll 持续返回 0/-EBUSY、线程空转不 read，2N 端
         * 因收不到 FRAME_ACK 超时重传失败）。无数据时返回 -EAGAIN。 */
        ssize_t n = read(cam_spi_fd, stream_buf + stream_len,
                         sizeof(stream_buf) - stream_len);
        if (n <= 0) {
            usleep(1000);
            continue;
        }
        stream_len += n;

        /* Parse complete frames from stream */
        int pos = 0;
        while (stream_len - pos >= CAM_SPI_HDR_SIZE) {
            uint32_t magic = ((uint32_t)stream_buf[pos] << 24) |
                             ((uint32_t)stream_buf[pos + 1] << 16) |
                             ((uint32_t)stream_buf[pos + 2] << 8) |
                             (uint32_t)stream_buf[pos + 3];
            if (magic != CAM_SPI_MAGIC) {
                pos++;
                continue;
            }

            uint8_t type = stream_buf[pos + 4];
            uint32_t session = ((uint32_t)stream_buf[pos + 5] << 24) |
                               ((uint32_t)stream_buf[pos + 6] << 16) |
                               ((uint32_t)stream_buf[pos + 7] << 8) |
                               (uint32_t)stream_buf[pos + 8];
            uint32_t total = ((uint32_t)stream_buf[pos + 9] << 24) |
                             ((uint32_t)stream_buf[pos + 10] << 16) |
                             ((uint32_t)stream_buf[pos + 11] << 8) |
                             (uint32_t)stream_buf[pos + 12];
            uint32_t offset = ((uint32_t)stream_buf[pos + 13] << 24) |
                              ((uint32_t)stream_buf[pos + 14] << 16) |
                              ((uint32_t)stream_buf[pos + 15] << 8) |
                              (uint32_t)stream_buf[pos + 16];
            uint16_t len = ((uint16_t)stream_buf[pos + 17] << 8) |
                           stream_buf[pos + 18];

            uint32_t frame_total = CAM_SPI_HDR_SIZE + len + 2;
            if (stream_len - pos < frame_total)
                break;

            uint16_t crc_recv = ((uint16_t)stream_buf[pos + CAM_SPI_HDR_SIZE + len] << 8) |
                                stream_buf[pos + CAM_SPI_HDR_SIZE + len + 1];
            uint16_t crc_calc = cam_crc16_ccitt(stream_buf + pos,
                                                CAM_SPI_HDR_SIZE + len);

            if (crc_calc != crc_recv) {
                syslog(LOG_WARNING,
                       "[camera_page] SPI CRC error (off=%lu), skip\n", (unsigned long)offset);
                pos++;
                continue;
            }

            /* 收到有效帧，更新空闲超时时间戳（单变量赋值，无需 dispatch） */
            cam_spi_last_recv_tick = lv_tick_get();

            if (type == CAM_SPI_TYPE_BIG_DATA) {
                if (cam_big_fd >= 0) {
                    lseek(cam_big_fd, offset, SEEK_SET);
                    ssize_t wn = write(cam_big_fd,
                                       stream_buf + pos + CAM_SPI_HDR_SIZE, len);
                    if (wn == len) {
                        if (offset + len > cam_big_offset)
                            cam_big_offset = offset + len;
                        if (len > 0) {
                            uint16_t fidx = (uint16_t)(offset / CAM_SPI_MAX_PAYLOAD);
                            if (fidx < CAM_RECV_BITMAP_SIZE * 8) {
                                cam_recv_bitmap[fidx >> 3] |= (1 << (fidx & 7));
                            }
                            if (offset + len > cam_ack_next_offset)
                                cam_ack_next_offset = offset + len;
                            if (fidx + 1 > cam_recv_nframes)
                                cam_recv_nframes = fidx + 1;
                        }
                    } else {
                        syslog(LOG_ERR,
                               "[camera_page] big write failed: %d/%d errno=%d\n",
                               (int)wn, len, errno);
                    }
                } else {
                    syslog(LOG_ERR,
                           "[camera_page] big write skipped: fd closed\n");
                }
            }
            pos += frame_total;

            if (type == CAM_SPI_TYPE_BIG_END) {
                uint16_t expected_nframes = (uint16_t)((cam_big_total + CAM_SPI_MAX_PAYLOAD - 1) / CAM_SPI_MAX_PAYLOAD);
                if (expected_nframes > cam_recv_nframes)
                    cam_recv_nframes = expected_nframes;
                uint8_t big_ok = 0;
                if (cam_big_offset >= cam_big_total && cam_recv_nframes > 0) {
                    big_ok = 1;
                    for (uint16_t fi = 0; fi < cam_recv_nframes; fi++) {
                        if (!(cam_recv_bitmap[fi >> 3] & (1 << (fi & 7)))) {
                            big_ok = 0;
                            break;
                        }
                    }
                }
                uint16_t bmp_len = (cam_recv_nframes + 7) / 8;
                if (bmp_len > CAM_RECV_BITMAP_SIZE) bmp_len = CAM_RECV_BITMAP_SIZE;
                uint16_t ack_payload_len = 4 + 4 + 1 + 2 + bmp_len;
                uint8_t *ack_payload = malloc(ack_payload_len);
                if (ack_payload) {
                    ack_payload[0] = (session >> 24) & 0xFF;
                    ack_payload[1] = (session >> 16) & 0xFF;
                    ack_payload[2] = (session >> 8) & 0xFF;
                    ack_payload[3] = session & 0xFF;
                    ack_payload[4] = (cam_big_offset >> 24) & 0xFF;
                    ack_payload[5] = (cam_big_offset >> 16) & 0xFF;
                    ack_payload[6] = (cam_big_offset >> 8) & 0xFF;
                    ack_payload[7] = cam_big_offset & 0xFF;
                    ack_payload[8] = big_ok;
                    ack_payload[9] = (cam_recv_nframes >> 8) & 0xFF;
                    ack_payload[10] = cam_recv_nframes & 0xFF;
                    memcpy(ack_payload + 11, cam_recv_bitmap, bmp_len);
                    cam_uart_send_frame(CAM_CMD_FRAME_ACK, ack_payload, ack_payload_len);
                    free(ack_payload);
                }
                syslog(LOG_INFO,
                       "[camera_page] big image complete (%lu/%lu bytes, ok=%u, nframes=%u)\n",
                       (unsigned long)cam_big_offset, (unsigned long)cam_big_total, big_ok, cam_recv_nframes);
                if (big_ok) {
                    cam_spi_recv_active = false;
                    cam_stop_spi_timeout();
                    if (cam_big_fd >= 0) {
                        close(cam_big_fd);
                        cam_big_fd = -1;
                    }
                    cam_add_log("Saved: %s", cam_big_filename);
                    /* === [方案B] 本地解码 JPG → RGB565 显示 === */
                    if (cam_thumb_buf) { free(cam_thumb_buf); cam_thumb_buf = NULL; }
                    cam_thumb_w = 280;
                    cam_thumb_h = 158;
                    cam_thumb_buf = cam_decode_jpg_to_rgb565(
                        cam_big_filename, cam_thumb_w, cam_thumb_h);
                    if (cam_thumb_buf) {
                        /* 解码成功：先播放快门提示音，再显示缩略图。
                         * wakeup_detector_request_prompt 非阻塞（VAD 未运行时由
                         * detached 线程同步播放 do_play_prompt），此处 SPI 接收已
                         * 完成（cam_spi_recv_active=false、超时已停），短暂 usleep
                         * 让提示音先发声再刷新缩略图，模拟真实相机快门体验。
                         * do_play_prompt 内部有 ~200ms 音量传播+prepare 启动延迟，
                         * 300ms 足以保证声音先于画面出现。 */
                        wakeup_detector_request_prompt(CAM_SHUTTER_WAV);
                        usleep(300000);

                        /* 显示 + 启用 AI */
                        cam_thumb_show_t *p = malloc(sizeof(cam_thumb_show_t));
                        if (p) {
                            p->w = cam_thumb_w;
                            p->h = cam_thumb_h;
                            if (!lvgl_dispatch_async(cam_show_thumbnail_async_cb, p)) {
                                free(p);
                            }
                        }
                        cam_has_photo = true;
                        cam_set_ai_btn_enabled_async(true);
                        cam_show_status_async(NULL, false, false);
                        cam_busy = false;
                        cam_set_buttons_enabled(true);
                    } else {
                        /* 解码失败：UI 提示“拍照失败，请重试”3s，AI 禁用，日志记真实原因 */
                        syslog(LOG_ERR, "[camera_page] jpg decode failed: %s\n", cam_big_filename);
                        cam_handle_capture_failure_async(2);
                    }
                } else {
                    cam_ack_next_offset = 0;
                    syslog(LOG_INFO, "[camera_page] big NACK, keep recv for retrans\n");
                }
            }
        }

        /* Shift remaining bytes to front */
        if (pos > 0 && stream_len > 0) {
            int remaining = stream_len - pos;
            if (remaining > 0) {
                memmove(stream_buf, stream_buf + pos, remaining);
                stream_len = remaining;
            } else {
                stream_len = 0;
            }
        }
    }

    syslog(LOG_INFO, "[camera_page] SPI RX thread exiting\n");
}

static void cam_uart_rx_thread_func(void *arg)
{
    (void)arg;
    uint8_t rxb;
    enum {
        S_SYNC0, S_SYNC1, S_LEN0, S_LEN1,
        S_CMD, S_SEQ, S_PAYLOAD, S_CRC0, S_CRC1
    } state = S_SYNC0;
    uint16_t frame_len = 0;
    uint8_t cmd = 0;
    uint8_t seq = 0;
    uint16_t payload_idx = 0;
    uint8_t payload_buf[1080];
    uint16_t crc_recv = 0;

    syslog(LOG_INFO, "[camera_page] UART RX thread started\n");

    while (cam_uart_running) {
        ssize_t n = read(cam_uart_fd, &rxb, 1);
        if (n <= 0) {
            if (!cam_uart_running) break;
            usleep(1000);
            continue;
        }

        switch (state) {
        case S_SYNC0:
            if (rxb == 0xAA) {
                syslog(LOG_INFO, "[camera_page] RX: sync 0xAA\n");
                state = S_SYNC1;
            }
            break;
        case S_SYNC1:
            if (rxb == 0x55) state = S_LEN0;
            else state = S_SYNC0;
            break;
        case S_LEN0:
            frame_len = (uint16_t)rxb << 8;
            state = S_LEN1;
            break;
        case S_LEN1:
            frame_len |= rxb;
            if (frame_len < 2 || frame_len > 1078) {
                state = S_SYNC0;
                break;
            }
            state = S_CMD;
            break;
        case S_CMD:
            cmd = rxb;
            state = S_SEQ;
            break;
        case S_SEQ:
            seq = rxb;
            payload_idx = 0;
            if (frame_len > 2) state = S_PAYLOAD;
            else state = S_CRC0;
            break;
        case S_PAYLOAD:
            if (payload_idx < sizeof(payload_buf))
                payload_buf[payload_idx++] = rxb;
            if (payload_idx >= frame_len - 2)
                state = S_CRC0;
            break;
        case S_CRC0:
            crc_recv = (uint16_t)rxb << 8;
            state = S_CRC1;
            break;
        case S_CRC1:
            crc_recv |= rxb;
            {
                uint8_t crc_buf[1080];
                crc_buf[0] = (frame_len >> 8) & 0xFF;
                crc_buf[1] = frame_len & 0xFF;
                crc_buf[2] = cmd;
                crc_buf[3] = seq;
                memcpy(crc_buf + 4, payload_buf, frame_len - 2);
                uint16_t crc_calc = cam_crc16_ccitt(crc_buf, 4 + frame_len - 2);
                if (crc_calc != crc_recv) {
                    syslog(LOG_WARNING,
                           "[camera_page] UART CRC error (calc=0x%04X recv=0x%04X)\n",
                           crc_calc, crc_recv);
                    state = S_SYNC0;
                    break;
                }
            }

            syslog(LOG_INFO, "[camera_page] UART RX: cmd=0x%02X seq=%d len=%d\n",
                   cmd, seq, frame_len - 2);

            if (cmd == CAM_CMD_CAPTURE_ACK) {
                /* 守卫：超时已触发失败后到达的迟到 ACK，忽略以防重新激活 SPI 流水线
                 * （否则会出现“已提示拍照失败却仍显示缩略图”的矛盾现象） */
                if (!cam_busy) {
                    cam_add_log("Late CAPTURE_ACK ignored");
                    state = S_SYNC0;
                    break;
                }
                char filename[33] = {0};
                if (frame_len - 2 >= 32) {
                    memcpy(filename, payload_buf, 32);
                    filename[32] = '\0';
                }
                cam_add_log("Capture done! %s", filename);
                cam_stop_timeout();

                /* [方案B] 跳过缩略图传输，直接请求大图 JPG */
                cam_add_log("Requesting image...");
                tcflush(cam_uart_fd, TCIFLUSH);
                if (cam_uart_send_frame(CAM_CMD_GET_IMAGE_REQ, NULL, 0) < 0) {
                    syslog(LOG_ERR, "[camera_page] GET_IMAGE_REQ send fail errno=%d\n", errno);
                    cam_add_log("Send GET_IMAGE_REQ failed");
                    cam_handle_capture_failure_async(1);
                } else {
                    cam_start_timeout(10000);  /* UART 10s 等 GET_IMAGE_ACK */
                }
            } else if (cmd == CAM_CMD_CAPTURE_FAIL) {
                cam_add_log("Capture failed: err=%d", payload_idx > 0 ? payload_buf[0] : -1);
                cam_stop_timeout();
                cam_handle_capture_failure_async(0);
            } else if (cmd == CAM_CMD_GET_IMAGE_ACK) {
                /* 守卫：超时已触发失败后到达的迟到 ACK，忽略以防重新激活大图接收 */
                if (!cam_busy) {
                    cam_add_log("Late GET_IMAGE_ACK ignored");
                    state = S_SYNC0;
                    break;
                }
                char filename[33] = {0};
                uint32_t file_size = 0;
                if (frame_len - 2 >= 36) {
                    memcpy(filename, payload_buf, 32);
                    filename[32] = '\0';
                    file_size = ((uint32_t)payload_buf[32] << 24) |
                                ((uint32_t)payload_buf[33] << 16) |
                                ((uint32_t)payload_buf[34] << 8) |
                                (uint32_t)payload_buf[35];
                }
                cam_add_log("Image: %s (%u bytes)", filename, file_size);
                cam_stop_timeout();

                if (file_size > 0) {
                    cam_big_session++;
                    snprintf(cam_big_filename, sizeof(cam_big_filename),
                             "/emmc/camera/%s", filename);
                    cam_big_fd = open(cam_big_filename,
                                      O_WRONLY | O_CREAT | O_TRUNC, 0644);
                    if (cam_big_fd < 0) {
                        syslog(LOG_ERR,
                               "[camera_page] open %s failed: %d\n",
                               cam_big_filename, errno);
                        cam_add_log("Save failed!");
                        cam_handle_capture_failure_async(1);
                    } else {
                        cam_big_total = file_size;
                        cam_big_offset = 0;
                        cam_ack_next_offset = 0;
                        cam_recv_nframes = 0;
                        memset(cam_recv_bitmap, 0, sizeof(cam_recv_bitmap));

                        uint8_t spi_ready[4];
                        spi_ready[0] = (cam_big_session >> 24) & 0xFF;
                        spi_ready[1] = (cam_big_session >> 16) & 0xFF;
                        spi_ready[2] = (cam_big_session >> 8) & 0xFF;
                        spi_ready[3] = cam_big_session & 0xFF;
                        cam_uart_send_frame(CAM_CMD_SPI_READY_BIG, spi_ready, 4);

                        cam_spi_recv_active = true;
                        cam_spi_is_big_image = true;  /* 修正：大图阶段标志 */
                        cam_add_log("Receiving...");
                        cam_start_spi_timeout();
                    }
                } else {
                    cam_add_log("No image file");
                    cam_handle_capture_failure_async(1);
                }
            }

            state = S_SYNC0;
            break;
        }
    }

    syslog(LOG_INFO, "[camera_page] UART RX thread exiting\n");
}

static void cam_capture_btn_cb(lv_event_t *e)
{
    (void)e;
    if (cam_busy) return;

    cam_busy = true;
    cam_set_buttons_enabled(false);
    cam_set_ai_btn_enabled_async(false);  /* 重拍时 AI 立即 disabled */

    /* 若存在旧 AI 分析聊天页，销毁之，回到 B 状态 */
    if (cam_ai_chat_active) {
        destroy_camera_ai_page();
        cam_ai_chat_active = false;
    }

    /* 清理上一次拍照残留：释放旧缩略图缓冲、隐藏旧缩略图、恢复占位图标 */
    if (cam_thumb_buf) {
        free(cam_thumb_buf);
        cam_thumb_buf = NULL;
    }
    cam_thumb_offset = 0;
    cam_thumb_total = 0;
    if (cam_thumb_img) lv_obj_add_flag(cam_thumb_img, LV_OBJ_FLAG_HIDDEN);
    if (cam_placeholder_icon) lv_obj_clear_flag(cam_placeholder_icon, LV_OBJ_FLAG_HIDDEN);
    cam_big_filename[0] = '\0';

    /* 预览区下方显示"请保持平稳..." */
    cam_show_status_async(i18n_get(STR_CAM_KEEP_STEADY), true, false);
    cam_add_log("%s", i18n_get(STR_CAM_KEEP_STEADY));
    /* 预览区中央启动 12s 倒计时，覆盖对焦+拍照+传输全流程 */
    cam_start_countdown();

    tcflush(cam_uart_fd, TCIFLUSH);

    if (cam_uart_send_frame(CAM_CMD_CAPTURE_REQ, NULL, 0) < 0) {
        cam_add_log("Send CAPTURE_REQ failed");
        cam_handle_capture_failure_async(0);
    } else {
        cam_add_log("CAPTURE_REQ sent, wait ACK...");
        cam_start_timeout(30000);  /* 2N 对焦+拍照+生成缩略图，对焦时间不确定，留 30s */
    }
}

/* ── 飞书发送结果回调（LVGL 线程执行） ───────────────────────── */

static void cam_feishu_result_cb(void *data)
{
    const char *result = (const char *)data;
    cam_add_log("[FEISHU] %s", result);
    free((void *)result);
}

/* 飞书上传完成后回调（LVGL 线程执行）：
 * - success=true 且 start_ai_after：启动 AI 分析
 * - success=false 或非 AI 模式：恢复按钮 */
static void cam_feishu_done_cb(void *data)
{
    intptr_t success = (intptr_t)data;
    if (success) {
        cam_add_log("[AI_IMG] Feishu done, starting AI analysis...");
        /* 开启飞书文本转发：复用图片发送时获取的 receive_id/id_type，
         * 转发 3 轮 AI 回复（图片分析 + 2 轮对话）后自动关闭。
         * 下次点击 AI 分析按钮会再次调用以重置计数。 */
        if (s_feishu_receive_id[0] != '\0' && s_feishu_id_type) {
            voice_assistant_set_feishu_forward(s_feishu_receive_id,
                                               s_feishu_id_type, true);
        }
        int ret = voice_assistant_analyze_image(cam_big_filename);
        if (ret != 0) {
            cam_add_log("[AI_IMG] AI analyze failed: %d", ret);
            cam_busy = false;
            cam_set_buttons_enabled(true);
        }
    } else {
        cam_add_log("[AI_IMG] Feishu upload failed, abort AI");
        cam_busy = false;
        cam_set_buttons_enabled(true);
    }
}

/* ── 飞书发送后台线程 ───────────────────────────────────────── */

static void *cam_feishu_thread(void *arg)
{
    cam_feishu_arg_t *fa = (cam_feishu_arg_t *)arg;
    char receive_id[64] = {0};
    const char *id_type = "chat_id";
    char log_buf[CAM_LOG_MAX_LEN];

    syslog(LOG_INFO, "[FEISHU] ===== thread start =====\n");
    syslog(LOG_INFO, "[FEISHU] image_path=%s\n", fa->image_path);
    syslog(LOG_INFO, "[FEISHU] target_type=%d (0=chat,1=user)\n",
        (int)AGENT_SECRET_FEISHU_TARGET_TYPE);

    /* Step 0: 检查飞书是否已配置 */
    const char *app_id = feishu_get_app_id();
    if (!app_id || app_id[0] == '\0') {
        syslog(LOG_ERR, "[FEISHU] App not configured (app_id empty), skip\n");
        cam_add_log("[FEISHU] App not configured, skip");
        if (fa->start_ai_after)
            lvgl_dispatch_async(cam_feishu_done_cb, (void *)(intptr_t)0);
        free(fa);
        return NULL;
    }
    syslog(LOG_INFO, "[FEISHU] app_id=%s\n", app_id);

    /* Step 1: 根据配置搜索目标（群聊 chat_id 或 用户 open_id） */
#if AGENT_SECRET_FEISHU_TARGET_TYPE == 1
    /* 模式1：通过手机号获取用户 open_id */
    cam_add_log("[FEISHU] Getting user open_id via mobile: " AGENT_SECRET_FEISHU_TARGET_MOBILE);
    syslog(LOG_INFO, "[FEISHU] step 1: get_user_open_id mobile=%s\n",
        AGENT_SECRET_FEISHU_TARGET_MOBILE);

    if (feishu_get_user_open_id(AGENT_SECRET_FEISHU_TARGET_MOBILE,
                                 receive_id, sizeof(receive_id)) != 0) {
        syslog(LOG_ERR, "[FEISHU] step 1 FAILED: user not found\n");
        snprintf(log_buf, sizeof(log_buf),
            "User not found: " AGENT_SECRET_FEISHU_TARGET_MOBILE);
        lvgl_dispatch_async(cam_feishu_result_cb, strdup(log_buf));
        if (fa->start_ai_after)
            lvgl_dispatch_async(cam_feishu_done_cb, (void *)(intptr_t)0);
        free(fa);
        return NULL;
    }
    id_type = "open_id";
    syslog(LOG_INFO, "[FEISHU] step 1 OK: open_id=%s\n", receive_id);
    snprintf(log_buf, sizeof(log_buf), "Found user open_id: %s", receive_id);
#else
    /* 模式0（默认）：通过群名搜索 chat_id */
    cam_add_log("[FEISHU] Searching chat: " AGENT_SECRET_FEISHU_TARGET_CHAT);
    syslog(LOG_INFO, "[FEISHU] step 1: search_chats \"%s\"\n",
        AGENT_SECRET_FEISHU_TARGET_CHAT);

    if (feishu_search_chats(AGENT_SECRET_FEISHU_TARGET_CHAT,
                             receive_id, sizeof(receive_id)) != 0) {
        syslog(LOG_ERR, "[FEISHU] step 1 FAILED: chat not found\n");
        snprintf(log_buf, sizeof(log_buf),
            "Chat not found: " AGENT_SECRET_FEISHU_TARGET_CHAT);
        lvgl_dispatch_async(cam_feishu_result_cb, strdup(log_buf));
        if (fa->start_ai_after)
            lvgl_dispatch_async(cam_feishu_done_cb, (void *)(intptr_t)0);
        free(fa);
        return NULL;
    }
    id_type = "chat_id";
    syslog(LOG_INFO, "[FEISHU] step 1 OK: chat_id=%s\n", receive_id);
    snprintf(log_buf, sizeof(log_buf), "Found chat: %s", receive_id);
#endif
    cam_add_log("%s", log_buf);

    /* 存储 receive_id/id_type（Phase 2 预留：发送 AI 分析结果文本） */
    strncpy(s_feishu_receive_id, receive_id, sizeof(s_feishu_receive_id) - 1);
    s_feishu_id_type = id_type;

    /* Step 2: 上传图片 + 发送 */
    cam_add_log("[FEISHU] Uploading & sending image...");
    syslog(LOG_INFO, "[FEISHU] step 2: upload + send image (id_type=%s)\n", id_type);

    /* 先上传图片获取 image_key */
    char image_key[128] = {0};
    if (feishu_upload_image(fa->image_path, image_key, sizeof(image_key)) != 0) {
        syslog(LOG_ERR, "[FEISHU] step 2 upload FAILED\n");
        lvgl_dispatch_async(cam_feishu_result_cb, strdup("Upload image FAILED"));
        if (fa->start_ai_after)
            lvgl_dispatch_async(cam_feishu_done_cb, (void *)(intptr_t)0);
        free(fa);
        return NULL;
    }
    syslog(LOG_INFO, "[FEISHU] upload OK: image_key=%s\n", image_key);

    /* 发送图片消息（用对应 id_type） */
    if (feishu_send_image_message_ex(receive_id, image_key, id_type) != 0) {
        syslog(LOG_ERR, "[FEISHU] step 2 send FAILED\n");
        lvgl_dispatch_async(cam_feishu_result_cb,
            strdup("Send image FAILED"));
        if (fa->start_ai_after)
            lvgl_dispatch_async(cam_feishu_done_cb, (void *)(intptr_t)0);
        free(fa);
        return NULL;
    }

    syslog(LOG_INFO, "[FEISHU] step 2 OK: image sent successfully\n");
    lvgl_dispatch_async(cam_feishu_result_cb,
        strdup("Image sent to Feishu OK"));
    if (fa->start_ai_after)
        lvgl_dispatch_async(cam_feishu_done_cb, (void *)(intptr_t)1);

    syslog(LOG_INFO, "[FEISHU] ===== thread done =====\n");
    free(fa);
    return NULL;
}

/* ── 启动飞书发送线程 ───────────────────────────────────────── */

static void cam_send_to_feishu_async(const char *image_path, bool start_ai_after)
{
    cam_feishu_arg_t *fa = malloc(sizeof(cam_feishu_arg_t));
    if (!fa) {
        syslog(LOG_ERR, "[FEISHU] OOM for thread arg\n");
        return;
    }
    strncpy(fa->image_path, image_path, sizeof(fa->image_path) - 1);
    fa->image_path[sizeof(fa->image_path) - 1] = '\0';
    fa->start_ai_after = start_ai_after;

    pthread_t tid;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    /* 飞书HTTPS上传需要较大栈（TLS握手+multipart+fread+cJSON+syslog），给128KB */
    pthread_attr_setstacksize(&attr, 128 * 1024);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    int ret = pthread_create(&tid, &attr, cam_feishu_thread, fa);
    pthread_attr_destroy(&attr);

    if (ret != 0) {
        syslog(LOG_ERR, "[FEISHU] pthread_create failed: %d\n", ret);
        free(fa);
    } else {
        syslog(LOG_INFO, "[FEISHU] thread created, tid=%lu\n",
            (unsigned long)tid);
    }
}

/* 图片AI分析状态回调（通过lvgl_dispatch在LVGL线程执行） */
static void cam_image_status_cb(const char *status, bool done)
{
    if (status && status[0]) {
        cam_add_log("[AI_IMG] %s", status);
    }
    if (done) {
        cam_busy = false;
        cam_set_buttons_enabled(true);
    }
}

static void cam_ai_btn_cb(lv_event_t *e);  /* 前置声明，供左滑复用 */
static void cam_ai_btn_cb(lv_event_t *e)
{
    (void)e;
    syslog(LOG_INFO, "[camera_page] AI btn clicked: busy=%d chat_active=%d has_file=%d\n",
           (int)cam_busy, (int)cam_ai_chat_active, (int)(cam_big_filename[0] != '\0'));
    if (cam_busy) {
        syslog(LOG_INFO, "[camera_page] AI btn ignored: cam_busy\n");
        return;
    }

    /* C状态（AI 聊天页已存在）：按钮已 ai_off，点击不响应。
     * 用户查看 AI 聊天请通过左滑切换。 */
    if (cam_ai_chat_active) {
        syslog(LOG_INFO, "[camera_page] AI btn ignored: chat already active\n");
        return;
    }

    if (cam_big_filename[0] == '\0') {
        /* 未拍照时在预览区下方提示，与“拍照中”同样位置，3s 后自动消失 */
        cam_show_status_async(i18n_get(STR_CAM_PLEASE_CAPTURE), true, false);
        cam_start_fail_hint_async();
        return;
    }

    struct stat st;
    if (stat(cam_big_filename, &st) != 0 || st.st_size == 0) {
        cam_add_log("[AI_IMG] Image not found: %s", cam_big_filename);
        return;
    }

    syslog(LOG_INFO, "[camera_page] AI btn: image=%s size=%ld\n",
        cam_big_filename, (long)st.st_size);

    if (!voice_assistant_is_running()) {
        syslog(LOG_WARNING, "[camera_page] AI btn: voice_assistant NOT running, starting now...\n");
        cam_add_log("[AI_IMG] Voice assistant starting, please retry...");
        cam_show_status_async(i18n_get(STR_CAM_AI_STARTING), true, false);
        cam_start_fail_hint_async();
        int sr = voice_assistant_start();
        syslog(LOG_INFO, "[camera_page] AI btn: voice_assistant_start() ret=%d running=%d\n",
               sr, (int)voice_assistant_is_running());
        return;
    }
    if (!voice_assistant_is_connected()) {
        syslog(LOG_WARNING, "[camera_page] AI btn abort: voice_assistant NOT connected (asr_stream NULL)\n");
        cam_add_log("[AI_IMG] Voice assistant connecting...");
        cam_show_status_async(i18n_get(STR_CAM_AI_CONNECTING), true, false);
        cam_start_fail_hint_async();
        return;
    }
    if (voice_assistant_is_image_busy()) {
        /* img_ctx（图片分析状态机）是 camera_page 专属，ai_page 不使用 analyze_image/img_ctx，
         * 所以这里 reset 不会影响 ai_page 的正常语音对话。走到这里时 cam_ai_chat_active 必为
         * false（函数开头已 return），img_ctx busy 必定是上次分析残留卡死（如服务端未回
         * response.done 导致 state 卡在 WAITING），直接强制复位并提示用户重试。 */
        syslog(LOG_WARNING, "[camera_page] AI btn abort: image busy, force resetting (va_down=%d)\n",
               (int)(!voice_assistant_is_running() || !voice_assistant_is_connected()));
        cam_add_log("[AI_IMG] Image state stuck, resetting");
        voice_assistant_reset_image_state();
        cam_show_status_async(i18n_get(STR_CAM_AI_STARTING), true, false);
        cam_start_fail_hint_async();
        return;
    }

    syslog(LOG_INFO, "[camera_page] AI btn proceed: starting analyze_image\n");
    cam_busy = true;
    cam_set_buttons_enabled(false);
    cam_set_ai_btn_enabled_async(false);  /* 分析期间禁用 AI 按钮 */

    /* AI 分析立即启动，不等待任何飞书操作。
     * 飞书聊天上传仅在无活跃文档时并行执行（有文档时文档写入线程自己上传图片）。 */
    cam_add_log("[AI_IMG] AI analyzing: %s", cam_big_filename);
    int ret = voice_assistant_analyze_image(cam_big_filename);
    if (ret != 0) {
        cam_add_log("[AI_IMG] AI analyze failed: %d", ret);
        cam_busy = false;
        cam_set_buttons_enabled(true);
        cam_set_ai_btn_enabled_async(true);  /* 恢复 AI 按钮 */
        return;
    }

    /* 无活跃飞书文档时：并行上传图片到飞书聊天，AI 回复时转发文本到聊天
     * 有活跃文档时：关闭聊天转发（清除可能残留的 s_fwd_enabled），跳过聊天上传 */
    const char *app_id = feishu_get_app_id();
    if (app_id && app_id[0] != '\0') {
        if (voice_assistant_has_active_feishu_doc()) {
            voice_assistant_disable_feishu_forward();
            cam_add_log("[AI_IMG] Active doc exists, chat forward disabled");
        } else {
            cam_add_log("[AI_IMG] No active doc, uploading to Feishu chat in parallel...");
            cam_send_to_feishu_async(cam_big_filename, true);
        }
    }

    /* 立即跳转到AI分析聊天页面，并标记 C 状态（聊天页持久） */
    lv_obj_t *ai_screen = create_camera_ai_page(cam_big_filename);
    lv_scr_load_anim(ai_screen, LV_SCR_LOAD_ANIM_MOVE_LEFT, 300, 0, false);
    cam_ai_chat_active = true;
}

/* 每次（再次）进入 camera 页面时重置拍照状态：按钮文案、AI 可用性、占位图标等 */
/* 从 AI 分析界面返回时调用：标记保留缩略图，仅恢复按钮状态 */
void camera_page_notify_return_from_ai(void)
{
    cam_busy = false;
    cam_return_from_ai = true;
}

static void cam_screen_load_cb(lv_event_t *e)
{
    (void)e;
    if (cam_busy) return;  /* 拍照进行中不重置 */

    if (cam_return_from_ai) {
        /* 从 AI 分析界面返回：保留缩略图与 cam_has_photo，仅恢复按钮与提示状态 */
        cam_return_from_ai = false;
        if (cam_fail_hint_timer) {
            lv_timer_del(cam_fail_hint_timer);
            cam_fail_hint_timer = NULL;
        }
        if (cam_status_label) lv_obj_add_flag(cam_status_label, LV_OBJ_FLAG_HIDDEN);
        /* 拍照按钮恢复可用（photo_on） */
        if (cam_capture_btn) {
            lv_obj_set_style_img_opa(cam_capture_btn, LV_OPA_COVER, 0);
            lv_image_set_src(cam_capture_btn, &camera_page_photo_on);
        }
        /* AI 按钮按 cam_has_photo && !cam_ai_chat_active 决定：
         * C状态（AI聊天活跃）保持 ai_off 灰；否则有照片则 ai_on */
        if (cam_ai_btn) {
            if (cam_has_photo && !cam_ai_chat_active) {
                lv_obj_set_style_img_opa(cam_ai_btn, LV_OPA_COVER, 0);
                lv_image_set_src(cam_ai_btn, &camera_page_ai_on);
            } else {
                lv_obj_set_style_img_opa(cam_ai_btn, LV_OPA_50, 0);
                lv_image_set_src(cam_ai_btn, &camera_page_ai_off);
            }
        }
        return;
    }

    /* 从 menu 进入：完整重置状态。
     * 同时 suspend 语音监听（关闭本地 VAD + 云端 mic），避免拍照期间
     * mic 录音/唤醒词触发对话对拍照产生干扰。须在销毁 AI 页前调用。 */
    /* 进入 camera 前确保云端线程已启动：用户可能从 menu 直接进入（未经过 ai_page），
     * 或之前进过 recorder/meeting 页被 stop。提前 start 让 WebSocket 在拍照期间
     * 预先建立，用户按 AI 按钮时 is_connected 已为 true。start 是幂等的。 */
    if (!voice_assistant_is_running()) {
        syslog(LOG_INFO, "[camera_page] enter: voice_assistant not running, starting...\n");
        int sr = voice_assistant_start();
        syslog(LOG_INFO, "[camera_page] enter: voice_assistant_start() ret=%d\n", sr);
    }
    voice_assistant_suspend_listening();
    if (cam_ai_chat_active) {
        destroy_camera_ai_page();
        cam_ai_chat_active = false;
    }
    if (cam_fail_hint_timer) {
        lv_timer_del(cam_fail_hint_timer);
        cam_fail_hint_timer = NULL;
    }
    if (cam_countdown_timer) {
        lv_timer_del(cam_countdown_timer);
        cam_countdown_timer = NULL;
    }
    if (cam_thumb_buf) {
        free(cam_thumb_buf);
        cam_thumb_buf = NULL;
    }
    cam_thumb_offset = 0;
    cam_thumb_total = 0;
    cam_big_filename[0] = '\0';

    if (cam_thumb_img) lv_obj_add_flag(cam_thumb_img, LV_OBJ_FLAG_HIDDEN);
    if (cam_placeholder_icon) lv_obj_clear_flag(cam_placeholder_icon, LV_OBJ_FLAG_HIDDEN);
    if (cam_status_label) lv_obj_add_flag(cam_status_label, LV_OBJ_FLAG_HIDDEN);
    if (cam_countdown_label) lv_obj_add_flag(cam_countdown_label, LV_OBJ_FLAG_HIDDEN);

    cam_has_photo = false;
    if (cam_capture_btn) {
        lv_obj_set_style_img_opa(cam_capture_btn, LV_OPA_COVER, 0);
        lv_image_set_src(cam_capture_btn, &camera_page_photo_on);
    }
    if (cam_ai_btn) {
        lv_obj_set_style_img_opa(cam_ai_btn, LV_OPA_50, 0);
        lv_image_set_src(cam_ai_btn, &camera_page_ai_off);
    }

    /* 进入 camera 页（从 menu 进入）：异步创建飞书文档用于本会话图片+AI回复追加。
     * 后台线程执行，不阻塞 UI/拍照；用户无感。
     * camera ↔ camera_ai 之间切换不触发本回调的"从 menu 进入"分支，文档保留。
     * 右滑回 menu / deinit 时调 camera_feishu_doc_cancel() 清空。 */
    camera_feishu_doc_create_async();
}

static void cam_gesture_cb(lv_event_t *e)
{
    (void)e;
    if (cam_thumb_fullscreen) {
        lv_indev_t *indev = lv_indev_get_act();
        if (indev) lv_indev_wait_release(indev);
        return;
    }
    lv_indev_t *indev = lv_indev_get_act();
    if (!indev) return;
    lv_dir_t dir = lv_indev_get_gesture_dir(indev);
    /* 右滑返回功能菜单页 */
    if (dir == LV_DIR_RIGHT) {
        /* AI回复播放期间禁止右滑退出camera，只允许在camera和camera_ai之间滑动。
         * 复用 ai_page 右滑退出保护：voice_assistant_is_exit_blocked() 覆盖
         * 云端响应活跃(s_response_active) + omni音频drain(s_omni_echo_active)
         * 整个窗口，camera_ai 页的图片分析回复与多轮对话回复均走同一云端通路。
         * cam_ai_chat_active 守卫确保仅在 camera_ai 页面活跃时拦截，左滑进
         * camera_ai 与 camera_ai 右滑回 camera 不受影响。 */
        if (cam_ai_chat_active && voice_assistant_is_exit_blocked()) {
            return;
        }
        /* 退出 camera 回菜单：销毁可能存在的 AI 聊天页（关闭云端 mic）。
         * 不调用 resume_listening——保持 s_listening_suspended = true：
         * exit_dialogue 是异步的，dashscope 线程延迟关闭云端 mic 时会调
         * va_maybe_start_wakeup()，若此时标志已被清掉，VAD 会被重启，
         * 导致退出 camera 后仍能唤醒小Q。保持标志为 true 使该重启被跳过，
         * 本地 VAD 维持关闭。云端 mic 与本地 VAD 均关闭，
         * 由其他界面按需自行开启 mic。 */
        /* 等待手指释放后再处理，防止动画期间手指抬起的 CLICKED 事件
         * 误触发菜单页按钮。比 lv_indev_reset 副作用小，不会干扰屏幕加载动画。 */
        lv_indev_wait_release(indev);
        if (cam_ai_chat_active) {
            destroy_camera_ai_page();
            cam_ai_chat_active = false;
        }
        /* 退出 camera 回 menu：取消建文档线程 + 清空活跃飞书文档。
         * 下次进入 camera 会重新建新文档，不复用本次会话的文档。 */
        camera_feishu_doc_cancel();
        if (menu_screen == NULL) {
            menu_screen = lv_obj_create(NULL);
            create_menu_page(menu_screen);
        }
        lv_scr_load_anim(menu_screen, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 300, 0, false);
    } else if (dir == LV_DIR_LEFT) {
        /* C状态（AI聊天页已存在）：只切屏，不重建，聊天记录保留 */
        if (cam_ai_chat_active) {
            lv_obj_t *ai = create_camera_ai_page(NULL);  /* 内部 guard：active则返回现有screen */
            lv_scr_load_anim(ai, LV_SCR_LOAD_ANIM_MOVE_LEFT, 300, 0, false);
        } else if (!cam_has_photo) {
            /* A状态（未拍照）：提示请先拍照 */
            cam_ai_btn_cb(NULL);
        }
        /* B状态（已拍照但无AI聊天）：静默不响应 */
    }
}

static void cam_preview_pressed_cb(lv_event_t *e)
{
    (void)e;
    cam_thumb_dragged = false;
}

static void cam_preview_pressing_cb(lv_event_t *e)
{
    (void)e;
    if (!cam_thumb_fullscreen || !cam_thumb_img) return;

    lv_indev_t *in = lv_indev_get_act();
    if (!in) return;
    lv_point_t vect;
    lv_indev_get_vect(in, &vect);
    if (vect.x || vect.y) cam_thumb_dragged = true;
    cam_thumb_off_x += vect.x;
    cam_thumb_off_y += vect.y;
    lv_obj_align(cam_thumb_img, LV_ALIGN_CENTER, cam_thumb_off_x, cam_thumb_off_y);
}

static void cam_preview_click_cb(lv_event_t *e)
{
    (void)e;
    if (!cam_thumb_img || cam_thumb_w == 0 || cam_thumb_h == 0) {
        return;
    }
    if (cam_thumb_dragged) {
        cam_thumb_dragged = false;
        return;
    }

    lv_coord_t screen_w = lv_display_get_horizontal_resolution(NULL);
    lv_coord_t screen_h = lv_display_get_vertical_resolution(NULL);

    if (!cam_thumb_fullscreen) {
        cam_thumb_fullscreen = true;

        lv_obj_set_size(cam_preview_img, screen_w, screen_h);
        lv_obj_align(cam_preview_img, LV_ALIGN_CENTER, 0, 0);
        lv_obj_set_style_radius(cam_preview_img, 0, 0);
        lv_obj_move_foreground(cam_preview_img);

        uint16_t scale_x = (uint16_t)((uint32_t)screen_w * LV_SCALE_NONE / cam_thumb_w);
        uint16_t scale_y = (uint16_t)((uint32_t)screen_h * LV_SCALE_NONE / cam_thumb_h);
        uint16_t scale = (scale_x < scale_y) ? scale_x : scale_y;
        lv_image_set_scale(cam_thumb_img, scale);

        cam_thumb_off_x = 0;
        cam_thumb_off_y = 0;
        lv_obj_align(cam_thumb_img, LV_ALIGN_CENTER, 0, 0);
    } else {
        cam_thumb_fullscreen = false;

        /* 返回正常16:9矩形居中布局 */
        lv_coord_t top_reserved = CAM_TITLE_MARGIN_TOP + 40;
        lv_coord_t bot_reserved = CAM_BTN_AREA_HEIGHT + CAM_BTN_AREA_MARGIN_BOT;
        lv_coord_t available_w = screen_w - 80;
        lv_coord_t available_h = screen_h - top_reserved - bot_reserved;
        
        /* 重新计算16:9预览尺寸 */
        lv_coord_t preview_w = available_w;
        lv_coord_t preview_h = (preview_w * CAM_PREVIEW_RATIO_H) / CAM_PREVIEW_RATIO_W;
        if (preview_h > available_h) {
            preview_h = available_h;
            preview_w = (preview_h * CAM_PREVIEW_RATIO_W) / CAM_PREVIEW_RATIO_H;
        }
        lv_coord_t preview_y = top_reserved + ((available_h - preview_h) / 2);

        lv_obj_set_size(cam_preview_img, preview_w, preview_h);
        lv_obj_align(cam_preview_img, LV_ALIGN_TOP_MID, 0, preview_y);
        lv_obj_set_style_radius(cam_preview_img, 0, 0);

        /* 恢复适配16:9矩形区域的缩放 */
        if (cam_thumb_w > 0 && cam_thumb_h > 0) {
            uint16_t scale_x = (uint16_t)((uint32_t)preview_w * LV_SCALE_NONE / cam_thumb_w);
            uint16_t scale_y = (uint16_t)((uint32_t)preview_h * LV_SCALE_NONE / cam_thumb_h);
            uint16_t scale = (scale_x < scale_y) ? scale_x : scale_y;
            lv_image_set_scale(cam_thumb_img, scale);
        }
        cam_thumb_off_x = 0;
        cam_thumb_off_y = 0;
        lv_obj_align(cam_thumb_img, LV_ALIGN_CENTER, 0, 0);
    }
}

void create_camera_page(lv_obj_t *parent)
{
    cam_screen_obj = parent;
    lv_coord_t screen_w = lv_display_get_horizontal_resolution(NULL);
    lv_coord_t screen_h = lv_display_get_vertical_resolution(NULL);

    /* ── 背景：纯黑色 ───────────────────────────────────────── */
    lv_obj_set_style_bg_color(parent, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_left(parent, 0, 0);
    lv_obj_set_style_pad_right(parent, 0, 0);
    lv_obj_set_style_pad_top(parent, 0, 0);
    lv_obj_set_style_pad_bottom(parent, 0, 0);
    lv_obj_set_scrollbar_mode(parent, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(parent, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(parent, LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_clear_flag(parent, LV_OBJ_FLAG_SCROLL_MOMENTUM);

    /* 确保大图保存目录存在 */
    mkdir("/emmc/camera", 0755);

    /* 每次进入页面视为初次：重置拍照状态 */
    cam_has_photo = false;

    /* ── 1. Log/Status area: FIXED at TOP (HIDDEN) ────────────── */
    lv_obj_t *status_label = lv_label_create(parent);
    lv_obj_align(status_label, LV_ALIGN_TOP_MID, 0, CAM_LOG_AREA_MARGIN_TOP);
    lv_label_set_text(status_label, i18n_get(STR_CAM_READY));
    lv_obj_set_style_text_color(status_label, lv_color_hex(0x00CC66), 0);
    i18n_apply_font(status_label);
    lv_label_set_long_mode(status_label, LV_LABEL_LONG_DOT);
    lv_obj_set_width(status_label, screen_w - 40);
    lv_obj_set_style_pad_top(status_label, 4, 0);
    lv_obj_set_style_pad_bottom(status_label, 4, 0);
    lv_obj_set_style_text_align(status_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_flag(status_label, LV_OBJ_FLAG_HIDDEN);  /* 屏蔽日志输出框 */
    cam_log_labels[0] = status_label;
    cam_log_index = 0;

    /* ── 加载中文字体(和meeting_page/ai_page保持一致) ─────────── */
#if LV_USE_FREETYPE
    if (!cam_title_font) {
        cam_title_font = lv_freetype_font_create(CAM_FONT_PATH,
            LV_FREETYPE_FONT_RENDER_MODE_BITMAP, CAM_TITLE_FONT_SIZE,
            LV_FREETYPE_FONT_STYLE_NORMAL);
        if (cam_title_font) {
            syslog(LOG_INFO, "[camera_page] Title FreeType font loaded\n");
        } else {
            syslog(LOG_WARNING, "[camera_page] Title FreeType font load failed, fallback to simsun\n");
        }
    }
    if (!cam_btn_font) {
        cam_btn_font = lv_freetype_font_create(CAM_FONT_PATH,
            LV_FREETYPE_FONT_RENDER_MODE_BITMAP, CAM_BTN_FONT_SIZE,
            LV_FREETYPE_FONT_STYLE_NORMAL);
        if (cam_btn_font) {
            syslog(LOG_INFO, "[camera_page] Button FreeType font loaded\n");
        } else {
            syslog(LOG_WARNING, "[camera_page] Button FreeType font load failed, fallback to simsun\n");
        }
    }
    if (!cam_countdown_font) {
        cam_countdown_font = lv_freetype_font_create(CAM_FONT_PATH,
            LV_FREETYPE_FONT_RENDER_MODE_BITMAP, CAM_COUNTDOWN_FONT_SIZE,
            LV_FREETYPE_FONT_STYLE_NORMAL);
        if (cam_countdown_font) {
            syslog(LOG_INFO, "[camera_page] Countdown FreeType font loaded\n");
        } else {
            syslog(LOG_WARNING, "[camera_page] Countdown FreeType font load failed, fallback to title font\n");
        }
    }
#endif

    /* ── 2. 顶部标题："图 文 智 录"(字间加空格，大字体) ─────── */
    lv_obj_t *title_label = lv_label_create(parent);
    lv_label_set_text(title_label, i18n_get(STR_CAM_TITLE));
    lv_obj_set_style_text_color(title_label, lv_color_white(), 0);
    cam_apply_font(title_label, cam_title_font);
    lv_obj_align(title_label, LV_ALIGN_TOP_MID, 0, CAM_TITLE_MARGIN_TOP);

    /* ── 3. 图片预览区域：16:9矩形，居中 ─────────────────────── */
    lv_coord_t top_reserved = CAM_TITLE_MARGIN_TOP + 40;  /* 大标题预留更多空间 */
    lv_coord_t bot_reserved = CAM_BTN_AREA_HEIGHT + CAM_BTN_AREA_MARGIN_BOT; /* 两排按钮预留空间 */
    lv_coord_t available_h = screen_h - top_reserved - bot_reserved;
    /* 圆形屏幕：增大左右边距，确保矩形完全在圆形可视区域内 */
    lv_coord_t available_w = screen_w - 80;
    
    /* 计算16:9预览尺寸，适配可用空间 */
    lv_coord_t preview_w = available_w;
    lv_coord_t preview_h = (preview_w * CAM_PREVIEW_RATIO_H) / CAM_PREVIEW_RATIO_W;
    if (preview_h > available_h) {
        preview_h = available_h;
        preview_w = (preview_h * CAM_PREVIEW_RATIO_W) / CAM_PREVIEW_RATIO_H;
    }
    lv_coord_t preview_y = top_reserved + ((available_h - preview_h) / 2);

    cam_preview_img = lv_obj_create(parent);
    lv_obj_remove_style_all(cam_preview_img);
    lv_obj_set_size(cam_preview_img, preview_w, preview_h);
    lv_obj_align(cam_preview_img, LV_ALIGN_TOP_MID, 0, preview_y);
    lv_obj_set_style_bg_color(cam_preview_img, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(cam_preview_img, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(cam_preview_img, 0, 0);
    lv_obj_set_style_radius(cam_preview_img, 0, 0);
    lv_obj_set_style_clip_corner(cam_preview_img, false, 0);
    lv_obj_clear_flag(cam_preview_img, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(cam_preview_img, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_flag(cam_preview_img, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(cam_preview_img, cam_preview_pressed_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(cam_preview_img, cam_preview_pressing_cb, LV_EVENT_PRESSING, NULL);

    /* 拍照前占位图标 - 使用转换后的C数组图片，只设置缩放不手动设置size避免双重缩放 */
    cam_placeholder_icon = lv_image_create(cam_preview_img);
    lv_image_set_src(cam_placeholder_icon, &image_preview);
    /* 缩放占位图标适配预览区域：不超过预览区域高度的70% */
    lv_coord_t placeholder_target = LV_MIN(preview_w, preview_h) * 7 / 10;
    uint16_t ph_scale = (uint16_t)((uint32_t)placeholder_target * LV_SCALE_NONE / 200);
    lv_image_set_scale(cam_placeholder_icon, ph_scale);
    lv_obj_center(cam_placeholder_icon);
    lv_obj_clear_flag(cam_placeholder_icon, LV_OBJ_FLAG_SCROLLABLE);

    /* 拍照后显示的照片 */
    cam_thumb_img = lv_image_create(cam_preview_img);
    lv_obj_add_flag(cam_thumb_img, LV_OBJ_FLAG_HIDDEN);

    /* ── 预览区中央倒计时大字号（点击拍照后显示，图片就绪/失败时隐藏） ── */
    cam_countdown_label = lv_label_create(cam_preview_img);
    lv_label_set_text(cam_countdown_label, "");
    lv_obj_set_style_text_color(cam_countdown_label, lv_color_white(), 0);
#if LV_USE_FREETYPE
    if (cam_countdown_font) {
        lv_obj_set_style_text_font(cam_countdown_label, cam_countdown_font, 0);
    } else
#endif
    if (cam_title_font) {
        lv_obj_set_style_text_font(cam_countdown_label, cam_title_font, 0);
    } else {
        cam_apply_font(cam_countdown_label, NULL);
    }
    lv_obj_center(cam_countdown_label);
    lv_obj_add_flag(cam_countdown_label, LV_OBJ_FLAG_HIDDEN);

    /* ── 预览区下方状态文字（拍照中/失败提示，透明背景） ─────── */
    cam_status_label = lv_label_create(parent);
    lv_label_set_text(cam_status_label, "");
    cam_apply_font(cam_status_label, cam_btn_font);
    lv_obj_set_style_text_color(cam_status_label, lv_color_white(), 0);
    lv_obj_align(cam_status_label, LV_ALIGN_TOP_MID, 0, preview_y + preview_h + 8);
    lv_obj_set_width(cam_status_label, screen_w - 40);
    lv_obj_set_style_text_align(cam_status_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_flag(cam_status_label, LV_OBJ_FLAG_HIDDEN);

    /* ── 4. 底部按钮区域：两排布局 ────────────────────────────
     * 第一排：拍照 + [空一个按钮宽度间距] + 下载（整体居中）
     * 第二排：分析（居中）
     * 三个按钮统一大小
     */
    lv_obj_t *btn_container = lv_obj_create(parent);
    lv_obj_remove_style_all(btn_container);
    lv_obj_set_size(btn_container, CAM_BTN_CONTAINER_WIDTH, CAM_BTN_AREA_HEIGHT);
    lv_obj_align(btn_container, LV_ALIGN_BOTTOM_MID, 0, -CAM_BTN_AREA_MARGIN_BOT);
    lv_obj_set_flex_flow(btn_container, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(btn_container,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(btn_container, 12, 0);
    lv_obj_clear_flag(btn_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(btn_container, LV_OBJ_FLAG_GESTURE_BUBBLE);

    /* ── 按钮排容器：拍照 + 间距 + 分析（单排居中） ── */
    lv_obj_t *row1 = lv_obj_create(btn_container);
    lv_obj_remove_style_all(row1);
    lv_obj_set_size(row1, CAM_BTN_CONTAINER_WIDTH, CAM_BTN_SIZE_H);
    lv_obj_set_flex_flow(row1, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row1,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row1, 70, 0); /* 两个图标之间间距70px */
    lv_obj_clear_flag(row1, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row1, LV_OBJ_FLAG_GESTURE_BUBBLE);

    /* 纯图标按钮辅助宏 - 直接用 lv_image，无 button 边框/背景
     * 图标本身作为可点击对象，56x56 触摸区域 */
    #define CREATE_ICON_BTN(btn_var, icon_dsc, parent_obj) \
        do { \
            btn_var = lv_image_create(parent_obj); \
            lv_image_set_src(btn_var, &(icon_dsc)); \
            lv_obj_add_flag(btn_var, LV_OBJ_FLAG_CLICKABLE); \
            lv_obj_set_size(btn_var, 56, 56); \
        } while(0)

    /* 拍照按钮：初始图标 photo_on（可拍照） */
    CREATE_ICON_BTN(cam_capture_btn, camera_page_photo_on, row1);
    lv_obj_add_event_cb(cam_capture_btn, cam_capture_btn_cb, LV_EVENT_CLICKED, NULL);

    /* 分析按钮：初始图标 ai_off + 半透明禁用态，大图落盘后启用并切 ai_on */
    CREATE_ICON_BTN(cam_ai_btn, camera_page_ai_off, row1);
    lv_obj_set_style_img_opa(cam_ai_btn, LV_OPA_50, 0);
    lv_obj_add_event_cb(cam_ai_btn, cam_ai_btn_cb, LV_EVENT_CLICKED, NULL);

    #undef CREATE_ICON_BTN

    lv_obj_add_event_cb(parent, cam_gesture_cb, LV_EVENT_GESTURE, NULL);
    lv_obj_add_event_cb(parent, cam_screen_load_cb, LV_EVENT_SCREEN_LOAD_START, NULL);

    cam_uart_fd = cam_uart_open();
    if (cam_uart_fd >= 0) {
        cam_uart_running = true;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, 4096);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        pthread_create(&cam_uart_rx_thread, &attr,
                       (void *(*)(void *))cam_uart_rx_thread_func, NULL);
        pthread_attr_destroy(&attr);
        cam_add_log("UART ready");
    } else {
        cam_add_log("UART open failed!");
    }

    cam_spi_fd = open(CAM_SPI_DEV, O_RDWR | O_NONBLOCK);
    if (cam_spi_fd >= 0) {
        cam_spi_running = true;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        /* SPI大图接收需大栈：stream_buf局部缓冲3.3KB + 帧解析(CRC16)
         * + 文件write/lseek + syslog格式化 + cam_uart_send_frame调用链。
         * 8192实测栈溢出致线程崩溃，无法回FRAME_ACK，2N端超时重传失败。
         * 与飞书上传线程对齐用128KB。 */
        pthread_attr_setstacksize(&attr, 128 * 1024);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        pthread_create(&cam_spi_rx_thread, &attr,
                       (void *(*)(void *))cam_spi_rx_thread_func, NULL);
        pthread_attr_destroy(&attr);
        syslog(LOG_INFO, "[camera_page] SPI device %s opened\n", CAM_SPI_DEV);
    } else {
        syslog(LOG_ERR, "[camera_page] open %s failed: %d\n", CAM_SPI_DEV, errno);
    }
}

/* ── 导出接口供AI页面调用 ───────────────────────────────────────── */
lv_obj_t *camera_page_get_screen(void)
{
    return cam_screen_obj;
}

void camera_page_set_buttons_enabled(bool enabled)
{
    cam_set_buttons_enabled(enabled);
}

void camera_page_set_busy(bool busy)
{
    cam_busy = busy;
}

void camera_page_deinit(void)
{
    /* 兜底：语言切换/程序退出时也取消建文档线程并清活跃文档，
     * 防止下次进入 camera 复用已失效的文档状态。 */
    camera_feishu_doc_cancel();

    cam_uart_running = false;
    cam_spi_running = false;
    cam_spi_recv_active = false;
    if (cam_timeout_timer) {
        lv_timer_del(cam_timeout_timer);
        cam_timeout_timer = NULL;
    }
    if (cam_spi_timeout_timer) {
        lv_timer_del(cam_spi_timeout_timer);
        cam_spi_timeout_timer = NULL;
    }
    if (cam_uart_fd >= 0) {
        close(cam_uart_fd);
        cam_uart_fd = -1;
    }
    if (cam_spi_fd >= 0) {
        close(cam_spi_fd);
        cam_spi_fd = -1;
    }
    if (cam_thumb_buf) {
        free(cam_thumb_buf);
        cam_thumb_buf = NULL;
    }
    if (cam_fail_hint_timer) {
        lv_timer_del(cam_fail_hint_timer);
        cam_fail_hint_timer = NULL;
    }
    if (cam_countdown_timer) {
        lv_timer_del(cam_countdown_timer);
        cam_countdown_timer = NULL;
    }
    cam_big_filename[0] = '\0';
    cam_has_photo = false;
    cam_preview_img = NULL;
    cam_thumb_img = NULL;
    cam_status_label = NULL;
    cam_capture_btn = NULL;
    cam_ai_btn = NULL;
    for (int i = 0; i < CAM_MAX_LOG_LINES; i++) {
        cam_log_labels[i] = NULL;
    }
    cam_placeholder_icon = NULL;
    cam_countdown_label = NULL;
    /* Bug3: 删除屏幕对象并置空，使下次进入时重建以刷新语言文案。
     * 注：全局 camera_screen 由调用方（lang_exit_to_settings）置空。 */
    if (cam_screen_obj) {
        lv_obj_del(cam_screen_obj);
        cam_screen_obj = NULL;
    }
}
