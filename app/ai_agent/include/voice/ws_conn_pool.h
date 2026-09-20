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

#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WS_POOL_IDLE_TIMEOUT_SEC 60

typedef void (*ws_pool_tts_cb)(const unsigned char* pcm_data,
    size_t pcm_len, int is_last, void* user_data);

int ws_conn_pool_init(void);
void ws_conn_pool_cleanup(void);

int ws_conn_pool_preconnect_tts(void);
void ws_conn_pool_idle_check(void);

int ws_conn_pool_asr_is_connected(void);
int ws_conn_pool_tts_is_connected(void);
void ws_conn_pool_invalidate_asr(void);
void ws_conn_pool_invalidate_tts(void);

/* Update TTS voice for subsequent syntheses. Invalidates existing TTS
 * connection so next synthesis opens a new session with the new voice. */
void ws_conn_pool_set_tts_voice(const char* voice);

int ws_conn_pool_tts_synthesize_stream(const char* text,
    ws_pool_tts_cb cb, void* user_data);

#ifdef __cplusplus
}
#endif
