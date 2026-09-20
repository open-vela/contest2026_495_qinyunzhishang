/****************************************************************************
 * apps/examples/lvgldemo/meeting_asr.c
 *
 * Meeting ASR module - transcribe WAV file via DashScope Paraformer
 * realtime WebSocket API.
 *
 * Protocol: wss://dashscope.aliyuncs.com/api-ws/v1/inference
 * Auth: Bearer <api_key> in WS upgrade header
 * Flow:
 *   1. TLS connect + WebSocket upgrade
 *   2. Send run-task (JSON, model=paraformer-realtime-v2)
 *   3. Send binary audio chunks (PCM from WAV)
 *   4. Receive result-generated events (transcript text)
 *   5. Send finish-task
 *   6. Receive task-finished
 *
 * API Key: reused from agent_config.h (AGENT_SECRET_API_KEY).
 *
 ****************************************************************************/

#include <nuttx/config.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <syslog.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/time.h>

#include <mbedtls/base64.h>
#include <mbedtls/ctr_drbg.h>

#ifdef CONFIG_MEDIA
#include <media_recorder.h>
#endif
#include <pthread.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>

#include "meeting_asr.h"

/* Reuse the DashScope API key from ai_agent config. */
#include "agent_config.h"
#include "agent_compat.h"

static const char *TAG = "meeting_asr";

#define ASR_HOST         "dashscope.aliyuncs.com"
#define ASR_PORT         "443"
#define ASR_WS_PATH      "/api-ws/v1/inference"
#define ASR_LANG_HINTS_PATH "/emmc/asr_lang_hints"
#define ASR_RAW_DEBUG_PATH  "/emmc/asr_debug_raw"

/* 多语言验证：语言代码从 /emmc/asr_lang_hints 读取（内容如 "ja" 或
 * "zh,en,ja"），文件不存在时默认全部 8 语种（paraformer-realtime-v2
 * 支持的全部语言：zh/en/ja/yue/ko/de/fr/ru），模型在集合内自动检测。
 * 单一语种质量最佳时可用文件收窄（如 echo ja > ...）。
 * RAW 帧调试日志由独立的 /emmc/asr_debug_raw 控制，与语言配置解耦：
 * 质量验证需最小干扰；需要区分识别问题/字体缺字时 touch 该文件开启 */
static int s_asr_test_mode = 0;

static void asr_fill_lang_hints(char *out, size_t cap)
{
    char raw[64] = "";
    FILE *f = fopen(ASR_LANG_HINTS_PATH, "r");
    if (f) {
        size_t n = fread(raw, 1, sizeof(raw) - 1, f);
        fclose(f);
        raw[n] = '\0';
    }
    /* 每次会议开始时刷新（含清零），文件删除后标志不残留 */
    FILE *dbg = fopen(ASR_RAW_DEBUG_PATH, "r");
    s_asr_test_mode = (dbg != NULL);
    if (dbg) fclose(dbg);
    /* 仅保留字母数字与逗号，防止内容破坏JSON结构 */
    char clean[64];
    size_t ci = 0;
    for (const char *p = raw; *p && ci + 1 < sizeof(clean); p++) {
        char c = *p;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == ',') {
            clean[ci++] = c;
        }
    }
    clean[ci] = '\0';
    out[0] = '\0';
    char *p = clean;
    while (*p) {
        while (*p == ',') p++;
        if (!*p) break;
        char *s = p;
        while (*p && *p != ',') p++;
        if (*p) *p++ = '\0';
        size_t len = strlen(out);
        snprintf(out + len, cap - len, "%s\"%s\"", len ? "," : "", s);
    }
    if (out[0] == '\0') {
        snprintf(out, cap, "\"zh\",\"en\",\"ja\",\"yue\",\"ko\",\"de\",\"fr\",\"ru\"");
    }
    /* 每次启动打印生效语言：该文件在 eMMC 掉电不丢，日语测试后
     * 忘删会导致重启后中文会议仍按日语解码（大量近音错字） */
    syslog(LOG_INFO, "[%s] language_hints=[%s] test_mode=%d\n",
           TAG, out, s_asr_test_mode);
}
#define ASR_WS_KEY_LEN   16
#define ASR_WS_BUF_SIZE  4096
/* 结果帧缓冲：长句累积帧含全量 words 数组可达数 KB，4KB 装不下。
 * 仅供 ASR 线程独占使用（发送期 drain 与收尾收包顺序执行无重入），
 * 避免大数组压垮 32KB 线程栈 */
#define ASR_RESULT_BUF_SIZE 16384
static char s_asr_result_buf[ASR_RESULT_BUF_SIZE];

/* TLS context */
typedef struct {
    mbedtls_ssl_context  ssl;
    mbedtls_ssl_config   cfg;
    mbedtls_net_context  net;
    mbedtls_ctr_drbg_context ctr_drbg;
} asr_tls_t;

/* 预热连接：进入会议页时后台建立 TLS+WS（不发 run-task），
 * 点击开始后 transcribe_stream 直接复用，消除连接建立期间
 * （数秒）音频管道积压溢出导致的开头内容丢失。
 * 注意：mbedtls 上下文内含自引用指针，不可结构体拷贝，
 * 预热握手必须直接在全局实例上完成 */
static asr_tls_t s_warm_tls;
static volatile int s_warm_valid = 0;
static volatile int s_warm_connecting = 0;  /* 预热线程正在握手，防重复发起 */
static pthread_mutex_t s_warm_lock = PTHREAD_MUTEX_INITIALIZER;

static int asr_entropy(void *data, unsigned char *output, size_t len)
{
    (void)data;
    if (agent_secure_random(output, len) == 0)
        return 0;
    return -1;
}

