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

#include "voice/voice_tts.h"
#include "voice/volc_tts.h"

#include <errno.h>
#include <string.h>
#include <syslog.h>

#define VOICE_TTS_MAX_BACKENDS 4

static const char* TAG = "voice_tts";

static const voice_tts_ops_t* s_backends[VOICE_TTS_MAX_BACKENDS];
static int s_backend_count;
static const voice_tts_ops_t* s_active;

int voice_tts_register(const voice_tts_ops_t* ops)
{
    if (!ops || !ops->name || !ops->synthesize) {
        return -EINVAL;
    }

    if (s_backend_count >= VOICE_TTS_MAX_BACKENDS) {
        syslog(LOG_ERR, "[%s] Too many TTS backends\n", TAG);
        return -ENOMEM;
    }

    s_backends[s_backend_count++] = ops;
    syslog(LOG_INFO, "[%s] Registered backend: %s\n", TAG, ops->name);

    /* First registered backend becomes the default */
    if (!s_active) {
        s_active = ops;
        if (ops->init) {
            ops->init();
        }
    }

    return 0;
}

int voice_tts_set_backend(const char* name)
{
    if (!name) {
        return -EINVAL;
    }

    for (int i = 0; i < s_backend_count; i++) {
        if (strcmp(s_backends[i]->name, name) == 0) {
            if (s_active && s_active->deinit) {
                s_active->deinit();
            }

            s_active = s_backends[i];
            if (s_active->init) {
                s_active->init();
            }

            syslog(LOG_INFO, "[%s] Backend set to: %s\n",
                TAG, name);
            return 0;
        }
    }

    syslog(LOG_ERR, "[%s] Backend not found: %s\n", TAG, name);
    return -ENOENT;
}

const char* voice_tts_get_backend(void)
{
    return s_active ? s_active->name : NULL;
}

int voice_tts_speak(const char* text,
    unsigned char* pcm_out,
    size_t pcm_cap,
    size_t* pcm_len)
{
    if (!s_active) {
        syslog(LOG_ERR, "[%s] No TTS backend registered\n", TAG);
        return -ENODEV;
    }

    return s_active->synthesize(text, pcm_out, pcm_cap, pcm_len);
}

/* ── Streaming TTS (delegates to active backend) ───────────── */

int voice_tts_speak_stream(const char *text,
    voice_tts_chunk_cb cb,
    void *user_data)
{
    if (!s_active) {
        syslog(LOG_ERR, "[%s] No TTS backend for streaming\n", TAG);
        return -ENODEV;
    }

    syslog(LOG_INFO, "[%s] speak_stream: backend=%s text=%zu bytes\n",
        TAG, s_active->name, text ? strlen(text) : 0);

    extern int ws_conn_pool_tts_synthesize_stream(const char *,
        void (*)(const unsigned char *, size_t, int, void *),
        void *);

    {
        int ret = ws_conn_pool_tts_synthesize_stream(text,
            (void (*)(const unsigned char *, size_t, int, void *))cb,
            user_data);
        if (ret == 0)
            return 0;
        if (ret != -EINVAL && ret != -ENOENT)
            syslog(LOG_WARNING,
                "[%s] pool TTS failed (%d), falling back to direct\n", TAG, ret);
    }

    if (strcmp(s_active->name, "dashscope") == 0) {
        extern int dashscope_tts_ws_synthesize_stream(const char *,
            void (*)(const unsigned char *, size_t, int, void *),
            void *);
        syslog(LOG_INFO, "[%s] delegating to dashscope_tts_ws_synthesize_stream\n", TAG);
        int ds_ret = dashscope_tts_ws_synthesize_stream(text,
            (void (*)(const unsigned char *, size_t, int, void *))cb,
            user_data);
        if (ds_ret == 0)
            return 0;
        syslog(LOG_WARNING, "[%s] dashscope TTS failed (%d), trying volc TTS\n",
            TAG, ds_ret);
    }

    extern int volc_tts_ws_synthesize_stream(const char *,
        void (*)(const unsigned char *, size_t, int, void *),
        void *);
    syslog(LOG_INFO, "[%s] delegating to volc_tts_ws_synthesize_stream\n", TAG);
    return volc_tts_ws_synthesize_stream(text,
        (void (*)(const unsigned char *, size_t, int, void *))cb,
        user_data);
}
