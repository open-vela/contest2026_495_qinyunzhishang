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

#include "voice/voice_quick_path.h"
#include "agent_compat.h"

#include <ctype.h>
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>

static const char* TAG = "quick_path";

#define QUICK_PATH_MAX_RULES 32
#define QUICK_PATH_MAX_PATTERN_LEN 64
#define QUICK_PATH_MAX_RESPONSE_LEN 256

typedef struct {
    char pattern[QUICK_PATH_MAX_PATTERN_LEN];
    char response[QUICK_PATH_MAX_RESPONSE_LEN];
    int valid;
} quick_rule_entry_t;

static struct {
    quick_rule_entry_t rules[QUICK_PATH_MAX_RULES];
    int count;
    pthread_mutex_t lock;
    int initialized;
} s_qp = {
    .lock = PTHREAD_MUTEX_INITIALIZER,
};

static void str_to_lower(char* s)
{
    for (; *s; s++)
        *s = (char)tolower((unsigned char)*s);
}

static int match_pattern(const char* pattern, const char* text)
{
    char pat[QUICK_PATH_MAX_PATTERN_LEN];
    char txt[QUICK_PATH_MAX_PATTERN_LEN];

    strncpy(pat, pattern, sizeof(pat) - 1);
    pat[sizeof(pat) - 1] = '\0';
    strncpy(txt, text, sizeof(txt) - 1);
    txt[sizeof(txt) - 1] = '\0';

    str_to_lower(pat);
    str_to_lower(txt);

    if (pat[0] == '*') {
        return (strstr(txt, pat + 1) != NULL) ? 1 : 0;
    }

    const char* pat_end = pat + strlen(pat) - 1;
    if (*pat_end == '*') {
        char prefix[QUICK_PATH_MAX_PATTERN_LEN];
        size_t plen = (size_t)(pat_end - pat);
        if (plen >= sizeof(prefix))
            plen = sizeof(prefix) - 1;
        memcpy(prefix, pat, plen);
        prefix[plen] = '\0';
        return (strncmp(txt, prefix, plen) == 0) ? 1 : 0;
    }

    return (strcmp(pat, txt) == 0) ? 1 : 0;
}

int voice_quick_path_init(void)
{
    pthread_mutex_lock(&s_qp.lock);
    memset(s_qp.rules, 0, sizeof(s_qp.rules));
    s_qp.count = 0;
    s_qp.initialized = 1;
    pthread_mutex_unlock(&s_qp.lock);

    syslog(LOG_INFO, "[%s] initialized\n", TAG);
    return 0;
}

void voice_quick_path_cleanup(void)
{
    pthread_mutex_lock(&s_qp.lock);
    s_qp.count = 0;
    s_qp.initialized = 0;
    pthread_mutex_unlock(&s_qp.lock);
    syslog(LOG_INFO, "[%s] cleaned up\n", TAG);
}

int voice_quick_path_register(const voice_quick_rule_t* rule)
{
    if (!rule || !rule->pattern || !rule->response)
        return -EINVAL;

    pthread_mutex_lock(&s_qp.lock);
    if (s_qp.count >= QUICK_PATH_MAX_RULES) {
        pthread_mutex_unlock(&s_qp.lock);
        syslog(LOG_WARNING, "[%s] rule table full\n", TAG);
        return -ENOMEM;
    }

    quick_rule_entry_t* e = &s_qp.rules[s_qp.count];
    strncpy(e->pattern, rule->pattern, sizeof(e->pattern) - 1);
    e->pattern[sizeof(e->pattern) - 1] = '\0';
    strncpy(e->response, rule->response, sizeof(e->response) - 1);
    e->response[sizeof(e->response) - 1] = '\0';
    e->valid = 1;
    s_qp.count++;

    pthread_mutex_unlock(&s_qp.lock);
    syslog(LOG_INFO, "[%s] registered: \"%s\" -> \"%s\"\n",
        TAG, rule->pattern, rule->response);
    return 0;
}

int voice_quick_path_match(const char* text,
    quick_path_tts_cb tts_cb, void* user_data)
{
    if (!text)
        return -EINVAL;

    pthread_mutex_lock(&s_qp.lock);
    if (!s_qp.initialized) {
        pthread_mutex_unlock(&s_qp.lock);
        return -ENOENT;
    }

    int found = -ENOENT;
    for (int i = 0; i < s_qp.count; i++) {
        if (!s_qp.rules[i].valid)
            continue;
        if (match_pattern(s_qp.rules[i].pattern, text)) {
            found = 0;
            syslog(LOG_INFO, "[%s] matched: \"%s\" -> \"%s\"\n",
                TAG, text, s_qp.rules[i].response);

            if (tts_cb) {
                tts_cb((const unsigned char*)s_qp.rules[i].response,
                    strlen(s_qp.rules[i].response), 1, user_data);
            }
            break;
        }
    }

    pthread_mutex_unlock(&s_qp.lock);
    return found;
}

int voice_quick_path_match_text(const char* text,
    char* resp_out, size_t resp_cap)
{
    if (!text || !resp_out || resp_cap == 0)
        return -EINVAL;

    pthread_mutex_lock(&s_qp.lock);
    if (!s_qp.initialized) {
        pthread_mutex_unlock(&s_qp.lock);
        return -ENOENT;
    }

    int found = -ENOENT;
    for (int i = 0; i < s_qp.count; i++) {
        if (!s_qp.rules[i].valid)
            continue;
        if (match_pattern(s_qp.rules[i].pattern, text)) {
            strncpy(resp_out, s_qp.rules[i].response, resp_cap - 1);
            resp_out[resp_cap - 1] = '\0';
            found = 0;
            syslog(LOG_INFO, "[%s] text matched: \"%s\" -> \"%s\"\n",
                TAG, text, resp_out);
            break;
        }
    }

    pthread_mutex_unlock(&s_qp.lock);
    return found;
}
