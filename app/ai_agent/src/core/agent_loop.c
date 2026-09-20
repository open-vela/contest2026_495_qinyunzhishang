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

#include <inttypes.h>

#include "core/agent_loop.h"
#include "core/agent_mem.h"
#include "core/agent_trace.h"
#include "core/context_builder.h"
#include "core/message_bus.h"
#include "core/session_mgr.h"
#include "llm/llm_cache.h"
#include "llm/llm_proxy.h"
#include "llm/llm_router.h"
#include "llm/llm_stream.h"
#include "voice/voice_quick_path.h"
#include "voice/voice_channel.h"
#include "tools/skill_loader.h"
#include "tools/tool_guard.h"
#include "tools/tool_registry.h"
#include "agent_compat.h"
#include "agent_config.h"

#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/time.h>

#include "cJSON.h"

/* Weak reference to voice_assistant_reset_feishu_fast_path().
 * When lvgldemo is linked, the strong definition from voice_assistant.c
 * overrides this.  In standalone ai_agent builds, the pointer is NULL
 * and the call is skipped.  This avoids a hard dependency from the
 * ai_agent package on the lvgldemo application. */
extern void voice_assistant_reset_feishu_fast_path(void)
    __attribute__((weak));

/* Weak references to feishu conversation mode functions */
extern void voice_assistant_enter_feishu_conversation(const char *chat_id)
    __attribute__((weak));
extern void voice_assistant_exit_feishu_conversation(void)
    __attribute__((weak));

/* Weak reference to feishu conversation exit intent matcher.
 * Implemented in voice_assistant.c; returns true if text matches
 * a feishu-specific exit phrase (fuzzy match). */
extern bool va_is_feishu_conv_exit_intent(const char *text)
    __attribute__((weak));

static const char* TAG = "agent";

#define TOOL_OUTPUT_SIZE (8 * 1024)
#define TOOL_OUTPUT_SIZE_LARGE (16 * 1024)
#define TOOL_OUTPUT_SIZE_MIN (2 * 1024)

/* ── Forward declarations ──────────────────────────────────── */

static char* handle_slash_note(const agent_msg_t* msg);
static char* handle_slash_remind(const agent_msg_t* msg);
static bool llm_call_timed_out(uint32_t latency_ms);

/* ── Timeout message constants ─────────────────────────────── */

#define LLM_TIMEOUT_MSG \
    "请求超时，LLM 响应时间过长。请稍后重试，或尝试简化你的问题。"
#define LLM_TIMEOUT_TASK_COMPLETE_MSG \
    "任务已完成，但生成确认消息超时。"

/* ── Clock-safe elapsed time calculation ───────────────────── */

static inline uint32_t calc_elapsed_ms(const struct timeval* t0,
    const struct timeval* t1)
{
    int32_t sec_diff = (int32_t)(t1->tv_sec - t0->tv_sec);
    int32_t usec_diff = (int32_t)(t1->tv_usec - t0->tv_usec);

    /* Clock went backwards (NTP jump, manual adjustment) */
    if (sec_diff < 0) {
        syslog(LOG_WARNING, "[%s] Clock went backwards, ignoring\n", TAG);
        return 0;
    }

    /* Microsecond borrow */
    if (usec_diff < 0) {
        sec_diff--;
        usec_diff += 1000000;
    }

    if (sec_diff < 0) {
        return 0;
    }

    return (uint32_t)sec_diff * 1000 + (uint32_t)usec_diff / 1000;
}

/* ── Memory pool (pre-allocated tool output buffers) ───────── */

static agent_mem_pool_t s_tool_pool;
static bool s_pool_ready = false;

/* Build OpenAI-format assistant message with tool_calls at the top level */
static void add_assistant_message(cJSON* messages, const llm_response_t* resp)
{
    cJSON* asst_msg = cJSON_CreateObject();
    cJSON_AddStringToObject(asst_msg, "role", "assistant");

    if (resp->text && resp->text_len > 0) {
        cJSON_AddStringToObject(asst_msg, "content", resp->text);
    } else {
        cJSON_AddNullToObject(asst_msg, "content");
    }

    /* Kimi thinking mode: echo back reasoning_content or the API returns 400 */
    if (resp->reasoning_content && resp->reasoning_content[0]) {
        cJSON_AddStringToObject(asst_msg, "reasoning_content",
            resp->reasoning_content);
    }

    if (resp->call_count > 0) {
        cJSON* tool_calls = cJSON_CreateArray();
        for (int i = 0; i < resp->call_count; i++) {
            const llm_tool_call_t* call = &resp->calls[i];
            cJSON* tc = cJSON_CreateObject();
            cJSON_AddStringToObject(tc, "id", call->id);
            cJSON_AddStringToObject(tc, "type", "function");

            cJSON* func_obj = cJSON_CreateObject();
            cJSON_AddStringToObject(func_obj, "name", call->name);
            cJSON_AddStringToObject(func_obj, "arguments",
                call->input ? call->input : "{}");
            cJSON_AddItemToObject(tc, "function", func_obj);
            cJSON_AddItemToArray(tool_calls, tc);
        }
        cJSON_AddItemToObject(asst_msg, "tool_calls", tool_calls);
    }

    cJSON_AddItemToArray(messages, asst_msg);
}

/* Auto-inject channel/chat_id into cron_add input JSON. */
static char* inject_cron_context(const char* tool_name,
    const char* input_json, const char* channel, const char* chat_id)
{
    if (strcmp(tool_name, "cron_add") != 0) {
        return NULL;
    }
    if (!channel || !channel[0] || !chat_id || !chat_id[0]) {
        return NULL;
    }

    cJSON* root = cJSON_Parse(input_json);
    if (!root) {
        return NULL;
    }

    cJSON_DeleteItemFromObject(root, "channel");
    cJSON_DeleteItemFromObject(root, "chat_id");
    cJSON_AddStringToObject(root, "channel", channel);
    cJSON_AddStringToObject(root, "chat_id", chat_id);

    char* patched = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return patched;
}

/* Parallel tool execution context */
typedef struct {
    const llm_tool_call_t* call;
    char* output;
    size_t output_size;
    bool from_pool;
    char channel[16];
    char chat_id[64];
} tool_task_t;

static void* tool_exec_thread(void* arg)
{
    tool_task_t* t = (tool_task_t*)arg;

    t->output[0] = '\0';
    char* patched = inject_cron_context(
        t->call->name, t->call->input, t->channel, t->chat_id);
    agent_tool_exec_streamed(t->call->name,
        patched ? patched : t->call->input,
        t->output, t->output_size);
    free(patched);
    syslog(LOG_INFO, "[%s] Tool %s result: %d bytes\n",
        TAG, t->call->name, (int)strlen(t->output));
    return NULL;
}

/* Add one role:"tool" message per tool result (OpenAI format).
 * When call_count > 1, all tools run in parallel threads.
 * Uses memory pool for parallel buffers, with heap fallback. */
static void add_tool_result_messages(cJSON* messages,
    const llm_response_t* resp, char* tool_output,
    size_t tool_output_size, const char* msg_channel,
    const char* msg_chat_id)
{
    int n = resp->call_count;

    if (n == 1) {
        const llm_tool_call_t* call = &resp->calls[0];

        syslog(LOG_INFO, "[%s] Tool call: %s args=%.500s\n", TAG,
            call->name, call->input ? call->input : "(null)");
        tool_output[0] = '\0';
        char* patched = inject_cron_context(
            call->name, call->input, msg_channel, msg_chat_id);
        agent_tool_exec_streamed(call->name,
            patched ? patched : call->input,
            tool_output, tool_output_size);
        free(patched);
        syslog(LOG_INFO, "[%s] Tool %s result: %d bytes\n", TAG,
            call->name, (int)strlen(tool_output));

        cJSON* result_msg = cJSON_CreateObject();
        cJSON_AddStringToObject(result_msg, "role", "tool");
        cJSON_AddStringToObject(result_msg, "tool_call_id", call->id);
        cJSON_AddStringToObject(result_msg, "content", tool_output);
        cJSON_AddItemToArray(messages, result_msg);
        return;
    }

    /* Parallel path */
    tool_task_t tasks[AGENT_MAX_TOOL_CALLS];
    pthread_t threads[AGENT_MAX_TOOL_CALLS];
    size_t par_buf_size = agent_mem_safe_size(
        TOOL_OUTPUT_SIZE_LARGE, TOOL_OUTPUT_SIZE_MIN);

    for (int i = 0; i < n; i++) {
        tasks[i].call = &resp->calls[i];
        strncpy(tasks[i].channel, msg_channel ? msg_channel : "",
            sizeof(tasks[i].channel) - 1);
        strncpy(tasks[i].chat_id, msg_chat_id ? msg_chat_id : "",
            sizeof(tasks[i].chat_id) - 1);

        char* buf = s_pool_ready
            ? agent_pool_acquire(&s_tool_pool)
            : NULL;
        if (buf) {
            tasks[i].output = buf;
            tasks[i].output_size = s_tool_pool.buf_size;
            tasks[i].from_pool = true;
        } else {
            tasks[i].output = calloc(1, par_buf_size);
            tasks[i].output_size = par_buf_size;
            tasks[i].from_pool = false;
        }

        if (!tasks[i].output) {
            threads[i] = 0;
            continue;
        }

        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, 16 * 1024);
        if (pthread_create(&threads[i], &attr,
                tool_exec_thread, &tasks[i])
            != 0) {
            syslog(LOG_ERR, "[%s] Failed to spawn thread for tool %s\n",
                TAG, resp->calls[i].name);
            threads[i] = 0;
        }
        pthread_attr_destroy(&attr);
    }

    /* Join and collect results, dedup by tool_call_id */
    char seen_ids[AGENT_MAX_TOOL_CALLS][32];
    int seen_count = 0;

    for (int i = 0; i < n; i++) {
        if (threads[i]) {
            pthread_join(threads[i], NULL);
        }

        bool dup = false;
        for (int j = 0; j < seen_count; j++) {
            if (strcmp(seen_ids[j], resp->calls[i].id) == 0) {
                dup = true;
                break;
            }
        }

        if (dup) {
            syslog(LOG_WARNING,
                "[%s] Skipping duplicate tool_call_id: %s\n",
                TAG, resp->calls[i].id);
        } else {
            strncpy(seen_ids[seen_count++], resp->calls[i].id,
                sizeof(seen_ids[0]) - 1);

            cJSON* result_msg = cJSON_CreateObject();
            cJSON_AddStringToObject(result_msg, "role", "tool");
            cJSON_AddStringToObject(result_msg, "tool_call_id",
                resp->calls[i].id);
            cJSON_AddStringToObject(result_msg, "content",
                (tasks[i].output && tasks[i].output[0])
                    ? tasks[i].output
                    : "{}");
            cJSON_AddItemToArray(messages, result_msg);
        }

        if (tasks[i].from_pool) {
            agent_pool_release(&s_tool_pool, tasks[i].output);
        } else {
            free(tasks[i].output);
        }
    }
}

/* ── Fast path counters ─────────────────────────────────── */

static int s_fast_path_count = 0;
static int s_llm_path_count = 0;

/* ── Natural language fast path (keyword → direct tool call) ── */

static bool contains_any(const char* text, const char** keywords)
{
    for (int i = 0; keywords[i]; i++) {
        if (strcasestr(text, keywords[i])) {
            return true;
        }
    }
    return false;
}

/* Fuzzy match "chat mode" and common ASR homophones.
 * Cloud ASR often mis-recognizes the short word "chat" /tʃæt/ as phonetically
 * similar words. This function detects "chat mode" or its homophone
 * combinations; must be combined with an action word to avoid false triggers.
 * Note: only selects homophones with low substring conflict, avoiding short
 * words like at/get/let that match many common words. */
static bool fuzzy_match_chat_mode(const char* text)
{
    if (!text) return false;
    if (!strcasestr(text, "mode")) return false;
    if (strcasestr(text, "chat")) return true;    /* /tʃæt/ correct */
    /* ASR homophones (ordered by phonetic similarity) */
    if (strcasestr(text, "check")) return true;   /* /tʃɛk/ vowel相近 */
    if (strcasestr(text, "catch")) return true;   /* /kætʃ/ consonant-vowel inverted */
    if (strcasestr(text, "chet")) return true;    /* /tʃɛt/ vowel相近 */
    if (strcasestr(text, "chart")) return true;   /* /tʃɑrt/ extra r */
    if (strcasestr(text, "charter")) return true; /* /tʃɑrtər/ chart + er */
    if (strcasestr(text, "chad")) return true;    /* /tʃæd/ final consonant differ */
    if (strcasestr(text, "that")) return true;    /* /ðæt/ same vowel */
    if (strcasestr(text, "jet")) return true;     /* /dʒɛt/ dʒ replaces tʃ */
    if (strcasestr(text, "shut")) return true;    /* /ʃʌt/ ʃ replaces tʃ */
    if (strcasestr(text, "shot")) return true;    /* /ʃɒt/ ʃ replaces tʃ */
    return false;
}

/* Detect if text is predominantly English (no CJK characters).
 * Used to choose TTS reply language for bilingual fast-path responses. */
static bool is_english_text(const char* text)
{
    if (!text) return false;
    for (const char* p = text; *p; p++) {
        /* CJK Unified Ideographs: U+4E00–U+9FFF
         * UTF-8 encoding: E4 B8 80 .. E9 BF BF → leading byte E4..E9 */
        unsigned char c = (unsigned char)*p;
        if (c >= 0xE4 && c <= 0xE9) {
            return false;
        }
    }
    return true;
}

/* Align an offset backwards to the start of a UTF-8 character.
 * If pos falls in the middle of a multi-byte sequence, move it
 * forward to the next character boundary. */
static int utf8_align_start(const char* s, int pos)
{
    while (pos > 0) {
        unsigned char c = (unsigned char)s[pos];
        if ((c & 0xC0) != 0x80)
            break; /* Start of a UTF-8 character */
        pos--;
    }
    return pos;
}

/* Align an offset forwards to the start of a UTF-8 character.
 * If pos falls in the middle of a multi-byte sequence, move it
 * forward to the next character boundary. */
static int utf8_align_end(const char* s, int len, int pos)
{
    while (pos < len) {
        unsigned char c = (unsigned char)s[pos];
        if ((c & 0xC0) != 0x80)
            break; /* Start of a UTF-8 character */
        pos++;
    }
    return pos;
}

