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

#include "cJSON.h"
#include "agent_compat.h"
#include "agent_config.h"
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char* text;
    size_t text_len;
    char* reasoning_content;
    char tool_call_id[64];
    char tool_call_name[32];
    char* tool_call_input;
    size_t tool_call_input_len;
    int has_tool_call;
    bool tool_use;
    int is_final;
} llm_stream_chunk_t;

typedef void (*llm_stream_cb)(const llm_stream_chunk_t* chunk,
    void* user_data);

int llm_chat_tools_stream(const char* system_prompt,
    cJSON* messages,
    const char* tools_json,
    llm_stream_cb cb,
    void* user_data);

void llm_stream_chunk_free(llm_stream_chunk_t* chunk);

void llm_pool_idle_check(void);

void llm_pool_preconnect(void);

void llm_stream_set_voice_model(const char* model);

#ifdef __cplusplus
}
#endif
