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

typedef struct {
    uint32_t asr_latency_ms;
    uint32_t llm_latency_ms;
    uint32_t tts_latency_ms;
    uint32_t total_latency_ms;
    uint32_t tts_first_chunk_ms;
    int tts_cache_hit;
    int quick_path_hit;
    int llm_stream_used;
} voice_perf_entry_t;

typedef struct {
    uint32_t total_requests;
    uint32_t asr_total_ms;
    uint32_t llm_total_ms;
    uint32_t tts_total_ms;
    uint32_t e2e_total_ms;
    uint32_t tts_cache_hits;
    uint32_t quick_path_hits;
    uint32_t llm_stream_count;
    uint32_t tts_first_chunk_total_ms;
    uint32_t asr_min_ms;
    uint32_t asr_max_ms;
    uint32_t llm_min_ms;
    uint32_t llm_max_ms;
    uint32_t tts_min_ms;
    uint32_t tts_max_ms;
    uint32_t e2e_min_ms;
    uint32_t e2e_max_ms;
} voice_perf_stats_t;

int voice_perf_init(void);
void voice_perf_cleanup(void);

void voice_perf_record(const voice_perf_entry_t* entry);

void voice_perf_get_stats(voice_perf_stats_t* stats);

void voice_perf_reset(void);

void voice_perf_dump(void);

#ifdef __cplusplus
}
#endif