/* 模糊匹配关键词：适应语音/ASR 识别误差（字符偏差、多余/缺失字符、
 * 中英文混排、标点干扰）。策略按精度从高到低依次尝试：
 *   1. 直接子串匹配（大小写不敏感）
 *   2. 忽略 haystack/needle 中所有空格后再匹配
 *   2.5. 忽略标点符号后匹配（覆盖 "沁园春雪" vs "沁园春·雪"）
 *   3. 按空格分割为多 token，过半匹配即视为命中
 *   4. 单 token 长度 >=3 时允许 1 字符偏差（首/尾各去 1 字符后匹配）
 *   5. needle 作为 haystack 子串的"包含"匹配，反之亦然
 */

/* 获取 UTF-8 字符的字节数 */
static int utf8_nbytes(const char* p)
{
    unsigned char c = (unsigned char)*p;
    if ((c & 0x80) == 0x00) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;
}

/* 判断 UTF-8 字符是否为标点/分隔符（需跳过的字符） */
static int utf8_is_punct(const char* p, int cb)
{
    if (cb == 1) {
        unsigned char c = (unsigned char)*p;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9'))
            return 0;
        return 1;
    }
    if (cb == 2) {
        unsigned int cp = ((unsigned char)p[0] & 0x1F) << 6
            | ((unsigned char)p[1] & 0x3F);
        if (cp == 0xB7) return 1; /* 间隔点 · */
        return 0;
    }
    if (cb == 3) {
        unsigned int cp = ((unsigned char)p[0] & 0x0F) << 12
            | ((unsigned char)p[1] & 0x3F) << 6
            | ((unsigned char)p[2] & 0x3F);
        if ((cp >= 0x3000 && cp <= 0x303F) ||
            (cp >= 0xFF00 && cp <= 0xFFEF) ||
            (cp >= 0x2000 && cp <= 0x206F))
            return 1;
        return 0;
    }
    return 0;
}

/* 在 haystack 中搜索 needle，忽略两者中的标点符号 */
static char* strcasestr_nopunct(const char* haystack, const char* needle)
{
    if (!haystack || !needle || !*needle) return NULL;

    char clean_ndl[256];
    int cnl = 0;
    for (const char* p = needle; *p && cnl < (int)sizeof(clean_ndl) - 4; ) {
        int cb = utf8_nbytes(p);
        if (!utf8_is_punct(p, cb)) {
            memcpy(clean_ndl + cnl, p, cb);
            cnl += cb;
        }
        p += cb;
    }
    clean_ndl[cnl] = '\0';
    if (cnl == 0) return NULL;

    for (const char* h = haystack; *h; ) {
        const char* hp = h;
        const char* np = clean_ndl;
        int match = 1;

        while (*hp && *np) {
            int hcb = utf8_nbytes(hp);
            if (utf8_is_punct(hp, hcb)) { hp += hcb; continue; }
            int ncb = utf8_nbytes(np);
            if (hcb != ncb) { match = 0; break; }
            if (strncasecmp(hp, np, hcb) != 0) { match = 0; break; }
            hp += hcb;
            np += ncb;
        }
        if (match) {
            while (*np && utf8_is_punct(np, utf8_nbytes(np)))
                np += utf8_nbytes(np);
            if (*np == '\0') return (char*)h;
        }
        h += utf8_nbytes(h);
    }
    return NULL;
}

static char* fuzzy_search(const char* haystack, const char* needle)
{
    if (!haystack || !needle || !*needle) return NULL;

    /* 1. 直接子串匹配（大小写不敏感） */
    char* hit = strcasestr(haystack, needle);
    if (hit) return hit;

    /* Helper: 去除空格压缩后匹配 */
    #define MATCH_COMPACT(hay, ndl) ({                                       \
        char _hb[512], _nb[256];                                            \
        int _hi = 0, _ni = 0;                                               \
        for (const char* _p = (hay); *_p && _hi < (int)sizeof(_hb)-1; _p++) \
            if (*_p != ' ' && *_p != '\t') _hb[_hi++] = *_p;               \
        _hb[_hi] = '\0';                                                    \
        for (const char* _p = (ndl); *_p && _ni < (int)sizeof(_nb)-1; _p++) \
            if (*_p != ' ' && *_p != '\t') _nb[_ni++] = *_p;               \
        _nb[_ni] = '\0';                                                    \
        (_ni > 0) ? strcasestr(_hb, _nb) : NULL;                            \
    })

    /* 2. 忽略所有空格后匹配 */
    char* compact_hit = MATCH_COMPACT(haystack, needle);
    if (compact_hit) return compact_hit;

    /* 2.5. 忽略标点符号后匹配（覆盖 "沁园春雪" vs "沁园春·雪"） */
    char* nopunct_hit = strcasestr_nopunct(haystack, needle);
    if (nopunct_hit) return nopunct_hit;

    /* 3. 按空格分割为多 token，部分匹配 */
    {
        char buf[256];
        strncpy(buf, needle, sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';

        char* save = NULL;
        char* tok = strtok_r(buf, " \t", &save);
        int total = 0, matched = 0;
        char* first_hit = NULL;
        while (tok) {
            total++;
            char* h = strcasestr(haystack, tok);
            if (h) {
                matched++;
                if (!first_hit) first_hit = h;
            }
            tok = strtok_r(NULL, " \t", &save);
        }
        /* 多 token 时，过半匹配即视为命中 */
        if (total > 1 && matched * 2 >= total) return first_hit;

        /* 4. 单 token 长度 >=4 bytes 时，允许首/尾各偏差 1 字符
         *    覆盖 ASR 常见误差（多/少 1 字符）。
         *    注意：必须按 UTF-8 字符边界操作，否则截断多字节字符
         *    会导致 strcasestr 匹配失败。 */
        if (total <= 1) {
            int nl = (int)strlen(needle);
            if (nl >= 4) {
                int first_cb = utf8_nbytes(needle);
                /* 去掉首字符（按UTF-8字符边界） */
                if (nl > first_cb) {
                    hit = strcasestr(haystack, needle + first_cb);
                    if (hit) return hit;
                }
                /* 去掉尾字符（按UTF-8字符边界） */
                if (nl > first_cb) {
                    int pos = 0, last_start = 0;
                    while (pos < nl) {
                        last_start = pos;
                        pos += utf8_nbytes(needle + pos);
                    }
                    if (last_start > 0) {
                        char tmp[256];
                        memcpy(tmp, needle, last_start);
                        tmp[last_start] = '\0';
                        hit = strcasestr(haystack, tmp);
                        if (hit) return hit;
                    }
                }
                /* haystack 去掉首字符后再匹配（应对 haystack 首 1 字符为噪声） */
                if (nl >= 4 && haystack[0]) {
                    int hcb = utf8_nbytes(haystack);
                    if (haystack[hcb]) {
                        hit = strcasestr(haystack + hcb, needle);
                        if (hit) return hit;
                    }
                }
            }
        }
    }

    /* 5. 包含匹配：haystack 包含 needle 或 needle 包含 haystack 的核心部分。
     *    注意：按 UTF-8 字符边界操作。 */
    {
        int hl = (int)strlen(haystack);
        int nl = (int)strlen(needle);
        if (nl > 0 && nl < hl) {
            int first_cb = utf8_nbytes(needle);
            int pos = first_cb, last_start = first_cb;
            while (pos < nl) {
                last_start = pos;
                pos += utf8_nbytes(needle + pos);
            }
            int core_len = last_start - first_cb;
            /* needle 去掉首尾各 1 字符后作为子串（长度 >=3 bytes 时） */
            if (core_len >= 3) {
                char core[256];
                memcpy(core, needle + first_cb, core_len);
                core[core_len] = '\0';
                hit = strcasestr(haystack, core);
                if (hit) return hit;
            }
        }
    }

    #undef MATCH_COMPACT
    return NULL;
}

/* Intent table: each entry maps keywords → tool + args + output size */

typedef struct {
    const char** keywords;
    const char* tool_name;
    const char* tool_args;
    size_t output_size;
} nl_intent_t;

static const char* kw_time[]       = { "几点了", "什么时间", "what time", "几点钟", NULL };
static const char* kw_battery[]    = { "电量多少", "电池电量", "battery", NULL };
static const char* kw_heartrate[]  = { "心率多少", "heart rate", "心率是多", NULL };
static const char* kw_steps[]      = { "走了多少步", "今天走了多少", "steps", "步数", NULL };
static const char* kw_pause[]      = { "暂停音乐", "暂停播放", "pause music", NULL };
static const char* kw_stop[]       = { "停止播放", "停止音乐", "stop music", NULL };
static const char* kw_resume[]     = { "继续播放", "resume", NULL };

static const nl_intent_t s_intents[] = {
    { kw_time,      "get_current_time", "{}",  256  },
    { kw_battery,   "get_battery",      "{}",  512  },
    { kw_heartrate, "get_heartrate",    "{}",  256  },
    { kw_steps,     "get_steps",        "{}",  256  },
    { kw_pause,     "music_pause",      "{}",  256  },
    { kw_stop,      "music_stop",       "{}",  256  },
    { kw_resume,    "music_resume",     "{}",  256  },
    { NULL,         NULL,               NULL,  0    },
};

/* sanitize_doc_content: shared with tool_feishu_doc.c */
extern size_t sanitize_doc_content(char *s);

