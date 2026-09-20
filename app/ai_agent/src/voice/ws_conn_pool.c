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

#include "voice/ws_conn_pool.h"
#include "voice/dashscope_tts.h"
#include "voice/dashscope_asr.h"
#include "infra/config_store.h"
#include "infra/http_proxy.h"
#include "agent_compat.h"
#include "agent_config.h"

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

static const char* TAG = "ws_pool";

#define WS_BUF_SIZE 4096
#define WS_MASK_KEY_LEN 4
#define WS_OPCODE_TEXT 0x01
#define WS_OPCODE_BINARY 0x02
#define WS_OPCODE_CLOSE 0x08
#define WS_FIN_BIT 0x80
#define WS_MASK_BIT 0x80

typedef struct {
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config cfg;
    mbedtls_net_context net;
    mbedtls_ctr_drbg_context ctr_drbg;
    unsigned char pending[WS_BUF_SIZE];
    size_t pending_len;
} pool_tls_t;

typedef struct {
    pool_tls_t tls;
    int valid;
    time_t last_used;
    int session_ready;
} pool_conn_t;

static struct {
    pool_conn_t tts;
    pool_conn_t asr;
    pthread_mutex_t lock;
    char api_key[128];
    char tts_model[64];
    char tts_voice[64];
    char asr_model[64];
    int initialized;
} s_pool = {
    .lock = PTHREAD_MUTEX_INITIALIZER,
};

static int pool_entropy(void* data, unsigned char* output, size_t len)
{
    (void)data;
    if (agent_secure_random(output, len) == 0)
        return 0;
    return -1;
}

static int pool_tls_connect(pool_tls_t* ctx,
    const char* host, const char* port)
{
    int ret;

    mbedtls_ssl_init(&ctx->ssl);
    mbedtls_ssl_config_init(&ctx->cfg);
    mbedtls_net_init(&ctx->net);
    mbedtls_ctr_drbg_init(&ctx->ctr_drbg);
    ctx->pending_len = 0;

    ret = mbedtls_ctr_drbg_seed(&ctx->ctr_drbg, pool_entropy,
        NULL, (const unsigned char*)"ws_pool", 7);
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
        if (ret != 0)
            return -ECONNREFUSED;
    }

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

    while ((ret = mbedtls_ssl_handshake(&ctx->ssl)) != 0) {
        if (ret != MBEDTLS_ERR_SSL_WANT_READ
            && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
            return -EIO;
        }
    }

    syslog(LOG_INFO, "[%s] TLS connected\n", TAG);
    return 0;
}

static void pool_tls_free(pool_tls_t* ctx)
{
    if (ctx->net.fd < 0)
        return;
    mbedtls_ssl_close_notify(&ctx->ssl);
    mbedtls_net_free(&ctx->net);
    mbedtls_ssl_free(&ctx->ssl);
    mbedtls_ssl_config_free(&ctx->cfg);
    mbedtls_ctr_drbg_free(&ctx->ctr_drbg);
    ctx->pending_len = 0;
}

static int pool_write_all(pool_tls_t* ctx,
    const unsigned char* buf, size_t len)
{
    size_t written = 0;
    int timeouts = 0;
    while (written < len) {
        int ret = mbedtls_ssl_write(&ctx->ssl, buf + written,
            len - written);
        if (ret > 0) {
            written += (size_t)ret;
            timeouts = 0;
        } else if (ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
            /* Socket timeout (SO_SNDTIMEO) fires — retry up to 2 times
             * (≈60s total) before giving up to avoid infinite blocking
             * when the network connection is dead (e.g. WiFi dropped). */
            if (++timeouts >= 2)
                return -ETIMEDOUT;
        } else {
            return -EIO;
        }
    }
    return 0;
}

static int pool_read_all(pool_tls_t* ctx,
    unsigned char* buf, size_t len)
{
    size_t got = 0;
    int timeouts = 0;

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
            timeouts = 0;
        } else if (ret == 0
            || ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
            return -ECONNRESET;
        } else if (ret == MBEDTLS_ERR_SSL_WANT_READ) {
            /* Socket timeout (SO_RCVTIMEO=30s) fired — retry up to 2 times
             * (≈60s total) before giving up. Without this limit, a dead
             * connection (e.g. after WiFi drop) would block forever because
             * mbedtls_ssl_read returns WANT_READ on each timeout and the
             * loop retries indefinitely, holding s_pool.lock and stalling
             * the outbound dispatch thread. */
            if (++timeouts >= 2)
                return -ETIMEDOUT;
        } else {
            return -EIO;
        }
    }
    return 0;
}

