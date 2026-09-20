/*
 * Copyright (C) 2026 Xiaomi Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * DashScope Qwen3-ASR-Flash via WebSocket (OpenAI Realtime-compatible).
 *
 * Protocol: wss://dashscope.aliyuncs.com/api-ws/v1/realtime
 * Auth: DashScope API Key in HTTP header + Bearer token in WS
 * Flow:
 *   1. TLS connect + HTTP Upgrade (with Authorization: Bearer <api_key>)
 *   2. Receive session.created event
 *   3. Send session.update (configure ASR model, language, audio format)
 *   4. Send input_audio_buffer.append (base64 PCM chunks)
 *   5. Send input_audio_buffer.commit (signal end of audio)
 *   6. Receive conversation.item.input_audio_transcription.completed
 *   7. Extract transcript text
 *
 * All events are JSON text frames (not binary like Volcengine).
 */

#include "voice/dashscope_asr.h"
#include "infra/config_store.h"
#include "infra/http_proxy.h"
#include "agent_compat.h"
#include "agent_config.h"
#include "voice/voice_asr.h"

#include "cJSON.h"
#include "mbedtls/base64.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/error.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/ssl.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

static const char* TAG = "ds_asr";

/* ── 回复语言配置（国际化） ─────────────────────────────────
 * 默认 Chinese。调用 dashscope_asr_set_image_reply_lang("English") 切换。
 * 同时作用于 session.update（普通语音对话）和 response.create（图片分析），
 * 保证整个会话语言一致，避免图片分析用英文而后续对话回退中文。 */
static const char *g_img_reply_lang = "chinese";

/* 构建 session 级 instructions：英文模板 + 可配置回复语言。
 * 用于 session.update（初始连接 / restore_omni）。
 * 英文模板保证模型遵循度稳定，Reply in {LANG} 控制输出语言。 */
static void ds_add_session_instructions(cJSON *session)
{
    char buf[384];
    snprintf(buf, sizeof(buf),
        "You are XiaoQ (Chinese name: 小Q), a friendly and concise voice assistant. "
        "Always reply in %s. Keep answers short, natural, and suitable for voice playback. "
        "When asked about your name or identity, you MUST introduce yourself as "
        "\"XiaoQ\" (Chinese: \"小Q\") — NEVER say you are Qwen, Qwen3.5, or any other name.",
        g_img_reply_lang);
    cJSON_AddStringToObject(session, "instructions", buf);
}

#define WS_BUF_SIZE 4096
#define WS_MASK_KEY_LEN 4
#define WS_OPCODE_TEXT 0x01
#define WS_OPCODE_CLOSE 0x08
#define WS_FIN_BIT 0x80
#define WS_MASK_BIT 0x80
#define TLS_READ_TIMEOUT_MS 10000  /* tls_read_all 整体超时上限 */

/* TLS context */
typedef struct {
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config cfg;
    mbedtls_net_context net;
    mbedtls_ctr_drbg_context ctr_drbg;
    unsigned char pending[WS_BUF_SIZE];
    size_t pending_len;
} ds_asr_tls_t;

/* ── Time helper ─────────────────────────────────────────────── */

static uint64_t ds_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ── Entropy ─────────────────────────────────────────────────── */

static int ds_asr_entropy(void* data, unsigned char* output, size_t len)
{
    (void)data;
    if (agent_secure_random(output, len) == 0)
        return 0;
    syslog(LOG_ERR, "[%s] No entropy source\n", TAG);
    return -1;
}

/* ── TLS connect / free ──────────────────────────────────────── */

/*
 * Non-blocking TCP connect with timeout.
 *
 * mbedtls_net_connect() blocks indefinitely on DNS resolution and TCP
 * connect. When the network is unreachable (e.g. after ECONNRESET with
 * stale DNS/route state), the DashScope thread hangs here and never
 * returns to the reconnect loop, leaving the device unresponsive.
 *
 * SO_SNDTIMEO does not affect connect() on NuttX, so we use the same
 * non-blocking connect + select() pattern as open_connect_tunnel() in
 * http_proxy.c.
 */
static int ds_net_connect_timeout(mbedtls_net_context* net,
    const char* host, const char* port, int timeout_ms)
{
    struct addrinfo hints;
    struct addrinfo *res = NULL, *cur;
    char port_str[8];
    int sock = -1;
    int rc;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    snprintf(port_str, sizeof(port_str), "%s", port);

    if (getaddrinfo(host, port_str, &hints, &res) != 0 || !res)
        return -ECONNREFUSED;

    /* Try each resolved address until one connects */
    for (cur = res; cur != NULL; cur = cur->ai_next) {
        sock = (int)socket(cur->ai_family, cur->ai_socktype,
                           cur->ai_protocol);
        if (sock < 0)
            continue;

        int flags = fcntl(sock, F_GETFL, 0);
        fcntl(sock, F_SETFL, flags | O_NONBLOCK);

        rc = connect(sock, cur->ai_addr, cur->ai_addrlen);

        if (rc == 0) {
            /* Immediate success */
            fcntl(sock, F_SETFL, flags);
            freeaddrinfo(res);
            net->fd = sock;
            return 0;
        }

        if (rc != 0 && errno != EINPROGRESS) {
            close(sock);
            sock = -1;
            continue;
        }

        /* Wait for connect to complete with timeout */
        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(sock, &wfds);
        struct timeval tv = {
            .tv_sec = timeout_ms / 1000,
            .tv_usec = (timeout_ms % 1000) * 1000
        };
        rc = select(sock + 1, NULL, &wfds, NULL, &tv);
        if (rc <= 0) {
            close(sock);
            sock = -1;
            continue;
        }

        /* Check SO_ERROR to confirm connect really succeeded */
        int err = 0;
        socklen_t errlen = sizeof(err);
        getsockopt(sock, SOL_SOCKET, SO_ERROR, &err, &errlen);
        if (err != 0) {
            close(sock);
            sock = -1;
            continue;
        }

        /* Success: restore blocking mode for TLS handshake */
        fcntl(sock, F_SETFL, flags);
        freeaddrinfo(res);
        net->fd = sock;
        return 0;
    }

    freeaddrinfo(res);
    return -ETIMEDOUT;
}

static int ds_asr_tls_connect(ds_asr_tls_t* ctx,
    const char* host, const char* port)
{
    int ret;

    mbedtls_ssl_init(&ctx->ssl);
    mbedtls_ssl_config_init(&ctx->cfg);
    mbedtls_net_init(&ctx->net);
    mbedtls_ctr_drbg_init(&ctx->ctr_drbg);
    ctx->pending_len = 0;

    ret = mbedtls_ctr_drbg_seed(&ctx->ctr_drbg, ds_asr_entropy,
        NULL, (const unsigned char*)"ds_asr", 6);
    if (ret != 0)
        return -EIO;

    if (http_proxy_is_enabled()) {
        int port_num = atoi(port);
        int tunnel_fd = proxy_open_tunnel(host, port_num, 30000);
        if (tunnel_fd < 0)
            return -ECONNREFUSED;
        ctx->net.fd = tunnel_fd;
    } else {
        ret = ds_net_connect_timeout(&ctx->net, host, port, 10000);
        if (ret != 0) {
            syslog(LOG_WARNING, "[%s] TCP connect to %s:%s timeout/failed (ret=%d)\n",
                TAG, host, port, ret);
            return ret;
        }
    }

    mbedtls_net_set_block(&ctx->net);
    if (ctx->net.fd >= 0) {
        struct timeval tv = { .tv_sec = 10, .tv_usec = 0 };
        setsockopt(ctx->net.fd, SOL_SOCKET, SO_RCVTIMEO,
            &tv, sizeof(tv));
    }

    ret = mbedtls_ssl_config_defaults(&ctx->cfg,
        MBEDTLS_SSL_IS_CLIENT,
        MBEDTLS_SSL_TRANSPORT_STREAM,
        MBEDTLS_SSL_PRESET_DEFAULT);
    if (ret != 0)
        return -EIO;

    mbedtls_ssl_conf_min_tls_version(&ctx->cfg,
        MBEDTLS_SSL_VERSION_TLS1_2);
#if defined(MBEDTLS_SSL_PROTO_TLS1_3)
    mbedtls_ssl_conf_max_tls_version(&ctx->cfg,
        MBEDTLS_SSL_VERSION_TLS1_3);
#else
    mbedtls_ssl_conf_max_tls_version(&ctx->cfg,
        MBEDTLS_SSL_VERSION_TLS1_2);
#endif

    mbedtls_ssl_conf_authmode(&ctx->cfg, MBEDTLS_SSL_VERIFY_OPTIONAL);
    mbedtls_ssl_conf_rng(&ctx->cfg, mbedtls_ctr_drbg_random,
        &ctx->ctr_drbg);

    ret = mbedtls_ssl_setup(&ctx->ssl, &ctx->cfg);
    if (ret != 0)
        return -EIO;

    mbedtls_ssl_set_hostname(&ctx->ssl, host);
    mbedtls_ssl_set_bio(&ctx->ssl, &ctx->net,
        mbedtls_net_send, mbedtls_net_recv, NULL);

    while ((ret = mbedtls_ssl_handshake(&ctx->ssl)) != 0) {
        if (ret != MBEDTLS_ERR_SSL_WANT_READ
            && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
            syslog(LOG_ERR, "[%s] handshake: -0x%04x\n", TAG, -ret);
            return -EIO;
        }
    }

    syslog(LOG_INFO, "[%s] TLS connected to %s:%s\n", TAG, host, port);
    return 0;
}

