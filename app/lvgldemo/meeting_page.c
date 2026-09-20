
/****************************************************************************
 * apps/examples/lvgldemo/meeting_page.c
 *
 * Meeting transcription page - displays meeting transcript with speaker
 * labels, a running timer, and an "end meeting" button.
 *
 * Stage 2: Add meeting recording (save WAV file to /emmc/audio/).
 * Stage 3.1: Single-segment recording for FunASR long-audio mode.
 *
 * Memory strategy: FreeType CJK font is loaded ONCE (singleton) and reused
 * across meeting page entries. Never deleted on exit to avoid heap
 * fragmentation. media_recorder failures properly release mic/SelCap.
 *
 * Recording strategy: WAV 16kHz/16bit/mono (~1.8MB/min). Single file
 * meeting_<timestamp>.wav. FunASR supports up to 2GB/12h per file, so no
 * need to split. Storage: 3.9GB emmc enough for 30+ hours.
 *
 * Timing strategy: voice_assistant_stop() is called FIRST in
 * meeting_page_start() to interrupt any in-progress AI TTS/LLM dialog,
 * so the meeting page appears instantly. UI timer starts ONLY after
 * recording successfully starts, ensuring displayed duration matches
 * actual recording duration.
 *
 ****************************************************************************/
#include <nuttx/config.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <syslog.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <pthread.h>
#include <lvgl/lvgl.h>
#include "lvgl_dispatch.h"
#ifdef CONFIG_MEDIA
#include <media_recorder.h>
#include <media_policy.h>
#include <media_defs.h>
#endif
#include "meeting_page.h"
#include "meeting_asr.h"
#include "lvgldemo_common.h"
#include "menu_page.h"
#include "voice_assistant.h"
#include "wakeup_detector.h"
#include "meeting_feishu_sync.h"
#include "agent_config.h"
#include "llm/llm_proxy.h"
#include "cJSON.h"
#include "i18n.h"
/* 外部图标资源（32×32 RGB565，由LVGL在线转换器生成）*/
#include "icons/meeting_camera.c"
#include "icons/meeting_pause.c"
#include "icons/meeting_play.c"
#include "icons/meeting_resume.c"
#include "icons/meeting_stop_on.c"
#include "icons/meeting_stop_off.c"
#define MEETING_FONT_PATH "/emmc/font/MiSans-Normal.ttf"
#define MEETING_FONT_SIZE 20
#define MEETING_MSG_MAX   512
static lv_obj_t *meeting_title_label = NULL;
static lv_obj_t *meeting_timer_label = NULL;
static lv_obj_t *meeting_chat_area   = NULL;
static lv_obj_t *meeting_status_label = NULL;
static lv_obj_t *meeting_progress_bar = NULL;  /* 转写进度条 */
static lv_timer_t *meeting_progress_timer = NULL;  /* 渐进式进度定时器 */
static int meeting_progress_cap = 0;               /* 当前进度爬升上限 */
static lv_obj_t *current_asr_label  = NULL;
static char current_asr_text[512]   = {0};
static int  current_asr_sid         = -1;  /* 当前字幕句子的云端sentence_id */
static char s_full_transcription[8192] = {0}; /* 完整会议转写文本 */
static char s_meeting_summary[8192]   = {0}; /* LLM 归纳的会议纪要（设备端显示用） */
static volatile bool s_feishu_save_ok = false; /* 飞书保存结果（done_cb读取） */
/* ASR字幕事件包：{text,sid} 打包随 dispatch 队列传递（队列本身保序）。
 * 替代跨线程共享变量：补发场景事件密集连发，共享值在两次派发之间
 * 可能已被下一个事件改写（竞态） */
typedef struct {
    char *text;
    int sid;
} asr_result_msg_t;
static lv_font_t *meeting_cjk_font   = NULL;
static lv_font_t *meeting_title_font = NULL;  /* 标题专用大字体(28px) */
static lv_timer_t *meeting_timer     = NULL;
static time_t meeting_start_time     = 0;  /* 墙上时钟，用于飞书文档时间戳（受NTP影响） */
static struct timespec meeting_start_mono_ts = {0, 0};  /* 单调时钟，用于计时器显示（不受NTP影响） */
static time_t meeting_pause_start_mono = 0;  /* 本次暂停起始单调时钟秒数 */
static volatile bool meeting_active  = false;
static volatile bool meeting_recording = false;
static volatile bool meeting_paused    = false;  /* 暂停标志：true=已暂停（recording仍保持true） */
static long   meeting_paused_total     = 0;      /* 累计暂停秒数（用于扣减计时显示，单调时钟） */
static lv_obj_t *pause_btn             = NULL;   /* 暂停/恢复按钮 */
static lv_obj_t *pause_btn_img         = NULL;   /* 暂停/恢复按钮图标（切换 pause/resume） */
static lv_obj_t *stop_btn              = NULL;   /* 停止按钮 */
static lv_obj_t *stop_btn_img          = NULL;   /* 停止按钮图标（切换 stop_on/stop_off） */
static bool toast_active               = false;  /* toast 防抖：显示期间忽略新请求 */
static char saved_status_text[64]      = {0};    /* toast 触发时保存的状态文案 */
static lv_color_t saved_status_color;            /* toast 触发时保存的状态颜色 */
static lv_timer_t *toast_timer         = NULL;   /* toast 自动恢复定时器 */
static lv_obj_t *meeting_idle_container = NULL;  /* 等待开始界面覆盖层 */
static char meeting_recorder_path[64];
#define MEETING_MAX_SEGMENTS 64                 /* 段文件上限（暂停次数） */
static char meeting_recorder_base[56];          /* 段文件名前缀（不含 .wav） */
static int  meeting_seg_count = 0;              /* 已录制段数（含当前正在录制的段） */
/* 前置声明：包装函数定义在后面，pause_btn_cb 需要提前调用 */
static int meeting_recorder_prepare(const char *path);
static int meeting_recorder_start(void);
static int meeting_recorder_stop(void);
#ifdef CONFIG_MEDIA
static void *meeting_recorder_handle = NULL;
/* 方案B：第二个 media_recorder 实例，不写文件，纯 PCM 读取供流式 ASR 消费。
 * 验证脚本 meeting_dual_recorder_test.c 已证明双实例可行。 */
static void *meeting_asr_recorder_handle = NULL;
static volatile int meeting_asr_stop_flag = 0;  /* 1=请求ASR线程退出 */
static volatile bool meeting_asr_aborted = false; /* 页面销毁中断：跳过LLM+飞书后处理 */
static pthread_t meeting_stream_asr_thread;
static volatile int meeting_stream_asr_thread_running = 0;
extern uint32_t af_stream_set_chan_vol(uint32_t id, uint32_t stream,
                                       uint32_t ch_map, uint8_t vol)
                                       __attribute__((weak));
#define MEETING_BES_AUD_ID_0     0
#define MEETING_BES_AUD_CAPTURE  1
#define MEETING_BES_AUD_CH0      1
#define MEETING_BES_MIC_GAIN_MAX 15
#endif
static void meeting_apply_font(lv_obj_t *obj)
{
#if LV_USE_FREETYPE
    if (meeting_cjk_font) {
        lv_obj_set_style_text_font(obj, meeting_cjk_font, 0);
        return;
    }
#endif
#if LV_FONT_SIMSUN_16_CJK
    lv_obj_set_style_text_font(obj, &lv_font_simsun_16_cjk, 0);
#endif
}
/* toast 自动恢复：写回拦截前保存的状态文案与颜色 */
static void meeting_restore_status_cb(lv_timer_t *t)
{
    LV_UNUSED(t);
    if (meeting_status_label && saved_status_text[0]) {
        lv_label_set_text(meeting_status_label, saved_status_text);
        lv_obj_set_style_text_color(meeting_status_label, saved_status_color, 0);
    }
    toast_active = false;
    toast_timer = NULL;
}

/* 显示拦截 toast：“请先停止录制”，1s 后恢复原状态文案。
 * 防抖：toast_active 期间忽略新请求，避免连续右滑导致重叠刷新。 */
static void meeting_show_toast(void)
{
    if (toast_active) return;
    toast_active = true;
    if (meeting_status_label) {
        const char *cur = lv_label_get_text(meeting_status_label);
        if (cur) {
            strncpy(saved_status_text, cur, sizeof(saved_status_text) - 1);
            saved_status_text[sizeof(saved_status_text) - 1] = '\0';
        }
        saved_status_color = lv_obj_get_style_text_color(meeting_status_label, 0);
        lv_label_set_text(meeting_status_label, i18n_get(STR_MEET_STOP_FIRST));
        lv_obj_set_style_text_color(meeting_status_label, lv_color_hex(0xFFC107), 0);
    }
    if (toast_timer) lv_timer_del(toast_timer);
    toast_timer = lv_timer_create(meeting_restore_status_cb, 1000, NULL);
    lv_timer_set_repeat_count(toast_timer, 1);
}

/* pause/resume 失败提示：闪现“操作失败，请重试”1s 后恢复。
 * 与 toast 互斥：仅在未触发 toast 时保存当前文案，避免覆盖真实状态。 */
static void meeting_show_pause_fail(void)
{
    if (!toast_active) {
        toast_active = true;
        if (meeting_status_label) {
            const char *cur = lv_label_get_text(meeting_status_label);
            if (cur) {
                strncpy(saved_status_text, cur, sizeof(saved_status_text) - 1);
                saved_status_text[sizeof(saved_status_text) - 1] = '\0';
            }
            saved_status_color = lv_obj_get_style_text_color(meeting_status_label, 0);
        }
    }
    if (meeting_status_label) {
        lv_label_set_text(meeting_status_label, i18n_get(STR_MEET_PAUSE_FAIL));
        lv_obj_set_style_text_color(meeting_status_label, lv_color_hex(0xE53935), 0);
    }
    if (toast_timer) lv_timer_del(toast_timer);
    toast_timer = lv_timer_create(meeting_restore_status_cb, 1000, NULL);
    lv_timer_set_repeat_count(toast_timer, 1);
}
/* 构建段文件路径：base + ".seg<idx>.wav" */
static void meeting_seg_path(int idx, char *buf, size_t bufsize)
{
    snprintf(buf, bufsize, "%s.seg%d.wav", meeting_recorder_base, idx);
}
/* 解析 WAV 文件头部，找到 fmt 和 data chunk。
 * 不假设固定 44 字节头，动态遍历 chunk 识别。
 * 返回 0 成功，<0 失败。成功时填入 fmt_body(16字节) 和 data_offset/data_size。 */