static int asr_tls_connect(asr_tls_t *ctx)
{
    int ret;

    mbedtls_ssl_init(&ctx->ssl);
    mbedtls_ssl_config_init(&ctx->cfg);
    mbedtls_net_init(&ctx->net);
    mbedtls_ctr_drbg_init(&ctx->ctr_drbg);

    ret = mbedtls_ctr_drbg_seed(&ctx->ctr_drbg, asr_entropy,
                                NULL, (const unsigned char *)"asr", 3);
    if (ret != 0) {
        syslog(LOG_ERR, "[%s] ctr_drbg_seed: -0x%04x\n", TAG, -ret);
        return -EIO;
    }

    ret = mbedtls_net_connect(&ctx->net, ASR_HOST, ASR_PORT,
                              MBEDTLS_NET_PROTO_TCP);
    if (ret != 0) {
        syslog(LOG_ERR, "[%s] net_connect: -0x%04x\n", TAG, -ret);
        return -ECONNREFUSED;
    }

    mbedtls_net_set_block(&ctx->net);
    struct timeval tv = { .tv_sec = 30, .tv_usec = 0 };
    setsockopt(ctx->net.fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    ret = mbedtls_ssl_config_defaults(&ctx->cfg,
                                      MBEDTLS_SSL_IS_CLIENT,
                                      MBEDTLS_SSL_TRANSPORT_STREAM,
                                      MBEDTLS_SSL_PRESET_DEFAULT);
    if (ret != 0) {
        syslog(LOG_ERR, "[%s] ssl_config_defaults: -0x%04x\n", TAG, -ret);
        return -EIO;
    }

    mbedtls_ssl_conf_min_tls_version(&ctx->cfg, MBEDTLS_SSL_VERSION_TLS1_2);
    mbedtls_ssl_conf_authmode(&ctx->cfg, MBEDTLS_SSL_VERIFY_OPTIONAL);
    mbedtls_ssl_conf_rng(&ctx->cfg, mbedtls_ctr_drbg_random, &ctx->ctr_drbg);

    ret = mbedtls_ssl_setup(&ctx->ssl, &ctx->cfg);
    if (ret != 0) {
        syslog(LOG_ERR, "[%s] ssl_setup: -0x%04x\n", TAG, -ret);
        return -EIO;
    }

    mbedtls_ssl_set_hostname(&ctx->ssl, ASR_HOST);
    mbedtls_ssl_set_bio(&ctx->ssl, &ctx->net,
                         mbedtls_net_send, mbedtls_net_recv, NULL);

    while ((ret = mbedtls_ssl_handshake(&ctx->ssl)) != 0) {
        if (ret != MBEDTLS_ERR_SSL_WANT_READ &&
            ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
            syslog(LOG_ERR, "[%s] handshake: -0x%04x\n", TAG, -ret);
            return -EIO;
        }
    }

    syslog(LOG_INFO, "[%s] TLS connected to %s:%s\n", TAG, ASR_HOST, ASR_PORT);
    return 0;
}

static void asr_tls_free(asr_tls_t *ctx)
{
    mbedtls_ssl_close_notify(&ctx->ssl);
    mbedtls_net_free(&ctx->net);
    mbedtls_ssl_free(&ctx->ssl);
    mbedtls_ssl_config_free(&ctx->cfg);
    mbedtls_ctr_drbg_free(&ctx->ctr_drbg);
}

static int asr_tls_write_all(asr_tls_t *ctx,
                             const unsigned char *buf, size_t len)
{
    size_t written = 0;
    while (written < len) {
        int ret = mbedtls_ssl_write(&ctx->ssl, buf + written, len - written);
        if (ret > 0) {
            written += (size_t)ret;
        } else if (ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
            return -EIO;
        }
    }
    return 0;
}

static int asr_ws_upgrade(asr_tls_t *ctx, const char *api_key)
{
    unsigned char key_raw[ASR_WS_KEY_LEN];
    unsigned char key_b64[32];
    size_t key_b64_len = 0;

    if (agent_secure_random(key_raw, sizeof(key_raw)) != 0) {
        return -EIO;
    }

    int ret = mbedtls_base64_encode(key_b64, sizeof(key_b64), &key_b64_len,
                                     key_raw, sizeof(key_raw));
    if (ret != 0) {
        return -EIO;
    }

    char req[512];
    int n = snprintf(req, sizeof(req),
        "GET %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: %.*s\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "Authorization: Bearer %s\r\n"
        "\r\n",
        ASR_WS_PATH, ASR_HOST, (int)key_b64_len, key_b64, api_key);

    if (n <= 0 || n >= (int)sizeof(req)) {
        return -EOVERFLOW;
    }

    ret = asr_tls_write_all(ctx, (const unsigned char *)req, (size_t)n);
    if (ret != 0) {
        return ret;
    }

    /* Read response */
    char resp[ASR_WS_BUF_SIZE];
    size_t rlen = 0;
    while (rlen < sizeof(resp) - 1) {
        int r = mbedtls_ssl_read(&ctx->ssl,
                                  (unsigned char *)resp + rlen,
                                  sizeof(resp) - 1 - rlen);
        if (r > 0) {
            rlen += (size_t)r;
            resp[rlen] = '\0';
            if (strstr(resp, "\r\n\r\n")) {
                break;
            }
        } else if (r == 0 || r == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
            break;
        } else if (r != MBEDTLS_ERR_SSL_WANT_READ) {
            return -EIO;
        } else {
            usleep(5000);
        }
    }

    int status = 0;
    if (sscanf(resp, "HTTP/1.1 %d", &status) != 1 || status != 101) {
        syslog(LOG_ERR, "[%s] WS upgrade failed: HTTP %d\n", TAG, status);
        syslog(LOG_ERR, "[%s] WS upgrade resp: %.200s\n", TAG, resp);
        return -EPROTO;
    }

    syslog(LOG_INFO, "[%s] WebSocket upgrade OK\n", TAG);
    return 0;
}


/* ── WebSocket frame helpers (Step B) ───────────────────────── */

#define WS_OPCODE_TEXT  0x01
#define WS_OPCODE_BINARY 0x02
#define WS_FIN_BIT      0x80
#define WS_MASK_BIT     0x80
#define WS_MASK_KEY_LEN 4

static int asr_ws_send_text(asr_tls_t *ctx,
                            const char *payload, size_t plen)
{
    unsigned char hdr[14];
    size_t hdr_len = 0;

    hdr[0] = WS_FIN_BIT | WS_OPCODE_TEXT;

    if (plen < 126) {
        hdr[1] = WS_MASK_BIT | (unsigned char)plen;
        hdr_len = 2;
    } else if (plen <= 0xFFFF) {
        hdr[1] = WS_MASK_BIT | 126;
        hdr[2] = (unsigned char)(plen >> 8);
        hdr[3] = (unsigned char)(plen & 0xFF);
        hdr_len = 4;
    } else {
        hdr[1] = WS_MASK_BIT | 127;
        memset(hdr + 2, 0, 4);
        hdr[6] = (unsigned char)((plen >> 24) & 0xFF);
        hdr[7] = (unsigned char)((plen >> 16) & 0xFF);
        hdr[8] = (unsigned char)((plen >> 8) & 0xFF);
        hdr[9] = (unsigned char)(plen & 0xFF);
        hdr_len = 10;
    }

    unsigned char mask[WS_MASK_KEY_LEN];
    if (agent_secure_random(mask, WS_MASK_KEY_LEN) != 0) {
        return -EIO;
    }
    memcpy(hdr + hdr_len, mask, WS_MASK_KEY_LEN);
    hdr_len += WS_MASK_KEY_LEN;

    int ret = asr_tls_write_all(ctx, hdr, hdr_len);
    if (ret != 0) return ret;

    unsigned char chunk[1024];
    size_t sent = 0;
    while (sent < plen) {
        size_t clen = plen - sent;
        if (clen > sizeof(chunk)) clen = sizeof(chunk);
        for (size_t i = 0; i < clen; i++)
            chunk[i] = ((const unsigned char *)payload)[sent + i]
                       ^ mask[(sent + i) % 4];
        ret = asr_tls_write_all(ctx, chunk, clen);
        if (ret != 0) return ret;
        sent += clen;
    }
    return 0;
}

/* 发送WebSocket二进制帧（用于PCM音频数据）*/
static int asr_ws_send_binary(asr_tls_t *ctx,
                             const unsigned char *data, size_t dlen)
{
    unsigned char hdr[14];
    size_t hdr_len = 0;

    hdr[0] = WS_FIN_BIT | WS_OPCODE_BINARY;

    if (dlen < 126) {
        hdr[1] = WS_MASK_BIT | (unsigned char)dlen;
        hdr_len = 2;
    } else if (dlen <= 0xFFFF) {
        hdr[1] = WS_MASK_BIT | 126;
        hdr[2] = (unsigned char)(dlen >> 8);
        hdr[3] = (unsigned char)(dlen & 0xFF);
        hdr_len = 4;
    } else {
        hdr[1] = WS_MASK_BIT | 127;
        memset(hdr + 2, 0, 4);
        hdr[6] = (unsigned char)((dlen >> 24) & 0xFF);
        hdr[7] = (unsigned char)((dlen >> 16) & 0xFF);
        hdr[8] = (unsigned char)((dlen >> 8) & 0xFF);
        hdr[9] = (unsigned char)(dlen & 0xFF);
        hdr_len = 10;
    }

    unsigned char mask[WS_MASK_KEY_LEN];
    if (agent_secure_random(mask, WS_MASK_KEY_LEN) != 0) {
        return -EIO;
    }
    memcpy(hdr + hdr_len, mask, WS_MASK_KEY_LEN);
    hdr_len += WS_MASK_KEY_LEN;

    int ret = asr_tls_write_all(ctx, hdr, hdr_len);
    if (ret != 0) return ret;

    /* 发送mask后的二进制数据 */
    unsigned char chunk[1024];
    size_t sent = 0;
    while (sent < dlen) {
        size_t clen = dlen - sent;
        if (clen > sizeof(chunk)) clen = sizeof(chunk);
        for (size_t i = 0; i < clen; i++)
            chunk[i] = data[sent + i] ^ mask[(sent + i) % 4];
        ret = asr_tls_write_all(ctx, chunk, clen);
        if (ret != 0) return ret;
        sent += clen;
    }
    return 0;
}

/* 丢弃一帧载荷但保持流同步，必须限时：流错位或对端停发数据时
 * 无限重试会永久阻塞 ASR 线程（duplex 下客户端不发音频后服务端
 * 也不再发包），线程无法退出最终看门狗复位 */
static int asr_ws_skip_payload(asr_tls_t *ctx, size_t plen)
{
    unsigned char skip[256];
    int idle = 0;
    while (plen > 0) {
        size_t n = plen < sizeof(skip) ? plen : sizeof(skip);
        int r = mbedtls_ssl_read(&ctx->ssl, skip, n);
        if (r > 0) { plen -= (size_t)r; idle = 0; }
        else if (r == MBEDTLS_ERR_SSL_WANT_READ) {
            if (++idle > 2000) return -EIO;  /* 10s：丢帧数据在途，容忍TCP重传 */
            usleep(5000);
        } else return -EIO;
    }
    return 0;
}

/* 精确读取n字节（WS帧头/扩展长度/载荷）。30s上限：收尾阶段阻塞等
 * task-finished 属正常等待(1-5s)；16KB大帧经拥塞WiFi分段trickle时
 * 3s会误杀正常帧提前结束转写。网络静默死亡时保证线程可退出 */
static int asr_ws_read_exact(asr_tls_t *ctx, unsigned char *buf, size_t n)
{
    size_t got = 0;
    int idle = 0;
    while (got < n) {
        int r = mbedtls_ssl_read(&ctx->ssl, buf + got, n - got);
        if (r > 0) { got += (size_t)r; idle = 0; }
        else if (r != MBEDTLS_ERR_SSL_WANT_READ) return -EIO;
        else {
            if (++idle > 6000) return -EIO;
            usleep(5000);
        }
    }
    return 0;
}

static int asr_ws_recv_text(asr_tls_t *ctx, char *buf, size_t cap)
{
    unsigned char hdr[2];
    if (asr_ws_read_exact(ctx, hdr, 2) != 0) return -EIO;

    int opcode = hdr[0] & 0x0F;
    size_t plen = hdr[1] & 0x7F;

    if (plen == 126) {
        unsigned char ext[2];
        if (asr_ws_read_exact(ctx, ext, 2) != 0) return -EIO;
        plen = ((size_t)ext[0] << 8) | ext[1];
    } else if (plen == 127) {
        unsigned char ext[8];
        if (asr_ws_read_exact(ctx, ext, 8) != 0) return -EIO;
        plen = 0;
        for (int i = 0; i < 8; i++) plen = (plen << 8) | ext[i];
    }

    if (opcode != WS_OPCODE_TEXT) {
        /* close(0x08)返回0让上层按连接结束收尾，其余帧跳过后继续 */
        int sr = asr_ws_skip_payload(ctx, plen);
        return (opcode == 0x08) ? 0 : ((sr == 0) ? -2 : -EIO);
    }

    if (plen >= cap) {
        /* 超大帧必须整帧消费保流同步：原实现残留载荷会被下次当帧头
         * 解析造成流错位死锁。但不能全丢：帧膨胀元凶是words数组
         * （每字一条时间戳），event与sentence.text都在words之前
         * （帧首~300字节内），读前段可解析出完整text，只截断words。
         * 整体丢弃会丢失超长句最终帧（含完整文本+句末标点），
         * 导致字幕/飞书缺该句尾部 */
        syslog(LOG_WARNING, "[%s] ws frame %u >= %u, partial keep\n",
               TAG, (unsigned)plen, (unsigned)cap);
        size_t keep = cap - 1;
        if (asr_ws_read_exact(ctx, (unsigned char *)buf, keep) != 0) return -EIO;
        if (asr_ws_skip_payload(ctx, plen - keep) != 0) return -EIO;
        buf[keep] = '\0';
        return (int)keep;
    }

    if (asr_ws_read_exact(ctx, (unsigned char *)buf, plen) != 0) return -EIO;
    buf[plen] = '\0';
    return (int)plen;
}


/* ── PCM binary frames via WebSocket ──────────────────────── */

#define ASR_PCM_CHUNK 3200

/* 前向声明：在duplex发送音频时需要调用 */
static int asr_extract_sentence(const char *json, int *out_sid,
                                 char *out, size_t cap);

/* 动态查找WAV文件中data chunk的偏移和大小 */
static off_t asr_wav_find_data(const char *wav_path, uint32_t *out_size)
{
    int fd = open(wav_path, O_RDONLY);
    if (fd < 0) return -1;

    unsigned char riff[12];
    if (read(fd, riff, 12) != 12 ||
        memcmp(riff, "RIFF", 4) != 0 ||
        memcmp(riff + 8, "WAVE", 4) != 0) {
        close(fd);
        return -1;
    }

    off_t pos = 12;
    while (pos < 65536) {
        if (lseek(fd, pos, SEEK_SET) != pos) break;
        unsigned char ch[8];
        if (read(fd, ch, 8) != 8) break;
        uint32_t csz = (uint32_t)ch[4] | ((uint32_t)ch[5] << 8)
                     | ((uint32_t)ch[6] << 16) | ((uint32_t)ch[7] << 24);

        if (memcmp(ch, "data", 4) == 0) {
            if (out_size) *out_size = csz;
            off_t data_start = pos + 8;
            close(fd);
            return data_start;
        }
        pos += 8 + (off_t)csz + (csz & 1);
    }

    close(fd);
    return -1;
}

/* 按WebSocket帧边界排空已到达的所有响应帧。
 * 突发补发后云端连发多帧结果，TCP会把多个WS帧合并进同一数据段：
 * 裸 mbedtls_ssl_read 一次拿到多帧却只提取第一帧的句子，其余整帧
 * 被静默丢弃（表现为补发后只有第一句字幕，如静夜思只剩"床前明月光"）。
 * asr_ws_recv_text 每次精确读完整一帧，循环排空直到mbedtls内部
 * 缓冲与socket均无已到达数据。 */
static void asr_drain_results(asr_tls_t *ctx, meeting_asr_result_cb cb,
                              int *has_text, void *ud)
{
    int frames = 0;
    while (1) {
        int readable = (mbedtls_ssl_get_bytes_avail(&ctx->ssl) > 0);
        if (!readable) {
            fd_set rfds;
            struct timeval tv = {0, 0};  /* 0超时：非阻塞探测 */
            FD_ZERO(&rfds);
            FD_SET(ctx->net.fd, &rfds);
            readable = (select(ctx->net.fd + 1, &rfds, NULL, NULL, &tv) > 0);
        }
        if (!readable) break;

        char *tmp = s_asr_result_buf;
        int rn = asr_ws_recv_text(ctx, tmp, ASR_RESULT_BUF_SIZE);
        if (rn == 0) break;       /* close帧：连接结束 */
        if (rn == -2) continue;   /* 非文本帧：已跳过，继续排后续帧 */
        if (rn < 0) break;

        frames++;
        if (s_asr_test_mode) {
            syslog(LOG_INFO, "[%s] RAW[%d]: %.400s\n", TAG, frames, tmp);
        }
        const char *ev_result = "\"event\":\"result-generated\"";
        if (strstr(tmp, ev_result)) {
            char text[512];  /* 与 current_asr_text[512] 及收尾路径对齐，
                             * 日语UTF-8每字3字节，256会截断长句 */
            int sid = -1;
            if (asr_extract_sentence(tmp, &sid, text, sizeof(text)) > 0) {
                cb(text, sid, ud);
                if (has_text) *has_text = 1;
            }
        }
    }
    if (frames > 1) {
        syslog(LOG_INFO, "[%s] drained %d ws frames\n", TAG, frames);
    }
}

static int asr_send_audio_file(asr_tls_t *ctx, const char *wav_path,
                               const char *task_id,
                               meeting_asr_result_cb cb,
                               meeting_asr_progress_cb progress_cb,
                               int *has_text,
                               void *ud)
{
    int fd = open(wav_path, O_RDONLY);
    if (fd < 0) return -EIO;

    /* 动态查找data chunk位置 */
    uint32_t data_size = 0;
    off_t data_pos = asr_wav_find_data(wav_path, &data_size);
    if (data_pos < 0) {
        close(fd);
        return -EIO;
    }

    if (lseek(fd, data_pos, SEEK_SET) != data_pos) {
        close(fd);
        return -EIO;
    }

    /* 直接通过WebSocket Binary Frame发送原始PCM二进制数据
     * paraformer-realtime-v2 是流式ASR模型，其滑动窗口按实时节奏设计。
     * 必须以1倍速（实时）发送，ASR才能完整处理整个会议的上下文。
     * 3倍速时窗口只覆盖后30%内容，1倍速才能100%覆盖。
     * ASR_PCM_CHUNK=3200B / (16kHz*16bit*mono) = 100ms 音频/chunk
     * sleep 100ms = 1倍速实时发送 */
    unsigned char pcm[ASR_PCM_CHUNK];
    int seq = 0;
    while (1) {
        ssize_t n = read(fd, pcm, sizeof(pcm));
        if (n <= 0) break;

        /* 直接发送二进制PCM数据，不需要base64编码和JSON包装 */
        if (asr_ws_send_binary(ctx, pcm, (size_t)n) != 0) {
            close(fd);
            return -EIO;
        }

        seq++;

        /* 阶段一进度：基于已发送字节占比，上限90%（留10%给收尾阶段） */
        if (progress_cb && data_size > 0) {
            /* data_size是uint32_t，转为size_t避免符号问题 */
            size_t total_bytes = (size_t)data_size;
            /* seq * ASR_PCM_CHUNK近似已发送字节（最后一个chunk可能不足） */
            size_t sent_bytes = (size_t)seq * ASR_PCM_CHUNK;
            if (sent_bytes > total_bytes) sent_bytes = total_bytes;
            int raw_percent = (int)(sent_bytes * 100 / total_bytes);
            if (raw_percent > 90) raw_percent = 90;
            progress_cb(raw_percent, ud);
        }

        /* 实时节奏控制：100ms/chunk = 1倍速，保证ASR流式上下文100%完整 */
        usleep(100000);

        /* Duplex模式：发送音频同时读取响应（按帧排空，防TCP合并丢帧） */
        asr_drain_results(ctx, cb, has_text, ud);
    }

    close(fd);

    /* finish-task */
    char fin[512];
    int f = snprintf(fin, sizeof(fin),
        "{\"header\":{\"action\":\"finish-task\","
        "\"task_id\":\"%s\",\"streaming\":\"duplex\"},"
        "\"payload\":{\"input\":{}}}",
        task_id);
    if (f > 0 && f < (int)sizeof(fin)) {
        asr_ws_send_text(ctx, fin, (size_t)f);
    }

    return 0;
}

/* 流式发送音频：从 media_recorder 实例读 PCM，实时送云端 ASR。
 * 用于方案B（边录边转）：录音同时进行转写，自然 1:1 节奏。
 * 退出条件：*stop_flag 被置位（录音停止时调用方设置）。 */
/* 读 API key：文件优先，失败用编译期默认 */
static const char *asr_load_key(char *buf, size_t cap)
{
    const char *use_key = AGENT_SECRET_API_KEY;
    FILE *fk = fopen("/emmc/wifi/agent_app_id", "r");
    if (fk) {
        size_t fn = fread(buf, 1, cap - 1, fk);
        fclose(fk);
        if (fn > 0) {
            /* 必须写终止符：调用方keybuf未初始化，否则strlen扫过
             * 文件内容后继续读脏栈数据，key被污染→服务器HTTP 400 */
            buf[fn] = '\0';
            char *s = buf;
            while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
            char *e = s + strlen(s);
            while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) e--;
            if (e > s) { *e = '\0'; use_key = s; }
        }
    }
    return use_key;
}