static int pool_ws_upgrade(pool_tls_t* ctx,
    const char* host, const char* path,
    const char* api_key)
{
    unsigned char key_raw[16];
    unsigned char key_b64[32];
    size_t key_b64_len;

    pool_entropy(NULL, key_raw, sizeof(key_raw));
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

    int ret = pool_write_all(ctx, (const unsigned char*)req, (size_t)n);
    free(req);
    if (ret != 0)
        return ret;

    char* resp = malloc(WS_BUF_SIZE);
    if (!resp)
        return -ENOMEM;
    size_t rlen = 0;
    int timeouts = 0;
    while (rlen < WS_BUF_SIZE - 1) {
        int r = mbedtls_ssl_read(&ctx->ssl,
            (unsigned char*)resp + rlen,
            WS_BUF_SIZE - 1 - rlen);
        if (r > 0) {
            rlen += (size_t)r;
            resp[rlen] = '\0';
            timeouts = 0;
            if (strstr(resp, "\r\n\r\n"))
                break;
        } else if (r == 0 || r == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
            free(resp);
            return -ECONNRESET;
        } else if (r == MBEDTLS_ERR_SSL_WANT_READ) {
            if (++timeouts >= 2) {
                free(resp);
                return -ETIMEDOUT;
            }
        } else {
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
        }
    }

    free(resp);
    syslog(LOG_INFO, "[%s] WS upgrade OK\n", TAG);
    return 0;
}

static int pool_ws_send_text(pool_tls_t* ctx,
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
    pool_entropy(NULL, mask, WS_MASK_KEY_LEN);
    memcpy(hdr + hdr_len, mask, WS_MASK_KEY_LEN);
    hdr_len += WS_MASK_KEY_LEN;

    int ret = pool_write_all(ctx, hdr, hdr_len);
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
        ret = pool_write_all(ctx, chunk, clen);
        if (ret != 0)
            return ret;
        sent += clen;
    }
    return 0;
}

static int pool_ws_recv_text(pool_tls_t* ctx,
    char* buf, size_t cap)
{
    unsigned char hdr[2];
    int ret = pool_read_all(ctx, hdr, 2);
    if (ret != 0)
        return ret;

    int opcode = hdr[0] & 0x0F;
    size_t plen = hdr[1] & 0x7F;

    if (opcode == WS_OPCODE_CLOSE) {
        /* Read close frame payload for diagnostics */
        if (plen >= 2) {
            unsigned char code_buf[2];
            if (pool_read_all(ctx, code_buf, 2) == 0) {
                int close_code = (code_buf[0] << 8) | code_buf[1];
                char reason[128] = "";
                size_t reason_len = plen - 2;
                if (reason_len > 0 && reason_len < sizeof(reason)) {
                    pool_read_all(ctx, (unsigned char*)reason, reason_len);
                    reason[reason_len] = '\0';
                }
                syslog(LOG_WARNING, "[%s] TTS pool ws close: code=%d reason=%s\n",
                    TAG, close_code, reason);
            }
        } else if (plen > 0) {
            unsigned char skip[256];
            pool_read_all(ctx, skip, plen);
        }
        return 0;
    }

    if (opcode == 0x09) {
        if (plen == 126) {
            unsigned char ext[2];
            pool_read_all(ctx, ext, 2);
            plen = ((size_t)ext[0] << 8) | ext[1];
        }
        unsigned char pp[128];
        if (plen > 0 && plen <= 125)
            pool_read_all(ctx, pp, plen);
        unsigned char pong[2] = { 0x8A, (unsigned char)plen };
        pool_write_all(ctx, pong, 2);
        if (plen > 0 && plen <= 125)
            pool_write_all(ctx, pp, plen);
        return -2;
    }

    if (opcode != WS_OPCODE_TEXT && opcode != WS_OPCODE_BINARY) {
        unsigned char skip[256];
        size_t left = plen;
        while (left > 0) {
            size_t n = (left < sizeof(skip)) ? left : sizeof(skip);
            pool_read_all(ctx, skip, n);
            left -= n;
        }
        return -2;
    }

    if (plen == 126) {
        unsigned char ext[2];
        ret = pool_read_all(ctx, ext, 2);
        if (ret != 0)
            return -EIO;
        plen = ((size_t)ext[0] << 8) | ext[1];
    } else if (plen == 127) {
        unsigned char ext[8];
        ret = pool_read_all(ctx, ext, 8);
        if (ret != 0)
            return -EIO;
        plen = ((size_t)ext[4] << 24) | ((size_t)ext[5] << 16)
            | ((size_t)ext[6] << 8) | ext[7];
    }

    if (plen >= cap)
        return -EOVERFLOW;

    ret = pool_read_all(ctx, (unsigned char*)buf, plen);
    if (ret != 0)
        return ret;
    buf[plen] = '\0';
    return (int)plen;
}

