/****************************************************************************
 * apps/examples/lvgldemo/voice_assistant.c
 *
 * Voice assistant manager - uses DashScope Qwen-Omni-Realtime multimodal
 * model for simultaneous ASR + AI + TTS in a single WebSocket connection.
 *
 * Architecture:
 *   Idle mode: Cloud connection maintained (pings only, no mic audio).
 *              Local VAD continues running for wake word detection.
 *   Dialogue mode: Microphone -> Qwen-Omni-Realtime -> text + audio
 *                  Local VAD stopped, cloud mic active.
 *
 ****************************************************************************/

#include <nuttx/config.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <syslog.h>
#include <unistd.h>
#include <time.h>
#include <stdint.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <errno.h>
#include <lvgl/lvgl.h>
#include <media_recorder.h>
#include <media_policy.h>

#include "voice_assistant.h"
#include "dashscope_asr.h"
#include "ai_page.h"
#include "i18n.h"
#include "lvgldemo_common.h"
#include "lvgl_dispatch.h"
#include "cJSON.h"
#include "wakeup_detector.h"

#ifdef CONFIG_AI_AGENT_FEISHU
#include "channels/feishu_bot.h"
#endif

#ifdef CONFIG_MEDIA
#include "voice/audio_playback.h"
#include "voice/voice_channel.h"
#include "voice/ws_conn_pool.h"
#include "agent_config.h"
#include "core/message_bus.h"
#include "mbedtls/base64.h"
#endif
#include <wireless/wapi.h>

/* WiFi PS (power save) mode toggle for low-latency voice.
 * During Omni dialogue / TTS playback, PS is turned OFF to avoid AP-side
 * buffering that introduces tens to hundreds of ms downlink jitter.
 * Reference: vowlan_main.cpp does the same via bwifi_set_ps_cfg().
 * Implemented through standard NuttX wapi ioctl SIOCSIWPWSAVE, which on
 * BES platforms routes to bwifi_set_ps_cfg() (see bes_netdev_ax_driver.c).
 *
 * Unified state: PS is ON (idle) only when BOTH the Omni realtime path and
 * the PTT/TTS refcounted path (voice_channel.c) are idle. This avoids one
 * path prematurely restoring PS while the other is still active. */
#define VA_WIFI_NETCARD "wlan0"
static volatile int8_t s_wifi_ps_state = -1; /* -1=unknown, 0=off, 1=on */
static volatile int8_t s_omni_wants_ps_off = 0;
static volatile int   s_channel_wants_ps_off = 0; /* refcount from voice_channel */

/* Recompute desired PS state from both demand sources and apply if changed.
 * Called under no lock (single-threaded callers: va_set_dialog_state from
 * ws_event thread, voice_wifi_ps_set_hook from voice_channel threads).
 * The two atomics are read non-atomically as a best-effort; worst case is a
 * redundant ioctl which the coalescing guard below suppresses. */
static void va_wifi_ps_recompute(bool force_off)
{
  bool want_off = force_off || (s_omni_wants_ps_off != 0) || (s_channel_wants_ps_off > 0);
  int8_t target = want_off ? 0 : 1;
  if (s_wifi_ps_state == target) return;

  int sock = wapi_make_socket();
  if (sock < 0) {
    syslog(LOG_WARNING, "[VA] wifi_ps: socket failed\n");
    return;
  }
  int ret = wapi_set_power_save(sock, VA_WIFI_NETCARD, false); /* 始终关闭PS，避免PS ON/OFF切换导致网络抖动 */
  close(sock);
  if (ret == 0) {
    s_wifi_ps_state = target;
    syslog(LOG_INFO, "[VA] wifi PS %s (omni=%d ch=%d%s)\n",
           want_off ? "OFF (voice)" : "ON (idle)",
           (int)s_omni_wants_ps_off, s_channel_wants_ps_off,
           force_off ? " force" : "");
  } else {
    syslog(LOG_WARNING, "[VA] wifi_ps_set(%d) failed: %d\n", want_off, ret);
  }
}

/* Omni realtime path entry: set/clear Omni's demand for PS-off. */
static void va_wifi_ps_set(bool off_for_voice)
{
  int8_t want = off_for_voice ? 1 : 0;
  if (s_omni_wants_ps_off == want) return;
  s_omni_wants_ps_off = want;
  va_wifi_ps_recompute(false);
}

/* Strong override of the weak hook in voice_channel.c.
 * voice_channel.c calls voice_wifi_ps_set_hook(1) when its refcount goes
 * 0->1 (acquire) and (0) when it goes 1->0 (release). We translate these
 * into the unified demand counter. */
void voice_wifi_ps_set_hook(int off)
{
  /* off==1: a channel session wants PS off (refcount 0->1)
   * off==0: channel session released (refcount 1->0) */
  s_channel_wants_ps_off = off ? 1 : 0;
  va_wifi_ps_recompute(false);
}

#ifdef CONFIG_MEDIA

extern uint32_t af_stream_set_chan_vol(uint32_t id, uint32_t stream,
                                       uint32_t ch_map, uint8_t vol)
                                       __attribute__((weak));

#define BES_AUD_STREAM_ID_0     0
#define BES_AUD_STREAM_CAPTURE  1
#define BES_AUD_CHANNEL_MAP_CH0 1
#define BES_MIC_GAIN_MAX        15

#endif

typedef void *media_recorder_handle_t;

extern lv_obj_t * ai_wakeup_label;

#define TAG "voice_assistant"

#define SAMPLE_RATE        16000
#define CHANNELS           1
#define BITS               16
#define CHUNK_SIZE         (SAMPLE_RATE / 1000 * 20 * (BITS / 8) * CHANNELS)
#define SEND_CHUNK_SIZE    (SAMPLE_RATE / 1000 * 100 * (BITS / 8) * CHANNELS)
#define WS_RECV_BUF        32768
#define PING_INTERVAL_MS   5000
#define PING_IDLE_INTERVAL_MS 2000  /* 空闲期缩短ping间隔，更快检测服务端静默断连 */
#define RECONNECT_DELAY_MS 1000
#define B64_DECODE_BUF     24576
#define RESPONSE_STALL_TIMEOUT_MS 5000

/* Silence timeout: 30 seconds of no user speech AND no AI audio */
#define SILENCE_TIMEOUT_MS 30000
/* Feishu conversation mode silence timeout: 600 seconds (10 minutes) */
#define FEISHU_CONV_SILENCE_TIMEOUT_MS 600000
/* Feishu fast path silence timeout: 30 seconds. Used after fast path
 * starts to allow time for tool execution + TTS playback. */
#define FEISHU_FAST_PATH_SILENCE_TIMEOUT_MS 30000
/* Feishu fast path hard timeout: 30 seconds.  If the query takes
 * longer than this, force-reset the fast path flag and restore mic. */
#define FEISHU_FAST_PATH_TIMEOUT_MS 30000
/* Minimum time the fast-path flag must stay active before it can be
 * released by the "TTS done" check.  The agent may need 1-2 seconds to
 * process feishu_doc_read calls before voice_channel_speak() starts,
 * during which voice_channel_is_speaking() is still false.  Without
 * this hold, the reset condition fires prematurely, re-enables mic
 * upload, and the mic picks up TTS echo causing "答非所问" replies. */
#define FEISHU_FAST_PATH_MIN_HOLD_MS 3000
#define FEISHU_FAST_PATH_TTS_STALL_MS 5000
/* PROMPT_SILENCE_TIMEOUT 已在 i18n.h 中定义为枚举值，通过 i18n_get_prompt() 获取多语言提示音路径 */

/* Idle mode loop sleep to avoid busy-waiting when not reading mic */
#define IDLE_LOOP_SLEEP_US 50000

static volatile bool s_va_running = false;
static volatile bool s_va_initialized = false;
static pthread_t s_va_thread;

static ds_asr_stream_t *s_asr_stream = NULL;

/* Network disconnect callback: called when the DashScope thread detects
 * ASR stream connection loss (ECONNRESET etc.), allowing lvgldemo to
 * trigger immediate WiFi disconnect handling without waiting for the
 * 3-second polling timer. */
static va_disconnect_cb_t s_disconnect_cb = NULL;

void voice_assistant_set_disconnect_callback(va_disconnect_cb_t cb)
{
  s_disconnect_cb = cb;
}

/* Dialogue mode state */
static volatile bool s_dialogue_active = false;
static volatile uint64_t s_last_dialogue_activity = 0;
static media_recorder_handle_t s_recorder = NULL;

/* Listening suspended flag: when true, local VAD (wake word) is kept stopped.
 * Used by the camera page to fully mute the mic during photo capture. */
static volatile bool s_listening_suspended = false;

/* Restart local VAD unless listening is suspended (e.g. camera page active).
 * All "restart VAD after dialogue/fast-path ends" paths go through here so the
 * suspend state is honored uniformly. */
static void va_maybe_start_wakeup(void)
{
  if (s_listening_suspended) {
    syslog(LOG_INFO, "[%s] VAD restart skipped (listening suspended)\n", TAG);
    return;
  }
  wakeup_detector_start();
}

/* ── 图片AI分析状态机 ─────────────────────────────────────────── */
typedef enum {
  IMG_STATE_IDLE = 0,     /* 空闲，可接受新请求 */
  IMG_STATE_QUEUED,       /* 已入队，等待主循环发送 */
  IMG_STATE_SENDING,      /* 主循环正在发送（disable_vad+PCM+图片+commit） */
  IMG_STATE_COMMITTING,   /* 已发input_audio_buffer.commit，等待committed事件 */
  IMG_STATE_WAITING,      /* 已发response.create，等待response.done */
  IMG_STATE_ERROR         /* 发送/分析失败 */
} img_state_t;

typedef struct {
  img_state_t state;
  unsigned char *data;    /* 原始JPEG字节流 */
  size_t data_len;
  char filename[64];
  pthread_mutex_t lock;
} img_ctx_t;

static img_ctx_t s_img_ctx = { .lock = PTHREAD_MUTEX_INITIALIZER };
/* 图片发送/等待期间暂停麦克风音频发送 */
static volatile bool s_img_sending = false;
static image_status_cb_t s_image_status_cb = NULL;
static va_user_asr_cb_t s_user_asr_cb = NULL;
static va_ai_reply_cb_t s_ai_reply_cb = NULL;
static va_state_cb_t s_state_cb = NULL;
static volatile va_dialog_state_t s_dialog_state = VA_STATE_IDLE;

/* 回调消息结构体 - 提前定义 */
typedef struct {
    va_state_cb_t cb;
    va_dialog_state_t state;
} va_state_msg_t;

typedef struct {
    va_user_asr_cb_t cb;
    char *text;
} va_user_asr_msg_t;

typedef struct {
    va_ai_reply_cb_t cb;
    char *text;
} va_ai_reply_msg_t;

/* dispatch 回调函数前置声明 */
static void va_state_dispatch_cb(void *arg);
static void va_user_asr_dispatch_cb(void *arg);
static void va_ai_reply_dispatch_cb(void *arg);
static void va_set_dialog_state(va_dialog_state_t state);

static void (*s_meeting_callback)(void) = NULL;

#ifdef CONFIG_AI_AGENT_FEISHU
/* 飞书文本转发状态。AI 图片分析成功后由 camera_page 开启，
 * 将 AI 回复(前缀"AI：")与用户转文字(前缀"我：")转发到飞书。
 * 生命周期：转发 5 条 AI 回复后自动关闭（=图片分析回复 + 4 轮对话回复）；
 * 下次点击 AI 分析按钮时由 camera_page 重新开启以重置计数。 */
static volatile bool s_fwd_enabled = false;
static char s_fwd_receive_id[64] = {0};
static char s_fwd_id_type[16] = {0};
static volatile int s_fwd_ai_count = 0;
static volatile bool s_fwd_img_analysis_started = false;  /* 图片分析是否已开始，只有开始后的AI回复才写入飞书文档，创建文档等工具回复过滤掉 */
static char s_last_user_question[1024] = {0};  /* 缓存上一轮用户问题，用于写入文档时添加"我：xxx"前缀 */
static volatile bool s_need_skip_doc_prefix = false;  /* 创建文档后，下一个AI回复开头需要截断创建文档相关提示语 */
static volatile bool s_doc_create_silent = false;  /* 本地创建文档时置 true，让 doc_created 回调用 silent 版设文档 */
#define FEISHU_FWD_MAX_AI_REPLIES 5

/* ── 飞书对话模式状态 ─────────────────────────────────────────
 * 用户说"开始飞书对话"后进入，持续双向对话：
 * - 用户说话 → transcript → 直接发飞书群（不走云端LLM）
 * - 飞书消息到达 → TTS播报 + UI显示（不走agent_bus/LLM）
 * 退出条件：说"退出飞书对话"或静音超时 */
static volatile bool s_feishu_conversation_mode = false;
static char s_feishu_conversation_chat_id[64] = {0};
/* ASR模式切换延迟标志：由 enter/exit 函数设置，由主循环线程执行实际切换 */
static volatile bool s_pending_asr_only_switch = false;
static volatile bool s_pending_omni_restore = false;
/* 语言切换后需刷新会话指令：由 i18n_set_lang 置位，主循环检测到后在 dashscope_thread 发 session.update */
static volatile bool s_pending_lang_refresh = false;
/* 延迟退出对话模式：exit函数设置，主循环在TTS播报完成+3s后执行实际退出 */
static volatile bool s_pending_conversation_exit = false;
static volatile uint64_t s_pending_conversation_exit_ts = 0;
/* 通用对话模式延迟退出：退出命令检测到时设置，主循环等待云端TTS播报完成
 * （s_omni_echo_active 清除）后执行最终清理。 */
static volatile bool s_pending_dialogue_exit = false;

/* 轮询补偿：每5秒轮询一次飞书消息，替代WS订阅 */
static volatile uint64_t s_last_feishu_poll_time = 0;
#define FEISHU_POLL_INTERVAL_MS  5000  /* 每5秒轮询一次 */

/* Forward declaration (defined later) */
static uint64_t get_time_ms(void);

/* 已投递消息的message_id去重环 */
#define CONV_DEDUP_SIZE 32
static char s_conv_msgid_ring[CONV_DEDUP_SIZE][64];
static int s_conv_msgid_idx = 0;

/* 设备自己发出的消息内容记录（用于轮询回显抑制） */
#define CONV_SENT_RING_SIZE 8
static char s_conv_sent_text_ring[CONV_SENT_RING_SIZE][256];
static uint64_t s_conv_sent_ts_ring[CONV_SENT_RING_SIZE];
static int s_conv_sent_idx = 0;

static void conv_record_sent(const char *text)
{
  if (!text || !text[0]) return;
  strncpy(s_conv_sent_text_ring[s_conv_sent_idx], text,
          sizeof(s_conv_sent_text_ring[0]) - 1);
  s_conv_sent_text_ring[s_conv_sent_idx][sizeof(s_conv_sent_text_ring[0]) - 1] = '\0';
  s_conv_sent_ts_ring[s_conv_sent_idx] = get_time_ms();
  s_conv_sent_idx = (s_conv_sent_idx + 1) % CONV_SENT_RING_SIZE;
}

static bool conv_is_echo(const char *text)
{
  if (!text || !text[0]) return false;
  uint64_t now = get_time_ms();
  for (int i = 0; i < CONV_SENT_RING_SIZE; i++) {
    if (s_conv_sent_text_ring[i][0] &&
        strcmp(s_conv_sent_text_ring[i], text) == 0 &&
        (now - s_conv_sent_ts_ring[i]) < 15000) {
      return true;
    }
  }
  return false;
}

static bool conv_msgid_seen(const char *msgid)
{
  if (!msgid || !msgid[0]) return false;
  for (int i = 0; i < CONV_DEDUP_SIZE; i++) {
    if (s_conv_msgid_ring[i][0] && strcmp(s_conv_msgid_ring[i], msgid) == 0) {
      return true;
    }
  }
  strncpy(s_conv_msgid_ring[s_conv_msgid_idx], msgid,
          sizeof(s_conv_msgid_ring[0]) - 1);
  s_conv_msgid_ring[s_conv_msgid_idx][sizeof(s_conv_msgid_ring[0]) - 1] = '\0';
  s_conv_msgid_idx = (s_conv_msgid_idx + 1) % CONV_DEDUP_SIZE;
  return false;
}

/* Forward declarations for TTS queue (defined later) */
#define FEISHU_TTS_QUEUE_SIZE 16
typedef struct { char text[256]; } feishu_tts_msg_t;
static feishu_tts_msg_t s_feishu_tts_queue[FEISHU_TTS_QUEUE_SIZE];
static volatile int s_feishu_tts_head = 0;
static volatile int s_feishu_tts_tail = 0;
static pthread_mutex_t s_feishu_tts_lock;
static pthread_cond_t s_feishu_tts_cond;
static volatile bool s_feishu_tts_running = false;
static volatile bool s_feishu_tts_worker_created = false;  /* worker 是否存活，stop 时重置 */
static volatile bool s_feishu_tts_va_initialized = false;
static void feishu_tts_worker_ensure_started(void);

/* ═══ 飞书轮询线程：将HTTPS请求从主线程剥离，避免阻塞mic管线 ═══ */
static pthread_t s_feishu_poll_tid;
static pthread_mutex_t s_feishu_poll_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_feishu_poll_cond = PTHREAD_COND_INITIALIZER;
static volatile bool s_feishu_poll_running = false;
static volatile bool s_feishu_poll_need_poll = false;
static volatile bool s_feishu_poll_thread_created = false;

/* 前向声明：轮询函数定义在后面 */
static void feishu_conv_poll_messages(void);

static void* feishu_poll_thread_func(void* arg)
{
  (void)arg;
  syslog(LOG_INFO, "[%s] [FEISHU_CONV] poll thread started\n", TAG);

  while (s_feishu_poll_running) {
    pthread_mutex_lock(&s_feishu_poll_mutex);
    while (s_feishu_poll_running && !s_feishu_poll_need_poll) {
      pthread_cond_wait(&s_feishu_poll_cond, &s_feishu_poll_mutex);
    }
    s_feishu_poll_need_poll = false;
    pthread_mutex_unlock(&s_feishu_poll_mutex);

    if (!s_feishu_poll_running) break;

    /* 在独立线程中执行HTTPS请求，不阻塞主线程mic数据读取 */
    feishu_conv_poll_messages();
  }

  syslog(LOG_INFO, "[%s] [FEISHU_CONV] poll thread exiting\n", TAG);
  return NULL;
}

static void feishu_poll_thread_start(void)
{
  if (s_feishu_poll_thread_created) return;

  s_feishu_poll_running = true;
  s_feishu_poll_need_poll = false;
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  pthread_attr_setstacksize(&attr, 8192);
  if (pthread_create(&s_feishu_poll_tid, &attr, feishu_poll_thread_func, NULL) == 0) {
    s_feishu_poll_thread_created = true;
  }
  pthread_attr_destroy(&attr);
}

static void feishu_poll_thread_stop(void)
{
  if (!s_feishu_poll_thread_created) return;

  s_feishu_poll_running = false;
  pthread_cond_signal(&s_feishu_poll_cond);
  s_feishu_poll_thread_created = false;
  syslog(LOG_INFO, "[%s] [FEISHU_CONV] poll thread stopped\n", TAG);
}

/* 轮询飞书消息：获取最近5秒内的消息，去重后投递给TTS+UI */
static void feishu_conv_poll_messages(void)
{
  syslog(LOG_INFO, "[%s] [FEISHU_CONV] poll_messages called, mode=%d chat_id=%.20s\n",
      TAG, s_feishu_conversation_mode, s_feishu_conversation_chat_id);

  if (!s_feishu_conversation_mode || !s_feishu_conversation_chat_id[0]) return;

  /* 计算时间范围：最近5秒（start_time/end_time 使用秒级时间戳） */
  long long now_s = (long long)time(NULL);
  long long start_s = now_s - 5;

  char path[256];
  snprintf(path, sizeof(path),
      "/open-apis/im/v1/messages?container_id_type=chat"
      "&container_id=%s&start_time=%lld&end_time=%lld"
      "&page_size=50&sort_type=ByCreateTimeAsc",
      s_feishu_conversation_chat_id, start_s, now_s);

  char *resp = malloc(4096);
  if (!resp) return;

  int status = feishu_api_request("GET", path, NULL, 0, resp, 4096);
  syslog(LOG_INFO, "[%s] [FEISHU_CONV] poll API status=%d\n", TAG, status);
  if (status != 200) {
    free(resp);
    return;
  }

  cJSON *root = cJSON_Parse(resp);
  free(resp);
  if (!root) return;

  cJSON *code = cJSON_GetObjectItem(root, "code");
  if (!cJSON_IsNumber(code) || code->valueint != 0) {
    cJSON_Delete(root);
    return;
  }

  cJSON *data = cJSON_GetObjectItem(root, "data");
  cJSON *items = data ? cJSON_GetObjectItem(data, "items") : NULL;
  if (!cJSON_IsArray(items)) {
    syslog(LOG_INFO, "[%s] [FEISHU_CONV] poll: no items array\n", TAG);
    cJSON_Delete(root);
    return;
  }

  int item_count = cJSON_GetArraySize(items);
  syslog(LOG_INFO, "[%s] [FEISHU_CONV] poll: found %d items\n", TAG, item_count);

  /* 按时间顺序遍历消息（API已按创建时间升序排序） */
  cJSON *item;
  cJSON_ArrayForEach(item, items)
  {
    /* message_id 去重 */
    cJSON *msgid_j = cJSON_GetObjectItem(item, "message_id");
    if (!cJSON_IsString(msgid_j) || conv_msgid_seen(msgid_j->valuestring)) {
      continue;
    }

    /* 只处理 text 类型 */
    cJSON *msg_type = cJSON_GetObjectItem(item, "msg_type");
    if (!cJSON_IsString(msg_type) || strcmp(msg_type->valuestring, "text") != 0) {
      continue;
    }

    /* 过滤 bot 自己的消息（API返回 sender.id + sender.id_type） */
    cJSON *sender = cJSON_GetObjectItem(item, "sender");
    if (cJSON_IsObject(sender)) {
      cJSON *id_type = cJSON_GetObjectItem(sender, "id_type");
      cJSON *sender_id_val = cJSON_GetObjectItem(sender, "id");
      /* 过滤 app (bot) 消息: sender_type=="app" 或 id_type=="app_id" */
      if (cJSON_IsString(id_type) && strcmp(id_type->valuestring, "app_id") == 0) {
        continue;
      }
    }

    /* 提取文本内容 */
    cJSON *body = cJSON_GetObjectItem(item, "body");
    if (!cJSON_IsObject(body)) continue;
    cJSON *content = cJSON_GetObjectItem(body, "content");
    if (!cJSON_IsString(content) || !content->valuestring[0]) continue;

    cJSON *cj = cJSON_Parse(content->valuestring);
    if (!cj) continue;
    cJSON *ti = cJSON_GetObjectItem(cj, "text");
    if (!cJSON_IsString(ti) || !ti->valuestring[0]) {
      cJSON_Delete(cj);
      continue;
    }

    char text[256];
    strncpy(text, ti->valuestring, sizeof(text) - 1);
    text[sizeof(text) - 1] = '\0';
    cJSON_Delete(cj);

    /* 去除 @mention 标记 */
    char *at = strstr(text, "@user_");
    if (at) {
      char *space = strchr(at, ' ');
      if (space) {
        memmove(text, space + 1, strlen(space + 1) + 1);
      }
    }

    /* 回显抑制：如果消息内容与设备最近15s内发出的消息相同，跳过 */
    if (conv_is_echo(text)) {
      syslog(LOG_INFO, "[%s] [FEISHU_CONV] poll: echo suppressed: %s\n", TAG, text);
      continue;
    }

    /* 获取发送者名称（API返回 sender.id 即 open_id） */
    char sender_name[64] = "unknown";
    if (cJSON_IsObject(sender)) {
      cJSON *sender_id_val = cJSON_GetObjectItem(sender, "id");
      cJSON *id_type = cJSON_GetObjectItem(sender, "id_type");
      if (cJSON_IsString(sender_id_val) && cJSON_IsString(id_type) &&
          strcmp(id_type->valuestring, "open_id") == 0) {
        const char *oid = sender_id_val->valuestring;
        /* 从成员缓存中按 open_id 查找真实姓名 */
        if (!feishu_recv_find_member_by_open_id(oid, sender_name, sizeof(sender_name))) {
          /* 未找到，使用 open_id 前缀作为 fallback */
          snprintf(sender_name, sizeof(sender_name), "%.8s...", oid);
        }
        syslog(LOG_INFO, "[%s] [FEISHU_CONV] poll: sender id=%s name=%s\n",
            TAG, oid, sender_name);
      }
    }

    /* 构造显示文本 */
    char display_text[256];
    if (sender_name[0] && strcmp(sender_name, "unknown") != 0) {
      snprintf(display_text, sizeof(display_text), "%s: %s", sender_name, text);
    } else {
      snprintf(display_text, sizeof(display_text), "%s", text);
    }

    syslog(LOG_INFO, "[%s] [FEISHU_CONV] poll: %s\n", TAG, display_text);

    /* UI显示：使用飞书专用样式 */
    ai_page_show_feishu_msg(display_text, sender_name);

    /* TTS播报 */
    feishu_tts_worker_ensure_started();
    pthread_mutex_lock(&s_feishu_tts_lock);
    int next_head = (s_feishu_tts_head + 1) % FEISHU_TTS_QUEUE_SIZE;
    if (next_head == s_feishu_tts_tail) {
      syslog(LOG_WARNING, "[%s] [FEISHU_CONV] TTS queue full, dropping oldest\n", TAG);
      s_feishu_tts_tail = (s_feishu_tts_tail + 1) % FEISHU_TTS_QUEUE_SIZE;
    }
    strncpy(s_feishu_tts_queue[s_feishu_tts_head].text, display_text,
            sizeof(s_feishu_tts_queue[0].text) - 1);
    s_feishu_tts_queue[s_feishu_tts_head].text[sizeof(s_feishu_tts_queue[0].text) - 1] = '\0';
    s_feishu_tts_head = next_head;
    pthread_cond_signal(&s_feishu_tts_cond);
    pthread_mutex_unlock(&s_feishu_tts_lock);
  }

  cJSON_Delete(root);
}