static int meeting_wav_parse_header(FILE *fp, uint8_t fmt_body[16],
                                     long *data_offset, uint32_t *data_size)
{
    uint8_t riff[12];
    if (fread(riff, 1, 12, fp) != 12) return -1;
    if (memcmp(riff, "RIFF", 4) != 0 || memcmp(riff + 8, "WAVE", 4) != 0) {
        return -2;
    }
    long pos = 12;
    int fmt_found = 0, data_found = 0;
    *data_offset = 0;
    *data_size = 0;
    while (pos < 4096) {
        if (fseek(fp, pos, SEEK_SET) != 0) break;
        uint8_t ch[8];
        if (fread(ch, 1, 8, fp) != 8) break;
        uint32_t csz = (uint32_t)ch[4] | ((uint32_t)ch[5] << 8)
                     | ((uint32_t)ch[6] << 16) | ((uint32_t)ch[7] << 24);
        if (memcmp(ch, "fmt ", 4) == 0 && csz >= 16) {
            if (fread(fmt_body, 1, 16, fp) == 16) fmt_found = 1;
        } else if (memcmp(ch, "data", 4) == 0) {
            *data_offset = pos + 8;
            *data_size = csz;
            data_found = 1;
            break;  /* data 通常在最后 */
        }
        pos += 8 + (long)csz + (csz & 1);  /* word-aligned */
    }
    if (!fmt_found || !data_found) return -3;
    return 0;
}
/* 构建标准 44 字节 WAV header（RIFF + fmt(16) + data）。
 * 使用 seg0 的 fmt_body 作为格式参数，填入正确的 size。 */
static void meeting_wav_build_header(uint8_t header[44], const uint8_t fmt_body[16],
                                      uint32_t total_data)
{
    memcpy(header, "RIFF", 4);
    uint32_t riff_size = total_data + 36;
    memcpy(header + 4, &riff_size, 4);
    memcpy(header + 8, "WAVE", 4);
    memcpy(header + 12, "fmt ", 4);
    uint32_t fmt_size = 16;
    memcpy(header + 16, &fmt_size, 4);
    memcpy(header + 20, fmt_body, 16);
    memcpy(header + 36, "data", 4);
    memcpy(header + 40, &total_data, 4);
}
/* 拼接多段 WAV 为单个标准 WAV（动态解析头部，不假设固定 44 字节）。
 * 1. 解析 seg0 头部提取 fmt + data 偏移
 * 2. 遍历所有段从 data 偏移拷贝 PCM 数据，计算总量
 * 3. 构建 44 字节标准 header + 顺序写入所有 PCM 数据
 * 4. 校验最终文件大小与预期一致
 * 返回 0 成功，<0 失败。 */
static int meeting_wav_concat(int n, const char *out)
{
    if (n <= 0) return -1;
    char seg_path[80];
    char buf[4096];

    /* 1. 解析 seg0 头部，提取 fmt_body 和 data 偏移作为参考 */
    meeting_seg_path(0, seg_path, sizeof(seg_path));
    FILE *src0 = fopen(seg_path, "rb");
    if (!src0) {
        syslog(LOG_ERR, "[meeting_page] concat: open seg0 failed\n");
        return -1;
    }
    uint8_t fmt_body[16];
    long seg0_data_offset;
    uint32_t seg0_data_size;
    int ret = meeting_wav_parse_header(src0, fmt_body, &seg0_data_offset, &seg0_data_size);
    fclose(src0);
    if (ret != 0) {
        syslog(LOG_ERR, "[meeting_page] concat: seg0 header invalid (%d)\n", ret);
        return -1;
    }
    syslog(LOG_INFO, "[meeting_page] concat: seg0 data_offset=%ld data_size=%u\n",
           seg0_data_offset, seg0_data_size);

    /* 2. 遍历所有段，解析每段头部找 data 偏移，从 data 偏移拷贝 PCM 数据。
     *    最后一段可能刚 stop，SMF AutoSink 尚未完成头部最终化，
     *    遇到解析失败时重试一次（等 100ms 让其完成）。 */
    uint32_t total_data = 0;
    long data_offsets[MEETING_MAX_SEGMENTS];
    for (int i = 0; i < n; i++) {
        meeting_seg_path(i, seg_path, sizeof(seg_path));
        FILE *src = fopen(seg_path, "rb");
        if (!src) {
            syslog(LOG_WARNING, "[meeting_page] concat: skip missing %s\n", seg_path);
            data_offsets[i] = -1;
            continue;
        }
        uint8_t ftmp[16];
        long doff;
        uint32_t dsize;
        if (meeting_wav_parse_header(src, ftmp, &doff, &dsize) != 0) {
            /* 重试：可能是最后一段 AutoSink 还在写头部 */
            syslog(LOG_INFO, "[meeting_page] concat: seg%d parse fail, retry after 100ms\n", i);
            fclose(src);
            usleep(100 * 1000);
            src = fopen(seg_path, "rb");
            if (!src || meeting_wav_parse_header(src, ftmp, &doff, &dsize) != 0) {
                syslog(LOG_WARNING, "[meeting_page] concat: seg%d header invalid, skip\n", i);
                if (src) fclose(src);
                data_offsets[i] = -1;
                continue;
            }
        }
        data_offsets[i] = doff;
        total_data += dsize;
        fclose(src);
    }
    if (total_data == 0) {
        syslog(LOG_ERR, "[meeting_page] concat: no valid data\n");
        return -1;
    }

    /* 3. 构建 44 字节标准 header + 顺序写入所有段的 PCM 数据 */
    uint8_t header[44];
    meeting_wav_build_header(header, fmt_body, total_data);

    FILE *dst = fopen(out, "wb");
    if (!dst) {
        syslog(LOG_ERR, "[meeting_page] concat: open out %s failed\n", out);
        return -1;
    }
    if (fwrite(header, 1, 44, dst) != 44) {
        syslog(LOG_ERR, "[meeting_page] concat: write header failed\n");
        fclose(dst);
        return -1;
    }
    /* 每段做 DC 阻断（一阶高通，R=0.995，截止~16Hz）消除 mic 上电后的 DC 偏置，
     * 再对开头 10ms(160样本)做线性 fade-in 平滑初始瞬态。
     * 根因：纯 fade-in 会把 DC 跳变拉成 33Hz 可闻啵声，必须先消除 DC。
     * DC 阻断对语音(80Hz+)透明；不改变样本数，total_data 与 header 校验不受影响。 */
    const uint32_t FADE_IN_SAMPLES = 1600;  /* 100ms */
    const int32_t DC_R = 32631;            /* 0.995 in Q15 */
    for (int i = 0; i < n; i++) {
        if (data_offsets[i] < 0) continue;
        meeting_seg_path(i, seg_path, sizeof(seg_path));
        FILE *src = fopen(seg_path, "rb");
        if (!src) continue;
        fseek(src, data_offsets[i], SEEK_SET);
        size_t r;
        uint32_t faded = 0;
        int32_t xm1 = 0, ym1 = 0;          /* DC 阻断状态，每段重置 */
        while ((r = fread(buf, 1, sizeof(buf), src)) > 0) {
            size_t nsamp = r / 2;
            int16_t *s = (int16_t *)buf;
            for (size_t j = 0; j < nsamp; j++) {
                int32_t x = s[j];
                int32_t y = x - xm1 + (DC_R * ym1) / 32768;
                xm1 = x;
                ym1 = y;
                if (faded < FADE_IN_SAMPLES) {
                    int32_t gain = (int32_t)faded * 32767 / FADE_IN_SAMPLES;
                    y = y * gain / 32767;
                    faded++;
                }
                if (y > 32767) y = 32767;     /* 限幅防溢出 */
                if (y < -32768) y = -32768;
                s[j] = (int16_t)y;
            }
            fwrite(buf, 1, r, dst);
        }
        fclose(src);
    }
    fflush(dst);
    long file_size = ftell(dst);
    fclose(dst);

    /* 4. 完整性校验：文件大小 = 44 + total_data，RIFF/data size 一致 */
    long expected_size = 44 + (long)total_data;
    if (file_size != expected_size) {
        syslog(LOG_ERR, "[meeting_page] concat: size mismatch file=%ld expected=%ld\n",
               file_size, expected_size);
        return -1;
    }
    /* 回读校验：打开最终文件验证 header 正确性 */
    FILE *verify = fopen(out, "rb");
    if (verify) {
        uint8_t vh[44];
        size_t vn = fread(vh, 1, 44, verify);
        fclose(verify);
        if (vn != 44 || memcmp(vh, "RIFF", 4) != 0 || memcmp(vh + 8, "WAVE", 4) != 0) {
            syslog(LOG_ERR, "[meeting_page] concat: verify RIFF/WAVE failed\n");
            return -1;
        }
        uint32_t vriff, vdata;
        memcpy(&vriff, vh + 4, 4);
        memcpy(&vdata, vh + 40, 4);
        if (vriff != total_data + 36 || vdata != total_data) {
            syslog(LOG_ERR, "[meeting_page] concat: verify size failed riff=%u data=%u\n",
                   vriff, vdata);
            return -1;
        }
    }
    syslog(LOG_INFO, "[meeting_page] concat: %d segs -> %s (data=%u size=%ld verified)\n",
           n, out, total_data, file_size);
    return 0;
}
/* 删除所有段临时文件 */
static void meeting_cleanup_segments(int n)
{
    char seg_path[80];
    for (int i = 0; i < n; i++) {
        meeting_seg_path(i, seg_path, sizeof(seg_path));
        if (unlink(seg_path) != 0) {
            syslog(LOG_WARNING, "[meeting_page] cleanup: unlink %s failed\n", seg_path);
        }
    }
}
/* 暂停/恢复按钮回调：toggle 暂停状态。
 * 防抖：调用期间禁用按钮，返回后恢复，避免快速连点导致状态错乱。
 * 失败容错：API 返回非 0 时不切换 UI 状态（图标/meeting_paused 均不变），
 * 保持原录制态，仅闪现失败提示。 */
