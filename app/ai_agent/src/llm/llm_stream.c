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

#include "llm/llm_stream.h"
#include "llm/llm_internal.h"
#include "llm/llm_parse.h"
#include "llm/llm_proxy.h"
#include "infra/config_store.h"
#include "infra/http_proxy.h"
#include "agent_compat.h"
#include "agent_config.h"

#include "cJSON.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/ssl.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <syslog.h>
#include <unistd.h>

static const char* TAG = "llm_stream";

static char s_voice_model[64] = { 0 };

void llm_stream_set_voice_model(const char* model)
{
    if (model && model[0]) {
        strncpy(s_voice_model, model, sizeof(s_voice_model) - 1);
        s_voice_model[sizeof(s_voice_model) - 1] = '\0';
    } else {
        s_voice_model[0] = '\0';
    }
}

void llm_stream_chunk_free(llm_stream_chunk_t* chunk)
{
    if (!chunk)
        return;
    if (chunk->text) {
        free(chunk->text);
        chunk->text = NULL;
    }
    if (chunk->reasoning_content) {
        free(chunk->reasoning_content);
        chunk->reasoning_content = NULL;
    }
    if (chunk->tool_call_input) {
        free(chunk->tool_call_input);
        chunk->tool_call_input = NULL;
    }
}

static int parse_sse_line(const char* line, llm_stream_cb cb,
    void* user_data, int* done)
{
    *done = 0;

    if (line[0] == '\0')
        return 0;

    if (strncmp(line, "data: ", 6) != 0)
        return 0;

    const char* data = line + 6;

    if (strcmp(data, "[DONE]") == 0) {
        *done = 1;
        llm_stream_chunk_t final_chunk;
        memset(&final_chunk, 0, sizeof(final_chunk));
        final_chunk.is_final = 1;
        if (cb)
            cb(&final_chunk, user_data);
        return 0;
    }

    cJSON* root = cJSON_Parse(data);
    if (!root)
        return 0;

    cJSON* choices = cJSON_GetObjectItem(root, "choices");
    if (!choices || !cJSON_IsArray(choices) || !choices->child) {
        cJSON_Delete(root);
        return 0;
    }

    cJSON* choice = choices->child;
    cJSON* delta = cJSON_GetObjectItem(choice, "delta");
    cJSON* finish = cJSON_GetObjectItem(choice, "finish_reason");

    llm_stream_chunk_t chunk;
    memset(&chunk, 0, sizeof(chunk));

    if (delta) {
        cJSON* content = cJSON_GetObjectItem(delta, "content");
        if (content && cJSON_IsString(content) && content->valuestring[0]) {
            size_t len = strlen(content->valuestring);
            chunk.text = malloc(len + 1);
            if (chunk.text) {
                memcpy(chunk.text, content->valuestring, len + 1);
                chunk.text_len = len;
            }
        }

        cJSON* rc = cJSON_GetObjectItem(delta, "reasoning_content");
        if (rc && cJSON_IsString(rc) && rc->valuestring[0]) {
            chunk.reasoning_content = strdup(rc->valuestring);
        }

        cJSON* tool_calls = cJSON_GetObjectItem(delta, "tool_calls");
        if (tool_calls && cJSON_IsArray(tool_calls) && tool_calls->child) {
            cJSON* tc = tool_calls->child;
            cJSON* id_item = cJSON_GetObjectItem(tc, "id");
            if (id_item && cJSON_IsString(id_item))
                strncpy(chunk.tool_call_id, id_item->valuestring,
                    sizeof(chunk.tool_call_id) - 1);

            cJSON* func = cJSON_GetObjectItem(tc, "function");
            if (func) {
                cJSON* name = cJSON_GetObjectItem(func, "name");
                if (name && cJSON_IsString(name))
                    strncpy(chunk.tool_call_name, name->valuestring,
                        sizeof(chunk.tool_call_name) - 1);

                cJSON* args = cJSON_GetObjectItem(func, "arguments");
                if (args && cJSON_IsString(args) && args->valuestring[0]) {
                    size_t alen = strlen(args->valuestring);
                    chunk.tool_call_input = malloc(alen + 1);
                    if (chunk.tool_call_input) {
                        memcpy(chunk.tool_call_input, args->valuestring,
                            alen + 1);
                        chunk.tool_call_input_len = alen;
                    }
                }
            }
            chunk.has_tool_call = 1;
        }
    }

    if (finish && cJSON_IsString(finish)) {
        if (strcmp(finish->valuestring, "tool_calls") == 0)
            chunk.tool_use = true;
        else if (strcmp(finish->valuestring, "stop") == 0)
            chunk.is_final = 1;
    }

    if (chunk.text || chunk.has_tool_call || chunk.is_final) {
        if (cb)
            cb(&chunk, user_data);
    }

    llm_stream_chunk_free(&chunk);
    cJSON_Delete(root);
    return 0;
}

