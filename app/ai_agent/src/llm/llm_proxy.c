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
 * This file contains code derived from MimiClaw (https://github.com/memovai/mimiclaw)
 * Copyright (c) 2026 Ziboyan Wang, licensed under the MIT License.
 * See NOTICE file for the original MIT License terms.
 */

#include "llm/llm_internal.h"
#include "llm/llm_parse.h"
#include "llm/llm_proxy.h"
#include "core/message_bus.h"
#include "infra/config_store.h"
#include "infra/http_proxy.h"
#include "infra/vela_tls.h"
#include "agent_compat.h"
#include "agent_config.h"

#ifdef CONFIG_AI_AGENT_NET_RPMSG
#include "network/network_manager.h"
#endif

#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char* TAG = "llm";

static char s_api_key[128] = { 0 };
static char s_model[64] = AGENT_LLM_DEFAULT_MODEL;
static char s_llm_host[128] = AGENT_LLM_API_HOST;
static char s_llm_path[128] = AGENT_LLM_API_PATH;
static char s_llm_port[8] = "443"; /* "443" for HTTPS, "80" etc for HTTP */

/* Independent vision model config — empty means fall back to main LLM */
static char s_vision_model[64] = { 0 };
static char s_vision_host[128] = { 0 };
static char s_vision_api_key[128] = { 0 };

static pthread_mutex_t s_llm_lock = PTHREAD_MUTEX_INITIALIZER;

/* Check if host uses OpenAI-compatible max_completion_tokens param */
bool is_openai_compat_host(const char* host)
{
    return strstr(host, "openai.com")
        || strstr(host, "openrouter.ai")
        || strstr(host, "xiaomimimo.com");
}


int resp_buf_init(resp_buf_t* rb, size_t initial_cap)
{
    rb->data = calloc(1, initial_cap);
    if (!rb->data)
        return ERROR;
    rb->len = 0;
    rb->cap = initial_cap;
    return OK;
}

int resp_buf_append(resp_buf_t* rb, const char* data, size_t len)
{
    while (rb->len + len >= rb->cap) {
        size_t new_cap = rb->cap * 2;
        if (new_cap > AGENT_LLM_MAX_RESP_SIZE) {
            syslog(LOG_ERR, "llm: resp_buf exceeded %d limit\n",
                AGENT_LLM_MAX_RESP_SIZE);
            return ERROR;
        }
        char* tmp = realloc(rb->data, new_cap);
        if (!tmp)
            return ERROR;
        rb->data = tmp;
        rb->cap = new_cap;
    }
    memcpy(rb->data + rb->len, data, len);
    rb->len += len;
    rb->data[rb->len] = '\0';
    return OK;
}

void resp_buf_free(resp_buf_t* rb)
{
    if (rb->data) {
        free(rb->data);
        rb->data = NULL;
    }
    rb->len = 0;
    rb->cap = 0;
}

/* ── Init ─────────────────────────────────────────────────── */

int llm_proxy_init(void)
{
    /* 默认使用宏定义的 API key */
    if (AGENT_SECRET_API_KEY[0])
        strncpy(s_api_key, AGENT_SECRET_API_KEY, sizeof(s_api_key) - 1);
    if (AGENT_SECRET_MODEL[0])
        strncpy(s_model, AGENT_SECRET_MODEL, sizeof(s_model) - 1);
    if (AGENT_SECRET_LLM_HOST[0])
        strncpy(s_llm_host, AGENT_SECRET_LLM_HOST, sizeof(s_llm_host) - 1);
    if (AGENT_SECRET_LLM_PATH[0])
        strncpy(s_llm_path, AGENT_SECRET_LLM_PATH, sizeof(s_llm_path) - 1);

    char tmp[128] = { 0 };
    if (claw_config_get(AGENT_CFG_KEY_API_KEY, tmp, sizeof(tmp)) == OK && tmp[0]) {
        if (strncmp(tmp, "sk-", 3) == 0 || strncmp(tmp, "sk_", 3) == 0)
            strncpy(s_api_key, tmp, sizeof(s_api_key) - 1);
        else
            syslog(LOG_WARNING, "[%s] Config store API key invalid (not sk- prefix), using default\n", TAG);
    }

    /* 优先从 /emmc/wifi/agent_app_id 文件读取 API key（最高优先级，覆盖 config 与宏） */
    {
        char file_key[128] = { 0 };
        if (claw_load_file_string("/emmc/wifi/agent_app_id",
                                  file_key, sizeof(file_key)) == OK) {
            strncpy(s_api_key, file_key, sizeof(s_api_key) - 1);
            s_api_key[sizeof(s_api_key) - 1] = '\0';
            syslog(LOG_INFO, "[%s] API key loaded from /emmc/wifi/agent_app_id: %s\n", TAG, s_api_key);
        }
    }
    memset(tmp, 0, sizeof(tmp));
    if (claw_config_get(AGENT_CFG_KEY_MODEL, tmp, sizeof(tmp)) == OK && tmp[0])
        strncpy(s_model, tmp, sizeof(s_model) - 1);

    memset(tmp, 0, sizeof(tmp));
    if (claw_config_get(AGENT_CFG_KEY_LLM_HOST, tmp, sizeof(tmp)) == OK && tmp[0])
        strncpy(s_llm_host, tmp, sizeof(s_llm_host) - 1);
    memset(tmp, 0, sizeof(tmp));
    if (claw_config_get(AGENT_CFG_KEY_LLM_PATH, tmp, sizeof(tmp)) == OK && tmp[0]) {
        /* Auto-correct path for DashScope endpoints. Match both:
         *   - legacy shared domain: dashscope.aliyuncs.com
         *   - workspace-dedicated domain: *.maas.aliyuncs.com
         * Both require /compatible-mode/v1/chat/completions instead of
         * /v1/chat/completions, otherwise API returns 404. */
        if ((strstr(s_llm_host, "dashscope") != NULL
             || strstr(s_llm_host, "maas.aliyuncs.com") != NULL)
            && strcmp(tmp, "/v1/chat/completions") == 0) {
            strncpy(s_llm_path, "/compatible-mode/v1/chat/completions",
                sizeof(s_llm_path) - 1);
            syslog(LOG_WARNING,
                "[%s] Auto-correcting DashScope path in init\n", TAG);
        } else {
            strncpy(s_llm_path, tmp, sizeof(s_llm_path) - 1);
        }
    }
    memset(tmp, 0, sizeof(tmp));
    if (claw_config_get("llm_port", tmp, sizeof(tmp)) == OK && tmp[0])
        strncpy(s_llm_port, tmp, sizeof(s_llm_port) - 1);

    /* Load optional vision model config — empty falls back to main LLM */
    memset(tmp, 0, sizeof(tmp));
    if (claw_config_get(AGENT_CFG_KEY_VISION_MODEL, tmp, sizeof(tmp)) == OK && tmp[0])
        strncpy(s_vision_model, tmp, sizeof(s_vision_model) - 1);
    memset(tmp, 0, sizeof(tmp));
    if (claw_config_get(AGENT_CFG_KEY_VISION_HOST, tmp, sizeof(tmp)) == OK && tmp[0])
        strncpy(s_vision_host, tmp, sizeof(s_vision_host) - 1);
    memset(tmp, 0, sizeof(tmp));
    if (claw_config_get(AGENT_CFG_KEY_VISION_API_KEY, tmp, sizeof(tmp)) == OK && tmp[0])
        strncpy(s_vision_api_key, tmp, sizeof(s_vision_api_key) - 1);

    if (s_api_key[0])
        syslog(LOG_INFO, "[%s] LLM proxy initialized (model: %s, host: %s)\n", TAG,
            s_model, s_llm_host);
    else
        syslog(LOG_WARNING, "[%s] No API key. Use CLI: set_llm <preset> <key>\n", TAG);

    printf("[DEBUG] LLM init: model=%s, host=%s, path=%s, port=%s, key=%s\n",
        s_model, s_llm_host, s_llm_path, s_llm_port,
        s_api_key[0] ? "set" : "EMPTY");
    return OK;
}

int llm_set_backend(const char* host, const char* path)
{
    pthread_mutex_lock(&s_llm_lock);
    if (host && host[0]) {
        claw_config_set(AGENT_CFG_KEY_LLM_HOST, host);
        strncpy(s_llm_host, host, sizeof(s_llm_host) - 1);
    }
    if (path && path[0]) {
        claw_config_set(AGENT_CFG_KEY_LLM_PATH, path);
        strncpy(s_llm_path, path, sizeof(s_llm_path) - 1);
    }
    pthread_mutex_unlock(&s_llm_lock);
    syslog(LOG_INFO, "[%s] LLM backend: %s%s\n", TAG, s_llm_host, s_llm_path);
    return OK;
}

int llm_set_port(const char* port)
{
    if (port && port[0]) {
        claw_config_set("llm_port", port);
        strncpy(s_llm_port, port, sizeof(s_llm_port) - 1);
        s_llm_port[sizeof(s_llm_port) - 1] = '\0';
    }
    return OK;
}

int llm_set_all(const char* host, const char* path,
    const char* port, const char* api_key, const char* model)
{
    pthread_mutex_lock(&s_llm_lock);

    if (host && host[0]) {
        claw_config_set(AGENT_CFG_KEY_LLM_HOST, host);
        strncpy(s_llm_host, host, sizeof(s_llm_host) - 1);
        s_llm_host[sizeof(s_llm_host) - 1] = '\0';
    }
    if (path && path[0]) {
        const char* effective_path = path;
        /* Auto-correct path for both legacy (dashscope.aliyuncs.com)
         * and workspace-dedicated (*.maas.aliyuncs.com) endpoints. */
        if ((strstr(s_llm_host, "dashscope") != NULL
             || strstr(s_llm_host, "maas.aliyuncs.com") != NULL)
            && strcmp(path, "/v1/chat/completions") == 0) {
            effective_path = "/compatible-mode/v1/chat/completions";
            syslog(LOG_WARNING,
                "[%s] Auto-correcting DashScope path: %s -> %s\n",
                TAG, path, effective_path);
        }
        claw_config_set(AGENT_CFG_KEY_LLM_PATH, effective_path);
        strncpy(s_llm_path, effective_path, sizeof(s_llm_path) - 1);
        s_llm_path[sizeof(s_llm_path) - 1] = '\0';
    }
    if (port && port[0]) {
        claw_config_set("llm_port", port);
        strncpy(s_llm_port, port, sizeof(s_llm_port) - 1);
        s_llm_port[sizeof(s_llm_port) - 1] = '\0';
    }
    if (api_key && api_key[0]) {
        claw_config_set(AGENT_CFG_KEY_API_KEY, api_key);
        strncpy(s_api_key, api_key, sizeof(s_api_key) - 1);
        s_api_key[sizeof(s_api_key) - 1] = '\0';
    }
    if (model && model[0]) {
        claw_config_set(AGENT_CFG_KEY_MODEL, model);
        strncpy(s_model, model, sizeof(s_model) - 1);
        s_model[sizeof(s_model) - 1] = '\0';
    }

    pthread_mutex_unlock(&s_llm_lock);

    syslog(LOG_INFO, "[%s] LLM config updated atomically: %s%s (model: %s)\n",
        TAG, s_llm_host, s_llm_path, s_model);
    return OK;
}

/* ── Config snapshot (used by llm_vision.c) ───────────────── */

void llm_snapshot_config(char* model, size_t model_sz,
    char* api_key, size_t key_sz,
    char* host, size_t host_sz)
{
    pthread_mutex_lock(&s_llm_lock);
    strncpy(model, s_model, model_sz - 1);
    model[model_sz - 1] = '\0';
    strncpy(api_key, s_api_key, key_sz - 1);
    api_key[key_sz - 1] = '\0';
    strncpy(host, s_llm_host, host_sz - 1);
    host[host_sz - 1] = '\0';
    pthread_mutex_unlock(&s_llm_lock);
}

void llm_snapshot_config_full(char* model, size_t model_sz,
    char* api_key, size_t key_sz,
    char* host, size_t host_sz,
    char* path, size_t path_sz,
    char* port, size_t port_sz)
{
    pthread_mutex_lock(&s_llm_lock);
    strncpy(model, s_model, model_sz - 1);
    model[model_sz - 1] = '\0';
    strncpy(api_key, s_api_key, key_sz - 1);
    api_key[key_sz - 1] = '\0';
    strncpy(host, s_llm_host, host_sz - 1);
    host[host_sz - 1] = '\0';
    strncpy(path, s_llm_path, path_sz - 1);
    path[path_sz - 1] = '\0';
    strncpy(port, s_llm_port, port_sz - 1);
    port[port_sz - 1] = '\0';
    pthread_mutex_unlock(&s_llm_lock);
}

/* ── Vision config snapshot ──────────────────────────────────── */

void llm_snapshot_vision_config(char* model, size_t model_sz,
    char* api_key, size_t key_sz,
    char* host, size_t host_sz)
{
    pthread_mutex_lock(&s_llm_lock);

    /* Use vision-specific config when set, otherwise fall back to main LLM */
    if (s_vision_model[0])
        strncpy(model, s_vision_model, model_sz - 1);
    else
        strncpy(model, s_model, model_sz - 1);
    model[model_sz - 1] = '\0';

    if (s_vision_api_key[0])
        strncpy(api_key, s_vision_api_key, key_sz - 1);
    else
        strncpy(api_key, s_api_key, key_sz - 1);
    api_key[key_sz - 1] = '\0';

    if (s_vision_host[0])
        strncpy(host, s_vision_host, host_sz - 1);
    else
        strncpy(host, s_llm_host, host_sz - 1);
    host[host_sz - 1] = '\0';

    pthread_mutex_unlock(&s_llm_lock);
}

/* ── Vision model setter ─────────────────────────────────────── */

int llm_set_vision_model(const char* host, const char* model,
    const char* api_key)
{
    pthread_mutex_lock(&s_llm_lock);

    if (host && host[0]) {
        claw_config_set(AGENT_CFG_KEY_VISION_HOST, host);
        strncpy(s_vision_host, host, sizeof(s_vision_host) - 1);
        s_vision_host[sizeof(s_vision_host) - 1] = '\0';
    } else {
        claw_config_set(AGENT_CFG_KEY_VISION_HOST, "");
        s_vision_host[0] = '\0';
    }

    if (model && model[0]) {
        claw_config_set(AGENT_CFG_KEY_VISION_MODEL, model);
        strncpy(s_vision_model, model, sizeof(s_vision_model) - 1);
        s_vision_model[sizeof(s_vision_model) - 1] = '\0';
    } else {
        claw_config_set(AGENT_CFG_KEY_VISION_MODEL, "");
        s_vision_model[0] = '\0';
    }

    if (api_key && api_key[0]) {
        claw_config_set(AGENT_CFG_KEY_VISION_API_KEY, api_key);
        strncpy(s_vision_api_key, api_key, sizeof(s_vision_api_key) - 1);
        s_vision_api_key[sizeof(s_vision_api_key) - 1] = '\0';
    } else {
        claw_config_set(AGENT_CFG_KEY_VISION_API_KEY, "");
        s_vision_api_key[0] = '\0';
    }

    syslog(LOG_INFO, "[%s] Vision LLM config updated: model=%s host=%s\n",
        TAG, s_vision_model[0] ? s_vision_model : "(inherit)",
        s_vision_host[0] ? s_vision_host : "(inherit)");

    pthread_mutex_unlock(&s_llm_lock);
    return OK;
}

/* ── HTTP helpers ─────────────────────────────────────────── */

/**
 * Return the model name to send in the API request.
 * Multi-provider gateways (e.g. OpenRouter) require the full
 * "provider/model" string, while single-vendor APIs expect only
 * the bare model name after the slash.
 */
const char* model_name_for_api(const char* model, const char* host)
{
    /* Multi-provider gateways need the full identifier */
    if (strstr(host, "openrouter.ai")) {
        return model;
    }

    const char* slash = strchr(model, '/');
    return slash ? slash + 1 : model;
}

/* Direct path via vela_tls */
static int llm_http_direct(const char* post_data, resp_buf_t* rb,
    int* out_status)
{
    /* Snapshot config under lock */
    char api_key[128], llm_host[128], llm_path[128], llm_port[8], model[64];
    pthread_mutex_lock(&s_llm_lock);
    memcpy(api_key, s_api_key, sizeof(api_key));
    memcpy(llm_host, s_llm_host, sizeof(llm_host));
    memcpy(llm_path, s_llm_path, sizeof(llm_path));
    memcpy(llm_port, s_llm_port, sizeof(llm_port));
    memcpy(model, s_model, sizeof(model));
    pthread_mutex_unlock(&s_llm_lock);

    /* Allocate the response buffer once and hand it directly to the
     * TLS layer.  Previously we allocated a separate raw_buf and then
     * copied into resp_buf — doubling peak memory usage.  Now the
     * resp_buf IS the raw buffer, eliminating the copy. */
    size_t raw_cap = AGENT_LLM_STREAM_BUF_SIZE;
    if (resp_buf_init(rb, raw_cap) != OK)
        return ERROR;

    /* OpenAI-compatible format: Authorization: Bearer <key> */
    char auth_header[256];
    snprintf(auth_header, sizeof(auth_header), "Bearer %s", api_key);

    /* Extract provider ID from model string (e.g. "xiaomi/mimo-claw-0301" →
     * "xiaomi") Mify gateway requires X-Model-Provider-Id header for routing. */
    char provider[64] = { 0 };
    const char* slash = strchr(model, '/');
    if (slash) {
        size_t plen = (size_t)(slash - model);
        if (plen >= sizeof(provider))
            plen = sizeof(provider) - 1;
        memcpy(provider, model, plen);
    }

    vela_header_t hdrs[] = { { "Authorization", auth_header },
        { provider[0] ? "X-Model-Provider-Id" : NULL,
            provider[0] ? provider : NULL },
        { NULL, NULL } };

    int status;
    int use_tls = (strcmp(llm_port, "443") == 0);

    if (use_tls) {
        status = vela_https_post_json(llm_host, llm_port, llm_path, hdrs, post_data,
            rb->data, raw_cap);
    } else {
        status = vela_http_post_json(llm_host, llm_port, llm_path, hdrs, post_data,
            rb->data, raw_cap);
    }

    if (status < 0) {
        printf("[DEBUG] HTTP request failed: status=%d, host=%s, port=%s, path=%s, tls=%d\n",
            status, llm_host, llm_port, llm_path, use_tls);
        syslog(LOG_ERR, "[%s] HTTP request failed: status=%d, host=%s:%s%s\n",
            TAG, status, llm_host, llm_port, llm_path);
        resp_buf_free(rb);
        return ERROR;
    }

    /* Update length — TLS layer NUL-terminated the buffer */
    rb->len = strlen(rb->data);

    *out_status = status;
    return OK;
}

/* Proxy path via CONNECT tunnel */
static int llm_http_via_proxy(const char* post_data, resp_buf_t* rb,
    int* out_status)
{
    /* Snapshot config under lock */
    char api_key[128], llm_host[128], llm_path[128], llm_port[8], model[64];
    pthread_mutex_lock(&s_llm_lock);
    memcpy(api_key, s_api_key, sizeof(api_key));
    memcpy(llm_host, s_llm_host, sizeof(llm_host));
    memcpy(llm_path, s_llm_path, sizeof(llm_path));
    memcpy(llm_port, s_llm_port, sizeof(llm_port));
    memcpy(model, s_model, sizeof(model));
    pthread_mutex_unlock(&s_llm_lock);

    int port = atoi(llm_port);
    if (port <= 0)
        port = 443;

    proxy_conn_t* conn = proxy_conn_open(llm_host, port, 30000);
    if (!conn)
        return ERROR;

    int body_len = (int)strlen(post_data);
    char auth_header[256];
    snprintf(auth_header, sizeof(auth_header), "Bearer %s", api_key);

    /* Extract provider ID from model (e.g. "xiaomi/mimo-claw-0301" → "xiaomi") */
    char provider_hdr[128] = { 0 };
    const char* slash = strchr(model, '/');
    if (slash) {
        char provider[64] = { 0 };
        size_t plen = (size_t)(slash - model);
        if (plen >= sizeof(provider))
            plen = sizeof(provider) - 1;
        memcpy(provider, model, plen);
        snprintf(provider_hdr, sizeof(provider_hdr), "X-Model-Provider-Id: %s\r\n",
            provider);
    }

    char header[1024];
    int hlen = snprintf(header, sizeof(header),
        "POST %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Content-Type: application/json\r\n"
        "Authorization: %s\r\n"
        "%s"
        "Content-Length: %d\r\n"
        "Connection: close\r\n\r\n",
        llm_path, llm_host, auth_header, provider_hdr, body_len);

    if (proxy_conn_write(conn, header, hlen) < 0 || proxy_conn_write(conn, post_data, body_len) < 0) {
        proxy_conn_close(conn);
        return ERROR;
    }

    if (resp_buf_init(rb, AGENT_LLM_STREAM_BUF_SIZE) != OK) {
        proxy_conn_close(conn);
        return ERROR;
    }

    char tmp[4096];
    while (1) {
        int n = proxy_conn_read(conn, tmp, sizeof(tmp), 120000);
        if (n <= 0)
            break;
        if (resp_buf_append(rb, tmp, (size_t)n) != OK) {
            syslog(LOG_ERR, "[%s] resp_buf_append OOM, truncating\n", TAG);
            break;
        }
    }
    proxy_conn_close(conn);

    /* Parse status from raw HTTP response */
    *out_status = 0;
    if (rb->len > 5 && strncmp(rb->data, "HTTP/", 5) == 0) {
        const char* sp = strchr(rb->data, ' ');
        if (sp)
            *out_status = atoi(sp + 1);
    }

    /* Strip HTTP header, keep body */
    char* body = strstr(rb->data, "\r\n\r\n");
    if (body) {
        body += 4;
        size_t blen = rb->len - (size_t)(body - rb->data);
        memmove(rb->data, body, blen);
        rb->len = blen;
        rb->data[rb->len] = '\0';
    }

    return OK;
}

int llm_http_call(const char* post_data, resp_buf_t* rb,
    int* out_status)
{
    /* For plain HTTP endpoints (port != 443), always use direct path.
     * The proxy does CONNECT + TLS which fails on non-TLS endpoints.
     * Non-TLS HTTP endpoints (port != 443) are reachable
     * directly without a proxy anyway. */
    int use_tls;
    pthread_mutex_lock(&s_llm_lock);
    use_tls = (strcmp(s_llm_port, "443") == 0);
    pthread_mutex_unlock(&s_llm_lock);

#ifdef CONFIG_AI_AGENT_NET_RPMSG
    int retry_max = network_get_retry_max();
    int retry_base = network_get_retry_base_sec();
    int ret = ERROR;

    for (int attempt = 0; attempt <= retry_max; attempt++) {
        if (attempt > 0) {
            /* Check network state before retrying */
            if (network_get_state() != NET_STATE_CONNECTED) {
                syslog(LOG_WARNING, "[llm] Network down, skip retry %d\n", attempt);
                break;
            }
            int delay = retry_base * (1 << (attempt - 1));
            syslog(LOG_INFO, "[llm] Retry %d/%d after %ds\n", attempt, retry_max, delay);
            sleep(delay);
        }

        if (use_tls && http_proxy_is_enabled()) {
            ret = llm_http_via_proxy(post_data, rb, out_status);
        } else {
            ret = llm_http_direct(post_data, rb, out_status);
        }

        if (ret == OK) {
            return OK;
        }

        syslog(LOG_WARNING, "[llm] HTTP call failed (attempt %d/%d)\n",
            attempt + 1, retry_max + 1);
    }

    /* All retries failed — notify user */
    syslog(LOG_ERR, "[llm] All %d retries failed\n", retry_max + 1);
    {
        agent_msg_t notify;
        memset(&notify, 0, sizeof(notify));
        strncpy(notify.channel, "cli", sizeof(notify.channel) - 1);
        notify.content = strdup("网络请求失败，请检查蓝牙连接");
        if (notify.content) {
            if (message_bus_push_outbound(&notify) != OK) {
                free(notify.content);
            }
        }
    }
    return ERROR;
#else
    if (use_tls && http_proxy_is_enabled())
        return llm_http_via_proxy(post_data, rb, out_status);
    return llm_http_direct(post_data, rb, out_status);
#endif
}

/* ── JSON helpers ─────────────────────────────────────────── */

/* Extract text from OpenAI response format: choices[0].message.content */
void extract_text(cJSON* root, char* buf, size_t size)
{
    buf[0] = '\0';
    cJSON* choices = cJSON_GetObjectItem(root, "choices");
    if (!choices || !cJSON_IsArray(choices))
        return;

    cJSON* first = choices->child;
    if (!first)
        return;

    cJSON* message = cJSON_GetObjectItem(first, "message");
    if (!message)
        return;

    cJSON* content = cJSON_GetObjectItem(message, "content");
    if (!content || !cJSON_IsString(content))
        return;

    size_t tlen = strlen(content->valuestring);
    size_t copy = (tlen < size - 1) ? tlen : size - 1;
    memcpy(buf, content->valuestring, copy);
    buf[copy] = '\0';
}

/* ── Public: simple chat ──────────────────────────────────── */

int llm_chat(const char* system_prompt, const char* messages_json,
    char* response_buf, size_t buf_size)
{
    /* Snapshot config under lock */
    char model[64], api_key[128], llm_host[128];
    pthread_mutex_lock(&s_llm_lock);
    memcpy(model, s_model, sizeof(model));
    memcpy(api_key, s_api_key, sizeof(api_key));
    memcpy(llm_host, s_llm_host, sizeof(llm_host));
    pthread_mutex_unlock(&s_llm_lock);

    if (api_key[0] == '\0') {
        snprintf(response_buf, buf_size, "Error: No API key configured");
        return ERROR;
    }

    cJSON* body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "model",
        model_name_for_api(model, llm_host));

    if (is_openai_compat_host(llm_host))
        cJSON_AddNumberToObject(body, "max_completion_tokens",
            AGENT_LLM_MAX_TOKENS_OPENAI);
    else
        cJSON_AddNumberToObject(body, "max_tokens", AGENT_LLM_MAX_TOKENS);

    /* Disable thinking mode for Qwen3.x series on DashScope.
     *
     * Per DashScope docs (2026-07): "qwen3.7、qwen3.6 和qwen3.5 系列
     * 模型默认开启思考模式" (thinking mode enabled by default).
     * Thinking mode generates hidden reasoning tokens before the final
     * answer, which:
     *   1. Increases latency 5-10x (observed: 29s vs expected 3-5s)
     *   2. Consumes max_tokens budget on thinking, truncating output
     *   3. Provides no benefit for simple tasks like summarization
     *
     * Setting enable_thinking=false explicitly disables it, reducing
     * Qwen3.5-flash summary latency from ~29s to ~3-5s.
     *
     * This parameter is DashScope-specific (ignored by OpenAI/Mimo),
     * but only sent when host is NOT OpenAI-compatible. */
    if (!is_openai_compat_host(llm_host)) {
        cJSON_AddBoolToObject(body, "enable_thinking", 0);
    }

    cJSON* messages = cJSON_Parse(messages_json);
    if (!messages)
        messages = cJSON_CreateArray();

    /* Prepend system message (OpenAI format) */
    cJSON* sys_msg = cJSON_CreateObject();
    cJSON_AddStringToObject(sys_msg, "role", "system");
    cJSON_AddStringToObject(sys_msg, "content", system_prompt);
    cJSON_InsertItemInArray(messages, 0, sys_msg);

    cJSON_AddItemToObject(body, "messages", messages);

    char* post_data = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (!post_data) {
        snprintf(response_buf, buf_size, "Error: Failed to build request");
        return ERROR;
    }

    syslog(LOG_INFO, "[%s] Calling LLM API (model: %s, host: %s, %d bytes)\n",
        TAG, model, llm_host, (int)strlen(post_data));

    resp_buf_t rb = { 0 };
    int status = 0;
    int err = ERROR;
    int retry;

    for (retry = 0; retry <= AGENT_LLM_MAX_RETRIES; retry++) {
        if (retry > 0) {
            unsigned int delay = AGENT_LLM_RETRY_BASE_SEC << (retry - 1);
            syslog(LOG_WARNING, "[%s] Retry %d/%d after %us\n",
                TAG, retry, AGENT_LLM_MAX_RETRIES, delay);
            sleep(delay);
        }

        rb.len = 0;
        status = 0;
        err = llm_http_call(post_data, &rb, &status);

        if (err != OK) {
            /* Network-level failure — retry */
            syslog(LOG_WARNING,
                "[%s] HTTP request failed (err=%d), will retry\n",
                TAG, err);
            resp_buf_free(&rb);
            memset(&rb, 0, sizeof(rb));
            continue;
        }

        if (status != 429)
            break;

        /* HTTP 429 rate limited — retry */
        resp_buf_free(&rb);
        memset(&rb, 0, sizeof(rb));
    }

    free(post_data);

    if (status != 200) {
        snprintf(response_buf, buf_size, "API error (HTTP %d): %.200s", status,
            rb.data ? rb.data : "");
        resp_buf_free(&rb);
        return ERROR;
    }

    cJSON* root = cJSON_Parse(rb.data);
    resp_buf_free(&rb);

    if (!root) {
        snprintf(response_buf, buf_size, "Error: Failed to parse response");
        return ERROR;
    }

    extract_text(root, response_buf, buf_size);
    cJSON_Delete(root);

    if (response_buf[0] == '\0')
        snprintf(response_buf, buf_size, "No response from LLM API");
    else
        syslog(LOG_INFO, "[%s] LLM response: %d bytes\n", TAG,
            (int)strlen(response_buf));

    return OK;
}

