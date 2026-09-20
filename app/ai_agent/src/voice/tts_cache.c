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

#include "voice/tts_cache.h"
#include "agent_compat.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>

static const char* TAG = "tts_cache";

typedef struct {
    char text[TTS_CACHE_MAX_TEXT_LEN];
    unsigned char* pcm;
    size_t pcm_len;
    int valid;
    uint32_t access_count;
    time_t last_access;
} tts_cache_entry_t;

static struct {
    tts_cache_entry_t entries[TTS_CACHE_MAX_ENTRIES];
    pthread_mutex_t lock;
    int initialized;
    int hits;
    int misses;
} s_cache = {
    .lock = PTHREAD_MUTEX_INITIALIZER,
};

static uint32_t simple_hash(const char* s)
{
    uint32_t h = 5381;
    while (*s) {
        h = ((h << 5) + h) + (unsigned char)*s;
        s++;
    }
    return h;
}

static int find_entry(const char* text)
{
    for (int i = 0; i < TTS_CACHE_MAX_ENTRIES; i++) {
        if (s_cache.entries[i].valid
            && strcmp(s_cache.entries[i].text, text) == 0) {
            return i;
        }
    }
    return -1;
}

static int find_lru_slot(void)
{
    int slot = -1;
    time_t oldest = 0;

    for (int i = 0; i < TTS_CACHE_MAX_ENTRIES; i++) {
        if (!s_cache.entries[i].valid) {
            return i;
        }
        if (slot == -1 || s_cache.entries[i].last_access < oldest) {
            oldest = s_cache.entries[i].last_access;
            slot = i;
        }
    }
    return slot;
}

int tts_cache_init(void)
{
    pthread_mutex_lock(&s_cache.lock);
    memset(s_cache.entries, 0, sizeof(s_cache.entries));
    s_cache.hits = 0;
    s_cache.misses = 0;
    s_cache.initialized = 1;
    pthread_mutex_unlock(&s_cache.lock);
    syslog(LOG_INFO, "[%s] initialized (%d slots)\n", TAG,
        TTS_CACHE_MAX_ENTRIES);
    return 0;
}

void tts_cache_cleanup(void)
{
    pthread_mutex_lock(&s_cache.lock);
    for (int i = 0; i < TTS_CACHE_MAX_ENTRIES; i++) {
        if (s_cache.entries[i].pcm) {
            free(s_cache.entries[i].pcm);
            s_cache.entries[i].pcm = NULL;
        }
    }
    memset(s_cache.entries, 0, sizeof(s_cache.entries));
    s_cache.initialized = 0;
    pthread_mutex_unlock(&s_cache.lock);
    syslog(LOG_INFO, "[%s] cleaned up (hits=%d, misses=%d)\n",
        TAG, s_cache.hits, s_cache.misses);
}

int tts_cache_lookup_stream(const char* text,
    tts_cache_chunk_cb cb, void* user_data)
{
    if (!text || !cb)
        return -EINVAL;

    if (strlen(text) >= TTS_CACHE_MAX_TEXT_LEN)
        return -ENOENT;

    pthread_mutex_lock(&s_cache.lock);
    if (!s_cache.initialized) {
        pthread_mutex_unlock(&s_cache.lock);
        return -ENOENT;
    }

    int idx = find_entry(text);
    if (idx < 0 || !s_cache.entries[idx].pcm) {
        s_cache.misses++;
        pthread_mutex_unlock(&s_cache.lock);
        return -ENOENT;
    }

    tts_cache_entry_t* e = &s_cache.entries[idx];
    e->access_count++;
    e->last_access = time(NULL);
    s_cache.hits++;

    unsigned char* pcm = e->pcm;
    size_t pcm_len = e->pcm_len;
    pthread_mutex_unlock(&s_cache.lock);

    size_t offset = 0;
    size_t chunk_size = 3200;
    while (offset < pcm_len) {
        size_t n = pcm_len - offset;
        if (n > chunk_size)
            n = chunk_size;
        int is_last = (offset + n >= pcm_len) ? 1 : 0;
        cb(pcm + offset, n, is_last, user_data);
        offset += n;
    }

    syslog(LOG_INFO, "[%s] hit: \"%s\" (%zu bytes)\n",
        TAG, text, pcm_len);
    return 0;
}

void tts_cache_store(const char* text,
    const unsigned char* pcm, size_t pcm_len)
{
    if (!text || !pcm || pcm_len == 0)
        return;

    if (strlen(text) >= TTS_CACHE_MAX_TEXT_LEN)
        return;

    if (pcm_len > TTS_CACHE_MAX_PCM_LEN)
        return;

    pthread_mutex_lock(&s_cache.lock);
    if (!s_cache.initialized) {
        pthread_mutex_unlock(&s_cache.lock);
        return;
    }

    int idx = find_entry(text);
    if (idx >= 0) {
        pthread_mutex_unlock(&s_cache.lock);
        return;
    }

    idx = find_lru_slot();
    if (idx < 0) {
        pthread_mutex_unlock(&s_cache.lock);
        return;
    }

    tts_cache_entry_t* e = &s_cache.entries[idx];
    if (e->pcm) {
        free(e->pcm);
        e->pcm = NULL;
    }

    e->pcm = malloc(pcm_len);
    if (!e->pcm) {
        pthread_mutex_unlock(&s_cache.lock);
        return;
    }

    memcpy(e->pcm, pcm, pcm_len);
    strncpy(e->text, text, TTS_CACHE_MAX_TEXT_LEN - 1);
    e->text[TTS_CACHE_MAX_TEXT_LEN - 1] = '\0';
    e->pcm_len = pcm_len;
    e->valid = 1;
    e->access_count = 0;
    e->last_access = time(NULL);

    pthread_mutex_unlock(&s_cache.lock);
    syslog(LOG_INFO, "[%s] stored: \"%s\" (%zu bytes)\n",
        TAG, text, pcm_len);
}

int tts_cache_hit_count(void)
{
    return s_cache.hits;
}

int tts_cache_miss_count(void)
{
    return s_cache.misses;
}