static int parse_sse_buffer(const char* buf, size_t len,
    size_t* parsed_offset, int* header_parsed,
    llm_stream_cb cb, void* user_data,
    int* done, int* out_status)
{
    *done = 0;

    const char* body_start;
    size_t body_len;

    if (!*header_parsed) {
        const char* hdr_end = memmem(buf, len, "\r\n\r\n", 4);
        if (!hdr_end)
            return 0;

        *out_status = 0;
        if (sscanf(buf, "HTTP/1.1 %d", out_status) != 1
            && sscanf(buf, "HTTP/1.0 %d", out_status) != 1) {
        }
        *header_parsed = 1;

        if (*out_status != 200) {
            syslog(LOG_ERR, "[%s] stream HTTP %d\n", TAG, *out_status);
            return ERROR;
        }

        body_start = hdr_end + 4;
        body_len = len - (size_t)(body_start - buf);
        *parsed_offset = 0;
    } else {
        body_start = buf;
        body_len = len;
    }

    const char* p = body_start + *parsed_offset;
    const char* end = body_start + body_len;

    while (p < end) {
        const char* nl = memchr(p, '\n', (size_t)(end - p));
        if (!nl) {
            break;
        }

        size_t line_len = (size_t)(nl - p);
        char* line = malloc(line_len + 1);
        if (line) {
            memcpy(line, p, line_len);
            line[line_len] = '\0';

            size_t trim = line_len;
            while (trim > 0 && (line[trim - 1] == '\r'
                                   || line[trim - 1] == '\n')) {
                line[--trim] = '\0';
            }

            int line_done = 0;
            parse_sse_line(line, cb, user_data, &line_done);
            free(line);

            if (line_done) {
                *parsed_offset = (size_t)(nl + 1 - body_start);
                *done = 1;
                return OK;
            }
        }

        p = nl + 1;
    }

    *parsed_offset = (size_t)(p - body_start);
    return OK;
}

static int stream_entropy(void* data, unsigned char* output, size_t len)
{
    (void)data;
    if (agent_secure_random(output, len) == 0)
        return 0;
    syslog(LOG_ERR, "[%s] No entropy source\n", TAG);
    return -1;
}

typedef struct {
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config cfg;
    mbedtls_net_context net;
    mbedtls_ctr_drbg_context ctr_drbg;
    int use_tls;
} stream_tls_t;

static void stream_tls_free(stream_tls_t* ctx);
static int stream_tls_connect(stream_tls_t* ctx,
    const char* host, const char* port);

static struct {
    stream_tls_t tls;
    char host[128];
    char port[8];
    int valid;
    time_t last_used;
    pthread_mutex_t lock;
} s_llm_pool = {
    .lock = PTHREAD_MUTEX_INITIALIZER,
};

static void llm_pool_invalidate(void)
{
    if (s_llm_pool.valid) {
        stream_tls_free(&s_llm_pool.tls);
        memset(&s_llm_pool.tls, 0, sizeof(s_llm_pool.tls));
        s_llm_pool.tls.net.fd = -1;
        s_llm_pool.valid = 0;
        s_llm_pool.host[0] = '\0';
        s_llm_pool.port[0] = '\0';
    }
}