/* Forward declarations for feishu conversation helpers */
static bool feishu_conversation_try_mention(const char *text,
                                             char *name_out, size_t name_cap,
                                             char *msg_out, size_t msg_cap);
static void feishu_conversation_send_mention(const char *chat_id,
                                              const char *open_id,
                                              const char *name,
                                              const char *text);

typedef struct
{
  char text[1024];
  char receive_id[64];
  char id_type[16];
} fwd_arg_t;

/* 飞书文档自动写入状态。用户语音创建文档成功后由tool_feishu_doc设置，
 * 将5轮图片分析及追问结果自动写入文档，完成后自动清空。 */
static char s_active_feishu_doc_id[128] = {0};
static char s_active_feishu_doc_title[256] = {0};
static volatile int s_doc_image_count = 0;  /* 当前文档已写入的图片数量，用于生成"图片x汇总"标题 */
static pthread_mutex_t s_doc_write_lock = PTHREAD_MUTEX_INITIALIZER;

typedef struct
{
  char doc_id[128];
  char image_path[128];
  char user_question[1024];
  char *ai_text;
  int round;
} doc_write_arg_t;

/* 辅助函数：截断AI回复开头的创建文档确认前缀（"好的，正在为你创建飞书文档...请稍等。"等）
 * 返回截断后文本的起始指针 */
/* 判断文本是否为唤醒词（"小Q小Q"及其变体），避免写入飞书文档 */
static bool is_wake_word(const char *text)
{
  if (!text) return false;
  const char *p = text;
  /* 跳过开头空白 */
  while (*p == ' ') p++;
  /* 匹配 "小Q小Q" 及其常见变体 */
  if (strncmp(p, "小Q小Q", sizeof("小Q小Q")-1) == 0) return true;
  if (strncmp(p, "小Q，小Q", sizeof("小Q，小Q")-1) == 0) return true;
  if (strncmp(p, "小Q,小Q", sizeof("小Q,小Q")-1) == 0) return true;
  return false;
}

static const char *strip_doc_create_prefix(const char *text)
{
  if (!text) return text;
  const char *p = text;
  /* 跳过开头空白和标点（含中文标点，UTF-8 多字节需用 strncmp） */
  while (*p == ' ' || *p == ',' || *p == '.' || *p == '\n' ||
         strncmp(p, "，", sizeof("，")-1) == 0 ||
         strncmp(p, "。", sizeof("。")-1) == 0) {
    if (*p == ' ' || *p == ',' || *p == '.' || *p == '\n') p++;
    else if (strncmp(p, "，", sizeof("，")-1) == 0) p += sizeof("，")-1;
    else if (strncmp(p, "。", sizeof("。")-1) == 0) p += sizeof("。")-1;
  }
  
  /* 匹配常见创建文档前缀：
   * "好的，正在为你创建飞书文档\"xxx\"，请稍等。"
   * "好的，正在为你创建飞书文档"
   * "正在为你创建飞书文档..."
   * 注意：中文字符在 UTF-8 中占 3 字节，必须用 sizeof()-1 获取完整字节数，
   * 不能用硬编码数字（如 2、10），否则会截断字符导致乱码。 */
  if (strncmp(p, "好的", sizeof("好的")-1) == 0) p += sizeof("好的")-1;
  if (strncmp(p, "，", sizeof("，")-1) == 0 || strncmp(p, ",", 1) == 0) p += (strncmp(p, "，", sizeof("，")-1) == 0) ? sizeof("，")-1 : 1;

  if (strncmp(p, "正在为你创建飞书文档", sizeof("正在为你创建飞书文档")-1) == 0) {
    p += sizeof("正在为你创建飞书文档")-1;
    /* 跳过文档标题部分（带引号）直到找到句号/感叹号结尾 */
    while (*p != '\0' && *p != '.' && *p != '!' &&
           strncmp(p, "。", sizeof("。")-1) != 0 &&
           strncmp(p, "！", sizeof("！")-1) != 0) p++;
    if (*p == '.' || *p == '!') p++;
    else if (strncmp(p, "。", sizeof("。")-1) == 0) p += sizeof("。")-1;
    else if (strncmp(p, "！", sizeof("！")-1) == 0) p += sizeof("！")-1;
    /* 跳过后续空白标点 */
    while (*p == ' ' || *p == ',' || *p == '\n' ||
           strncmp(p, "，", sizeof("，")-1) == 0) {
      if (*p == ' ' || *p == ',' || *p == '\n') p++;
      else if (strncmp(p, "，", sizeof("，")-1) == 0) p += sizeof("，")-1;
    }
    syslog(LOG_INFO, "[FEISHU_DOC] Stripped doc create prefix, remaining text starts with: %.20s...\n", p);
  }
  
  /* 如果截断后为空，返回原文本避免空内容 */
  if (*p == '\0') return text;
  return p;
}
#endif

#ifdef CONFIG_MEDIA
static audio_playback_t *s_omni_pb = NULL;
static pthread_mutex_t s_omni_pb_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile bool s_omni_pb_active = false;

/* ── Omni playback 文件缓存方案 ────────────────────────────────
 * 服务器下发的所有 PCM 数据先缓存到 /emmc 临时文件，consumer 线程
 * 流式读取文件播放。彻底避免内存队列满丢包，接收线程只做磁盘写
 * （快），永不阻塞。
 *
 * 三层架构：
 *   接收线程: recv WS → base64 decode → write(file)追加 → 立即返回
 *   consumer: read(file) → audio_playback_write(可能反压，不影响接收)
 *   drain:    ring → media_player_write_data
 *
 * 文件读写指针由 mutex 保护，sem 通知 consumer 有新数据。
 * consumer 读完所有数据（含 EOF 标记后剩余）才退出，不丢数据。
 */
#define OMNI_CACHE_FILE   "/emmc/omni_audio.pcm"
#define OMNI_READ_CHUNK   8192   /* consumer 每次从文件读取 8KB */
/* 文件级预缓冲：等 emmc 文件累计达到此字节数再开始读取播放。
 * 72KB ≈ 1.5s @ 24kHz/16bit/mono，抗网络首包慢+抖动。
 * 真正的缓冲在 emmc 文件（容量无限），不占 RAM。 */
#define OMNI_FILE_PREBUFFER_BYTES    (72 * 1024)
#define OMNI_FILE_PREBUFFER_TIMEOUT_MS 3000  /* 3s 超时强制开播 */
static int s_omni_file_fd = -1;
static off_t s_omni_file_write_pos = 0;  /* 接收线程写入位置 */
static off_t s_omni_file_read_pos = 0;   /* consumer 线程读取位置 */
static pthread_mutex_t s_omni_file_mtx = PTHREAD_MUTEX_INITIALIZER;
static sem_t s_omni_file_sem;            /* 通知 consumer 有新数据 */
static volatile bool s_omni_file_eof = false;  /* 接收完成标记 */
static volatile bool s_omni_consumer_running = false;
static pthread_t s_omni_consumer_tid;

static char s_transcript_buf[2048];
static int s_transcript_len = 0;
static volatile bool s_response_active = false;
static volatile bool s_echo_suppress = false;
static volatile uint64_t s_last_audio_time = 0;
#define ECHO_SETTLE_MS 500

/* Local TTS (agent voice_channel) echo suppression.
 * When the agent speaks via voice_channel_speak() (e.g. Feishu fast path),
 * the cloud mic must not send audio, otherwise the cloud VAD picks up the
 * TTS output and triggers a self-talk loop.
 * s_tts_echo_active: true while TTS is playing + ECHO_SETTLE_MS after.
 * s_tts_echo_end_ts: when TTS stopped (0 while still playing). */
static volatile bool s_tts_echo_active = false;
static volatile uint64_t s_tts_echo_end_ts = 0;

/* Omni (cloud LLM) playback echo suppression — mirrors s_tts_echo_active:
 * while the cloud response is being played back via omni_pb, the mic stream
 * must NOT be uploaded to the server, otherwise residual TTS audio is picked
 * up by the cloud VAD/ASR and mis-recognized as user speech (self-talk loop,
 * e.g. AI says "你要注意防暑降温" and ASR picks up "你要注意").
 * s_omni_echo_active: true from response.created/delta until ECHO_SETTLE_MS
 *                    after omni_pb_close() returns (playback drained).
 * s_omni_echo_end_ts: 0 while response is active; set to get_time_ms() when
 *                     response.done finishes closing playback. Main loop polls
 *                     and clears s_omni_echo_active after the settle window. */
static volatile bool s_omni_echo_active = false;
static volatile uint64_t s_omni_echo_end_ts = 0;
#endif

/* Layer1 飞书快速路径激活时，跳过云端 omni 的响应事件，避免自问自答 */
static volatile bool s_feishu_fast_path = false;
/* Timestamp (ms) when s_feishu_fast_path was last set to true. */
static volatile uint64_t s_feishu_fast_path_start_ts = 0;
/* 标记快速路径期间TTS是否已开始播放过。
 * 用于防止在TTS还没开始播放时就提前重置 s_feishu_fast_path。 */
static volatile bool s_fast_path_tts_started = false;
/* 标记快速路径期间是否已收到agent响应内容（消息过滤结果等）。
 * 当voice_channel_is_speaking()首次为true时设置，表示agent已调用
 * voice_channel_speak()，内容已从消息过滤返回并开始TTS播放。
 * 快速路径释放必须同时满足：内容已收到 + TTS已播放完成。 */
static volatile bool s_fast_path_content_received = false;
/* 标记快速路径期间云端omni响应是否已完成（response.done已到达）。
 * 快速路径释放前必须等待云端response.done，否则释放后云端可能
 * 继续发送事件干扰用户新语音。替代之前过长的12秒MIN_HOLD等待。 */
static volatile bool s_fast_path_omni_done = false;
/* 记录快速路径期间TTS播报完成的时间戳。
 * 当 voice_channel_is_speaking() 从 true 变为 false 时设置。
 * 用于条件2：TTS完成后5秒内仍未释放则强制切换状态。 */
static volatile uint64_t s_fast_path_tts_done_ts = 0;