static char* handle_nl_fast_path(const char* text)
{
    /* Table-driven: scan intents, first match wins */
    for (int i = 0; s_intents[i].keywords; i++) {
        if (contains_any(text, s_intents[i].keywords)) {
            char* reply = calloc(1, s_intents[i].output_size);
            if (reply) {
                tool_registry_execute(s_intents[i].tool_name,
                    s_intents[i].tool_args, reply,
                    s_intents[i].output_size);
            }
            if (reply) {
                s_fast_path_count++;
                syslog(LOG_INFO,
                    "[%s] NL fast path: %s (fast=%d llm=%d)\n",
                    TAG, s_intents[i].tool_name,
                    s_fast_path_count, s_llm_path_count);
            }
            return reply;
        }
    }

    /* Skill list (special: no tool call) */
    static const char* kw_skill[] = {
        "技能列表", "有什么技能", "list skill", NULL };
    if (contains_any(text, kw_skill)) {
        char buf[2048];
        size_t n = skill_loader_build_summary(buf, sizeof(buf));
        s_fast_path_count++;
        return n > 0 ? strdup(buf) : strdup("暂无已加载的技能。");
    }

    /* Weather (special: needs city extraction) */
    static const char* kw_weather[] = {
        "天气怎么样", "weather", "天气如何", NULL };
    if (contains_any(text, kw_weather)) {
        char safe_city[64];
        strncpy(safe_city, "Beijing", sizeof(safe_city) - 1);
        safe_city[sizeof(safe_city) - 1] = '\0';

        const char* p = strstr(text, "天气");
        if (p && p > text && (size_t)(p - text) < sizeof(safe_city) - 1) {
            int ci = 0;
            for (const char* s = text; s < p && ci < 62; s++) {
                if (*s != '"' && *s != '\\' && *s != ' ')
                    safe_city[ci++] = *s;
            }
            if (ci > 0) safe_city[ci] = '\0';
        }

        char input[256];
        snprintf(input, sizeof(input),
            "{\"location\":\"%s\"}", safe_city);
        char* reply = calloc(1, 4096);
        if (reply)
            tool_registry_execute("get_weather", input, reply, 4096);
        if (reply) {
            s_fast_path_count++;
            syslog(LOG_INFO,
                "[%s] NL fast path: get_weather (fast=%d llm=%d)\n",
                TAG, s_fast_path_count, s_llm_path_count);
        }
        return reply;
    }

    /* Music play (special: needs keyword extraction) */
    static const char* kw_play[] = { "播放", "play ", "放一首", NULL };
    if (contains_any(text, kw_play)) {
        const char* kw = NULL;
        const char* p = strstr(text, "播放");
        if (p) { p += strlen("播放"); while (*p == ' ') p++; if (*p) kw = p; }
        if (!kw) {
            p = strcasestr(text, "play ");
            if (p) { p += 5; while (*p == ' ') p++; if (*p) kw = p; }
        }
        if (kw) {
            int klen = 0;
            while (kw[klen] && kw[klen] != '"' && kw[klen] != '\\' && klen < 100)
                klen++;
            char input[256];
            snprintf(input, sizeof(input),
                "{\"keyword\":\"%.*s\"}", klen, kw);
            char search_result[4096];
            memset(search_result, 0, sizeof(search_result));
            tool_registry_execute("music_search", input, search_result, sizeof(search_result));

            /* Auto-play first result if search succeeded */
            char* reply = NULL;
            cJSON* sr = cJSON_Parse(search_result);
            cJSON* songs = sr ? cJSON_GetObjectItem(sr, "songs") : NULL;
            cJSON* first = songs ? cJSON_GetArrayItem(songs, 0) : NULL;
            cJSON* url = first ? cJSON_GetObjectItem(first, "url") : NULL;
            if (url && cJSON_IsString(url) && url->valuestring[0]) {
                char play_input[512];
                snprintf(play_input, sizeof(play_input),
                    "{\"url\":\"%s\"}", url->valuestring);
                char play_result[256];
                memset(play_result, 0, sizeof(play_result));
                tool_registry_execute("music_play", play_input, play_result, sizeof(play_result));

                /* Build reply with song info */
                cJSON* name = cJSON_GetObjectItem(first, "name");
                cJSON* artist = cJSON_GetObjectItem(first, "artist");
                const char* sname = (name && cJSON_IsString(name)) ? name->valuestring : "未知";
                const char* sartist = (artist && cJSON_IsString(artist)) ? artist->valuestring : "未知";

                /* Check if play actually succeeded */
                bool play_ok = (strstr(play_result, "error") == NULL && play_result[0] != '\0');
                reply = calloc(1, 512);
                if (reply) {
                    if (play_ok) {
                        snprintf(reply, 512, "正在播放: %s - %s", sname, sartist);
                    } else {
                        snprintf(reply, 512, "找到了 %s - %s，但播放失败: %s",
                            sname, sartist, play_result);
                    }
                }
            } else {
                reply = calloc(1, 4096);
                if (reply)
                    strncpy(reply, search_result, 4095);
            }
            cJSON_Delete(sr);

            if (reply) {
                s_fast_path_count++;
                syslog(LOG_INFO,
                    "[%s] NL fast path: music_search (fast=%d llm=%d)\n",
                    TAG, s_fast_path_count, s_llm_path_count);
            }
            return reply;
        }
    }

    /* ── Feishu conversation mode: exit command check (before enter matching) ──
     * 模糊匹配飞书专属退出短语，与 voice_assistant.c 的 va_is_feishu_conv_exit_intent()
     * 保持一致：先精确匹配完整短语，再用动作词+对象词组合匹配。 */
    if (text && va_is_feishu_conv_exit_intent(text)) {
        syslog(LOG_INFO, "[%s] [FEISHU_CONV] Layer2 exit command detected, performing exit\n", TAG);
        /* 确保实际退出动作已执行（voice_assistant层可能已经退出，此处为安全保底） */
        if (voice_assistant_exit_feishu_conversation) {
            voice_assistant_exit_feishu_conversation();
        }
        return strdup(is_english_text(text)
            ? "Exited Lark conversation mode"
            : "已退出飞书对话模式");
    }

    /* ── Feishu conversation mode: enter persistent bidirectional chat ── */
    static const char* kw_conv[] = {
        "开始飞书对话", "进入飞书对话", "飞书对话模式",
        "开始飞书聊天", "飞书聊天模式", "进入飞书聊天",
        "start chat mode", "enter chat mode",
        "begin chat mode", "open chat mode",
        "start conversation mode", "enter conversation mode",
        "begin conversation mode", "open conversation mode",
        NULL
    };
    if (contains_any(text, kw_conv) ||
        /* chat mode 模糊匹配：容错 ASR 将 chat 识别为 check/catch 等近音词 */
        (contains_any(text, (const char*[]){"start", "enter", "begin", "open", NULL}) &&
         fuzzy_match_chat_mode(text))) {
        syslog(LOG_INFO, "[%s] [FEISHU_CONV] Layer2 conversation intent matched, text=\"%.200s\"\n",
               TAG, text);

        /* Step 1: Get chat_id (search for default chat) */
        char chat_id[64] = {0};
        int search_ret = feishu_search_chats("", chat_id, sizeof(chat_id));
        if (search_ret != OK || !chat_id[0]) {
            /* Fallback: try to list chats and use the first one */
            char list_buf[4096] = {0};
            int lst_ret = feishu_api_request("GET",
                "/open-apis/im/v1/chats?page_size=5&user_id_type=open_id",
                NULL, 0, list_buf, sizeof(list_buf));
            if (lst_ret == 200) {
                cJSON *root = cJSON_Parse(list_buf);
                cJSON *code = root ? cJSON_GetObjectItem(root, "code") : NULL;
                if (cJSON_IsNumber(code) && code->valueint == 0) {
                    cJSON *data = cJSON_GetObjectItem(root, "data");
                    cJSON *items = data ? cJSON_GetObjectItem(data, "items") : NULL;
                    if (cJSON_IsArray(items) && cJSON_GetArraySize(items) > 0) {
                        cJSON *first = cJSON_GetArrayItem(items, 0);
                        cJSON *cid = cJSON_GetObjectItem(first, "chat_id");
                        if (cJSON_IsString(cid)) {
                            strncpy(chat_id, cid->valuestring, sizeof(chat_id) - 1);
                        }
                    }
                }
                cJSON_Delete(root);
            }
        }

        if (!chat_id[0]) {
            syslog(LOG_ERR, "[%s] [FEISHU_CONV] failed to get chat_id\n", TAG);
            s_fast_path_count++;
            return strdup(is_english_text(text)
                ? "Failed to get Lark chat ID, please check Lark configuration"
                : "无法获取飞书群聊ID，请检查飞书配置");
        }

        syslog(LOG_INFO, "[%s] [FEISHU_CONV] chat_id=%s\n", TAG, chat_id);

        /* Step 2: Get and cache chat members */
        char members_input[128];
        snprintf(members_input, sizeof(members_input),
                 "{\"chat_id\":\"%s\"}", chat_id);
        char members_result[8192] = {0};
        tool_registry_execute("feishu_chat_members", members_input,
                              members_result, sizeof(members_result));
        if (members_result[0] && !strstr(members_result, "Error")) {
            feishu_recv_cache_members(members_result);
            syslog(LOG_INFO, "[%s] [FEISHU_CONV] members cached\n", TAG);
        }

        /* Step 3: Enter conversation mode */
        if (voice_assistant_enter_feishu_conversation) {
            voice_assistant_enter_feishu_conversation(chat_id);
        }

        s_fast_path_count++;
        syslog(LOG_INFO, "[%s] NL fast path: feishu_conversation (fast=%d)\n",
               TAG, s_fast_path_count);
        return strdup(is_english_text(text)
            ? "Entered Lark conversation mode, please start speaking"
            : "已进入飞书对话模式，请开始说话");
    }

    /* Feishu document creation (special: needs title extraction)
     * Combination matching: action word AND object word must BOTH be present */
    static const char* kw_doc_action[] = {
        "创建", "新建", "帮我写", "帮我建", "写一个", "建一个",
        "写入", "写到", "写进", "保存到", "存到", "create", NULL
    };
    static const char* kw_doc_object[] = {
        "飞书文档", "飞书", "文档", "document", "doc", NULL
    };
    bool has_action = contains_any(text, kw_doc_action);
    bool has_object = contains_any(text, kw_doc_object);
    syslog(LOG_INFO, "[%s] [FEISHU_DOC] Layer2 intent check: has_action=%d, has_object=%d, text=\"%.200s\"\n",
           TAG, has_action, has_object, text ? text : "(null)");
    if (has_action && has_object) {
        bool is_en = is_english_text(text);
        const char* title_start = NULL;
        const char* title_end = NULL;
        char extracted_title[256] = {0};
        const char* extract_method = "none";

        /* Priority 1: English double quotes "..." */
        const char* q1 = strchr(text, '"');
        if (q1) {
            const char* q2 = strchr(q1 + 1, '"');
            if (q2 && q2 > q1 + 1) {
                title_start = q1 + 1;
                title_end = q2;
                extract_method = "english_quotes";
            }
        }

        /* Priority 2: Chinese double quotes ("...") */
        if (!title_start) {
            const char* lq = strstr(text, "\xe2\x80\x9c");
            if (lq) {
                const char* rq = strstr(lq + 3, "\xe2\x80\x9d");
                if (rq && rq > lq + 3) {
                    title_start = lq + 3;
                    title_end = rq;
                    extract_method = "chinese_quotes";
                }
            }
        }

        /* Priority 3: Title brackets 《...》 (书名号) */
        if (!title_start) {
            const char* lb = strstr(text, "\xe3\x80\x8a");
            if (lb) {
                const char* rb = strstr(lb + 3, "\xe3\x80\x8b");
                if (rb && rb > lb + 3) {
                    title_start = lb + 3;
                    title_end = rb;
                    extract_method = "title_brackets";
                }
            }
        }

        /* Priority 4: Corner brackets (「...」) */
        if (!title_start) {
            const char* lb = strstr(text, "\xe3\x80\x8c");
            if (lb) {
                const char* rb = strstr(lb + 3, "\xe3\x80\x8d");
                if (rb && rb > lb + 3) {
                    title_start = lb + 3;
                    title_end = rb;
                    extract_method = "corner_brackets";
                }
            }
        }

        /* Priority 5: Fallback - take text after object keyword */
        if (!title_start) {
            const char* obj_pos = NULL;
            size_t obj_len = 0;
            const char* matched_kw = NULL;
            for (int i = 0; kw_doc_object[i]; i++) {
                const char* p = strcasestr(text, kw_doc_object[i]);
                if (p) {
                    size_t len = strlen(kw_doc_object[i]);
                    if (!obj_pos || p < obj_pos) {
                        obj_pos = p;
                        obj_len = len;
                        matched_kw = kw_doc_object[i];
                    }
                }
            }
            if (obj_pos) {
                syslog(LOG_INFO, "[%s] [FEISHU_DOC] Extraction fallback after keyword \"%s\"\n", TAG, matched_kw);
                const char* p = obj_pos + obj_len;
                /* Skip UTF-8 punctuation precisely */
                while (*p) {
                    if (*p == ' ' || *p == ',' || *p == '.' || *p == ':' ||
                        *p == ';' || *p == '?' || *p == '!') {
                        p++;
                    } else if ((unsigned char)p[0] == 0xe3 && (unsigned char)p[1] == 0x80) {
                        p += 3; /* Chinese punctuation */
                    } else if ((unsigned char)p[0] == 0xef && (unsigned char)p[1] == 0xbc) {
                        p += 3; /* Full-width punctuation */
                    } else if ((unsigned char)p[0] == 0xe2 && (unsigned char)p[1] == 0x80) {
                        p += 3; /* Dash/ellipsis */
                    } else {
                        break;
                    }
                }
                title_start = p;
                title_end = text + strlen(text);
                extract_method = "fallback";
            }
        }

        syslog(LOG_INFO, "[%s] [FEISHU_DOC] Extraction method: %s\n", TAG, extract_method);

        /* Extract and clean up title */
        if (title_start && title_end && title_end > title_start) {
            size_t tlen = (size_t)(title_end - title_start);
            if (tlen >= sizeof(extracted_title)) tlen = sizeof(extracted_title) - 1;

            syslog(LOG_INFO, "[%s] [FEISHU_DOC] Raw title (%zu bytes): \"%.*s\"\n", TAG, tlen, (int)tlen, title_start);

            /* Copy while filtering JSON-unsafe characters " and \ */
            int ti = 0;
            for (size_t i = 0; i < tlen && title_start[i]; i++) {
                char c = title_start[i];
                if (c == '"' || c == '\\') continue;
                if (ti < (int)sizeof(extracted_title) - 1) {
                    extracted_title[ti++] = c;
                }
            }

            /* Strip trailing punctuation */
            while (ti > 0) {
                bool stripped = false;
                char last = extracted_title[ti - 1];
                if (last == ' ' || last == ',' || last == '.' || last == ':' ||
                    last == ';' || last == '?' || last == '!' ||
                    last == '\n' || last == '\r') {
                    ti--;
                    extracted_title[ti] = '\0';
                    stripped = true;
                } else if (ti >= 3) {
                    unsigned char* p = (unsigned char*)&extracted_title[ti - 3];
                    if ((p[0] == 0xe3 && p[1] == 0x80) ||
                        (p[0] == 0xef && p[1] == 0xbc) ||
                        (p[0] == 0xe2 && p[1] == 0x80)) {
                        ti -= 3;
                        extracted_title[ti] = '\0';
                        stripped = true;
                    }
                }
                if (!stripped) break;
            }
        }

        const char* title = extracted_title[0] ? extracted_title : "untitled";
        syslog(LOG_INFO, "[%s] [FEISHU_DOC] Final cleaned title: \"%s\"\n", TAG, title);

        /* Call tool */
        char tool_input[512];
        snprintf(tool_input, sizeof(tool_input), "{\"title\":\"%s\"}", title);
        syslog(LOG_INFO, "[%s] [FEISHU_DOC] Calling feishu_doc_create, input=%s\n", TAG, tool_input);
        char tool_result[4096];
        memset(tool_result, 0, sizeof(tool_result));
        tool_registry_execute("feishu_doc_create", tool_input, tool_result, sizeof(tool_result));
        syslog(LOG_INFO, "[%s] [FEISHU_DOC] Tool result: %.500s\n", TAG, tool_result);

        char* reply = calloc(1, 512);
        if (reply) {
            cJSON* res = cJSON_Parse(tool_result);
            cJSON* res_title = res ? cJSON_GetObjectItem(res, "title") : NULL;
            if (res_title && cJSON_IsString(res_title) && res_title->valuestring && res_title->valuestring[0]) {
                if (is_en)
                    snprintf(reply, 512, "Created Lark document: %s", res_title->valuestring);
                else
                    snprintf(reply, 512, "已为你创建飞书文档《%s》", res_title->valuestring);
            } else if (strstr(tool_result, "Error") || tool_result[0] == '\0') {
                if (is_en)
                    snprintf(reply, 512, "Failed to create Lark document, please try again later");
                else
                    snprintf(reply, 512, "飞书文档创建失败，请稍后再试");
            } else {
                if (is_en)
                    snprintf(reply, 512, "Created Lark document: %s", title);
                else
                    snprintf(reply, 512, "已为你创建飞书文档《%s》", title);
            }
            cJSON_Delete(res);

            syslog(LOG_INFO, "[%s] [FEISHU_DOC] Reply to TTS: \"%s\" (fast=%d llm=%d)\n",
                   TAG, reply, s_fast_path_count + 1, s_llm_path_count);
            s_fast_path_count++;
            syslog(LOG_INFO,
                "[%s] NL fast path: feishu_doc_create (fast=%d llm=%d)\n",
                TAG, s_fast_path_count, s_llm_path_count);
        }
        return reply;
    }

    /* ── Feishu document query: read document content by title ─────── */
    static const char* kw_query_action[] = {
        "读一下", "查看", "看一下", "打开", "读取", "查询", "看看",
        "read", "show", "view", "open", "look at", "check", "display", NULL
    };
    bool has_query_action = contains_any(text, kw_query_action);
    if (has_query_action && has_object) {
        syslog(LOG_INFO, "[%s] [FEISHU_DOC] Layer2 query intent, text=\"%.200s\"\n", TAG, text);
        bool is_en = is_english_text(text);

        /* Extract title using same priority logic as create */
        char q_title[256] = {0};
        const char* ts = NULL;
        const char* te = NULL;
        /* English quotes */
        const char* eq1 = strchr(text, '"');
        if (eq1) { const char* eq2 = strchr(eq1+1, '"'); if (eq2 && eq2 > eq1+1) { ts = eq1+1; te = eq2; } }
        /* Chinese quotes */
        if (!ts) {
            const char* lq = strstr(text, "\xe2\x80\x9c");
            if (lq) { const char* rq = strstr(lq+3, "\xe2\x80\x9d"); if (rq && rq > lq+3) { ts = lq+3; te = rq; } }
        }
        /* Corner brackets 「」 */
        if (!ts) {
            const char* lb = strstr(text, "\xe3\x80\x8c");
            if (lb) { const char* rb = strstr(lb+3, "\xe3\x80\x8d"); if (rb && rb > lb+3) { ts = lb+3; te = rb; } }
        }
        /* Title brackets 《》 (书名号) */
        if (!ts) {
            const char* lb = strstr(text, "\xe3\x80\x8a");  /* 《 */
            if (lb) { const char* rb = strstr(lb+3, "\xe3\x80\x8b"); if (rb && rb > lb+3) { ts = lb+3; te = rb; } }
        }
        /* Fallback: text after object keyword */
        if (!ts) {
            for (int i = 0; kw_doc_object[i]; i++) {
                const char* p = strcasestr(text, kw_doc_object[i]);
                if (p) {
                    size_t len = strlen(kw_doc_object[i]);
                    const char* q = p + len;
                    while (*q == ' ' || *q == ',' || *q == '.' || *q == ':' ||
                           *q == ';' || *q == '?' || *q == '!' ||
                           ((unsigned char)q[0] == 0xe3 && (unsigned char)q[1] == 0x80) ||
                           ((unsigned char)q[0] == 0xef && (unsigned char)q[1] == 0xbc)) q += 3;
                    if (*q) { ts = q; te = text + strlen(text); break; }
                }
            }
        }
        if (ts && te && te > ts) {
            size_t tl = (size_t)(te - ts);
            if (tl >= sizeof(q_title)) tl = sizeof(q_title) - 1;
            int qi = 0;
            for (size_t i = 0; i < tl && ts[i]; i++) {
                /* Filter quote, backslash, and CJK brackets 《》「」 */
                if (ts[i] == '"' || ts[i] == '\\') continue;
                if ((unsigned char)ts[i] == 0xe3 && (unsigned char)ts[i+1] == 0x80 &&
                    ((unsigned char)ts[i+2] == 0x8a || (unsigned char)ts[i+2] == 0x8b ||
                     (unsigned char)ts[i+2] == 0x8c || (unsigned char)ts[i+2] == 0x8d)) {
                    i += 2;  /* skip 3-byte CJK bracket */
                    continue;
                }
                if (qi < (int)sizeof(q_title)-1) q_title[qi++] = ts[i];
            }
            q_title[qi] = '\0';
            /* Strip trailing punctuation (ASCII + Chinese, same as create) */
            while (qi > 0) {
                char c = q_title[qi-1];
                if (c==' '||c==','||c=='.'||c==':'||c==';'||c=='?'||c=='!'||c=='\n') { q_title[--qi]='\0'; }
                else if (qi >= 3) {
                    unsigned char c0 = (unsigned char)q_title[qi-3];
                    unsigned char c1 = (unsigned char)q_title[qi-2];
                    /* Clear all CJK punctuation by prefix (covers 、。《》「」 etc.) */
                    if ((c0 == 0xE3 && c1 == 0x80) ||
                        (c0 == 0xEF && c1 == 0xBC) ||
                        (c0 == 0xE2 && c1 == 0x80)) {
                        qi -= 3; q_title[qi] = '\0';
                    } else break;
                }
                else break;
            }
        }
        syslog(LOG_INFO, "[%s] [FEISHU_DOC] Query title: \"%s\"\n", TAG, q_title);

        /* Step 1: List documents to find doc_id by title */
        char list_result[4096];
        memset(list_result, 0, sizeof(list_result));
        tool_registry_execute("feishu_doc_list", "{}", list_result, sizeof(list_result));
        syslog(LOG_INFO, "[%s] [FEISHU_DOC] List result: %.500s\n", TAG, list_result);

        /* Parse list to find matching doc_id */
        char doc_id[128] = {0};
        cJSON* list_json = cJSON_Parse(list_result);
        if (list_json && cJSON_IsArray(list_json)) {
            int arr_size = cJSON_GetArraySize(list_json);
            for (int i = 0; i < arr_size; i++) {
                cJSON* item = cJSON_GetArrayItem(list_json, i);
                cJSON* name = cJSON_GetObjectItem(item, "name");
                cJSON* token = cJSON_GetObjectItem(item, "token");
                cJSON* type = cJSON_GetObjectItem(item, "type");
                if (cJSON_IsString(name) && cJSON_IsString(token) && name->valuestring) {
                    /* Match by title (fuzzy: supports multi-token partial match) */
                    if ((q_title[0] && fuzzy_search(name->valuestring, q_title)) ||
                        (q_title[0] == '\0' && i == 0)) {
                        /* Only docx type */
                        if (!type || (cJSON_IsString(type) && strcmp(type->valuestring, "docx") == 0)) {
                            strncpy(doc_id, token->valuestring, sizeof(doc_id)-1);
                            break;
                        }
                    }
                }
            }
        }
        cJSON_Delete(list_json);

        if (doc_id[0] == '\0') {
            /* Fallback: use q_title as keyword to search document content */
            syslog(LOG_INFO, "[%s] [FEISHU_DOC] No title match, fallback to content search: \"%s\"\n", TAG, q_title);
            char* reply = calloc(1, 1024);
            int match_count = 0;
            cJSON* s_json = cJSON_Parse(list_result);
            if (s_json && cJSON_IsArray(s_json) && q_title[0]) {
                int arr_size = cJSON_GetArraySize(s_json);
                for (int i = 0; i < arr_size && match_count < 3; i++) {
                    cJSON* item = cJSON_GetArrayItem(s_json, i);
                    cJSON* name = cJSON_GetObjectItem(item, "name");
                    cJSON* token = cJSON_GetObjectItem(item, "token");
                    cJSON* type = cJSON_GetObjectItem(item, "type");
                    if (cJSON_IsString(token) && cJSON_IsString(name) &&
                        (!type || (cJSON_IsString(type) && strcmp(type->valuestring, "docx") == 0))) {
                        char rd_input[512];
                        snprintf(rd_input, sizeof(rd_input), "{\"document_id\":\"%s\"}", token->valuestring);
                        char rd_result[8192];
                        memset(rd_result, 0, sizeof(rd_result));
                        tool_registry_execute("feishu_doc_read", rd_input, rd_result, sizeof(rd_result));
                        if (rd_result[0] && !strstr(rd_result, "Error")) {
                            char* hit = fuzzy_search(rd_result, q_title);
                            if (hit) {
                                match_count++;
                                int ctx_start = (int)(hit - rd_result);
                                if (ctx_start > 50) ctx_start -= 50; else ctx_start = 0;
                                /* Align to UTF-8 character boundary */
                                ctx_start = utf8_align_start(rd_result, ctx_start);
                                int ctx_len = strlen(q_title) + 100;
                                int rd_len = (int)strlen(rd_result);
                                if (ctx_start + ctx_len > rd_len)
                                    ctx_len = rd_len - ctx_start;
                                /* Align end to UTF-8 character boundary */
                                ctx_len = utf8_align_end(rd_result, rd_len, ctx_start + ctx_len) - ctx_start;
                                if (match_count == 1) {
                                    if (is_en)
                                        snprintf(reply, 1024, "Found keyword in document %s: %.*s",
                                                 name->valuestring, ctx_len, rd_result + ctx_start);
                                    else
                                        snprintf(reply, 1024, "在文档《%s》中找到关键词：%.*s",
                                                 name->valuestring, ctx_len, rd_result + ctx_start);
                                } else {
                                    char tmp[400];
                                    if (is_en)
                                        snprintf(tmp, sizeof(tmp), ". In document %s: %.*s",
                                                 name->valuestring, ctx_len, rd_result + ctx_start);
                                    else
                                        snprintf(tmp, sizeof(tmp), "。在文档《%s》中：%.*s",
                                                 name->valuestring, ctx_len, rd_result + ctx_start);
                                    strncat(reply, tmp, 1024 - strlen(reply) - 1);
                                }
                            }
                        }
                    }
                }
            }
            cJSON_Delete(s_json);
            if (match_count == 0) {
                if (is_en)
                    snprintf(reply, 1024, "No Lark document found with title containing %s",
                             q_title[0] ? q_title : "that name");
                else
                    snprintf(reply, 1024, "未找到标题包含%s的飞书文档", q_title[0] ? q_title : "该名称");
            }
            syslog(LOG_INFO, "[%s] [FEISHU_DOC] Reply to TTS: \"%s\" (fast=%d llm=%d)\n",
                   TAG, reply, s_fast_path_count+1, s_llm_path_count);
            s_fast_path_count++;
            return reply;
        }

        /* Step 2: Read document content */
        char read_input[512];
        snprintf(read_input, sizeof(read_input), "{\"document_id\":\"%s\"}", doc_id);
        char* read_result = calloc(1, 8192);
        if (!read_result) return NULL;
        tool_registry_execute("feishu_doc_read", read_input, read_result, 8192);
        syslog(LOG_INFO, "[%s] [FEISHU_DOC] Read result: %.500s\n", TAG, read_result);

        char* reply = calloc(1, 8192);
        if (reply) {
            if (read_result[0] && !strstr(read_result, "Error")) {
                /* Pass full content to TTS; voice_channel will split by
                 * punctuation into segments for synthesis. */
                if (is_en)
                    snprintf(reply, 8192, "Lark document content: %s", read_result);
                else
                    snprintf(reply, 8192, "飞书文档内容：%s", read_result);
            } else {
                if (is_en)
                    snprintf(reply, 8192, "Failed to read Lark document, please try again later");
                else
                    snprintf(reply, 8192, "读取飞书文档失败，请稍后再试");
            }
            syslog(LOG_INFO, "[%s] [FEISHU_DOC] Reply to TTS: \"%s\" (fast=%d llm=%d)\n",
                   TAG, reply, s_fast_path_count+1, s_llm_path_count);
            s_fast_path_count++;
            syslog(LOG_INFO, "[%s] NL fast path: feishu_doc_query (fast=%d llm=%d)\n",
                   TAG, s_fast_path_count, s_llm_path_count);
        }
        free(read_result);
        return reply;
    }

    /* ── Feishu document search: search keyword across documents ───── */
    static const char* kw_search_action[] = {
        "搜索", "查找", "检索", "找一下", "搜一下",
        "search", "find", "look for", "search for", NULL
    };
    static const char* kw_search_obj[] = {
        "关键词", "内容", "飞书文档", "文档", "飞书",
        "keyword", "content", NULL
    };
    bool has_search_a = contains_any(text, kw_search_action);
    bool has_search_o = contains_any(text, kw_search_obj);
    if (has_search_a && has_search_o) {
        syslog(LOG_INFO, "[%s] [FEISHU_DOC] Layer2 search intent, text=\"%.200s\"\n", TAG, text);
        bool is_en = is_english_text(text);

        /* Extract keyword: prefer quotes, else text after search action word */
        char keyword[256] = {0};
        const char* ks = NULL;
        const char* ke = NULL;
        /* English quotes */
        const char* sq1 = strchr(text, '"');
        if (sq1) { const char* sq2 = strchr(sq1+1, '"'); if (sq2 && sq2 > sq1+1) { ks = sq1+1; ke = sq2; } }
        /* Chinese quotes */
        if (!ks) {
            const char* slq = strstr(text, "\xe2\x80\x9c");
            if (slq) { const char* srq = strstr(slq+3, "\xe2\x80\x9d"); if (srq && srq > slq+3) { ks = slq+3; ke = srq; } }
        }
        /* Title brackets 《》 (书名号) */
        if (!ks) {
            const char* lb = strstr(text, "\xe3\x80\x8a");
            if (lb) { const char* rb = strstr(lb+3, "\xe3\x80\x8b"); if (rb && rb > lb+3) { ks = lb+3; ke = rb; } }
        }
        /* Corner brackets 「」 */
        if (!ks) {
            const char* lb = strstr(text, "\xe3\x80\x8c");
            if (lb) { const char* rb = strstr(lb+3, "\xe3\x80\x8d"); if (rb && rb > lb+3) { ks = lb+3; ke = rb; } }
        }
        /* Fallback: text after "关键词"/"keyword" */
        if (!ks) {
            const char* kw_pos = strcasestr(text, "关键词");
            size_t kw_skip = strlen("关键词");
            if (!kw_pos) {
                kw_pos = strcasestr(text, "keyword");
                kw_skip = strlen("keyword");
            }
            if (kw_pos) {
                const char* p = kw_pos + kw_skip;
                while (*p == ' ' || *p == ',' || *p == ':' || *p == ';') p++;
                if (*p) { ks = p; ke = text + strlen(text); }
            }
            /* Or text after search action word */
            if (!ks) {
                for (int i = 0; kw_search_action[i]; i++) {
                    const char* p = strcasestr(text, kw_search_action[i]);
                    if (p) {
                        p += strlen(kw_search_action[i]);
                        while (*p == ' ' || *p == ',' || *p == ':' || *p == ';') p++;
                        /* Skip object words (飞书文档, 文档, 飞书, etc.)
                         * so they are not included in the keyword */
                        for (int j = 0; kw_search_obj[j]; j++) {
                            size_t olen = strlen(kw_search_obj[j]);
                            if (strncmp(p, kw_search_obj[j], olen) == 0) {
                                p += olen;
                                break;
                            }
                        }
                        while (*p == ' ' || *p == ',' || *p == ':' || *p == ';') p++;
                        if (*p) { ks = p; ke = text + strlen(text); break; }
                    }
                }
            }
        }
        if (ks && ke && ke > ks) {
            size_t kl = (size_t)(ke - ks);
            if (kl >= sizeof(keyword)) kl = sizeof(keyword) - 1;
            int ki = 0;
            for (size_t i = 0; i < kl && ks[i]; i++) {
                /* Filter quote, backslash, and CJK brackets 《》「」 */
                if (ks[i] == '"' || ks[i] == '\\') continue;
                if ((unsigned char)ks[i] == 0xe3 && (unsigned char)ks[i+1] == 0x80 &&
                    ((unsigned char)ks[i+2] == 0x8a || (unsigned char)ks[i+2] == 0x8b ||
                     (unsigned char)ks[i+2] == 0x8c || (unsigned char)ks[i+2] == 0x8d)) {
                    i += 2;
                    continue;
                }
                if (ki < (int)sizeof(keyword)-1) keyword[ki++] = ks[i];
            }
            keyword[ki] = '\0';
            while (ki > 0) {
                char c = keyword[ki-1];
                if (c==' '||c==','||c=='.'||c==':'||c==';'||c=='?'||c=='!'||c=='\n') { keyword[--ki]='\0'; }
                else if (ki >= 3) {
                    unsigned char c0 = (unsigned char)keyword[ki-3];
                    unsigned char c1 = (unsigned char)keyword[ki-2];
                    /* Clear all CJK punctuation by prefix (covers 、。《》「」 etc.) */
                    if ((c0 == 0xE3 && c1 == 0x80) ||
                        (c0 == 0xEF && c1 == 0xBC) ||
                        (c0 == 0xE2 && c1 == 0x80)) {
                        ki -= 3; keyword[ki] = '\0';
                    } else break;
                }
                else break;
            }
        }
        syslog(LOG_INFO, "[%s] [FEISHU_DOC] Search keyword: \"%s\"\n", TAG, keyword);

        if (keyword[0] == '\0') {
            char* reply = calloc(1, 512);
            if (reply)
                snprintf(reply, 512, is_en ? "Please tell me the keyword to search" : "请告诉我要搜索的关键词");
            return reply;
        }

        /* Step 1: List all documents */
        char list_result[4096];
        memset(list_result, 0, sizeof(list_result));
        tool_registry_execute("feishu_doc_list", "{}", list_result, sizeof(list_result));

        /* Step 2: Iterate documents, read content, search keyword.
         * Limit the number of feishu_doc_read calls (each is a ~1s
         * network round-trip) to avoid multi-second blocking delays.
         * Documents whose title already contains the keyword are
         * reported immediately without reading their content. */
        char* reply = calloc(1, 1024);
        int match_count = 0;
        int docs_read = 0;
        const int MAX_DOCS_TO_READ = 5;
        cJSON* list_json = cJSON_Parse(list_result);
        if (list_json && cJSON_IsArray(list_json)) {
            int arr_size = cJSON_GetArraySize(list_json);
            for (int i = 0; i < arr_size && match_count < 3; i++) {
                cJSON* item = cJSON_GetArrayItem(list_json, i);
                cJSON* name = cJSON_GetObjectItem(item, "name");
                cJSON* token = cJSON_GetObjectItem(item, "token");
                cJSON* type = cJSON_GetObjectItem(item, "type");
                if (cJSON_IsString(token) && cJSON_IsString(name) &&
                    (!type || (cJSON_IsString(type) && strcmp(type->valuestring, "docx") == 0))) {
                    /* If the document title already contains the keyword,
                     * report it as a match without reading content —
                     * saves a ~1s network round-trip per document. */
                    if (strcasestr(name->valuestring, keyword)) {
                        match_count++;
                        if (match_count == 1) {
                            if (is_en)
                                snprintf(reply, 1024, "Found document %s, title matches keyword: %s",
                                         name->valuestring, keyword);
                            else
                                snprintf(reply, 1024, "找到文档《%s》，标题匹配关键词：%s",
                                         name->valuestring, keyword);
                        } else {
                            char tmp[400];
                            if (is_en)
                                snprintf(tmp, sizeof(tmp), "; document %s",
                                         name->valuestring);
                            else
                                snprintf(tmp, sizeof(tmp), "；文档《%s》",
                                         name->valuestring);
                            strncat(reply, tmp, 1024 - strlen(reply) - 1);
                        }
                        continue;
                    }
                    /* Limit expensive feishu_doc_read calls */
                    if (docs_read >= MAX_DOCS_TO_READ)
                        continue;
                    docs_read++;
                    /* Read doc content */
                    char rd_input[512];
                    snprintf(rd_input, sizeof(rd_input), "{\"document_id\":\"%s\"}", token->valuestring);
                    char* rd_result = calloc(1, 8192);
                    if (!rd_result) continue;
                    tool_registry_execute("feishu_doc_read", rd_input, rd_result, 8192);
                    /* Search keyword in content */
                    if (rd_result[0] && !strstr(rd_result, "Error")) {
                        char* hit = fuzzy_search(rd_result, keyword);
                        if (hit) {
                            match_count++;
                            /* Extract context around keyword (50 chars before, 100 after) */
                            int ctx_start = (int)(hit - rd_result);
                            if (ctx_start > 50) ctx_start -= 50; else ctx_start = 0;
                            /* Align to UTF-8 character boundary */
                            ctx_start = utf8_align_start(rd_result, ctx_start);
                            int ctx_len = strlen(keyword) + 100;
                            int rd_len = (int)strlen(rd_result);
                            if (ctx_start + ctx_len > rd_len)
                                ctx_len = rd_len - ctx_start;
                            /* Align end to UTF-8 character boundary */
                            ctx_len = utf8_align_end(rd_result, rd_len, ctx_start + ctx_len) - ctx_start;
                            if (match_count == 1) {
                                if (is_en)
                                    snprintf(reply, 1024, "Found keyword in document %s: %.*s",
                                             name->valuestring, ctx_len, rd_result + ctx_start);
                                else
                                    snprintf(reply, 1024, "在文档《%s》中找到关键词：%.*s",
                                             name->valuestring, ctx_len, rd_result + ctx_start);
                            } else {
                                char tmp[400];
                                if (is_en)
                                    snprintf(tmp, sizeof(tmp), ". In document %s: %.*s",
                                             name->valuestring, ctx_len, rd_result + ctx_start);
                                else
                                    snprintf(tmp, sizeof(tmp), "。在文档《%s》中：%.*s",
                                             name->valuestring, ctx_len, rd_result + ctx_start);
                                strncat(reply, tmp, 1024 - strlen(reply) - 1);
                            }
                        }
                    }
                    free(rd_result);
                }
            }
        }
        cJSON_Delete(list_json);

        if (match_count == 0) {
            if (is_en)
                snprintf(reply, 1024, "No Lark document found containing %s", keyword);
            else
                snprintf(reply, 1024, "未找到包含%s的飞书文档", keyword);
        }
        syslog(LOG_INFO, "[%s] [FEISHU_DOC] Reply to TTS: \"%s\" (fast=%d llm=%d)\n",
               TAG, reply, s_fast_path_count+1, s_llm_path_count);
        s_fast_path_count++;
        syslog(LOG_INFO, "[%s] NL fast path: feishu_doc_search (fast=%d llm=%d)\n",
               TAG, s_fast_path_count, s_llm_path_count);
        return reply;
    }

    /* ── Feishu today's messages: fetch + local keyword filter ──── */
    static const char* kw_msg_today[] = {
        "今日消息", "今天的消息", "今日总结", "消息总结",
        "今日飞书消息", "今天飞书消息", "飞书今日消息",
        /* English keywords */
        "today's messages", "todays messages", "today's message",
        "todays message", "today summary", "today's summary",
        "messages today", "today's news", "daily summary",
        NULL
    };
    if (contains_any(text, kw_msg_today)) {
        syslog(LOG_INFO, "[%s] [FEISHU_MSG] Layer2 today message intent, text=\"%.200s\"\n", TAG, text);
        bool is_en = is_english_text(text);

        /* Step 1: Fetch today's messages via tool */
        char* msg_result = calloc(1, 32 * 1024);  /* Must be >=32KB to handle large message responses */
        if (!msg_result) return NULL;
        tool_registry_execute("feishu_message_today", "{}",
            msg_result, 32 * 1024);
        syslog(LOG_INFO, "[%s] [FEISHU_MSG] Tool result: %.500s\n", TAG, msg_result);

        /* Check for errors or empty */
        if (!msg_result[0] || strstr(msg_result, "Error:") ||
            strstr(msg_result, "今日暂无消息") ||
            strstr(msg_result, "没有重要信息")) {
            char* reply;
            if (!msg_result[0])
                reply = strdup(is_en ? "Failed to read Lark messages" : "读取飞书消息失败");
            else
                reply = strdup(msg_result);
            free(msg_result);
            s_fast_path_count++;
            syslog(LOG_INFO,
                "[%s] NL fast path: feishu_message_today (empty/error, fast=%d)\n",
                TAG, s_fast_path_count);
            return reply;
        }

        /* Tool already filtered by keywords and stripped time prefix.
         * Just sanitize and return. */
        sanitize_doc_content(msg_result);
        s_fast_path_count++;
        syslog(LOG_INFO,
            "[%s] [FEISHU_MSG] Result: %.500s (fast=%d)\n",
            TAG, msg_result, s_fast_path_count);
        return msg_result;  /* transfer ownership to caller */
    }

    return NULL;
}