static void pool_conn_destroy(pool_conn_t* conn)
{
    if (!conn || !conn->valid)
        return;
    pool_tls_free(&conn->tls);
    conn->valid = 0;
    conn->session_ready = 0;
}

static int connect_tts_pool(pool_conn_t* conn)
{
    if (conn->valid && conn->session_ready)
        return 0;

    pool_conn_destroy(conn);

    if (s_pool.api_key[0] == '\0')
        return -ENOENT;

    int ret = pool_tls_connect(&conn->tls,
        AGENT_DASHSCOPE_TTS_HOST, AGENT_DASHSCOPE_TTS_PORT);
    if (ret != 0) {
        syslog(LOG_ERR, "[%s] TTS pool: TLS connect failed: %d\n", TAG, ret);
        return ret;
    }

    char* ws_path = malloc(256);
    if (!ws_path) {
        pool_tls_free(&conn->tls);
        return -ENOMEM;
    }
    snprintf(ws_path, 256, "%s?model=%s",
        AGENT_DASHSCOPE_TTS_WS_PATH, s_pool.tts_model);
    ret = pool_ws_upgrade(&conn->tls, AGENT_DASHSCOPE_TTS_HOST,
        ws_path, s_pool.api_key);
    free(ws_path);
    if (ret != 0) {
        pool_tls_free(&conn->tls);
        return ret;
    }

    {
        char* frame_buf = malloc(WS_BUF_SIZE);
        if (!frame_buf) {
            pool_tls_free(&conn->tls);
            return -ENOMEM;
        }
        int n = pool_ws_recv_text(&conn->tls, frame_buf, WS_BUF_SIZE);
        if (n <= 0) {
            syslog(LOG_ERR, "[%s] TTS pool: no session.created\n", TAG);
            free(frame_buf);
            pool_tls_free(&conn->tls);
            return -EPROTO;
        }
        free(frame_buf);
    }

    {
        cJSON* root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "type", "session.update");
        cJSON_AddStringToObject(root, "event_id", "evt_pool_tts");
        cJSON* session = cJSON_AddObjectToObject(root, "session");
        cJSON_AddStringToObject(session, "voice", s_pool.tts_voice);
        cJSON_AddStringToObject(session, "mode", "server_commit");
        cJSON_AddStringToObject(session, "response_format", "pcm");
        cJSON_AddNumberToObject(session, "sample_rate", 24000);

        char* json = cJSON_PrintUnformatted(root);
        cJSON_Delete(root);
        if (!json) {
            pool_tls_free(&conn->tls);
            return -ENOMEM;
        }
        ret = pool_ws_send_text(&conn->tls, json, strlen(json));
        free(json);
        if (ret != 0) {
            pool_tls_free(&conn->tls);
            return ret;
        }
    }

    {
        char* frame_buf = malloc(WS_BUF_SIZE);
        if (!frame_buf) {
            pool_tls_free(&conn->tls);
            return -ENOMEM;
        }
        int n = pool_ws_recv_text(&conn->tls, frame_buf, WS_BUF_SIZE);
        if (n <= 0) {
            syslog(LOG_ERR, "[%s] TTS pool: no session.updated\n", TAG);
            free(frame_buf);
            pool_tls_free(&conn->tls);
            return -EPROTO;
        }
        free(frame_buf);
    }

    conn->valid = 1;
    conn->session_ready = 1;
    conn->last_used = time(NULL);
    syslog(LOG_INFO, "[%s] TTS pool: connected and ready\n", TAG);
    return 0;
}