static uint64_t get_time_ms(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

#ifdef CONFIG_MEDIA
/* Consumer 线程：从文件读取 PCM 写入 audio_playback。
 * 此线程可能被 ring buffer 反压（usleep 5ms），但不再阻塞接收线程。
 * 退出条件：consumer_running=false 且文件已读完（read_pos>=write_pos），
 * 确保不丢数据。
 *
 * 文件级预缓冲：启动后先等 emmc 文件累计达到 OMNI_FILE_PREBUFFER_BYTES
 * （1.5s 数据）再开始读取播放，抗网络首包慢+抖动。超时 3s 强制开播。
 * 真正的缓冲在 emmc 文件（容量无限），不占 RAM ring buffer。 */
static void* omni_pb_consumer_thread(void *arg)
{
    (void)arg;
    unsigned char *read_buf = malloc(OMNI_READ_CHUNK);
    if (!read_buf) {
        syslog(LOG_ERR, "[%s] consumer malloc read_buf failed\n", TAG);
        return NULL;
    }

    bool file_prebuffer_done = false;
    long long prebuffer_start_ms = 0;

    while (1) {
        pthread_mutex_lock(&s_omni_file_mtx);
        off_t avail = s_omni_file_write_pos - s_omni_file_read_pos;
        off_t file_total = s_omni_file_write_pos;  /* 文件累计写入量 */
        bool eof = s_omni_file_eof;
        pthread_mutex_unlock(&s_omni_file_mtx);

        /* ═══ 文件级预缓冲阶段：等文件攒够 1.5s 数据再开始读取 ═══ */
        if (!file_prebuffer_done) {
            if (file_total >= OMNI_FILE_PREBUFFER_BYTES || eof) {
                file_prebuffer_done = true;
                syslog(LOG_INFO, "[%s] Omni file prebuffer done (%ld bytes)\n",
                       TAG, (long)file_total);
            } else {
                /* 检查超时 */
                struct timeval tv;
                gettimeofday(&tv, NULL);
                long long now_ms = (long long)tv.tv_sec * 1000
                    + tv.tv_usec / 1000;
                if (prebuffer_start_ms == 0) {
                    prebuffer_start_ms = now_ms;
                } else if (now_ms - prebuffer_start_ms
                           >= OMNI_FILE_PREBUFFER_TIMEOUT_MS) {
                    file_prebuffer_done = true;  /* 超时强制开播 */
                    syslog(LOG_INFO, "[%s] Omni file prebuffer timeout (%ld bytes)\n",
                           TAG, (long)file_total);
                }
                /* 等待新数据 */
                sem_wait(&s_omni_file_sem);
                continue;
            }
        }

        if (avail <= 0) {
            /* 无数据可读 */
            if (!s_omni_consumer_running && eof) {
                /* 接收完成且文件已读完，退出 */
                break;
            }
            /* 等待新数据 */
            sem_wait(&s_omni_file_sem);
            continue;
        }

        size_t to_read = (avail < (off_t)OMNI_READ_CHUNK) ? (size_t)avail : OMNI_READ_CHUNK;

        /* 从文件读取 */
        if (s_omni_file_fd >= 0) {
            lseek(s_omni_file_fd, s_omni_file_read_pos, SEEK_SET);
            ssize_t n = read(s_omni_file_fd, read_buf, to_read);
            if (n > 0) {
                s_omni_file_read_pos += n;
                /* 写入 audio_playback ring（可能反压，只阻塞 consumer） */
                pthread_mutex_lock(&s_omni_pb_lock);
                if (s_omni_pb) {
                    int wret = audio_playback_write(s_omni_pb, read_buf, (size_t)n);
                    if (wret < 0) {
                        syslog(LOG_ERR, "[%s] Omni playback write failed: %d\n", TAG, wret);
                    }
                }
                pthread_mutex_unlock(&s_omni_pb_lock);
            }
        }
    }

    free(read_buf);
    return NULL;
}

static void omni_pb_open(void)
{
    /* 若旧 consumer 仍在运行，先停止并等待其退出 */
    if (s_omni_consumer_running) {
        s_omni_consumer_running = false;
        s_omni_file_eof = true;
        sem_post(&s_omni_file_sem);
        pthread_join(s_omni_consumer_tid, NULL);
        sem_destroy(&s_omni_file_sem);
    }

    /* 打开/清空临时缓存文件 */
    if (s_omni_file_fd >= 0) {
        close(s_omni_file_fd);
    }
    s_omni_file_fd = open(OMNI_CACHE_FILE, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (s_omni_file_fd < 0) {
        syslog(LOG_ERR, "[%s] Failed to open cache file %s: %d\n",
               TAG, OMNI_CACHE_FILE, errno);
    }
    s_omni_file_write_pos = 0;
    s_omni_file_read_pos = 0;
    s_omni_file_eof = false;

    pthread_mutex_lock(&s_omni_pb_lock);
    if (s_omni_pb) {
        audio_playback_close(s_omni_pb);
        s_omni_pb = NULL;
    }

    s_omni_pb = audio_playback_open(AGENT_AUDIO_PLAYBACK_DEV,
                                     AGENT_DASHSCOPE_OMNI_SAMPLE_RATE,
                                     AGENT_VOICE_CHANNELS,
                                     AGENT_VOICE_BITS);
    if (!s_omni_pb) {
        syslog(LOG_ERR, "[%s] Failed to open Omni audio playback\n", TAG);
    } else {
        s_omni_pb_active = true;
        syslog(LOG_INFO, "[%s] Omni audio playback opened (24kHz)\n", TAG);
        /* 启动 consumer 线程 */
        sem_init(&s_omni_file_sem, 0, 0);
        s_omni_consumer_running = true;
        pthread_create(&s_omni_consumer_tid, NULL,
                       omni_pb_consumer_thread, NULL);
    }
    pthread_mutex_unlock(&s_omni_pb_lock);
}

static void omni_pb_close(void)
{
    /* 标记接收完成，让 consumer 读完文件剩余数据后退出 */
    s_omni_file_eof = true;
    sem_post(&s_omni_file_sem);

    /* 等 consumer 读完所有数据并退出（不丢数据） */
    if (s_omni_consumer_running) {
        s_omni_consumer_running = false;
        sem_post(&s_omni_file_sem);
        pthread_join(s_omni_consumer_tid, NULL);
        sem_destroy(&s_omni_file_sem);
    }

    /* consumer 已退出，安全关闭 playback（会 drain ring buffer） */
    pthread_mutex_lock(&s_omni_pb_lock);
    s_omni_pb_active = false;
    if (s_omni_pb) {
        audio_playback_close(s_omni_pb);
        s_omni_pb = NULL;
    }
    pthread_mutex_unlock(&s_omni_pb_lock);

    /* 关闭并删除临时文件 */
    if (s_omni_file_fd >= 0) {
        close(s_omni_file_fd);
        s_omni_file_fd = -1;
        unlink(OMNI_CACHE_FILE);
    }
}

/* 接收线程调用：将 PCM 追加写入缓存文件，不阻塞。
 * pcm 由调用方分配，此函数内 free。 */
static void omni_pb_enqueue(unsigned char *pcm, size_t len)
{
    if (!pcm || len == 0 || s_omni_file_fd < 0) {
        free(pcm);
        return;
    }

    /* 追加写入文件（循环写完，防止部分写） */
    size_t written = 0;
    while (written < len) {
        pthread_mutex_lock(&s_omni_file_mtx);
        lseek(s_omni_file_fd, s_omni_file_write_pos, SEEK_SET);
        ssize_t n = write(s_omni_file_fd, pcm + written, len - written);
        if (n <= 0) {
            pthread_mutex_unlock(&s_omni_file_mtx);
            syslog(LOG_ERR, "[%s] cache file write failed: %d (written=%zu/%zu)\n",
                   TAG, errno, written, len);
            break;
        }
        s_omni_file_write_pos += n;
        pthread_mutex_unlock(&s_omni_file_mtx);
        written += (size_t)n;
    }

    /* 通知 consumer 有新数据 */
    sem_post(&s_omni_file_sem);
    free(pcm);
}
#endif

/* ── 图片AI分析：状态回调通知（线程安全，通过lv_async投递） ──────── */
typedef struct {
  char status[64];
  bool done;
  image_status_cb_t cb;
} img_status_msg_t;

static void img_status_async_cb(void *data)
{
  img_status_msg_t *msg = (img_status_msg_t *)data;
  if (msg && msg->cb) {
    msg->cb(msg->status, msg->done);
  }
  free(msg);
}

static void img_notify_status(const char *status, bool done)
{
  syslog(LOG_INFO, "[AI_IMG] status update: %s (done=%d)\n", status, done);
  if (!s_image_status_cb) return;
  img_status_msg_t *msg = malloc(sizeof(img_status_msg_t));
  if (!msg) return;
  strncpy(msg->status, status, sizeof(msg->status) - 1);
  msg->status[sizeof(msg->status) - 1] = '\0';
  msg->done = done;
  msg->cb = s_image_status_cb;
  if (!lvgl_dispatch_async(img_status_async_cb, msg)) {
    free(msg);
  }
}

/* 图片AI分析：发送静音PCM + 图片 + 对话项 + response.create
 * 在 dashscope_thread 主循环中调用，期间 s_img_sending=true 暂停音频发送 */
static int do_image_send_and_commit(void)
{
  int ret;

  syslog(LOG_INFO, "[AI_IMG] ===== do_image_send_and_commit start (Manual mode) =====\n");

  /* Step 0: 禁用VAD，切换到Manual模式（关键！避免VAD自动管理冲突） */
  syslog(LOG_INFO, "[AI_IMG] step 0/4: disabling VAD (switch to manual mode)...\n");
  ret = dashscope_asr_stream_disable_vad(s_asr_stream);
  if (ret != 0) {
    syslog(LOG_ERR, "[AI_IMG] step 0/4 failed: disable VAD error (%d)\n", ret);
    return ret;
  }
  syslog(LOG_INFO, "[AI_IMG] step 0/4 OK: VAD disabled\n");

  /* Step 1: 发送静音PCM（前置条件：input_image_buffer.append前至少发过一次audio） */
  syslog(LOG_INFO, "[AI_IMG] step 1/4: sending silent PCM (100ms)...\n");
  ret = dashscope_asr_stream_send_silent(s_asr_stream, 100);
  if (ret != 0) {
    syslog(LOG_ERR, "[AI_IMG] step 1/4 failed: send silent PCM error (%d)\n", ret);
    return ret;
  }
  syslog(LOG_INFO, "[AI_IMG] step 1/4 OK: silent PCM sent\n");

  /* Step 2: 发送图片到缓冲区（input_image_buffer.append） */
  syslog(LOG_INFO, "[AI_IMG] step 2/4: sending image data (%zu bytes)...\n", s_img_ctx.data_len);
  ret = dashscope_asr_stream_send_image(s_asr_stream,
    s_img_ctx.data, s_img_ctx.data_len);
  if (ret != 0) {
    syslog(LOG_ERR, "[AI_IMG] step 2/4 failed: send image error (%d)\n", ret);
    return ret;
  }
  syslog(LOG_INFO, "[AI_IMG] step 2/4 OK: image data sent to buffer\n");

  /* Step 3: 提交音频+图片缓冲区（input_audio_buffer.commit）
   * 注意：不存在 input_image_buffer.commit！
   * input_audio_buffer.commit 同时提交音频缓冲区和图像缓冲区 */
  syslog(LOG_INFO, "[AI_IMG] step 3/4: sending input_audio_buffer.commit...\n");
  ret = dashscope_asr_stream_commit_buffer(s_asr_stream);
  if (ret != 0) {
    syslog(LOG_ERR, "[AI_IMG] step 3/4 failed: commit buffer error (%d)\n", ret);
    return ret;
  }
  syslog(LOG_INFO, "[AI_IMG] step 3/4 OK: buffer committed, waiting for input_audio_buffer.committed...\n");

  syslog(LOG_INFO, "[AI_IMG] ===== do_image_send_and_commit DONE, waiting for committed event =====\n");
  return 0;
}

static void dialogue_close_mic(media_recorder_handle_t *recorder);
#ifdef CONFIG_AI_AGENT_FEISHU
/* 转发线程：阻塞式发送飞书文本，避免拖慢 WS 接收线程。
 * 失败仅记日志，不影响本地 AI 对话与语音播放。 */
static void *feishu_fwd_thread(void *arg)
{
  fwd_arg_t *fa = (fwd_arg_t *)arg;
  int ret = feishu_send_text_message_ex(fa->receive_id, fa->text, fa->id_type);
  if (ret != 0) {
    syslog(LOG_WARNING, "[%s] feishu fwd FAILED: %d\n", TAG, ret);
  } else {
    syslog(LOG_INFO, "[%s] feishu fwd OK: %.80s\n", TAG, fa->text);
  }
  free(fa);
  return NULL;
}

/* 在 WS 接收线程调用，必须非阻塞：拷贝文本并 spawn detached 线程。 */
static void feishu_fwd_dispatch(const char *prefix, const char *content)
{
  if (!s_fwd_enabled || !content || content[0] == '\0') return;
  if (s_fwd_receive_id[0] == '\0') return;

  fwd_arg_t *fa = malloc(sizeof(fwd_arg_t));
  if (!fa) return;
  snprintf(fa->text, sizeof(fa->text), "%s%s", prefix, content);
  fa->text[sizeof(fa->text) - 1] = '\0';
  strncpy(fa->receive_id, s_fwd_receive_id, sizeof(fa->receive_id) - 1);
  fa->receive_id[sizeof(fa->receive_id) - 1] = '\0';
  strncpy(fa->id_type, s_fwd_id_type, sizeof(fa->id_type) - 1);
  fa->id_type[sizeof(fa->id_type) - 1] = '\0';

  pthread_t tid;
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, 64 * 1024);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  if (pthread_create(&tid, &attr, feishu_fwd_thread, fa) != 0) {
    free(fa);
  }
  pthread_attr_destroy(&attr);
}

/* 对外接口：查询是否有活跃的飞书文档 */
bool voice_assistant_has_active_feishu_doc(void)
{
#ifdef CONFIG_AI_AGENT_FEISHU
  return (s_active_feishu_doc_id[0] != '\0');
#else
  return false;
#endif
}

/* 对外接口：仅关闭飞书聊天转发，不影响文档写入状态 */
void voice_assistant_disable_feishu_forward(void)
{
#ifdef CONFIG_AI_AGENT_FEISHU
  if (s_fwd_enabled) {
    syslog(LOG_INFO, "[%s] feishu forward DISABLED (doc mode, keep img_analysis_started=%d)\n",
           TAG, s_fwd_img_analysis_started);
  }
  s_fwd_enabled = false;
#endif
}

/* ── 飞书文档自动写入：异步线程 ──────────────────────────────
 * 第1轮：先写图片，再写文字，加分隔线
 * 第2~5轮：追加文字，最后一轮写完后清空活跃文档 */
static void *feishu_doc_write_thread(void *arg)
{
  doc_write_arg_t *a = (doc_write_arg_t *)arg;
  int ret;
  uint64_t start_ms = get_time_ms();

  syslog(LOG_INFO, "[FEISHU_DOC] Writing round %d to doc %s\n", a->round, a->doc_id);

  /* 第1轮写入图片（先写标题，再写图片） */
  if (a->round == 1 && a->image_path[0] != '\0') {
    /* 写入标题块：图片x汇总 */
    s_doc_image_count++;
    char heading_title[64];
    snprintf(heading_title, sizeof(heading_title), "图片%d汇总", s_doc_image_count);
    syslog(LOG_INFO, "[FEISHU_DOC] Appending heading: %s\n", heading_title);
    ret = feishu_doc_append_heading1(a->doc_id, heading_title);
    if (ret != OK) {
      syslog(LOG_ERR, "[FEISHU_DOC] Failed to append heading, ret=%d (continuing)\n", ret);
    }

    syslog(LOG_INFO, "[FEISHU_DOC] Appending image: %s\n", a->image_path);
    ret = feishu_doc_append_image(a->doc_id, a->image_path);
    if (ret != OK) {
      syslog(LOG_ERR, "[FEISHU_DOC] Failed to append image, ret=%d (continuing with text)\n", ret);
    }
  }

  /* 按照聊天格式排版：先写用户问题"我：xxx"换行，再写AI回复"AI：xxx"，和飞书聊天格式一致 */
  const char *clean_ai_text = strip_doc_create_prefix(a->ai_text);
  
  size_t user_q_len = strlen(a->user_question);
  size_t ai_text_len = strlen(clean_ai_text);
  const char *user_prefix = "我：";
  size_t user_prefix_len = strlen(user_prefix);
  const char *ai_prefix = "AI：";
  size_t ai_prefix_len = strlen(ai_prefix);
  const char *newline = "\n";
  size_t newline_len = strlen(newline);
  
  char *full_content = NULL;
  if (user_q_len > 0) {
    /* 有用户问题：我：xxx + 换行 + AI：xxx */
    full_content = malloc(user_prefix_len + user_q_len + newline_len + ai_prefix_len + ai_text_len + 1);
    if (full_content) {
      char *p = full_content;
      memcpy(p, user_prefix, user_prefix_len); p += user_prefix_len;
      memcpy(p, a->user_question, user_q_len); p += user_q_len;
      memcpy(p, newline, newline_len); p += newline_len;
      memcpy(p, ai_prefix, ai_prefix_len); p += ai_prefix_len;
      memcpy(p, clean_ai_text, ai_text_len); p += ai_text_len;
      *p = '\0';
    }
  } else {
    /* 无用户问题（第1轮拍照触发没有语音问题）：直接 AI：xxx */
    full_content = malloc(ai_prefix_len + ai_text_len + 1);
    if (full_content) {
      char *p = full_content;
      memcpy(p, ai_prefix, ai_prefix_len); p += ai_prefix_len;
      memcpy(p, clean_ai_text, ai_text_len); p += ai_text_len;
      *p = '\0';
    }
  }

  if (full_content) {
    ret = feishu_doc_append_text(a->doc_id, full_content);
    if (ret != OK) {
      syslog(LOG_ERR, "[FEISHU_DOC] Failed to append text, ret=%d\n", ret);
    } else {
      syslog(LOG_INFO, "[FEISHU_DOC] Round %d text appended successfully (user question: %s)\n",
             a->round, user_q_len > 0 ? "yes" : "no");
    }
    free(full_content);
  }

  /* 追加分割线（非最后一轮时） */
  if (a->round < FEISHU_FWD_MAX_AI_REPLIES) {
    feishu_doc_append_divider(a->doc_id);
  }

  /* 第5轮（单张图片的5轮问答）完成，重置计数标志，等待下一次拍照继续追加
   * 注意：不要清空活跃文档ID，创建文档后允许多次拍照分析持续追加到同一个文档，
   * 直到用户创建新文档时才会覆盖活跃文档ID */
  if (a->round >= FEISHU_FWD_MAX_AI_REPLIES) {
    s_fwd_img_analysis_started = false;
    s_last_user_question[0] = '\0';
    syslog(LOG_INFO, "[FEISHU_DOC] One image analysis round (%d replies) completed, waiting for next image... (active doc remains: %s) took %llums\n",
           FEISHU_FWD_MAX_AI_REPLIES, a->doc_id,
           (unsigned long long)(get_time_ms() - start_ms));
  }

  free(a->ai_text);
  free(a);
  return NULL;
}

/* 分发文档写入任务（在WS接收线程调用，非阻塞） */
static void feishu_doc_write_dispatch(const char *image_path, const char *full_text, const char *user_question)
{
  syslog(LOG_DEBUG, "[FEISHU_DOC] dispatch called: image_path=%s text_len=%zu active_doc=%s user_q=%s\n",
         image_path ? image_path : "(null)", full_text ? strlen(full_text) : 0,
         s_active_feishu_doc_id[0] ? s_active_feishu_doc_id : "(none)",
         user_question ? user_question : "(none)");

  if (s_active_feishu_doc_id[0] == '\0') {
    syslog(LOG_DEBUG, "[FEISHU_DOC] No active document, skipping write\n");
    return;
  }
  if (!full_text || full_text[0] == '\0') {
    syslog(LOG_DEBUG, "[FEISHU_DOC] Empty text, skipping write\n");
    return;
  }

  int round = s_fwd_ai_count;
  syslog(LOG_DEBUG, "[FEISHU_DOC] Current fwd_ai_count = %d\n", round);
  if (round < 1 || round > FEISHU_FWD_MAX_AI_REPLIES) {
    syslog(LOG_DEBUG, "[FEISHU_DOC] Round %d out of range [1,%d], skipping\n", round, FEISHU_FWD_MAX_AI_REPLIES);
    return;
  }

  pthread_mutex_lock(&s_doc_write_lock);
  char doc_id[sizeof(s_active_feishu_doc_id)];
  strncpy(doc_id, s_active_feishu_doc_id, sizeof(doc_id)-1);
  doc_id[sizeof(doc_id)-1] = '\0';
  pthread_mutex_unlock(&s_doc_write_lock);

  doc_write_arg_t *arg = malloc(sizeof(doc_write_arg_t));
  if (!arg) {
    syslog(LOG_ERR, "[FEISHU_DOC] OOM for write arg\n");
    return;
  }
  memset(arg, 0, sizeof(*arg));
  strncpy(arg->doc_id, doc_id, sizeof(arg->doc_id)-1);
  /* 截断创建文档前缀后再拷贝 */
  const char *clean_text = strip_doc_create_prefix(full_text);
  arg->ai_text = strdup(clean_text);
  if (!arg->ai_text) {
    syslog(LOG_ERR, "[FEISHU_DOC] OOM for ai_text strdup\n");
    free(arg);
    return;
  }
  arg->round = round;

  /* 保存用户问题 */
  if (user_question && user_question[0] != '\0') {
    strncpy(arg->user_question, user_question, sizeof(arg->user_question)-1);
  }

  /* 只有第1轮有图片，此时s_img_ctx.filename仍有效 */
  if (round == 1 && image_path && image_path[0] != '\0') {
    strncpy(arg->image_path, image_path, sizeof(arg->image_path)-1);
    syslog(LOG_DEBUG, "[FEISHU_DOC] Round 1 will include image: %s\n", image_path);
  } else {
    syslog(LOG_DEBUG, "[FEISHU_DOC] Round %d no image\n", round);
  }

  pthread_t tid;
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, 64 * 1024);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  int ret = pthread_create(&tid, &attr, feishu_doc_write_thread, arg);
  pthread_attr_destroy(&attr);
  if (ret != 0) {
    syslog(LOG_ERR, "[FEISHU_DOC] Failed to create write thread: %d (errno=%d)\n", ret, errno);
    free(arg->ai_text);
    free(arg);
  } else {
    syslog(LOG_INFO, "[FEISHU_DOC] Started write thread for round %d, tid=%lu\n", round, (unsigned long)tid);
  }
}
#endif /* CONFIG_AI_AGENT_FEISHU */

/* 通用对话模式退出清理：静音超时退出和延迟退出共用。
 * 清除对话状态标志、关闭mic、关闭omni播放、清除回声标志、重启VAD。
 * 调用者负责额外操作（如飞书对话退出、播放提示音等）。 */
static void va_dialogue_exit_cleanup(void)
{
  s_dialogue_active = false;
  s_last_dialogue_activity = 0;
  s_fast_path_tts_started = false;
  s_fast_path_content_received = false;
  s_fast_path_omni_done = false;
  s_fast_path_tts_done_ts = 0;

  /* Close mic */
  if (s_recorder) {
    dialogue_close_mic(&s_recorder);
  }

#ifdef CONFIG_MEDIA
  omni_pb_close();
  s_response_active = false;
  s_echo_suppress = false;
  s_omni_echo_active = false;
  s_omni_echo_end_ts = 0;
  s_tts_echo_active = false;
  s_tts_echo_end_ts = 0;
  va_set_dialog_state(VA_STATE_IDLE);
#endif

  /* VAD 启动和状态更新由调用方负责：
   * 静音超时场景需要先播放提示音（do_play_prompt 会关闭 VAD recorder），
   * 必须等提示音播完后才能启动 VAD，否则 recorder 被意外关闭导致 VAD 失效。
   * 退出命令场景无提示音，可直接启动 VAD。 */
}

/* 查询当前是否禁止 ai_page 右滑退出。
 * 4种场景：
 * 1. 普通对话：AI回复中（s_response_active 或 s_omni_echo_active）
 * 2. 今日消息：飞书快速路径激活（消息归纳中）
 * 3. 飞书查询：飞书快速路径激活（文档查询/搜索中）
 * 4. 飞书对话：飞书对话模式 + TTS播报中
 * 场景2&3共用 s_feishu_fast_path 标志（涵盖所有飞书快速路径操作）
 * 场景1额外检查 s_omni_echo_active：response.done 先置 s_response_active=false
 * 再调用 omni_pb_close() drain 音频（阻塞），drain 期间 s_response_active 已 false
 * 但 omni 音频仍在播放，s_omni_echo_active 仍为 true，覆盖整个播放+回声消散窗口 */
bool voice_assistant_is_exit_blocked(void)
{
  /* 场景2&3: 飞书快速路径激活（今日消息归纳/飞书文档查询/搜索）
   * s_feishu_fast_path 在所有编译配置下均定义，无 CONFIG_AI_AGENT_FEISHU 时恒为 false
   * 注意：TTS 播报完成时会立即清除 s_feishu_fast_path（见主循环 echo 跟踪），
   * 不等待 omni_done，避免网络断连时右滑锁/VAD 长时间卡住。 */
  if (s_feishu_fast_path) {
    return true;
  }

  /* 场景1: 普通对话 - AI回复中（云端响应活跃 或 omni音频播放中）
   * s_response_active: response.created→response.done 期间为 true
   * s_omni_echo_active: response.created→omni_pb_close() drain 完成+ECHO_SETTLE_MS
   * 后者覆盖 response.done 后 omni_pb_close() 阻塞 drain 音频的窗口 */
#ifdef CONFIG_MEDIA
  if (s_response_active || s_omni_echo_active) {
    return true;
  }
#endif

  /* 场景4: 飞书对话模式 + TTS播报中
   * voice_assistant_is_feishu_conversation_active() 在所有配置下可用（无飞书时返回 false）
   * voice_channel_is_speaking() 需要 CONFIG_MEDIA（voice_channel.h 在此条件下引入） */
  if (voice_assistant_is_feishu_conversation_active()) {
#ifdef CONFIG_MEDIA
    if (voice_channel_is_speaking()) {
      return true;
    }
#endif
  }

  return false;
}

/* 飞书文档创建回调：ai_agent库通知文档创建成功，这里设置活跃文档 */
static void feishu_doc_created_callback(const char *doc_id, const char *title, void *user_data)
{
  (void)user_data;
  /* 静默模式（本地代码直接创建文档）：用 silent 版，不置 s_need_skip_doc_prefix。
   * 语音创建文档场景默认走原版，保留截断"好的，正在创建文档..."前缀的能力。 */
  if (s_doc_create_silent) {
    voice_assistant_set_active_feishu_doc_silent(doc_id, title);
  } else {
    voice_assistant_set_active_feishu_doc(doc_id, title);
  }
}

/* 对外接口：设置当前活跃飞书文档 */
void voice_assistant_set_active_feishu_doc(const char *doc_id, const char *title)
{
#ifdef CONFIG_AI_AGENT_FEISHU
  pthread_mutex_lock(&s_doc_write_lock);
  if (doc_id && doc_id[0] != '\0') {
    strncpy(s_active_feishu_doc_id, doc_id, sizeof(s_active_feishu_doc_id)-1);
    s_active_feishu_doc_id[sizeof(s_active_feishu_doc_id)-1] = '\0';
    if (title) {
      strncpy(s_active_feishu_doc_title, title, sizeof(s_active_feishu_doc_title)-1);
      s_active_feishu_doc_title[sizeof(s_active_feishu_doc_title)-1] = '\0';
    } else {
      s_active_feishu_doc_title[0] = '\0';
    }
    syslog(LOG_INFO, "[FEISHU_DOC] Active doc set: id=%s title=%s\n",
           s_active_feishu_doc_id, s_active_feishu_doc_title);
    s_doc_image_count = 0;  /* 新文档重置图片计数 */
    s_need_skip_doc_prefix = true;  /* 本地创建文档后，云端LLM返回会包含创建文档确认语，下一个AI回复需要截断前缀 */
    s_last_user_question[0] = '\0';  /* 清空用户问题缓存 */
  } else {
    s_active_feishu_doc_id[0] = '\0';
    s_active_feishu_doc_title[0] = '\0';
    s_doc_image_count = 0;
    s_need_skip_doc_prefix = false;
    s_last_user_question[0] = '\0';
    syslog(LOG_INFO, "[FEISHU_DOC] Active doc cleared\n");
  }
  pthread_mutex_unlock(&s_doc_write_lock);
#else
  (void)doc_id;
  (void)title;
#endif
}

/* 静默版：本地代码直接创建文档后调用，无 LLM 介入，不置 s_need_skip_doc_prefix。
 * doc_id 为空则等价于清空活跃文档（与 set_active_feishu_doc(NULL,NULL) 一致）。 */
void voice_assistant_set_active_feishu_doc_silent(const char *doc_id, const char *title)
{
#ifdef CONFIG_AI_AGENT_FEISHU
  pthread_mutex_lock(&s_doc_write_lock);
  if (doc_id && doc_id[0] != '\0') {
    strncpy(s_active_feishu_doc_id, doc_id, sizeof(s_active_feishu_doc_id)-1);
    s_active_feishu_doc_id[sizeof(s_active_feishu_doc_id)-1] = '\0';
    if (title) {
      strncpy(s_active_feishu_doc_title, title, sizeof(s_active_feishu_doc_title)-1);
      s_active_feishu_doc_title[sizeof(s_active_feishu_doc_title)-1] = '\0';
    } else {
      s_active_feishu_doc_title[0] = '\0';
    }
    syslog(LOG_INFO, "[FEISHU_DOC] Active doc set (silent): id=%s title=%s\n",
           s_active_feishu_doc_id, s_active_feishu_doc_title);
    s_doc_image_count = 0;  /* 新文档重置图片计数 */
    /* 不动 s_need_skip_doc_prefix / s_last_user_question —— 本地创建无前缀需截 */
  } else {
    s_active_feishu_doc_id[0] = '\0';
    s_active_feishu_doc_title[0] = '\0';
    s_doc_image_count = 0;
    s_need_skip_doc_prefix = false;
    s_last_user_question[0] = '\0';
    syslog(LOG_INFO, "[FEISHU_DOC] Active doc cleared (silent)\n");
  }
  pthread_mutex_unlock(&s_doc_write_lock);
#else
  (void)doc_id;
  (void)title;
#endif
}

/* 设置文档创建回调的静默模式（见头文件说明） */
void voice_assistant_set_doc_create_silent(bool silent)
{
  s_doc_create_silent = silent;
  syslog(LOG_INFO, "[FEISHU_DOC] doc create silent mode = %d\n", silent ? 1 : 0);
}

/* ── 对话退出意图模糊识别 ──────────────────────────────────────
 * 识别用户想结束对话的各种口语化表达，覆盖 ASR 识别误差。
 * 分两类关键词：
 *   1. 直接退出词：退下/退出/退一下/结束对话/不聊了 等
 *   2. 告别/休息词：再见/拜拜/休息吧/你可以休息了 等
 * 使用 strstr 子串匹配，兼容用户带唤醒词前缀的说法（如“小Q，退下！”）。
 */
static bool va_is_exit_intent(const char *text)
{
  if (!text || !text[0]) return false;

  /* 中文退出关键词 */
  static const char *exit_kw[] = {
    /* 核心退出词 */
    "退下", "退出", "退一下", "退了吧",
    /* 结束对话 */
    "结束对话", "结束聊天", "不聊了", "结束吧",
    /* 不需要了 */
    "不用了", "不需要了", "没事了", "没什么了",
    /* 告别 */
    "再见", "拜拜",
    /* 休息 */
    "休息吧", "去休息吧", "你可以休息了", "你可以退下了",
    /* 就这样 */
    "先这样吧", "就这样吧", "没有其他了", "没有别的了",
    NULL
  };
  for (int i = 0; exit_kw[i]; i++) {
    if (strstr(text, exit_kw[i])) return true;
  }

  /* 英文退出关键词 */
  if (strcasestr(text, "stop talking") ||
      strcasestr(text, "goodbye") ||
      strcasestr(text, "bye bye") ||
      strcasestr(text, " bye")) {
    return true;
  }

  return false;
}

/* 模糊匹配 "chat mode" 及其常见 ASR 近音词组合。
 * 云端 ASR 常将短词 "chat" /tʃæt/ 误识别为发音相近的词，
 * 此函数检测 "chat mode" 或其近音词组合，需与动作词组合使用以避免误触发。
 * 注意：仅选择子串冲突少的近音词，避免 at/get/let 等会匹配大量普通词的短词。 */
static bool fuzzy_match_chat_mode(const char *text)
{
  if (!text) return false;
  if (!strcasestr(text, "mode")) return false;
  if (strcasestr(text, "chat")) return true;    /* /tʃæt/ 正确 */
  /* ASR 近音词（按音标相似度排列） */
  if (strcasestr(text, "check")) return true;   /* /tʃɛk/ 元音相近 */
  if (strcasestr(text, "catch")) return true;   /* /kætʃ/ 辅音元音倒置 */
  if (strcasestr(text, "chet")) return true;    /* /tʃɛt/ 元音相近 */
  if (strcasestr(text, "chart")) return true;   /* /tʃɑrt/ 多 r 音 */
  if (strcasestr(text, "charter")) return true; /* /tʃɑrtər/ chart + er */
  if (strcasestr(text, "chad")) return true;    /* /tʃæd/ 尾辅音不同 */
  if (strcasestr(text, "that")) return true;    /* /ðæt/ 元音相同 */
  if (strcasestr(text, "jet")) return true;     /* /dʒɛt/ dʒ 代替 tʃ */
  if (strcasestr(text, "shut")) return true;    /* /ʃʌt/ ʃ 代替 tʃ */
  if (strcasestr(text, "shot")) return true;    /* /ʃɒt/ ʃ 代替 tʃ */
  return false;
}