static void ds_asr_tls_free(ds_asr_tls_t* ctx)
{
    mbedtls_ssl_close_notify(&ctx->ssl);
    mbedtls_net_free(&ctx->net);
    mbedtls_ssl_free(&ctx->ssl);
    mbedtls_ssl_config_free(&ctx->cfg);
    mbedtls_ctr_drbg_free(&ctx->ctr_drbg);
}

/* ── TLS I/O helpers ─────────────────────────────────────────── */

static int tls_write_all(ds_asr_tls_t* ctx,
    const unsigned char* buf, size_t len)
{
    size_t written = 0;
    while (written < len) {
        int ret = mbedtls_ssl_write(&ctx->ssl, buf + written,
            len - written);
        if (ret > 0) {
            written += (size_t)ret;
        } else if (ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
            return -EIO;
        }
    }
    return 0;
}

static int tls_read_all(ds_asr_tls_t* ctx,
    unsigned char* buf, size_t len)
{
    size_t got = 0;

    if (ctx->pending_len > 0) {
        size_t copy = (ctx->pending_len < len) ? ctx->pending_len : len;
        memcpy(buf, ctx->pending, copy);
        got = copy;
        if (ctx->pending_len > copy) {
            memmove(ctx->pending, ctx->pending + copy,
                ctx->pending_len - copy);
        }
        ctx->pending_len -= copy;
        if (got >= len)
            return 0;
    }

    /* poll + deadline 方案：整体超时 TLS_READ_TIMEOUT_MS，避免服务端
     * 断连或无响应时长时间阻塞导致设备卡死。 */
    uint64_t deadline = ds_now_ms() + TLS_READ_TIMEOUT_MS;

    while (got < len) {
        int ret = mbedtls_ssl_read(&ctx->ssl, buf + got, len - got);
        if (ret > 0) {
            got += (size_t)ret;
            continue;
        }
        if (ret == 0
            || ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY
            || ret == MBEDTLS_ERR_SSL_CONN_EOF
            || ret == MBEDTLS_ERR_NET_RECV_FAILED
            || ret == MBEDTLS_ERR_NET_CONN_RESET) {
            return -ECONNRESET;
        }
        if (ret != MBEDTLS_ERR_SSL_WANT_READ) {
            syslog(LOG_ERR, "[%s] ssl_read: -0x%04x\n", TAG, -ret);
            return -EIO;
        }

        /* WANT_READ: 用 poll 等待底层 socket 可读，带剩余超时 */
        uint64_t now = ds_now_ms();
        if (now >= deadline) {
            syslog(LOG_ERR, "[%s] tls_read_all timed out (%zu/%zu bytes)\n",
                TAG, got, len);
            return -ETIMEDOUT;
        }
        int remaining_ms = (int)(deadline - now);
        struct pollfd pfd;
        pfd.fd = ctx->net.fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        int pret = poll(&pfd, 1, remaining_ms);
        if (pret <= 0) {
            syslog(LOG_ERR, "[%s] tls_read_all poll timeout (%zu/%zu bytes)\n",
                TAG, got, len);
            return -ETIMEDOUT;
        }
    }
    return 0;
}

/* ── WebSocket upgrade ───────────────────────────────────────── */

static int ds_ws_upgrade(ds_asr_tls_t* ctx,
    const char* host, const char* path,
    const char* api_key)
{
    unsigned char key_raw[16];
    unsigned char key_b64[32];
    size_t key_b64_len;

    ds_asr_entropy(NULL, key_raw, sizeof(key_raw));
    mbedtls_base64_encode(key_b64, sizeof(key_b64), &key_b64_len,
        key_raw, sizeof(key_raw));

    char* req = malloc(768);
    if (!req)
        return -ENOMEM;
    int n = snprintf(req, 768,
        "GET %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: %.*s\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "Authorization: Bearer %s\r\n"
        "\r\n",
        path, host, (int)key_b64_len, key_b64, api_key);

    if (n <= 0 || n >= 768) {
        free(req);
        return -EOVERFLOW;
    }

    int ret = tls_write_all(ctx, (const unsigned char*)req, (size_t)n);
    free(req);
    if (ret != 0)
        return ret;

    char* resp = malloc(WS_BUF_SIZE);
    if (!resp)
        return -ENOMEM;
    size_t rlen = 0;
    while (rlen < WS_BUF_SIZE - 1) {
        int r = mbedtls_ssl_read(&ctx->ssl,
            (unsigned char*)resp + rlen,
            WS_BUF_SIZE - 1 - rlen);
        if (r > 0) {
            rlen += (size_t)r;
            resp[rlen] = '\0';
            if (strstr(resp, "\r\n\r\n"))
                break;
        } else if (r == 0 || r == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
            free(resp);
            return -ECONNRESET;
        } else if (r != MBEDTLS_ERR_SSL_WANT_READ) {
            free(resp);
            return -EIO;
        } else {
            usleep(5000);
        }
    }

    int status = 0;
    if (sscanf(resp, "HTTP/1.1 %d", &status) != 1 || status != 101) {
        syslog(LOG_ERR, "[%s] WS upgrade failed: HTTP %d\n", TAG, status);
        free(resp);
        return -EPROTO;
    }

    char* hdr_end = strstr(resp, "\r\n\r\n");
    if (hdr_end) {
        hdr_end += 4;
        size_t leftover = rlen - (size_t)(hdr_end - resp);
        if (leftover > 0 && leftover <= sizeof(ctx->pending)) {
            memcpy(ctx->pending, hdr_end, leftover);
            ctx->pending_len = leftover;
            syslog(LOG_INFO, "[%s] WS upgrade: %zu bytes pending after headers\n",
                TAG, leftover);
        }
    }

    free(resp);
    syslog(LOG_INFO, "[%s] WebSocket upgrade OK\n", TAG);
    return 0;
}

/* ── WebSocket frame send (text, masked) ─────────────────────── */

static int ws_send_text(ds_asr_tls_t* ctx,
    const char* payload, size_t plen)
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
    ds_asr_entropy(NULL, mask, WS_MASK_KEY_LEN);
    memcpy(hdr + hdr_len, mask, WS_MASK_KEY_LEN);
    hdr_len += WS_MASK_KEY_LEN;

    int ret = tls_write_all(ctx, hdr, hdr_len);
    if (ret != 0)
        return ret;

    /* Mask and send payload in chunks */
    unsigned char chunk[1024];
    size_t sent = 0;
    while (sent < plen) {
        size_t clen = plen - sent;
        if (clen > sizeof(chunk))
            clen = sizeof(chunk);
        for (size_t i = 0; i < clen; i++)
            chunk[i] = ((const unsigned char*)payload)[sent + i]
                ^ mask[(sent + i) % 4];
        ret = tls_write_all(ctx, chunk, clen);
        if (ret != 0)
            return ret;
        sent += clen;
    }

    return 0;
}

/* ── WebSocket frame recv (text, unmasked) ───────────────────── */