static void meeting_pause_btn_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    if (!meeting_recording) return;

    lv_obj_add_state(pause_btn, LV_STATE_DISABLED);  /* 防抖 */

    int ret = -1;
    if (!meeting_paused) {
        /* 段数上限检查：防止无限暂停导致段文件过多 */
        if (meeting_seg_count >= MEETING_MAX_SEGMENTS) {
            syslog(LOG_WARNING, "[meeting_page] max segments reached: %d\n", meeting_seg_count);
            meeting_show_pause_fail();
            lv_obj_clear_state(pause_btn, LV_STATE_DISABLED);
            return;
        }
        /* 暂停 = stop 当前段（生成合法 WAV），不调用 pause API */
        ret = meeting_recorder_stop();
        if (ret == 0) {
            meeting_paused = true;
            struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
            meeting_pause_start_mono = ts.tv_sec;
            lv_img_set_src(pause_btn_img, &meeting_resume);
            if (meeting_status_label) {
                lv_label_set_text(meeting_status_label, i18n_get(STR_MEET_PAUSED));
                lv_obj_set_style_text_color(meeting_status_label,
                    lv_color_hex(0xFFC107), 0);  /* 黄色 */
            }
            syslog(LOG_INFO, "[meeting_page] recording paused (seg %d stopped)\n", meeting_seg_count - 1);
        }
    } else {
        /* 恢复 = prepare(新段路径) + start，复用同一 handle */
        char seg_path[80];
        meeting_seg_path(meeting_seg_count, seg_path, sizeof(seg_path));
        usleep(50 * 1000);  /* 等 framework 完成上一轮 stop 清理 */
        ret = meeting_recorder_prepare(seg_path);
        if (ret == 0) {
            ret = meeting_recorder_start();
        }
        if (ret == 0) {
            meeting_seg_count++;  /* 新段已开始 */
            struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
            meeting_paused_total += (long)(ts.tv_sec - meeting_pause_start_mono);
            meeting_paused = false;
            lv_img_set_src(pause_btn_img, &meeting_pause);
            if (meeting_status_label) {
                lv_label_set_text(meeting_status_label, i18n_get(STR_MEET_RECORDING_PROMPT));
                lv_obj_set_style_text_color(meeting_status_label,
                    lv_color_hex(0x00C853), 0);  /* 绿色 */
            }
            syslog(LOG_INFO, "[meeting_page] recording resumed (seg %d started)\n", meeting_seg_count - 1);
        }
    }

    if (ret != 0) {
        syslog(LOG_ERR, "[meeting_page] pause/resume failed: %d\n", ret);
        meeting_show_pause_fail();
    }

    lv_obj_clear_state(pause_btn, LV_STATE_DISABLED);  /* 恢复可点击 */
}
/* 主线程：字幕更新回调（由后台线程dispatch调用） */
static void meeting_asr_result_async_cb(void *arg)
{
    asr_result_msg_t *m = (asr_result_msg_t *)arg;
    if (!m) return;
    meeting_page_update_asr_transcript(m->text, m->sid);
    free(m->text);
    free(m);
}
/* 主线程：进度条更新回调（文件模式逐块直设，维持原行为） */
static void meeting_asr_progress_async_cb(void *arg)
{
    int percent = (int)(intptr_t)arg;
    if (meeting_progress_bar) {
        lv_bar_set_value(meeting_progress_bar, percent, LV_ANIM_OFF);
    }
}
/* 渐进式进度定时器：向 cap 以 +1/100ms（≈10%/s）爬升。
 * 里程碑只抬高 cap、从不直接设值——两次里程碑之间的等待期
 * （finish-task收尾/LLM归纳/飞书保存）进度条持续缓慢前进，
 * 从0%连续推进到100%无跳变。到达 cap 后定时器自删，
 * 待下一里程碑重建。 */
static void meeting_progress_timer_cb(lv_timer_t *t)
{
    if (!meeting_progress_bar ||
        lv_bar_get_value(meeting_progress_bar) >= meeting_progress_cap) {
        lv_timer_del(t);
        meeting_progress_timer = NULL;
        return;
    }
    lv_bar_set_value(meeting_progress_bar,
                     lv_bar_get_value(meeting_progress_bar) + 1, LV_ANIM_OFF);
}
/* 阶段推进（LVGL线程）：抬升 cap 至下一里程碑-2 并启动爬升 */
static void meeting_progress_stage(int stage)
{
    if (!meeting_progress_bar) return;
    lv_obj_clear_flag(meeting_progress_bar, LV_OBJ_FLAG_HIDDEN);
    static const int stages[] = {30, 40, 70, 90, 100};
    int cap = stage;
    for (size_t i = 0; i < sizeof(stages) / sizeof(stages[0]); i++) {
        if (stages[i] > stage) { cap = stages[i] - 2; break; }
    }
    if (cap > meeting_progress_cap) meeting_progress_cap = cap;
    if (!meeting_progress_timer) {
        meeting_progress_timer = lv_timer_create(meeting_progress_timer_cb,
                                                 100, NULL);
    }
}
/* 主线程：阶段里程碑异步回调（ASR线程派发） */
static void meeting_asr_stage_async_cb(void *arg)
{
    meeting_progress_stage((int)(intptr_t)arg);
}
/* 主线程：转写完成回调
 * 进度条行为保持原样：100%+隐藏。
 * 状态文本根据飞书保存结果显示：成功显示"转写完成"，失败显示"飞书保存失败"。 */
