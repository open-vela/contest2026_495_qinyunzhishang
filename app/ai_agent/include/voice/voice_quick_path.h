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

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*quick_path_tts_cb)(const unsigned char* pcm_data,
    size_t pcm_len, int is_last, void* user_data);

typedef struct {
    const char* pattern;
    const char* response;
} voice_quick_rule_t;

int voice_quick_path_init(void);
void voice_quick_path_cleanup(void);

int voice_quick_path_register(const voice_quick_rule_t* rule);
int voice_quick_path_match(const char* text,
    quick_path_tts_cb tts_cb, void* user_data);

int voice_quick_path_match_text(const char* text,
    char* resp_out, size_t resp_cap);

#ifdef __cplusplus
}
#endif