static int ws_recv_text(ds_asr_tls_t* ctx,
    char* buf, size_t cap)
{
    unsigned char hdr[2];
    int ret = tls_read_all(ctx, hdr, 2);
    if (ret != 0)
        return ret;

    int opcode = hdr[0] & 0x0F;
    size_t plen = hdr[1] & 0x7F;

    if (opcode == WS_OPCODE_CLOSE) {
        unsigned char close_payload[512];
        unsigned char close_data[2];
        size_t close_len = 0;
        if (plen == 126) {
            unsigned char ext[2];
            if (tls_read_all(ctx, ext, 2) == 0)
                plen = ((size_t)ext[0] << 8) | ext[1];
        }
        if (plen >= 2) {
            if (tls_read_all(ctx, close_data, 2) == 0) {
                unsigned int code = ((unsigned int)close_data[0] << 8) | close_data[1];
                close_len = plen - 2;
                if (close_len > 0) {
                    size_t read_len = close_len;
                    if (read_len >= sizeof(close_payload))
                        read_len = sizeof(close_payload) - 1;
                    tls_read_all(ctx, close_payload, read_len);
                    close_payload[read_len] = '\0';
                } else {
                    close_payload[0] = '\0';
                }
                syslog(LOG_WARNING, "[%s] WS close frame: code=%u reason=%.500s\n",
                    TAG, code, close_payload);
            }
        }
        return 0;
    }

    /* Handle ping */
    if (opcode == 0x09) {
        if (plen == 126) {
            unsigned char ext[2];
            tls_read_all(ctx, ext, 2);
            plen = ((size_t)ext[0] << 8) | ext[1];
        }
        unsigned char pp[128];
        if (plen > 0 && plen <= 125)
            tls_read_all(ctx, pp, plen);
        unsigned char pong[2] = { 0x8A, (unsigned char)plen };
        tls_write_all(ctx, pong, 2);
        if (plen > 0 && plen <= 125)
            tls_write_all(ctx, pp, plen);
        return -2; /* continue */
    }

    if (opcode != WS_OPCODE_TEXT)
        return -2; /* skip non-text */

    if (plen == 126) {
        unsigned char ext[2];
        ret = tls_read_all(ctx, ext, 2);
        if (ret != 0)
            return -EIO;
        plen = ((size_t)ext[0] << 8) | ext[1];
    } else if (plen == 127) {
        unsigned char ext[8];
        ret = tls_read_all(ctx, ext, 8);
        if (ret != 0)
            return -EIO;
        plen = 0;
        for (int i = 0; i < 8; i++)
            plen = (plen << 8) | ext[i];
        if (plen > 1024 * 1024)
            return -EOVERFLOW;
    }

    if (plen >= cap)
        return -EOVERFLOW;

    /* Read full payload */
    ret = tls_read_all(ctx, (unsigned char*)buf, plen);
    if (ret != 0)
        return ret;
    buf[plen] = '\0';
    return (int)plen;
}

/* ── Credentials ─────────────────────────────────────────────── */

static char s_api_key[128];
static char s_model[64];

static int dashscope_asr_init(void)
{
    memset(s_api_key, 0, sizeof(s_api_key));
    memset(s_model, 0, sizeof(s_model));

    claw_config_get(AGENT_CFG_KEY_DASHSCOPE_API_KEY,
        s_api_key, sizeof(s_api_key));

    /* Try LLM api_key as fallback for DashScope */
    if (s_api_key[0] == '\0') {
        claw_config_get(AGENT_CFG_KEY_API_KEY,
            s_api_key, sizeof(s_api_key));
    }

    /* 优先从 /emmc/wifi/agent_app_id 文件读取（最高优先级） */
    {
        char file_key[128] = { 0 };
        if (claw_load_file_string("/emmc/wifi/agent_app_id",
                                  file_key, sizeof(file_key)) == OK) {
            strncpy(s_api_key, file_key, sizeof(s_api_key) - 1);
            s_api_key[sizeof(s_api_key) - 1] = '\0';
        }
    }

    if (claw_config_get(AGENT_CFG_KEY_DASHSCOPE_ASR_MODEL,
            s_model, sizeof(s_model)) != OK
        || s_model[0] == '\0') {
        /* 默认使用宏定义的 ASR 模型 */
        strncpy(s_model, AGENT_DASHSCOPE_ASR_MODEL,
            sizeof(s_model) - 1);
    }

    /* 优先从 /emmc/wifi/agent_asr_model 文件读取，覆盖默认宏 */
    {
        char file_model[128] = { 0 };
        if (claw_load_file_string("/emmc/wifi/agent_asr_model",
                                  file_model, sizeof(file_model)) == OK) {
            strncpy(s_model, file_model, sizeof(s_model) - 1);
            s_model[sizeof(s_model) - 1] = '\0';
            syslog(LOG_INFO, "[%s] ASR model loaded from /emmc/wifi/agent_asr_model: %s\n", TAG, s_model);
        }
    }

    return 0;
}

/* ── PCM to base64 helper ────────────────────────────────────── */

static char* pcm_to_base64(const unsigned char* pcm, size_t pcm_len)
{
    size_t b64_len;
    /* Calculate output size: ceil(pcm_len/3)*4 + 1 */
    size_t out_size = (pcm_len + 2) / 3 * 4 + 1;
    char* b64 = malloc(out_size);
    if (!b64)
        return NULL;

    if (mbedtls_base64_encode((unsigned char*)b64, out_size,
            &b64_len, pcm, pcm_len) != 0) {
        free(b64);
        return NULL;
    }
    b64[b64_len] = '\0';
    return b64;
}

/* ── Ops: recognize (one-shot) ───────────────────────────────── */