static void meeting_asr_done_async_cb(void *arg)
{
    LV_UNUSED(arg);
    if (meeting_progress_timer) {
        lv_timer_del(meeting_progress_timer);
        meeting_progress_timer = NULL;
    }
    meeting_progress_cap = 0;
    if (meeting_progress_bar) {
        lv_bar_set_value(meeting_progress_bar, 100, LV_ANIM_OFF);
        lv_obj_add_flag(meeting_progress_bar, LV_OBJ_FLAG_HIDDEN);
    }

    /* 转写完成后，如果有 LLM 纪要，清空字幕区显示纪要内容（可滚动查看） */
    if (s_meeting_summary[0] && meeting_chat_area) {
        lv_obj_clean(meeting_chat_area);  /* 清空转写过程中的字幕 labels */
        current_asr_label = NULL;  /* 已被 clean 释放，置空避免悬空指针 */

        /* 纪要标题 */
        lv_obj_t *title = lv_label_create(meeting_chat_area);
        lv_label_set_text(title, i18n_get(STR_MEET_SUMMARY_TITLE));
        lv_label_set_long_mode(title, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(title, LV_PCT(100));
        lv_obj_set_style_text_color(title, lv_color_hex(0x00C853), 0);  /* 绿色标题 */
        meeting_apply_font(title);

        /* 纪要内容 */
        lv_obj_t *body = lv_label_create(meeting_chat_area);
        lv_label_set_text(body, s_meeting_summary);
        lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(body, LV_PCT(100));
        lv_obj_set_style_text_color(body, lv_color_hex(0xFFFFFF), 0);  /* 白色正文 */
        meeting_apply_font(body);

        /* 滚动到顶部，从纪要标题开始查看 */
        lv_obj_scroll_to_y(meeting_chat_area, 0, LV_ANIM_OFF);
    }

    if (meeting_status_label) {
        if (s_feishu_save_ok) {
            lv_label_set_text(meeting_status_label, i18n_get(STR_MEET_DONE));
            lv_obj_set_style_text_color(meeting_status_label,
                lv_color_hex(0x00C853), 0);
        } else {
            lv_label_set_text(meeting_status_label, i18n_get(STR_MEET_DONE_FEISHU_FAIL));
            lv_obj_set_style_text_color(meeting_status_label,
                lv_color_hex(0xFF8800), 0);
        }
    }
}
/* 主线程：错误回调（红色提示） */
static void meeting_asr_error_async_cb(void *arg)
{
    char *msg = (char *)arg;
    if (!msg) return;
    if (meeting_status_label) {
        lv_label_set_text(meeting_status_label, msg);
        lv_obj_set_style_text_color(meeting_status_label,
            lv_color_hex(0xE53935), 0);
    }
    if (meeting_progress_bar) {
        lv_obj_add_flag(meeting_progress_bar, LV_OBJ_FLAG_HIDDEN);
    }
    free(msg);
}

/* C：连接中占位提示异步回调（定义在文件后部，此处前向声明） */
static void meeting_conn_hint_async_cb(void *unused);
/* ASR线程：结果回调 */
static void meeting_asr_on_result(const char *text, int sentence_id,
                                  void *user_data)
{
    LV_UNUSED(user_data);
    if (!text) return;
    if (sentence_id < 0) {
        /* 错误/状态提示：派发到主线程红色显示 */
        char *msg = strdup(text);
        if (msg) {
            if (!lvgl_dispatch_async(meeting_asr_error_async_cb, msg)) {
                free(msg);
            }
        }
        return;
    }
    /* 正常字幕：{text,sid} 打包后派发到主线程显示。
     * sid 必须随事件传递而非共享全局变量：补发场景事件密集连发，
     * 两次派发之间全局值可能已被下一个事件改写 */
    asr_result_msg_t *m = malloc(sizeof(*m));
    if (m) {
        m->sid = sentence_id;
        m->text = strdup(text);
        if (m->text) {
            if (!lvgl_dispatch_async(meeting_asr_result_async_cb, m)) {
                free(m->text);
                free(m);
            }
        } else {
            free(m);
        }
    }
}
/* ASR线程：进度回调 */
static void meeting_asr_on_progress(int percent, void *user_data)
{
    LV_UNUSED(user_data);
    lvgl_dispatch_async(meeting_asr_progress_async_cb,
                        (void *)(intptr_t)percent);
}
/* ASR后台线程入口（方案B：边录边转）
 * 使用流式API从 rec2 读 PCM，录音期间实时显示字幕到 meeting_chat_area。
 * 录音停止时 meeting_page_stop 置位 meeting_asr_stop_flag，本线程退出后执行
 * 后处理：补最后一句话 + LLM归纳 + 飞书保存（同原 file 版逻辑）。 */
static void *meeting_asr_thread_func(void *arg)
{
    LV_UNUSED(arg);
    syslog(LOG_INFO, "[meeting_page] ASR stream thread started\n");
    int ret;
    if (meeting_asr_recorder_handle != NULL) {
        /* 方案B：边录边转。pause_flag 指向 meeting_paused，
         * 暂停期间线程休眠不发音频，恢复后继续。 */
        if (meeting_asr_conn_pending()) {
            lvgl_dispatch_async(meeting_conn_hint_async_cb, NULL);
        }
        ret = meeting_asr_transcribe_stream(meeting_asr_recorder_handle,
                                            meeting_asr_on_result,
                                            meeting_asr_on_progress,
                                            &meeting_asr_stop_flag,
                                            &meeting_paused,
                                            NULL);
    } else {
        /* 降级：rec2 打开失败时等待会议结束（stop_flag 置位、WAV 已 finalize），
         * 再走原文件转写路径。此时录音刚开始，立即转文件必然失败。 */
        syslog(LOG_WARNING, "[meeting_page] asr_recorder NULL, wait meeting end then fallback to file mode\n");
        while (meeting_asr_stop_flag == 0) {
            usleep(200 * 1000);
        }
        usleep(500 * 1000);  /* 等待 stop() 完成 WAV concat 后处理 */
        ret = meeting_asr_transcribe_file(meeting_recorder_path,
                                          meeting_asr_on_result,
                                          meeting_asr_on_progress,
                                          NULL);
    }
    if (ret != 0) {
        syslog(LOG_ERR, "[meeting_page] ASR failed: %d\n", ret);
    }
    /* 流式路径返回成功却无任何文本：rec2无数据静默超时或云端task失败。
     * 在聊天区显示错误提示，避免用户误以为转写正常完成 */
    if (ret == 0 && s_full_transcription[0] == '\0' && current_asr_text[0] == '\0') {
        syslog(LOG_ERR, "[meeting_page] stream ASR produced no text\n");
        char *msg = strdup("[ASR] 未收到转写内容，录音已保存");
        if (msg) {
            if (!lvgl_dispatch_async(meeting_asr_error_async_cb, msg)) {
                free(msg);
            }
        }
    }
    /* 转写收尾完成（边录边转下音频早已发完，此点通常在点结束后
     * 数秒内到达）：进度30%。异步派发：直接调用会在 ASR 线程
     * 操作 LVGL 控件（非线程安全）且会被排队中的旧事件覆盖 */
    lvgl_dispatch_async(meeting_asr_stage_async_cb, (void *)(intptr_t)30);
    /* 页面已销毁（语言切换重建等）：跳过补末句+LLM归纳+飞书保存。
     * 否则旧线程后处理会与用户重启的新会议并发读写 s_full_transcription
     * （start 时会清空该缓冲区），造成数据竞争与内容错乱 */
    if (meeting_asr_aborted) {
        syslog(LOG_WARNING, "[meeting_page] meeting aborted, skip post-processing\n");
#ifdef CONFIG_MEDIA
        if (meeting_asr_recorder_handle != NULL) {
            media_recorder_close(meeting_asr_recorder_handle);
            meeting_asr_recorder_handle = NULL;
        }
#endif
        meeting_stream_asr_thread_running = 0;
        syslog(LOG_INFO, "[meeting_page] ASR stream thread exited (aborted)\n");
        return NULL;
    }
    /* ASR完成后，本线程内同步保存到飞书文档（复用本线程32KB栈，避免
     * 创建detached线程的栈大小风险。直接传 s_full_transcription，函数内只读）。
     *
     * 补充：将最后一句话的最终版本追加到完整转录文本。
     * 因为 update_asr_transcript 只在新句子开始时(!is_continuation)追加中间版本，
     * 最后一句话的同句累积(is_continuation=true)不会触发追加。
     * current_asr_text 始终保存该句的最新(最长)版本，需要在这里补上。 */
    if (current_asr_text[0]) {
        size_t existing_len = strlen(s_full_transcription);
        size_t available = sizeof(s_full_transcription) - existing_len - 2;
        if (available > 0 && existing_len > 0) {
            strncat(s_full_transcription, "\n", available--);
        }
        if (available > 0) {
            strncat(s_full_transcription, current_asr_text, available);
        }
    }
    s_feishu_save_ok = true; /* 默认成功：空内容或ASR失败时不掩盖转写完成事实 */
    if (s_full_transcription[0]) {
        syslog(LOG_INFO, "[meeting_page] Saving to Feishu (in ASR thread)...\n");
        syslog(LOG_INFO, "[meeting_page] transcription length=%zu\n", strlen(s_full_transcription));

        /* 云端LLM归纳会议纪要（复用预连接的LLM连接池，避免重复TLS握手）。
         * 纪要放在文档开头，完整转写放在后面，便于快速浏览重点。
         * 用堆分配 8192 字节：LLM max_tokens=4096，中文 UTF-8 约 3 字节/字，
         * 4096 tokens ≈ 2000-3000 汉字 ≈ 6000-9000 字节，4096 缓冲区会截断。 */
        char *summary_buf = malloc(8192);
        const char *final_content;
        char *merged = NULL;

        if (summary_buf) {
            summary_buf[0] = '\0';
            const char *summary_prompt;

            lang_t current_lang = i18n_get_lang();
            if (current_lang == LANG_EN) {
                summary_prompt =
                    "Generate a concise content summary from the following voice transcription. "
                    "The recording could be a work meeting, daily conversation, study notes, or any type of speech.\n"
                    "Output format requirements (plain text only, no markdown symbols like *, #, or -):\n"
                    "Topic: One sentence describing the core subject or theme of this recording "
                    "(if it's poetry recitation, state the poem name; if it's casual chat, summarize the topic)\n"
                    "Summary: Adapt based on content type. For work/study, extract key points and conclusions; "
                    "for poetry/reading, briefly describe the work info and main content; "
                    "for daily conversation, summarize the discussion topics and key information. Keep within 200 words\n"
                    "Important: Always generate a meaningful summary regardless of content type. "
                    "Never say 'unable to generate' or 'no substantive content'. Output the two parts directly.";
            } else {
                summary_prompt =
                    "请根据以下语音转写文本生成简洁的内容摘要。这段录音可能是工作会议、日常对话、学习记录或任何类型的语音内容。\n"
                    "输出格式要求（纯文本，不要使用任何 markdown 标记符号如星号、井号、短横线等）：\n"
                    "内容主题：用一句话概括这段录音的核心内容或主题（如果是诗词朗诵就说明是诗词及名称，如果是闲聊就概括聊天话题）\n"
                    "内容总结：根据实际情况灵活总结。若是工作/学习类，提炼要点和结论；若是诗词/朗读类，简要说明作品信息及主要内容；若是日常对话，概括讨论的话题和关键信息。控制在200字以内\n"
                    "重要：无论内容类型如何，都要生成有意义的摘要，不要说'无法生成'或'无实质内容'之类的话。直接输出上述两部分内容即可。";
            }

            cJSON *msg_arr = cJSON_CreateArray();
            cJSON *user_msg = cJSON_CreateObject();
            cJSON_AddStringToObject(user_msg, "role", "user");
            cJSON_AddStringToObject(user_msg, "content", s_full_transcription);
            cJSON_AddItemToArray(msg_arr, user_msg);
            char *messages_json = cJSON_PrintUnformatted(msg_arr);
            cJSON_Delete(msg_arr);

            if (messages_json) {
                syslog(LOG_INFO, "[meeting_page] Calling LLM for meeting summary...\n");
                lvgl_dispatch_async(meeting_asr_stage_async_cb, (void *)(intptr_t)40);
                int llm_ret = llm_chat(summary_prompt, messages_json,
                                       summary_buf, 8192);
                free(messages_json);
                if (llm_ret == OK && summary_buf[0]) {
                    syslog(LOG_INFO, "[meeting_page] LLM summary done, %d bytes\n",
                           (int)strlen(summary_buf));
                    lvgl_dispatch_async(meeting_asr_stage_async_cb, (void *)(intptr_t)70);
                    /* 复制到全局缓冲区，供 done_cb 在设备端显示 */
                    strncpy(s_meeting_summary, summary_buf, sizeof(s_meeting_summary) - 1);
                    s_meeting_summary[sizeof(s_meeting_summary) - 1] = '\0';
                } else {
                    syslog(LOG_WARNING, "[meeting_page] LLM summary failed (err=%d), "
                           "saving transcription only\n", llm_ret);
                    summary_buf[0] = '\0';
                    s_meeting_summary[0] = '\0';
                }
            } else {
                summary_buf[0] = '\0';
            }

            /* 合并纪要+会议时间+转写，动态分配避免溢出 */
            if (summary_buf[0]) {
                /* 生成会议时间字符串（本地时间） */
                struct tm mt = agent_localtime();
                char meeting_time_str[32] = {0};
                strftime(meeting_time_str, sizeof(meeting_time_str),
                         "%Y-%m-%d %H:%M", &mt);

                size_t merged_len = strlen(summary_buf)
                                  + strlen(s_full_transcription)
                                  + strlen(meeting_time_str)
                                  + 128;
                merged = malloc(merged_len);
                if (merged) {
                    const char *title_topic, *label_time, *title_transcript;
                    if (current_lang == LANG_EN) {
                        title_topic = "Content Topic and Summary";
                        label_time = "Meeting Time:";
                        title_transcript = "Full Transcription";
                    } else {
                        title_topic = "会议主题和内容总结";
                        label_time = "会议时间：";
                        title_transcript = "完整转录内容";
                    }
                    snprintf(merged, merged_len,
                             "%s\n\n"
                             "%s %s\n\n"
                             "%s\n\n"
                             "%s\n\n"
                             "%s",
                             title_topic,
                             label_time, meeting_time_str,
                             summary_buf,
                             title_transcript,
                             s_full_transcription);
                    final_content = merged;
                } else {
                    final_content = s_full_transcription;
                }
            } else {
                final_content = s_full_transcription;
            }
        } else {
            /* malloc 失败，降级为仅保存转写 */
            syslog(LOG_WARNING, "[meeting_page] summary_buf malloc failed, skip LLM\n");
            final_content = s_full_transcription;
        }

        s_feishu_save_ok = (meeting_save_to_feishu(final_content, meeting_start_time) == 0);
        lvgl_dispatch_async(meeting_asr_stage_async_cb, (void *)(intptr_t)90);

        if (merged) free(merged);
        if (summary_buf) free(summary_buf);
    }
    /* 最后派发完成回调：进度条100%+隐藏+显示状态文本（含飞书结果） */
    lvgl_dispatch_async(meeting_asr_done_async_cb, NULL);
    /* rec2 的关闭由本线程负责：meeting_recorder_close 在线程运行中
     * 不会碰 rec2（避免 use-after-close），此处统一收尾 */
#ifdef CONFIG_MEDIA
    if (meeting_asr_recorder_handle != NULL) {
        media_recorder_close(meeting_asr_recorder_handle);
        meeting_asr_recorder_handle = NULL;
    }
#endif
    meeting_stream_asr_thread_running = 0;
    syslog(LOG_INFO, "[meeting_page] ASR stream thread exited\n");
    return NULL;
}
static void meeting_end_btn_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    /* 防止重复点击：仅当既不在录制、ASR线程也不在运行时忽略。
     * 不能只看ASR线程：若线程已提前死亡（如rec2无数据静默超时），
     * 仍须允许停止录音释放资源，否则meeting_recording永远为true，
     * 左滑也被阻止，界面永久卡死 */
    if (!meeting_recording && !meeting_stream_asr_thread_running) {
        return;
    }
    if (!meeting_stream_asr_thread_running) {
        /* ASR线程已死但录音还在：快速停止路径。不进入"生成中"流程
         * （没有线程会派发done_cb，进度条会永远停在0%）。
         * 此时done_cb已显示过文案，停止后左滑即恢复可用 */
        meeting_page_stop();
        return;
    }
    /* 立即切换停止按钮为 off 状态（仅点击停止本身才变 off） */
    if (stop_btn_img) lv_img_set_src(stop_btn_img, &meeting_stop_off);
    if (pause_btn) lv_obj_add_state(pause_btn, LV_STATE_DISABLED);
    /* 播放按钮点击提示音（非阻塞：会议页 VAD 已停，走 detached 线程同步播放） */
    wakeup_detector_request_prompt("/emmc/xiaoqxiaoq/button_click.wav");
    meeting_page_update_status(i18n_get(STR_MEET_GENERATING));
    /* 先显示进度条并初始化为0，再进入可能阻塞的 stop()。
     * 必须在 stop() 之前设置 UI，并强制 LVGL 立即渲染，
     * 否则 stop() 内的 sync()/concat() 会阻塞 UI 线程 3-6s，
     * 用户点击后看到画面冻结无反馈。 */
    if (meeting_progress_bar) {
        lv_obj_clear_flag(meeting_progress_bar, LV_OBJ_FLAG_HIDDEN);
        lv_bar_set_value(meeting_progress_bar, 0, LV_ANIM_OFF);
        if (meeting_asr_recorder_handle != NULL) {
            /* 流式模式：收尾等待期（finish-task+末句补全）即开始缓慢爬升 */
            meeting_progress_cap = 0;
            meeting_progress_stage(0);
        }
    }
    lv_refr_now(NULL);  /* 强制立即渲染：按钮变白+进度条+状态文字 */
    /* 设置 stop_flag 通知流式ASR线程退出：先置位再调用 stop()，
     * stop() 内会调用 media_recorder_stop/rec2，ASR线程检测到stop_flag
     * 后会退出循环并发送 finish-task，随后进入后处理（LLM归纳+飞书保存）。 */
    meeting_asr_stop_flag = 1;
    meeting_page_stop();
}
/* 防抖锁：防止动画期间重复触发右滑手势 */
static bool meeting_gesture_locked = false;
static void meeting_gesture_unlock_cb(lv_timer_t *t)
{
    lv_timer_del(t);
    meeting_gesture_locked = false;
}
static void meeting_gesture_cb(lv_event_t *e)
{
    /* 录制中/暂停中/转录中禁止右滑返回，避免打断会议流程。
     * 弹出“请先停止录制”提示（1s 后恢复原状态文案），toast 防抖避免连续刷新。 */
    if (meeting_recording || meeting_stream_asr_thread_running) {
        meeting_show_toast();
        return;
    }
    if (meeting_gesture_locked) return;  /* 动画期间忽略重复手势 */
    lv_indev_t *indev = lv_indev_active();
    lv_dir_t dir = lv_indev_get_gesture_dir(indev);
    if (dir == LV_DIR_RIGHT) {
        meeting_gesture_locked = true;  /* 锁定，防止重复触发 */
        meeting_page_stop();
        /* 等待手指释放后再处理，防止动画期间手指抬起的 CLICKED 事件
         * 误触发菜单页按钮。比 lv_indev_reset 副作用小，不会干扰屏幕加载动画。 */
        lv_indev_wait_release(indev);
        if (menu_screen == NULL) {
            menu_screen = lv_obj_create(NULL);
            create_menu_page(menu_screen);
        }
        lv_scr_load_anim(menu_screen, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 300, 0, false);
        /* 350ms后解锁（略长于动画时间300ms，确保安全） */
        lv_timer_t *unlock_timer = lv_timer_create(meeting_gesture_unlock_cb, 350, NULL);
        lv_timer_set_repeat_count(unlock_timer, 1);
    }
}
static void meeting_screen_unloaded_cb(lv_event_t *e);
static int meeting_recorder_open(void)
{
#ifdef CONFIG_MEDIA
    media_policy_set_mic_mute(1);
    int vol_min = 0, vol_max = 10;
    media_policy_get_range(MEDIA_SCENARIO_RECORD MEDIA_POLICY_VOLUME,
                           &vol_min, &vol_max);
    media_policy_set_stream_volume(MEDIA_SCENARIO_RECORD, vol_max);
    media_policy_include("SelCap", "mic1", 1);
    meeting_recorder_handle = media_recorder_open(MEDIA_SOURCE_MIC);
    if (meeting_recorder_handle == NULL) {
        syslog(LOG_ERR, "[meeting_page] media_recorder_open error\n");
        media_policy_exclude("SelCap", "mic1", 1);
        return -1;
    }
    /* 方案B：打开第二个 recorder 实例用于流式 ASR（path=NULL，纯 PCM 读取）。
     * meeting_dual_recorder_test.c 验证过双实例可行。 */
    meeting_asr_recorder_handle = media_recorder_open(MEDIA_SOURCE_MIC);
    if (meeting_asr_recorder_handle == NULL) {
        syslog(LOG_WARNING, "[meeting_page] asr_recorder_open failed, fallback to file mode\n");
        /* 不致命：rec1 仍可用，降级回原有文件转写路径 */
    }
#endif
    return 0;
}
static int meeting_recorder_close(void)
{
#ifdef CONFIG_MEDIA
    if (meeting_recorder_handle == NULL) return -1;
    int ret = media_recorder_close(meeting_recorder_handle);
    if (ret != 0) {
        syslog(LOG_ERR, "[meeting_page] media_recorder_close error %d\n", ret);
    }
    meeting_recorder_handle = NULL;
    /* 关闭 ASR recorder：仅当流式ASR线程已退出时才由本函数关闭；
     * 线程运行中由线程退出前自行关闭，避免 read_data 与 close 竞态
     * （use-after-close 可能崩溃） */
    if (meeting_asr_recorder_handle != NULL
        && !meeting_stream_asr_thread_running) {
        media_recorder_close(meeting_asr_recorder_handle);
        meeting_asr_recorder_handle = NULL;
    }
    media_policy_exclude("SelCap", "mic1", 1);
#endif
    return 0;
}
static void meeting_recorder_release(void)
{
#ifdef CONFIG_MEDIA
    if (meeting_recorder_handle != NULL) {
        media_recorder_stop(meeting_recorder_handle);
        media_recorder_close(meeting_recorder_handle);
        meeting_recorder_handle = NULL;
    }
    if (meeting_asr_recorder_handle != NULL
        && !meeting_stream_asr_thread_running) {
        media_recorder_close(meeting_asr_recorder_handle);
        meeting_asr_recorder_handle = NULL;
    }
    media_policy_exclude("SelCap", "mic1", 1);
#endif
}
static int meeting_recorder_prepare(const char *path)
{
#ifdef CONFIG_MEDIA
    if (meeting_recorder_handle == NULL) return -1;
    int ret = media_recorder_prepare(meeting_recorder_handle, path,
        "mux=[keys=wav],fmt=[rate=#16000,ch=#1,bits=#16,width=#2],enc=[keys=pcm,imin=#1920]");
    if (ret != 0) {
        syslog(LOG_ERR, "[meeting_page] media_recorder_prepare error %d\n", ret);
        return -1;
    }
    /* rec2 同步 prepare：path=NULL 不写文件，仅作为 PCM 读取源 */
    if (meeting_asr_recorder_handle != NULL) {
        int r2 = media_recorder_prepare(meeting_asr_recorder_handle, NULL,
            "fmt=[rate=#16000,ch=#1,bits=#16,width=#2],enc=[keys=pcm,imin=#1920]");
        if (r2 != 0) {
            syslog(LOG_WARNING, "[meeting_page] asr_recorder_prepare error %d, fallback to file mode\n", r2);
            /* 失败必须关闭句柄并置NULL：否则ASR线程走流式路径却读不到
             * 任何数据，5s静默超时后提前退出，无字幕+误显示"转写完成" */
            media_recorder_close(meeting_asr_recorder_handle);
            meeting_asr_recorder_handle = NULL;
        }
    }
#endif
    return 0;
}
static int meeting_recorder_start(void)
{
#ifdef CONFIG_MEDIA
    if (meeting_recorder_handle == NULL) return -1;
    int ret = media_recorder_start(meeting_recorder_handle);
    if (ret != 0) {
        syslog(LOG_ERR, "[meeting_page] media_recorder_start error %d\n", ret);
        return -1;
    }
    if (af_stream_set_chan_vol) {
        af_stream_set_chan_vol(MEETING_BES_AUD_ID_0, MEETING_BES_AUD_CAPTURE,
                               MEETING_BES_AUD_CH0, MEETING_BES_MIC_GAIN_MAX);
    }
    /* rec2 同步 start */
    if (meeting_asr_recorder_handle != NULL) {
        if (media_recorder_start(meeting_asr_recorder_handle) != 0) {
            syslog(LOG_WARNING, "[meeting_page] asr_recorder_start failed, fallback to file mode\n");
            /* 同prepare失败：必须关闭句柄置NULL走文件降级，
             * 否则流式线程read_data永远无数据→5s超时→界面卡死 */
            media_recorder_close(meeting_asr_recorder_handle);
            meeting_asr_recorder_handle = NULL;
        }
    }
#endif
    return 0;
}
static int meeting_recorder_stop(void)
{
#ifdef CONFIG_MEDIA
    if (meeting_recorder_handle == NULL) return -1;
    int ret = media_recorder_stop(meeting_recorder_handle);
    if (ret != 0) {
        syslog(LOG_ERR, "[meeting_page] media_recorder_stop error %d\n", ret);
        return -1;
    }
    /* rec2 同步 stop（安全：已 start 时才 stop） */
    if (meeting_asr_recorder_handle != NULL) {
        media_recorder_stop(meeting_asr_recorder_handle);
    }
#endif
    return 0;
}
static void meeting_timer_cb(lv_timer_t *timer)
{
    LV_UNUSED(timer);
    if (!meeting_active || !meeting_timer_label) {
        return;
    }
    /* 暂停期间冻结计时显示，不更新 */
    if (meeting_paused) {
        return;
    }
    /* 使用 CLOCK_MONOTONIC 计算经过时长：该时钟从系统启动起单调递增，
     * 绝不会被 NTP 或 clock_settime 修改，避免初次启动时 NTP 同步
     * 导致时钟跳变而出现计时器从几秒跳到几小时的现象。 */
    struct timespec now_mono;
    clock_gettime(CLOCK_MONOTONIC, &now_mono);
    /* 扣减累计暂停时长（同样用单调时钟计算），确保显示与实际录音时长一致 */
    time_t elapsed = now_mono.tv_sec - meeting_start_mono_ts.tv_sec - meeting_paused_total;
    if (elapsed < 0) elapsed = 0;
    int hours = (int)(elapsed / 3600);
    int mins  = (int)((elapsed % 3600) / 60);
    int secs  = (int)(elapsed % 60);
    char time_str[16];
    snprintf(time_str, sizeof(time_str), "%02d:%02d:%02d", hours, mins, secs);
    lv_label_set_text(meeting_timer_label, time_str);
}
/* Start recording. Returns 0 on success, -1 on failure. */
static int meeting_recorder_start_segment(void)
{
    char seg_path[80];
    meeting_seg_path(0, seg_path, sizeof(seg_path));
    if (meeting_recorder_open() != 0) {
        syslog(LOG_ERR, "[meeting_page] recorder open failed\n");
        return -1;
    }
    if (meeting_recorder_prepare(seg_path) != 0) {
        syslog(LOG_ERR, "[meeting_page] recorder prepare failed\n");
        meeting_recorder_release();
        return -1;
    }
    if (meeting_recorder_start() != 0) {
        syslog(LOG_ERR, "[meeting_page] recorder start failed\n");
        meeting_recorder_release();
        return -1;
    }
    meeting_seg_count = 1;  /* 首段已开始录制 */
    syslog(LOG_INFO, "[meeting_page] recording started: %s (seg0)\n", seg_path);
    return 0;
}
/* 延迟启动转录的定时器回调：让UI先刷新隐藏覆盖层，再执行阻塞的启动操作 */
static void meeting_start_timer_cb(lv_timer_t *t)
{
    lv_timer_del(t);
    meeting_page_start();
}
static void meeting_fade_out_ready_cb(lv_anim_t *a)
{
    lv_obj_t *obj = (lv_obj_t *)a->var;
    lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_opa(obj, LV_OPA_COVER, 0);
    /* 延迟50ms启动转录，让UI先刷新隐藏覆盖层，避免播放按钮叠在转录界面之上 */
    lv_timer_t *t = lv_timer_create(meeting_start_timer_cb, 50, NULL);
    lv_timer_set_repeat_count(t, 1);
}
/* 动画透明度回调包装函数：参考lvgl_ui_channel.c:808-813的成熟模式 */
static void meeting_anim_opa_cb(void *obj, int32_t v)
{
    lv_obj_set_style_opa((lv_obj_t *)obj, (lv_opa_t)v, 0);
}
/* 等待开始界面：点击播放按钮后隐藏覆盖层并启动会议录制 */
static void meeting_play_btn_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    if (meeting_idle_container) {
        /* 使用淡出动画过渡，避免瞬间切换显得突兀 */
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, meeting_idle_container);
        lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_TRANSP);
        lv_anim_set_time(&a, 300);
        lv_anim_set_exec_cb(&a, meeting_anim_opa_cb);
        lv_anim_set_ready_cb(&a, meeting_fade_out_ready_cb);
        lv_anim_start(&a);
    } else {
        meeting_page_start();
    }
}
/* 创建等待开始覆盖层：覆盖在录制UI之上，包含标题"会议模式"和大圆形播放按钮。
 * 覆盖层尺寸240x320，通过负偏移(-60,-25)抵消父容器的padding，覆盖整个屏幕。 */