/* ── Handle slash commands + NL fast path (bypass LLM) ─── */

static char* handle_slash_command(agent_msg_t* msg)
{
    if (!msg->content) {
        return NULL;
    }

    /* Phase 1: slash commands (highest priority) */
    if (msg->content[0] != '/') {
        return NULL; /* NL fast path handled separately after injection check */
    }

    char* reply = NULL;

    if (strncmp(msg->content, "/help", 5) == 0) {
        reply = strdup(
            "AI Agent 快捷命令：\n\n"
            "/help           显示本帮助\n"
            "/time           当前时间\n"
            "/weather 城市   实时天气\n"
            "/news [关键词]  最新新闻\n"
            "/memory         查看长期记忆\n"
            "/note 内容      记一条笔记\n"
            "/remind 秒数 内容  设提醒\n"
            "/skill          列出技能\n"
            "/translate 文本 翻译\n"
            "/daily          每日简报\n\n"
            "也可以直接用自然语言对话，我会自动调用工具。");
    } else if (strncmp(msg->content, "/time", 5) == 0) {
        reply = calloc(1, 256);
        if (reply) {
            tool_registry_execute("get_current_time", "{}", reply, 256);
        }
    } else if (strncmp(msg->content, "/weather", 8) == 0) {
        const char* arg = msg->content + 8;
        while (*arg == ' ') {
            arg++;
        }
        if (!*arg) {
            arg = "Beijing";
        }
        char input[256];
        snprintf(input, sizeof(input),
            "{\"location\":\"%s\"}", arg);
        reply = calloc(1, 4096);
        if (reply) {
            tool_registry_execute("get_weather", input, reply, 4096);
        }
    } else if (strncmp(msg->content, "/news", 5) == 0) {
        const char* arg = msg->content + 5;
        while (*arg == ' ') {
            arg++;
        }
        if (!*arg) {
            arg = "today";
        }
        char input[256];
        snprintf(input, sizeof(input),
            "{\"query\":\"%s\",\"top_headlines\":true}", arg);
        reply = calloc(1, 8192);
        if (reply) {
            tool_registry_execute("news_search", input, reply, 8192);
        }
    } else if (strncmp(msg->content, "/memory", 7) == 0) {
        char mem_path[256];
        snprintf(mem_path, sizeof(mem_path),
            "{\"path\":\"%s/memory/MEMORY.md\"}", AGENT_DATA_DIR);
        reply = calloc(1, 4096);
        if (reply) {
            int r = tool_registry_execute(
                "read_file", mem_path, reply, 4096);
            if (r != OK || !reply[0]) {
                snprintf(reply, 4096, "暂无长期记忆。");
            }
        }
    } else if (strncmp(msg->content, "/note ", 6) == 0) {
        reply = handle_slash_note(msg);
    } else if (strncmp(msg->content, "/remind ", 8) == 0) {
        reply = handle_slash_remind(msg);
    } else if (strncmp(msg->content, "/skill", 6) == 0) {
        char skills_buf[2048];
        size_t slen = skill_loader_build_summary(
            skills_buf, sizeof(skills_buf));
        reply = (slen > 0)
            ? strdup(skills_buf)
            : strdup("暂无已加载的技能。");
    } else if (strncmp(msg->content, "/translate ", 11) == 0) {
        /* Rewrite content to pass through LLM */
        char* nc = malloc(strlen(msg->content) + 64);
        if (nc) {
            snprintf(nc, strlen(msg->content) + 64,
                "请翻译以下内容（中英互译）：%s", msg->content + 11);
            free(msg->content);
            msg->content = nc;
        }
    } else if (strncmp(msg->content, "/daily", 6) == 0) {
        char* nc = strdup(
            "请给我一份今日简报，包括：当前时间、今日天气、"
            "最新新闻摘要。");
        if (nc) {
            free(msg->content);
            msg->content = nc;
        }
    }

    if (reply) {
        s_fast_path_count++;
    }
    return reply;
}