/* 现场冷连接（TLS+WS 握手），失败返回 NULL。
 * 预热的取走/等待逻辑在 asr_stream_connect_wait 中 */
static asr_tls_t *asr_cold_connect(asr_tls_t *tls_out)
{
    memset(tls_out, 0, sizeof(*tls_out));
    if (asr_tls_connect(tls_out) != 0) {
        return NULL;
    }
    char keybuf[128];
    if (asr_ws_upgrade(tls_out, asr_load_key(keybuf, sizeof(keybuf))) != 0) {
        asr_tls_free(tls_out);
        return NULL;
    }
    return tls_out;
}

/* ── 积压音频缓冲（B）：等待预热连接期间预读 rec2，防 shm fifo 溢出 ──
 * 场景：秒点开始时预热线程仍在握手。等待期间主动排空 fifo 存入链表，
 * 连接就绪后全速补发（云端按收包顺序转写，支持快于实时节奏），
 * 字幕迟到但不丢失。上限约10s（100块），超限丢最旧保实时连续性。 */
#define ASR_BACKLOG_MAX_NODES 100

typedef struct asr_blk {
    struct asr_blk *next;
    size_t len;
    unsigned char data[ASR_PCM_CHUNK];
} asr_blk_t;

static void asr_backlog_push(asr_blk_t **head, asr_blk_t **tail,
                              const unsigned char *pcm, size_t n)
{
    if (n == 0 || n > ASR_PCM_CHUNK) {
        return;
    }
    asr_blk_t *node = malloc(sizeof(asr_blk_t));
    if (!node) {
        return;  /* 内存不足：丢弃本块（不致命，fifo暂存） */
    }
    node->next = NULL;
    node->len = n;
    memcpy(node->data, pcm, n);
    if (*tail) (*tail)->next = node; else *head = node;
    *tail = node;
    int count = 0;
    for (asr_blk_t *p = *head; p; p = p->next) count++;
    if (count > ASR_BACKLOG_MAX_NODES) {
        asr_blk_t *old = *head;
        *head = old->next;
        free(old);
    }
}