static int llm_pool_get(const char* host, const char* port,
    stream_tls_t* out)
{
    pthread_mutex_lock(&s_llm_pool.lock);

    if (s_llm_pool.valid
        && strcmp(s_llm_pool.host, host) == 0
        && strcmp(s_llm_pool.port, port) == 0
        && s_llm_pool.tls.net.fd >= 0) {
        int err = 0;
        socklen_t err_len = sizeof(err);
        if (getsockopt(s_llm_pool.tls.net.fd, SOL_SOCKET, SO_ERROR,
                &err, &err_len) == 0
            && err == 0) {
            unsigned char probe;
            int ret = mbedtls_ssl_read(&s_llm_pool.tls.ssl, &probe, 1);
            if (ret == 0
                || ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY
                || ret < 0) {
                if (ret != MBEDTLS_ERR_SSL_WANT_READ
                    && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
                    syslog(LOG_WARNING,
                        "[%s] LLM pool: TLS connection dead (ret=%d), reconnecting\n",
                        TAG, ret);
                    llm_pool_invalidate();
                    pthread_mutex_unlock(&s_llm_pool.lock);
                    return -1;
                }
            }
            *out = s_llm_pool.tls;
            memset(&s_llm_pool.tls, 0, sizeof(s_llm_pool.tls));
            s_llm_pool.tls.net.fd = -1;
            s_llm_pool.valid = 0;
            s_llm_pool.host[0] = '\0';
            s_llm_pool.port[0] = '\0';
            pthread_mutex_unlock(&s_llm_pool.lock);
            syslog(LOG_INFO, "[%s] LLM pool: reused connection to %s:%s\n",
                TAG, host, port);
            return 0;
        }
        syslog(LOG_WARNING, "[%s] LLM pool: stale connection, reconnecting\n",
            TAG);
        llm_pool_invalidate();
    }

    pthread_mutex_unlock(&s_llm_pool.lock);
    return -1;
}

static void llm_pool_return(stream_tls_t* tls, const char* host,
    const char* port)
{
    pthread_mutex_lock(&s_llm_pool.lock);

    llm_pool_invalidate();

    if (tls && tls->net.fd >= 0) {
        s_llm_pool.tls = *tls;
        strncpy(s_llm_pool.host, host, sizeof(s_llm_pool.host) - 1);
        strncpy(s_llm_pool.port, port, sizeof(s_llm_pool.port) - 1);
        s_llm_pool.valid = 1;
        s_llm_pool.last_used = time(NULL);
        memset(tls, 0, sizeof(*tls));
        tls->net.fd = -1;
        syslog(LOG_INFO, "[%s] LLM pool: returned connection to %s:%s\n",
            TAG, host, port);
    }

    pthread_mutex_unlock(&s_llm_pool.lock);
}

void llm_pool_idle_check(void)
{
    pthread_mutex_lock(&s_llm_pool.lock);
    if (s_llm_pool.valid) {
        time_t now = time(NULL);
        if (now - s_llm_pool.last_used > 120) {
            syslog(LOG_INFO, "[%s] LLM pool: idle timeout, closing\n", TAG);
            llm_pool_invalidate();
        }
    }
    pthread_mutex_unlock(&s_llm_pool.lock);
}

static void* llm_pool_preconnect_thread(void* arg)
{
    (void)arg;
    pthread_mutex_lock(&s_llm_pool.lock);
    if (s_llm_pool.valid) {
        pthread_mutex_unlock(&s_llm_pool.lock);
        return NULL;
    }
    pthread_mutex_unlock(&s_llm_pool.lock);

    char api_key[128], host[128], path[128], port[8], model[64];
    llm_snapshot_config_full(model, sizeof(model),
        api_key, sizeof(api_key),
        host, sizeof(host),
        path, sizeof(path),
        port, sizeof(port));

    syslog(LOG_INFO, "[%s] LLM pool: preconnecting to %s:%s\n", TAG, host, port);

    stream_tls_t tls;
    memset(&tls, 0, sizeof(tls));
    tls.net.fd = -1;

    int ret = stream_tls_connect(&tls, host, port);
    if (ret != 0) {
        syslog(LOG_WARNING, "[%s] LLM pool: preconnect failed: %d\n", TAG, ret);
        stream_tls_free(&tls);
        return NULL;
    }

    llm_pool_return(&tls, host, port);
    syslog(LOG_INFO, "[%s] LLM pool: preconnect OK\n", TAG);
    return NULL;
}

void llm_pool_preconnect(void)
{
    pthread_mutex_lock(&s_llm_pool.lock);
    if (s_llm_pool.valid) {
        pthread_mutex_unlock(&s_llm_pool.lock);
        return;
    }
    pthread_mutex_unlock(&s_llm_pool.lock);

    pthread_t tid;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 24 * 1024);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&tid, &attr, llm_pool_preconnect_thread, NULL) != 0) {
        syslog(LOG_WARNING, "[%s] LLM pool: preconnect thread failed\n", TAG);
    }
    pthread_attr_destroy(&attr);
}