int ws_conn_pool_init(void)
{
    pthread_mutex_lock(&s_pool.lock);

    memset(s_pool.api_key, 0, sizeof(s_pool.api_key));
    memset(s_pool.tts_model, 0, sizeof(s_pool.tts_model));
    memset(s_pool.tts_voice, 0, sizeof(s_pool.tts_voice));
    memset(s_pool.asr_model, 0, sizeof(s_pool.asr_model));

    claw_config_get(AGENT_CFG_KEY_DASHSCOPE_API_KEY,
        s_pool.api_key, sizeof(s_pool.api_key));
    if (s_pool.api_key[0] == '\0')
        claw_config_get(AGENT_CFG_KEY_API_KEY,
            s_pool.api_key, sizeof(s_pool.api_key));

    /* 优先从 /emmc/wifi/agent_app_id 文件读取（最高优先级） */
    {
        char file_key[128] = { 0 };
        if (claw_load_file_string("/emmc/wifi/agent_app_id",
                                  file_key, sizeof(file_key)) == OK) {
            strncpy(s_pool.api_key, file_key, sizeof(s_pool.api_key) - 1);
            s_pool.api_key[sizeof(s_pool.api_key) - 1] = '\0';
        }
    }

    if (claw_config_get(AGENT_CFG_KEY_DASHSCOPE_TTS_MODEL,
            s_pool.tts_model, sizeof(s_pool.tts_model)) != OK
        || s_pool.tts_model[0] == '\0')
        strncpy(s_pool.tts_model, AGENT_DASHSCOPE_TTS_MODEL,
            sizeof(s_pool.tts_model) - 1);

    /* 优先从 /emmc/wifi/agent_tts_model 文件读取，覆盖默认宏 */
    {
        char file_model[128] = { 0 };
        if (claw_load_file_string("/emmc/wifi/agent_tts_model",
                                  file_model, sizeof(file_model)) == OK) {
            strncpy(s_pool.tts_model, file_model, sizeof(s_pool.tts_model) - 1);
            s_pool.tts_model[sizeof(s_pool.tts_model) - 1] = '\0';
            syslog(LOG_INFO, "[ws_pool] TTS model loaded from /emmc/wifi/agent_tts_model: %s\n", s_pool.tts_model);
        }
    }

    if (claw_config_get(AGENT_CFG_KEY_DASHSCOPE_TTS_VOICE,
            s_pool.tts_voice, sizeof(s_pool.tts_voice)) != OK
        || s_pool.tts_voice[0] == '\0')
        strncpy(s_pool.tts_voice, AGENT_DASHSCOPE_TTS_VOICE,
            sizeof(s_pool.tts_voice) - 1);

    if (claw_config_get(AGENT_CFG_KEY_DASHSCOPE_ASR_MODEL,
            s_pool.asr_model, sizeof(s_pool.asr_model)) != OK
        || s_pool.asr_model[0] == '\0')
        strncpy(s_pool.asr_model, AGENT_DASHSCOPE_ASR_MODEL,
            sizeof(s_pool.asr_model) - 1);

    /* 优先从 /emmc/wifi/agent_asr_model 文件读取，覆盖默认宏 */
    {
        char file_model[128] = { 0 };
        if (claw_load_file_string("/emmc/wifi/agent_asr_model",
                                  file_model, sizeof(file_model)) == OK) {
            strncpy(s_pool.asr_model, file_model, sizeof(s_pool.asr_model) - 1);
            s_pool.asr_model[sizeof(s_pool.asr_model) - 1] = '\0';
            syslog(LOG_INFO, "[ws_pool] ASR model loaded from /emmc/wifi/agent_asr_model: %s\n", s_pool.asr_model);
        }
    }

    memset(&s_pool.tts, 0, sizeof(s_pool.tts));
    memset(&s_pool.asr, 0, sizeof(s_pool.asr));
    s_pool.initialized = 1;

    pthread_mutex_unlock(&s_pool.lock);
    syslog(LOG_INFO, "[%s] initialized\n", TAG);
    return 0;
}

void ws_conn_pool_cleanup(void)
{
    pthread_mutex_lock(&s_pool.lock);
    pool_conn_destroy(&s_pool.tts);
    pool_conn_destroy(&s_pool.asr);
    s_pool.initialized = 0;
    pthread_mutex_unlock(&s_pool.lock);
    syslog(LOG_INFO, "[%s] cleaned up\n", TAG);
}