static void asr_backlog_free(asr_blk_t **head)
{
    while (*head) {
        asr_blk_t *p = *head;
        *head = p->next;
        free(p);
    }
}

/* B：追赶式补发。不能全速狂发：shm fifo 容量仅约 316ms（10112B），
 * 全速发送 8s 积压耗时 1-2s，期间无人消费 fifo 会溢出丢失等待
 * 末段的音频。改为每发 2 块读 1 块：
 * - 连续不读 fifo 的最长时长仅发送 2 块的 ~30ms << 316ms，不溢出
 * - 每轮净消耗 1 块（发2进1），有限轮内发完，无死循环
 * - fifo 空时读阻塞 ~100ms 充当限速器，约 2.5 倍速追赶，
 *   8s 积压约 3s 追平后转入主循环 */
static int asr_backlog_send(asr_tls_t *ctx, asr_blk_t **head, asr_blk_t **tail,
                            void *recorder_handle)
{
    int since_read = 0;
    while (*head) {
        asr_blk_t *p = *head;
        if (asr_ws_send_binary(ctx, p->data, p->len) != 0) {
            asr_backlog_free(head);
            if (*head == NULL) *tail = NULL;
            return -EIO;
        }
        *head = p->next;
        free(p);
        if (*head == NULL) *tail = NULL;  /* 维护 tail==NULL ⟺ head==NULL 不变量 */
#ifdef CONFIG_MEDIA
        if (++since_read >= 2 && recorder_handle) {
            since_read = 0;
            unsigned char pcm[ASR_PCM_CHUNK];
            ssize_t n = media_recorder_read_data(recorder_handle,
                                                 pcm, sizeof(pcm));
            if (n > 0) {
                asr_backlog_push(head, tail, pcm, (size_t)n);
            }
        }
#endif
    }
    return 0;
}