/* 飞书对话模式退出意图：模糊匹配飞书专属退出短语。
 * 覆盖 ASR 识别误差和口语化表达（如"退出书飞对话""结束飞"等）。
 * 分两类匹配：
 *   1. 动作词+对象词组合：退出/结束/关闭/离开 + 飞书对话/飞书聊天/飞书
 *   2. 完整短语直接匹配：退出飞书对话 等作为保底
 * 注意：不包含通用退出词（退下/再见等），因为在飞书对话模式下
 * 用户说"退下"应作为普通消息发送到飞书群，而不是退出对话。
 * 通用退出词只在普通对话模式下生效（见 va_is_exit_intent）。 */
bool va_is_feishu_conv_exit_intent(const char *text)
{
  if (!text || !text[0]) return false;

  /* 完整短语直接匹配（保底，覆盖明确说法） */
  static const char *feishu_exit_exact[] = {
    "退出飞书对话", "结束飞书对话",
    "退出飞书聊天", "结束飞书聊天",
    "退出飞书", "结束飞书",
    "关闭飞书对话", "关闭飞书聊天",
    "离开飞书对话", "离开飞书聊天",
    "exit chat mode", "exit conversation mode",
    "leave chat mode", "leave conversation mode",
    "close chat mode", "close conversation mode",
    "end chat mode", "end conversation mode",
    NULL
  };
  for (int i = 0; feishu_exit_exact[i]; i++) {
    if (strcasestr(text, feishu_exit_exact[i])) return true;
  }

  /* 组合匹配：动作词 + 对象词，兼容口语化/ASR误差
   * 如"退了飞书对话""结束跟飞书的对话""关掉飞书聊天"等 */
  static const char *exit_actions[] = {
    "退出", "结束", "关闭", "关掉", "离开", "退了",
    "exit", "leave", "close", "end", "quit", NULL
  };
  static const char *exit_objects[] = {
    "飞书对话", "飞书聊天", "飞书",
    "chat mode", "conversation mode", NULL
  };
  int has_action = 0;
  for (int i = 0; exit_actions[i]; i++) {
    if (strcasestr(text, exit_actions[i])) { has_action = 1; break; }
  }
  if (has_action) {
    for (int i = 0; exit_objects[i]; i++) {
      if (strcasestr(text, exit_objects[i])) return true;
    }
    /* chat mode 模糊匹配：容错 ASR 将 chat 识别为 check/catch 等近音词 */
    if (fuzzy_match_chat_mode(text)) return true;
  }

  return false;
}

static void on_dashscope_vad_event(const char *event_type, const char *data)
{
  /* 快速路径期间不响应VAD事件，避免覆盖“消息筛选”/“文档查询”等状态 */
  if (s_feishu_fast_path) {
    syslog(LOG_DEBUG, "[%s] Cloud VAD event '%s' ignored (fast path active)\n", TAG, event_type);
    return;
  }

  if (strcmp(event_type, "speech_started") == 0) {
    syslog(LOG_INFO, "[%s] Cloud VAD: speech_started (resetting silence timer)\n", TAG);
    /* 飞书对话模式下不更新状态，保持“飞书对话” */
    if (!s_feishu_conversation_mode) {
      voice_assistant_update_status("正在监听...");
    }
    s_last_dialogue_activity = get_time_ms();
    va_set_dialog_state(VA_STATE_USER_SPEAKING);
#ifdef CONFIG_MEDIA
    s_transcript_len = 0;
    s_transcript_buf[0] = '\0';
#endif
  } else if (strcmp(event_type, "speech_stopped") == 0) {
    /* 飞书对话模式下不更新状态，保持“飞书对话” */
    if (!s_feishu_conversation_mode) {
      voice_assistant_update_status("AI回复中...");
    }
  } else if (strcmp(event_type, "transcript") == 0) {
    syslog(LOG_INFO, "[%s] Transcript: %s\n", TAG, data);
    /* TTS播放期间的transcript是回声，不是真实用户输入，直接忽略。
     * 否则会重置s_feishu_fast_path，导致云端response.created与TTS播放器冲突崩溃。
     * 空transcript也不重置快速路径标志。 */
    if (voice_channel_is_speaking()) {
      syslog(LOG_INFO, "[%s] Transcript ignored (TTS playing, echo suppression)\n", TAG);
      return;
    }
    if (!data || !data[0]) {
      syslog(LOG_INFO, "[%s] Empty transcript ignored\n", TAG);
      return;
    }

    /* 用户ASR文本回调通知UI */
    if (s_user_asr_cb) {
      va_user_asr_msg_t *m = malloc(sizeof(va_user_asr_msg_t));
      char *text = strdup(data);
      if (m && text) {
        m->cb = s_user_asr_cb;
        m->text = text;
        lvgl_dispatch_async(va_user_asr_dispatch_cb, m);
      } else {
        free(m);
        free(text);
      }
    }

    /* 新语音输入时重置飞书快速路径标志 */
    s_feishu_fast_path = false;
    s_fast_path_tts_started = false;
    s_fast_path_content_received = false;
    s_fast_path_omni_done = false;
    s_fast_path_tts_done_ts = 0;

#ifdef CONFIG_AI_AGENT_FEISHU
    /* ── 飞书对话模式：屏蔽所有文字任务（飞书专属退出词除外）─────────── */
    /* 对话模式下，用户语音直接发飞书群，不应触发会议、今日消息等任务。
     * 只有飞书专属退出词（退出飞书对话等）落穿到下方飞书对话退出检测，
     * 通用退出词（退下/再见等）不触发退出，作为普通消息发到飞书群。 */
    if (s_feishu_conversation_mode) {
      if (data && va_is_feishu_conv_exit_intent(data)) {
        /* 飞书退出命令落穿到 feishu_conv_send 标签处处理 */
      } else {
        /* 跳过 Layer1/Layer2 意图匹配，直接走对话模式发送逻辑 */
        goto feishu_conv_send;
      }
    }
#endif

    /* Meeting keyword detection (only in dialogue mode) */
    int is_meeting = 0;
    if (data) {
      const char *meeting_keywords[] = {
        /* 核心词 */
        "开会", "开始会议", "开启会议", "启动会议",
        /* 记录类 */
        "记录会议", "会议记录",
        /* 口语化表达 */
        "开个会", "开会了",
        NULL
      };
      for (int i = 0; meeting_keywords[i]; i++) {
        if (strstr(data, meeting_keywords[i])) {
          is_meeting = 1;
          break;
        }
      }
    }

    if (is_meeting) {
      if (s_meeting_callback) {
        s_meeting_callback();
      }
      return;
    }

#ifdef CONFIG_AI_AGENT_FEISHU
    /* Feishu document creation intent detection (first layer)
     * Combination matching: action word AND object word must BOTH be present.
     * If matched, suppress DashScope audio response immediately to avoid AI
     * interrupting, and route text to local agent for fast-path processing. */
    int is_feishu_doc = 0;
    int is_feishu_query = 0;   /* 查询文档内容 */
    int is_feishu_search = 0; /* 搜索关键词 */
    int is_feishu_msg_today = 0; /* 今日消息总结 */
    int is_feishu_conversation = 0; /* 飞书对话模式 */
    const char *matched_action = NULL;
    const char *matched_object = NULL;
    if (data) {
      const char *doc_actions[] = {
        "创建", "新建", "帮我写", "帮我建", "写一个", "建一个",
        "写入", "写到", "写进", "保存到", "存到", "create", NULL
      };
      const char *doc_objects[] = {
        "飞书文档", "飞书", "文档", "document", "doc", NULL
      };
      /* 查询意图：读取/查看/打开文档内容 */
      const char *query_actions[] = {
        "读一下", "查看", "看一下", "打开", "读取", "查询", "看看",
        "read", "show", "view", "open", "look at", "check", "display", NULL
      };
      /* 搜索意图：搜索/查找关键词 */
      const char *search_actions[] = {
        "搜索", "查找", "检索", "找一下", "搜一下",
        "search", "find", "look for", "search for", NULL
      };
      const char *search_objects[] = {
        "关键词", "内容", "飞书文档", "文档", "飞书",
        "keyword", "content", NULL
      };
      int has_doc_action = 0;
      int has_doc_object = 0;
      for (int i = 0; doc_actions[i]; i++) {
        if (strcasestr(data, doc_actions[i])) {
          has_doc_action = 1;
          matched_action = doc_actions[i];
          break;
        }
      }
      for (int i = 0; doc_objects[i]; i++) {
        if (strcasestr(data, doc_objects[i])) {
          has_doc_object = 1;
          matched_object = doc_objects[i];
          break;
        }
      }
      is_feishu_doc = (has_doc_action && has_doc_object);

      /* 查询意图：读一下飞书文档XXX / 查看文档XXX内容 */
      if (!is_feishu_doc) {
        int has_query = 0;
        for (int i = 0; query_actions[i]; i++) {
          if (strcasestr(data, query_actions[i])) { has_query = 1; break; }
        }
        if (has_query && has_doc_object) {
          is_feishu_query = 1;
          matched_action = "查询";
        }
      }

      /* 搜索意图：搜索关键词 / 查找文档中包含XXX */
      if (!is_feishu_doc && !is_feishu_query) {
        int has_search_action = 0;
        for (int i = 0; search_actions[i]; i++) {
          if (strcasestr(data, search_actions[i])) { has_search_action = 1; break; }
        }
        int has_search_object = 0;
        for (int i = 0; search_objects[i]; i++) {
          if (strcasestr(data, search_objects[i])) { has_search_object = 1; break; }
        }
        if (has_search_action && has_search_object) {
          is_feishu_search = 1;
          matched_action = "搜索";
        }
      }

      /* 今日消息意图：读取今日飞书消息并总结 */
      if (!is_feishu_doc && !is_feishu_query && !is_feishu_search) {
        const char *msg_today_keywords[] = {
          "今日消息", "今天的消息", "今日总结", "消息总结",
          "今日飞书消息", "今天飞书消息", "飞书今日消息",
          /* English keywords */
          "today's messages", "todays messages", "today's message",
          "todays message", "today summary", "today's summary",
          "messages today", "today's news", "daily summary",
          NULL
        };
        for (int i = 0; msg_today_keywords[i]; i++) {
          if (strcasestr(data, msg_today_keywords[i])) {
            is_feishu_msg_today = 1;
            matched_action = "今日消息";
            break;
          }
        }
      }

      /* 飞书对话模式意图：进入持续双向对话 */
      if (!is_feishu_doc && !is_feishu_query && !is_feishu_search && !is_feishu_msg_today) {
        const char *conv_keywords[] = {
          "开始飞书对话", "进入飞书对话", "飞书对话模式",
          "开始飞书聊天", "飞书聊天模式", "进入飞书聊天",
          "start chat mode", "enter chat mode",
          "begin chat mode", "open chat mode",
          "start conversation mode", "enter conversation mode",
          "begin conversation mode", "open conversation mode",
          NULL
        };
        for (int i = 0; conv_keywords[i]; i++) {
          if (strcasestr(data, conv_keywords[i])) {
            is_feishu_conversation = 1;
            matched_action = "飞书对话";
            break;
          }
        }
        /* chat mode 模糊匹配：容错 ASR 将 chat 识别为 check/catch 等近音词 */
        if (!is_feishu_conversation) {
          static const char *enter_actions[] = {
            "start", "enter", "begin", "open", NULL
          };
          for (int i = 0; enter_actions[i]; i++) {
            if (strcasestr(data, enter_actions[i]) &&
                fuzzy_match_chat_mode(data)) {
              is_feishu_conversation = 1;
              matched_action = "飞书对话";
              break;
            }
          }
        }
      }
    }
    if (is_feishu_doc || is_feishu_query || is_feishu_search || is_feishu_msg_today || is_feishu_conversation) {
      const char *intent = is_feishu_doc ? "create" : (is_feishu_query ? "query" : (is_feishu_search ? "search" : (is_feishu_msg_today ? "msg_today" : "conversation")));
      syslog(LOG_INFO, "[%s] [FEISHU_DOC] Layer1 matched intent=%s action=\"%s\" object=\"%s\", suppressing cloud audio\n",
             TAG, intent, matched_action ? matched_action : "?", matched_object ? matched_object : "?");
      /* 切换到对应的飞书状态显示 */
      if (is_feishu_msg_today) {
        voice_assistant_update_status("消息筛选");
      } else if (is_feishu_conversation) {
        voice_assistant_update_status("飞书对话");
      } else {
        voice_assistant_update_status("文档查询");
      }
#ifdef CONFIG_MEDIA
      omni_pb_close();
      /* 重置云端响应状态：response.created 可能已先于 Layer1 匹配到达
       * 并设置了 s_response_active=true。若不重置，后续快速路径重置条件
       * !s_response_active 永不满足，导致 mic 被 60s hard timeout 卡死。 */
      s_response_active = false;
      s_last_audio_time = 0;
      s_echo_suppress = false;
      s_omni_echo_active = false;
      s_omni_echo_end_ts = 0;
      s_feishu_fast_path = true;  /* 阻止后续云端 omni 响应事件 */
      s_feishu_fast_path_start_ts = get_time_ms();
      s_fast_path_tts_started = false;
      s_fast_path_content_received = false;  /* 等待消息过滤结果返回 */
      s_fast_path_omni_done = false;  /* 等待云端 response.done 到达 */
      s_fast_path_tts_done_ts = 0;
      /* 关闭mic录音机，防止用户在TTS播报期间继续说话影响交互体验。
       * 快速路径结束后（TTS完成/超时/异常）会重新打开mic。 */
      if (s_recorder) {
        dialogue_close_mic(&s_recorder);
        syslog(LOG_INFO, "[%s] [FEISHU_FAST_PATH] mic closed for fast path\n", TAG);
      }
      /* 清除云端音频缓冲区，阻止云端基于已接收音频生成响应。
       * 不关闭ASR流：关闭会导致s_asr_stream=NULL，内层while循环退出，
       * 主循环在ASR重连期间（~20秒阻塞）无法执行TTS跟踪和快速路径释放逻辑，
       * s_fast_path_tts_started/s_fast_path_content_received永远不会被设置，
       * 最终触发90秒硬超时，状态卡在"文档查询"无法恢复。
       * 保留连接让主循环持续运行，ping机制维持WebSocket存活，
       * 云端response.done会正确到达并清理s_response_active。 */
      if (s_asr_stream) {
        dashscope_asr_stream_clear_buffer(s_asr_stream);
        syslog(LOG_INFO, "[%s] [FEISHU_FAST_PATH] ASR buffer cleared (stream kept alive)\n", TAG);
      }
#endif
      /* Display user message on UI first */
      ai_page_show_user_message(data);
      /* Push to local agent message bus for second-layer matching & title extraction */
      agent_msg_t msg;
      memset(&msg, 0, sizeof(msg));
      strncpy(msg.channel, AGENT_CHAN_VOICE, sizeof(msg.channel) - 1);
      strncpy(msg.chat_id, "voice", sizeof(msg.chat_id) - 1);
      msg.content = strdup(data);
      if (msg.content) {
        int mret = message_bus_push_inbound(&msg);
        syslog(LOG_INFO, "[%s] [FEISHU_DOC] Pushed to local agent bus, ret=%d\n", TAG, mret);
      } else {
        syslog(LOG_ERR, "[%s] [FEISHU_DOC] Failed to strdup text (OOM)\n", TAG);
      }
      return; /* Bypass normal cloud conversation flow */
    }
#endif

    /* Exit command detection: fuzzy match exit intent
     * (退下/退出/退一下/结束对话/不聊了/再见/拜拜/休息吧 等)
     * 仅在普通对话模式（非飞书对话模式）下生效：飞书对话模式下通用退出词
     * 作为普通消息发到飞书群，不触发退出。 */
    if (data && s_dialogue_active && !s_feishu_conversation_mode) {
      if (va_is_exit_intent(data)) {
        s_pending_dialogue_exit = true;
      }
    }

#ifdef CONFIG_AI_AGENT_FEISHU
feishu_conv_send:
    /* ── 飞书对话模式：用户语音直接发飞书群 ──────────────── */
    if (data && s_feishu_conversation_mode && !voice_channel_is_speaking()) {
      ai_page_show_user_message(data);

      /* 检查飞书对话退出命令（仅飞书专属退出词：退出飞书对话/结束飞书对话 等） */
      if (va_is_feishu_conv_exit_intent(data)) {
        syslog(LOG_INFO, "[%s] [FEISHU_CONV] exit command detected: %s\n", TAG, data);
        voice_assistant_exit_feishu_conversation();
        /* 不直接调用voice_channel_speak()：此代码在dashscope_thread中执行，
         * 而feishu_tts_worker线程也可能在调用voice_channel_speak()，
         * 并发调用会导致TTS WebSocket内存损坏（Unaligned access崩溃）。
         * 改为通过agent_bus发送文字回复，由agent线程安全地执行TTS。 */
        {
          agent_msg_t exit_msg;
          memset(&exit_msg, 0, sizeof(exit_msg));
          strncpy(exit_msg.channel, AGENT_CHAN_VOICE, sizeof(exit_msg.channel) - 1);
          strncpy(exit_msg.chat_id, "voice", sizeof(exit_msg.chat_id) - 1);
          /* 推送用户原文（而非固定中文），由 agent_loop 的 handle_nl_fast_path
           * 根据 is_english_text() 检测语言并返回对应语言的退出提示。 */
          exit_msg.content = strdup(data);
          if (exit_msg.content) {
            message_bus_push_inbound(&exit_msg);
          }
        }
        return;
      }

      /* 检查是否包含@指定人模式: "给XXX发消息 ..." / "跟XXX说 ..." */
      char mention_name[64] = {0};
      char mention_msg[512] = {0};
      if (feishu_conversation_try_mention(data, mention_name, sizeof(mention_name),
                                           mention_msg, sizeof(mention_msg))) {
        char open_id[64] = {0};
        char display_name[64] = {0};
        if (feishu_recv_find_member_by_name(mention_name, open_id, sizeof(open_id),
                                             display_name, sizeof(display_name))) {
          /* 发送@mention消息到飞书群 */
          feishu_conversation_send_mention(s_feishu_conversation_chat_id,
                                            open_id, display_name, mention_msg);
          conv_record_sent(mention_msg);
          syslog(LOG_INFO, "[%s] [FEISHU_CONV] @mention sent: to=%s msg=%s\n",
              TAG, display_name, mention_msg);
        } else {
          /* 未找到成员，作为普通消息发送 */
          feishu_send_text_message_ex(s_feishu_conversation_chat_id, data, "chat_id");
          conv_record_sent(data);
          syslog(LOG_WARNING, "[%s] [FEISHU_CONV] member not found: %s, sent as normal\n",
              TAG, mention_name);
        }
      } else {
        /* 普通消息：直接发飞书群 */
        feishu_send_text_message_ex(s_feishu_conversation_chat_id, data, "chat_id");
        conv_record_sent(data);
        syslog(LOG_INFO, "[%s] [FEISHU_CONV] sent to group: %s\n", TAG, data);
      }

      /* 更新对话活动时间，防止 silence timeout */
      s_last_dialogue_activity = get_time_ms();

#ifdef CONFIG_MEDIA
      /* 清除云端音频缓冲区，发送 input_audio_buffer.clear，
       * 阻止云端基于已接收音频生成响应。不清除 s_echo_suppress，
       * 由 TTS 播放期间的 s_tts_echo_active 机制处理回声抑制。 */
      if (s_asr_stream) {
        dashscope_asr_stream_clear_buffer(s_asr_stream);
      }
      s_response_active = false;
#endif
      return; /* 不走 feishu_fwd_dispatch，不走云端LLM */
    }
#endif

    ai_page_show_user_message(data);
#ifdef CONFIG_AI_AGENT_FEISHU
    feishu_fwd_dispatch("我：", data);
#endif
  }
}

