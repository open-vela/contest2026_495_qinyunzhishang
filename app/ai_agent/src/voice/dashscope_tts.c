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
 * DashScope Qwen3-TTS-Flash-Realtime via WebSocket (Realtime API).
 *
 * Protocol: wss://dashscope.aliyuncs.com/api-ws/v1/realtime?model=qwen3-tts-flash-realtime
 * Auth: DashScope API Key in HTTP header (Bearer token)
 * Flow:
 *   1. TLS connect + HTTP Upgrade (with Authorization: Bearer <api_key>)
 *   2. Receive session.created
 *   3. Send session.update (voice, response_format, mode)
 *   4. Receive session.updated
 *   5. Send input_text_buffer.append (text to synthesize)
 *   6. Receive response.created, response.audio.delta (base64 PCM), response.done
 *   7. Send session.finish
 *   8. Receive session.finished
 *
 * Audio output: PCM 24kHz mono 16-bit signed LE
 */

#include "voice/dashscope_tts.h"
#include "voice/ws_conn_pool.h"
#include "voice/tts_cache.h"
#include "infra/config_store.h"
#include "infra/http_proxy.h"
#include "agent_compat.h"
#include "agent_config.h"
#include "voice/voice_tts.h"

#include "cJSON.h"
#include "mbedtls/base64.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/ssl.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>

static const char* TAG = "ds_tts";

#define WS_BUF_SIZE 4096
#define WS_MASK_KEY_LEN 4
#define WS_OPCODE_TEXT 0x01
#define WS_OPCODE_BINARY 0x02
#define WS_OPCODE_CLOSE 0x08
#define WS_FIN_BIT 0x80
#define WS_MASK_BIT 0x80

/* ── TLS context and helpers (same pattern as dashscope_asr.c) ── */

typedef struct {
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config cfg;
    mbedtls_net_context net;
    mbedtls_ctr_drbg_context ctr_drbg;
    unsigned char pending[WS_BUF_SIZE];
    size_t pending_len;
} ds_tts_tls_t;

static int ds_tts_entropy(void* data, unsigned char* output, size_t len)
{
    (void)data;
    if (agent_secure_random(output, len) == 0)
        return 0;
    syslog(LOG_ERR, "[%s] No entropy source\n", TAG);
    return -1;
}