/* A：获取连接。预热就绪直接复用；预热线程握手中则边等待边预读rec2；
 * 预热失败/未启动则冷连接（含一次重试）。 */
static asr_tls_t *asr_stream_connect_wait(asr_tls_t *tls_out, void *recorder_handle,
                                          volatile int *stop_flag,
                                          volatile bool *pause_flag,
                                          asr_blk_t **blk_head, asr_blk_t **blk_tail)
{
    pthread_mutex_lock(&s_warm_lock);
    int warm = s_warm_valid;
    if (warm) s_warm_valid = 0;
    int connecting = s_warm_connecting;
    pthread_mutex_unlock(&s_warm_lock);
    if (warm) {
        syslog(LOG_INFO, "[%s] reuse warm conn\n", TAG);
        return &s_warm_tls;
    }
    if (connecting) {
        syslog(LOG_INFO, "[%s] wait warm conn, pre-reading audio\n", TAG);
        int waited_ms = 0;
        while (waited_ms < 12000 && *stop_flag == 0) {
            /* 关键：每轮只读一块。read_data 是阻塞式读：数据不足时
             * 阻塞到凑满 3200 字节（≈100ms 实时音频），本身就是限速器。
             * 绝不能 for(;;) 连续排空：录音持续时读永远有数据，循环
             * 永不退出，预热状态整场得不到检查（已复现：整场会议卡
             * 在排空，停止后仅送出链表里留存的最后~10s） */
            unsigned char pcm[ASR_PCM_CHUNK];
            ssize_t n = 0;
            if (!(pause_flag && *pause_flag)) {
#ifdef CONFIG_MEDIA
                n = media_recorder_read_data(recorder_handle,
                                              pcm, sizeof(pcm));
#endif
            }
            if (n > 0) {
                asr_backlog_push(blk_head, blk_tail, pcm, (size_t)n);
            } else {
                usleep(100 * 1000);  /* 暂停中或暂无数据 */
            }
            pthread_mutex_lock(&s_warm_lock);
            warm = s_warm_valid;
            if (warm) s_warm_valid = 0;
            int conn = s_warm_connecting;
            pthread_mutex_unlock(&s_warm_lock);
            if (warm) {
                syslog(LOG_INFO, "[%s] reuse warm conn after %dms\n",
                       TAG, waited_ms);
                return &s_warm_tls;
            }
            if (!conn) break;  /* 预热线程已结束且无连接（失败） */
            waited_ms += 100;
        }
        if (*stop_flag) {
            return NULL;  /* 等待期间用户已停止：不再发起冷连接 */
        }
    }
    /* 冷连接（弱网/预热失败），失败1s后重试一次 */
    asr_tls_t *tp = asr_cold_connect(tls_out);
    if (!tp) {
        sleep(1);
        tp = asr_cold_connect(tls_out);
    }
    return tp;
}

int meeting_asr_prepare_conn(void)
{
    pthread_mutex_lock(&s_warm_lock);
    int have = s_warm_valid;
    int connecting = s_warm_connecting;
    if (!have && !connecting) {
        s_warm_connecting = 1;
    }
    pthread_mutex_unlock(&s_warm_lock);
    if (have || connecting) {
        return 0;  /* 已有预热连接，或另一线程正在握手中 */
    }
    syslog(LOG_INFO, "[%s] warm conn START\n", TAG);
    memset(&s_warm_tls, 0, sizeof(s_warm_tls));
    if (asr_tls_connect(&s_warm_tls) != 0) {
        pthread_mutex_lock(&s_warm_lock);
        s_warm_connecting = 0;
        pthread_mutex_unlock(&s_warm_lock);
        return -ECONNREFUSED;
    }
    char keybuf[128];
    if (asr_ws_upgrade(&s_warm_tls, asr_load_key(keybuf, sizeof(keybuf))) != 0) {
        asr_tls_free(&s_warm_tls);
        pthread_mutex_lock(&s_warm_lock);
        s_warm_connecting = 0;
        pthread_mutex_unlock(&s_warm_lock);
        return -EPIPE;
    }
    pthread_mutex_lock(&s_warm_lock);
    s_warm_connecting = 0;
    s_warm_valid = 1;
    pthread_mutex_unlock(&s_warm_lock);
    syslog(LOG_INFO, "[%s] warm conn READY\n", TAG);
    return 0;
}

void meeting_asr_discard_warm_conn(void)
{
    pthread_mutex_lock(&s_warm_lock);
    int warm = s_warm_valid;
    s_warm_valid = 0;
    pthread_mutex_unlock(&s_warm_lock);
    if (warm) {
        asr_tls_free(&s_warm_tls);
        syslog(LOG_INFO, "[%s] warm conn discarded\n", TAG);
    }
}

int meeting_asr_conn_pending(void)
{
    pthread_mutex_lock(&s_warm_lock);
    int pending = !s_warm_valid;
    pthread_mutex_unlock(&s_warm_lock);
    return pending;
}