static void process_dashscope_event(const char *json)
{
  cJSON *root = cJSON_Parse(json);
  if (!root) {
    syslog(LOG_WARNING, "[%s] Failed to parse event: %s\n", TAG, json);
    return;
  }

  cJSON *type_item = cJSON_GetObjectItem(root, "type");
  if (!type_item || !cJSON_IsString(type_item)) {
    cJSON_Delete(root);
    return;
  }

  const char *event_type = type_item->valuestring;

  /* 监听已暂停（用户退出AI页面）：丢弃云端残留事件，防止上次对话内容
   * 被推送到新会话。input_audio_buffer.clear 只能丢弃未处理的音频缓冲，
   * 无法取消云端已开始生成的 response（VAD 的 speech_stopped 已触发 commit）。
   * 因此必须在客户端侧丢弃 response / speech_stopped / transcript 等事件。 */
  if (s_listening_suspended) {
    /* session.* 和 input_audio_buffer.committed(图片分析流程) 仍需处理。
     * 另：图片分析活跃期间(SENDING/COMMITTING/WAITING)须放行所有事件，
     * 否则 AI 回复(response.*)会被误丢弃，导致 camera_ai 页无回复。
     * 原因：s_listening_suspended 标志被两种语义共用——
     *   (1) ai_page 退出时丢弃云端残留事件；
     *   (2) camera_page 进入时关闭 mic/VAD 防干扰。
     * 语义(2)期间不应丢弃事件，用 img_ctx 活跃状态区分两者。 */
    bool img_active = (s_img_ctx.state == IMG_STATE_SENDING ||
                       s_img_ctx.state == IMG_STATE_COMMITTING ||
                       s_img_ctx.state == IMG_STATE_WAITING);
    if (strncmp(event_type, "session.", 8) != 0 &&
        strcmp(event_type, "input_audio_buffer.committed") != 0 &&
        !img_active) {
      syslog(LOG_INFO, "[%s] listening suspended, dropping event: %s\n",
             TAG, event_type);
      cJSON_Delete(root);
      return;
    }
  }

  /* 只对关键事件打日志，避免日志过多 */
  if (strncmp(event_type, "response", 8) == 0 ||
      strstr(event_type, "image") != NULL ||
      strcmp(event_type, "error") == 0 ||
      strcmp(event_type, "session.created") == 0 ||
      strstr(event_type, "speech_started") != NULL ||
      strstr(event_type, "speech_stopped") != NULL) {
    syslog(LOG_INFO, "[WS_EVENT] type=%s\n", event_type);
  }

  if (strcmp(event_type, "session.created") == 0) {
  } else if (strcmp(event_type, "session.updated") == 0) {
  } else if (strcmp(event_type, "input_audio_buffer.speech_started") == 0) {
    on_dashscope_vad_event("speech_started", NULL);
  } else if (strcmp(event_type, "input_audio_buffer.speech_stopped") == 0) {
    on_dashscope_vad_event("speech_stopped", NULL);
  } else if (strcmp(event_type, "input_audio_buffer.committed") == 0) {
    /* Manual模式：服务端确认音频+图片缓冲区提交完成，此时发送response.create */
    pthread_mutex_lock(&s_img_ctx.lock);
    if (s_img_ctx.state == IMG_STATE_COMMITTING) {
      syslog(LOG_INFO, "[AI_IMG] ===== input_audio_buffer.committed received, sending response.create =====\n");
      pthread_mutex_unlock(&s_img_ctx.lock);
      img_notify_status("Analyzing image...", false);
      int cr = dashscope_asr_stream_commit(s_asr_stream);
      if (cr == 0) {
        pthread_mutex_lock(&s_img_ctx.lock);
        s_img_ctx.state = IMG_STATE_WAITING;
        pthread_mutex_unlock(&s_img_ctx.lock);
        syslog(LOG_INFO, "[AI_IMG] response.create sent, entering WAITING state\n");
      } else {
        syslog(LOG_ERR, "[AI_IMG] response.create send failed (%d), cleaning up\n", cr);
        pthread_mutex_lock(&s_img_ctx.lock);
        if (s_img_ctx.data) { free(s_img_ctx.data); s_img_ctx.data = NULL; }
        s_img_ctx.data_len = 0;
        s_img_ctx.state = IMG_STATE_ERROR;
        s_img_sending = false;
        pthread_mutex_unlock(&s_img_ctx.lock);
        img_notify_status("Analysis failed", true);
        /* 恢复VAD模式 */
        dashscope_asr_stream_enable_vad(s_asr_stream);
      }
    } else {
      pthread_mutex_unlock(&s_img_ctx.lock);
    }
  } else if (strcmp(event_type, "conversation.item.created") == 0) {
  } else if (strcmp(event_type,
             "conversation.item.input_audio_transcription.text") == 0) {
  } else if (strcmp(event_type,
             "conversation.item.input_audio_transcription.completed") == 0) {
    cJSON *transcript = cJSON_GetObjectItem(root, "transcript");
    if (transcript && cJSON_IsString(transcript)) {
      on_dashscope_vad_event("transcript", transcript->valuestring);
#ifdef CONFIG_AI_AGENT_FEISHU
      /* 飞书对话模式下不缓存用户问题（不走文档写入流程） */
      if (!s_feishu_conversation_mode) {
        /* 缓存用户问题，用于文档写入时添加"我：xxx"前缀。
         * 过滤唤醒词（如"小Q，小Q。"），避免无意义文本写入飞书文档 */
        if (!is_wake_word(transcript->valuestring)) {
          strncpy(s_last_user_question, transcript->valuestring, sizeof(s_last_user_question) - 1);
          s_last_user_question[sizeof(s_last_user_question) - 1] = '\0';
          syslog(LOG_DEBUG, "[FEISHU_DOC] Cached user question: %s\n", s_last_user_question);
        } else {
          syslog(LOG_DEBUG, "[FEISHU_DOC] Skipped wake word: %s\n", transcript->valuestring);
        }
      }
#endif
    }
  } else if (strcmp(event_type,
             "conversation.item.input_audio_transcription.failed") == 0) {
    cJSON *err = cJSON_GetObjectItem(root, "error");
    syslog(LOG_ERR, "[%s] Transcription failed: %s\n", TAG,
      err ? (cJSON_GetObjectItem(err, "message") &&
             cJSON_IsString(cJSON_GetObjectItem(err, "message"))
            ? cJSON_GetObjectItem(err, "message")->valuestring
            : "unknown") : "unknown");
  } else if (strcmp(event_type, "response.created") == 0) {
    if (s_feishu_fast_path || s_feishu_conversation_mode) {
      syslog(LOG_INFO, "[%s] %s active, skipping response.created\n", TAG,
             s_feishu_conversation_mode ? "Feishu conversation mode" : "Feishu fast path");
    } else if (voice_channel_is_speaking()) {
      /* TTS播放期间收到的response.created是回声触发的云端响应，
       * 必须跳过，否则omni_pb_open与TTS播放器冲突导致崩溃。 */
      syslog(LOG_INFO, "[%s] TTS playing, skipping response.created (echo)\n", TAG);
    } else
#ifdef CONFIG_MEDIA
    {
    s_response_active = true;
    s_echo_suppress = true;
    /* Omni 播报回声抑制：AI 开始说话，阻止 mic 上传直到播报完成 + settle */
    s_omni_echo_active = true;
    s_omni_echo_end_ts = 0;
    s_last_audio_time = 0;
    va_set_dialog_state(VA_STATE_AI_SPEAKING);
    if (s_asr_stream) {
      dashscope_asr_stream_clear_buffer(s_asr_stream);
    }
    /* 清空AI回复转录缓存，避免上一个response的内容残留到下一个回复 */
    s_transcript_len = 0;
    s_transcript_buf[0] = '\0';
    omni_pb_open();
    }
#endif
  } else if (strcmp(event_type, "response.audio_transcript.delta") == 0) {
    if (s_feishu_fast_path || s_feishu_conversation_mode) {
      /* 飞书快速路径/对话模式激活时，丢弃云端转录文本，避免显示云端大模型的回复 */
    } else {
    cJSON *delta = cJSON_GetObjectItem(root, "delta");
    if (cJSON_IsString(delta) && delta->valuestring[0]) {
#ifdef CONFIG_MEDIA
      int dlen = strlen(delta->valuestring);
      if (s_transcript_len + dlen < (int)sizeof(s_transcript_buf) - 1) {
        memcpy(s_transcript_buf + s_transcript_len,
               delta->valuestring, dlen);
        s_transcript_len += dlen;
        s_transcript_buf[s_transcript_len] = '\0';
      }
#endif
    }
    }
  } else if (strcmp(event_type, "response.audio_transcript.done") == 0) {
    cJSON *text_item = cJSON_GetObjectItem(root, "text");
    const char *full_text = NULL;
    if (cJSON_IsString(text_item) && text_item->valuestring[0]) {
      full_text = text_item->valuestring;
    }
#ifdef CONFIG_MEDIA
    else if (s_transcript_len > 0) {
      full_text = s_transcript_buf;
    }
#endif

    if (full_text) {
      syslog(LOG_INFO, "[%s] Omni transcript done: %s\n", TAG, full_text);
      if (s_feishu_fast_path || s_feishu_conversation_mode) {
        syslog(LOG_INFO, "[%s] %s active, discarding cloud LLM response\n", TAG,
               s_feishu_conversation_mode ? "Feishu conversation mode" : "Feishu fast path");
      } else {
      ai_page_show_omni_response(full_text);
#ifdef CONFIG_AI_AGENT_FEISHU
      /* 截断创建文档前缀，飞书聊天和文档写入都使用干净的文本 */
      const char *clean_fwd_text = full_text;
      if (s_need_skip_doc_prefix) {
        clean_fwd_text = strip_doc_create_prefix(full_text);
        s_need_skip_doc_prefix = false;
        syslog(LOG_INFO, "[FEISHU_DOC] Stripped doc create prefix from reply\n");
      }

      /* AI回复文本回调通知UI */
      if (s_ai_reply_cb) {
        va_ai_reply_msg_t *m = malloc(sizeof(va_ai_reply_msg_t));
        char *text = strdup(clean_fwd_text);
        if (m && text) {
          m->cb = s_ai_reply_cb;
          m->text = text;
          lvgl_dispatch_async(va_ai_reply_dispatch_cb, m);
        } else {
          free(m);
          free(text);
        }
      }

      /* 聊天转发：feishu_fwd_dispatch 内部检查 s_fwd_enabled，
       * 仅当 cam_ai_btn_cb 判断无文档时才会开启转发 */
      feishu_fwd_dispatch("AI：", clean_fwd_text);

      /* 文档写入：独立于聊天转发，仅当图片分析进行中且有活跃文档时写入 */
      if (s_fwd_img_analysis_started && s_active_feishu_doc_id[0] != '\0') {
        s_fwd_ai_count++;
        syslog(LOG_DEBUG, "[FEISHU_DOC] Doc write round counted: %d\n", s_fwd_ai_count);

        const char *img_path = (s_fwd_ai_count == 1) ? s_img_ctx.filename : NULL;
        syslog(LOG_INFO, "[FEISHU_DOC] Triggering doc write: round=%d image=%s doc=%s\n",
               s_fwd_ai_count, img_path ? img_path : "(none)", s_active_feishu_doc_id);
        feishu_doc_write_dispatch(img_path, clean_fwd_text, s_last_user_question);
        s_last_user_question[0] = '\0';

        if (s_fwd_ai_count >= FEISHU_FWD_MAX_AI_REPLIES) {
          syslog(LOG_INFO, "[%s] feishu doc: 5 replies written, waiting for next image...\n", TAG);
          s_fwd_img_analysis_started = false;
        }
      }
#endif
      } /* end of else (non-fast-path) */
    }
  } else if (strcmp(event_type, "response.audio.delta") == 0) {
    if (s_feishu_fast_path || s_feishu_conversation_mode) {
      /* 飞书快速路径/对话模式激活时，丢弃云端音频数据 */
    } else {
    cJSON *delta = cJSON_GetObjectItem(root, "delta");
    if (cJSON_IsString(delta) && delta->valuestring[0]) {
      /* Update dialogue activity time (AI is speaking) */
      s_last_dialogue_activity = get_time_ms();
#ifdef CONFIG_MEDIA
      /* TTS播报期间禁止mic上传，防止回声被服务端误识别为新语音 */
      s_echo_suppress = true;
      /* Omni 播报进行中：持续保持回声抑制，重置 settle 计时 */
      s_omni_echo_active = true;
      s_omni_echo_end_ts = 0;
      size_t b64_len = strlen(delta->valuestring);
      unsigned char *pcm_buf = malloc(B64_DECODE_BUF);
      if (pcm_buf) {
        size_t pcm_len = 0;
        int bret = mbedtls_base64_decode(pcm_buf, B64_DECODE_BUF,
                                          &pcm_len,
                                          (const unsigned char *)delta->valuestring,
                                          b64_len);
        if (bret == 0 && pcm_len > 0) {
          /* 投递到 consumer 队列，所有权转移，不阻塞接收线程 */
          omni_pb_enqueue(pcm_buf, pcm_len);
          pcm_buf = NULL;
          s_last_audio_time = get_time_ms();
        } else if (bret != 0) {
          syslog(LOG_ERR, "[%s] Base64 decode failed: %d\n", TAG, bret);
        }
        free(pcm_buf);
      }
#endif
    }
    } /* end else (not feishu fast path) */
  } else if (strcmp(event_type, "response.audio.done") == 0) {
  } else if (strcmp(event_type, "response.content_part.done") == 0) {
  } else if (strcmp(event_type, "response.output_item.done") == 0) {
  } else if (strcmp(event_type, "response.done") == 0) {
    /* 飞书快速路径：云端 omni 响应完成，但飞书查询可能还在进行中。
     * 快速路径期间 response.created 被跳过，omni_pb 从未打开，
     * 因此不需要执行 omni_pb_close / echo settle / 状态切换等正常清理逻辑。
     * 只更新 dialogue activity 时间防止 silence timeout，
     * s_feishu_fast_path 由主循环的 TTS-done 释放逻辑或硬超时重置。 */
    if (s_feishu_fast_path) {
      syslog(LOG_INFO, "[%s] Feishu fast path: omni done, query still in progress\n", TAG);
      s_last_dialogue_activity = get_time_ms();
      s_response_active = false;
      s_fast_path_omni_done = true;  /* 标记云端响应完成，允许快速路径释放 */
      /* 快速路径期间不做其他清理：
       * - omni_pb_close()：omni_pb 从未打开，无需关闭
       * - dashscope_asr_stream_clear_buffer()：Layer1已清除，无需重复
       * - voice_assistant_update_status()：快速路径仍激活，不应切换为"正在监听..."
       *   否则用户看到"正在监听..."但 mic 仍关闭，说话无响应 */
    } else if (s_feishu_conversation_mode) {
      syslog(LOG_INFO, "[%s] Feishu conversation mode: omni done, query still in progress\n", TAG);
      s_last_dialogue_activity = get_time_ms();
#ifdef CONFIG_MEDIA
      s_response_active = false;
      s_last_audio_time = 0;
      omni_pb_close();
      /* 对话模式下不清除云端缓冲区，避免误删用户新语音 */
      s_omni_echo_end_ts = get_time_ms();
      syslog(LOG_INFO, "[%s] Omni done: pb closed, echo settle for %d ms\n",
             TAG, ECHO_SETTLE_MS);
      s_echo_suppress = false;
#endif
    } else {
#ifdef CONFIG_MEDIA
      s_response_active = false;
      s_last_audio_time = 0;
      omni_pb_close();
      if (s_asr_stream) {
        dashscope_asr_stream_clear_buffer(s_asr_stream);
      }
      /* omni_pb_close() 同步 drain 等待音频播完，期间可能耗时数十秒。
       * drain 期间 s_last_dialogue_activity 未更新（最后一个 delta 后停），
       * 且事件线程被阻塞导致主循环 silence timeout 检查无法执行。
       * drain 返回后立即补刷活动时间戳，避免误判静音超时立即退出对话。 */
      s_last_dialogue_activity = get_time_ms();
      /* Omni 播报回声抑制（类比 s_tts_echo_active 方案）：
       * omni_pb_close() 已 drain 等待音频播放完毕并停止播放器。
       * 不再在事件线程里 usleep 阻塞，改为记录结束时间戳，
       * 由主循环轮询等 ECHO_SETTLE_MS 后才清除 s_omni_echo_active，
       * 放开 mic 上传。这样既不阻塞事件线程，又能确保残余回声衰减完。 */
      s_omni_echo_end_ts = get_time_ms();
      syslog(LOG_INFO, "[%s] Omni done: pb closed, echo settle for %d ms\n",
             TAG, ECHO_SETTLE_MS);
      /* 注意：不在此清除 s_omni_echo_active，由主循环 settle 后清除 */
      s_echo_suppress = false;  /* s_echo_suppress 保留兼容旧逻辑，由 s_omni_echo_active 主导 */
#endif
    }
    /* 图片AI分析完成清理 */
    pthread_mutex_lock(&s_img_ctx.lock);
    if (s_img_ctx.state == IMG_STATE_WAITING) {
      syslog(LOG_INFO, "[AI_IMG] ===== response.done: analysis COMPLETED =====\n");
      if (s_img_ctx.data) {
        free(s_img_ctx.data);
        s_img_ctx.data = NULL;
      }
      s_img_ctx.data_len = 0;
      s_img_ctx.state = IMG_STATE_IDLE;
      s_img_sending = false;
      img_notify_status("Analysis done", true);
      pthread_mutex_unlock(&s_img_ctx.lock);
      /* 恢复VAD模式，回到语音自动检测 */
      dashscope_asr_stream_enable_vad(s_asr_stream);
    } else {
      pthread_mutex_unlock(&s_img_ctx.lock);
    }
    /* 状态切换：飞书快速路径/对话模式下不切换，由各自的释放逻辑处理 */
    if (!s_feishu_fast_path && !s_feishu_conversation_mode) {
      voice_assistant_update_status("正在监听...");
    }
  } else if (strcmp(event_type,
             "conversation.item.input_audio_transcription.delta") == 0) {
    /* Intermediate transcription delta — silently absorbed;
     * the completed transcription is handled above. */
  } else if (strcmp(event_type, "error") == 0) {
    cJSON *error = cJSON_GetObjectItem(root, "error");
    if (error) {
      cJSON *msg = cJSON_GetObjectItem(error, "message");
      syslog(LOG_ERR, "[%s] Error: %s\n", TAG,
             msg ? msg->valuestring : "unknown");
    }
    /* 图片分析期间出错，清理状态 */
    pthread_mutex_lock(&s_img_ctx.lock);
    if (s_img_ctx.state == IMG_STATE_WAITING ||
        s_img_ctx.state == IMG_STATE_SENDING ||
        s_img_ctx.state == IMG_STATE_COMMITTING) {
      syslog(LOG_ERR, "[AI_IMG] ===== server ERROR, analysis FAILED =====\n");
      if (s_img_ctx.data) {
        free(s_img_ctx.data);
        s_img_ctx.data = NULL;
      }
      s_img_ctx.data_len = 0;
      s_img_ctx.state = IMG_STATE_IDLE;
      s_img_sending = false;
      pthread_mutex_unlock(&s_img_ctx.lock);
      img_notify_status("Analysis failed", true);
      /* 恢复VAD模式 */
      dashscope_asr_stream_enable_vad(s_asr_stream);
    } else {
      pthread_mutex_unlock(&s_img_ctx.lock);
    }
  } else {
  }

  cJSON_Delete(root);
}

/* ── Dialogue mode mic management ──────────────────────────────── */

/* Open mic recorder for dialogue mode.
 * Caller must have already stopped wakeup_detector. */
static int dialogue_open_mic(media_recorder_handle_t *recorder_out)
{
  media_recorder_handle_t recorder;
  int ret;

#ifdef CONFIG_MEDIA
  media_policy_set_mic_mute(1);
  s_echo_suppress = false;
  s_omni_echo_active = false;
  s_omni_echo_end_ts = 0;
  s_tts_echo_active = false;
  s_tts_echo_end_ts = 0;

  int vol_min = 0, vol_max = 10;
  media_policy_get_range(MEDIA_SCENARIO_RECORD MEDIA_POLICY_VOLUME,
                         &vol_min, &vol_max);
  media_policy_set_stream_volume(MEDIA_SCENARIO_RECORD, vol_max);

  vol_min = 0; vol_max = 15;
  media_policy_get_range(MEDIA_STREAM_MEDIA MEDIA_POLICY_VOLUME,
                         &vol_min, &vol_max);
  media_policy_set_stream_volume(MEDIA_STREAM_MEDIA, vol_max);

  media_policy_include("SelCap", "mic1", 1);
#endif

  recorder = media_recorder_open(MEDIA_SOURCE_MIC);
  if (!recorder) {
    syslog(LOG_WARNING, "[%s] Failed to open media_recorder for dialogue\n", TAG);
#ifdef CONFIG_MEDIA
    media_policy_exclude("SelCap", "mic1", 1);
#endif
    return -1;
  }

  ret = media_recorder_prepare(recorder, NULL,
      "fmt=[rate=#16000,ch=#1,bits=#16,width=#2],enc=[keys=pcm,imin=#1920]");
  if (ret != 0) {
    syslog(LOG_WARNING, "[%s] Failed to prepare dialogue recorder (ret=%d)\n", TAG, ret);
    media_recorder_close(recorder);
#ifdef CONFIG_MEDIA
    media_policy_exclude("SelCap", "mic1", 1);
#endif
    return -1;
  }

  ret = media_recorder_start(recorder);
  if (ret != 0) {
    syslog(LOG_WARNING, "[%s] Failed to start dialogue recorder (ret=%d)\n", TAG, ret);
    media_recorder_close(recorder);
#ifdef CONFIG_MEDIA
    media_policy_exclude("SelCap", "mic1", 1);
#endif
    return -1;
  }

#ifdef CONFIG_MEDIA
  if (af_stream_set_chan_vol) {
    uint32_t af_ret = af_stream_set_chan_vol(
        BES_AUD_STREAM_ID_0,
        BES_AUD_STREAM_CAPTURE,
        BES_AUD_CHANNEL_MAP_CH0,
        BES_MIC_GAIN_MAX);
    syslog(LOG_INFO, "[%s] mic gain max, af_ret=%lu\n", TAG, (unsigned long)af_ret);
  } else {
    syslog(LOG_WARNING, "[%s] af_stream_set_chan_vol not available\n", TAG);
  }
#endif

  *recorder_out = recorder;
  syslog(LOG_INFO, "[%s] Dialogue mic recorder started\n", TAG);
  return 0;
}

/* Close mic recorder for dialogue mode */
static void dialogue_close_mic(media_recorder_handle_t *recorder)
{
  if (*recorder) {
    media_recorder_stop(*recorder);
    media_recorder_close(*recorder);
    *recorder = NULL;
  }
#ifdef CONFIG_MEDIA
  media_policy_exclude("SelCap", "mic1", 1);
#endif
  syslog(LOG_INFO, "[%s] Dialogue mic recorder closed\n", TAG);
}