static void meeting_create_idle_ui(lv_obj_t *parent)
{
    meeting_idle_container = lv_obj_create(parent);
    lv_obj_remove_style_all(meeting_idle_container);
    /* 使用LV_PCT(100)填满parent内容区，背景色与parent一致(0x000000)，视觉上无缝融合 */
    lv_obj_set_size(meeting_idle_container, LV_PCT(100), LV_PCT(100));
    lv_obj_align(meeting_idle_container, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(meeting_idle_container, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(meeting_idle_container, LV_OPA_COVER, 0);
    lv_obj_clear_flag(meeting_idle_container, LV_OBJ_FLAG_SCROLLABLE);
    /* 标题"会议模式"：顶部居中，y=80留出顶部空间，使用24px大字体 */
    lv_obj_t *idle_title = lv_label_create(meeting_idle_container);
    lv_label_set_text(idle_title, i18n_get(STR_MEET_TITLE_IDLE));
    lv_obj_set_style_text_color(idle_title, lv_color_hex(0xFFFFFF), 0);
#if LV_USE_FREETYPE
    if (!meeting_title_font) {
        meeting_title_font = lv_freetype_font_create(MEETING_FONT_PATH,
            LV_FREETYPE_FONT_RENDER_MODE_BITMAP, 28,
            LV_FREETYPE_FONT_STYLE_NORMAL);
        if (meeting_title_font) {
            lv_obj_set_style_text_font(idle_title, meeting_title_font, 0);
        } else {
            meeting_apply_font(idle_title);  /* 回退到默认字体 */
        }
    } else {
        lv_obj_set_style_text_font(idle_title, meeting_title_font, 0);
    }
#else
    meeting_apply_font(idle_title);
#endif
    lv_obj_align(idle_title, LV_ALIGN_TOP_MID, 0, 15);
    /* 播放按钮：直接使用外部下载的128x128图标（已包含圆形边框+▶符号） */
    lv_obj_t *play_icon = lv_img_create(meeting_idle_container);
    lv_img_set_src(play_icon, &meeting_play);
    lv_obj_align(play_icon, LV_ALIGN_CENTER, 0, 15);
    lv_obj_add_flag(play_icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(play_icon, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(play_icon, meeting_play_btn_cb, LV_EVENT_CLICKED, NULL);
}
/* 预热线程：后台建立 ASR TLS+WS 连接，用户点开始时直接复用，
 * 消除连接建立期间（数秒）音频管道积压导致的并头内容丢失 */
static void *meeting_asr_warm_thread(void *arg)
{
    LV_UNUSED(arg);
    meeting_asr_prepare_conn();
    return NULL;
}

void create_meeting_page(lv_obj_t *parent)
{
#if LV_USE_FREETYPE
    if (!meeting_cjk_font) {
        meeting_cjk_font = lv_freetype_font_create(MEETING_FONT_PATH,
            LV_FREETYPE_FONT_RENDER_MODE_BITMAP, MEETING_FONT_SIZE,
            LV_FREETYPE_FONT_STYLE_NORMAL);
        if (!meeting_cjk_font) {
            syslog(LOG_WARNING, "[meeting_page] FreeType font load failed: %s\n",
                   MEETING_FONT_PATH);
        }
    }
#endif
    /* 外部背景：深蓝紫（与idle_container一致，保持界面颜色统一） */
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
    /* 标题栏：透明背景 + 底部细线分隔（黑白大气风格） */
    lv_obj_t *title_bar = lv_obj_create(parent);
    lv_obj_remove_style_all(title_bar);
    lv_obj_set_size(title_bar, LV_PCT(100), 36);
    lv_obj_align(title_bar, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_opa(title_bar, LV_OPA_TRANSP, 0);  /* 透明背景 */
    lv_obj_set_style_border_width(title_bar, 0, 0);  /* 无边框，去掉白线 */
    lv_obj_set_style_pad_all(title_bar, 3, 0);
    lv_obj_clear_flag(title_bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(title_bar, LV_OBJ_FLAG_CLICKABLE);
    meeting_title_label = lv_label_create(title_bar);
    lv_label_set_text(meeting_title_label, i18n_get(STR_MEET_TITLE));
    lv_obj_set_style_text_color(meeting_title_label, lv_color_hex(0xFFFFFF), 0);
#if LV_USE_FREETYPE
    if (!meeting_title_font) {
        meeting_title_font = lv_freetype_font_create(MEETING_FONT_PATH,
            LV_FREETYPE_FONT_RENDER_MODE_BITMAP, 28,
            LV_FREETYPE_FONT_STYLE_NORMAL);
    }
    if (meeting_title_font) {
        lv_obj_set_style_text_font(meeting_title_label, meeting_title_font, 0);
    } else {
        meeting_apply_font(meeting_title_label);
    }
#else
    meeting_apply_font(meeting_title_label);
#endif
    lv_obj_align(meeting_title_label, LV_ALIGN_CENTER, 0, 0);
    meeting_timer_label = lv_label_create(parent);
    lv_label_set_text(meeting_timer_label, "00:00:00");
    lv_obj_set_style_text_color(meeting_timer_label, lv_color_hex(0xFFFFFF), 0);  /* 白色计时器 */
    meeting_apply_font(meeting_timer_label);
    lv_obj_align(meeting_timer_label, LV_ALIGN_TOP_MID, 0, 42);
    /* 字幕区：纯黑背景（与外部一致）+无边框，让转写文本直接生成在背景上 */
    meeting_chat_area = lv_obj_create(parent);
    lv_obj_set_size(meeting_chat_area, LV_PCT(100), LV_PCT(38));
    lv_obj_align(meeting_chat_area, LV_ALIGN_TOP_MID, 0, 68);
    lv_obj_set_style_bg_color(meeting_chat_area, lv_color_hex(0x000000), 0);  /* 纯黑背景，与外部一致 */
    lv_obj_set_style_bg_opa(meeting_chat_area, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(meeting_chat_area, 0, 0);  /* 移除边框 */
    lv_obj_set_style_border_opa(meeting_chat_area, LV_OPA_TRANSP, 0);
    lv_obj_set_style_radius(meeting_chat_area, 0, 0);  /* 去除圆角 */
    lv_obj_set_flex_flow(meeting_chat_area, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(meeting_chat_area,
        LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(meeting_chat_area, 6, 0);
    lv_obj_set_style_pad_all(meeting_chat_area, 8, 0);
    lv_obj_set_scroll_dir(meeting_chat_area, LV_DIR_VER);
    meeting_status_label = lv_label_create(parent);
    lv_label_set_text(meeting_status_label, i18n_get(STR_MEET_RECORDING));
    lv_obj_set_style_text_color(meeting_status_label, lv_color_hex(0xFFFFFF), 0);  /* 白色文字 */
    meeting_apply_font(meeting_status_label);
    lv_obj_align(meeting_status_label, LV_ALIGN_BOTTOM_MID, 0, -82);
    /* 进度条：叠加在字幕区底部，初始隐藏。转写时显示，完成后隐藏 */
    meeting_progress_bar = lv_bar_create(parent);
    lv_obj_set_size(meeting_progress_bar, LV_PCT(80), 8);
    lv_obj_align(meeting_progress_bar, LV_ALIGN_BOTTOM_MID, 0, -115);
    lv_bar_set_range(meeting_progress_bar, 0, 100);
    lv_bar_set_value(meeting_progress_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(meeting_progress_bar, lv_color_hex(0x444466), LV_PART_MAIN);   /* 灰色背景 */
    lv_obj_set_style_bg_opa(meeting_progress_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(meeting_progress_bar, lv_color_hex(0xFFFFFF), LV_PART_INDICATOR);  /* 白色指示 */
    lv_obj_set_style_bg_opa(meeting_progress_bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_add_flag(meeting_progress_bar, LV_OBJ_FLAG_HIDDEN);
    /* 双按钮居中布局：[⏸ 暂停] [■ 停止]
     * 横向排列，间距50px，浮动定位在底部。idle覆盖层（黑色不透明）会自然遮住按钮，
     * 覆盖层淡出后显现，无需显式隐藏。 */
    lv_obj_t *btn_container = lv_obj_create(parent);
    lv_obj_remove_style_all(btn_container);
    lv_obj_set_size(btn_container, 178, 76);   /* 64+50+64=178 */
    lv_obj_align(btn_container, LV_ALIGN_BOTTOM_MID, 0, -6);
    lv_obj_set_style_bg_opa(btn_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(btn_container, 0, 0);
    lv_obj_set_style_pad_column(btn_container, 50, 0);  /* 按钮间距50px */
    lv_obj_set_flex_flow(btn_container, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn_container,
        LV_FLEX_ALIGN_CENTER,
        LV_FLEX_ALIGN_CENTER,
        LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(btn_container, LV_OBJ_FLAG_FLOATING);
    lv_obj_clear_flag(btn_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(btn_container, LV_OBJ_FLAG_CLICKABLE);
    /* 暂停/恢复按钮（左侧）：点击 toggle 暂停状态，图标在 pause/resume 间切换 */
    pause_btn = lv_obj_create(btn_container);
    lv_obj_remove_style_all(pause_btn);
    lv_obj_add_flag(pause_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(pause_btn, 64, 64);
    lv_obj_set_style_bg_opa(pause_btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_radius(pause_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(pause_btn, 0, 0);
    lv_obj_clear_flag(pause_btn, LV_OBJ_FLAG_SCROLLABLE);
    pause_btn_img = lv_img_create(pause_btn);
    lv_img_set_src(pause_btn_img, &meeting_pause);  /* 初始为暂停图标 */
    lv_obj_center(pause_btn_img);
    lv_obj_add_event_cb(pause_btn, meeting_pause_btn_cb, LV_EVENT_CLICKED, NULL);
    /* 停止按钮（右侧）：图标在 stop_on/stop_off 间切换 */
    stop_btn = lv_obj_create(btn_container);
    lv_obj_remove_style_all(stop_btn);
    lv_obj_add_flag(stop_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(stop_btn, 64, 64);
    lv_obj_set_style_bg_opa(stop_btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_radius(stop_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(stop_btn, 0, 0);
    lv_obj_clear_flag(stop_btn, LV_OBJ_FLAG_SCROLLABLE);
    stop_btn_img = lv_img_create(stop_btn);
    lv_img_set_src(stop_btn_img, &meeting_stop_on);  /* 初始为停止(on)图标 */
    lv_obj_center(stop_btn_img);
    lv_obj_add_event_cb(stop_btn, meeting_end_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(parent, meeting_gesture_cb, LV_EVENT_GESTURE, NULL);
    /* 创建等待开始覆盖层（替代原自动开始逻辑）：
     * 用户点击播放按钮后才调用meeting_page_start()启动录制 */
    meeting_create_idle_ui(parent);
    lv_obj_add_event_cb(parent, meeting_screen_unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);
    /* 后台预热 ASR 连接：用户阅读等待界面期间完成 TLS+WS 握手 */
    {
        pthread_t warm_tid;
        pthread_attr_t warm_attr;
        pthread_attr_init(&warm_attr);
        pthread_attr_setstacksize(&warm_attr, 24 * 1024);
        pthread_attr_setdetachstate(&warm_attr, PTHREAD_CREATE_DETACHED);
        if (pthread_create(&warm_tid, &warm_attr, meeting_asr_warm_thread, NULL) == 0) {
            syslog(LOG_INFO, "[meeting_page] ASR warm conn thread started\n");
        }
        pthread_attr_destroy(&warm_attr);
    }
}
void meeting_page_start(void)
{
    /* Stop AI assistant FIRST to interrupt any in-progress TTS/LLM dialog.
     * This must happen before any UI/timer setup so the meeting page appears
     * instantly without waiting for the AI reply to finish. */
    voice_assistant_stop();
    wakeup_detector_stop();
    /* 等音频通道异步释放完成：log证实stop后立即开recorder会概率性
     * 底层graph启动失败（smf start_wait error/chan_vol busy）且API层
     * 吞错返回成功，导致rec1/rec2均无数据。牺牲500ms启动速度换稳定 */
    usleep(500 * 1000);
    /* 重置转写缓冲区：防止上次会议残留内容混入本次飞书文档 */
    s_full_transcription[0] = '\0';
    s_meeting_summary[0] = '\0';  /* 清空上次会议纪要 */
    meeting_asr_aborted = false;  /* 新会议：清除中断标记 */
    meeting_active = true;
    meeting_start_time = time(NULL);  /* 用于飞书文档时间戳（墙上时钟） */
    clock_gettime(CLOCK_MONOTONIC, &meeting_start_mono_ts);  /* 用于计时器显示（单调时钟） */
    /* 重置暂停状态：每次开始录制时清零累计暂停时长 */
    meeting_paused = false;
    meeting_pause_start_mono = 0;
    meeting_paused_total = 0;
    if (meeting_timer_label) {
        lv_label_set_text(meeting_timer_label, "00:00:00");
    }
    /* 设置按钮初始状态：暂停按钮=pause图标，停止按钮=stop_on图标，均启用 */
    if (pause_btn_img) lv_img_set_src(pause_btn_img, &meeting_pause);
    if (stop_btn_img) lv_img_set_src(stop_btn_img, &meeting_stop_on);
    if (pause_btn) lv_obj_clear_state(pause_btn, LV_STATE_DISABLED);
    if (stop_btn) lv_obj_clear_state(stop_btn, LV_STATE_DISABLED);
    if (meeting_status_label) {
        lv_label_set_text(meeting_status_label, i18n_get(STR_MEET_RECORDING_PROMPT));
        lv_obj_set_style_text_color(meeting_status_label,
            lv_color_hex(0x00C853), 0);
    }
    /* 重置进度条：隐藏并归零，防止上次状态残留 */
    if (meeting_progress_timer) {
        lv_timer_del(meeting_progress_timer);
        meeting_progress_timer = NULL;
    }
    meeting_progress_cap = 0;
    if (meeting_progress_bar) {
        lv_obj_add_flag(meeting_progress_bar, LV_OBJ_FLAG_HIDDEN);
        lv_bar_set_value(meeting_progress_bar, 0, LV_ANIM_OFF);
    }

    mkdir("/emmc/audio", 0777);
    /* Generate single recording path: meeting_YYYYMMDD_HHMMSS.wav */
    struct tm time_info = agent_localtime();
    char time_str[20];
    strftime(time_str, sizeof(time_str), "%Y%m%d_%H%M%S", &time_info);
    snprintf(meeting_recorder_base, sizeof(meeting_recorder_base),
             "/emmc/audio/meeting_%s", time_str);
    snprintf(meeting_recorder_path, sizeof(meeting_recorder_path),
             "%s.wav", meeting_recorder_base);
    meeting_seg_count = 0;  /* 重置段计数（首次进入或重新进入页面时） */
    if (meeting_recorder_start_segment() != 0) {
        syslog(LOG_ERR, "[meeting_page] recording start failed, abort\n");
        return;
    }
    meeting_recording = true;
    /* Start UI timer ONLY after recording successfully starts, so the
     * displayed duration matches the actual recording duration. */
    if (!meeting_timer) {
        meeting_timer = lv_timer_create(meeting_timer_cb, 1000, NULL);
    }
    syslog(LOG_INFO, "[meeting_page] meeting started: %s\n", meeting_recorder_path);

    /* 方案B：启动流式ASR线程（边录边转）。
     * 录音过程中从 rec2 读 PCM 送云端，识别结果实时显示为字幕。
     * meeting_end_btn_cb 仅置位 stop_flag，本线程退出后自行做后处理。 */
    pthread_attr_t asr_attr;
    pthread_attr_init(&asr_attr);
    pthread_attr_setstacksize(&asr_attr, 32 * 1024);
    pthread_attr_setdetachstate(&asr_attr, PTHREAD_CREATE_DETACHED);
    /* 先清零 stop_flag 再创建线程，避免"创建后极快点结束"的竞态：
     * 若在线程内清零，可能把 end_btn_cb 已置位的 1 清掉导致永不退出 */
    meeting_asr_stop_flag = 0;
    if (pthread_create(&meeting_stream_asr_thread, &asr_attr,
                       meeting_asr_thread_func, NULL) == 0) {
        meeting_stream_asr_thread_running = 1;
        syslog(LOG_INFO, "[meeting_page] stream ASR thread created (detached)\n");
    } else {
        syslog(LOG_ERR, "[meeting_page] stream ASR thread create failed\n");
    }
    pthread_attr_destroy(&asr_attr);
}
void meeting_page_stop(void)
{
    meeting_active = false;
    /* 兕底：通知流式ASR线程退出（正常路径 end_btn_cb 已提前置位，
     * 重复置位无害；语言切换等异常路径靠此处，
     * 避免线程等5s静默超时才退出） */
    meeting_asr_stop_flag = 1;
    if (meeting_timer) {
        lv_timer_del(meeting_timer);
        meeting_timer = NULL;
    }
    if (meeting_recording) {
        /* 暂停状态下当前段已在 pause_btn_cb 中 stop（生成合法 WAV），无需重复 stop；
         * 非暂停状态下 stop 当前段（最后一段）以 finalize WAV */
        if (!meeting_paused) {
            meeting_recorder_stop();
        }
        meeting_recorder_close();
        meeting_recording = false;
        meeting_paused = false;
        /* 等待 SMF 框架完成 AutoSink 析构：stop/close 返回 0 后，
         * WAV 写入器仍在异步销毁中（约 30-40ms），需等其回写 data chunk
         * size 后才能正确解析最后一段的头部。否则 concat 会跳过最后一段。 */
        usleep(150 * 1000);
        sync();
        /* 统一走 concat：无论单段还是多段，都经 DC 阻断+fade-in 消除开头 pop。 */
        if (meeting_seg_count >= 1) {
            if (meeting_wav_concat(meeting_seg_count, meeting_recorder_path) == 0) {
                meeting_cleanup_segments(meeting_seg_count);
                syslog(LOG_INFO, "[meeting_page] concatenated %d segments -> %s\n",
                       meeting_seg_count, meeting_recorder_path);
            } else {
                /* 兜底：拼接失败时用 seg0 作为最终文件，保证 ASR 有文件可转写 */
                syslog(LOG_ERR, "[meeting_page] concat failed, fallback to seg0\n");
                char seg0_path[80];
                meeting_seg_path(0, seg0_path, sizeof(seg0_path));
                rename(seg0_path, meeting_recorder_path);
            }
        }
        meeting_seg_count = 0;
        syslog(LOG_INFO, "[meeting_page] recording stopped: %s\n",
               meeting_recorder_path);
    }
    /* 停止后切换图标：停止按钮→off，禁用两个按钮避免转写期间误触 */
    if (stop_btn_img) lv_img_set_src(stop_btn_img, &meeting_stop_off);
    if (pause_btn) lv_obj_add_state(pause_btn, LV_STATE_DISABLED);
    if (stop_btn) lv_obj_add_state(stop_btn, LV_STATE_DISABLED);
}
void meeting_page_add_transcript(const char *speaker, const char *text)
{
    if (!meeting_chat_area || !speaker || !text) {
        return;
    }
    lv_obj_t *label = lv_label_create(meeting_chat_area);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(label, LV_PCT(100));
    meeting_apply_font(label);
    if (strcmp(speaker, "系统") == 0) {
        /* 系统提示消息：灰色弱化，无前缀 */
        lv_label_set_text(label, text);
        lv_obj_set_style_text_color(label, lv_color_hex(0x888899), 0);
    } else {
        /* 说话人消息：浅色显示，保留前缀 */
        char msg[MEETING_MSG_MAX];
        snprintf(msg, sizeof(msg), "[%s]: %s", speaker, text);
        lv_label_set_text(label, msg);
        lv_obj_set_style_text_color(label, lv_color_hex(0xE0E0E0), 0);
    }
    /* 新增系统/说话人消息后，重置 ASR 流式状态，下一轮转写另起新行 */
    current_asr_label = NULL;
    current_asr_text[0] = '\0';
    current_asr_sid = -1;
    lv_obj_scroll_to_y(meeting_chat_area, LV_COORD_MAX, LV_ANIM_OFF);
}
/* C：连接建立期间的占位提示（灰色）。ASR线程判定连接非立即可用时
 * 异步派发创建；首条真实字幕到达时自动清除，不进 s_full_transcription */
static lv_obj_t *conn_hint_label = NULL;

static void meeting_conn_hint_async_cb(void *unused)
{
    (void)unused;
    if (!meeting_chat_area || conn_hint_label) {
        return;
    }
    conn_hint_label = lv_label_create(meeting_chat_area);
    lv_label_set_text(conn_hint_label, i18n_get(STR_MEET_CONN_HINT));
    lv_label_set_long_mode(conn_hint_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(conn_hint_label, LV_PCT(100));
    lv_obj_set_style_text_color(conn_hint_label, lv_color_hex(0x9E9E9E), 0);
    meeting_apply_font(conn_hint_label);
    lv_obj_scroll_to_y(meeting_chat_area, LV_COORD_MAX, LV_ANIM_OFF);
}

void meeting_page_update_asr_transcript(const char *text, int sentence_id)
{
    if (!meeting_chat_area || !text) {
        return;
    }
    /* 智能跟随滚动（聊天软件标准行为）：仅当用户位于底部时才自动滚底。
     * 必须在更新 label 前检测——新建行会立即改变滚动几何。
     * 修复：字幕区仅容约1-2行，每句新建行+无条件滚底导致旧行滚出
     * 可视区，用户感知为“累计1-2行后被清空、最终只剩最后一句”。
     * 用户上翻回看历史时不再被打断，滚回底部后恢复自动跟随。 */
    bool at_bottom = (lv_obj_get_scroll_bottom(meeting_chat_area) <= 40);
    /* 首条字幕到达：清除"连接中"占位提示 */
    if (conn_hint_label) {
        lv_obj_del(conn_hint_label);
        conn_hint_label = NULL;
    }
    /* 同句判定以云端 sentence_id 为准：同 sid = 同句累积（覆盖同一行），
     * 不同 sid = 新句（新建行）。长度比较仅在 sid 缺失时兜底——
     * 补发场景各句以完整长度突发到达，等长短句（如五言诗每句5字、    * "好的""明白"）会被长度比较误判为同句而互相覆盖；sid 判定无此
     * 问题，且天然覆盖 ASR 修正前文导致的文本变长/变短 */
    int is_continuation = 0;
    if (current_asr_label && current_asr_text[0]) {
        if (sentence_id >= 0 && current_asr_sid >= 0) {
            is_continuation = (sentence_id == current_asr_sid);
        } else {
            is_continuation = (strlen(text) >= strlen(current_asr_text));
        }
    }
    char msg[MEETING_MSG_MAX];
    snprintf(msg, sizeof(msg), "%s", text);
    if (is_continuation && current_asr_label) {
        lv_label_set_text(current_asr_label, msg);
    } else {
        current_asr_label = lv_label_create(meeting_chat_area);
        lv_label_set_text(current_asr_label, msg);
        lv_label_set_long_mode(current_asr_label, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(current_asr_label, LV_PCT(100));
        lv_obj_set_style_text_color(current_asr_label, lv_color_hex(0xFFFFFF), 0);
        meeting_apply_font(current_asr_label);
    }
    /* 累积完整转写文本：当新句子开始时(!is_continuation)，先保存上一句的
     * 最终版本（current_asr_text此时还是上一句的最新累积结果），再更新
     * current_asr_text 为当前 text。这样每句话的最终版本都会被完整保存。 */
    if (!is_continuation && current_asr_text[0]) {
        size_t existing_len = strlen(s_full_transcription);
        size_t available = sizeof(s_full_transcription) - existing_len - 2;
        if (available > 0 && existing_len < sizeof(s_full_transcription) - 2) {
            if (existing_len > 0) {
                strncat(s_full_transcription, "\n", available--);
            }
            strncat(s_full_transcription, current_asr_text, available);
        }
    }
    if (strlen(text) < sizeof(current_asr_text)) {
        strcpy(current_asr_text, text);
    } else {
        strncpy(current_asr_text, text, sizeof(current_asr_text) - 1);
        current_asr_text[sizeof(current_asr_text) - 1] = '\0';
    }
    current_asr_sid = sentence_id;
    if (at_bottom) {
        lv_obj_scroll_to_y(meeting_chat_area, LV_COORD_MAX, LV_ANIM_OFF);
    }
}
void meeting_page_update_status(const char *status)
{
    if (meeting_status_label && status) {
        lv_label_set_text(meeting_status_label, status);
    }
}
void meeting_page_cleanup(void)
{
    /* 页面销毁（语言切换/应用退出）：先标记中断再stop，
     * ASR线程检测到后跳过LLM归纳+飞书保存，防止与新会话并发 */
    if (meeting_stream_asr_thread_running) {
        meeting_asr_aborted = true;
    }
    meeting_page_stop();
    /* 释放未被消费的预热连接（如用户进入会议页未点开始就退出），
     * 避免 socket/TLS 上下文泄漏 */
    meeting_asr_discard_warm_conn();
    /* Font singleton: do NOT delete here to avoid heap fragmentation.
     * Font is reused across subsequent meeting page entries. */
    /* ASR线程为detach模式，此处仅重置标志位，不join/detach */
    if (meeting_stream_asr_thread_running) {
        meeting_stream_asr_thread_running = 0;
    }
    meeting_title_label = NULL;
    meeting_timer_label = NULL;
    meeting_chat_area = NULL;
    meeting_status_label = NULL;
    if (meeting_progress_timer) {
        lv_timer_del(meeting_progress_timer);
        meeting_progress_timer = NULL;
    }
    meeting_progress_cap = 0;
    meeting_progress_bar = NULL;
    meeting_idle_container = NULL;
    conn_hint_label = NULL;
    current_asr_label = NULL;
    current_asr_text[0] = '\0';
    current_asr_sid = -1;
    pause_btn = NULL;
    pause_btn_img = NULL;
    stop_btn = NULL;
    stop_btn_img = NULL;
    meeting_paused = false;
    meeting_pause_start_mono = 0;
    meeting_paused_total = 0;
    toast_active = false;
    saved_status_text[0] = '\0';
    if (toast_timer) {
        lv_timer_del(toast_timer);
        toast_timer = NULL;
    }
}
void meeting_page_deinit(void)
{
    meeting_page_cleanup();
    if (meeting_screen) {
        lv_obj_t *tmp = meeting_screen;
        meeting_screen = NULL;
        lv_obj_del(tmp);
    }
}

bool meeting_is_active(void)
{
    return meeting_active;
}
static void meeting_deinit_async_cb(void *arg)
{
    LV_UNUSED(arg);
    meeting_page_deinit();
}
static void meeting_screen_unloaded_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    lv_indev_reset(NULL, NULL);
    /* 唤醒保障：ASR线程为detach模式（自行退出清理），且只读WAV文件不上传mic，
     * 与wakeup_detector(VAD source)无资源竞争，因此无需等待即可安全启动wakeup。
     * 若ASR仍在运行，dispatch回调内已做NULL检查，页面销毁后继续运行也安全。 */
    if (meeting_stream_asr_thread_running) {
        pthread_detach(meeting_stream_asr_thread);  /* 兜底：确保已detach */
        meeting_stream_asr_thread_running = 0;
    }

    /* 新需求：VAD 由 ai_page 生命周期控制，退出 meeting_page 不启动 VAD/云端 */
    lv_async_call(meeting_deinit_async_cb, NULL);
}