static int asr_send_audio_stream(asr_tls_t *ctx, void *recorder_handle,
                                 const char *task_id,
                                 meeting_asr_result_cb cb,
                                 meeting_asr_progress_cb progress_cb,
                                 int *has_text,
                                 volatile int *stop_flag,
                                 volatile bool *pause_flag,
                                 void *ud)
{
    if (!recorder_handle || !stop_flag) {
        return -EINVAL;
    }
    (void)progress_cb;  /* 边录边转无进度概念，仅保留签名兼容 */

    unsigned char pcm[ASR_PCM_CHUNK];
    int silent_count = 0;  /* 连续静默读次数：recorder已停时read可能返回0 */

    while (*stop_flag == 0) {
        /* 暂停中：recorder 已 stop，不读不发（否则静默超时会误判为
         * 音频结束而提前 finish-task，导致会议未结束就触发后处理） */
        if (pause_flag && *pause_flag) {
            usleep(200 * 1000);
            continue;
        }
        ssize_t n = -1;
#ifdef CONFIG_MEDIA
        n = media_recorder_read_data(recorder_handle, pcm, sizeof(pcm));
#endif
        /* recorder 已 stop/close 或返回 0：连续静默则退出。
         * 暂停恢复瞬间 recorder 重启中也可能短暂无数据，
         * 统一累积 silent_count 等待，仅真正持续无数据才退出 */
        if (n <= 0) {
            silent_count++;
            if (silent_count > 50) {  /* ~5s 无数据视为异常结束 */
                break;
            }
            usleep(100000);
            continue;
        }
        silent_count = 0;

        if (asr_ws_send_binary(ctx, pcm, (size_t)n) != 0) {
            return -EIO;
        }

        /* Duplex模式：发送音频同时读响应（按帧排空，防TCP合并丢帧） */
        asr_drain_results(ctx, cb, has_text, ud);
    }

    /* finish-task：通知服务端音频流结束，等待最终结果 */
    char fin[512];
    int f = snprintf(fin, sizeof(fin),
        "{\"header\":{\"action\":\"finish-task\","
        "\"task_id\":\"%s\",\"streaming\":\"duplex\"},"
        "\"payload\":{\"input\":{}}}",
        task_id);
    if (f > 0 && f < (int)sizeof(fin)) {
        asr_ws_send_text(ctx, fin, (size_t)f);
    }

    return 0;
}

/* ── Simple text extraction from result-generated ──────────── */

/* 提取 result-generated 事件中的 sentence_id 和 text。
 * paraformer-realtime-v2 返回 JSON 中含 sentence_id 字段，
 * 同一 sentence_id 的多次结果是同一句子的累积/修正，
 * UI 据此判断更新同一行还是新建行，避免前缀检测在修正时失败。 */
static int asr_extract_sentence(const char *json, int *out_sid,
                                 char *out, size_t cap)
{
    /* 提取 sentence_id */
    int sid = -1;
    const char *sp = strstr(json, "\"sentence_id\":");
    if (sp) {
        /* 跳过完整字面量 "sentence_id": 共 14 字符（含冒号）。
         * 曾为 sp+=13 只跳到冒号，atoi(":3") 恒返 0：云端每句 sid 都
         * 被解析成 0，UI 把所有句子误判为同一句，每句覆盖上一句，
         * 字幕表现为“每说一两句就清零重来”，且除末句外全部
         * 句子都不进 s_full_transcription/飞书纪要 */
        sp += strlen("\"sentence_id\":");
        while (*sp == ' ') sp++;
        sid = atoi(sp);
    }
    if (out_sid) *out_sid = sid;

    /* 提取 text */
    const char *p = strstr(json, "\"text\":\"");
    if (!p) return -1;
    p += 8;
    size_t i = 0;
    while (*p && *p != '"' && i < cap - 1) {
        if (*p == '\\' && *(p + 1)) {
            p++;
            if (*p == 'n') out[i++] = '\n';
            else if (*p == 't') out[i++] = '\t';
            else out[i++] = *p;
            p++;
        } else {
            out[i++] = *p++;
        }
    }
    out[i] = '\0';
    return (int)i;
}