/* /note sub-handler */
static char* handle_slash_note(const agent_msg_t* msg)
{
    const char* note = msg->content + 6;
    struct tm tm_now = agent_localtime();

    char date_str[16];
    strftime(date_str, sizeof(date_str), "%Y-%m-%d", &tm_now);
    char time_str[16];
    strftime(time_str, sizeof(time_str), "%H:%M", &tm_now);

    char path[256];
    snprintf(path, sizeof(path), "%s/memory/daily/%s.md",
        AGENT_DATA_DIR, date_str);

    char input[4096];
    snprintf(input, sizeof(input),
        "{\"path\":\"%s\",\"content\":\"%s %s\\n- %s %s\\n\"}",
        path, "# ", date_str, time_str, note);

    char* reply = calloc(1, 512);
    if (reply) {
        tool_registry_execute("write_file", input, reply, 512);
        snprintf(reply, 512, "已记录：%s", note);
    }
    return reply;
}

/* /remind sub-handler */
static char* handle_slash_remind(const agent_msg_t* msg)
{
    int secs = 0;
    char remind_msg[512];

    remind_msg[0] = '\0';
    if (sscanf(msg->content + 8, "%d %511[^\n]",
            &secs, remind_msg)
        < 1) {
        return NULL;
    }
    if (!remind_msg[0]) {
        strncpy(remind_msg, "提醒时间到", sizeof(remind_msg) - 1);
    }

    char input[1024];
    snprintf(input, sizeof(input),
        "{\"name\":\"remind\",\"schedule_type\":\"at\","
        "\"at_epoch\":%lld,\"message\":\"%s\","
        "\"channel\":\"%s\",\"chat_id\":\"%s\"}",
        (long long)(time(NULL) + secs), remind_msg,
        msg->channel, msg->chat_id);

    char* reply = calloc(1, 512);
    if (reply) {
        tool_registry_execute("cron_add", input, reply, 512);
        snprintf(reply, 512, "好的，%d 秒后提醒你：%s",
            secs, remind_msg);
    }
    return reply;
}

/* ── Extracted: handle vision message ──────────────────────── */

static char* handle_vision_message(agent_msg_t* msg)
{
    if (!msg->image_b64 || !msg->image_b64[0]) {
        return NULL;
    }

    syslog(LOG_INFO,
        "[%s] Vision message detected, calling llm_chat_vision\n", TAG);

    size_t vis_size = agent_mem_safe_size(
        TOOL_OUTPUT_SIZE, TOOL_OUTPUT_SIZE_MIN);
    char* vision_resp = calloc(1, vis_size);

    if (!vision_resp) {
        free(msg->image_b64);
        msg->image_b64 = NULL;
        return NULL;
    }

    const char* prompt = (msg->content && msg->content[0])
        ? msg->content
        : AGENT_VISION_DEFAULT_PROMPT;
    int err = llm_chat_vision(
        prompt, msg->image_b64, NULL, vision_resp, vis_size);

    free(msg->image_b64);
    msg->image_b64 = NULL;

    if (err == OK && vision_resp[0]) {
        return vision_resp;
    }

    free(vision_resp);
    return NULL;
}