int ws_conn_pool_preconnect_tts(void)
{
    pthread_mutex_lock(&s_pool.lock);
    if (!s_pool.initialized) {
        pthread_mutex_unlock(&s_pool.lock);
        return -EINVAL;
    }

    if (s_pool.tts.valid && s_pool.tts.session_ready) {
        pthread_mutex_unlock(&s_pool.lock);
        return 0;
    }

    int ret = connect_tts_pool(&s_pool.tts);
    if (ret != 0)
        syslog(LOG_ERR, "[%s] TTS pool preconnect failed: %d\n", TAG, ret);
    else
        syslog(LOG_INFO, "[%s] TTS pool preconnect OK\n", TAG);
    pthread_mutex_unlock(&s_pool.lock);
    return ret;
}

void ws_conn_pool_idle_check(void)
{
    pthread_mutex_lock(&s_pool.lock);
    time_t now = time(NULL);

    if (s_pool.tts.valid
        && (now - s_pool.tts.last_used) > WS_POOL_IDLE_TIMEOUT_SEC) {
        syslog(LOG_INFO, "[%s] TTS pool: idle timeout, closing\n", TAG);
        pool_conn_destroy(&s_pool.tts);
    }

    if (s_pool.asr.valid
        && (now - s_pool.asr.last_used) > WS_POOL_IDLE_TIMEOUT_SEC) {
        syslog(LOG_INFO, "[%s] ASR pool: idle timeout, closing\n", TAG);
        pool_conn_destroy(&s_pool.asr);
    }

    pthread_mutex_unlock(&s_pool.lock);
}

int ws_conn_pool_tts_is_connected(void)
{
    pthread_mutex_lock(&s_pool.lock);
    int connected = s_pool.tts.valid && s_pool.tts.session_ready;
    pthread_mutex_unlock(&s_pool.lock);
    return connected;
}

int ws_conn_pool_asr_is_connected(void)
{
    pthread_mutex_lock(&s_pool.lock);
    int connected = s_pool.asr.valid && s_pool.asr.session_ready;
    pthread_mutex_unlock(&s_pool.lock);
    return connected;
}

void ws_conn_pool_invalidate_tts(void)
{
    /* If the lock is held (TTS stream in progress on a dead connection),
     * forcibly shutdown the socket to interrupt the blocked recv().
     * This allows the TTS stream function to return quickly instead of
     * waiting for the full socket timeout cycle. */
    if (pthread_mutex_trylock(&s_pool.lock) != 0) {
        int fd = s_pool.tts.tls.net.fd;
        if (fd >= 0) {
            syslog(LOG_INFO, "[%s] TTS pool: lock held, shutting down fd=%d to unblock\n",
                TAG, fd);
            shutdown(fd, SHUT_RDWR);
        }
        /* Wait for the TTS stream function to release the lock */
        pthread_mutex_lock(&s_pool.lock);
    }
    pool_conn_destroy(&s_pool.tts);
    pthread_mutex_unlock(&s_pool.lock);
}

void ws_conn_pool_set_tts_voice(const char* voice)
{
    if (!voice || !voice[0])
        return;
    pthread_mutex_lock(&s_pool.lock);
    strncpy(s_pool.tts_voice, voice, sizeof(s_pool.tts_voice) - 1);
    s_pool.tts_voice[sizeof(s_pool.tts_voice) - 1] = '\0';
    /* Destroy existing connection so next synthesis opens a new session
     * with the updated voice. */
    pool_conn_destroy(&s_pool.tts);
    pthread_mutex_unlock(&s_pool.lock);
    syslog(LOG_INFO, "[%s] TTS voice set to: %s\n", TAG, s_pool.tts_voice);
}

void ws_conn_pool_invalidate_asr(void)
{
    /* Same pattern as invalidate_tts: if the lock is held (ASR stream
     * in progress), shutdown the socket to unblock the recv(). */
    if (pthread_mutex_trylock(&s_pool.lock) != 0) {
        int fd = s_pool.asr.tls.net.fd;
        if (fd >= 0) {
            syslog(LOG_INFO, "[%s] ASR pool: lock held, shutting down fd=%d to unblock\n",
                TAG, fd);
            shutdown(fd, SHUT_RDWR);
        }
        pthread_mutex_lock(&s_pool.lock);
    }
    pool_conn_destroy(&s_pool.asr);
    pthread_mutex_unlock(&s_pool.lock);
}