static int dashscope_asr_recognize(const unsigned char* pcm_data,
    size_t pcm_len,
    char* text_out,
    size_t text_cap)
{
    dashscope_asr_init();

    if (s_api_key[0] == '\0') {
        syslog(LOG_ERR, "[%s] DashScope API key not configured\n", TAG);
        return -ENOENT;
    }

    text_out[0] = '\0';

    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    ds_asr_tls_t ctx;
    int ret;

    /* 1. TLS connect */
    ret = ds_asr_tls_connect(&ctx, AGENT_DASHSCOPE_ASR_HOST,
        AGENT_DASHSCOPE_ASR_PORT);
    if (ret != 0) {
        ds_asr_tls_free(&ctx);
        return ret;
    }

    /* 2. WS upgrade with Bearer token and model parameter */
    char* ws_path = malloc(256);
    if (!ws_path) {
        ds_asr_tls_free(&ctx);
        return -ENOMEM;
    }
    snprintf(ws_path, 256, "%s?model=%s",
        AGENT_DASHSCOPE_ASR_WS_PATH, s_model);
    ret = ds_ws_upgrade(&ctx, AGENT_DASHSCOPE_ASR_HOST,
        ws_path, s_api_key);
    free(ws_path);
    if (ret != 0) {
        ds_asr_tls_free(&ctx);
        return ret;
    }

    /* 3. Wait for session.created */
    {
        char* frame_buf = malloc(WS_BUF_SIZE);
        if (!frame_buf) {
            ds_asr_tls_free(&ctx);
            return -ENOMEM;
        }
        int n;
        do {
            n = ws_recv_text(&ctx, frame_buf, WS_BUF_SIZE);
        } while (n == -2); /* 跳过 ping frame */
        if (n <= 0) {
            syslog(LOG_ERR, "[%s] Failed to receive session.created (n=%d)\n", TAG, n);
            free(frame_buf);
            ds_asr_tls_free(&ctx);
            return -EPROTO;
        }
        syslog(LOG_INFO, "[%s] Got session event: %.80s\n", TAG, frame_buf);
        free(frame_buf);
    }

    /* 4. Send session.update to configure Omni multimodal */
    {
        cJSON* root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "type", "session.update");
        cJSON_AddStringToObject(root, "event_id", "evt_asr_session");
        cJSON* session = cJSON_AddObjectToObject(root, "session");
        cJSON_AddStringToObject(session, "input_audio_format", "pcm");

        cJSON* modalities = cJSON_AddArrayToObject(session, "modalities");
        cJSON_AddItemToArray(modalities, cJSON_CreateString("text"));
        cJSON_AddItemToArray(modalities, cJSON_CreateString("audio"));

        cJSON_AddStringToObject(session, "voice", AGENT_DASHSCOPE_OMNI_VOICE);
        cJSON_AddStringToObject(session, "output_audio_format", "pcm");
        ds_add_session_instructions(session);

        cJSON_AddBoolToObject(session, "enable_input_audio_transcription", 1);
        cJSON* td = cJSON_AddObjectToObject(session, "turn_detection");
        cJSON_AddStringToObject(td, "type", "server_vad");
        cJSON_AddNumberToObject(td, "threshold", 0.5);
        cJSON_AddNumberToObject(td, "silence_duration_ms", 800);

        char* json = cJSON_PrintUnformatted(root);
        cJSON_Delete(root);
        if (!json) {
            ds_asr_tls_free(&ctx);
            return -ENOMEM;
        }
        ret = ws_send_text(&ctx, json, strlen(json));
        free(json);
        if (ret != 0) {
            ds_asr_tls_free(&ctx);
            return ret;
        }
    }

    /* 4b. Wait for session.updated */
    {
        char* frame_buf = malloc(WS_BUF_SIZE);
        if (!frame_buf) {
            ds_asr_tls_free(&ctx);
            return -ENOMEM;
        }
        int n;
        do {
            n = ws_recv_text(&ctx, frame_buf, WS_BUF_SIZE);
        } while (n == -2); /* 跳过 ping frame */
        if (n <= 0) {
            syslog(LOG_ERR, "[%s] No session.updated (recv=%d)\n", TAG, n);
            free(frame_buf);
            ds_asr_tls_free(&ctx);
            return -EPROTO;
        }
        frame_buf[n < WS_BUF_SIZE - 1 ? n : WS_BUF_SIZE - 1] = '\0';
        cJSON* root = cJSON_Parse(frame_buf);
        if (root) {
            cJSON* type_item = cJSON_GetObjectItem(root, "type");
            if (cJSON_IsString(type_item)
                && strcmp(type_item->valuestring, "error") == 0) {
                cJSON* err = cJSON_GetObjectItem(root, "error");
                cJSON* msg = err ? cJSON_GetObjectItem(err, "message")
                                 : NULL;
                syslog(LOG_ERR, "[%s] session.update error: %s\n", TAG,
                    (msg && cJSON_IsString(msg)) ? msg->valuestring
                                                 : frame_buf);
                cJSON_Delete(root);
                free(frame_buf);
                ds_asr_tls_free(&ctx);
                return -EPROTO;
            }
            cJSON_Delete(root);
        }
        free(frame_buf);
        syslog(LOG_INFO, "[%s] session.updated OK\n", TAG);
    }

    /* 5. Send audio chunks as input_audio_buffer.append */
    size_t offset = 0;
    size_t chunk_size = AGENT_ASR_CHUNK_SIZE;
    int chunk_seq = 0;
    while (offset < pcm_len) {
        size_t remain = pcm_len - offset;
        size_t clen = (remain < chunk_size) ? remain : chunk_size;

        char* b64 = pcm_to_base64(pcm_data + offset, clen);
        if (!b64) {
            ds_asr_tls_free(&ctx);
            return -ENOMEM;
        }

        char evt_id[32];
        snprintf(evt_id, sizeof(evt_id), "evt_aud_%d", chunk_seq++);

        cJSON* root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "type", "input_audio_buffer.append");
        cJSON_AddStringToObject(root, "event_id", evt_id);
        cJSON_AddStringToObject(root, "audio", b64);

        char* json = cJSON_PrintUnformatted(root);
        cJSON_Delete(root);
        free(b64);

        if (!json) {
            ds_asr_tls_free(&ctx);
            return -ENOMEM;
        }

        ret = ws_send_text(&ctx, json, strlen(json));
        free(json);
        if (ret != 0) {
            ds_asr_tls_free(&ctx);
            return ret;
        }
        offset += clen;
    }

    /* 6. Send session.finish to end the session */
    {
        const char* finish = "{\"type\":\"session.finish\",\"event_id\":\"evt_finish\"}";
        ret = ws_send_text(&ctx, finish, strlen(finish));
        if (ret != 0) {
            ds_asr_tls_free(&ctx);
            return ret;
        }
    }

    /* 7. Receive responses until we get the final transcription */
    int attempts = 0;
    char* frame_buf = malloc(WS_BUF_SIZE * 2);
    if (!frame_buf) {
        ds_asr_tls_free(&ctx);
        return -ENOMEM;
    }
    while (attempts < 200) {
        int n = ws_recv_text(&ctx, frame_buf, WS_BUF_SIZE * 2);
        if (n == 0)
            break; /* close */
        if (n < 0) {
            if (n == -2) {
                attempts++;
                continue; /* ping handled */
            }
            break;
        }

        frame_buf[n < WS_BUF_SIZE * 2 - 1 ? n : WS_BUF_SIZE * 2 - 1] = '\0';

        cJSON* root = cJSON_Parse(frame_buf);
        if (!root) {
            syslog(LOG_WARNING, "[%s] unparseable frame (%d bytes)\n", TAG, n);
            attempts++;
            continue;
        }

        cJSON* type_item = cJSON_GetObjectItem(root, "type");
        if (!cJSON_IsString(type_item)) {
            cJSON_Delete(root);
            attempts++;
            continue;
        }

        const char* evt_type = type_item->valuestring;

        if (strcmp(evt_type,
                "conversation.item.input_audio_transcription.completed")
            == 0) {
            cJSON* transcript = cJSON_GetObjectItem(root,
                "transcript");
            if (cJSON_IsString(transcript)
                && transcript->valuestring[0] != '\0') {
                strncpy(text_out, transcript->valuestring,
                    text_cap - 1);
                text_out[text_cap - 1] = '\0';
            }
            cJSON_Delete(root);
            break;
        }

        if (strcmp(evt_type,
                "conversation.item.input_audio_transcription.text")
            == 0) {
            cJSON* stash = cJSON_GetObjectItem(root, "stash");
            if (cJSON_IsString(stash) && stash->valuestring[0]) {
                strncat(text_out, stash->valuestring,
                    text_cap - strlen(text_out) - 1);
            }
            cJSON_Delete(root);
            attempts++;
            continue;
        }

        if (strcmp(evt_type, "response.text.delta") == 0) {
            cJSON* delta = cJSON_GetObjectItem(root, "delta");
            if (cJSON_IsString(delta) && delta->valuestring[0]) {
                strncat(text_out, delta->valuestring,
                    text_cap - strlen(text_out) - 1);
            }
            cJSON_Delete(root);
            attempts++;
            continue;
        }

        if (strcmp(evt_type, "response.text.done") == 0) {
            cJSON* text_item = cJSON_GetObjectItem(root, "text");
            if (cJSON_IsString(text_item) && text_item->valuestring[0]) {
                strncpy(text_out, text_item->valuestring,
                    text_cap - 1);
                text_out[text_cap - 1] = '\0';
            }
            cJSON_Delete(root);
            break;
        }

        if (strcmp(evt_type, "error") == 0) {
            cJSON* err = cJSON_GetObjectItem(root, "error");
            cJSON* msg = err ? cJSON_GetObjectItem(err, "message")
                             : NULL;
            syslog(LOG_ERR, "[%s] ASR error: %s\n", TAG,
                (msg && cJSON_IsString(msg)) ? msg->valuestring
                                             : "unknown");
            cJSON_Delete(root);
            free(frame_buf);
            ds_asr_tls_free(&ctx);
            return -EIO;
        }

        if (strcmp(evt_type, "session.finished") == 0) {
            cJSON_Delete(root);
            break;
        }

        syslog(LOG_INFO, "[%s] ASR event: %s\n", TAG, evt_type);
        cJSON_Delete(root);
        attempts++;
    }

    free(frame_buf);
    ds_asr_tls_free(&ctx);

    struct timespec t1;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    long ms = (t1.tv_sec - t0.tv_sec) * 1000
        + (t1.tv_nsec - t0.tv_nsec) / 1000000;

    if (text_out[0] == '\0') {
        syslog(LOG_WARNING, "[%s] No text recognized (%ldms)\n", TAG, ms);
        return -ENODATA;
    }

    syslog(LOG_INFO, "[%s] Recognized: %s (%ldms)\n",
        TAG, text_out, ms);
    return 0;
}

/* ── Backend ops registration ─────────────────────────────────── */

static const voice_asr_ops_t s_dashscope_asr_ops = {
    .name = "dashscope",
    .init = dashscope_asr_init,
    .recognize = dashscope_asr_recognize,
    .deinit = NULL,
};

int dashscope_asr_register(void)
{
    return voice_asr_register(&s_dashscope_asr_ops);
}

/* ── Streaming ASR ──────────────────────────────────────────── */

struct ds_asr_stream {
    ds_asr_tls_t ctx;
    int connected;
    uint32_t seq;
};