static int ds_tts_tls_connect(ds_tts_tls_t* ctx,
    const char* host, const char* port)
{
    int ret;

    mbedtls_ssl_init(&ctx->ssl);
    mbedtls_ssl_config_init(&ctx->cfg);
    mbedtls_net_init(&ctx->net);
    mbedtls_ctr_drbg_init(&ctx->ctr_drbg);
    ctx->pending_len = 0;

    ret = mbedtls_ctr_drbg_seed(&ctx->ctr_drbg, ds_tts_entropy,
        NULL, (const unsigned char*)"ds_tts", 6);
    if (ret != 0)
        return -EIO;

    if (http_proxy_is_enabled()) {
        int port_num = atoi(port);
        int tunnel_fd = proxy_open_tunnel(host, port_num, 30000);
        if (tunnel_fd < 0)
            return -ECONNREFUSED;
        ctx->net.fd = tunnel_fd;
    } else {
        ret = mbedtls_net_connect(&ctx->net, host, port,
            MBEDTLS_NET_PROTO_TCP);
        if (ret != 0) {
            syslog(LOG_ERR, "[%s] TCP connect failed: -0x%04x\n", TAG, -ret);
            return -ECONNREFUSED;
        }
    }
    syslog(LOG_INFO, "[%s] TCP connected (fd=%d)\n", TAG, ctx->net.fd);

    mbedtls_net_set_block(&ctx->net);
    if (ctx->net.fd >= 0) {
        struct timeval tv = { .tv_sec = 30, .tv_usec = 0 };
        setsockopt(ctx->net.fd, SOL_SOCKET, SO_RCVTIMEO,
            &tv, sizeof(tv));
        setsockopt(ctx->net.fd, SOL_SOCKET, SO_SNDTIMEO,
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

    syslog(LOG_INFO, "[%s] starting TLS handshake...\n", TAG);
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

static void ds_tts_tls_free(ds_tts_tls_t* ctx)
{
    mbedtls_ssl_close_notify(&ctx->ssl);
    mbedtls_net_free(&ctx->net);
    mbedtls_ssl_free(&ctx->ssl);
    mbedtls_ssl_config_free(&ctx->cfg);
    mbedtls_ctr_drbg_free(&ctx->ctr_drbg);
}

static int ds_tts_write_all(ds_tts_tls_t* ctx,
    const unsigned char* buf, size_t len)
{
    size_t written = 0;
    while (written < len) {
        int ret = mbedtls_ssl_write(&ctx->ssl, buf + written,
            len - written);
        if (ret > 0)
            written += (size_t)ret;
        else if (ret != MBEDTLS_ERR_SSL_WANT_WRITE)
            return -EIO;
    }
    return 0;
}

static int ds_tts_read_all(ds_tts_tls_t* ctx,
    unsigned char* buf, size_t len)
{
    size_t got = 0;

    if (ctx->pending_len > 0) {
        size_t copy = (ctx->pending_len < len) ? ctx->pending_len : len;
        memcpy(buf, ctx->pending, copy);
        if (ctx->pending_len > copy) {
            memmove(ctx->pending, ctx->pending + copy,
                ctx->pending_len - copy);
        }
        ctx->pending_len -= copy;
        got = copy;
    }

    while (got < len) {
        int ret = mbedtls_ssl_read(&ctx->ssl, buf + got, len - got);
        if (ret > 0) {
            got += (size_t)ret;
        } else if (ret == 0
            || ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
            return -ECONNRESET;
        } else if (ret != MBEDTLS_ERR_SSL_WANT_READ) {
            syslog(LOG_ERR, "[%s] ssl_read: -0x%04x\n", TAG, -ret);
            return -EIO;
        }
    }
    return 0;
}

/* ── WebSocket upgrade ───────────────────────────────────────── */

static int ds_tts_ws_upgrade(ds_tts_tls_t* ctx,
    const char* host, const char* path,
    const char* api_key)
{
    unsigned char key_raw[16];
    unsigned char key_b64[32];
    size_t key_b64_len;

    ds_tts_entropy(NULL, key_raw, sizeof(key_raw));
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

    int ret = ds_tts_write_all(ctx, (const unsigned char*)req, (size_t)n);
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
    syslog(LOG_INFO, "[%s] TTS WebSocket upgrade OK\n", TAG);
    return 0;
}

/* ── WebSocket frame send (text, masked) ─────────────────────── */

static int ds_tts_ws_send_text(ds_tts_tls_t* ctx,
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
    ds_tts_entropy(NULL, mask, WS_MASK_KEY_LEN);
    memcpy(hdr + hdr_len, mask, WS_MASK_KEY_LEN);
    hdr_len += WS_MASK_KEY_LEN;

    int ret = ds_tts_write_all(ctx, hdr, hdr_len);
    if (ret != 0)
        return ret;

    unsigned char chunk[1024];
    size_t sent = 0;
    while (sent < plen) {
        size_t clen = plen - sent;
        if (clen > sizeof(chunk))
            clen = sizeof(chunk);
        for (size_t i = 0; i < clen; i++)
            chunk[i] = ((const unsigned char*)payload)[sent + i]
                ^ mask[(sent + i) % 4];
        ret = ds_tts_write_all(ctx, chunk, clen);
        if (ret != 0)
            return ret;
        sent += clen;
    }
    return 0;
}

/* ── WebSocket frame recv (text or binary, unmasked) ─────────── */

typedef struct {
    int opcode;
    size_t plen;
} ws_frame_hdr_t;

static int ds_tts_ws_recv_frame_hdr(ds_tts_tls_t* ctx,
    ws_frame_hdr_t* out)
{
    unsigned char hdr[2];
    int ret = ds_tts_read_all(ctx, hdr, 2);
    if (ret != 0)
        return ret;

    out->opcode = hdr[0] & 0x0F;
    out->plen = hdr[1] & 0x7F;

    if (out->opcode == WS_OPCODE_CLOSE)
        return 0;

    if (out->opcode == 0x09) {
        if (out->plen == 126) {
            unsigned char ext[2];
            ds_tts_read_all(ctx, ext, 2);
            out->plen = ((size_t)ext[0] << 8) | ext[1];
        }
        unsigned char pp[128];
        if (out->plen > 0 && out->plen <= 125)
            ds_tts_read_all(ctx, pp, out->plen);
        unsigned char pong[2] = { 0x8A, (unsigned char)out->plen };
        ds_tts_write_all(ctx, pong, 2);
        if (out->plen > 0 && out->plen <= 125)
            ds_tts_write_all(ctx, pp, out->plen);
        out->opcode = 0x09;
        return 0;
    }

    if (out->plen == 126) {
        unsigned char ext[2];
        ret = ds_tts_read_all(ctx, ext, 2);
        if (ret != 0)
            return -EIO;
        out->plen = ((size_t)ext[0] << 8) | ext[1];
    } else if (out->plen == 127) {
        unsigned char ext[8];
        ret = ds_tts_read_all(ctx, ext, 8);
        if (ret != 0)
            return -EIO;
        out->plen = ((size_t)ext[4] << 24) | ((size_t)ext[5] << 16)
            | ((size_t)ext[6] << 8) | ext[7];
    }

    return 0;
}

static int ds_tts_ws_recv_text(ds_tts_tls_t* ctx,
    char* buf, size_t cap)
{
    ws_frame_hdr_t fh;
    int ret = ds_tts_ws_recv_frame_hdr(ctx, &fh);
    if (ret != 0)
        return ret;

    if (fh.opcode == WS_OPCODE_CLOSE) {
        /* Read close frame payload to get close code and reason */
        if (fh.plen >= 2) {
            unsigned char code_buf[2];
            if (ds_tts_read_all(ctx, code_buf, 2) == 0) {
                int close_code = (code_buf[0] << 8) | code_buf[1];
                char reason[128] = "";
                size_t reason_len = fh.plen - 2;
                if (reason_len > 0 && reason_len < sizeof(reason)) {
                    ds_tts_read_all(ctx, (unsigned char*)reason, reason_len);
                    reason[reason_len] = '\0';
                }
                syslog(LOG_WARNING, "[%s] ws close: code=%d reason=%s\n",
                    TAG, close_code, reason);
            }
        } else if (fh.plen > 0) {
            unsigned char skip[256];
            ds_tts_read_all(ctx, skip, fh.plen);
        }
        return 0;
    }

    if (fh.opcode == 0x09)
        return -2;

    if (fh.opcode != WS_OPCODE_TEXT) {
        unsigned char skip[256];
        size_t left = fh.plen;
        while (left > 0) {
            size_t n = (left < sizeof(skip)) ? left : sizeof(skip);
            ds_tts_read_all(ctx, skip, n);
            left -= n;
        }
        return -2;
    }

    if (fh.plen >= cap)
        return -EOVERFLOW;

    ret = ds_tts_read_all(ctx, (unsigned char*)buf, fh.plen);
    if (ret != 0)
        return ret;
    buf[fh.plen] = '\0';
    syslog(LOG_INFO, "[%s] ws_recv text: plen=%zu\n", TAG, fh.plen);
    return (int)fh.plen;
}

static int ds_tts_ws_recv_binary(ds_tts_tls_t* ctx,
    unsigned char* buf, size_t cap, size_t* out_len)
{
    ws_frame_hdr_t fh;
    int ret = ds_tts_ws_recv_frame_hdr(ctx, &fh);
    if (ret != 0)
        return ret;

    if (fh.opcode == WS_OPCODE_CLOSE) {
        *out_len = 0;
        return 0;
    }

    if (fh.opcode == 0x09) {
        *out_len = 0;
        return -2;
    }

    if (fh.opcode == WS_OPCODE_TEXT) {
        if (fh.plen >= cap)
            return -EOVERFLOW;
        ret = ds_tts_read_all(ctx, buf, fh.plen);
        if (ret != 0)
            return ret;
        buf[fh.plen] = '\0';
        *out_len = fh.plen;
        return 1;
    }

    if (fh.opcode != WS_OPCODE_BINARY) {
        unsigned char skip[256];
        size_t left = fh.plen;
        while (left > 0) {
            size_t n = (left < sizeof(skip)) ? left : sizeof(skip);
            ds_tts_read_all(ctx, skip, n);
            left -= n;
        }
        *out_len = 0;
        return -2;
    }

    if (fh.plen > cap)
        return -EOVERFLOW;

    ret = ds_tts_read_all(ctx, buf, fh.plen);
    if (ret != 0)
        return ret;
    *out_len = fh.plen;
    return 0;
}

/* ── Credentials ─────────────────────────────────────────────── */

static char s_api_key[128];
static char s_model[64];
static char s_voice[64];

static int dashscope_tts_init(void)
{
    memset(s_api_key, 0, sizeof(s_api_key));
    memset(s_model, 0, sizeof(s_model));
    memset(s_voice, 0, sizeof(s_voice));

    claw_config_get(AGENT_CFG_KEY_DASHSCOPE_API_KEY,
        s_api_key, sizeof(s_api_key));

    /* Try LLM api_key as fallback */
    if (s_api_key[0] == '\0')
        claw_config_get(AGENT_CFG_KEY_API_KEY,
            s_api_key, sizeof(s_api_key));

    /* 优先从 /emmc/wifi/agent_app_id 文件读取（最高优先级） */
    {
        char file_key[128] = { 0 };
        if (claw_load_file_string("/emmc/wifi/agent_app_id",
                                  file_key, sizeof(file_key)) == OK) {
            strncpy(s_api_key, file_key, sizeof(s_api_key) - 1);
            s_api_key[sizeof(s_api_key) - 1] = '\0';
        }
    }

    if (claw_config_get(AGENT_CFG_KEY_DASHSCOPE_TTS_MODEL,
            s_model, sizeof(s_model)) != OK
        || s_model[0] == '\0')
        strncpy(s_model, AGENT_DASHSCOPE_TTS_MODEL,
            sizeof(s_model) - 1);

    /* 优先从 /emmc/wifi/agent_tts_model 文件读取，覆盖默认宏 */
    {
        char file_model[128] = { 0 };
        if (claw_load_file_string("/emmc/wifi/agent_tts_model",
                                  file_model, sizeof(file_model)) == OK) {
            strncpy(s_model, file_model, sizeof(s_model) - 1);
            s_model[sizeof(s_model) - 1] = '\0';
            syslog(LOG_INFO, "[%s] TTS model loaded from /emmc/wifi/agent_tts_model: %s\n", TAG, s_model);
        }
    }

    if (claw_config_get(AGENT_CFG_KEY_DASHSCOPE_TTS_VOICE,
            s_voice, sizeof(s_voice)) != OK
        || s_voice[0] == '\0')
        strncpy(s_voice, AGENT_DASHSCOPE_TTS_VOICE,
            sizeof(s_voice) - 1);

    return 0;
}

/* 运行时切换 TTS 音色。普通 TTS 每次合成都新建 WS 连接，
 * 更新 s_voice 后下次合成自动使用新音色，无需刷新在线会话。
 * 同时同步更新 ws_pool 的音色并销毁其现有连接，确保连接池下次合成也用新音色。
 * 清空 TTS 缓存，避免命中旧音色合成的 PCM 数据。
 * 若 voice 与当前相同则跳过，避免重复清缓存。 */
void dashscope_tts_set_voice(const char *voice)
{
    if (!voice || voice[0] == '\0') return;
    if (strcmp(s_voice, voice) == 0) return;  /* 音色未变，跳过 */
    strncpy(s_voice, voice, sizeof(s_voice) - 1);
    s_voice[sizeof(s_voice) - 1] = '\0';
    ws_conn_pool_set_tts_voice(voice);
    tts_cache_cleanup();
    tts_cache_init();
    syslog(LOG_INFO, "[%s] TTS voice set to: %s (cache cleared)\n", TAG, s_voice);
}

/* ── Streaming TTS ───────────────────────────────────────────── */

int dashscope_tts_ws_synthesize_stream(const char* text,
    void (*cb)(const unsigned char*, size_t, int, void*),
    void* user_data)
{
    syslog(LOG_INFO, "[%s] stream start: text=%zu bytes\n", TAG,
        text ? strlen(text) : 0);

    dashscope_tts_init();

    if (s_api_key[0] == '\0') {
        syslog(LOG_ERR, "[%s] DashScope API key not configured\n", TAG);
        return -ENOENT;
    }

    if (!text || !cb)
        return -EINVAL;

    ds_tts_tls_t* ctx = calloc(1, sizeof(ds_tts_tls_t));
    if (!ctx) {
        syslog(LOG_ERR, "[%s] ctx alloc failed\n", TAG);
        return -ENOMEM;
    }
    int ret;

    /* 1. TLS connect */
    syslog(LOG_INFO, "[%s] connecting to %s:%s ...\n", TAG,
        AGENT_DASHSCOPE_TTS_HOST, AGENT_DASHSCOPE_TTS_PORT);
    ret = ds_tts_tls_connect(ctx, AGENT_DASHSCOPE_TTS_HOST,
        AGENT_DASHSCOPE_TTS_PORT);
    if (ret != 0) {
        syslog(LOG_ERR, "[%s] TLS connect failed: %d\n", TAG, ret);
        ds_tts_tls_free(ctx);
        free(ctx);
        return ret;
    }

    /* 2. WS upgrade with model parameter */
    char* ws_path = malloc(256);
    if (!ws_path) {
        ds_tts_tls_free(ctx);
        free(ctx);
        return -ENOMEM;
    }
    snprintf(ws_path, 256, "%s?model=%s",
        AGENT_DASHSCOPE_TTS_WS_PATH, s_model);
    ret = ds_tts_ws_upgrade(ctx, AGENT_DASHSCOPE_TTS_HOST,
        ws_path, s_api_key);
    free(ws_path);
    if (ret != 0) {
        ds_tts_tls_free(ctx);
        free(ctx);
        return ret;
    }

    /* 3. Wait for session.created */
    {
        char* frame_buf = malloc(WS_BUF_SIZE);
        if (!frame_buf) {
            ds_tts_tls_free(ctx);
            free(ctx);
            return -ENOMEM;
        }
        syslog(LOG_INFO, "[%s] waiting for session.created (pending=%zu)...\n",
            TAG, ctx->pending_len);
        int n = ds_tts_ws_recv_text(ctx, frame_buf, WS_BUF_SIZE);
        if (n <= 0) {
            syslog(LOG_ERR, "[%s] No session.created (recv=%d)\n", TAG, n);
            free(frame_buf);
            ds_tts_tls_free(ctx);
            free(ctx);
            return -EPROTO;
        }
        syslog(LOG_INFO, "[%s] Got session event: %.80s\n", TAG, frame_buf);
        free(frame_buf);
    }

    /* 4. Send session.update for TTS */
    {
        cJSON* root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "type", "session.update");
        cJSON_AddStringToObject(root, "event_id", "evt_tts_session");
        cJSON* session = cJSON_AddObjectToObject(root, "session");
        cJSON_AddStringToObject(session, "voice", s_voice);
        cJSON_AddStringToObject(session, "mode", "server_commit");
        cJSON_AddStringToObject(session, "response_format", "pcm");
        cJSON_AddNumberToObject(session, "sample_rate", 24000);

        char* json = cJSON_PrintUnformatted(root);
        cJSON_Delete(root);
        if (!json) {
            ds_tts_tls_free(ctx);
            free(ctx);
            return -ENOMEM;
        }
        syslog(LOG_INFO, "[%s] sending session.update: voice=%s\n",
            TAG, s_voice);
        syslog(LOG_INFO, "[%s] session.update JSON: %s\n", TAG, json);
        ret = ds_tts_ws_send_text(ctx, json, strlen(json));
        free(json);
        if (ret != 0) {
            ds_tts_tls_free(ctx);
            free(ctx);
            return ret;
        }
    }

    /* Wait for session.updated */
    {
        char* frame_buf = malloc(WS_BUF_SIZE);
        if (!frame_buf) {
            ds_tts_tls_free(ctx);
            free(ctx);
            return -ENOMEM;
        }
        int n = ds_tts_ws_recv_text(ctx, frame_buf, WS_BUF_SIZE);
        if (n <= 0) {
            syslog(LOG_ERR, "[%s] No session.updated (recv=%d)\n", TAG, n);
            free(frame_buf);
            ds_tts_tls_free(ctx);
            free(ctx);
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
                ds_tts_tls_free(ctx);
                free(ctx);
                return -EPROTO;
            }
            cJSON_Delete(root);
        }
        free(frame_buf);
        syslog(LOG_INFO, "[%s] session.updated OK\n", TAG);
    }

    /* 5. Send input_text_buffer.append with the text to synthesize */
    {
        cJSON* root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "type", "input_text_buffer.append");
        cJSON_AddStringToObject(root, "event_id", "evt_tts_text");
        cJSON_AddStringToObject(root, "text", text);

        char* json = cJSON_PrintUnformatted(root);
        cJSON_Delete(root);
        if (!json) {
            ds_tts_tls_free(ctx);
            free(ctx);
            return -ENOMEM;
        }
        syslog(LOG_INFO, "[%s] sending input_text_buffer.append (%zu bytes)\n",
            TAG, strlen(text));
        ret = ds_tts_ws_send_text(ctx, json, strlen(json));
        free(json);
        if (ret != 0) {
            syslog(LOG_ERR, "[%s] input_text_buffer.append send failed: %d\n",
                TAG, ret);
            ds_tts_tls_free(ctx);
            free(ctx);
            return ret;
        }
    }

    /* 5b. Send input_text_buffer.commit to signal that text input is
     *    complete.  In server_commit mode the server may wait for more
     *    text before synthesising, which causes it to stop mid-speech.
     *    Commit forces immediate synthesis of the buffered text. */
    {
        cJSON* root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "type", "input_text_buffer.commit");
        cJSON_AddStringToObject(root, "event_id", "evt_tts_commit");

        char* json = cJSON_PrintUnformatted(root);
        cJSON_Delete(root);
        if (json) {
            ret = ds_tts_ws_send_text(ctx, json, strlen(json));
            free(json);
            if (ret != 0) {
                ds_tts_tls_free(ctx);
                free(ctx);
                return ret;
            }
        }
    }

    /* 6. Receive audio deltas and response.done */
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int got_audio = 0;

    size_t frame_cap = 64 * 1024;
    size_t pcm_cap = 32 * 1024;
    char* frame_buf = malloc(frame_cap);
    unsigned char* pcm_buf = malloc(pcm_cap);
    if (!frame_buf || !pcm_buf) {
        free(frame_buf);
        free(pcm_buf);
        ds_tts_tls_free(ctx);
        free(ctx);
        return -ENOMEM;
    }

    while (1) {
        int n = ds_tts_ws_recv_text(ctx, frame_buf, frame_cap);
        if (n == 0) {
            syslog(LOG_INFO, "[%s] ws_recv close frame\n", TAG);
            break;
        }
        if (n < 0) {
            if (n == -2)
                continue;
            syslog(LOG_ERR, "[%s] ws_recv error: %d\n", TAG, n);
            break;
        }

        cJSON* root = cJSON_Parse(frame_buf);
        if (!root) {
            syslog(LOG_WARNING, "[%s] parse failed: %.80s\n", TAG, frame_buf);
            continue;
        }

        cJSON* type_item = cJSON_GetObjectItem(root, "type");
        if (!cJSON_IsString(type_item)) {
            cJSON_Delete(root);
            continue;
        }

        const char* evt_type = type_item->valuestring;

        if (strcmp(evt_type, "response.audio.delta") == 0) {
            cJSON* delta = cJSON_GetObjectItem(root, "delta");
            if (cJSON_IsString(delta) && delta->valuestring[0]) {
                size_t b64_len = strlen(delta->valuestring);
                size_t need = (b64_len / 4) * 3 + 3;
                unsigned char* decode_buf = pcm_buf;
                size_t decode_cap = pcm_cap;

                if (need > decode_cap) {
                    decode_buf = malloc(need);
                    if (!decode_buf) {
                        cJSON_Delete(root);
                        continue;
                    }
                    decode_cap = need;
                }

                size_t pcm_len;
                if (mbedtls_base64_decode(decode_buf, decode_cap,
                        &pcm_len,
                        (const unsigned char*)delta->valuestring,
                        b64_len) == 0
                    && pcm_len > 0) {
                    cb(decode_buf, pcm_len, 0, user_data);
                    got_audio++;
                }

                if (decode_buf != pcm_buf)
                    free(decode_buf);
            }
        } else if (strcmp(evt_type, "response.done") == 0) {
            syslog(LOG_INFO, "[%s] response.done (%d audio chunks)\n",
                TAG, got_audio);
            if (got_audio > 0)
                cb(NULL, 0, 1, user_data);
            cJSON_Delete(root);
            break;
        } else if (strcmp(evt_type, "session.finished") == 0) {
            syslog(LOG_INFO, "[%s] session.finished\n", TAG);
            if (got_audio > 0)
                cb(NULL, 0, 1, user_data);
            cJSON_Delete(root);
            break;
        } else if (strcmp(evt_type, "error") == 0) {
            cJSON* err = cJSON_GetObjectItem(root, "error");
            cJSON* msg = err ? cJSON_GetObjectItem(err, "message")
                             : NULL;
            syslog(LOG_ERR, "[%s] TTS error: %s\n", TAG,
                (msg && cJSON_IsString(msg)) ? msg->valuestring
                                             : "unknown");
            cJSON_Delete(root);
            free(frame_buf);
            free(pcm_buf);
            ds_tts_tls_free(ctx);
            free(ctx);
            return -EIO;
        } else {
            syslog(LOG_INFO, "[%s] ws event: %s\n", TAG, evt_type);
        }

        cJSON_Delete(root);
    }

    free(frame_buf);
    free(pcm_buf);

    ds_tts_tls_free(ctx);
    free(ctx);

    struct timespec t1;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    long ms = (t1.tv_sec - t0.tv_sec) * 1000
        + (t1.tv_nsec - t0.tv_nsec) / 1000000;

    syslog(LOG_INFO, "[%s] TTS complete (%ldms, %d audio chunks)\n",
        TAG, ms, got_audio);
    return (got_audio > 0) ? 0 : -ENODATA;
}

