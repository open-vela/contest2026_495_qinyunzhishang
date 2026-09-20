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

#include "voice/voice_perf.h"
#include "agent_compat.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>

static const char* TAG = "voice_perf";

#define PERF_MAX_ENTRIES 64

typedef struct {
    voice_perf_entry_t entries[PERF_MAX_ENTRIES];
    int count;
    int write_idx;
    voice_perf_stats_t stats;
    pthread_mutex_t lock;
    int initialized;
} voice_perf_state_t;

static voice_perf_state_t s_perf = {
    .lock = PTHREAD_MUTEX_INITIALIZER,
};

static void update_minmax(uint32_t val, uint32_t* min_val, uint32_t* max_val)
{
    if (*min_val == 0 || val < *min_val)
        *min_val = val;
    if (val > *max_val)
        *max_val = val;
}

int voice_perf_init(void)
{
    pthread_mutex_lock(&s_perf.lock);
    memset(s_perf.entries, 0, sizeof(s_perf.entries));
    memset(&s_perf.stats, 0, sizeof(s_perf.stats));
    s_perf.count = 0;
    s_perf.write_idx = 0;
    s_perf.initialized = 1;
    pthread_mutex_unlock(&s_perf.lock);

    syslog(LOG_INFO, "[%s] initialized\n", TAG);
    return 0;
}

void voice_perf_cleanup(void)
{
    pthread_mutex_lock(&s_perf.lock);
    s_perf.initialized = 0;
    pthread_mutex_unlock(&s_perf.lock);

    syslog(LOG_INFO, "[%s] cleaned up\n", TAG);
}

void voice_perf_record(const voice_perf_entry_t* entry)
{
    if (!entry)
        return;

    pthread_mutex_lock(&s_perf.lock);
    if (!s_perf.initialized) {
        pthread_mutex_unlock(&s_perf.lock);
        return;
    }

    if (s_perf.count < PERF_MAX_ENTRIES)
        s_perf.count++;

    s_perf.entries[s_perf.write_idx] = *entry;
    s_perf.write_idx = (s_perf.write_idx + 1) % PERF_MAX_ENTRIES;

    voice_perf_stats_t* s = &s_perf.stats;
    s->total_requests++;

    s->asr_total_ms += entry->asr_latency_ms;
    s->llm_total_ms += entry->llm_latency_ms;
    s->tts_total_ms += entry->tts_latency_ms;
    s->e2e_total_ms += entry->total_latency_ms;

    if (entry->tts_cache_hit)
        s->tts_cache_hits++;
    if (entry->quick_path_hit)
        s->quick_path_hits++;
    if (entry->llm_stream_used)
        s->llm_stream_count++;

    s->tts_first_chunk_total_ms += entry->tts_first_chunk_ms;

    update_minmax(entry->asr_latency_ms, &s->asr_min_ms, &s->asr_max_ms);
    update_minmax(entry->llm_latency_ms, &s->llm_min_ms, &s->llm_max_ms);
    update_minmax(entry->tts_latency_ms, &s->tts_min_ms, &s->tts_max_ms);
    update_minmax(entry->total_latency_ms, &s->e2e_min_ms, &s->e2e_max_ms);

    pthread_mutex_unlock(&s_perf.lock);

    syslog(LOG_INFO,
        "[%s] record: asr=%u llm=%u tts=%u total=%u tts1st=%u%s%s\n",
        TAG,
        entry->asr_latency_ms,
        entry->llm_latency_ms,
        entry->tts_latency_ms,
        entry->total_latency_ms,
        entry->tts_first_chunk_ms,
        entry->tts_cache_hit ? " [CACHE]" : "",
        entry->quick_path_hit ? " [QUICK]" : "");
}

void voice_perf_get_stats(voice_perf_stats_t* stats)
{
    if (!stats)
        return;

    pthread_mutex_lock(&s_perf.lock);
    *stats = s_perf.stats;
    pthread_mutex_unlock(&s_perf.lock);
}

void voice_perf_reset(void)
{
    pthread_mutex_lock(&s_perf.lock);
    memset(s_perf.entries, 0, sizeof(s_perf.entries));
    memset(&s_perf.stats, 0, sizeof(s_perf.stats));
    s_perf.count = 0;
    s_perf.write_idx = 0;
    pthread_mutex_unlock(&s_perf.lock);

    syslog(LOG_INFO, "[%s] stats reset\n", TAG);
}

void voice_perf_dump(void)
{
    pthread_mutex_lock(&s_perf.lock);
    voice_perf_stats_t* s = &s_perf.stats;

    if (s->total_requests == 0) {
        pthread_mutex_unlock(&s_perf.lock);
        syslog(LOG_INFO, "[%s] no data\n", TAG);
        return;
    }

    uint32_t avg_asr = s->asr_total_ms / s->total_requests;
    uint32_t avg_llm = s->llm_total_ms / s->total_requests;
    uint32_t avg_tts = s->tts_total_ms / s->total_requests;
    uint32_t avg_e2e = s->e2e_total_ms / s->total_requests;
    uint32_t avg_tts1st = s->tts_first_chunk_total_ms / s->total_requests;

    syslog(LOG_INFO,
        "[%s] === Voice Performance Stats ===\n"
        "[%s] Requests: %u\n"
        "[%s] ASR:  avg=%u min=%u max=%u\n"
        "[%s] LLM:  avg=%u min=%u max=%u\n"
        "[%s] TTS:  avg=%u min=%u max=%u\n"
        "[%s] E2E:  avg=%u min=%u max=%u\n"
        "[%s] TTS1st: avg=%u\n"
        "[%s] Cache: %u/%u (%u%%)\n"
        "[%s] Quick: %u/%u (%u%%)\n"
        "[%s] Stream: %u/%u (%u%%)\n",
        TAG,
        TAG, s->total_requests,
        TAG, avg_asr, s->asr_min_ms, s->asr_max_ms,
        TAG, avg_llm, s->llm_min_ms, s->llm_max_ms,
        TAG, avg_tts, s->tts_min_ms, s->tts_max_ms,
        TAG, avg_e2e, s->e2e_min_ms, s->e2e_max_ms,
        TAG, avg_tts1st,
        TAG, s->tts_cache_hits, s->total_requests,
        s->total_requests > 0 ? (s->tts_cache_hits * 100 / s->total_requests) : 0,
        TAG, s->quick_path_hits, s->total_requests,
        s->total_requests > 0 ? (s->quick_path_hits * 100 / s->total_requests) : 0,
        TAG, s->llm_stream_count, s->total_requests,
        s->total_requests > 0 ? (s->llm_stream_count * 100 / s->total_requests) : 0);

    pthread_mutex_unlock(&s_perf.lock);
}