ds_asr_stream_t* dashscope_asr_stream_open(void)
{
    if (s_api_key[0] == '\0') {
        syslog(LOG_ERR, "[%s] stream: API key not configured\n", TAG);
        return NULL;
    }

    ds_asr_stream_t* s = calloc(1, sizeof(*s));
    if (!s) {
        return NULL;
    }

    int ret = ds_asr_tls_connect(&s->ctx,
        AGENT_DASHSCOPE_ASR_HOST, AGENT_DASHSCOPE_ASR_PORT);
    if (ret != 0) {
        free(s);
        return NULL;
    }

    char* ws_path = malloc(256);
    if (!ws_path) {
        ds_asr_tls_free(&s->ctx);
        free(s);
        return NULL;
    }
    snprintf(ws_path, 256, "%s?model=%s",
        AGENT_DASHSCOPE_ASR_WS_PATH, s_model);
    ret = ds_ws_upgrade(&s->ctx, AGENT_DASHSCOPE_ASR_HOST,
        ws_path, s_api_key);
    free(ws_path);
    if (ret != 0) {
        ds_asr_tls_free(&s->ctx);
        free(s);
        return NULL;
    }

    {
        char* frame_buf = malloc(WS_BUF_SIZE);
        if (!frame_buf) {
            ds_asr_tls_free(&s->ctx);
            free(s);
            return NULL;
        }
        syslog(LOG_INFO, "[%s] stream: waiting for session.created...\n", TAG);
        int n;
        do {
            n = ws_recv_text(&s->ctx, frame_buf, WS_BUF_SIZE);
        } while (n == -2); /* 跳过 ping frame */
        if (n <= 0) {
            syslog(LOG_ERR, "[%s] stream: no session.created (n=%d)\n", TAG, n);
            free(frame_buf);
            ds_asr_tls_free(&s->ctx);
            free(s);
            return NULL;
        }
        syslog(LOG_INFO, "[%s] stream: got session event: %.80s\n", TAG, frame_buf);
        free(frame_buf);
    }

    {
        cJSON* root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "type", "session.update");
        cJSON_AddStringToObject(root, "event_id", "evt_asr_stream");
        cJSON* session = cJSON_AddObjectToObject(root, "session");
        cJSON_AddStringToObject(session, "input_audio_format", "pcm");

        cJSON* modalities = cJSON_AddArrayToObject(session, "modalities");
        cJSON_AddItemToArray(modalities, cJSON_CreateString("text"));
        cJSON_AddItemToArray(modalities, cJSON_CreateString("audio"));

        cJSON_AddStringToObject(session, "voice", AGENT_DASHSCOPE_OMNI_VOICE);
        cJSON_AddStringToObject(session, "output_audio_format", "pcm");
        ds_add_session_instructions(session);

        cJSON_AddBoolToObject(session, "enable_input_audio_transcription", 1);
        cJSON* td = cJSON_AddObjectToObject(session, "turn_detection");
        cJSON_AddStringToObject(td, "type", "server_vad");
        cJSON_AddNumberToObject(td, "threshold", 0.5);
        cJSON_AddNumberToObject(td, "silence_duration_ms", 800);

        char* json = cJSON_PrintUnformatted(root);
        cJSON_Delete(root);
        if (!json) {
            ds_asr_tls_free(&s->ctx);
            free(s);
            return NULL;
        }
        ret = ws_send_text(&s->ctx, json, strlen(json));
        free(json);
        if (ret != 0) {
            ds_asr_tls_free(&s->ctx);
            free(s);
            return NULL;
        }
    }

    {
        char* frame_buf = malloc(WS_BUF_SIZE);
        if (!frame_buf) {
            ds_asr_tls_free(&s->ctx);
            free(s);
            return NULL;
        }
        int n;
        do {
            n = ws_recv_text(&s->ctx, frame_buf, WS_BUF_SIZE);
        } while (n == -2); /* 跳过 ping frame */
        if (n <= 0) {
            syslog(LOG_ERR, "[%s] stream: no session.updated (n=%d)\n", TAG, n);
            free(frame_buf);
            ds_asr_tls_free(&s->ctx);
            free(s);
            return NULL;
        }
        frame_buf[n < WS_BUF_SIZE - 1 ? n : WS_BUF_SIZE - 1] = '\0';
        cJSON* root = cJSON_Parse(frame_buf);
        if (root) {
            cJSON* type_item = cJSON_GetObjectItem(root, "type");
            if (cJSON_IsString(type_item)
                && strcmp(type_item->valuestring, "error") == 0) {
                cJSON* err = cJSON_GetObjectItem(root, "error");
                cJSON* msg = err ? cJSON_GetObjectItem(err, "message")
                                 : NULL;
                syslog(LOG_ERR, "[%s] stream session.update error: %s\n",
                    TAG, (msg && cJSON_IsString(msg)) ? msg->valuestring
                                                       : frame_buf);
                cJSON_Delete(root);
                free(frame_buf);
                ds_asr_tls_free(&s->ctx);
                free(s);
                return NULL;
            }
            cJSON_Delete(root);
        }
        free(frame_buf);
    }

    s->connected = 1;
    syslog(LOG_INFO, "[%s] stream: connected\n", TAG);
    return s;
}

int dashscope_asr_stream_send(ds_asr_stream_t* s,
    const unsigned char* pcm, size_t len)
{
    if (!s || !s->connected) {
        return -EINVAL;
    }

    char* b64 = pcm_to_base64(pcm, len);
    if (!b64) {
        return -ENOMEM;
    }

    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "input_audio_buffer.append");
    char evt_id[64];
    snprintf(evt_id, sizeof(evt_id), "evt_aud_%u", s->seq++);
    cJSON_AddStringToObject(root, "event_id", evt_id);
    cJSON_AddStringToObject(root, "audio", b64);

    char* json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    free(b64);

    if (!json) {
        return -ENOMEM;
    }

    int ret = ws_send_text(&s->ctx, json, strlen(json));
    free(json);
    return ret;
}

int dashscope_asr_stream_recv(ds_asr_stream_t* s,
    char* buf, size_t cap)
{
    if (!s || !s->connected) {
        return -ENOTCONN;
    }

    if (cap < WS_BUF_SIZE) {
        return -ENOBUFS;
    }

    int n = ws_recv_text(&s->ctx, buf, cap);
    if (n <= 0) {
        if (n == 0) {
            syslog(LOG_INFO, "[%s] stream: WS close frame received\n", TAG);
            s->connected = 0;
            return -ECONNRESET;
        }
        if (n == -2) {
            return -2;
        }
        if (n == -ECONNRESET || n == -EIO) {
            syslog(LOG_WARNING, "[%s] stream: connection lost (n=%d)\n", TAG, n);
            s->connected = 0;
            return -ECONNRESET;
        }
        syslog(LOG_WARNING, "[%s] stream: ws_recv_text failed (n=%d)\n", TAG, n);
        return n;
    }

    return n;
}

int dashscope_asr_stream_send_ping(ds_asr_stream_t* s)
{
    if (!s || !s->connected) {
        return -ENOTCONN;
    }

    unsigned char ping_frame[6];
    ping_frame[0] = 0x89;
    ping_frame[1] = 0x80;
    ds_asr_entropy(NULL, ping_frame + 2, 4);
    int ret = tls_write_all(&s->ctx, ping_frame, sizeof(ping_frame));
    if (ret != 0) {
        syslog(LOG_WARNING, "[%s] stream: send ping failed (ret=%d)\n", TAG, ret);
        return ret;
    }

    return 0;
}