/* ── Extracted: strip leaked tool-call XML markup ─────────── */

static char* strip_tool_call_markup(char* text)
{
    if (!text) {
        return NULL;
    }

    int dirty = 0;
    if (strstr(text, "<tool_call>") || strstr(text, ":tool_call>")) {
        dirty = 1;
    }
    if (!dirty) {
        return text;
    }

    syslog(LOG_WARNING,
        "[%s] Force-finish text contains tool-call markup, stripping\n",
        TAG);

    size_t len = strlen(text);
    char* clean = calloc(1, len + 1);
    if (!clean) {
        return text;
    }

    const char* r = text;
    char* w = clean;

    while (*r) {
        const char* ts = NULL;
        const char* te = NULL;

        const char* plain = strstr(r, "<tool_call>");
        const char* ns = strstr(r, ":tool_call>");
        const char* ns_open = NULL;

        if (ns) {
            ns_open = ns;
            while (ns_open > r && *(ns_open - 1) != '<') {
                ns_open--;
            }
            if (ns_open > r) {
                ns_open--;
            } else {
                ns_open = NULL;
            }
        }

        if (plain && (!ns_open || plain <= ns_open)) {
            ts = plain;
            te = strstr(ts + 11, "</tool_call>");
            if (te) {
                te += 12;
            }
        } else if (ns_open) {
            ts = ns_open;
            size_t plen = (size_t)(ns - (ts + 1));
            if (plen > 0 && plen < 32) {
                char ctag[64];
                snprintf(ctag, sizeof(ctag), "</%.*s:tool_call>",
                    (int)plen, ts + 1);
                te = strstr(ts, ctag);
                if (te) {
                    te += strlen(ctag);
                }
            }
        }

        if (ts && te) {
            size_t prefix = (size_t)(ts - r);
            memcpy(w, r, prefix);
            w += prefix;
            r = te;
        } else {
            size_t rest = strlen(r);
            memcpy(w, r, rest);
            w += rest;
            break;
        }
    }
    *w = '\0';

    free(text);
    if (clean[0] == '\0' || strspn(clean, " \t\r\n") == strlen(clean)) {
        free(clean);
        return strdup(
            "抱歉，这个任务比较复杂，"
            "我已经收集了一些信息但未能完成全部步骤。"
            "请尝试拆分成更小的问题再问我。");
    }
    return clean;
}

/* ── Extracted: force finish when iteration limit reached ─── */

static char* force_finish_reply(const char* system_prompt,
    cJSON* messages)
{
    syslog(LOG_WARNING,
        "[%s] Tool iteration limit (%d) reached, forcing finish\n",
        TAG, AGENT_AI_AGENT_MAX_TOOL_ITER);

    cJSON* hint = cJSON_CreateObject();
    cJSON_AddStringToObject(hint, "role", "system");
    cJSON_AddStringToObject(hint, "content",
        "You have used all available tool iterations. "
        "Do NOT call any more tools. Summarize what you have "
        "learned so far and reply to the user in plain text now.");
    cJSON_AddItemToArray(messages, hint);

    llm_response_t resp;
    char* result = NULL;
    struct timeval t0, t1;
    gettimeofday(&t0, NULL);
    int err = llm_chat_tools(system_prompt, messages, NULL, &resp);
    gettimeofday(&t1, NULL);
    uint32_t ms = calc_elapsed_ms(&t0, &t1);
    bool timed_out = llm_call_timed_out(ms);

    if (err == OK && !timed_out && resp.text && resp.text_len > 0) {
        result = strdup(resp.text);
    }
    llm_response_free(&resp);

    if (!result && timed_out) {
        result = strdup(LLM_TIMEOUT_MSG);
    }

    return strip_tool_call_markup(result);
}

/* ── Extracted: dispatch response to outbound bus ─────────── */

static void dispatch_response(const agent_msg_t* msg,
    char* final_text)
{
    if (final_text && final_text[0]) {
        session_append(msg->chat_id, "user", msg->content);
        session_append(msg->chat_id, "assistant", final_text);

        /* For voice channel, directly call voice_channel_speak() to avoid
         * dependency on the outbound dispatch thread (may be stuck after
         * WiFi disconnect/reconnect). */
        if (strcmp(msg->channel, AGENT_CHAN_VOICE) == 0) {
            extern int ws_server_broadcast_typed(const char*,
                const char*, const char*);
            ws_server_broadcast_typed("response_silent", final_text,
                AGENT_CHAN_VOICE);
            syslog(LOG_INFO,
                "[%s] dispatch_response: direct speak (%zu bytes)\n",
                TAG, strlen(final_text));
            int vret = voice_channel_speak(final_text);
            if (vret != 0) {
                syslog(LOG_ERR,
                    "[%s] dispatch_response speak failed: %d\n",
                    TAG, vret);
            }
            free(final_text);
        } else {
            agent_msg_t out = { 0 };
            strncpy(out.channel, msg->channel,
                sizeof(out.channel) - 1);
            strncpy(out.chat_id, msg->chat_id,
                sizeof(out.chat_id) - 1);
            out.content = final_text;
            if (message_bus_push_outbound(&out) != OK) {
                free(final_text);
            }
        }
    } else {
        free(final_text);
        agent_msg_t out = { 0 };
        strncpy(out.channel, msg->channel,
            sizeof(out.channel) - 1);
        strncpy(out.chat_id, msg->chat_id,
            sizeof(out.chat_id) - 1);
        out.content = strdup("Sorry, I encountered an error.");
        if (out.content) {
            if (message_bus_push_outbound(&out) != OK) {
                free(out.content);
            }
        }
    }
}

/* ── ReAct tool-calling loop ──────────────────────────────── */

static const char* s_working_phrases[] = {
    "正在思考中...",
    "稍等，处理中...",
    "让我查一下...",
    "正在分析...",
    "马上好...",
};
#define WORKING_PHRASE_COUNT \
    ((int)(sizeof(s_working_phrases) / sizeof(s_working_phrases[0])))

/* Send a "working" status message on the first iteration.
 * Skip for feishu (has its own typing indicator) and voice
 * (TTS synthesis of a status phrase wastes time and memory). */
static void send_working_status(const agent_msg_t* msg, int iteration)
{
    if (iteration != 0) {
        return;
    }
    if (strcmp(msg->channel, AGENT_CHAN_FEISHU) == 0
        || strcmp(msg->channel, AGENT_CHAN_VOICE) == 0
        || strcmp(msg->channel, AGENT_CHAN_WEIXIN) == 0) {
        return;
    }

    agent_msg_t status = { 0 };

    strncpy(status.channel, msg->channel, sizeof(status.channel) - 1);
    strncpy(status.chat_id, msg->chat_id, sizeof(status.chat_id) - 1);
    status.content = strdup(
        s_working_phrases[(unsigned)rand() % WORKING_PHRASE_COUNT]);
    if (status.content) {
        if (message_bus_push_outbound(&status) != OK) {
            free(status.content);
        }
    }
}

/* Check for duplicate tool calls. Returns true if loop should break. */
static bool check_tool_dup(const llm_response_t* resp,
    char* prev_sig, int* dup_count,
    char* prev_name, int* name_repeat)
{
    if (resp->call_count != 1) {
        prev_sig[0] = '\0';
        *dup_count = 0;
        prev_name[0] = '\0';
        *name_repeat = 0;
        return false;
    }

    /* Exact match (name + args) */
    char cur_sig[512];

    snprintf(cur_sig, sizeof(cur_sig), "%s|%.400s",
        resp->calls[0].name,
        resp->calls[0].input ? resp->calls[0].input : "");

    if (strcmp(cur_sig, prev_sig) == 0) {
        (*dup_count)++;
        if (*dup_count >= 2) {
            syslog(LOG_WARNING,
                "[%s] Duplicate tool call detected (%s), breaking\n",
                TAG, resp->calls[0].name);
            return true;
        }
    } else {
        *dup_count = 0;
    }
    strncpy(prev_sig, cur_sig, 511);
    prev_sig[511] = '\0';

    /* Name-only repeat detection */
    if (strcmp(resp->calls[0].name, prev_name) == 0) {
        (*name_repeat)++;
        if (*name_repeat >= AGENT_TOOL_NAME_REPEAT_MAX) {
            syslog(LOG_WARNING,
                "[%s] Tool name repeat limit (%s called %d times)\n",
                TAG, resp->calls[0].name, *name_repeat + 1);
            return true;
        }
    } else {
        *name_repeat = 0;
    }
    strncpy(prev_name, resp->calls[0].name, 63);
    prev_name[63] = '\0';

    return false;
}

/* Check if an LLM call exceeded the watchdog timeout.
 * Returns true when latency_ms exceeds AGENT_LLM_TIMEOUT_SEC. */
static bool llm_call_timed_out(uint32_t latency_ms)
{
    return latency_ms > (uint32_t)AGENT_LLM_TIMEOUT_SEC * 1000;
}

/* Handle TASK_COMPLETE: inject hint and do one final LLM call.
 * If the LLM call times out, *out_timed_out is set to true. */
static char* handle_task_complete(const char* sys_prompt, cJSON* messages,
    bool* out_timed_out)
{
    cJSON* hint = cJSON_CreateObject();

    cJSON_AddStringToObject(hint, "role", "system");
    cJSON_AddStringToObject(hint, "content",
        "The task has been completed successfully. "
        "Do NOT call any more tools. "
        "Reply to the user now confirming what was done.");
    cJSON_AddItemToArray(messages, hint);

    llm_response_t final_resp;
    char* result = NULL;
    struct timeval t0, t1;
    gettimeofday(&t0, NULL);
    int err = llm_chat_tools(sys_prompt, messages, NULL, &final_resp);
    gettimeofday(&t1, NULL);
    uint32_t ms = calc_elapsed_ms(&t0, &t1);
    bool timed_out = llm_call_timed_out(ms);

    if (err == OK && !timed_out
        && final_resp.text && final_resp.text_len > 0) {
        result = strdup(final_resp.text);
    }
    llm_response_free(&final_resp);

    if (!result && timed_out) {
        if (out_timed_out) {
            *out_timed_out = true;
        }
        return strdup(LLM_TIMEOUT_TASK_COMPLETE_MSG);
    }
    return result;
}

/* ── LLM Streaming Voice Path ─────────────────────────────── */

#define VOICE_STREAM_SENTENCE_MIN_BYTES 12
#define VOICE_STREAM_FLUSH_BYTES 45

typedef struct {
    char* buf;
    size_t len;
    size_t cap;
    char* full_response;
    size_t full_len;
    size_t full_cap;
    int sentence_count;
    int tool_use;
    char tool_call_id[64];
    char tool_call_name[32];
    char* tool_call_input;
    size_t tool_call_input_len;
} voice_stream_ctx_t;

static int is_sentence_end(const char* s, size_t len)
{
    if (len == 0)
        return 0;
    char c = s[len - 1];
    if (c == '。' || c == '！' || c == '？' || c == '.' || c == '!'
        || c == '?' || c == '\n')
        return 1;
    if (len >= 3) {
        const char* p = s + len - 3;
        if ((p[0] & 0xC0) == 0xC0) {
            unsigned int cp = 0;
            int bytes = 1;
            if ((p[0] & 0xE0) == 0xC0) bytes = 2;
            else if ((p[0] & 0xF0) == 0xE0) bytes = 3;
            else if ((p[0] & 0xF8) == 0xF0) bytes = 4;
            if (bytes == 3) {
                cp = ((unsigned char)p[0] & 0x0F) << 12
                    | ((unsigned char)p[1] & 0x3F) << 6
                    | ((unsigned char)p[2] & 0x3F);
                if (cp == 0x3002 || cp == 0xFF01 || cp == 0xFF1F
                    || cp == 0x2026)
                    return 1;
            }
        }
    }
    return 0;
}

static void voice_stream_flush_sentence(voice_stream_ctx_t* ctx)
{
    if (!ctx->buf || ctx->len == 0)
        return;

    ctx->buf[ctx->len] = '\0';

    {
        char* p = ctx->buf;
        while (*p == '\n' || *p == '\r' || *p == ' ' || *p == '\t')
            p++;
        if (*p == '\0') {
            ctx->len = 0;
            return;
        }
        if (p != ctx->buf) {
            size_t shift = p - ctx->buf;
            memmove(ctx->buf, p, ctx->len - shift + 1);
            ctx->len -= shift;
        }
    }

    if (voice_pipeline_is_active()) {
        voice_pipeline_push(ctx->buf);
        extern int ws_server_broadcast(const char*, const char*);
        ws_server_broadcast(ctx->buf, AGENT_CHAN_VOICE);
    } else {
        agent_msg_t out = { 0 };
        strncpy(out.channel, AGENT_CHAN_VOICE, sizeof(out.channel) - 1);
        strncpy(out.chat_id, "voice", sizeof(out.chat_id) - 1);
        out.content = strdup(ctx->buf);
        if (out.content) {
            if (message_bus_push_outbound(&out) != OK)
                free(out.content);
        }
    }

    ctx->len = 0;
    ctx->sentence_count++;
}