/* ── Public: chat with tools ──────────────────────────────── */


int llm_chat_tools(const char* system_prompt, cJSON* messages,
    const char* tools_json, llm_response_t* resp)
{
    memset(resp, 0, sizeof(*resp));

    /* Snapshot config under lock */
    char model[64];
    char api_key[128];
    char llm_host[128];

    pthread_mutex_lock(&s_llm_lock);
    memcpy(model, s_model, sizeof(model));
    memcpy(api_key, s_api_key, sizeof(api_key));
    memcpy(llm_host, s_llm_host, sizeof(llm_host));
    pthread_mutex_unlock(&s_llm_lock);

    if (api_key[0] == '\0') {
        syslog(LOG_ERR, "[%s] No API key configured\n", TAG);
        printf("[DEBUG] llm_chat_tools: API key is EMPTY\n");
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

    /* Disable thinking mode for Qwen3.x on DashScope (see llm_chat
     * for detailed rationale). Critical for latency: thinking mode
     * adds 20-30s hidden reasoning that provides no value for
     * typical agent conversations. */
    if (!is_openai_compat_host(llm_host)) {
        cJSON_AddBoolToObject(body, "enable_thinking", 0);
    }

    /* Clone messages and prepend system message */
    cJSON* msgs = cJSON_Duplicate(messages, 1);
    cJSON* sys_msg = cJSON_CreateObject();

    cJSON_AddStringToObject(sys_msg, "role", "system");
    cJSON_AddStringToObject(sys_msg, "content", system_prompt);
    cJSON_InsertItemInArray(msgs, 0, sys_msg);
    cJSON_AddItemToObject(body, "messages", msgs);

    /* Convert tools to OpenAI format */
    cJSON* tools_arr = build_openai_tools_array(tools_json);

    if (tools_arr) {
        cJSON_AddItemToObject(body, "tools", tools_arr);
    }

    char* post_data = cJSON_PrintUnformatted(body);

    cJSON_Delete(body);
    if (!post_data) {
        return ERROR;
    }

    syslog(LOG_INFO,
        "[%s] OpenAI API with tools (model: %s, %d bytes)\n",
        TAG, model, (int)strlen(post_data));

    resp_buf_t rb = { 0 };
    int status = 0;
    int err = ERROR;
    int retry;

    for (retry = 0; retry <= AGENT_LLM_MAX_RETRIES; retry++) {
        if (retry > 0) {
            unsigned int delay = AGENT_LLM_RETRY_BASE_SEC << (retry - 1);
            syslog(LOG_WARNING, "[%s] Rate limited (429), retry %d/%d after %us\n",
                TAG, retry, AGENT_LLM_MAX_RETRIES, delay);
            sleep(delay);
        }

        rb.len = 0;
        status = 0;
        err = llm_http_call(post_data, &rb, &status);

        if (err != OK) {
            resp_buf_free(&rb);
            free(post_data);
            return err;
        }

        if (status != 429)
            break;

        resp_buf_free(&rb);
        memset(&rb, 0, sizeof(rb));
    }

    free(post_data);

    if (status != 200) {
        syslog(LOG_ERR, "[%s] API error %d: %.500s\n", TAG, status,
            rb.data ? rb.data : "");
        printf("[DEBUG] API returned HTTP %d: %.200s\n", status,
            rb.data ? rb.data : "(empty)");
        resp_buf_free(&rb);
        return ERROR;
    }

    cJSON* root = cJSON_Parse(rb.data);

    resp_buf_free(&rb);
    if (!root) {
        syslog(LOG_ERR, "[%s] Failed to parse API JSON\n", TAG);
        return ERROR;
    }

    /* Extract from OpenAI response */
    cJSON* choices = cJSON_GetObjectItem(root, "choices");

    if (!choices || !cJSON_IsArray(choices) || !choices->child) {
        cJSON_Delete(root);
        return ERROR;
    }

    cJSON* choice = choices->child;
    cJSON* finish = cJSON_GetObjectItem(choice, "finish_reason");

    resp->tool_use = (finish && cJSON_IsString(finish)
        && strcmp(finish->valuestring, "tool_calls") == 0);

    cJSON* message = cJSON_GetObjectItem(choice, "message");

    if (message) {
        cJSON* text_content = cJSON_GetObjectItem(message, "content");
        if (text_content && cJSON_IsString(text_content) && text_content->valuestring) {
            size_t tlen = strlen(text_content->valuestring);
            resp->text = calloc(1, tlen + 1);
            if (resp->text) {
                memcpy(resp->text, text_content->valuestring, tlen);
                resp->text_len = tlen;
            }
        }

        /* Kimi thinking mode: preserve reasoning_content so it can be echoed
         * back in the next turn (required by the API). */
        cJSON* rc = cJSON_GetObjectItem(message, "reasoning_content");
        if (rc && cJSON_IsString(rc) && rc->valuestring && rc->valuestring[0]) {
            resp->reasoning_content = strdup(rc->valuestring);
        }

        /* Extract tool calls from OpenAI format */
        cJSON* tool_calls = cJSON_GetObjectItem(message, "tool_calls");
        if (tool_calls && cJSON_IsArray(tool_calls)) {
            cJSON* tc;
            cJSON_ArrayForEach(tc, tool_calls)
            {
                if (resp->call_count >= AGENT_MAX_TOOL_CALLS)
                    break;

                llm_tool_call_t* call = &resp->calls[resp->call_count];

                /* OpenAI tool_calls format:
                   { "id": "call_xyz", "type": "function",
                     "function": { "name": "foo", "arguments": "{...}" }
                   }
                */
                cJSON* id_item = cJSON_GetObjectItem(tc, "id");
                if (id_item && cJSON_IsString(id_item))
                    strncpy(call->id, id_item->valuestring, sizeof(call->id) - 1);

                cJSON* func_obj = cJSON_GetObjectItem(tc, "function");
                if (func_obj) {
                    cJSON* name_item = cJSON_GetObjectItem(func_obj, "name");
                    cJSON* args_item = cJSON_GetObjectItem(func_obj, "arguments");

                    if (name_item && cJSON_IsString(name_item))
                        strncpy(call->name, name_item->valuestring, sizeof(call->name) - 1);

                    if (args_item && cJSON_IsString(args_item)) {
                        size_t alen = strlen(args_item->valuestring);
                        call->input = calloc(1, alen + 1);
                        if (call->input) {
                            memcpy(call->input, args_item->valuestring, alen);
                            call->input_len = alen;
                        }
                    } else if (args_item && cJSON_IsObject(args_item)) {
                        /* Some models return arguments as a JSON object
                         * instead of a JSON string — serialize it. */
                        char* serialized = cJSON_PrintUnformatted(args_item);
                        if (serialized) {
                            call->input = serialized;
                            call->input_len = strlen(serialized);
                        }
                    }
                }

                resp->call_count++;
            }

            /* If we found tool calls, ensure tool_use flag is set
             * (some models/proxies return stop_reason="end_turn" even with tools) */
            if (resp->call_count > 0) {
                resp->tool_use = true;
            }
        }
    }

    /* Extract token usage from API response */
    cJSON* usage = cJSON_GetObjectItem(root, "usage");
    if (usage) {
        cJSON* pt = cJSON_GetObjectItem(usage, "prompt_tokens");
        cJSON* ct = cJSON_GetObjectItem(usage, "completion_tokens");
        cJSON* tt = cJSON_GetObjectItem(usage, "total_tokens");
        if (pt && cJSON_IsNumber(pt))
            resp->prompt_tokens = (int)pt->valuedouble;
        if (ct && cJSON_IsNumber(ct))
            resp->completion_tokens = (int)ct->valuedouble;
        if (tt && cJSON_IsNumber(tt))
            resp->total_tokens = (int)tt->valuedouble;
    }

    /* XML fallback parsers for non-standard models */
    parse_xml_tool_calls(resp);
    parse_ns_xml_tool_calls(resp);

    cJSON_Delete(root);

    syslog(LOG_INFO,
        "[%s] Response: %d bytes text, %d tool calls, finish=%s\n",
        TAG, (int)resp->text_len, resp->call_count,
        resp->tool_use ? "tool_calls" : "end_turn");

    return OK;
}