int dashscope_asr_stream_finish(ds_asr_stream_t* s,
    char* text_out, size_t text_cap)
{
    if (!s) {
        return -EINVAL;
    }

    text_out[0] = '\0';

    if (!s->connected) {
        free(s);
        return -ENOTCONN;
    }

    {
        const char* finish = "{\"type\":\"session.finish\",\"event_id\":\"evt_stream_finish\"}";
        int ret = ws_send_text(&s->ctx, finish, strlen(finish));
        if (ret != 0) {
            ds_asr_tls_free(&s->ctx);
            free(s);
            return ret;
        }
    }

    int attempts = 0;
    char* frame_buf = malloc(WS_BUF_SIZE * 2);
    if (!frame_buf) {
        ds_asr_tls_free(&s->ctx);
        free(s);
        return -ENOMEM;
    }
    while (attempts < 200) {
        int n = ws_recv_text(&s->ctx, frame_buf, WS_BUF_SIZE * 2);
        if (n == 0)
            break;
        if (n < 0) {
            if (n == -2) {
                attempts++;
                continue;
            }
            break;
        }

        frame_buf[n < WS_BUF_SIZE * 2 - 1 ? n : WS_BUF_SIZE * 2 - 1] = '\0';

        cJSON* root = cJSON_Parse(frame_buf);
        if (!root) {
            attempts++;
            continue;
        }

        cJSON* type_item = cJSON_GetObjectItem(root, "type");
        if (!cJSON_IsString(type_item)) {
            cJSON_Delete(root);
            attempts++;
            continue;
        }

        const char* evt_type = type_item->valuestring;

        if (strcmp(evt_type,
                "conversation.item.input_audio_transcription.completed")
            == 0) {
            cJSON* transcript = cJSON_GetObjectItem(root, "transcript");
            if (cJSON_IsString(transcript)
                && transcript->valuestring[0] != '\0') {
                strncpy(text_out, transcript->valuestring,
                    text_cap - 1);
                text_out[text_cap - 1] = '\0';
            }
            cJSON_Delete(root);
            break;
        }

        if (strcmp(evt_type,
                "conversation.item.input_audio_transcription.text")
            == 0) {
            cJSON* stash = cJSON_GetObjectItem(root, "stash");
            if (cJSON_IsString(stash) && stash->valuestring[0]) {
                strncat(text_out, stash->valuestring,
                    text_cap - strlen(text_out) - 1);
            }
            cJSON_Delete(root);
            attempts++;
            continue;
        }

        if (strcmp(evt_type, "response.text.delta") == 0) {
            cJSON* delta = cJSON_GetObjectItem(root, "delta");
            if (cJSON_IsString(delta) && delta->valuestring[0]) {
                strncat(text_out, delta->valuestring,
                    text_cap - strlen(text_out) - 1);
            }
            cJSON_Delete(root);
            attempts++;
            continue;
        }

        if (strcmp(evt_type, "response.text.done") == 0) {
            cJSON* text_item = cJSON_GetObjectItem(root, "text");
            if (cJSON_IsString(text_item) && text_item->valuestring[0]) {
                strncpy(text_out, text_item->valuestring,
                    text_cap - 1);
                text_out[text_cap - 1] = '\0';
            }
            cJSON_Delete(root);
            break;
        }

        if (strcmp(evt_type, "error") == 0) {
            cJSON* err = cJSON_GetObjectItem(root, "error");
            cJSON* msg = err ? cJSON_GetObjectItem(err, "message")
                             : NULL;
            syslog(LOG_ERR, "[%s] stream ASR error: %s\n", TAG,
                (msg && cJSON_IsString(msg)) ? msg->valuestring
                                             : "unknown");
            cJSON_Delete(root);
            free(frame_buf);
            ds_asr_tls_free(&s->ctx);
            s->connected = 0;
            free(s);
            return -EIO;
        }

        if (strcmp(evt_type, "session.finished") == 0) {
            cJSON_Delete(root);
            break;
        }

        syslog(LOG_INFO, "[%s] stream ASR event: %s\n", TAG, evt_type);
        cJSON_Delete(root);
        attempts++;
    }

    free(frame_buf);
    ds_asr_tls_free(&s->ctx);
    s->connected = 0;

    if (text_out[0] == '\0') {
        free(s);
        return -ENODATA;
    }

    syslog(LOG_INFO, "[%s] stream recognized: %.80s\n", TAG, text_out);
    free(s);
    return 0;
}

void dashscope_asr_stream_abort(ds_asr_stream_t* s)
{
    if (!s) {
        return;
    }

    if (s->connected) {
        ds_asr_tls_free(&s->ctx);
        s->connected = 0;
    }

    free(s);
}

int dashscope_asr_stream_vad_done(ds_asr_stream_t* s)
{
    if (!s || !s->connected)
        return 0;

    struct pollfd pfd;
    pfd.fd = s->ctx.net.fd;
    pfd.events = POLLIN;
    pfd.revents = 0;

    int pret = poll(&pfd, 1, 0);
    if (pret <= 0)
        return 0;

    return 1;
}

int dashscope_asr_stream_clear_buffer(ds_asr_stream_t* s)
{
    if (!s || !s->connected) {
        return -ENOTCONN;
    }

    const char* clear_cmd =
        "{\"type\":\"input_audio_buffer.clear\","
        "\"event_id\":\"evt_buf_clear\"}";

    int ret = ws_send_text(&s->ctx, clear_cmd, strlen(clear_cmd));
    if (ret != 0) {
        syslog(LOG_WARNING, "[%s] stream: clear buffer failed (ret=%d)\n",
            TAG, ret);
        return ret;
    }

    syslog(LOG_INFO, "[%s] stream: input_audio_buffer.clear sent\n", TAG);
    return 0;
}

/* ── 图片AI分析：静音PCM发送 ────────────────────────────────────
 * 发送一段全0的PCM数据，满足"发送input_image_buffer.append前
 * 至少发送过一次input_audio_buffer.append"的前置条件。
 * 16kHz/16bit/mono：100ms = 3200字节。静音不含任何语音信息，
 * 零干扰图片分析。 */
int dashscope_asr_stream_send_silent(ds_asr_stream_t* s, size_t ms)
{
    if (!s || !s->connected) {
        return -ENOTCONN;
    }
    if (ms == 0) ms = 100;

    /* 计算PCM字节数：16000Hz * 2bytes * 1ch * (ms/1000) */
    size_t pcm_bytes = 16000 * 2 * (ms / 1000);
    if (pcm_bytes == 0) pcm_bytes = 3200;

    /* 全0静音buffer（BSS段自动清零，避免每次malloc） */
    static unsigned char silent_pcm[3200];
    if (pcm_bytes > sizeof(silent_pcm)) pcm_bytes = sizeof(silent_pcm);

    char* b64 = pcm_to_base64(silent_pcm, pcm_bytes);
    if (!b64) {
        return -ENOMEM;
    }

    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "input_audio_buffer.append");
    char evt_id[64];
    snprintf(evt_id, sizeof(evt_id), "evt_silent_%u", s->seq++);
    cJSON_AddStringToObject(root, "event_id", evt_id);
    cJSON_AddStringToObject(root, "audio", b64);

    char* json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    free(b64);

    if (!json) {
        return -ENOMEM;
    }

    int ret = ws_send_text(&s->ctx, json, strlen(json));
    free(json);
    if (ret == 0) {
        syslog(LOG_INFO, "[%s] stream: silent PCM sent (%zu ms)\n", TAG, ms);
    }
    return ret;
}

/* ── 图片AI分析：渐进式图片发送 ──────────────────────────────────
 * 构建单个 input_image_buffer.append WS帧，但payload分块写入TLS：
 *   1. 发送WS帧头（含总长度）
 *   2. 发送JSON前缀（含mask）
 *   3. 分块读取图片→Base64编码→mask→写入TLS（每块768字节输入/1024字节输出）
 *   4. 发送JSON后缀（含mask）
 * 内存占用固定约2KB，可处理任意大小图片。 */