int ws_conn_pool_tts_synthesize_stream(const char* text,
    ws_pool_tts_cb cb,
    void* user_data)
{
    if (!text || !cb)
        return -EINVAL;

    pthread_mutex_lock(&s_pool.lock);
    if (!s_pool.initialized) {
        pthread_mutex_unlock(&s_pool.lock);
        return -EINVAL;
    }

    int ret = connect_tts_pool(&s_pool.tts);
    if (ret != 0) {
        pthread_mutex_unlock(&s_pool.lock);
        return ret;
    }

    pool_conn_t* conn = &s_pool.tts;

    {
        cJSON* root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "type", "input_text_buffer.append");
        cJSON_AddStringToObject(root, "event_id", "evt_pool_tts_text");
        cJSON_AddStringToObject(root, "text", text);

        char* json = cJSON_PrintUnformatted(root);
        cJSON_Delete(root);
        if (!json) {
            pthread_mutex_unlock(&s_pool.lock);
            return -ENOMEM;
        }
        ret = pool_ws_send_text(&conn->tls, json, strlen(json));
        free(json);
        if (ret != 0) {
            pool_conn_destroy(conn);
            pthread_mutex_unlock(&s_pool.lock);
            return ret;
        }
    }

    {
        cJSON* root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "type", "input_text_buffer.commit");
        cJSON_AddStringToObject(root, "event_id", "evt_pool_tts_commit");

        char* json = cJSON_PrintUnformatted(root);
        cJSON_Delete(root);
        if (json) {
            ret = pool_ws_send_text(&conn->tls, json, strlen(json));
            free(json);
            if (ret != 0) {
                pool_conn_destroy(conn);
                pthread_mutex_unlock(&s_pool.lock);
                return ret;
            }
        }
    }

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
        pool_conn_destroy(conn);
        pthread_mutex_unlock(&s_pool.lock);
        return -ENOMEM;
    }

    while (1) {
        int n = pool_ws_recv_text(&conn->tls, frame_buf, frame_cap);
        if (n == 0) {
            syslog(LOG_INFO, "[%s] TTS pool: ws close frame\n", TAG);
            break;
        }
        if (n < 0) {
            if (n == -2)
                continue;
            syslog(LOG_ERR, "[%s] TTS pool: ws_recv error: %d\n", TAG, n);
            break;
        }

        cJSON* root = cJSON_Parse(frame_buf);
        if (!root) {
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
            if (got_audio > 0)
                cb(NULL, 0, 1, user_data);
            cJSON_Delete(root);
            break;
        } else if (strcmp(evt_type, "session.finished") == 0) {
            if (got_audio > 0)
                cb(NULL, 0, 1, user_data);
            cJSON_Delete(root);
            break;
        } else if (strcmp(evt_type, "error") == 0) {
            cJSON* err = cJSON_GetObjectItem(root, "error");
            cJSON* msg = err ? cJSON_GetObjectItem(err, "message")
                             : NULL;
            syslog(LOG_ERR, "[%s] TTS pool error: %s\n", TAG,
                (msg && cJSON_IsString(msg)) ? msg->valuestring
                                             : "unknown");
            cJSON_Delete(root);
            free(frame_buf);
            free(pcm_buf);
            pool_conn_destroy(conn);
            pthread_mutex_unlock(&s_pool.lock);
            return -EIO;
        } else {
            syslog(LOG_INFO, "[%s] TTS pool event: %s\n", TAG, evt_type);
        }

        cJSON_Delete(root);
    }

    free(frame_buf);
    free(pcm_buf);

    if (got_audio == 0) {
        syslog(LOG_WARNING, "[%s] TTS pool: 0 audio chunks, destroying conn\n", TAG);
        pool_conn_destroy(conn);
    } else {
        conn->last_used = time(NULL);
    }

    struct timespec t1;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    long ms = (t1.tv_sec - t0.tv_sec) * 1000
        + (t1.tv_nsec - t0.tv_nsec) / 1000000;

    syslog(LOG_INFO, "[%s] TTS pool complete (%ldms, %d chunks)\n",
        TAG, ms, got_audio);

    pthread_mutex_unlock(&s_pool.lock);
    return (got_audio > 0) ? 0 : -ENODATA;
}