int meeting_asr_transcribe_file(const char *wav_path,
                                meeting_asr_result_cb callback,
                                meeting_asr_progress_cb progress_cb,
                                void *user_data)
{
    if (!wav_path || !callback) {
        return -EINVAL;
    }

    syslog(LOG_INFO, "[%s] transcribe START: %s\n", TAG, wav_path);

    /* 1. Verify WAV file (with retry: media_recorder_close may be async) */
    int fd = -1;
    unsigned char header[44];
    ssize_t n = 0;
    int retry;

    for (retry = 0; retry < 10; retry++) {
        fd = open(wav_path, O_RDONLY);
        if (fd >= 0) {
            n = read(fd, header, sizeof(header));
            close(fd);
            fd = -1;
            if (n >= (ssize_t)sizeof(header) &&
                memcmp(header, "RIFF", 4) == 0 &&
                memcmp(header + 8, "WAVE", 4) == 0) {
                break;
            }
        }
        usleep(200 * 1000);
    }

    if (retry >= 10) {
        callback("[ASR] 录音文件无效", -1, user_data);
        return -EIO;
    }

    syslog(LOG_INFO, "[%s] WAV file OK after %d retries\n", TAG, retry);
#if 0  /* 诊断代码已禁用：转写功能验证成功，不再需要调试信息 */
    callback("[ASR] WAV文件验证通过，正在诊断...", user_data);

    /* === 诊断：只读不写，定位 NO_VALID_AUDIO_ERROR 根因 === */
    do {
        int dfd = open(wav_path, O_RDONLY);
        if (dfd < 0) {
            callback("[ASR] 诊断: 打开文件失败", user_data);
            break;
        }

        /* 跳过 12 字节 RIFF/WAVE 头 */
        unsigned char riff[12];
        if (read(dfd, riff, 12) != 12) {
            callback("[ASR] 诊断: 读RIFF头失败", user_data);
            close(dfd);
            break;
        }

        /* 遍历 chunk 找 fmt 和 data */
        off_t pos = 12;
        int fmt_found = 0;
        int data_found = 0;
        unsigned char fmt_body[16];
        off_t data_pos = 0;
        uint32_t data_size = 0;
        int chunk_count = 0;

        while (pos < 4096 && chunk_count < 16) {
            if (lseek(dfd, pos, SEEK_SET) != pos) break;
            unsigned char ch[8];
            if (read(dfd, ch, 8) != 8) break;
            uint32_t csz = (uint32_t)ch[4] | ((uint32_t)ch[5] << 8)
                         | ((uint32_t)ch[6] << 16) | ((uint32_t)ch[7] << 24);
            chunk_count++;

            if (memcmp(ch, "fmt ", 4) == 0 && csz >= 16 && csz <= 40) {
                if (read(dfd, fmt_body, 16) == 16) {
                    fmt_found = 1;
                }
            } else if (memcmp(ch, "data", 4) == 0) {
                data_pos = pos + 8;
                data_size = csz;
                data_found = 1;
                break;  /* data 通常在最后，找到即可停 */
            }
            pos += 8 + (off_t)csz + (csz & 1);
        }

        /* 显示 chunk 结构 */
        {
            char buf[128];
            snprintf(buf, sizeof(buf), "[ASR] 共%d个chunk, fmt=%d data=%d",
                     chunk_count, fmt_found, data_found);
            callback(buf, user_data);
        }

        /* 显示 data chunk 的偏移和大小（判断 WAV 头是否真的是 44 字节）*/
        {
            char buf[128];
            snprintf(buf, sizeof(buf),
                     "[ASR] data偏移=%ld 大小=%lu",
                     (long)data_pos, (unsigned long)data_size);
            callback(buf, user_data);
        }

        /* 显示 fmt chunk 解析结果（采样率/声道/位深/编码格式）
         * WAV fmt结构偏移：
         * [0-1] audio_format, [2-3] num_channels, [4-7] sample_rate,
         * [8-11] byte_rate, [12-13] block_align, [14-15] bits_per_sample */
        if (fmt_found) {
            uint16_t audio_fmt = (uint16_t)fmt_body[0]
                               | ((uint16_t)fmt_body[1] << 8);
            uint16_t chs = (uint16_t)fmt_body[2]
                         | ((uint16_t)fmt_body[3] << 8);
            uint32_t sr = (uint32_t)fmt_body[4]
                        | ((uint32_t)fmt_body[5] << 8)
                        | ((uint32_t)fmt_body[6] << 16)
                        | ((uint32_t)fmt_body[7] << 24);
            uint16_t bits = (uint16_t)fmt_body[14]
                          | ((uint16_t)fmt_body[15] << 8);
            char buf[128];
            snprintf(buf, sizeof(buf),
                     "[ASR] fmt=0x%04x sr=%lu ch=%u bits=%u",
                     audio_fmt, (unsigned long)sr, chs, bits);
            callback(buf, user_data);
        } else {
            callback("[ASR] 未找到fmt chunk!", user_data);
        }

        /* 统计 PCM 数据的 max/min/RMS 判断是否静音 */
        if (data_found && data_size > 0) {
            if (lseek(dfd, data_pos, SEEK_SET) != data_pos) {
                callback("[ASR] 定位data失败", user_data);
                close(dfd);
                break;
            }
            int16_t smin = 0, smax = 0;
            uint64_t sum_sq = 0;
            uint32_t samples = 0;
            unsigned char pb[2048];
            uint32_t left = data_size;
            uint32_t cap = left > 65536 ? 65536 : left;  /* 最多统计前64KB */
            while (cap >= sizeof(pb)) {
                ssize_t r = read(dfd, pb, sizeof(pb));
                if (r <= 0) break;
                int16_t *p16 = (int16_t *)pb;
                uint32_t nsmp = r / 2;
                for (uint32_t i = 0; i < nsmp; i++) {
                    int16_t v = p16[i];
                    if (v < smin) smin = v;
                    if (v > smax) smax = v;
                    sum_sq += (uint32_t)((int32_t)v * (int32_t)v);
                    samples++;
                }
                cap -= r;
            }
            uint32_t rms = 0;
            if (samples > 0) rms = (uint32_t)(sum_sq / samples);
            char buf[128];
            snprintf(buf, sizeof(buf),
                     "[ASR] 采样数=%lu min=%d max=%d RMS=%lu",
                     (unsigned long)samples, smin, smax,
                     (unsigned long)rms);
            callback(buf, user_data);
            if (smax - smin < 100) {
                callback("[ASR] !! 音频近乎静音 !!", user_data);
            }
        } else {
            callback("[ASR] 未找到data chunk!", user_data);
        }
        close(dfd);
    } while (0);
    callback("[ASR] ---诊断完成(请拍照)---", user_data);
    usleep(2000 * 1000);  /* 停顿2秒，让用户看清诊断信息 */
#endif
    /* 2. TLS connect */
    asr_tls_t tls;
    memset(&tls, 0, sizeof(tls));
    int ret = asr_tls_connect(&tls);
    if (ret != 0) {
        callback("[ASR] 连接服务器失败", -1, user_data);
        asr_tls_free(&tls);
        return ret;
    }

    /* 3. WebSocket upgrade - 优先从 /emmc/wifi/agent_app_id 读取 api key */
    char file_key[128];
    ret = asr_ws_upgrade(&tls, asr_load_key(file_key, sizeof(file_key)));
    if (ret != 0) {
        callback("[ASR] WebSocket握手失败", -1, user_data);
        asr_tls_free(&tls);
        return ret;
    }

    /* 4. Send run-task (Step B-1) */
    char task_id[33];
    static const char hex_chars[] = "0123456789abcdef";
    for (int i = 0; i < 16; i++) {
        unsigned char b;
        if (agent_secure_random(&b, 1) != 0) { b = (unsigned char)i; }
        task_id[i * 2]     = hex_chars[(b >> 4) & 0xF];
        task_id[i * 2 + 1] = hex_chars[b & 0xF];
    }
    task_id[32] = '\0';

    char hints[96];
    asr_fill_lang_hints(hints, sizeof(hints));
    char run_task[640];
    int m = snprintf(run_task, sizeof(run_task),
        "{\"header\":{\"action\":\"run-task\",\"task_id\":\"%s\","
        "\"streaming\":\"duplex\"},"
        "\"payload\":{\"model\":\"paraformer-realtime-v2\","
        "\"task_group\":\"audio\",\"task\":\"asr\","
        "\"function\":\"recognition\","
        "\"parameters\":{\"format\":\"pcm\",\"sample_rate\":16000,"
        "\"language_hints\":[%s]},\"input\":{}}}",
        task_id, hints);

    if (m <= 0 || m >= (int)sizeof(run_task)) {
        callback("[ASR] run-task构造失败", -1, user_data);
        asr_tls_free(&tls);
        return -EOVERFLOW;
    }

    ret = asr_ws_send_text(&tls, run_task, (size_t)m);
    if (ret != 0) {
        callback("[ASR] 发送任务请求失败", -1, user_data);
        asr_tls_free(&tls);
        return ret;
    }

    /* 5. Receive one response frame (Step B-1 verification) */
    char resp_buf[ASR_WS_BUF_SIZE];
    int rn = asr_ws_recv_text(&tls, resp_buf, sizeof(resp_buf));
    if (rn < 0) {
        char err[64];
        snprintf(err, sizeof(err), "[ASR] 接收响应失败: %d", rn);
        callback(err, -1, user_data);
        asr_tls_free(&tls);
        return -EIO;
    }

    /* 6. Send audio chunks (Step B-2) */
    int has_text = 0;  /* 函数级标志：阶段一和阶段二共用，避免误报未收到 */
    ret = asr_send_audio_file(&tls, wav_path, task_id, callback,
                              progress_cb, &has_text, user_data);
    if (ret != 0) {
        callback("[ASR] 发送音频失败", -1, user_data);
        asr_tls_free(&tls);
        return ret;
    }

    /* 7. Receive results until task-finished or close */
    char *recv_buf = s_asr_result_buf;
    int tail_progress = 90;  /* 阶段二：收尾进度，从90%开始 */
    for (int attempt = 0; attempt < 500; attempt++) {
        int rn2 = asr_ws_recv_text(&tls, recv_buf, ASR_RESULT_BUF_SIZE);
        if (rn2 == 0) {
            break;
        }
        if (rn2 < 0) {
            if (rn2 == -2) continue;
            break;
        }

        /* 精简日志：只显示关键事件，避免刷屏 */
        {
            const char *ev_result = "\"event\":\"result-generated\"";
            const char *ev_done   = "\"event\":\"task-finished\"";
            const char *ev_error  = "\"event\":\"error\"";
            const char *ev_failed = "\"event\":\"task-failed\"";

            if (strstr(recv_buf, ev_result)) {
                char text[512];
                int sid = -1;
                if (asr_extract_sentence(recv_buf, &sid, text, sizeof(text)) > 0) {
                    callback(text, sid, user_data);
                    has_text = 1;
                    /* 阶段二：每段字幕推进2%，但不超过99% */
                    if (progress_cb && tail_progress < 99) {
                        tail_progress += 2;
                        if (tail_progress > 99) tail_progress = 99;
                        progress_cb(tail_progress, user_data);
                    }
                }
            } else if (strstr(recv_buf, ev_done)) {
                /* task-finished：所有结果已接收，进度到100% */
                if (progress_cb) {
                    progress_cb(100, user_data);
                }
                break;
            } else if (strstr(recv_buf, ev_error) ||
                       strstr(recv_buf, ev_failed)) {
                char errmsg[200];
                const char *ec_key = "\"error_code\":\"";
                const char *ec = strstr(recv_buf, ec_key);
                if (ec) {
                    ec += strlen(ec_key);
                    const char *end = strchr(ec, '"');
                    int elen = end ? (int)(end - ec) : 0;
                    if (elen > 0 && elen < 100) {
                        snprintf(errmsg, sizeof(errmsg),
                                 "[ASR] 错误: %.*s", elen, ec);
                    } else {
                        snprintf(errmsg, sizeof(errmsg),
                                 "[ASR] 服务器返回错误");
                    }
                } else {
                    snprintf(errmsg, sizeof(errmsg),
                             "[ASR] 服务器返回错误");
                }
                callback(errmsg, -1, user_data);
                break;
            }
        }
        /* 其他帧（task-started等）静默跳过 */
    }

    if (!has_text) {
        callback("[ASR] 未收到转写内容", -1, user_data);
    }

    asr_tls_free(&tls);
    syslog(LOG_INFO, "[%s] transcribe END (step B-2)\n", TAG);
    return 0;
}