/* ── Ops: synthesize (buffered, for voice_tts_ops_t) ─────────── */

/* Buffered TTS context */
typedef struct {
    unsigned char* buf;
    size_t cap;
    size_t len;
} ds_tts_buf_t;

static void ds_tts_buf_cb(const unsigned char* pcm, size_t pcm_len,
    int is_last, void* user_data)
{
    ds_tts_buf_t* b = (ds_tts_buf_t*)user_data;
    if (!b || !pcm || pcm_len == 0)
        return;
    size_t space = b->cap - b->len;
    size_t copy = (pcm_len < space) ? pcm_len : space;
    if (copy > 0) {
        memcpy(b->buf + b->len, pcm, copy);
        b->len += copy;
    }
}

static int dashscope_tts_synthesize_buf(const char* text,
    unsigned char* pcm_out,
    size_t pcm_cap,
    size_t* pcm_len)
{
    dashscope_tts_init();

    if (s_api_key[0] == '\0') {
        syslog(LOG_ERR, "[%s] DashScope API key not configured\n", TAG);
        return -ENOENT;
    }

    ds_tts_buf_t buf_ctx = {
        .buf = pcm_out,
        .cap = pcm_cap,
        .len = 0,
    };

    int ret = dashscope_tts_ws_synthesize_stream(text,
        ds_tts_buf_cb, &buf_ctx);

    *pcm_len = buf_ctx.len;
    return ret;
}

/* ── Backend ops registration ─────────────────────────────────── */

static const voice_tts_ops_t s_dashscope_tts_ops = {
    .name = "dashscope",
    .init = dashscope_tts_init,
    .synthesize = dashscope_tts_synthesize_buf,
    .deinit = NULL,
};

int dashscope_tts_register(void)
{
    return voice_tts_register(&s_dashscope_tts_ops);
}