static void *dashscope_thread(void *arg)
{
  (void)arg;

  syslog(LOG_INFO, "[%s] DashScope thread started\n", TAG);

  unsigned char *chunk = malloc(CHUNK_SIZE);
  if (!chunk) {
    syslog(LOG_ERR, "[%s] Failed to allocate audio buffer\n", TAG);
    return NULL;
  }

  int retry_count = 0;

  while (s_va_running) {
    s_asr_stream = dashscope_asr_stream_open();
    if (!s_asr_stream) {
      int delay_sec = 3;
      if (retry_count > 0) {
        delay_sec = 3 << (retry_count > 4 ? 4 : retry_count);
        if (delay_sec > 60) delay_sec = 60;
      }
      syslog(LOG_WARNING, "[%s] Failed to connect to DashScope, retrying in %ds...\n", TAG, delay_sec);
      /* 快速路径/飞书对话期间不更新UI状态 */
      if (!s_feishu_fast_path && !s_feishu_conversation_mode) {
        voice_assistant_update_status("网络连接中...");
      }
      usleep(delay_sec * 1000000);
      retry_count++;
      continue;
    }

    if (s_feishu_fast_path) {
      syslog(LOG_INFO, "[%s] Connected to DashScope Omni (fast path active, standby)\n", TAG);
      /* 快速路径期间不更新UI状态，避免误导用户 */
    } else {
      syslog(LOG_INFO, "[%s] Connected to DashScope Omni (idle mode)\n", TAG);
      /* 飞书对话模式下保持"飞书对话"状态 */
      if (!s_feishu_conversation_mode) {
        voice_assistant_update_status("正在监听...");
      }
    }
    retry_count = 0;

    /* Idle mode: cloud connected, no mic, local VAD stays running */
    /* Do NOT stop wakeup_detector, do NOT open media_recorder */

    char *recv_buf = malloc(WS_RECV_BUF);
    if (!recv_buf) {
      syslog(LOG_ERR, "[%s] Failed to allocate receive buffer\n", TAG);
      break;
    }

    unsigned char *send_buf = malloc(SEND_CHUNK_SIZE);
    if (!send_buf) {
      syslog(LOG_ERR, "[%s] Failed to allocate send buffer\n", TAG);
      free(recv_buf);
      break;
    }

    size_t send_buf_used = 0;
    uint64_t last_ping_time = 0;
    int send_count = 0;
    int ret = 0;

    while (s_va_running && s_asr_stream) {
      /* ═══ Dialogue mode transitions ═══ */

      if (s_dialogue_active && !s_recorder && !s_feishu_fast_path) {
        /* Enter dialogue mode: stop local VAD, open cloud mic.
         * Skip if fast path is active: the mic was closed intentionally
         * and will be reopened by the fast path exit logic. */
        syslog(LOG_INFO, "[%s] Entering dialogue mode...\n", TAG);
        wakeup_detector_stop();

        if (dialogue_open_mic(&s_recorder) != 0) {
          syslog(LOG_ERR, "[%s] Failed to open dialogue mic, aborting dialogue\n", TAG);
          s_dialogue_active = false;
          va_maybe_start_wakeup();
        } else {
          s_last_dialogue_activity = get_time_ms();
          send_buf_used = 0;
          /* 飞书对话模式下保持"飞书对话"状态 */
          if (!s_feishu_conversation_mode) {
            voice_assistant_update_status("正在监听...");
          }
        }
      }

      if (!s_dialogue_active && s_recorder) {
        /* Exit dialogue mode: close cloud mic, restart local VAD */
        syslog(LOG_INFO, "[%s] Exiting dialogue mode...\n", TAG);
        dialogue_close_mic(&s_recorder);
        va_maybe_start_wakeup();
        s_last_dialogue_activity = 0;
      }

      /* ═══ 飞书对话模式：ASR/Omi模式切换（主循环线程安全执行） ═══ */
      if (s_pending_asr_only_switch) {
        s_pending_asr_only_switch = false;
        if (s_asr_stream) {
          syslog(LOG_INFO, "[%s] [FEISHU_CONV] Main loop: switching to ASR-only mode\n", TAG);
          dashscope_asr_stream_set_asr_only(s_asr_stream);
        }
        /* 飞书对话模式：ASR-only切换完成后立即重新打开mic。
         * 不等TTS播报完成，让用户可以在TTS播报期间就开始说话，
         * 语音会被云端ASR识别并发送到飞书群。 */
        if (s_feishu_conversation_mode && s_dialogue_active && !s_recorder) {
          if (dialogue_open_mic(&s_recorder) == 0) {
            syslog(LOG_INFO, "[%s] [FEISHU_CONV] mic reopened after ASR-only switch\n", TAG);
            s_last_dialogue_activity = get_time_ms();
          } else {
            syslog(LOG_WARNING, "[%s] [FEISHU_CONV] mic reopen failed after ASR-only switch\n", TAG);
          }
        }
      }
      if (s_pending_omni_restore) {
        s_pending_omni_restore = false;
        if (s_asr_stream) {
          syslog(LOG_INFO, "[%s] [FEISHU_CONV] Main loop: restoring Omni mode\n", TAG);
          dashscope_asr_stream_restore_omni(s_asr_stream);
        }
      }
      if (s_pending_lang_refresh) {
        s_pending_lang_refresh = false;
        if (s_asr_stream) {
          syslog(LOG_INFO, "[%s] Main loop: refreshing session instructions (lang changed)\n", TAG);
          dashscope_asr_stream_update_session_instructions(s_asr_stream);
        }
      }

      /* ═══ Image AI analysis (works in both idle and dialogue mode) ═══ */
      if (s_asr_stream && !s_img_sending) {
        pthread_mutex_lock(&s_img_ctx.lock);
        if (s_img_ctx.state == IMG_STATE_QUEUED) {
          s_img_ctx.state = IMG_STATE_SENDING;
          s_img_sending = true;
        }
        pthread_mutex_unlock(&s_img_ctx.lock);
      }

      if (s_asr_stream &&
          s_img_sending && s_img_ctx.state == IMG_STATE_SENDING) {
        syslog(LOG_INFO, "[AI_IMG] detected QUEUED image, starting send...\n");
        int iret = do_image_send_and_commit();
        pthread_mutex_lock(&s_img_ctx.lock);
        if (iret == 0) {
          syslog(LOG_INFO, "[AI_IMG] send+commit OK, entering COMMITTING state\n");
          s_img_ctx.state = IMG_STATE_COMMITTING;
        } else {
          syslog(LOG_ERR, "[AI_IMG] send FAILED (iret=%d), cleaning up\n", iret);
          if (s_img_ctx.data) {
            free(s_img_ctx.data);
            s_img_ctx.data = NULL;
          }
          s_img_ctx.data_len = 0;
          s_img_ctx.state = IMG_STATE_ERROR;
          s_img_sending = false;
          img_notify_status("Image upload failed", true);
        }
        pthread_mutex_unlock(&s_img_ctx.lock);
        continue;
      }

      /* ═══ TTS echo suppression tracking (always, even during fast path) ═══ */
      /* 无论mic是否打开，都需要跟踪TTS播放状态和回声抑制。
       * 否则快速路径期间mic关闭时，回声抑制逻辑不执行，
       * mic重新打开后残余TTS回声会被云端VAD误判为用户语音。 */
#ifdef CONFIG_MEDIA
      if (voice_channel_is_speaking()) {
        s_tts_echo_active = true;
        s_tts_echo_end_ts = 0;
        /* TTS播放期间持续更新对话活动时间，避免播放过程中触发silence timeout。
         * 飞书文档内容较长时（如沁园春雪），TTS播放可能持续30秒以上，
         * 若不更新会导致播放完成立即误触发"没有问题我先退下了"。 */
        if (s_dialogue_active) {
          s_last_dialogue_activity = get_time_ms();
        }
        /* 标记快速路径期间TTS已开始播放，且内容已收到 */
        if (s_feishu_fast_path) {
          s_fast_path_tts_started = true;
          s_fast_path_content_received = true;  /* agent已调用voice_channel_speak，内容已从LLM返回 */
          s_fast_path_tts_done_ts = 0;  /* TTS重新开始，重置完成时间戳 */
        }
      } else if (s_tts_echo_active) {
        if (s_tts_echo_end_ts == 0) {
          s_tts_echo_end_ts = get_time_ms();
          /* 记录快速路径期间TTS播报完成时间，用于5秒强制释放检测 */
          if (s_feishu_fast_path && s_fast_path_tts_started && s_fast_path_tts_done_ts == 0) {
            s_fast_path_tts_done_ts = s_tts_echo_end_ts;
          }
        }
        if (get_time_ms() - s_tts_echo_end_ts >= ECHO_SETTLE_MS)
          s_tts_echo_active = false;
      }

      /* ═══ Omni (cloud LLM) playback echo suppression tracking ═══ */
      /* 类比 s_tts_echo_active：Omni 播报期间持续阻止 mic 上传。
       * s_omni_echo_active 在 response.created/delta 时置 true，
       * response.done 关闭 omni_pb 后记录 s_omni_echo_end_ts，
       * 此处轮询等 ECHO_SETTLE_MS 后才清除，放开 mic 上传。
       * 期间 mic 读到的数据会被音频门控丢弃（不上传服务器）。 */
      if (s_omni_echo_active && s_omni_echo_end_ts > 0) {
        if (get_time_ms() - s_omni_echo_end_ts >= ECHO_SETTLE_MS) {
          s_omni_echo_active = false;
          syslog(LOG_INFO, "[%s] Omni echo settle done, mic upload re-enabled\n", TAG);
        }
      }
#endif

      /* ═══ Mic audio reading & sending (dialogue mode only) ═══ */
      if (s_dialogue_active && s_recorder) {
        ssize_t n = media_recorder_read_data(s_recorder, chunk, CHUNK_SIZE);
        if (n < 0) {
          syslog(LOG_WARNING, "[%s] media_recorder_read failed (n=%zd)\n", TAG, n);
          break;
        }

#ifdef CONFIG_MEDIA
        /* Omni 播报回声抑制：s_omni_echo_active 为 true 时（AI 正在播报
         * 或播报刚结束的 settle 窗口内），丢弃 mic 数据不上传服务器，
         * 防止云端 VAD/ASR 把 TTS 回声误识别为用户语音（自问自答）。 */
        if (!s_omni_echo_active && !s_img_sending && !s_tts_echo_active
            && !s_feishu_fast_path) {
#else
        if (!s_img_sending && !s_feishu_fast_path) {
#endif
          if (send_buf_used + n <= SEND_CHUNK_SIZE) {
            memcpy(send_buf + send_buf_used, chunk, n);
            send_buf_used += n;
          }

          if (send_buf_used >= SEND_CHUNK_SIZE) {
            if (!s_asr_stream) {
              /* ASR流已被快速路径关闭，丢弃缓冲数据，等待重连 */
              send_buf_used = 0;
            } else {
              ret = dashscope_asr_stream_send(s_asr_stream, send_buf, send_buf_used);
              if (ret != 0) {
                syslog(LOG_WARNING, "[%s] dashscope_asr_stream_send failed (ret=%d)\n", TAG, ret);
                break;
              }
              send_count++;
              send_buf_used = 0;
            }
          }
#ifdef CONFIG_MEDIA
        }
#endif
      } else if (!s_dialogue_active || !s_recorder) {
        /* Idle mode: sleep to avoid busy loop */
        usleep(IDLE_LOOP_SLEEP_US);
      }

      /* ═══ Receive events (always) ═══ */
      if (s_asr_stream) {
        while (dashscope_asr_stream_vad_done(s_asr_stream)) {
          int nrecv = dashscope_asr_stream_recv(s_asr_stream, recv_buf, WS_RECV_BUF);
          if (nrecv > 0) {
            recv_buf[nrecv] = '\0';
            process_dashscope_event(recv_buf);
            /* 快速路径期间，每处理完一个云端事件后break，让外层循环执行
             * TTS跟踪和快速路径释放检查。否则dashscope_asr_stream_recv()
             * 会阻塞等待下一个事件，导致主循环卡死，s_fast_path_tts_started
             * 永远不会被设置，状态卡在"文档查询"直到90s硬超时。 */
            if (s_feishu_fast_path)
              break;
          } else if (nrecv == -2) {
            continue;
          } else {
            if (nrecv < 0 && nrecv != -EAGAIN && nrecv != -EWOULDBLOCK) {
              syslog(LOG_WARNING, "[%s] dashscope_asr_stream_recv failed (nrecv=%d)\n", TAG, nrecv);
            }
            break;
          }
        }
      }

      /* ═══ Ping (always) ═══ */
      uint64_t current_time = get_time_ms();
      /* 空闲期（无待发送音频、非TTS播报）用更短间隔ping，更快检测
       * 服务端静默断连，避免用户说话时才发现连接已断。 */
      bool is_idle = (send_buf_used == 0) && !s_response_active;
      uint32_t ping_interval = is_idle ? PING_IDLE_INTERVAL_MS : PING_INTERVAL_MS;
      if (current_time - last_ping_time >= ping_interval) {
        if (!s_asr_stream) {
          /* ASR流已关闭，跳过ping，等待重连 */
          last_ping_time = current_time;
        } else {
          int ping_ret = dashscope_asr_stream_send_ping(s_asr_stream);
          if (ping_ret == 0) {
            last_ping_time = current_time;
          } else {
            syslog(LOG_WARNING, "[%s] Failed to send ping (ret=%d), connection lost\n", TAG, ping_ret);
            break;
          }
        }
      }

      /* ═══ Silence timeout (dialogue mode only) ═══ */
      /* 双向静音超时：mic上行、TTS播报、大模型omni播报全部结束后，
       * 30秒无任何活动才触发超时退出。
       * - s_feishu_fast_path: 飞书快速路径处理中不触发，持续刷新活动戳
       * - voice_channel_is_speaking(): 本地TTS播放中不触发
       * - s_response_active: 云端omni音频播放中不触发
       * - s_tts_echo_active: TTS回声settling期间不触发
       * - 飞书快速路径开始后90秒内使用更长超时，等待工具执行+LLM总结 */
      if (s_dialogue_active && s_last_dialogue_activity > 0 &&
          !s_feishu_fast_path &&
          !voice_channel_is_speaking() && !s_response_active
          && !s_tts_echo_active) {
        /* 飞书对话模式使用更长的静音超时（600s）
         * 飞书快速路径开始后90秒内使用90s超时（等待工具+LLM）
         * 普通模式30s */
        uint64_t timeout_ms;
        if (s_feishu_conversation_mode) {
          timeout_ms = FEISHU_CONV_SILENCE_TIMEOUT_MS;
        } else if (s_feishu_fast_path_start_ts > 0 &&
                   (current_time - s_feishu_fast_path_start_ts) < FEISHU_FAST_PATH_SILENCE_TIMEOUT_MS) {
          timeout_ms = FEISHU_FAST_PATH_SILENCE_TIMEOUT_MS;
        } else {
          timeout_ms = SILENCE_TIMEOUT_MS;
        }
        if ((current_time - s_last_dialogue_activity) >= timeout_ms) {
        syslog(LOG_INFO, "[%s] Silence timeout (%llu ms), exiting dialogue\n",
               TAG, (unsigned long long)(current_time - s_last_dialogue_activity));

        va_dialogue_exit_cleanup();

#ifdef CONFIG_AI_AGENT_FEISHU
        /* 静音超时：自动退出飞书对话模式 */
        if (s_feishu_conversation_mode) {
          syslog(LOG_INFO, "[%s] [FEISHU_CONV] silence timeout, auto-exiting conversation mode\n", TAG);
          voice_assistant_exit_feishu_conversation();
        }
#endif

        /* 先播放提示音（此时 VAD 尚未启动，do_play_prompt 不会与 VAD recorder 冲突）。
         * do_play_prompt 是同步阻塞的，播完后才返回。
         * 不能先启动 VAD 再播放：do_play_prompt 会关闭 VAD 的 s_recorder 导致检测失效。 */
        wakeup_detector_play_prompt(i18n_get_prompt(PROMPT_SILENCE_TIMEOUT));

        /* 提示音播完后启动本地 VAD */
        va_maybe_start_wakeup();
        voice_assistant_update_status("正在监听...");
        }  /* end of timeout check */
      }  /* end of dialogue active check */

      /* ═══ Feishu fast path: TTS播放完成后释放mic ═══ */
      /* TTS播放完成且echo settling后(ECHO_SETTLE_MS=500ms)，重置快速路径标志，
       * 恢复mic上传。避免TTS播完后mic被长时间抑制直到60s硬超时。
       * voice_channel_is_speaking()返回false + s_tts_echo_active为false
       * 表示TTS已播放完且回声已消散。
       * 
       * 重置条件：快速路径激活时间 >= MIN_HOLD 且 内容已收到 且 TTS已开始并已完成
       * 必须检查s_fast_path_content_received：确保消息过滤结果已收到
       * 必须检查s_fast_path_tts_started：确保TTS已播放过
       * 如果工具还在执行（如飞书API调用+消息过滤），不能提前释放快速路径，
       * 否则状态会被错误地切回“正在监听”。
       * 
       * 这处理了两种情况：
       * 1. 正常流程：TTS播放时间较长，主循环检测到voice_channel_is_speaking()
       * 2. 快速完成：TTS播放很快（如"今日暂无消息"仅2.9秒），主循环来不及
       *    检测，但TTS实际已完成，此时也应重置标志恢复mic */
      
      /* 调试日志：快速路径激活且超过MIN_HOLD但仍未释放时，每5秒输出一次各条件状态 */
      if (s_feishu_fast_path && s_feishu_fast_path_start_ts > 0
          && (current_time - s_feishu_fast_path_start_ts) >= FEISHU_FAST_PATH_MIN_HOLD_MS) {
        static uint64_t s_fp_debug_last_log = 0;
        if (current_time - s_fp_debug_last_log >= 5000) {
          s_fp_debug_last_log = current_time;
          syslog(LOG_INFO,
              "[%s] [FEISHU_FAST_PATH] release pending: content_recv=%d tts_started=%d speaking=%d tts_echo=%d resp_active=%d held=%llu ms\n",
              TAG, s_fast_path_content_received, s_fast_path_tts_started,
              voice_channel_is_speaking(), s_tts_echo_active, s_response_active,
              (unsigned long long)(current_time - s_feishu_fast_path_start_ts));
        }
      }
      
      if (s_feishu_fast_path
          && s_fast_path_content_received  /* 消息过滤结果已收到 */
          && s_fast_path_tts_started  /* TTS必须至少播放过一次 */
          /* 不再要求 s_fast_path_omni_done：TTS 播报完成即代表查询内容已呈现，
           * 立即释放 mic、解除右滑锁、切回正常 VAD。omni_done 可能因网络
           * 断连（ECONNRESET）迟迟不到，若继续等待会导致右滑锁卡住直至
           * stall 超时（TTS done + 5s）。 */
          && !voice_channel_is_speaking() && !s_tts_echo_active
          && !s_response_active
          && s_feishu_fast_path_start_ts > 0
          && (current_time - s_feishu_fast_path_start_ts)
             >= FEISHU_FAST_PATH_MIN_HOLD_MS) {
        syslog(LOG_INFO,
            "[%s] Feishu fast path: TTS done, releasing mic (content_received=%d, tts_started=%d, omni_done=%d, held %llu ms)\n",
            TAG, s_fast_path_content_received, s_fast_path_tts_started,
            s_fast_path_omni_done,
            (unsigned long long)(current_time - s_feishu_fast_path_start_ts));
        s_feishu_fast_path = false;
        s_fast_path_tts_started = false;
        s_fast_path_content_received = false;
        s_fast_path_omni_done = false;
        s_fast_path_tts_done_ts = 0;
        voice_assistant_update_status(
            s_feishu_conversation_mode ? "飞书对话" : "正在监听...");
        /* 重新打开mic录音机，恢复用户语音输入。
         * ASR流由dashscope线程自动重连（检测到s_asr_stream=NULL后会重新连接）。
         * 失败时重试一次，仍失败则退出对话模式。 */
        if (s_dialogue_active && !s_recorder) {
          if (dialogue_open_mic(&s_recorder) != 0) {
            syslog(LOG_WARNING, "[%s] [FEISHU_FAST_PATH] mic reopen failed, retrying...\n", TAG);
            usleep(100000);  /* 100ms */
            if (dialogue_open_mic(&s_recorder) != 0) {
              syslog(LOG_ERR, "[%s] [FEISHU_FAST_PATH] mic reopen failed twice, exiting dialogue\n", TAG);
              s_dialogue_active = false;
              va_maybe_start_wakeup();
            } else {
              syslog(LOG_INFO, "[%s] [FEISHU_FAST_PATH] mic reopened on retry (TTS done)\n", TAG);
            }
          } else {
            syslog(LOG_INFO, "[%s] [FEISHU_FAST_PATH] mic reopened after TTS done\n", TAG);
          }
        }
      }

      /* ═══ Feishu fast path: TTS done but release blocked for 5s → force release ═══ */
      /* 条件2：TTS播报完成后5秒内仍未释放（如omni_done未到达、s_response_active
       * 未清除等），强制切换状态，避免用户长时间等待。
       * 与条件1（正常释放）互补：条件1要求omni_done且TTS都完成，
       * 条件2在TTS完成后给5秒宽限期，超时则不再等待omni_done。 */
      if (s_feishu_fast_path && s_fast_path_tts_started
          && s_fast_path_tts_done_ts > 0
          && !voice_channel_is_speaking() && !s_tts_echo_active
          && (current_time - s_fast_path_tts_done_ts) >= FEISHU_FAST_PATH_TTS_STALL_MS) {
        syslog(LOG_WARNING,
            "[%s] Feishu fast path: TTS done but release blocked for %llu ms, force-releasing (omni_done=%d, resp_active=%d)\n",
            TAG, (unsigned long long)(current_time - s_fast_path_tts_done_ts),
            s_fast_path_omni_done, s_response_active);
        s_feishu_fast_path = false;
        s_fast_path_tts_started = false;
        s_fast_path_content_received = false;
        s_fast_path_omni_done = false;
        s_fast_path_tts_done_ts = 0;
        voice_assistant_update_status(
            s_feishu_conversation_mode ? "飞书对话" : "正在监听...");
        if (s_dialogue_active && !s_recorder) {
          if (dialogue_open_mic(&s_recorder) != 0) {
            syslog(LOG_WARNING, "[%s] [FEISHU_FAST_PATH] mic reopen failed, retrying...\n", TAG);
            usleep(100000);
            if (dialogue_open_mic(&s_recorder) != 0) {
              syslog(LOG_ERR, "[%s] [FEISHU_FAST_PATH] mic reopen failed twice, exiting dialogue\n", TAG);
              s_dialogue_active = false;
              va_maybe_start_wakeup();
            } else {
              syslog(LOG_INFO, "[%s] [FEISHU_FAST_PATH] mic reopened on retry (TTS stall)\n", TAG);
            }
          } else {
            syslog(LOG_INFO, "[%s] [FEISHU_FAST_PATH] mic reopened after TTS stall timeout\n", TAG);
          }
        }
      }

      /* ═══ Feishu fast path hard timeout ═══ */
      /* If the fast path flag is still set after 90s, force-reset it
       * so the next silence timeout can fire normally.  This handles
       * cases where the agent reply was lost or TTS failed silently. */
      if (s_feishu_fast_path && s_last_dialogue_activity > 0 &&
          (current_time - s_last_dialogue_activity) >= FEISHU_FAST_PATH_TIMEOUT_MS) {
        syslog(LOG_WARNING,
            "[%s] Feishu fast path hard timeout (%llu ms), force-resetting\n",
            TAG, (unsigned long long)(current_time - s_last_dialogue_activity));
        s_feishu_fast_path = false;
        s_fast_path_tts_started = false;
        s_fast_path_content_received = false;
        s_fast_path_omni_done = false;
        s_fast_path_tts_done_ts = 0;
        /* 恢复状态显示 */
        voice_assistant_update_status(
            s_feishu_conversation_mode ? "飞书对话" : "正在监听...");
        /* 重新打开mic录音机，恢复用户语音输入。
         * 失败时重试一次，仍失败则退出对话模式。 */
        if (s_dialogue_active && !s_recorder) {
          if (dialogue_open_mic(&s_recorder) != 0) {
            syslog(LOG_WARNING, "[%s] [FEISHU_FAST_PATH] mic reopen failed, retrying...\n", TAG);
            usleep(100000);  /* 100ms */
            if (dialogue_open_mic(&s_recorder) != 0) {
              syslog(LOG_ERR, "[%s] [FEISHU_FAST_PATH] mic reopen failed twice, exiting dialogue\n", TAG);
              s_dialogue_active = false;
              va_maybe_start_wakeup();
            } else {
              syslog(LOG_INFO, "[%s] [FEISHU_FAST_PATH] mic reopened on retry (hard timeout)\n", TAG);
            }
          } else {
            syslog(LOG_INFO, "[%s] [FEISHU_FAST_PATH] mic reopened after hard timeout\n", TAG);
          }
        }
      }

      /* ═══ 飞书对话模式延迟退出：TTS播报完成+3s后清除conversation_mode ═══ */
      if (s_pending_conversation_exit) {
        if (!voice_channel_is_speaking() && !s_tts_echo_active) {
          if (s_pending_conversation_exit_ts == 0) {
            s_pending_conversation_exit_ts = current_time;
            syslog(LOG_INFO, "[%s] [FEISHU_CONV] Exit TTS done, will clear conversation mode in 3s\n", TAG);
          } else if ((current_time - s_pending_conversation_exit_ts) >= 3000) {
            s_feishu_conversation_mode = false;
            s_pending_conversation_exit = false;
            s_pending_conversation_exit_ts = 0;
            /* 停止轮询线程 */
            feishu_poll_thread_stop();
            syslog(LOG_INFO, "[%s] [FEISHU_CONV] Conversation mode cleared (3s after exit TTS)\n", TAG);
            /* 重置mic管线：对话模式期间poll线程可能导致SMF管线反复reset，
             * sink缓冲区堆积无人消费。关闭并重新打开mic恢复管线正常。 */
            if (s_dialogue_active && s_recorder) {
              dialogue_close_mic(&s_recorder);
              usleep(50000); /* 50ms等待管线完全停止 */
              if (dialogue_open_mic(&s_recorder) != 0) {
                syslog(LOG_WARNING, "[%s] [FEISHU_CONV] mic reopen failed after exit\n", TAG);
                usleep(100000);
                if (dialogue_open_mic(&s_recorder) != 0) {
                  syslog(LOG_ERR, "[%s] [FEISHU_CONV] mic reopen failed twice, exiting dialogue\n", TAG);
                  s_dialogue_active = false;
                  va_maybe_start_wakeup();
                }
              }
              if (s_recorder) {
                syslog(LOG_INFO, "[%s] [FEISHU_CONV] mic reset after conversation exit\n", TAG);
              }
            }
          }
        } else {
          s_pending_conversation_exit_ts = 0; /* TTS还在播放，重置计时 */
        }
      }

      /* ═══ 通用对话模式延迟退出：等待云端TTS播报完成后清理状态 ═══
       * 退出命令检测到时不立即 omni_pb_close()，让云端回复播完。
       * 当 s_omni_echo_active 清除（AI回复播完+回声消散）时，
       * 状态自然回到"正在监听..."，此处检查退出标记执行清理。 */
      if (s_pending_dialogue_exit && !s_omni_echo_active && !s_response_active) {
        s_pending_dialogue_exit = false;
        syslog(LOG_INFO, "[%s] Exit: cloud TTS done, cleanup and restart VAD\n", TAG);
        va_dialogue_exit_cleanup();
        /* 退出命令场景无提示音播放，直接启动 VAD */
        va_maybe_start_wakeup();
        voice_assistant_update_status("正在监听...");
      }

      /* ═══ 飞书对话模式轮询：通知独立轮询线程执行（不阻塞主线程） ═══ */
      if (s_feishu_conversation_mode && !s_pending_conversation_exit) {
        uint64_t since_poll = current_time - s_last_feishu_poll_time;
        if (since_poll >= FEISHU_POLL_INTERVAL_MS) {
          s_last_feishu_poll_time = current_time;
          /* 通知轮询线程执行HTTPS请求，避免主线程阻塞导致mic管线data lost */
          pthread_mutex_lock(&s_feishu_poll_mutex);
          s_feishu_poll_need_poll = true;
          pthread_cond_signal(&s_feishu_poll_cond);
          pthread_mutex_unlock(&s_feishu_poll_mutex);
        }
      }

#ifdef CONFIG_MEDIA
      /* ═══ Stall watchdog (dialogue mode only) ═══ */
      if (s_dialogue_active && s_response_active && s_last_audio_time > 0 &&
          (current_time - s_last_audio_time) >= RESPONSE_STALL_TIMEOUT_MS) {
        syslog(LOG_WARNING, "[%s] Response stalled (no audio for %llu ms), force-closing playback\n",
               TAG, (unsigned long long)(current_time - s_last_audio_time));
        s_response_active = false;
        s_last_audio_time = 0;
        omni_pb_close();
        if (s_asr_stream) {
          dashscope_asr_stream_clear_buffer(s_asr_stream);
        }
        s_echo_suppress = false;
        s_omni_echo_active = false;
        s_omni_echo_end_ts = 0;
        s_tts_echo_active = false;
        s_tts_echo_end_ts = 0;
        /* 图片分析期间stall，清理图片状态避免卡死 */
        pthread_mutex_lock(&s_img_ctx.lock);
        if (s_img_ctx.state != IMG_STATE_IDLE) {
          syslog(LOG_WARNING, "[AI_IMG] ===== response TIMEOUT, aborting analysis =====\n");
          if (s_img_ctx.data) {
            free(s_img_ctx.data);
            s_img_ctx.data = NULL;
          }
          s_img_ctx.data_len = 0;
          s_img_ctx.state = IMG_STATE_IDLE;
          s_img_sending = false;
          img_notify_status("AI response timeout", true);
        }
        pthread_mutex_unlock(&s_img_ctx.lock);
        /* 飞书对话模式下保持"飞书对话"状态 */
        if (!s_feishu_conversation_mode) {
          voice_assistant_update_status("正在监听...");
        }
      }
#endif
    }

    /* ═══ Connection cleanup ═══ */

    /* Flush remaining send buffer */
    if (send_buf_used > 0 && s_asr_stream) {
      dashscope_asr_stream_send(s_asr_stream, send_buf, send_buf_used);
    }

    free(send_buf);
    free(recv_buf);

#ifdef CONFIG_MEDIA
    omni_pb_close();
    s_response_active = false;
    s_echo_suppress = false;
    s_omni_echo_active = false;
    s_omni_echo_end_ts = 0;
    s_tts_echo_active = false;
    s_tts_echo_end_ts = 0;
#endif

    /* 图片分析状态清理 */
    pthread_mutex_lock(&s_img_ctx.lock);
    if (s_img_ctx.state != IMG_STATE_IDLE) {
      syslog(LOG_ERR, "[AI_IMG] ===== connection LOST, aborting analysis =====\n");
      if (s_img_ctx.data) {
        free(s_img_ctx.data);
        s_img_ctx.data = NULL;
      }
      s_img_ctx.data_len = 0;
      s_img_ctx.state = IMG_STATE_IDLE;
      s_img_sending = false;
      img_notify_status("Disconnected, analysis aborted", true);
    }
    pthread_mutex_unlock(&s_img_ctx.lock);

    /* Close mic if open */
    if (s_recorder) {
      dialogue_close_mic(&s_recorder);
    }

    if (s_asr_stream) {
      dashscope_asr_stream_abort(s_asr_stream);
      s_asr_stream = NULL;
    }

    /* If was in dialogue mode, restart local VAD.
     * 快速路径从不主动关闭 ASR 流（只清缓冲区），所以走到这里一定是
     * 网络错误断连。必须释放快速路径并重启 VAD，否则：
     * 1. 外层重连循环用 usleep 指数退避（3s->60s），期间所有快速路径
     *    释放检查（TTS done / 5s stall / 30s 硬超时）都不执行
     * 2. TTS 播完后无人检查释放条件，快速路径永久卡住
     * 3. 网络持续不可达时，UI 永远卡在"文档查询"状态 */
    if (s_dialogue_active && s_feishu_fast_path) {
      syslog(LOG_WARNING,
          "[%s] ASR stream lost during fast path, force-releasing fast path\n", TAG);
      s_feishu_fast_path = false;
      s_fast_path_tts_started = false;
      s_fast_path_content_received = false;
      s_fast_path_omni_done = false;
      s_fast_path_tts_done_ts = 0;
      s_feishu_fast_path_start_ts = 0;
      s_dialogue_active = false;
      s_last_dialogue_activity = 0;
#ifdef CONFIG_MEDIA
      voice_channel_abort_tts();
      omni_pb_close();
      s_response_active = false;
      s_tts_echo_active = false;
      s_tts_echo_end_ts = 0;
      s_omni_echo_active = false;
      s_omni_echo_end_ts = 0;
#endif
      va_maybe_start_wakeup();
      voice_assistant_update_status("正在监听...");
    } else if (s_dialogue_active) {
      s_dialogue_active = false;
      s_last_dialogue_activity = 0;
      va_maybe_start_wakeup();
      syslog(LOG_INFO, "[%s] Was in dialogue mode, restarted local VAD\n", TAG);
    }

    syslog(LOG_INFO, "[%s] Disconnected from DashScope\n", TAG);

    /* Notify lvgldemo immediately: the ASR stream dropped, which usually
     * means WiFi disconnected.  The callback checks wifi_get_if_flags()
     * and only triggers on_network_disconnected() if WiFi is actually
     * down, so false positives (server-side disconnect) are harmless. */
    if (s_disconnect_cb) {
      s_disconnect_cb();
    }

    /* 快速路径/飞书对话期间不更新UI状态 */
    if (!s_feishu_fast_path && !s_feishu_conversation_mode) {
      voice_assistant_update_status("网络连接中...");
    }

    if (s_va_running) {
      usleep(RECONNECT_DELAY_MS * 1000);
    }
  }

  free(chunk);

  /* Final cleanup: restart VAD if was in dialogue */
  if (s_dialogue_active) {
    s_dialogue_active = false;
    s_last_dialogue_activity = 0;
    va_maybe_start_wakeup();
  }

  syslog(LOG_INFO, "[%s] DashScope thread exited\n", TAG);
  return NULL;
}

/* Async callback for thread-safe status label update */
static void va_update_status_async_cb(void * data)
{
    char * text = (char *)data;
    if (!text) return;

    size_t free_heap = lvgldemo_get_free_heap();
    size_t text_len = strlen(text);
    size_t estimated_need = text_len + 512;
    if (free_heap < estimated_need) {
        syslog(LOG_WARNING, "[%s] heap low (%u bytes free, need ~%u), skip wakeup status update\n",
               TAG, (unsigned)free_heap, (unsigned)estimated_need);
        free(text);
        return;
    }

    if (ai_wakeup_label) {
        lv_label_set_text(ai_wakeup_label, text);
    }
    free(text);
}

void voice_assistant_update_status(const char *status)
{
    /* Bug1: 单点国际化。调用方仍传中文常量，这里根据当前语言映射为 i18n 文案，
     * 确保切换语言后 AI 页运行时状态（正在监听/AI回复中/飞书意图等）随之刷新。 */
    if (status) {
        if (strcmp(status, "正在监听...") == 0) status = i18n_get(STR_AI_LISTENING);
        else if (strcmp(status, "AI回复中...") == 0) status = i18n_get(STR_AI_REPLYING);
        else if (strcmp(status, "网络连接中...") == 0) status = i18n_get(STR_AI_NET_CONNECTING);
        else if (strcmp(status, "网络已断开") == 0) status = i18n_get(STR_AI_NET_DISCONNECTED);
        else if (strcmp(status, "飞书对话") == 0) status = i18n_get(STR_FEISHU_CONV);
        else if (strcmp(status, "消息筛选") == 0) status = i18n_get(STR_FEISHU_MSG_FILTER);
        else if (strcmp(status, "文档查询") == 0) status = i18n_get(STR_FEISHU_DOC_QUERY);
    }
    char *buf = strdup(status);
    if (buf) {
        if (!lvgl_dispatch_async(va_update_status_async_cb, buf)) {
            free(buf);
        }
    }
}

int voice_assistant_init(void)
{
  if (s_va_initialized) {
    return 0;
  }

  syslog(LOG_INFO, "[%s] init\n", TAG);

#ifdef CONFIG_AI_AGENT_FEISHU
  /* 注册飞书文档创建回调，文档创建成功后自动设置活跃文档用于后续写入 */
  feishu_set_doc_created_callback(feishu_doc_created_callback, NULL);
#endif

  s_va_initialized = true;
  return 0;
}

int voice_assistant_start(void)
{
  /* 防重入：基于 voice_assistant 资源状态标记，已开启则不允许再开启，只能 stop */
  if (s_va_running) {
    return 0;
  }

  syslog(LOG_INFO, "[%s] starting (idle mode)\n", TAG);

  /* 重置 PS 状态缓存，由 voice_channel 的 acquire/release 动态控制。
   * 启动时 BES 驱动默认 PS ON，首次 voice_wifi_ps_set_hook(1) 时切到 OFF。 */
  s_wifi_ps_state = -1;

  s_va_running = true;
  s_dialogue_active = false;
  s_last_dialogue_activity = 0;
  s_recorder = NULL;

  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, 16 * 1024);

  if (pthread_create(&s_va_thread, &attr, dashscope_thread, NULL) != 0) {
    syslog(LOG_ERR, "[%s] Failed to create dashscope thread\n", TAG);
    s_va_running = false;
    return -1;
  }

  pthread_attr_destroy(&attr);

  syslog(LOG_INFO, "[%s] started\n", TAG);
  return 0;
}