/* ── 流式转写：方案B 边录边转 ─────────────────────────────────
 * 从 media_recorder 实例实时读 PCM 送云端，录音停止时通过 stop_flag 退出。 */
int meeting_asr_transcribe_stream(void *recorder_handle,
                                  meeting_asr_result_cb callback,
                                  meeting_asr_progress_cb progress_cb,
                                  volatile int *stop_flag,
                                  volatile bool *pause_flag,
                                  void *user_data)
{
    if (!recorder_handle || !callback || !stop_flag) {
        return -EINVAL;
    }

    syslog(LOG_INFO, "[%s] stream transcribe START\n", TAG);

    /* 1+2. 获取可用连接：优先复用预热连接（进会议页时已后台握手完成），
     * 否则现场冷连接（TLS+WS 握手需数秒，期间音频管道积压会丢开头内容） */
    asr_tls_t tls;
    asr_blk_t *blk_head = NULL, *blk_tail = NULL;
    /* A+B：预热线程握手中则等待（期间预读rec2防fifo溢出），就绪后复用；
     * 均无则冷连接（含一次重试） */
    asr_tls_t *tp = asr_stream_connect_wait(&tls, recorder_handle, stop_flag,
                                            pause_flag, &blk_head, &blk_tail);
    int warm_used = (tp == &s_warm_tls);
    int ret = 0;
    if (!tp) {
        asr_backlog_free(&blk_head);
        callback("[ASR] 连接服务器失败", -1, user_data);
        return -ECONNREFUSED;
    }

    /* 3. Send run-task */
    char task_id[33];
    static const char hex_chars[] = "0123456789abcdef";
    for (int i = 0; i < 16; i++) {
        unsigned char b;
        if (agent_secure_random(&b, 1) != 0) { b = (unsigned char)i; }
        task_id[i * 2]     = hex_chars[(b >> 4) & 0xF];
        task_id[i * 2 + 1] = hex_chars[b & 0xF];
    }
    task_id[32] = '\0';

    char hints[96];
    asr_fill_lang_hints(hints, sizeof(hints));
    char run_task[640];
    int m = snprintf(run_task, sizeof(run_task),
        "{\"header\":{\"action\":\"run-task\",\"task_id\":\"%s\","
        "\"streaming\":\"duplex\"},"
        "\"payload\":{\"model\":\"paraformer-realtime-v2\","
        "\"task_group\":\"audio\",\"task\":\"asr\","
        "\"function\":\"recognition\","
        "\"parameters\":{\"format\":\"pcm\",\"sample_rate\":16000,"
        "\"language_hints\":[%s]},\"input\":{}}}",
        task_id, hints);

    if (m <= 0 || m >= (int)sizeof(run_task)) {
        callback("[ASR] run-task构造失败", -1, user_data);
        asr_backlog_free(&blk_head);
        asr_tls_free(tp);
        return -EOVERFLOW;
    }

    char resp_buf[ASR_WS_BUF_SIZE];
    int rn = -1;
    if (asr_ws_send_text(tp, run_task, (size_t)m) == 0) {
        /* 4. Receive one response frame */
        rn = asr_ws_recv_text(tp, resp_buf, sizeof(resp_buf));
    }
    /* 预热连接可能被服务端空闲断开（用户长时间未点开始）：
     * 发送/接收失败时丢弃并冷连接重试一次 */
    if (rn < 0 && warm_used) {
        syslog(LOG_WARNING, "[%s] warm conn stale, cold retry\n", TAG);
        asr_tls_free(tp);
        warm_used = 0;
        tp = asr_cold_connect(&tls);
        if (tp && asr_ws_send_text(tp, run_task, (size_t)m) == 0) {
            rn = asr_ws_recv_text(tp, resp_buf, sizeof(resp_buf));
        }
    }
    if (s_asr_test_mode && rn > 0) {
        syslog(LOG_INFO, "[%s] RUNTASK-RESP: %.400s\n", TAG, resp_buf);
    }
    if (rn < 0) {
        callback("[ASR] 接收响应失败", -1, user_data);
        asr_backlog_free(&blk_head);
        if (tp) asr_tls_free(tp);
        return -EIO;
    }

    /* B：追赶式补发等待连接期间预读的积压音频（边发边读防 fifo 溢出） */
    if (blk_head) {
        syslog(LOG_INFO, "[%s] backlog catch-up send\n", TAG);
        if (asr_backlog_send(tp, &blk_head, &blk_tail, recorder_handle) != 0) {
            callback("[ASR] 发送积压音频失败", -1, user_data);
            asr_tls_free(tp);
            return -EIO;
        }
    }

    /* 5. Send audio chunks from media_recorder (stream mode) */
    int has_text = 0;
    ret = asr_send_audio_stream(tp, recorder_handle, task_id, callback,
                                progress_cb, &has_text, stop_flag,
                                pause_flag, user_data);
    if (ret != 0) {
        callback("[ASR] 流式发送音频失败", -1, user_data);
        asr_tls_free(tp);
        return ret;
    }

    /* 6. Receive results until task-finished or close */
    char *recv_buf = s_asr_result_buf;
    for (int attempt = 0; attempt < 500; attempt++) {
        int rn2 = asr_ws_recv_text(tp, recv_buf, ASR_RESULT_BUF_SIZE);
        if (rn2 == 0) {
            break;
        }
        if (rn2 < 0) {
            if (rn2 == -2) continue;
            break;
        }

        const char *ev_result = "\"event\":\"result-generated\"";
        const char *ev_done   = "\"event\":\"task-finished\"";
        const char *ev_error  = "\"event\":\"error\"";
        const char *ev_failed = "\"event\":\"task-failed\"";

        if (strstr(recv_buf, ev_result)) {
            char text[512];
            int sid = -1;
            if (asr_extract_sentence(recv_buf, &sid, text, sizeof(text)) > 0) {
                callback(text, sid, user_data);
                has_text = 1;
            }
        } else if (strstr(recv_buf, ev_done)) {
            /* 边录边转下音频早已发完，点结束后此事件 1-2 秒内即到达。
             * 不发 100%：进度节奏由调用方分阶段管理（转写30/LLM40-70/飞书90/完成100），
             * 此处提前 100% 会抢占后续阶段显示 */
            break;
        } else if (strstr(recv_buf, ev_error) ||
                   strstr(recv_buf, ev_failed)) {
            syslog(LOG_ERR, "[%s] stream task failed: %.200s\n", TAG, recv_buf);
            break;
        }
    }

    asr_tls_free(tp);
    syslog(LOG_INFO, "[%s] stream transcribe END\n", TAG);
    return 0;
}