static void stream_tls_free(stream_tls_t* ctx)
{
    if (!ctx)
        return;
    if (ctx->use_tls) {
        mbedtls_ssl_close_notify(&ctx->ssl);
        mbedtls_ssl_free(&ctx->ssl);
        mbedtls_ssl_config_free(&ctx->cfg);
        mbedtls_ctr_drbg_free(&ctx->ctr_drbg);
    }
    if (ctx->net.fd >= 0) {
        close(ctx->net.fd);
        ctx->net.fd = -1;
    }
}

static int stream_tls_connect(stream_tls_t* ctx,
    const char* host, const char* port)
{
    int ret;
    int use_tls = (strcmp(port, "443") == 0);
    ctx->use_tls = use_tls;

    if (use_tls) {
        mbedtls_ssl_init(&ctx->ssl);
        mbedtls_ssl_config_init(&ctx->cfg);
        mbedtls_net_init(&ctx->net);
        mbedtls_ctr_drbg_init(&ctx->ctr_drbg);

        ret = mbedtls_ctr_drbg_seed(&ctx->ctr_drbg, stream_entropy,
            NULL, (const unsigned char*)"llm_stm", 7);
        if (ret != 0) {
            syslog(LOG_ERR, "[%s] ctr_drbg_seed: -0x%04x\n", TAG, -ret);
            return -EIO;
        }
    } else {
        mbedtls_net_init(&ctx->net);
    }

    if (http_proxy_is_enabled()) {
        int port_num = atoi(port);
        int tunnel_fd = proxy_open_tunnel(host, port_num, 30000);
        if (tunnel_fd < 0) {
            syslog(LOG_ERR, "[%s] proxy tunnel failed\n", TAG);
            return -ECONNREFUSED;
        }
        ctx->net.fd = tunnel_fd;
    } else {
        ret = mbedtls_net_connect(&ctx->net, host, port,
            MBEDTLS_NET_PROTO_TCP);
        if (ret != 0) {
            syslog(LOG_ERR, "[%s] TCP connect: -0x%04x\n", TAG, -ret);
            return -ECONNREFUSED;
        }
    }

    mbedtls_net_set_block(&ctx->net);
    if (ctx->net.fd >= 0) {
        struct timeval tv = { .tv_sec = 30, .tv_usec = 0 };
        setsockopt(ctx->net.fd, SOL_SOCKET, SO_RCVTIMEO,
            &tv, sizeof(tv));
        setsockopt(ctx->net.fd, SOL_SOCKET, SO_SNDTIMEO,
            &tv, sizeof(tv));
    }

    if (!use_tls)
        return 0;

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

    syslog(LOG_INFO, "[%s] TLS handshake...\n", TAG);
    while ((ret = mbedtls_ssl_handshake(&ctx->ssl)) != 0) {
        if (ret != MBEDTLS_ERR_SSL_WANT_READ
            && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
            syslog(LOG_ERR, "[%s] handshake: -0x%04x\n", TAG, -ret);
            return -EIO;
        }
    }
    syslog(LOG_INFO, "[%s] TLS OK\n", TAG);
    return 0;
}

static int stream_tls_write_all(stream_tls_t* ctx,
    const unsigned char* buf, size_t len)
{
    if (!ctx->use_tls) {
        ssize_t n = write(ctx->net.fd, buf, len);
        if (n < 0)
            return -EIO;
        return 0;
    }

    size_t sent = 0;
    while (sent < len) {
        int ret = mbedtls_ssl_write(&ctx->ssl, buf + sent, len - sent);
        if (ret == MBEDTLS_ERR_SSL_WANT_READ
            || ret == MBEDTLS_ERR_SSL_WANT_WRITE)
            continue;
        if (ret <= 0)
            return -EIO;
        sent += (size_t)ret;
    }
    return 0;
}

static int stream_tls_read(stream_tls_t* ctx,
    unsigned char* buf, size_t cap)
{
    if (!ctx->use_tls) {
        ssize_t n = read(ctx->net.fd, buf, cap);
        if (n <= 0)
            return -1;
        return (int)n;
    }

    int ret = mbedtls_ssl_read(&ctx->ssl, buf, cap);
    if (ret == MBEDTLS_ERR_SSL_WANT_READ)
        return 0;
    if (ret == 0 || ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY)
        return -1;
    if (ret < 0)
        return -1;
    return ret;
}

