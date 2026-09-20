/* camera_feishu_doc.c
 *
 * Camera 页进入时异步创建飞书文档，标题 "图文智录_YYYYMMDD_HHMMSS"。
 * 本会话内所有拍照的图片+AI回复追加到该文档，退出 camera 回 menu 时取消。
 *
 * 参考 meeting_feishu_sync.c 的实现，区别：
 *  - folder_token 硬编码 camera 专属文件夹
 *  - 异步线程创建（meeting 是同步调用），用户无感
 *  - 用 voice_assistant_set_doc_create_silent 让 doc_created 回调走 silent 版
 *    （本地创建无 LLM 介入，不置 s_need_skip_doc_prefix，无需截断前缀）
 *
 * 回调机制：tool_feishu_doc_create_execute 内部成功后会调
 * feishu_doc_notify_created → voice_assistant 的 feishu_doc_created_callback
 * 自动设置活跃文档。我们只需在调用前后包裹 silent 标志控制用哪个 setter。
 *
 * 线程控制（generation 方案）：
 *   - 每次启动线程递增 s_generation，线程持有自己的 generation 快照
 *   - cancel 递增 generation 让所有在跑线程失效
 *   - 线程在 create 返回后对比 generation，不匹配则清空回调可能已设的文档
 *   - 不 join（HTTPS 可能阻塞数秒，会卡 UI）
 */

#include "camera_feishu_doc.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <syslog.h>
#include <pthread.h>

#include "tools/tool_feishu_doc.h"
#include "channels/feishu_bot.h"
#include "agent_config.h"
#include "voice_assistant.h"

static const char *TAG = "cam_feishu_doc";

/* camera 专属飞书文件夹 token（区别于 meeting 用的默认文件夹） */
#define CAM_FEISHU_FOLDER_TOKEN "CM8nf148hl8CXtd8bSUcbomKnkd"

/* generation：每次启动新线程递增，cancel 也递增。
 * 线程持有启动时的快照，create 返回后对比，不匹配说明已被取代/取消。 */
static volatile unsigned s_generation = 0;
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;

/* 线程主函数：创建文档（回调自动设活跃文档），全程不调 UI 接口，仅 syslog。
 * arg 是启动时的 generation 快照。 */
static void *cam_feishu_doc_thread(void *arg)
{
    unsigned my_gen = (unsigned)(uintptr_t)arg;
    syslog(LOG_INFO, "[%s] thread started (gen=%u)\n", TAG, my_gen);

    /* Step 0: 飞书未配置则静默退出 */
    const char *app_id = feishu_get_app_id();
    if (!app_id || app_id[0] == '\0') {
        syslog(LOG_WARNING, "[%s] Feishu not configured, skip doc create\n", TAG);
        return NULL;
    }

    /* Step 1: 生成标题 "图文智录_YYYYMMDD_HHMMSS"（北京时间） */
    time_t now = time(NULL);
    struct tm tm;
    time_t local_epoch = now + agent_tz_offset_sec();
    gmtime_r(&local_epoch, &tm);

    char title[128];
    snprintf(title, sizeof(title), "图文智录_%04d%02d%02d_%02d%02d%02d",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec);
    syslog(LOG_INFO, "[%s] title=%s\n", TAG, title);

    /* Step 2: 创建飞书文档（传 folder_token 指定 camera 专属文件夹）。
     * 包裹 silent 标志：让内部 doc_created 回调用 silent 版设文档，
     * 不置 s_need_skip_doc_prefix（本地创建无 LLM 介入，无需截断前缀）。
     * camera 进入时语音监听已 suspend，不会与语音创建文档并发。 */
    char create_input[256];
    snprintf(create_input, sizeof(create_input),
             "{\"title\":\"%s\",\"folder_token\":\"%s\"}",
             title, CAM_FEISHU_FOLDER_TOKEN);

    char create_output[2048];
    memset(create_output, 0, sizeof(create_output));

    voice_assistant_set_doc_create_silent(true);
    int ret = tool_feishu_doc_create_execute(create_input,
                                             create_output, sizeof(create_output));
    voice_assistant_set_doc_create_silent(false);

    /* 创建期间被 cancel 或被新线程取代：回调可能已设了文档，清空它。
     * 此时若创建成功，文档已存在于飞书文件夹，成为空文档残留（已确认不处理）。 */
    if (s_generation != my_gen) {
        syslog(LOG_INFO, "[%s] superseded (gen %u→%u), clear doc and exit\n",
               TAG, my_gen, s_generation);
        voice_assistant_set_active_feishu_doc(NULL, NULL);
        return NULL;
    }
    if (ret != 0) {
        syslog(LOG_ERR, "[%s] create failed: %s\n", TAG, create_output);
        return NULL;
    }
    /* 成功：回调已在 create 内部自动设置活跃文档，无需手动设置 */
    syslog(LOG_INFO, "[%s] ====== CAMERA DOC CREATED SUCCESS ======\n", TAG);
    syslog(LOG_INFO, "[%s] response: %.200s\n", TAG, create_output);

    return NULL;
}

void camera_feishu_doc_create_async(void)
{
    pthread_mutex_lock(&s_lock);
    /* 递增 generation：若上一轮线程仍在跑，它会因 generation 不匹配而自动退出 */
    unsigned my_gen = ++s_generation;

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    /* 飞书HTTPS请求需要较大栈：调用链 tool_feishu_doc_create_execute →
     * feishu_get_app_token → vela_https_request → tls_ctx_connect →
     * mbedTLS 握手（RSA 临时栈约4KB+）+ cJSON + syslog 格式化。
     * 8192 实测会 Stack Overflow，与 camera_page.c 飞书上传线程对齐用 128KB。 */
    pthread_attr_setstacksize(&attr, 128 * 1024);
    int ret = pthread_create(NULL, &attr, cam_feishu_doc_thread,
                             (void *)(uintptr_t)my_gen);
    pthread_attr_destroy(&attr);

    if (ret != 0) {
        syslog(LOG_ERR, "[%s] pthread_create failed: %d\n", TAG, ret);
    }
    pthread_mutex_unlock(&s_lock);
}

void camera_feishu_doc_cancel(void)
{
    pthread_mutex_lock(&s_lock);
    /* 递增 generation 让所有在跑线程失效 */
    s_generation++;
    pthread_mutex_unlock(&s_lock);

    /* 清空活跃文档：用原版（清空时与 silent 等价，都会置 s_need_skip_doc_prefix=false）。
     * 正常退出场景下文档已建好，清掉后下次进入会重建新的；
     * 若创建线程还在跑，清活跃文档可防止它完成后回调设置的残留文档状态
     * （generation 检查已能阻止并清空，这里是双保险）。 */
    voice_assistant_set_active_feishu_doc(NULL, NULL);
    syslog(LOG_INFO, "[%s] cancelled, active doc cleared\n", TAG);
}
