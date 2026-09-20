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

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TTS_CACHE_MAX_ENTRIES 32
#define TTS_CACHE_MAX_TEXT_LEN 128
/* 512KB 可存储约10.9秒音频(24kHz/16bit/mono)，覆盖大部分单段TTS文本。
 * 之前256KB只能存5.5秒(24kHz)，导致较长TTS音频被截断，
 * CACHE命中时只播放前半段，后半段静默丢失。 */
#define TTS_CACHE_MAX_PCM_LEN (512 * 1024)

int tts_cache_init(void);
void tts_cache_cleanup(void);

typedef void (*tts_cache_chunk_cb)(const unsigned char* pcm_data,
    size_t pcm_len, int is_last, void* user_data);

int tts_cache_lookup_stream(const char* text,
    tts_cache_chunk_cb cb, void* user_data);

void tts_cache_store(const char* text,
    const unsigned char* pcm, size_t pcm_len);

int tts_cache_hit_count(void);
int tts_cache_miss_count(void);

#ifdef __cplusplus
}
#endif