static int llm_stream_http_direct(const char* post_data,
    llm_stream_cb cb, void* user_data,
    int* out_status)
{
    char api_key[128], llm_host[128], llm_path[128], llm_port[8], model[64];
    llm_snapshot_config_full(model, sizeof(model),
        api_key, sizeof(api_key),
        llm_host, sizeof(llm_host),
        llm_path, sizeof(llm_path),
        llm_port, sizeof(llm_port));

    char auth_header[256];
    snprintf(auth_header, sizeof(auth_header), "Bearer %s", api_key);

    char provider[64] = { 0 };
    const char* slash = strchr(model, '/');
    if (slash) {
        size_t plen = (size_t)(slash - model);
        if (plen >= sizeof(provider))
            plen = sizeof(provider) - 1;
        memcpy(provider, model, plen);
    }

    char header[1024];
    int hlen = snprintf(header, sizeof(header),
        "POST %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Content-Type: application/json\r\n"
        "Authorization: %s\r\n"
        "%s%s"
        "Content-Length: %zu\r\n"
        "Connection: keep-alive\r\n\r\n",
        llm_path, llm_host, auth_header,
        provider[0] ? "X-Model-Provider-Id: " : "",
        provider[0] ? provider : "",
        strlen(post_data));

    stream_tls_t tls;
    memset(&tls, 0, sizeof(tls));
    tls.net.fd = -1;

    int reused = 0;
    if (llm_pool_get(llm_host, llm_port, &tls) == 0) {
        reused = 1;
    } else {
        int ret = stream_tls_connect(&tls, llm_host, llm_port);
        if (ret != 0) {
            stream_tls_free(&tls);
            return ERROR;
        }
    }

    int ret = stream_tls_write_all(&tls,
        (const unsigned char*)header, (size_t)hlen);
    if (ret != 0) {
        if (reused) {
            syslog(LOG_WARNING, "[%s] LLM pool: write failed on reused conn, retrying\n", TAG);
            stream_tls_free(&tls);
            reused = 0;
            memset(&tls, 0, sizeof(tls));
            tls.net.fd = -1;
            ret = stream_tls_connect(&tls, llm_host, llm_port);
            if (ret != 0) {
                stream_tls_free(&tls);
                return ERROR;
            }
            ret = stream_tls_write_all(&tls,
                (const unsigned char*)header, (size_t)hlen);
            if (ret != 0) {
                stream_tls_free(&tls);
                return ERROR;
            }
        } else {
            stream_tls_free(&tls);
            return ERROR;
        }
    }

    ret = stream_tls_write_all(&tls,
        (const unsigned char*)post_data, strlen(post_data));
    if (ret != 0) {
        stream_tls_free(&tls);
        return ERROR;
    }

    char* resp_buf = malloc(AGENT_LLM_STREAM_BUF_SIZE);
    if (!resp_buf) {
        stream_tls_free(&tls);
        return ERROR;
    }
    size_t resp_len = 0;
    size_t resp_cap = AGENT_LLM_STREAM_BUF_SIZE;
    int header_parsed = 0;
    size_t parsed_offset = 0;
    int conn_keep_alive = 1;
    int got_bad_response = 0;

    while (1) {
        if (resp_len >= resp_cap - 1) {
            size_t new_cap = resp_cap * 2;
            if (new_cap > AGENT_LLM_MAX_RESP_SIZE) {
                break;
            }
            char* tmp = realloc(resp_buf, new_cap);
            if (!tmp)
                break;
            resp_buf = tmp;
            resp_cap = new_cap;
        }

        int n = stream_tls_read(&tls,
            (unsigned char*)resp_buf + resp_len,
            resp_cap - resp_len - 1);
        if (n < 0)
            break;
        if (n == 0)
            continue;
        resp_len += (size_t)n;
        resp_buf[resp_len] = '\0';

        if (!header_parsed) {
            const char* hdr_end = memmem(resp_buf, resp_len, "\r\n\r\n", 4);
            if (hdr_end) {
                const char* cl = strcasestr(resp_buf, "Connection:");
                if (cl && cl < hdr_end) {
                    cl += 11;
                    while (*cl == ' ') cl++;
                    if (strncasecmp(cl, "close", 5) == 0)
                        conn_keep_alive = 0;
                }
            }
        }

        int done = 0;
        int pr = parse_sse_buffer(resp_buf, resp_len,
            &parsed_offset, &header_parsed,
            cb, user_data, &done, out_status);
        if (pr != OK) {
            if (reused && *out_status == 0 && !got_bad_response) {
                syslog(LOG_WARNING,
                    "[%s] LLM pool: bad response from reused conn, retrying\n",
                    TAG);
                got_bad_response = 1;
                stream_tls_free(&tls);
                free(resp_buf);
                reused = 0;

                memset(&tls, 0, sizeof(tls));
                tls.net.fd = -1;
                ret = stream_tls_connect(&tls, llm_host, llm_port);
                if (ret != 0) {
                    stream_tls_free(&tls);
                    return ERROR;
                }

                ret = stream_tls_write_all(&tls,
                    (const unsigned char*)header, (size_t)hlen);
                if (ret != 0) {
                    stream_tls_free(&tls);
                    return ERROR;
                }
                ret = stream_tls_write_all(&tls,
                    (const unsigned char*)post_data, strlen(post_data));
                if (ret != 0) {
                    stream_tls_free(&tls);
                    return ERROR;
                }

                resp_buf = malloc(AGENT_LLM_STREAM_BUF_SIZE);
                if (!resp_buf) {
                    stream_tls_free(&tls);
                    return ERROR;
                }
                resp_len = 0;
                resp_cap = AGENT_LLM_STREAM_BUF_SIZE;
                header_parsed = 0;
                parsed_offset = 0;
                conn_keep_alive = 1;
                continue;
            }
            stream_tls_free(&tls);
            free(resp_buf);
            return pr;
        }
        if (done) {
            if (conn_keep_alive && tls.net.fd >= 0) {
                llm_pool_return(&tls, llm_host, llm_port);
            } else {
                stream_tls_free(&tls);
            }
            free(resp_buf);
            return OK;
        }
    }

    stream_tls_free(&tls);
    free(resp_buf);
    return OK;
}