int voice_assistant_stop(void)
{
  /* 防重入：基于 voice_assistant 资源状态标记，已关闭则不允许再关闭，只能 start */
  if (!s_va_running) {
    return 0;
  }

  syslog(LOG_INFO, "[%s] stopping\n", TAG);

  /* voice_channel_abort_tts() 设置 tts_abort 标志并停止音频播放。
   *
   * 不再调用 ws_conn_pool_invalidate_tts/asr()：
   * 该函数会 close(fd)，在 NuttX 中可能导致 libuv epoll fd 失效
   * (EBADF 崩溃)。WiFi 断连后 socket 会自然收到 ECONNRESET，
   * 阻塞在 mbedtls_ssl_read() 的线程会自动返回，无需手动 close。
   * TTS/ASR 连接池的 fd 将在 voice_assistant_start() 时由
   * connect_tts_pool() → pool_conn_destroy() 安全清理。 */
#ifdef CONFIG_MEDIA
  voice_channel_abort_tts();
#endif

  /* 清除飞书对话模式标志，必须在 feishu_poll_thread_stop() 之前清除：
   * poll 线程是 detached 的，stop() 仅设标志不 join，线程可能仍在
   * feishu_conv_poll_messages() 中。若 conversation_mode 仍为 true，
   * on_feishu_msg 会继续向 TTS 队列推送消息。
   * 提前清除后，on_feishu_msg 的入口检查 (!s_feishu_conversation_mode)
   * 会直接返回，阻止新消息入队。 */
  if (s_feishu_conversation_mode) {
    syslog(LOG_INFO, "[%s] stop: clearing feishu conversation mode\n", TAG);
    s_feishu_conversation_mode = false;
    s_feishu_conversation_chat_id[0] = '\0';
  }

  /* 停止飞书 TTS worker 线程并清空队列，防止 stop() 后残留消息被播报。
   * s_feishu_tts_running 永不设 false 是原始设计缺陷：worker 线程泄漏且
   * 会在 stop 后继续处理队列残留消息调用 voice_channel_speak()。
   * worker 是 detached 线程，此处仅设标志唤醒它退出，不 join。
   * 同时重置 s_feishu_tts_worker_created，允许下次 start 后重新创建。 */
  if (s_feishu_tts_running) {
    s_feishu_tts_running = false;
    s_feishu_tts_worker_created = false;
    pthread_mutex_lock(&s_feishu_tts_lock);
    s_feishu_tts_head = 0;
    s_feishu_tts_tail = 0;
    pthread_cond_signal(&s_feishu_tts_cond);
    pthread_mutex_unlock(&s_feishu_tts_lock);
    syslog(LOG_INFO, "[%s] stop: feishu TTS worker stopped, queue cleared\n", TAG);
  }

  /* 停止飞书轮询线程，防止 WiFi 断连/驱动 reset 后该线程继续访问
   * 损坏的网络栈触发 CP 核 MemFault（feishu_poll_task → mbedtls → rptun panic）。
   * 必须在 s_va_running=false 之前停止，避免 poll 线程与 VA 主线程退出过程竞争。 */
  feishu_poll_thread_stop();

  s_va_running = false;

  if (s_va_thread != 0) {
    int ret = pthread_join(s_va_thread, NULL);
    if (ret == 0) {
      s_va_thread = 0;
    } else {
      syslog(LOG_WARNING, "[%s] thread join failed (ret=%d)\n", TAG, ret);
      pthread_cancel(s_va_thread);
      s_va_thread = 0;
    }
  }

  /* 清除飞书快速路径标志，解除右滑锁。
   * 否则 voice_assistant_is_exit_blocked() 永远返回 true（主循环已退出，
   * s_feishu_fast_path 的 echo 跟踪清除逻辑永远不会执行），导致用户无法右滑退出 AI 页。 */
  if (s_feishu_fast_path) {
    syslog(LOG_INFO, "[%s] stop: clearing feishu fast path flag\n", TAG);
    s_feishu_fast_path = false;
    s_fast_path_tts_started = false;
    s_fast_path_content_received = false;
    s_fast_path_omni_done = false;
    s_fast_path_tts_done_ts = 0;
  }

  /* 清除所有 pending 标志，防止下次 start() 后主循环误处理残留状态：
   * - s_pending_asr_only_switch: 非飞书对话下切 ASR-only → 用户语音丢失
   * - s_pending_omni_restore: 已在 Omni 模式，重复 restore 无害但浪费
   * - s_pending_conversation_exit: 尝试清除已清除的 conversation_mode
   * - s_pending_dialogue_exit: 触发 va_dialogue_exit_cleanup 干扰新会话 */
  s_pending_asr_only_switch = false;
  s_pending_omni_restore = false;
  s_pending_conversation_exit = false;
  s_pending_conversation_exit_ts = 0;
  s_pending_dialogue_exit = false;

  /* 清除对话模式标志。主循环退出时若 s_feishu_fast_path=true，会跳过
   * s_dialogue_active 清除和 va_maybe_start_wakeup()。虽然 start() 会重置，
   * 但 stop→start 窗口期内 va_maybe_start_wakeup() 会被 s_dialogue_active=true
   * 阻止，影响 VAD 恢复。 */
  if (s_dialogue_active) {
    syslog(LOG_INFO, "[%s] stop: clearing dialogue_active\n", TAG);
    s_dialogue_active = false;
    s_last_dialogue_activity = 0;
  }

  /* 线程退出后强制复位图片分析状态，避免上次分析的残留 state（如 WAITING / COMMITTING）
   * 导致下次 voice_assistant_start 后 is_image_busy 永远返回 true，UI 静默无反应。 */
  pthread_mutex_lock(&s_img_ctx.lock);
  if (s_img_ctx.state != IMG_STATE_IDLE || s_img_ctx.data) {
    syslog(LOG_WARNING, "[AI_IMG] stop: force-reset img_ctx state=%d data=%p\n",
           s_img_ctx.state, s_img_ctx.data);
    if (s_img_ctx.data) {
      free(s_img_ctx.data);
      s_img_ctx.data = NULL;
    }
    s_img_ctx.data_len = 0;
    s_img_ctx.state = IMG_STATE_IDLE;
  }
  pthread_mutex_unlock(&s_img_ctx.lock);
  s_img_sending = false;

  syslog(LOG_INFO, "[%s] stopped\n", TAG);
  return 0;
}

void voice_assistant_deinit(void)
{
  voice_assistant_stop();
  s_va_initialized = false;
  syslog(LOG_INFO, "[%s] deinit\n", TAG);
}

bool voice_assistant_is_running(void)
{
  return s_va_running;
}

bool voice_assistant_is_connected(void)
{
  return s_va_running && s_asr_stream != NULL;
}

int voice_assistant_enter_dialogue(void)
{
  if (!s_va_running) {
    syslog(LOG_WARNING, "[%s] Cannot enter dialogue: not running\n", TAG);
    return -1;
  }

  /* 防重入：基于 dialogue 资源状态标记，已开启则不允许再开启，只能 exit_dialogue */
  if (s_dialogue_active) {
    return 0;
  }

  syslog(LOG_INFO, "[%s] Enter dialogue mode requested\n", TAG);
  s_dialogue_active = true;
  s_last_dialogue_activity = get_time_ms();
  va_set_dialog_state(VA_STATE_IDLE);
  /* The dashscope_thread will detect this flag and:
   * 1. Stop wakeup_detector (frees mic hardware)
   * 2. Open media_recorder (cloud mic)
   * 3. Start streaming audio to cloud
   * 4. Initialize silence timeout timer */
  return 0;
}

int voice_assistant_exit_dialogue(void)
{
  /* 防重入：基于 dialogue 资源状态标记，已关闭则不允许再关闭，只能 enter_dialogue */
  if (!s_dialogue_active) {
    return 0;
  }

  syslog(LOG_INFO, "[%s] Exit dialogue mode requested\n", TAG);
  s_dialogue_active = false;
  s_last_dialogue_activity = 0;
  va_set_dialog_state(VA_STATE_IDLE);
  /* The dashscope_thread will detect this flag and:
   * 1. Close media_recorder (cloud mic)
   * 2. Start wakeup_detector (local VAD)
   * 3. Stop streaming audio */
  return 0;
}

void voice_assistant_suspend_listening(void)
{
  /* 防重入：基于 listening 资源状态标记，已关闭则不允许再关闭，只能 resume_listening */
  if (s_listening_suspended) {
    return;
  }

  syslog(LOG_INFO, "[%s] Suspend listening (stop VAD + cloud mic + audio)\n", TAG);
  s_listening_suspended = true;
  voice_assistant_exit_dialogue();   /* close cloud mic if active */
  wakeup_detector_stop();            /* stop local VAD (wake word) */

  /* 停止所有正在播放的音频，防止退出 AI 页面后残余音频继续播报 */
#ifdef CONFIG_MEDIA
  if (s_omni_pb_active || s_omni_consumer_running) {
    omni_pb_close();
  }
  voice_channel_abort_tts();
  s_response_active = false;
  s_echo_suppress = false;
  s_omni_echo_active = false;
  s_omni_echo_end_ts = 0;
  s_tts_echo_active = false;
  s_tts_echo_end_ts = 0;
#endif

  /* 清除云端ASR音频缓冲区，防止退出后云端继续处理残留音频并在下次进入时
   * 推送上一次对话的响应（input_audio_buffer.clear 命令丢弃云端已接收音频） */
  if (s_asr_stream) {
    dashscope_asr_stream_clear_buffer(s_asr_stream);
    syslog(LOG_INFO, "[%s] suspend: ASR buffer cleared\n", TAG);
  }

  /* 清除飞书快速路径标志，解除右滑锁 */
  if (s_feishu_fast_path) {
    syslog(LOG_INFO, "[%s] suspend: clearing feishu fast path\n", TAG);
    s_feishu_fast_path = false;
    s_fast_path_tts_started = false;
    s_fast_path_content_received = false;
    s_fast_path_omni_done = false;
    s_fast_path_tts_done_ts = 0;
  }

  /* 清除 pending 标志，防止主循环误处理残留状态 */
  s_pending_dialogue_exit = false;
  s_pending_conversation_exit = false;
  s_pending_conversation_exit_ts = 0;

  va_set_dialog_state(VA_STATE_IDLE);
}

void voice_assistant_resume_listening(void)
{
  /* 防重入：基于 listening 资源状态标记，已开启则不允许再开启，只能 suspend_listening */
  if (!s_listening_suspended) {
    return;
  }
  syslog(LOG_INFO, "[%s] Resume listening (clear suspend flag, VAD stays off)\n", TAG);
  s_listening_suspended = false;
  /* 不在此重启本地 VAD：退出 camera 后云端 mic 与本地 VAD 均保持关闭，
   * 由各界面（如语音助手界面）按需自行开启 mic，避免界面间 mic 状态混乱。 */
}

bool voice_assistant_is_listening_suspended(void)
{
  return s_listening_suspended;
}

bool voice_assistant_is_dialogue_active(void)
{
  return s_dialogue_active;
}

void voice_assistant_reset_feishu_fast_path(void)
{
  if (s_feishu_fast_path) {
    syslog(LOG_INFO, "[%s] Feishu fast path reset by agent (Layer2 no match)\n", TAG);
    s_feishu_fast_path = false;
    s_fast_path_tts_started = false;
    s_fast_path_content_received = false;
    s_fast_path_omni_done = false;
    s_fast_path_tts_done_ts = 0;
    /* 恢复状态显示 */
    voice_assistant_update_status(
        s_feishu_conversation_mode ? "飞书对话" : "正在监听...");
    /* 重新打开mic录音机，恢复用户语音输入。
     * 失败时重试一次，仍失败则退出对话模式。 */
    if (s_dialogue_active && !s_recorder) {
      if (dialogue_open_mic(&s_recorder) != 0) {
        syslog(LOG_WARNING, "[%s] [FEISHU_FAST_PATH] mic reopen failed, retrying...\n", TAG);
        usleep(100000);  /* 100ms */
        if (dialogue_open_mic(&s_recorder) != 0) {
          syslog(LOG_ERR, "[%s] [FEISHU_FAST_PATH] mic reopen failed twice, exiting dialogue\n", TAG);
          s_dialogue_active = false;
          va_maybe_start_wakeup();
        } else {
          syslog(LOG_INFO, "[%s] [FEISHU_FAST_PATH] mic reopened on retry (agent reset)\n", TAG);
        }
      } else {
        syslog(LOG_INFO, "[%s] [FEISHU_FAST_PATH] mic reopened after agent reset\n", TAG);
      }
    }
  }
}

/* 请求刷新会话指令（语言切换后调用）。
 * 仅置标志位，由主循环在 dashscope_thread 检测并发送 session.update。 */
void voice_assistant_request_lang_refresh(void)
{
  s_pending_lang_refresh = true;
  syslog(LOG_INFO, "[%s] Lang refresh requested\n", TAG);
}

/* ── 图片AI分析 API ───────────────────────────────────────────── */