static void voice_stream_chunk_cb(const llm_stream_chunk_t* chunk,
    void* user_data)
{
    voice_stream_ctx_t* ctx = (voice_stream_ctx_t*)user_data;

    if (chunk->is_final) {
        if (ctx->len > 0)
            voice_stream_flush_sentence(ctx);
        return;
    }

    if (chunk->has_tool_call) {
        ctx->tool_use = 1;
        if (chunk->tool_call_id[0])
            strncpy(ctx->tool_call_id, chunk->tool_call_id,
                sizeof(ctx->tool_call_id) - 1);
        if (chunk->tool_call_name[0])
            strncpy(ctx->tool_call_name, chunk->tool_call_name,
                sizeof(ctx->tool_call_name) - 1);
        if (chunk->tool_call_input && chunk->tool_call_input_len > 0) {
            if (!ctx->tool_call_input) {
                ctx->tool_call_input = malloc(chunk->tool_call_input_len + 1);
                ctx->tool_call_input_len = 0;
            } else {
                char* tmp = realloc(ctx->tool_call_input,
                    ctx->tool_call_input_len
                        + chunk->tool_call_input_len + 1);
                if (tmp)
                    ctx->tool_call_input = tmp;
            }
            if (ctx->tool_call_input) {
                memcpy(ctx->tool_call_input + ctx->tool_call_input_len,
                    chunk->tool_call_input, chunk->tool_call_input_len);
                ctx->tool_call_input_len += chunk->tool_call_input_len;
                ctx->tool_call_input[ctx->tool_call_input_len] = '\0';
            }
        }
        return;
    }

    if (chunk->tool_use) {
        if (ctx->len > 0)
            voice_stream_flush_sentence(ctx);
        return;
    }

    if (!chunk->text || chunk->text_len == 0)
        return;

    if (ctx->full_len + chunk->text_len + 1 > ctx->full_cap) {
        size_t new_cap = ctx->full_cap ? ctx->full_cap * 2 : 4096;
        char* tmp = realloc(ctx->full_response, new_cap);
        if (tmp) {
            ctx->full_response = tmp;
            ctx->full_cap = new_cap;
        }
    }
    if (ctx->full_response && ctx->full_len + chunk->text_len < ctx->full_cap) {
        memcpy(ctx->full_response + ctx->full_len, chunk->text,
            chunk->text_len);
        ctx->full_len += chunk->text_len;
        ctx->full_response[ctx->full_len] = '\0';
    }

    for (size_t i = 0; i < chunk->text_len; i++) {
        if (ctx->len + 1 >= ctx->cap) {
            size_t new_cap = ctx->cap ? ctx->cap * 2 : 512;
            char* tmp = realloc(ctx->buf, new_cap);
            if (tmp) {
                ctx->buf = tmp;
                ctx->cap = new_cap;
            } else {
                break;
            }
        }
        if (ctx->len < ctx->cap - 1)
            ctx->buf[ctx->len++] = chunk->text[i];

        if (is_sentence_end(ctx->buf, ctx->len)
            && ctx->len >= VOICE_STREAM_SENTENCE_MIN_BYTES) {
            voice_stream_flush_sentence(ctx);
        } else if (ctx->len >= VOICE_STREAM_FLUSH_BYTES) {
            voice_stream_flush_sentence(ctx);
        }
    }
}

static char* run_voice_stream_path(const char* sys_prompt,
    cJSON* messages, const char* tools_json,
    const agent_msg_t* msg)
{
    voice_stream_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));

    syslog(LOG_INFO, "[%s] voice stream path: starting LLM stream\n", TAG);

    llm_stream_set_voice_model(AGENT_LLM_QWEN_MODEL);

    int pipeline_started = 0;
    if (voice_pipeline_start() == 0)
        pipeline_started = 1;

    int err = llm_chat_tools_stream(sys_prompt, messages,
        tools_json, voice_stream_chunk_cb, &ctx);

    llm_stream_set_voice_model(NULL);

    if (pipeline_started) {
        if (err == OK && !ctx.tool_use && ctx.full_response
            && ctx.full_response[0]) {
            voice_pipeline_finish();
        } else {
            voice_pipeline_abort();
        }
    }

    if (err != OK) {
        syslog(LOG_WARNING,
            "[%s] voice stream path failed: %d, falling back\n", TAG, err);
        free(ctx.buf);
        free(ctx.full_response);
        free(ctx.tool_call_input);
        return NULL;
    }

    if (ctx.tool_use) {
        syslog(LOG_INFO,
            "[%s] voice stream path: tool call detected, falling back to ReAct\n",
            TAG);
        free(ctx.buf);
        free(ctx.full_response);
        free(ctx.tool_call_input);
        return NULL;
    }

    if (ctx.full_response && ctx.full_response[0]) {
        char* result = ctx.full_response;
        ctx.full_response = NULL;

        if (msg->content)
            session_append(msg->chat_id, "user", msg->content);
        session_append(msg->chat_id, "assistant", result);

        free(ctx.buf);
        free(ctx.tool_call_input);
        syslog(LOG_INFO,
            "[%s] voice stream path: complete (%d sentences, %zu bytes)\n",
            TAG, ctx.sentence_count, strlen(result));
        return result;
    }

    free(ctx.buf);
    free(ctx.full_response);
    free(ctx.tool_call_input);
    return NULL;
}

/* Run the ReAct tool-calling loop. Returns final_text (caller owns). */
static char* run_react_loop(const char* sys_prompt, cJSON* messages,
    const char* tools_json, char* tool_output, size_t tool_size,
    const agent_msg_t* msg)
{
    char prev_sig[512];
    int dup_count;
    char prev_name[64];
    int name_repeat;
    int iteration;
    char* final_text = NULL;

    prev_sig[0] = '\0';
    dup_count = 0;
    prev_name[0] = '\0';
    name_repeat = 0;
    int last_total_tokens = 0;
    bool watchdog_fired = false;

    /* Router: select and apply best backend before first LLM call.
     * Estimate complexity from the last user message. */
    llm_complexity_t complexity = LLM_COMPLEXITY_SIMPLE;
    if (msg->content) {
        complexity = llm_router_estimate_complexity(
            msg->content, strlen(msg->content));
    }
    int router_idx = llm_router_select(complexity);
    if (router_idx >= 0) {
        llm_router_apply(router_idx);
    }

    /* Structured trace: begin */
    agent_trace_t trace;
    agent_trace_begin(&trace, msg->chat_id, msg->channel);
    trace.backend_idx = router_idx;

    /* Cache check: for simple queries, try cache before LLM call */
    if (msg->content && complexity == LLM_COMPLEXITY_SIMPLE) {
        char* cached = llm_cache_get(msg->content, strlen(msg->content));
        if (cached) {
            syslog(LOG_INFO, "[%s] Cache hit, skipping LLM call\n", TAG);
            final_text = cached;
            agent_trace_step(&trace, 0, NULL, 0, 1);
            agent_trace_end(&trace, AGENT_TRACE_OK);
            goto send_reply;
        }
    }

    for (iteration = 0; iteration < AGENT_AI_AGENT_MAX_TOOL_ITER;
         iteration++) {
        send_working_status(msg, iteration);

        llm_response_t resp;
        struct timeval tv_start, tv_end;
        gettimeofday(&tv_start, NULL);
        int err = llm_chat_tools(sys_prompt, messages, tools_json, &resp);
        gettimeofday(&tv_end, NULL);
        uint32_t latency_ms = calc_elapsed_ms(&tv_start, &tv_end);

        /* Router failover: on LLM call failure, try next backend */
        if (err != OK && router_idx >= 0) {
            syslog(LOG_WARNING,
                "[%s] LLM call failed on backend %d, trying failover\n",
                TAG, router_idx);
            llm_router_report_failure(router_idx);

            int next_idx = llm_router_select(complexity);
            if (next_idx >= 0 && next_idx != router_idx) {
                llm_router_apply(next_idx);
                router_idx = next_idx;
                trace.backend_idx = next_idx;
                llm_response_free(&resp);
                gettimeofday(&tv_start, NULL);
                err = llm_chat_tools(sys_prompt, messages,
                    tools_json, &resp);
                gettimeofday(&tv_end, NULL);
                latency_ms = calc_elapsed_ms(&tv_start, &tv_end);
            }
        }

        if (err != OK) {
            /* Distinguish timeout-induced failure from other errors */
            if (llm_call_timed_out(latency_ms)) {
                syslog(LOG_WARNING,
                    "[%s] LLM watchdog: call failed after %" PRIu32 " ms "
                    "(limit %ds)\n",
                    TAG, latency_ms, AGENT_LLM_TIMEOUT_SEC);
                agent_trace_step(&trace, iteration, NULL,
                    latency_ms, 0);
                llm_response_free(&resp);
                final_text = strdup(LLM_TIMEOUT_MSG);
                watchdog_fired = true;
                break;
            }
            syslog(LOG_ERR, "[%s] LLM call failed (iter %d)\n",
                TAG, iteration);
            printf("[DEBUG] LLM call failed: err=%d, latency=%" PRIu32 "ms\n",
                err, latency_ms);
            printf("[DEBUG] network=%s, ip=%s\n",
                network_is_connected() ? "UP" : "DOWN",
                network_get_ip());
            agent_trace_step(&trace, iteration, NULL, latency_ms, 0);
            llm_response_free(&resp);
            break;
        }

        /* Watchdog: if the LLM call took longer than the configured
         * timeout, treat it as a timeout even if it returned OK.
         * The socket SO_RCVTIMEO may have fired and caused a partial
         * or error response that llm_chat_tools mapped to ERROR above,
         * but if the call barely completed, we still flag it. */
        if (llm_call_timed_out(latency_ms)) {
            syslog(LOG_WARNING,
                "[%s] LLM watchdog: call took %" PRIu32 " ms (limit %ds), "
                "treating as timeout\n",
                TAG, latency_ms, AGENT_LLM_TIMEOUT_SEC);
            agent_trace_step(&trace, iteration, NULL, latency_ms, 0);
            llm_response_free(&resp);
            final_text = strdup(LLM_TIMEOUT_MSG);
            watchdog_fired = true;
            break;
        }

        /* Report success to router */
        if (router_idx >= 0) {
            llm_router_report_success(router_idx);
            llm_router_report_latency(router_idx, latency_ms);
            if (resp.total_tokens > 0) {
                llm_router_report_tokens(router_idx,
                    resp.prompt_tokens, resp.completion_tokens);
                last_total_tokens = resp.total_tokens;
            }
        }

        syslog(LOG_INFO,
            "[%s] LLM resp: text=%zu, tool_use=%d, calls=%d\n",
            TAG, resp.text_len, resp.tool_use, resp.call_count);

        if (!resp.tool_use) {
            /* Cascade routing: if AUTO profile selected a cheap backend
             * for a SIMPLE query but the response looks inadequate,
             * retry once with PREMIUM tier. */
            bool cascade_retry = false;
            if (llm_router_get_profile() == LLM_ROUTE_AUTO
                && complexity == LLM_COMPLEXITY_SIMPLE
                && router_idx >= 0
                && resp.text != NULL) {
                bool low_quality = (strstr(resp.text, "I don't know") != NULL
                    || strstr(resp.text, "I cannot") != NULL
                    || strstr(resp.text, "无法") != NULL);
                if (low_quality) {
                    cascade_retry = true;
                }
            }

            if (cascade_retry) {
                syslog(LOG_INFO,
                    "[%s] Cascade: cheap response inadequate, "
                    "retrying with PREMIUM\n",
                    TAG);

                /* Save the cheap response as fallback before freeing */
                char* fallback_text = NULL;
                if (resp.text && resp.text_len > 0) {
                    fallback_text = strdup(resp.text);
                }
                llm_response_free(&resp);

                /* Select a PREMIUM backend without changing profile */
                int prem_idx = llm_router_select_with_profile(
                    LLM_COMPLEXITY_MEDIUM, LLM_ROUTE_PREMIUM);

                if (prem_idx >= 0 && prem_idx != router_idx) {
                    llm_router_apply(prem_idx);
                    router_idx = prem_idx;
                    trace.backend_idx = prem_idx;

                    gettimeofday(&tv_start, NULL);
                    err = llm_chat_tools(sys_prompt, messages,
                        tools_json, &resp);
                    gettimeofday(&tv_end, NULL);
                    latency_ms = calc_elapsed_ms(&tv_start, &tv_end);

                    /* Watchdog check on cascade retry */
                    if (llm_call_timed_out(latency_ms)) {
                        syslog(LOG_WARNING,
                            "[%s] LLM watchdog: cascade retry "
                            "took %" PRIu32 " ms\n",
                            TAG, latency_ms);
                        free(fallback_text);
                        fallback_text = NULL;
                        final_text = strdup(LLM_TIMEOUT_MSG);
                        watchdog_fired = true;
                        agent_trace_step(&trace, iteration, NULL,
                            latency_ms, 0);
                        llm_response_free(&resp);
                        break;
                    }

                    if (err == OK && resp.text && resp.text_len > 0) {
                        final_text = strdup(resp.text);
                        free(fallback_text);
                        fallback_text = NULL;
                    } else {
                        /* Premium failed — fall back to cheap response */
                        final_text = fallback_text;
                        fallback_text = NULL;
                    }
                    agent_trace_step(&trace, iteration, NULL,
                        latency_ms, err == OK);
                    llm_response_free(&resp);
                    break;
                }
                /* No premium backend available — use cheap response */
                final_text = fallback_text;
                fallback_text = NULL;
                agent_trace_step(&trace, iteration, NULL, latency_ms, 1);
                break;
            }

            if (resp.text && resp.text_len > 0 && !final_text) {
                final_text = strdup(resp.text);
            }
            agent_trace_step(&trace, iteration, NULL, latency_ms, 1);
            llm_response_free(&resp);
            break;
        }

        syslog(LOG_INFO, "[%s] Tool iter %d: %d calls\n",
            TAG, iteration + 1, resp.call_count);

        /* Trace: log tool step */
        agent_trace_step(&trace, iteration,
            resp.call_count > 0 ? resp.calls[0].name : NULL,
            latency_ms, 1);

        /* Duplicate detection — break if stuck in a loop */
        bool should_break = check_tool_dup(
            &resp, prev_sig, &dup_count, prev_name, &name_repeat);

        if (should_break) {
            add_assistant_message(messages, &resp);
            add_tool_result_messages(messages, &resp, tool_output,
                tool_size, msg->channel, msg->chat_id);
            llm_response_free(&resp);
            break;
        }

        add_assistant_message(messages, &resp);
        add_tool_result_messages(messages, &resp, tool_output,
            tool_size, msg->channel, msg->chat_id);

        /* Local tool shortcut: if the single tool in this round is a
         * local file op, skip the next LLM round and use the tool
         * output directly as the reply. Saves ~2s.
         * Restricted to call_count == 1: the parallel path does not
         * write into tool_output, so multi-call rounds must go through
         * the LLM to aggregate results (and tool_output would be stale
         * from a prior iteration if we allowed call_count > 1 here). */
        if (resp.call_count == 1) {
            bool all_local = true;
            for (int i = 0; i < resp.call_count; i++) {
                if (strcmp(resp.calls[i].name, "read_file") != 0
                    && strcmp(resp.calls[i].name, "write_file") != 0
                    && strcmp(resp.calls[i].name, "edit_file") != 0
                    && strcmp(resp.calls[i].name, "list_dir") != 0) {
                    all_local = false;
                    break;
                }
            }
            if (all_local && tool_output[0]) {
                /* Don't shortcut if the tool read a skill file
                 * from AGENT_SKILLS_DIR — the LLM needs to process
                 * the skill content and generate a proper user-facing
                 * response instead of dumping raw markdown to the user. */
                bool is_skill_read = false;
                if (strcmp(resp.calls[0].name, "read_file") == 0
                    && resp.calls[0].input != NULL) {
                    cJSON *input_obj = cJSON_Parse(resp.calls[0].input);
                    if (input_obj) {
                        cJSON *path_obj = cJSON_GetObjectItem(input_obj, "path");
                        if (path_obj && cJSON_IsString(path_obj)
                            && strncmp(path_obj->valuestring,
                                AGENT_SKILLS_DIR,
                                strlen(AGENT_SKILLS_DIR)) == 0) {
                            is_skill_read = true;
                        }
                        cJSON_Delete(input_obj);
                    }
                }

                if (!is_skill_read) {
                    syslog(LOG_INFO,
                        "[%s] Local tool shortcut: skip LLM round\n",
                        TAG);
                    final_text = strdup(tool_output);
                    llm_response_free(&resp);
                    break;
                }

                syslog(LOG_INFO,
                    "[%s] Skill file read — no shortcut, LLM will process\n",
                    TAG);
            }
        }

        /* TASK_COMPLETE detection */
        if (resp.call_count == 1 && tool_output[0]
            && strstr(tool_output, "TASK_COMPLETE")) {
            syslog(LOG_INFO,
                "[%s] Tool %s returned TASK_COMPLETE\n",
                TAG, resp.calls[0].name);
            llm_response_free(&resp);
            bool task_timed_out = false;
            final_text = handle_task_complete(sys_prompt, messages,
                &task_timed_out);
            if (task_timed_out) {
                watchdog_fired = true;
            }
            break;
        }

        llm_response_free(&resp);
    }

    /* Iteration limit reached — force a summary reply */
    if (!final_text && iteration >= AGENT_AI_AGENT_MAX_TOOL_ITER) {
        final_text = force_finish_reply(sys_prompt, messages);
        agent_trace_end(&trace, AGENT_TRACE_TIMEOUT);
    } else if (watchdog_fired) {
        agent_trace_end(&trace, AGENT_TRACE_TIMEOUT);
    } else if (final_text) {
        agent_trace_end(&trace, AGENT_TRACE_OK);
    } else {
        agent_trace_end(&trace, AGENT_TRACE_FAIL);
    }

send_reply:
    /* Cache store: save simple query responses for future reuse */
    if (final_text && msg->content
        && complexity == LLM_COMPLEXITY_SIMPLE) {
        llm_cache_put(msg->content, strlen(msg->content), final_text);
        if (last_total_tokens > 0) {
            llm_cache_put_tokens(msg->content, strlen(msg->content),
                last_total_tokens);
        }
    }

    return final_text;
}