int llm_chat_tools_stream(const char* system_prompt,
    cJSON* messages,
    const char* tools_json,
    llm_stream_cb cb,
    void* user_data)
{
    char model[64], api_key[128], llm_host[128];
    llm_snapshot_config(model, sizeof(model),
        api_key, sizeof(api_key),
        llm_host, sizeof(llm_host));

    if (s_voice_model[0]) {
        strncpy(model, s_voice_model, sizeof(model) - 1);
        model[sizeof(model) - 1] = '\0';
    }

    if (api_key[0] == '\0') {
        syslog(LOG_ERR, "[%s] No API key configured\n", TAG);
        return ERROR;
    }

    cJSON* body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "model",
        model_name_for_api(model, llm_host));

    if (is_openai_compat_host(llm_host)) {
        cJSON_AddNumberToObject(body, "max_completion_tokens",
            AGENT_LLM_MAX_TOKENS_OPENAI);
    } else {
        cJSON_AddNumberToObject(body, "max_tokens",
            AGENT_LLM_MAX_TOKENS);
    }

    cJSON_AddBoolToObject(body, "stream", true);

    cJSON* msgs = cJSON_Duplicate(messages, 1);
    cJSON* sys_msg = cJSON_CreateObject();
    cJSON_AddStringToObject(sys_msg, "role", "system");
    cJSON_AddStringToObject(sys_msg, "content", system_prompt);
    cJSON_InsertItemInArray(msgs, 0, sys_msg);
    cJSON_AddItemToObject(body, "messages", msgs);

    cJSON* tools_arr = build_openai_tools_array(tools_json);
    if (tools_arr) {
        cJSON_AddItemToObject(body, "tools", tools_arr);
    }

    char* post_data = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (!post_data)
        return ERROR;

    syslog(LOG_INFO, "[%s] streaming API call (model: %s, %d bytes)\n",
        TAG, model, (int)strlen(post_data));

    int status = 0;
    int err = llm_stream_http_direct(post_data, cb, user_data, &status);
    free(post_data);

    if (err != OK) {
        syslog(LOG_ERR, "[%s] stream failed: err=%d status=%d\n",
            TAG, err, status);
        return err;
    }

    syslog(LOG_INFO, "[%s] stream complete (status=%d)\n", TAG, status);
    return OK;
}