int voice_assistant_analyze_image(const char *image_path)
{
  syslog(LOG_INFO, "[AI_IMG] ===== analyze_image request: %s =====\n", image_path ? image_path : "(null)");
  if (!image_path) return -EINVAL;
  if (!s_va_running) {
    syslog(LOG_WARNING, "[AI_IMG] failed: voice assistant not started\n");
    return -ENOTCONN;
  }
  if (!s_asr_stream) {
    syslog(LOG_WARNING, "[AI_IMG] failed: voice assistant not connected (reconnecting...)\n");
    return -ENOTCONN;
  }
  /* 图片分析不依赖对话模式，只需 WebSocket 连接即可 */
  /* 检查是否已有图片在处理 */
  pthread_mutex_lock(&s_img_ctx.lock);
  if (s_img_ctx.state != IMG_STATE_IDLE) {
    img_state_t st = s_img_ctx.state;
    pthread_mutex_unlock(&s_img_ctx.lock);
    syslog(LOG_WARNING, "[AI_IMG] failed: busy (state=%d)\n", st);
    return -EBUSY;
  }
  /* 预占状态（先设中间态，数据就绪后再切QUEUED，避免主循环竞态） */
  s_img_ctx.state = IMG_STATE_QUEUED;
  pthread_mutex_unlock(&s_img_ctx.lock);

  /* 读取图片文件 */
  int fd = open(image_path, O_RDONLY);
  if (fd < 0) {
    int err = -errno;
    syslog(LOG_ERR, "[AI_IMG] failed: open %s error (%d)\n", image_path, err);
    pthread_mutex_lock(&s_img_ctx.lock);
    s_img_ctx.state = IMG_STATE_IDLE;
    pthread_mutex_unlock(&s_img_ctx.lock);
    return err;
  }

  struct stat st;
  if (fstat(fd, &st) != 0) {
    int err = -errno;
    close(fd);
    pthread_mutex_lock(&s_img_ctx.lock);
    s_img_ctx.state = IMG_STATE_IDLE;
    pthread_mutex_unlock(&s_img_ctx.lock);
    return err;
  }

  size_t file_size = (size_t)st.st_size;
  /* 官方限制：Base64编码前 ≤ 500KB */
  if (file_size == 0 || file_size > 500 * 1024) {
    close(fd);
    syslog(LOG_ERR, "[AI_IMG] failed: file size %zu invalid (max 500KB)\n", file_size);
    pthread_mutex_lock(&s_img_ctx.lock);
    s_img_ctx.state = IMG_STATE_IDLE;
    pthread_mutex_unlock(&s_img_ctx.lock);
    return (file_size == 0) ? -EINVAL : -EFBIG;
  }

  unsigned char *img_data = malloc(file_size);
  if (!img_data) {
    close(fd);
    pthread_mutex_lock(&s_img_ctx.lock);
    s_img_ctx.state = IMG_STATE_IDLE;
    pthread_mutex_unlock(&s_img_ctx.lock);
    return -ENOMEM;
  }

  size_t total_read = 0;
  while (total_read < file_size) {
    ssize_t n = read(fd, img_data + total_read, file_size - total_read);
    if (n < 0) {
      if (errno == EINTR) continue;
      int err = -errno;
      free(img_data);
      close(fd);
      pthread_mutex_lock(&s_img_ctx.lock);
      s_img_ctx.state = IMG_STATE_IDLE;
      pthread_mutex_unlock(&s_img_ctx.lock);
      return err;
    }
    if (n == 0) break;
    total_read += (size_t)n;
  }
  close(fd);

  if (total_read != file_size) {
    free(img_data);
    pthread_mutex_lock(&s_img_ctx.lock);
    s_img_ctx.state = IMG_STATE_IDLE;
    pthread_mutex_unlock(&s_img_ctx.lock);
    return -EIO;
  }

  /* 先设置数据，再切到 QUEUED 状态，避免主循环在数据就绪前检测到 QUEUED */
  pthread_mutex_lock(&s_img_ctx.lock);
  /* 释放旧数据（如果有） */
  if (s_img_ctx.data) {
    free(s_img_ctx.data);
    s_img_ctx.data = NULL;
  }
  s_img_ctx.data = img_data;
  s_img_ctx.data_len = file_size;
  strncpy(s_img_ctx.filename, image_path, sizeof(s_img_ctx.filename) - 1);
  s_img_ctx.filename[sizeof(s_img_ctx.filename) - 1] = '\0';
  /* 数据就绪，现在安全切换到 QUEUED */
  s_img_ctx.state = IMG_STATE_QUEUED;
  syslog(LOG_INFO, "[AI_IMG] ===== image QUEUED: %s (%zu bytes) =====\n", image_path, file_size);

#ifdef CONFIG_AI_AGENT_FEISHU
  /* 图片分析入队成功，重置文档写入轮次计数并标记开始：
   * 此时创建文档等工具调用产生的回复已结束，接下来第一个AI回复即为图片分析结果，作为Round 1 */
  s_fwd_ai_count = 0;
  s_fwd_img_analysis_started = true;
  syslog(LOG_INFO, "[FEISHU_DOC] Image analysis queued, doc write round counter reset to 0\n");
#endif

  pthread_mutex_unlock(&s_img_ctx.lock);

  return 0;
}

bool voice_assistant_is_image_busy(void)
{
  bool busy;
  pthread_mutex_lock(&s_img_ctx.lock);
  busy = (s_img_ctx.state != IMG_STATE_IDLE);
  pthread_mutex_unlock(&s_img_ctx.lock);
  return busy;
}

/* 强制复位图片分析状态。供 UI 层在检测到状态机卡死（如多次点击均 busy）时调用。
 * 注意：若 dashscope_thread 正在发送中调用，可能中断当前分析；UI 层应在确认未在进行
 * 有效分析（例如刚启动且 is_image_busy=true 但用户从未成功发起过分析）时使用。 */
void voice_assistant_reset_image_state(void)
{
  pthread_mutex_lock(&s_img_ctx.lock);
  img_state_t prev = s_img_ctx.state;
  if (prev != IMG_STATE_IDLE || s_img_ctx.data) {
    syslog(LOG_WARNING, "[AI_IMG] reset_image_state: state %d -> IDLE, data=%p\n",
           prev, s_img_ctx.data);
    if (s_img_ctx.data) {
      free(s_img_ctx.data);
      s_img_ctx.data = NULL;
    }
    s_img_ctx.data_len = 0;
    s_img_ctx.state = IMG_STATE_IDLE;
  }
  pthread_mutex_unlock(&s_img_ctx.lock);
  s_img_sending = false;
}

void voice_assistant_set_image_status_callback(image_status_cb_t cb)
{
  s_image_status_cb = cb;
}

/* 内部辅助：更新对话状态并触发回调（通过lvgl_dispatch保证UI线程安全） */

/* dispatch 回调函数实现 */
static void va_state_dispatch_cb(void *arg)
{
    va_state_msg_t *msg = (va_state_msg_t *)arg;
    if (msg->cb) msg->cb(msg->state);
    free(msg);
}

static void va_user_asr_dispatch_cb(void *arg)
{
    va_user_asr_msg_t *msg = (va_user_asr_msg_t *)arg;
    if (msg->cb) msg->cb(msg->text);
    free(msg->text);
    free(msg);
}

static void va_ai_reply_dispatch_cb(void *arg)
{
    va_ai_reply_msg_t *msg = (va_ai_reply_msg_t *)arg;
    if (msg->cb) msg->cb(msg->text);
    free(msg->text);
    free(msg);
}

static void va_set_dialog_state(va_dialog_state_t state)
{
  if (s_dialog_state == state) return;
  va_dialog_state_t old = s_dialog_state;
  s_dialog_state = state;

  /* WiFi PS mode: turn OFF when entering active dialogue (user speaking
   * or AI speaking) to minimize downlink jitter; turn ON when going idle.
   * Only trigger on idle<->active boundary crossings to avoid redundant
   * ioctls. Note: PTT (voice_channel) and TTS pipeline manage PS via
   * their own hooks, so here we only handle the Omni realtime path. */
  bool old_active = (old == VA_STATE_USER_SPEAKING || old == VA_STATE_AI_SPEAKING);
  bool new_active = (state == VA_STATE_USER_SPEAKING || state == VA_STATE_AI_SPEAKING);
  if (old_active != new_active) {
    /* new_active: entering dialogue → PS off (true);
     * !new_active: returning to idle → PS on (false). */
    va_wifi_ps_set(new_active);
  }

  if (s_state_cb) {
    va_state_msg_t *m = malloc(sizeof(va_state_msg_t));
    if (m) {
      m->cb = s_state_cb;
      m->state = state;
      lvgl_dispatch_async(va_state_dispatch_cb, m);
    }
  }
}

void voice_assistant_set_user_asr_callback(va_user_asr_cb_t cb)
{
  s_user_asr_cb = cb;
}

void voice_assistant_set_ai_reply_callback(va_ai_reply_cb_t cb)
{
  s_ai_reply_cb = cb;
}

void voice_assistant_set_dialog_state_callback(va_state_cb_t cb)
{
  s_state_cb = cb;
}

void voice_assistant_set_meeting_callback(void (*callback)(void))
{
  s_meeting_callback = callback;
}

#ifdef CONFIG_AI_AGENT_FEISHU
void voice_assistant_set_feishu_forward(const char *receive_id,
                                         const char *id_type, bool enable)
{
  if (enable && receive_id && id_type && receive_id[0]) {
    strncpy(s_fwd_receive_id, receive_id, sizeof(s_fwd_receive_id) - 1);
    s_fwd_receive_id[sizeof(s_fwd_receive_id) - 1] = '\0';
    strncpy(s_fwd_id_type, id_type, sizeof(s_fwd_id_type) - 1);
    s_fwd_id_type[sizeof(s_fwd_id_type) - 1] = '\0';
    s_fwd_ai_count = 0;
    s_fwd_img_analysis_started = false;
    s_need_skip_doc_prefix = false;
    s_last_user_question[0] = '\0';
    s_fwd_enabled = true;
    syslog(LOG_INFO, "[%s] feishu forward ENABLED: %s=%s\n",
           TAG, s_fwd_id_type, s_fwd_receive_id);
  } else {
    if (s_fwd_enabled) {
      syslog(LOG_INFO, "[%s] feishu forward DISABLED (ai_count=%d)\n",
             TAG, s_fwd_ai_count);
    }
    s_fwd_enabled = false;
    s_fwd_ai_count = 0;
    s_fwd_img_analysis_started = false;
    s_need_skip_doc_prefix = false;
    s_last_user_question[0] = '\0';
    s_fwd_receive_id[0] = '\0';
  }
}
#else
void voice_assistant_set_feishu_forward(const char *receive_id,
                                         const char *id_type, bool enable)
{
  (void)receive_id;
  (void)id_type;
  (void)enable;
}
#endif

/* ── 飞书对话模式实现 ─────────────────────────────────────────── */

#ifdef CONFIG_AI_AGENT_FEISHU

/* 解析@指定人模式: "给XXX发消息 ..." / "跟XXX说 ..." / "@XXX ..." */
static bool feishu_conversation_try_mention(const char *text,
                                             char *name_out, size_t name_cap,
                                             char *msg_out, size_t msg_cap)
{
  if (!text || !name_out || !msg_out) return false;

  const char *p = NULL;
  const char *name_start = NULL;
  const char *name_end = NULL;
  const char *msg_start = NULL;

  /* Pattern 1: "给XXX发消息 ..." */
  p = strstr(text, "\xe7\xbb\x99"); /* "给" UTF-8 */
  if (p) {
    name_start = p + 3; /* skip "给" */
    const char *markers[] = { "\xe5\x8f\x91\xe6\xb6\x88\xe6\x81\xaf", /* "发消息" */
                               "\xe8\xaf\xb4", /* "说" */
                               NULL };
    for (int i = 0; markers[i]; i++) {
      const char *m = strstr(name_start, markers[i]);
      if (m && (!name_end || m < name_end)) {
        name_end = m;
        msg_start = m + strlen(markers[i]);
        /* skip leading spaces */
        while (*msg_start == ' ') msg_start++;
      }
    }
  }

  /* Pattern 2: "跟XXX说 ..." */
  if (!name_start) {
    p = strstr(text, "\xe8\xb7\x9f"); /* "跟" UTF-8 */
    if (p) {
      name_start = p + 3;
      const char *m = strstr(name_start, "\xe8\xaf\xb4"); /* "说" */
      if (m) {
        name_end = m;
        msg_start = m + 3; /* skip "说" */
        while (*msg_start == ' ') msg_start++;
      }
    }
  }

  if (!name_start || !name_end || name_end <= name_start) return false;

  /* Extract name (limit to 20 bytes to avoid capturing too much) */
  size_t nlen = (size_t)(name_end - name_start);
  if (nlen > 20) nlen = 20;
  if (nlen >= name_cap) nlen = name_cap - 1;
  memcpy(name_out, name_start, nlen);
  name_out[nlen] = '\0';

  /* Extract message */
  if (msg_start && *msg_start) {
    strncpy(msg_out, msg_start, msg_cap - 1);
    msg_out[msg_cap - 1] = '\0';
  } else {
    msg_out[0] = '\0';
  }

  syslog(LOG_INFO, "[%s] [FEISHU_CONV] mention parsed: name=%s msg=%s\n",
      TAG, name_out, msg_out);
  return true;
}

/* 发送@mention消息到飞书群 */
static void feishu_conversation_send_mention(const char *chat_id,
                                              const char *open_id,
                                              const char *name,
                                              const char *text)
{
  if (!chat_id || !open_id || !name) return;

  /* 构造 @mention content:
   * {"text":"<at user_id=\"ou_xxx\">name</at> message text"} */
  char mention_text[1024];
  if (text && text[0]) {
    snprintf(mention_text, sizeof(mention_text),
             "<at user_id=\"%s\">%s</at> %s", open_id, name, text);
  } else {
    snprintf(mention_text, sizeof(mention_text),
             "<at user_id=\"%s\">%s</at>", open_id, name);
  }

  /* Build JSON body */
  cJSON *content_obj = cJSON_CreateObject();
  cJSON_AddStringToObject(content_obj, "text", mention_text);
  char *content_str = cJSON_PrintUnformatted(content_obj);
  cJSON_Delete(content_obj);
  if (!content_str) return;

  cJSON *body = cJSON_CreateObject();
  cJSON_AddStringToObject(body, "receive_id", chat_id);
  cJSON_AddStringToObject(body, "msg_type", "text");
  cJSON_AddStringToObject(body, "content", content_str);
  free(content_str);

  char *body_str = cJSON_PrintUnformatted(body);
  cJSON_Delete(body);
  if (!body_str) return;

  char resp[1024] = {0};
  int status = feishu_api_post("/open-apis/im/v1/messages?receive_id_type=chat_id",
                                body_str, resp, sizeof(resp));
  free(body_str);

  if (status != 200 && status != 201) {
    syslog(LOG_WARNING, "[%s] [FEISHU_CONV] @mention send failed: HTTP %d\n",
        TAG, status);
  }
}

/* 进入飞书对话模式 */
void voice_assistant_enter_feishu_conversation(const char *chat_id)
{
  if (!chat_id || !chat_id[0]) {
    syslog(LOG_ERR, "[%s] [FEISHU_CONV] enter failed: invalid chat_id\n", TAG);
    return;
  }

  strncpy(s_feishu_conversation_chat_id, chat_id,
          sizeof(s_feishu_conversation_chat_id) - 1);
  s_feishu_conversation_chat_id[sizeof(s_feishu_conversation_chat_id) - 1] = '\0';
  s_feishu_conversation_mode = true;

  /* 清除 Layer1 匹配时设置的飞书快速路径标志。
   * 对话模式下用户语音需直接发送到云端ASR，快速路径标志会阻止音频上传
   * （line 1628 的 !s_feishu_fast_path 条件），必须在此释放。
   * 快速路径仅用于Layer1→Layer2处理期间的短暂保护，进入对话模式后不再需要。 */
  s_feishu_fast_path = false;
  s_fast_path_tts_started = false;
  s_fast_path_content_received = false;
  s_fast_path_omni_done = false;
  s_fast_path_tts_done_ts = 0;

  /* 更新状态显示为飞书对话 */
  voice_assistant_update_status("飞书对话");

  /* 初始化轮询模式：不注册WS回调，改为每5秒轮询获取飞书消息 */
  s_last_feishu_poll_time = get_time_ms();
  /* 清空去重环 */
  memset(s_conv_msgid_ring, 0, sizeof(s_conv_msgid_ring));
  s_conv_msgid_idx = 0;
  memset(s_conv_sent_text_ring, 0, sizeof(s_conv_sent_text_ring));
  memset(s_conv_sent_ts_ring, 0, sizeof(s_conv_sent_ts_ring));
  s_conv_sent_idx = 0;

  /* 延迟切换ASR模式：设置标志，由voice_assistant主循环线程执行
   * （WebSocket只能由主循环线程写入，跨线程写会导致EIO） */
  s_pending_asr_only_switch = true;

  /* 更新对话活动时间 */
  s_last_dialogue_activity = get_time_ms();

  /* 启动独立轮询线程（HTTPS请求不阻塞主线程mic管线） */
  feishu_poll_thread_start();

  syslog(LOG_INFO, "[%s] [FEISHU_CONV] ENTERED: chat_id=%s\n", TAG, chat_id);
}

/* 退出飞书对话模式 */
void voice_assistant_exit_feishu_conversation(void)
{
  if (s_feishu_conversation_mode) {
    syslog(LOG_INFO, "[%s] [FEISHU_CONV] EXITING: chat_id=%s\n",
        TAG, s_feishu_conversation_chat_id);
  }

  /* 不立即设置 s_feishu_conversation_mode = false：
   * 退出TTS"已退出飞书对话模式"播报期间，云端可能仍有in-flight的response事件，
   * 保持 s_feishu_conversation_mode=true 使 response.audio.transcript.done 等
   * 处理器继续过滤云端回复文字，避免AI页面显示一大段不需要的云端回复。
   * 由主循环在TTS播报完成+3s后执行实际退出（含停止poll线程+mic重置）。 */
  s_pending_conversation_exit = true;
  s_pending_conversation_exit_ts = 0; /* 主循环在TTS完成后设置 */
  s_feishu_conversation_chat_id[0] = '\0';

  /* 恢复状态显示 */
  voice_assistant_update_status("正在监听...");

  /* 注销消息回调 */
  feishu_recv_set_conversation_callback(NULL);

  /* 延迟恢复Omni模式：设置标志，由voice_assistant主循环线程执行 */
  s_pending_omni_restore = true;
}

/* 查询飞书对话模式是否激活 */
bool voice_assistant_is_feishu_conversation_active(void)
{
  return s_feishu_conversation_mode;
}

/* 飞书消息回调：由 feishu_recv 在 WS 接收线程调用
 * 职责：UI显示 + TTS播报，不走 agent_bus/LLM */

/* ═══ 飞书TTS专用线程：单线程+消息队列，保证播报顺序 ═══ */
/* TTS变量已在文件前部声明 */

static void* feishu_tts_worker(void* arg)
{
  (void)arg;
  syslog(LOG_INFO, "[%s] [FEISHU_CONV] TTS worker started\n", TAG);

  while (s_feishu_tts_running) {
    char text_buf[256] = {0};
    bool got_msg = false;

    pthread_mutex_lock(&s_feishu_tts_lock);
    while (s_feishu_tts_head == s_feishu_tts_tail && s_feishu_tts_running) {
      pthread_cond_wait(&s_feishu_tts_cond, &s_feishu_tts_lock);
    }
    if (!s_feishu_tts_running && s_feishu_tts_head == s_feishu_tts_tail) {
      pthread_mutex_unlock(&s_feishu_tts_lock);
      break;
    }
    if (s_feishu_tts_head != s_feishu_tts_tail) {
      strncpy(text_buf, s_feishu_tts_queue[s_feishu_tts_tail].text,
              sizeof(text_buf) - 1);
      s_feishu_tts_tail = (s_feishu_tts_tail + 1) % FEISHU_TTS_QUEUE_SIZE;
      got_msg = true;
    }
    pthread_mutex_unlock(&s_feishu_tts_lock);

    if (got_msg) {
#ifdef CONFIG_MEDIA
      voice_channel_speak(text_buf);
#endif
      s_last_dialogue_activity = get_time_ms();
    }
  }

  syslog(LOG_INFO, "[%s] [FEISHU_CONV] TTS worker exited\n", TAG);
  s_feishu_tts_worker_created = false;  /* 允许 ensure_started 重新创建 */
  return NULL;
}

static void feishu_tts_worker_ensure_started(void)
{
  if (s_feishu_tts_worker_created) return;

  /* NuttX下静态初始化器(PTHREAD_MUTEX_INITIALIZER)不可靠，必须运行时初始化 */
  if (!s_feishu_tts_va_initialized) {
    pthread_mutex_init(&s_feishu_tts_lock, NULL);
    pthread_cond_init(&s_feishu_tts_cond, NULL);
    s_feishu_tts_va_initialized = true;
  }

  s_feishu_tts_running = true;
  pthread_t tid;
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  pthread_attr_setstacksize(&attr, 4096);
  if (pthread_create(&tid, &attr, feishu_tts_worker, NULL) == 0) {
    s_feishu_tts_worker_created = true;
  }
  pthread_attr_destroy(&attr);
}

void voice_assistant_on_feishu_msg(const char *chat_id,
                                    const char *sender_name,
                                    const char *text)
{
  if (!s_feishu_conversation_mode) return;
  if (!text || !text[0]) return;

  /* 构造显示文本: "张三: 消息内容" */
  char display_text[256];
  if (sender_name && sender_name[0] && strcmp(sender_name, "unknown") != 0) {
    snprintf(display_text, sizeof(display_text), "%s: %s", sender_name, text);
  } else {
    snprintf(display_text, sizeof(display_text), "%s", text);
  }
  display_text[sizeof(display_text) - 1] = '\0';

  syslog(LOG_INFO, "[%s] [FEISHU_CONV] incoming: %s\n", TAG, display_text);

  /* UI显示 */
  ai_page_show_omni_response(display_text);

  /* TTS播报：入队由专用TTS线程按序处理，避免多线程竞争导致乱序 */
  feishu_tts_worker_ensure_started();

  pthread_mutex_lock(&s_feishu_tts_lock);
  int next_head = (s_feishu_tts_head + 1) % FEISHU_TTS_QUEUE_SIZE;
  if (next_head == s_feishu_tts_tail) {
    /* 队列满，丢弃最旧的消息 */
    syslog(LOG_WARNING, "[%s] [FEISHU_CONV] TTS queue full, dropping oldest\n", TAG);
    s_feishu_tts_tail = (s_feishu_tts_tail + 1) % FEISHU_TTS_QUEUE_SIZE;
  }
  strncpy(s_feishu_tts_queue[s_feishu_tts_head].text, display_text,
          sizeof(s_feishu_tts_queue[0].text) - 1);
  s_feishu_tts_queue[s_feishu_tts_head].text[sizeof(s_feishu_tts_queue[0].text) - 1] = '\0';
  s_feishu_tts_head = next_head;
  pthread_cond_signal(&s_feishu_tts_cond);
  pthread_mutex_unlock(&s_feishu_tts_lock);
}

#else /* !CONFIG_AI_AGENT_FEISHU */

void voice_assistant_enter_feishu_conversation(const char *chat_id) { (void)chat_id; }
void voice_assistant_exit_feishu_conversation(void) {}
bool voice_assistant_is_feishu_conversation_active(void) { return false; }
void voice_assistant_on_feishu_msg(const char *c, const char *s, const char *t) {
  (void)c; (void)s; (void)t;
}

#endif /* CONFIG_AI_AGENT_FEISHU */