int dashscope_asr_stream_send_image(ds_asr_stream_t* s,
    const unsigned char* img_data, size_t img_len)
{
    if (!s || !s->connected) {
        syslog(LOG_ERR, "[AI_IMG] send_image failed: not connected\n");
        return -ENOTCONN;
    }
    if (!img_data || img_len == 0) {
        syslog(LOG_ERR, "[AI_IMG] send_image failed: invalid param (data=%p len=%zu)\n", img_data, img_len);
        return -EINVAL;
    }

    syslog(LOG_INFO, "[AI_IMG] ===== send_image start: %zu bytes =====\n", img_len);

    /* 1. 计算Base64长度 */
    size_t b64_len = (img_len + 2) / 3 * 4;

    /* 2. 构建JSON前缀和后缀 */
    char prefix[128];
    char evt_id[64];
    snprintf(evt_id, sizeof(evt_id), "evt_img_%u", s->seq++);
    int prefix_len = snprintf(prefix, sizeof(prefix),
        "{\"type\":\"input_image_buffer.append\",\"event_id\":\"%s\",\"image\":\"", evt_id);
    if (prefix_len <= 0 || prefix_len >= (int)sizeof(prefix)) {
        return -ENOMEM;
    }
    static const char suffix[] = "\"}";
    size_t suffix_len = 2;

    size_t total_payload = (size_t)prefix_len + b64_len + suffix_len;

    /* 3. 构建WS帧头 */
    unsigned char hdr[14];
    size_t hdr_len = 0;
    hdr[0] = WS_FIN_BIT | WS_OPCODE_TEXT;
    if (total_payload < 126) {
        hdr[1] = WS_MASK_BIT | (unsigned char)total_payload;
        hdr_len = 2;
    } else if (total_payload <= 0xFFFF) {
        hdr[1] = WS_MASK_BIT | 126;
        hdr[2] = (unsigned char)(total_payload >> 8);
        hdr[3] = (unsigned char)(total_payload & 0xFF);
        hdr_len = 4;
    } else {
        hdr[1] = WS_MASK_BIT | 127;
        memset(hdr + 2, 0, 4);
        hdr[6] = (unsigned char)((total_payload >> 24) & 0xFF);
        hdr[7] = (unsigned char)((total_payload >> 16) & 0xFF);
        hdr[8] = (unsigned char)((total_payload >> 8) & 0xFF);
        hdr[9] = (unsigned char)(total_payload & 0xFF);
        hdr_len = 10;
    }

    unsigned char mask[WS_MASK_KEY_LEN];
    ds_asr_entropy(NULL, mask, WS_MASK_KEY_LEN);
    memcpy(hdr + hdr_len, mask, WS_MASK_KEY_LEN);
    hdr_len += WS_MASK_KEY_LEN;

    syslog(LOG_INFO, "[AI_IMG] WS frame: total_payload=%zu, hdr_len=%zu\n", total_payload, hdr_len);

    int ret = tls_write_all(&s->ctx, hdr, hdr_len);
    if (ret != 0) {
        syslog(LOG_ERR, "[AI_IMG] send WS header failed (ret=%d)\n", ret);
        return ret;
    }

    /* 4. 发送JSON前缀（含mask） */
    size_t mask_off = 0;
    unsigned char masked_prefix[128];
    for (int i = 0; i < prefix_len; i++) {
        masked_prefix[i] = (unsigned char)prefix[i] ^ mask[mask_off++ % 4];
    }
    ret = tls_write_all(&s->ctx, masked_prefix, (size_t)prefix_len);
    if (ret != 0) {
        syslog(LOG_ERR, "[AI_IMG] send JSON prefix failed (ret=%d)\n", ret);
        return ret;
    }
    syslog(LOG_INFO, "[AI_IMG] JSON prefix sent (%d bytes), start sending base64 chunks...\n", prefix_len);

    /* 5. 分块Base64编码并发送 */
#define IMG_B64_CHUNK_IN  768   /* 768*4/3 = 1024 base64输出 */
#define IMG_B64_CHUNK_OUT 1080  /* 留余量 */
    unsigned char b64_out[IMG_B64_CHUNK_OUT];
    size_t in_off = 0;
    while (in_off < img_len) {
        size_t chunk = img_len - in_off;
        if (chunk > IMG_B64_CHUNK_IN) chunk = IMG_B64_CHUNK_IN;
        /* 中间块必须是3的倍数，避免Base64添加padding；
         * 仅最后一块允许非3对齐 */
        if (in_off + chunk < img_len && (chunk % 3 != 0)) {
            chunk -= (chunk % 3);
        }
        size_t out_len = 0;
        int bret = mbedtls_base64_encode(b64_out, sizeof(b64_out),
            &out_len, img_data + in_off, chunk);
        if (bret != 0 || out_len == 0) {
            syslog(LOG_ERR, "[AI_IMG] base64 encode failed at off %zu (bret=%d)\n", in_off, bret);
            return -EIO;
        }
        for (size_t i = 0; i < out_len; i++) {
            b64_out[i] ^= mask[mask_off++ % 4];
        }
        ret = tls_write_all(&s->ctx, b64_out, out_len);
        if (ret != 0) {
            syslog(LOG_ERR, "[AI_IMG] send chunk failed at off %zu (ret=%d)\n", in_off, ret);
            return ret;
        }
        in_off += chunk;
    }
    syslog(LOG_INFO, "[AI_IMG] base64 chunks sent, sending suffix...\n");

    /* 6. 发送JSON后缀（含mask） */
    unsigned char masked_suffix[2];
    for (size_t i = 0; i < suffix_len; i++) {
        masked_suffix[i] = (unsigned char)suffix[i] ^ mask[mask_off++ % 4];
    }
    ret = tls_write_all(&s->ctx, masked_suffix, suffix_len);
    if (ret != 0) {
        syslog(LOG_ERR, "[AI_IMG] send JSON suffix failed (ret=%d)\n", ret);
        return ret;
    }

    syslog(LOG_INFO, "[AI_IMG] ===== send_image DONE: raw=%zu bytes, b64=%zu bytes, total=%zu =====\n",
        img_len, b64_len, total_payload);
    return 0;
}

/* ── 图片AI分析：禁用VAD，切换到Manual模式 ──────────────────
 * Manual模式下客户端显式发送commit和response.create，
 * 适用于按下即说/图片上传场景 */
int dashscope_asr_stream_disable_vad(ds_asr_stream_t* s)
{
    if (!s || !s->connected) {
        syslog(LOG_ERR, "[AI_IMG] disable_vad failed: not connected\n");
        return -ENOTCONN;
    }

    char json[256];
    int jlen = snprintf(json, sizeof(json),
        "{\"type\":\"session.update\","
        "\"event_id\":\"evt_dis_vad_%u\","
        "\"session\":{\"turn_detection\":null}}", s->seq++);
    if (jlen <= 0 || jlen >= (int)sizeof(json)) {
        return -ENOMEM;
    }

    syslog(LOG_INFO, "[AI_IMG] sending session.update (disable VAD)...\n");
    int ret = ws_send_text(&s->ctx, json, (size_t)jlen);
    if (ret != 0) {
        syslog(LOG_ERR, "[AI_IMG] disable_vad send failed (ret=%d)\n", ret);
        return ret;
    }

    /* 等待 session.updated 确认 */
    char* rbuf = malloc(WS_BUF_SIZE * 2);
    if (!rbuf) return -ENOMEM;
    int n = ws_recv_text(&s->ctx, rbuf, WS_BUF_SIZE * 2);
    if (n > 0) {
        rbuf[n < WS_BUF_SIZE * 2 - 1 ? n : WS_BUF_SIZE * 2 - 1] = '\0';
        cJSON* root = cJSON_Parse(rbuf);
        if (root) {
            cJSON* t = cJSON_GetObjectItem(root, "type");
            if (cJSON_IsString(t) && strcmp(t->valuestring, "error") == 0) {
                cJSON* err = cJSON_GetObjectItem(root, "error");
                cJSON* msg = err ? cJSON_GetObjectItem(err, "message") : NULL;
                syslog(LOG_ERR, "[AI_IMG] disable_vad error: %s\n",
                    (msg && cJSON_IsString(msg)) ? msg->valuestring : rbuf);
                cJSON_Delete(root); free(rbuf);
                return -EPROTO;
            }
            cJSON_Delete(root);
        }
    }
    free(rbuf);
    syslog(LOG_INFO, "[AI_IMG] ===== VAD disabled (Manual mode) =====\n");
    return 0;
}

/* ── 图片AI分析：恢复VAD模式（分析完成后调用） ────────────────── */
int dashscope_asr_stream_enable_vad(ds_asr_stream_t* s)
{
    if (!s || !s->connected) {
        syslog(LOG_ERR, "[AI_IMG] enable_vad failed: not connected\n");
        return -ENOTCONN;
    }

    char json[512];
    int jlen = snprintf(json, sizeof(json),
        "{\"type\":\"session.update\","
        "\"event_id\":\"evt_en_vad_%u\","
        "\"session\":{\"turn_detection\":{"
        "\"type\":\"server_vad\","
        "\"threshold\":0.5,"
        "\"silence_duration_ms\":800}}}", s->seq++);
    if (jlen <= 0 || jlen >= (int)sizeof(json)) {
        return -ENOMEM;
    }

    syslog(LOG_INFO, "[AI_IMG] sending session.update (enable VAD)...\n");
    int ret = ws_send_text(&s->ctx, json, (size_t)jlen);
    if (ret != 0) {
        syslog(LOG_ERR, "[AI_IMG] enable_vad send failed (ret=%d)\n", ret);
        return ret;
    }

    char* rbuf = malloc(WS_BUF_SIZE * 2);
    if (!rbuf) return -ENOMEM;
    int n = ws_recv_text(&s->ctx, rbuf, WS_BUF_SIZE * 2);
    if (n > 0) {
        rbuf[n < WS_BUF_SIZE * 2 - 1 ? n : WS_BUF_SIZE * 2 - 1] = '\0';
        cJSON* root = cJSON_Parse(rbuf);
        if (root) {
            cJSON* t = cJSON_GetObjectItem(root, "type");
            if (cJSON_IsString(t) && strcmp(t->valuestring, "error") == 0) {
                cJSON* err = cJSON_GetObjectItem(root, "error");
                cJSON* msg = err ? cJSON_GetObjectItem(err, "message") : NULL;
                syslog(LOG_ERR, "[AI_IMG] enable_vad error: %s\n",
                    (msg && cJSON_IsString(msg)) ? msg->valuestring : rbuf);
                cJSON_Delete(root); free(rbuf);
                return -EPROTO;
            }
            cJSON_Delete(root);
        }
    }
    free(rbuf);
    syslog(LOG_INFO, "[AI_IMG] ===== VAD enabled (server_vad mode) =====\n");
    return 0;
}