/* ── Build messages array from session history + current msg ─ */

static cJSON* build_messages(const char* chat_id, const char* content,
    char* history_json, size_t hist_size)
{
    session_get_history_json(chat_id, history_json, hist_size,
        AGENT_AI_AGENT_MAX_HISTORY);

    cJSON* messages = cJSON_Parse(history_json);

    if (!messages) {
        messages = cJSON_CreateArray();
    }

    cJSON* user_msg = cJSON_CreateObject();

    cJSON_AddStringToObject(user_msg, "role", "user");
    cJSON_AddStringToObject(user_msg, "content", content);
    cJSON_AddItemToArray(messages, user_msg);
    return messages;
}

#define VOICE_MAX_HISTORY 4

static cJSON* build_messages_voice(const char* chat_id,
    const char* content,
    char* history_json, size_t hist_size)
{
    session_get_history_json(chat_id, history_json, hist_size,
        VOICE_MAX_HISTORY);

    cJSON* messages = cJSON_Parse(history_json);

    if (!messages) {
        messages = cJSON_CreateArray();
    }

    cJSON* user_msg = cJSON_CreateObject();

    cJSON_AddStringToObject(user_msg, "role", "user");
    cJSON_AddStringToObject(user_msg, "content", content);
    cJSON_AddItemToArray(messages, user_msg);
    return messages;
}

/* ── Inject session context into system prompt ───────────── */

static void inject_session_context(char* sys_prompt, size_t size,
    const char* channel, const char* chat_id)
{
    size_t len = strlen(sys_prompt);

    snprintf(sys_prompt + len, size - len,
        "\n## Current Session\n"
        "channel: %s\n"
        "chat_id: %s\n"
        "When using feishu_send_mention or feishu_chat_members, "
        "use the chat_id above unless the user specifies a "
        "different one.\n"
        "If the user message ends with "
        "[mentioned_users: name=open_id], "
        "use those open_ids when you need to @mention or remind "
        "those users. Do NOT include the [mentioned_users: ...] "
        "block in your reply text.\n",
        channel, chat_id);
}

/* ── Main agent loop task ─────────────────────────────────── */

static void* agent_loop_task(void* arg)
{
    (void)arg;
    agent_mem_status_t mem_st;

    agent_mem_get_status(&mem_st);
    syslog(LOG_INFO, "[%s] Agent loop started, free heap: %zu\n",
        TAG, mem_st.free_heap);

    size_t ctx_size = agent_mem_safe_size(
        AGENT_CONTEXT_BUF_SIZE, 4 * 1024);
    size_t hist_size = agent_mem_safe_size(
        AGENT_LLM_STREAM_BUF_SIZE, 8 * 1024);
    size_t tool_size = agent_mem_safe_size(
        TOOL_OUTPUT_SIZE, TOOL_OUTPUT_SIZE_MIN);

    char* sys_prompt = calloc(1, ctx_size);
    char* history_json = calloc(1, hist_size);
    char* tool_output = calloc(1, tool_size);

    if (!sys_prompt || !history_json || !tool_output) {
        syslog(LOG_ERR, "[%s] Failed to allocate agent buffers\n", TAG);
        free(sys_prompt);
        free(history_json);
        free(tool_output);
        return NULL;
    }

    syslog(LOG_INFO, "[%s] Buffers: ctx=%zu hist=%zu tool=%zu\n",
        TAG, ctx_size, hist_size, tool_size);

    /* Initialize memory pool for parallel tool outputs */
    size_t pool_buf_size = agent_mem_safe_size(
        TOOL_OUTPUT_SIZE_LARGE, TOOL_OUTPUT_SIZE_MIN);
    if (agent_pool_init(&s_tool_pool, pool_buf_size,
            AGENT_MAX_TOOL_CALLS)
        == OK) {
        s_pool_ready = true;
        syslog(LOG_INFO, "[%s] Tool output pool: %d x %zu bytes\n",
            TAG, AGENT_MAX_TOOL_CALLS, pool_buf_size);
    } else {
        syslog(LOG_WARNING,
            "[%s] Pool init failed, using heap fallback\n", TAG);
    }

    char* tools_json = tool_registry_get_tools_json();

    syslog(LOG_INFO, "[%s] Tools JSON loaded: %d bytes\n",
        TAG, tools_json ? (int)strlen(tools_json) : 0);

    while (!agent_shutdown_requested()) {
        agent_msg_t msg;
        int err = message_bus_pop_inbound(&msg, UINT32_MAX);

        if (err != OK) {
            continue;
        }

        if (agent_shutdown_requested()) {
            free(msg.content);
            free(msg.image_b64);
            break;
        }

        syslog(LOG_INFO, "[%s] Processing message from %s:%s\n",
            TAG, msg.channel, msg.chat_id);

        /* Check memory pressure */
        agent_mem_get_status(&mem_st);
        if (mem_st.free_heap < AGENT_MEM_RESERVE_BYTES) {
            syslog(LOG_WARNING,
                "[%s] Low memory: %zu bytes free, skipping\n",
                TAG, mem_st.free_heap);
            agent_msg_t out = { 0 };
            strncpy(out.channel, msg.channel,
                sizeof(out.channel) - 1);
            strncpy(out.chat_id, msg.chat_id,
                sizeof(out.chat_id) - 1);
            out.content = strdup("系统内存不足，请稍后再试。");
            if (out.content) {
                if (message_bus_push_outbound(&out) != OK) {
                    free(out.content);
                }
            }
            free(msg.content);
            continue;
        }

        /* Slash commands — fast path, bypass LLM */
        char* reply = handle_slash_command(&msg);

        /* Prompt injection check — before LLM */
        if (!reply && tool_guard_check_injection(msg.content)) {
            syslog(LOG_WARNING,
                "[%s] Prompt injection blocked from %s:%s\n",
                TAG, msg.channel, msg.chat_id);
            reply = strdup("I can't do that.");
            if (!reply) {
                /* Fail-closed: skip this message entirely */
                free(msg.content);
                continue;
            }
        }

        /* NL fast path — after injection check, before LLM */
        if (!reply) {
            reply = handle_nl_fast_path(msg.content);
        }

        /* If voice channel and NL fast path didn't match, but Layer1 in
         * voice_assistant had matched (setting s_feishu_fast_path=true),
         * reset the flag so cloud omni audio and silence timeout work
         * normally.  This prevents the scenario where Layer1 matches
         * (e.g. due to keyword "内容") but Layer2 doesn't, leaving the
         * fast-path flag stuck and blocking audio for 120+ seconds. */
        if (!reply && strcmp(msg.channel, AGENT_CHAN_VOICE) == 0) {
            if (voice_assistant_reset_feishu_fast_path) {
                voice_assistant_reset_feishu_fast_path();
            }
        }

        if (reply) {
            /* For voice channel, directly call voice_channel_speak() instead of
             * pushing to the outbound queue.  The outbound dispatch thread may
             * be stuck after WiFi disconnect/reconnect (observed in field logs:
             * thread 124 stops processing messages, no "Dispatching response"
             * log appears, cause unclear).  Direct invocation from agent_loop
             * ensures TTS is played reliably.  voice_channel_speak() uses
             * s_voice.speak_lock to serialize calls, so concurrent invocations
             * from the outbound dispatch thread (if it recovers) are safe. */
            if (strcmp(msg.channel, AGENT_CHAN_VOICE) == 0) {
                extern int ws_server_broadcast_typed(const char*,
                    const char*, const char*);
                ws_server_broadcast_typed("response_silent", reply,
                    AGENT_CHAN_VOICE);
                syslog(LOG_INFO,
                    "[%s] NL fast path: direct speak (%zu bytes)\n",
                    TAG, strlen(reply));
                int vret = voice_channel_speak(reply);
                if (vret != 0) {
                    syslog(LOG_ERR,
                        "[%s] NL fast path speak failed: %d\n", TAG, vret);
                }
                free(reply);
            } else {
                agent_msg_t out = { 0 };
                strncpy(out.channel, msg.channel,
                    sizeof(out.channel) - 1);
                strncpy(out.chat_id, msg.chat_id,
                    sizeof(out.chat_id) - 1);
                out.content = reply;
                if (message_bus_push_outbound(&out) != OK) {
                    free(reply);
                }
            }
            free(msg.content);
            continue;
        }

        /* Hot-reload skills if directory changed */
        if (skill_loader_check_changed()) {
            syslog(LOG_INFO, "[%s] Skills changed, refreshing\n", TAG);
            skill_loader_refresh();
        }

        context_build_system_prompt(sys_prompt, ctx_size);
        inject_session_context(sys_prompt, ctx_size,
            msg.channel, msg.chat_id);

        /* Refresh tools JSON — Node tools are dynamic */
        free(tools_json);
        tools_json = tool_registry_get_tools_json();

        cJSON* messages = NULL;
        if (strcmp(msg.channel, AGENT_CHAN_VOICE) == 0) {
            messages = build_messages_voice(msg.chat_id, msg.content,
                history_json, hist_size);
        } else {
            messages = build_messages(msg.chat_id, msg.content,
                history_json, hist_size);
        }

        char* final_text = NULL;

        /* Vision path */
        final_text = handle_vision_message(&msg);
        if (final_text) {
            cJSON_Delete(messages);
        } else if (strcmp(msg.channel, AGENT_CHAN_VOICE) == 0) {
            char quick_resp[256];
            if (msg.content
                && voice_quick_path_match_text(msg.content,
                    quick_resp, sizeof(quick_resp)) == 0) {
                syslog(LOG_INFO,
                    "[%s] quick path matched: \"%s\" -> \"%s\"\n",
                    TAG, msg.content, quick_resp);
                voice_channel_set_quick_path(1);
                final_text = strdup(quick_resp);
                if (msg.content)
                    session_append(msg.chat_id, "user", msg.content);
                session_append(msg.chat_id, "assistant", final_text);
                cJSON_Delete(messages);
            } else {
                s_llm_path_count++;
                int pipeline_ok = 0;
                final_text = run_voice_stream_path(sys_prompt, messages,
                    tools_json, &msg);
                if (final_text) {
                    free(final_text);
                    final_text = NULL;
                    pipeline_ok = 1;
                } else {
                    llm_set_all(NULL, NULL, NULL, NULL, AGENT_LLM_QWEN_MODEL);
                    final_text = run_react_loop(sys_prompt, messages,
                        tools_json, tool_output, tool_size, &msg);
                }
                cJSON_Delete(messages);
                if (pipeline_ok) {
                    free(final_text);
                    final_text = NULL;
                    goto voice_done;
                }
            }
        } else {
            /* ReAct loop */
            s_llm_path_count++;
            final_text = run_react_loop(sys_prompt, messages,
                tools_json, tool_output, tool_size, &msg);
            cJSON_Delete(messages);
        }

        dispatch_response(&msg, final_text);

voice_done:
        /* Free image_b64 if not already freed by vision path */
        free(msg.image_b64);
        msg.image_b64 = NULL;
        free(msg.content);
    }

    /* Cleanup (unreachable in normal operation) */
    if (s_pool_ready) {
        agent_pool_destroy(&s_tool_pool);
        s_pool_ready = false;
    }
    free(tools_json);
    free(sys_prompt);
    free(history_json);
    free(tool_output);
    return NULL;
}

/* ── Public interface ─────────────────────────────────────── */

int agent_loop_init(void)
{
    syslog(LOG_INFO, "[%s] Agent loop initialized\n", TAG);
    return OK;
}

int agent_loop_start(void)
{
    int ret = agent_task_create(agent_loop_task, "agent_loop",
        AGENT_AI_AGENT_STACK, NULL, AGENT_AI_AGENT_PRIO);

    if (ret != OK) {
        syslog(LOG_ERR,
            "[%s] Failed to create agent_loop task\n", TAG);
    }
    return ret;
}