/* ── 图片AI分析：提交音频+图片缓冲区（Manual模式） ──────────────────
 * input_audio_buffer.commit 同时提交音频缓冲区和图像缓冲区，
 * 告知服务端本轮用户输入已全部发送完毕。
 * 服务端会回复 input_audio_buffer.committed 事件。
 * 注意：不存在 input_image_buffer.commit 事件！ */
int dashscope_asr_stream_commit_buffer(ds_asr_stream_t* s)
{
    if (!s || !s->connected) {
        syslog(LOG_ERR, "[AI_IMG] commit_buffer failed: not connected\n");
        return -ENOTCONN;
    }

    char json[128];
    int jlen = snprintf(json, sizeof(json),
        "{\"type\":\"input_audio_buffer.commit\","
        "\"event_id\":\"evt_buf_commit_%u\"}", s->seq++);
    if (jlen <= 0 || jlen >= (int)sizeof(json)) {
        return -ENOMEM;
    }

    syslog(LOG_INFO, "[AI_IMG] sending input_audio_buffer.commit...\n");
    int ret = ws_send_text(&s->ctx, json, (size_t)jlen);
    if (ret == 0) {
        syslog(LOG_INFO, "[AI_IMG] ===== input_audio_buffer.commit sent OK, waiting for committed event =====\n");
    } else {
        syslog(LOG_ERR, "[AI_IMG] input_audio_buffer.commit send failed (ret=%d)\n", ret);
    }
    return ret;
}

void dashscope_asr_set_image_reply_lang(const char *lang)
{
    if (!lang || lang[0] == '\0') return;
    g_img_reply_lang = lang;
    syslog(LOG_INFO, "[AI_IMG] image reply language set to: %s\n", g_img_reply_lang);
}

/* ── 图片AI分析：发送response.create触发模型响应 ──────────────────
 * 在 response.create 中携带图片分析专用 instructions（响应级覆盖），
 * 不修改 session 默认 instructions，分析完成后无需恢复。
 * 指令用英文保证模型遵循度稳定，通过 Reply in {LANG} 控制输出语言。 */
int dashscope_asr_stream_commit(ds_asr_stream_t* s)
{
    if (!s || !s->connected) {
        syslog(LOG_ERR, "[AI_IMG] commit failed: not connected (s=%p connected=%d)\n",
               s, s ? s->connected : 0);
        return -ENOTCONN;
    }

    char commit[384];
    int clen = snprintf(commit, sizeof(commit),
        "{\"type\":\"response.create\",\"event_id\":\"evt_img_resp_%u\","
        "\"response\":{\"modalities\":[\"text\",\"audio\"],"
        "\"instructions\":\"Describe the image in one sentence, then identify its main subject (object/person/text/scene) and add 2-3 sentences of detail. Reply in %s. No lists or symbols. Keep it spoken-friendly.\"}}",
        s->seq++, g_img_reply_lang);
    if (clen <= 0 || clen >= (int)sizeof(commit)) {
        syslog(LOG_ERR, "[AI_IMG] commit snprintf failed (clen=%d)\n", clen);
        return -ENOMEM;
    }

    syslog(LOG_INFO, "[AI_IMG] sending response.create: %s\n", commit);
    int ret = ws_send_text(&s->ctx, commit, (size_t)clen);
    if (ret == 0) {
        syslog(LOG_INFO, "[AI_IMG] ===== response.create sent OK, waiting for server response =====\n");
    } else {
        syslog(LOG_ERR, "[AI_IMG] response.create send failed (ret=%d)\n", ret);
    }
    return ret;
}

/* ── 飞书对话模式：ASR-only / Omni 模式切换 ──────────────── */

int dashscope_asr_stream_set_asr_only(ds_asr_stream_t* s)
{
    if (!s) return -EINVAL;

    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "session.update");
    cJSON_AddStringToObject(root, "event_id", "evt_asr_only");
    cJSON* session = cJSON_AddObjectToObject(root, "session");

    /* 仅输出文本（不生成音频），云端不做 LLM 回复 */
    cJSON* modalities = cJSON_AddArrayToObject(session, "modalities");
    cJSON_AddItemToArray(modalities, cJSON_CreateString("text"));

    /* 清空 instructions，避免触发 LLM */
    cJSON_AddStringToObject(session, "instructions", "");

    /* 保留 server_vad 检测用户说话结束，自动提交音频缓冲区触发转写
     * modalities=["text"] 确保云端只生成转写文本，不生成音频回复 */
    cJSON* td = cJSON_AddObjectToObject(session, "turn_detection");
    cJSON_AddStringToObject(td, "type", "server_vad");

    /* 保留输入转录，确保用户语音转写正常工作 */
    cJSON_AddBoolToObject(session, "enable_input_audio_transcription", 1);

    char* json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) return -ENOMEM;

    syslog(LOG_INFO, "[%s] Switching to ASR-only mode\n", TAG);
    int ret = ws_send_text(&s->ctx, json, strlen(json));
    free(json);
    if (ret != 0) {
        syslog(LOG_ERR, "[%s] ASR-only session.update failed (ret=%d)\n", TAG, ret);
        return ret;
    }
    syslog(LOG_INFO, "[%s] ASR-only mode active\n", TAG);
    return 0;
}

int dashscope_asr_stream_restore_omni(ds_asr_stream_t* s)
{
    if (!s) return -EINVAL;

    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "session.update");
    cJSON_AddStringToObject(root, "event_id", "evt_restore_omni");
    cJSON* session = cJSON_AddObjectToObject(root, "session");
    cJSON_AddStringToObject(session, "input_audio_format", "pcm");

    /* 恢复多模态输出：文本 + 音频 */
    cJSON* modalities = cJSON_AddArrayToObject(session, "modalities");
    cJSON_AddItemToArray(modalities, cJSON_CreateString("text"));
    cJSON_AddItemToArray(modalities, cJSON_CreateString("audio"));

    cJSON_AddStringToObject(session, "voice", AGENT_DASHSCOPE_OMNI_VOICE);
    cJSON_AddStringToObject(session, "output_audio_format", "pcm");
    ds_add_session_instructions(session);

    cJSON_AddBoolToObject(session, "enable_input_audio_transcription", 1);
    cJSON* td = cJSON_AddObjectToObject(session, "turn_detection");
    cJSON_AddStringToObject(td, "type", "server_vad");
    cJSON_AddNumberToObject(td, "threshold", 0.5);
    cJSON_AddNumberToObject(td, "silence_duration_ms", 800);

    char* json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) return -ENOMEM;

    syslog(LOG_INFO, "[%s] Restoring Omni multimodal mode\n", TAG);
    int ret = ws_send_text(&s->ctx, json, strlen(json));
    free(json);
    if (ret != 0) {
        syslog(LOG_ERR, "[%s] Omni restore session.update failed (ret=%d)\n", TAG, ret);
        return ret;
    }
    syslog(LOG_INFO, "[%s] Omni mode restored\n", TAG);
    return 0;
}

/* 运行时刷新会话指令（语言切换场景）。
 * 与 restore_omni 不同：不改 modalities/voice/turn_detection，只重发 instructions。
 * 复用 ds_add_session_instructions，用当前 g_img_reply_lang 构建。 */
int dashscope_asr_stream_update_session_instructions(ds_asr_stream_t* s)
{
    if (!s) return -EINVAL;

    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "session.update");
    cJSON_AddStringToObject(root, "event_id", "evt_update_instructions");
    cJSON* session = cJSON_AddObjectToObject(root, "session");
    ds_add_session_instructions(session);

    char* json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) return -ENOMEM;

    syslog(LOG_INFO, "[%s] Updating session instructions (lang refresh)\n", TAG);
    int ret = ws_send_text(&s->ctx, json, strlen(json));
    free(json);
    if (ret != 0) {
        syslog(LOG_ERR, "[%s] update session instructions failed (ret=%d)\n", TAG, ret);
        return ret;
    }
    return 0;
}
