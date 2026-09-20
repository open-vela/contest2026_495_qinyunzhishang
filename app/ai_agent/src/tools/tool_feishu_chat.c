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

#include "tools/tool_feishu_chat.h"
#include "cJSON.h"
#include "channels/feishu_bot.h"
#ifdef CONFIG_AI_AGENT_NODE
#include "node/node_manager.h"
#endif
#include "agent_compat.h"
#include "agent_config.h"  /* for agent_tz_offset_sec() */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char* TAG = "tool_feishu_chat";

#define RESP_BUF_SIZE (32 * 1024)  /* Increased from 16KB to handle large message responses */

/* ── feishu_chat_members ─────────────────────────────────────── */

int tool_feishu_chat_members_execute(const char* input_json, char* output,
    size_t output_size)
{
    cJSON* input = cJSON_Parse(input_json);
    if (!input) {
        snprintf(output, output_size, "Error: invalid JSON input");
        return ERROR;
    }

    const char* chat_id = cJSON_GetStringValue(cJSON_GetObjectItem(input, "chat_id"));
    if (!chat_id || !chat_id[0]) {
        snprintf(output, output_size, "Error: chat_id is required");
        cJSON_Delete(input);
        return ERROR;
    }

    /* GET /open-apis/im/v1/chats/{chat_id}/members?member_id_type=open_id */
    char path[256];
    snprintf(path, sizeof(path),
        "/open-apis/im/v1/chats/%s/members?member_id_type=open_id", chat_id);

    char* resp = malloc(RESP_BUF_SIZE);
    if (!resp) {
        snprintf(output, output_size, "Error: OOM");
        cJSON_Delete(input);
        return ERROR;
    }

    int status = feishu_api_request("GET", path, NULL, 0, resp, RESP_BUF_SIZE);
    cJSON_Delete(input);

    if (status != 200) {
        snprintf(output, output_size, "Error: Feishu API HTTP %d: %.200s", status,
            resp);
        free(resp);
        return ERROR;
    }

    /* Parse response:
     * { "code":0, "data":{ "items":[
     *     {"member_id":"ou_xxx","name":"nana","member_id_type":"open_id"}, ...
     * ] } }
     */
    cJSON* root = cJSON_Parse(resp);
    free(resp);
    if (!root) {
        snprintf(output, output_size, "Error: invalid JSON from Feishu API");
        return ERROR;
    }

    cJSON* code = cJSON_GetObjectItem(root, "code");
    if (!cJSON_IsNumber(code) || code->valueint != 0) {
        cJSON* msg = cJSON_GetObjectItem(root, "msg");
        snprintf(output, output_size, "Error: Feishu API code=%d msg=%s",
            cJSON_IsNumber(code) ? (int)code->valueint : -1,
            (msg && cJSON_IsString(msg)) ? msg->valuestring : "unknown");
        cJSON_Delete(root);
        return ERROR;
    }

    cJSON* data = cJSON_GetObjectItem(root, "data");
    cJSON* items = data ? cJSON_GetObjectItem(data, "items") : NULL;

    /* Build simplified output: [{name, open_id}, ...] */
    cJSON* result = cJSON_CreateArray();
    if (items && cJSON_IsArray(items)) {
        cJSON* item;
        cJSON_ArrayForEach(item, items)
        {
            cJSON* name_j = cJSON_GetObjectItem(item, "name");
            cJSON* mid_j = cJSON_GetObjectItem(item, "member_id");
            if (cJSON_IsString(mid_j)) {
                cJSON* entry = cJSON_CreateObject();
                cJSON_AddStringToObject(
                    entry, "name", cJSON_IsString(name_j) ? name_j->valuestring : "");
                cJSON_AddStringToObject(entry, "open_id", mid_j->valuestring);
                cJSON_AddItemToArray(result, entry);
            }
        }
    }

    char* out_str = cJSON_PrintUnformatted(result);
    cJSON_Delete(result);
    cJSON_Delete(root);

    if (out_str) {
        snprintf(output, output_size, "%s", out_str);
        free(out_str);
    } else {
        snprintf(output, output_size, "[]");
    }

    syslog(LOG_INFO, "[%s] chat_members for %s done\n", TAG, chat_id);
    return OK;
}

/* ── feishu_send_mention ─────────────────────────────────────── */

int tool_feishu_send_mention_execute(const char* input_json, char* output,
    size_t output_size)
{
    cJSON* input = cJSON_Parse(input_json);
    if (!input) {
        snprintf(output, output_size, "Error: invalid JSON input");
        return ERROR;
    }

    const char* chat_id_raw = cJSON_GetStringValue(cJSON_GetObjectItem(input, "chat_id"));
    const char* open_id_raw = cJSON_GetStringValue(cJSON_GetObjectItem(input, "open_id"));
    const char* name_raw = cJSON_GetStringValue(cJSON_GetObjectItem(input, "name"));
    const char* text_raw = cJSON_GetStringValue(cJSON_GetObjectItem(input, "text"));

    if (!chat_id_raw || !chat_id_raw[0]) {
        snprintf(output, output_size, "Error: chat_id is required");
        cJSON_Delete(input);
        return ERROR;
    }
    if (!open_id_raw || !open_id_raw[0]) {
        snprintf(output, output_size, "Error: open_id is required");
        cJSON_Delete(input);
        return ERROR;
    }
    if (!text_raw)
        text_raw = "";
    if (!name_raw || !name_raw[0])
        name_raw = "user";

    /* Copy values before freeing input cJSON */
    char* chat_id = strdup(chat_id_raw);
    char* open_id = strdup(open_id_raw);
    char* name = strdup(name_raw);
    char* text = strdup(text_raw);
    cJSON_Delete(input);

    if (!chat_id || !open_id || !name || !text) {
        snprintf(output, output_size, "Error: OOM");
        free(chat_id);
        free(open_id);
        free(name);
        free(text);
        return ERROR;
    }

    /* Feishu @mention format in text message:
     * content: {"text":"<at user_id=\"ou_xxx\">name</at> message text"}
     *
     * The <at> tag is a special Feishu rich-text marker that renders
     * as an @mention in the chat UI and sends a notification.
     */

    /* Build content string with @mention tag */
    cJSON* content_obj = cJSON_CreateObject();
    size_t mention_len = strlen(open_id) + strlen(name) + strlen(text) + 64;
    char* mention_text = malloc(mention_len);
    if (!mention_text) {
        snprintf(output, output_size, "Error: OOM");
        cJSON_Delete(content_obj);
        free(chat_id);
        free(open_id);
        free(name);
        free(text);
        return ERROR;
    }
    snprintf(mention_text, mention_len, "<at user_id=\"%s\">%s</at> %s",
             open_id, name, text);
    cJSON_AddStringToObject(content_obj, "text", mention_text);
    free(mention_text);

    char* content_str = cJSON_PrintUnformatted(content_obj);
    cJSON_Delete(content_obj);

    if (!content_str) {
        snprintf(output, output_size, "Error: OOM building content");
        free(chat_id);
        free(open_id);
        free(name);
        free(text);
        return ERROR;
    }

    /* Build request body */
    cJSON* body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "receive_id", chat_id);
    cJSON_AddStringToObject(body, "msg_type", "text");
    cJSON_AddStringToObject(body, "content", content_str);
    free(content_str);

    char* body_str = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);

    if (!body_str) {
        snprintf(output, output_size, "Error: OOM");
        free(chat_id);
        free(open_id);
        free(name);
        free(text);
        return ERROR;
    }

    char* resp = malloc(4096);
    if (!resp) {
        free(body_str);
        snprintf(output, output_size, "Error: OOM");
        free(chat_id);
        free(open_id);
        free(name);
        free(text);
        return ERROR;
    }

    syslog(LOG_INFO, "[%s] Sending @mention to %s in %s\n", TAG, open_id,
        chat_id);

    int status = feishu_api_post("/open-apis/im/v1/messages?receive_id_type=chat_id",
        body_str, resp, 4096);
    free(body_str);

    if (status != 200 && status != 201) {
        snprintf(output, output_size, "Error: send failed HTTP %d: %.200s", status,
            resp);
        free(resp);
        free(chat_id);
        free(open_id);
        free(name);
        free(text);
        return ERROR;
    }

    /* Parse response for message_id */
    cJSON* root = cJSON_Parse(resp);
    free(resp);

    if (root) {
        cJSON* code = cJSON_GetObjectItem(root, "code");
        if (cJSON_IsNumber(code) && code->valueint != 0) {
            cJSON* msg = cJSON_GetObjectItem(root, "msg");
            snprintf(output, output_size, "Error: code=%d msg=%s",
                (int)code->valueint,
                (msg && cJSON_IsString(msg)) ? msg->valuestring : "?");
            cJSON_Delete(root);
            free(chat_id);
            free(open_id);
            free(name);
            free(text);
            return ERROR;
        }
        cJSON_Delete(root);
    }

    snprintf(output, output_size,
        "@mention message sent successfully to %s in the chat. "
        "No further action needed for this mention.",
        name);
    syslog(LOG_INFO, "[%s] @mention message sent to %s\n", TAG, name);

    /* Forward the message to connected nodes so bot-to-bot @mentions
     * work even when the platform doesn't push events between bots. */
#ifdef CONFIG_AI_AGENT_NODE
    if (node_manager_active_count() > 0) {
        char fwd_text[512];
        snprintf(fwd_text, sizeof(fwd_text), "<at user_id=\"%s\">%s</at> %s",
            open_id, name, text);
        node_manager_broadcast_chat("feishu", chat_id, fwd_text);
    }
#endif

    free(chat_id);
    free(open_id);
    free(name);
    free(text);
    return OK;
}

/* ── feishu_message_today: cursor-based fetch + multi-signal scoring ── */

#define MSG_TODAY_MAX_CHATS    20  /* max chats to scan */
#define MSG_SCORE_THRESHOLD    30  /* min score to include message (0-100) */

/* ── Multi-signal scoring: mimics LLM's importance assessment ─────── */

/* Signal 1: Action verbs (30 pts) — messages requesting someone to do something */
static const char* g_action_verbs[] = {
    "请", "需要", "务必", "确认", "回复", "完成", "处理",
    "审核", "审批", "修改", "更新", "提交", "发送",
    "检查", "测试", "修复", "部署", "安排", "准备", "整理",
    "跟进", "推动", "协调", "提供", "补充", "完善", "落实",
    /* 职场高频动作 */
    "同步", "对齐", "负责", "邀请", "上传", "下载",
    "麻烦", "拉群",
    /* English action verbs */
    "please", "need to", "must", "confirm", "reply", "complete",
    "handle", "approve", "modify", "update", "submit", "send",
    "check", "test", "fix", "deploy", "arrange", "prepare",
    "organize", "follow up", "coordinate", "provide", "review",
    "approve", "schedule", "assign", "conduct", "prepare",
    "sync", "align", "responsible", "invite", "upload",
    "download", "share", "discuss", "escalate", "investigate",
    NULL
};

/* Signal 2: Question markers (25 pts) — messages asking for information/decision */
static const char* g_question_markers[] = {
    "?", "？", "吗", "呢", "怎么", "什么", "哪", "是否",
    "能不能", "可不可以", "为什么", "多久", "多少",
    "有没有", "是不是", "对不对", "行不行",
    /* 补充疑问词 */
    "谁", "啥", "咋", "几", "如何", "何时", "能否", "请问",
    "什么时候", "哪天", "哪位",
    /* English question markers */
    "what", "when", "where", "who", "why", "which",
    "can you", "could you", "would you", "will you",
    "is it", "are we", "do we", "should we",
    "any update", "any progress", "any news",
    "how about", "what about", "how long",
    "how many", "how much", "how to",
    "are you", "do you", "did you", "have you",
    NULL
};

/* Signal 3: Urgency markers (25 pts) — time-critical messages */
static const char* g_urgency_markers[] = {
    "紧急", "尽快", "马上", "立即", "ASAP", "asap",
    "赶紧", "加急", "重要", "优先", "今天内", "现在",
    "立刻", "随时",
    /* 补充紧急/提醒词 */
    "急", "催", "第一时间", "别忘", "记得", "千万别",
    /* English urgency markers */
    "urgent", "immediately", "as soon as possible",
    "right away", "important", "priority", "critical",
    "quickly", "today", "now", "don't forget",
    "reminder", "deadline", "last chance", "act now",
    NULL
};

/* Signal 4: Issues/risks (20 pts) — problems that need attention */
static const char* g_issue_markers[] = {
    "bug", "Bug", "BUG", "故障", "报错", "异常",
    "失败", "崩溃", "风险", "阻塞", "卡住", "挂了",
    "错误", "warning", "error", "crash",
    /* 补充问题/风险词 */
    "问题", "issue", "Issue", "事故", "延期", "超时",
    "不对", "不行", "没法", "卡", "死机", "漏",
    /* English issue/risk markers */
    "failed", "failure", "broken", "blocked", "stuck",
    "risk", "timeout", "wrong", "incident", "delay",
    "outage", "down", "crash", "fault", "defect",
    NULL
};

/* Signal 5: Decision/approval (20 pts) — requires a decision */
static const char* g_decision_markers[] = {
    "同意", "拒绝", "批准", "驳回", "决定", "选择",
    "通过", "否决", "待定", "取消",
    /* 补充决策/审批词 */
    "认可", "赞成", "反对", "拍板", "敲定",
    "暂缓", "延后", "搁置", "没意见",
    /* English decision/approval markers */
    "agree", "reject", "approve", "decline", "decide",
    "choose", "accept", "deny", "pending", "cancel",
    "approved", "rejected", " LGTM", "lgtm",
    NULL
};

/* Signal 6: Status changes (15 pts) — important status updates */
static const char* g_status_markers[] = {
    "上线", "发布", "交付", "关闭", "启动", "结束",
    "已解决", "已修复", "合并", "验收",
    /* 补充状态变更词 */
    "搞定", "做完", "结项", "归档", "停用",
    "生效", "迁移", "升级", "回滚", "已读",
    /* English status markers */
    "deployed", "released", "delivered", "closed",
    "started", "finished", "resolved", "fixed",
    "merged", "done", "completed", "archived",
    "migrated", "upgraded", "rolled back",
    "shipped", "live", "stable",
    NULL
};

/* Signal 7: Work schedule & life event keywords (15 pts) */
static const char* g_filter_keywords[] = {
    /* 工作日程 */
    "日程", "面试", "会议", "开会", "会议室", "培训", "评审",
    "演示", "汇报", "签约", "截止", "到期", "deadline",
    "提醒", "通知", "安排", "预约", "候选人", "offer",
    "onboard", "入职", "离职", "调休", "请假", "加班",
    "项目", "上线", "发版", "迭代", "sprint",
    "开个会", "有个会", "例会", "早会", "晚会", "大会",
    "碰头会", "沟通会", "同步会", "复盘会", "启动会",
    "评审会", "面试会", "宣讲会", "交流会", "分享会",
    /* 工作文档 */
    "文档", "ppt", "PPT", "报告", "方案", "材料",
    "需求", "设计", "代码", "计划", "总结",
    "周报", "月报", "宣传", "合同", "协议",
    /* 工作事务 */
    "任务", "进度", "里程碑", "目标", "排期",
    "报销", "预算", "客户", "版本", "权限",
    "账号", "工单", "review", "Review",
    /* 生活聚会 */
    "聚会", "聚餐", "吃饭", "约饭", "饭局",
    "爬山", "徒步", "登山", "露营", "野餐",
    "KTV", "唱歌", "火锅", "烧烤", "生日",
    "婚礼", "喜酒", "满月", "乔迁",
    "团建", "出差", "放假", "值班", "迟到",
    /* English work schedule & life event keywords */
    "schedule", "interview", "meeting", "standup",
    "stand-up", "scrum", "training", "demo",
    "presentation", "report", "contract", "reminder",
    "notice", "appointment", "candidate", "resign",
    "leave", "overtime", "project", "release",
    "iteration", "document", "proposal", "requirement",
    "design", "plan", "summary", "weekly", "monthly",
    "task", "progress", "milestone", "goal",
    "budget", "customer", "version", "permission",
    "ticket", "sprint", "backlog", "roadmap",
    /* English life events */
    "party", "dinner", "lunch", "birthday",
    "wedding", "team building", "business trip",
    "vacation", "holiday", "on call", "outing",
    NULL
};

/* Noise filter: short acknowledgment messages to skip */
static const char* g_noise_phrases[] = {
    "收到", "好的", "好的好的", "了解", "嗯", "ok", "OK",
    "Ok", "+1", "知道了", "明白", "收到收到", "好",
    "ok！", "收到！", "了解了", "好的。", "嗯嗯",
    /* 补充常见短语回复 */
    "好嘞", "可以", "没问题", "对的", "是的",
    "好滴", "嗯呢", "收到啦", "收到哈", "好的哈",
    "好的呀", "嗯嗯嗯", "好的！", "收到。", "好。",
    /* English noise phrases */
    "ok", "okay", "okk", "kk", "k", "got it",
    "received", "understood", "sure", "yes", "no",
    "thanks", "thank you", "thx", "done", "will do",
    "noted", "agreed", "cool", "great", "nice",
    "lol", "haha", "sounds good", "no problem",
    "sounds good!", "gotcha", "roger", "ack",
    "np", "yw", "fyi", "nvm", "brb",
    NULL
};

/* Fuzzy match: "开...会" where ... is 1~6 UTF-8 chars */
static int fuzzy_match_meeting(const char* text) {
    const char* p = text;
    while ((p = strstr(p, "开")) != NULL) {
        const char* q = p + 3; /* skip '开' (3 bytes UTF-8) */
        for (int skip = 0; skip < 6 && *q; skip++) {
            if ((unsigned char)q[0] == 0xE4 &&
                (unsigned char)q[1] == 0xBC &&
                (unsigned char)q[2] == 0x9A) {
                return 1;
            }
            int clen = 1;
            if ((unsigned char)q[0] >= 0xE0) clen = 3;
            else if ((unsigned char)q[0] >= 0xC0) clen = 2;
            q += clen;
        }
        p += 3;
    }
    return 0;
}

/* Check if text contains specific time expressions (numeric time, dates,
 * relative time words like 明天/今日/本周 etc.) */
static int contains_time_date(const char* text) {
    /* Relative time words */
    static const char* rel_time[] = {
        "明天", "今日", "今天", "明日", "后天", "昨天",
        "下周", "本周", "这周", "今晚", "今明",
        "本月", "下月", "月底", "年初", "年底",
        /* 补充时段词 */
        "上午", "下午", "晚上", "中午", "早上",
        "周末", "凌晨", "年底前", "节前",
        /* English relative time words */
        "tomorrow", "today", "tonight", "yesterday",
        "next week", "this week", "next month",
        "this month", "morning", "afternoon",
        "evening", "noon", "weekend", "midnight",
        "EOD", "eod", "COB", "cob",
        NULL
    };
    for (int i = 0; rel_time[i]; i++) {
        if (strcasestr(text, rel_time[i])) return 1;
    }
    /* English day-of-week names */
    static const char* weekdays[] = {
        "monday", "tuesday", "wednesday", "thursday",
        "friday", "saturday", "sunday",
        "mon", "tue", "wed", "thu", "fri", "sat", "sun",
        NULL
    };
    for (int i = 0; weekdays[i]; i++) {
        if (strcasestr(text, weekdays[i])) return 1;
    }
    /* Date pattern: digit + "." + digit (e.g. "7.31", "12.01") */
    for (const char* p = text; *p; p++) {
        if (*p >= '0' && *p <= '9') {
            const char* q = p + 1;
            while (*q >= '0' && *q <= '9') q++;
            if (*q == '.' && q[1] >= '0' && q[1] <= '9') return 1;
        }
    }
    /* Numeric time patterns: digit + ":" (e.g. "4:56", "16:00") */
    for (const char* p = text; *p; p++) {
        if (*p >= '0' && *p <= '9') {
            const char* q = p + 1;
            while (q - p <= 2 && *q >= '0' && *q <= '9') q++;
            if (*q == ':') return 1;
        }
    }
    /* English AM/PM patterns: digit + "am"/"pm" (e.g. "3pm", "10am", "3 pm") */
    for (const char* p = text; *p; p++) {
        if (*p >= '0' && *p <= '9') {
            const char* q = p + 1;
            while (q - p <= 2 && *q >= '0' && *q <= '9') q++;
            /* skip optional space */
            if (*q == ' ') q++;
            if ((q[0] == 'a' || q[0] == 'A') &&
                (q[1] == 'm' || q[1] == 'M')) return 1;
            if ((q[0] == 'p' || q[0] == 'P') &&
                (q[1] == 'm' || q[1] == 'M')) return 1;
        }
    }
    /* "X点" / "X时" / "X号" where X is a digit or Chinese number */
    static const char* time_units[] = {"点", "时", "号"};
    for (int i = 0; i < 3; i++) {
        const char* p = text;
        while ((p = strstr(p, time_units[i])) != NULL) {
            if (p > text) {
                unsigned char prev = (unsigned char)*(p - 1);
                if (prev >= '0' && prev <= '9') return 1;
                if (p >= text + 3) {
                    unsigned char b0 = (unsigned char)*(p - 3);
                    unsigned char b1 = (unsigned char)*(p - 2);
                    unsigned char b2 = (unsigned char)*(p - 1);
                    if (b0 == 0xE4) {
                        if ((b1 == 0xB8 && (b2 == 0x80 || b2 == 0x89 ||
                                            b2 == 0x93 || b2 == 0xA4)) ||
                            (b1 == 0xBA && (b2 == 0x8C || b2 == 0x94)) ||
                            (b1 == 0xB9 && b2 == 0x9D)) {
                            return 1;
                        }
                    }
                    else if (b0 == 0xE5) {
                        if ((b1 == 0x9B && b2 == 0x9B) ||
                            (b1 == 0x85 && (b2 == 0xAD || b2 == 0xAB)) ||
                            (b1 == 0x8D && b2 == 0x81)) {
                            return 1;
                        }
                    }
                }
            }
            p += 3;
        }
    }
    /* "X点半" / "X点整" */
    if (strstr(text, "点半") || strstr(text, "点整")) return 1;
    /* "X月X日" / "X月X号" — only match digit before 月, not any Chinese char */
    if (strstr(text, "月") && (strstr(text, "日") || strstr(text, "号"))) {
        for (const char* p = text; *p; p++) {
            if (*p == '月' && p > text) {
                unsigned char prev = (unsigned char)*(p - 1);
                if (prev >= '0' && prev <= '9') return 1;
            }
        }
    }
    return 0;
}

/* Helper: check if text contains any keyword from array */
static int contains_any_kw(const char* text, const char* const kws[]) {
    for (int i = 0; kws[i]; i++) {
        if (strcasestr(text, kws[i]))
            return 1;
    }
    return 0;
}

/* Check if text is noise (short acknowledgment) */
static int is_noise(const char* text) {
    size_t len = strlen(text);
    /* Very short messages (< 6 bytes ~ 2 Chinese chars) likely noise */
    if (len < 6) return 1;
    for (int i = 0; g_noise_phrases[i]; i++) {
        if (strcmp(text, g_noise_phrases[i]) == 0)
            return 1;
    }
    return 0;
}

/* Multi-signal importance scoring.
 * Returns score 0-100; messages with score >= MSG_SCORE_THRESHOLD are included.
 * Mimics LLM's semantic importance assessment using explicit weighted rules. */
static int score_message(const char* text) {
    /* Noise filter: skip short acknowledgments */
    if (is_noise(text))
        return 0;

    int score = 0;

    /* Signal 1: Action verbs (30 pts) */
    if (contains_any_kw(text, g_action_verbs))
        score += 30;

    /* Signal 2: Question markers (25 pts) */
    if (contains_any_kw(text, g_question_markers))
        score += 25;

    /* Signal 3: Urgency markers (25 pts) */
    if (contains_any_kw(text, g_urgency_markers))
        score += 25;

    /* Signal 4: Issues/risks (20 pts) */
    if (contains_any_kw(text, g_issue_markers))
        score += 20;

    /* Signal 5: Decision/approval (20 pts) */
    if (contains_any_kw(text, g_decision_markers))
        score += 20;

    /* Signal 6: Status changes (15 pts) */
    if (contains_any_kw(text, g_status_markers))
        score += 15;

    /* Signal 7: Work schedule & life event keywords (15 pts) */
    if (contains_any_kw(text, g_filter_keywords))
        score += 15;

    /* Signal 8: Fuzzy meeting match "开...会" (15 pts) */
    if (fuzzy_match_meeting(text))
        score += 15;

    /* Signal 9: Time/date references (15 pts) */
    if (contains_time_date(text))
        score += 15;

    /* Signal 10: @mentions (10 pts) */
    if (strchr(text, '@'))
        score += 10;

    return score;
}

/* ── Message entry for today's messages ─────────────────────────── */

#define MAX_MSG_ENTRIES     200   /* max messages to collect */

typedef struct {
    char text[224];       /* message text (truncated to 200 + margin) */
    char sender[48];      /* sender name */
    char chat_name[96];   /* chat/group name */
    int  chat_idx;        /* chat index for grouping */
} msg_entry_t;

/* Extract text from message body content.
 * For text type: content is {"text":"actual message"}
 * For post type: content is {"zh_cn":{"title":"...","content":[...]}}
 * Returns heap-allocated string, caller must free. */
static char* extract_msg_text(cJSON* msg)
{
    cJSON* body = cJSON_GetObjectItem(msg, "body");
    if (!body) return NULL;

    cJSON* content_j = cJSON_GetObjectItem(body, "content");
    if (!cJSON_IsString(content_j) || !content_j->valuestring[0]) return NULL;

    cJSON* msg_type = cJSON_GetObjectItem(msg, "msg_type");
    const char* mtype = cJSON_IsString(msg_type) ? msg_type->valuestring : "";

    if (strcmp(mtype, "text") == 0) {
        /* content: {"text":"message text"} */
        cJSON* cj = cJSON_Parse(content_j->valuestring);
        if (cj) {
            cJSON* text_j = cJSON_GetObjectItem(cj, "text");
            char* result = cJSON_IsString(text_j) ? strdup(text_j->valuestring) : NULL;
            cJSON_Delete(cj);
            return result;
        }
    } else if (strcmp(mtype, "post") == 0) {
        /* content: {"zh_cn":{"content":[[{"tag":"text","text":"..."},...]]}} */
        cJSON* cj = cJSON_Parse(content_j->valuestring);
        if (cj) {
            cJSON* lang = cJSON_GetObjectItem(cj, "zh_cn");
            if (!lang) lang = cJSON_GetObjectItem(cj, "en_us");
            cJSON* content_arr = lang ? cJSON_GetObjectItem(lang, "content") : NULL;
            if (content_arr && cJSON_IsArray(content_arr)) {
                char buf[1024] = {0};
                size_t off = 0;
                cJSON* para;
                cJSON_ArrayForEach(para, content_arr) {
                    cJSON* elem;
                    cJSON_ArrayForEach(elem, para) {
                        cJSON* txt = cJSON_GetObjectItem(elem, "text");
                        if (cJSON_IsString(txt) && off < sizeof(buf) - 2) {
                            off += snprintf(buf + off, sizeof(buf) - off, "%s", txt->valuestring);
                        }
                    }
                    if (off < sizeof(buf) - 2) buf[off++] = ' ';
                }
                cJSON_Delete(cj);
                return buf[0] ? strdup(buf) : NULL;
            }
            cJSON_Delete(cj);
        }
    }

    /* Non-text message types */
    if (strcmp(mtype, "image") == 0) return strdup("[图片]");
    if (strcmp(mtype, "file") == 0) return strdup("[文件]");
    if (strcmp(mtype, "audio") == 0) return strdup("[语音]");
    if (strcmp(mtype, "interactive") == 0) return strdup("[卡片]");
    return strdup("[非文本消息]");
}

int tool_feishu_message_today_execute(const char* input_json, char* output,
    size_t output_size)
{
    (void)input_json;

    /* Calculate today's time boundaries */
    time_t now_t = time(NULL);
    int tz_offset = agent_tz_offset_sec();
    time_t local_now = now_t + tz_offset;
    time_t local_midnight = local_now - (local_now % 86400);
    time_t today_start = local_midnight - tz_offset;
    time_t today_end = now_t;

    syslog(LOG_INFO, "[%s] msg_today: cursor-based fetch from 00:00\n", TAG);

    /* Step 1: Fetch chat list ONCE */
    char* chats_resp = malloc(RESP_BUF_SIZE);
    if (!chats_resp) {
        snprintf(output, output_size, "Error: OOM");
        return ERROR;
    }
    int status = feishu_api_request("GET",
        "/open-apis/im/v1/chats?page_size=20&user_id_type=open_id",
        NULL, 0, chats_resp, RESP_BUF_SIZE);
    if (status != 200) {
        snprintf(output, output_size, "Error: list chats HTTP %d: %.200s",
            status, chats_resp);
        free(chats_resp);
        return ERROR;
    }
    cJSON* chats_root = cJSON_Parse(chats_resp);
    free(chats_resp);
    if (!chats_root) {
        snprintf(output, output_size, "Error: invalid JSON from chats API");
        return ERROR;
    }
    cJSON* code = cJSON_GetObjectItem(chats_root, "code");
    if (!cJSON_IsNumber(code) || code->valueint != 0) {
        cJSON* msg = cJSON_GetObjectItem(chats_root, "msg");
        snprintf(output, output_size, "Error: chats code=%d msg=%s",
            cJSON_IsNumber(code) ? (int)code->valueint : -1,
            (msg && cJSON_IsString(msg)) ? msg->valuestring : "unknown");
        cJSON_Delete(chats_root);
        return ERROR;
    }
    cJSON* data = cJSON_GetObjectItem(chats_root, "data");
    cJSON* items = data ? cJSON_GetObjectItem(data, "items") : NULL;
    if (!items || !cJSON_IsArray(items)) {
        snprintf(output, output_size, "No chats found");
        cJSON_Delete(chats_root);
        return OK;
    }
    int total_chats = cJSON_GetArraySize(items);
    syslog(LOG_INFO, "[%s] msg_today: bot joined %d chats\n", TAG, total_chats);

    /* Step 2: Cursor-based pagination per chat.
     * Query from midnight to now; record last msg timestamp;
     * next query starts from last_timestamp+1s until 0 messages. */
    int total_matched = 0;
    int total_scanned = 0;
    output[0] = '\0';

    /* Allocate array to collect scored messages */
    msg_entry_t* msg_entries = (msg_entry_t*)malloc(
        MAX_MSG_ENTRIES * sizeof(msg_entry_t));
    int msg_count = 0;
    if (!msg_entries) {
        snprintf(output, output_size, "Error: OOM for msg_entries");
        cJSON_Delete(chats_root);
        return ERROR;
    }

    int chat_idx = 0;
    cJSON* chat;
    cJSON_ArrayForEach(chat, items) {
        if (chat_idx >= MSG_TODAY_MAX_CHATS) break;

        cJSON* chat_id_j = cJSON_GetObjectItem(chat, "chat_id");
        cJSON* name_j = cJSON_GetObjectItem(chat, "name");
        const char* chat_id = cJSON_IsString(chat_id_j) ? chat_id_j->valuestring : NULL;
        const char* chat_name = cJSON_IsString(name_j) ? name_j->valuestring : "未命名群";

        if (!chat_id || !chat_id[0]) { chat_idx++; continue; }

        /* 先获取群成员并缓存，以便后续查找发送者姓名 */
        {
            char members_path[256];
            snprintf(members_path, sizeof(members_path),
                "/open-apis/im/v1/chats/%s/members?member_id_type=open_id",
                chat_id);
            char* members_resp = malloc(RESP_BUF_SIZE);
            if (members_resp) {
                int mstatus = feishu_api_request("GET", members_path, NULL, 0,
                    members_resp, RESP_BUF_SIZE);
                if (mstatus == 200) {
                    cJSON* mroot = cJSON_Parse(members_resp);
                    if (mroot) {
                        cJSON* mcode = cJSON_GetObjectItem(mroot, "code");
                        if (cJSON_IsNumber(mcode) && mcode->valueint == 0) {
                            cJSON* mdata = cJSON_GetObjectItem(mroot, "data");
                            cJSON* mitems = mdata ? cJSON_GetObjectItem(mdata, "items") : NULL;
                            if (mitems && cJSON_IsArray(mitems)) {
                                /* API返回 member_id，需转换为 open_id */
                                cJSON* converted = cJSON_CreateArray();
                                cJSON* mi;
                                cJSON_ArrayForEach(mi, mitems) {
                                    cJSON* name_j = cJSON_GetObjectItem(mi, "name");
                                    cJSON* mid_j = cJSON_GetObjectItem(mi, "member_id");
                                    if (cJSON_IsString(mid_j)) {
                                        cJSON* entry = cJSON_CreateObject();
                                        cJSON_AddStringToObject(entry, "name",
                                            cJSON_IsString(name_j) ? name_j->valuestring : "");
                                        cJSON_AddStringToObject(entry, "open_id", mid_j->valuestring);
                                        cJSON_AddItemToArray(converted, entry);
                                    }
                                }
                                char* json_str = cJSON_PrintUnformatted(converted);
                                if (json_str) {
                                    feishu_recv_cache_members(json_str);
                                    free(json_str);
                                }
                                cJSON_Delete(converted);
                            }
                        }
                        cJSON_Delete(mroot);
                    }
                }
                free(members_resp);
            }
            syslog(LOG_INFO, "[%s] msg_today: chat '%s' members cached\n", TAG, chat_name);
        }

        time_t cursor = today_start; /* start from midnight */
        int page = 0;

        while (cursor < today_end) {
            char start_ts[32], end_ts[32];
            snprintf(start_ts, sizeof(start_ts), "%lld", (long long)cursor);
            snprintf(end_ts, sizeof(end_ts), "%lld", (long long)today_end);

            char msg_path[512];
            snprintf(msg_path, sizeof(msg_path),
                "/open-apis/im/v1/messages?container_id_type=chat"
                "&container_id=%s&start_time=%s&end_time=%s"
                "&page_size=50&sort_type=ByCreateTimeAsc",
                chat_id, start_ts, end_ts);

            char* msg_resp = malloc(RESP_BUF_SIZE);
            if (!msg_resp) break;
            int mstatus = feishu_api_request("GET", msg_path, NULL, 0,
                msg_resp, RESP_BUF_SIZE);
            if (mstatus != 200) {
                free(msg_resp); break;
            }
            cJSON* msg_root = cJSON_Parse(msg_resp);
            free(msg_resp);
            if (!msg_root) break;
            cJSON* mcode = cJSON_GetObjectItem(msg_root, "code");
            if (!cJSON_IsNumber(mcode) || mcode->valueint != 0) {
                cJSON_Delete(msg_root); break;
            }
            cJSON* mdata = cJSON_GetObjectItem(msg_root, "data");
            cJSON* mitems = mdata ? cJSON_GetObjectItem(mdata, "items") : NULL;
            int item_count = (!mitems || !cJSON_IsArray(mitems)) ? 0 : cJSON_GetArraySize(mitems);

            if (item_count == 0) {
                cJSON_Delete(msg_root);
                break; /* no more messages, stop cursor loop */
            }

            /* Track last message timestamp for next cursor */
            time_t last_create_sec = 0;

            /* Process messages: filter by keywords immediately */
            cJSON* mitem;
            cJSON_ArrayForEach(mitem, mitems) {
                if (msg_count >= MAX_MSG_ENTRIES) break;

                /* Record last create_time for cursor */
                cJSON* ct_j = cJSON_GetObjectItem(mitem, "create_time");
                if (cJSON_IsString(ct_j)) {
                    time_t ct = (time_t)(atoll(ct_j->valuestring) / 1000); /* ms -> s */
                    if (ct > last_create_sec) last_create_sec = ct;
                }

                char* text = extract_msg_text(mitem);
                if (!text || !text[0]) { free(text); continue; }

                int tlen = (int)strlen(text);
                if (tlen > 200) text[200] = '\0';

                if (score_message(text) >= MSG_SCORE_THRESHOLD) {
                    /* 提取发送者信息 */
                    char sender_name[48] = {0};
                    cJSON *sender = cJSON_GetObjectItem(mitem, "sender");
                    if (cJSON_IsObject(sender)) {
                        cJSON *id_type = cJSON_GetObjectItem(sender, "id_type");
                        cJSON *sender_id = cJSON_GetObjectItem(sender, "id");
                        if (cJSON_IsString(id_type) &&
                            strcmp(id_type->valuestring, "app_id") == 0) {
                            strncpy(sender_name, "小Q", sizeof(sender_name) - 1);
                        } else if (cJSON_IsString(id_type) && cJSON_IsString(sender_id) &&
                                   strcmp(id_type->valuestring, "open_id") == 0) {
                            if (!feishu_recv_find_member_by_open_id(sender_id->valuestring,
                                    sender_name, sizeof(sender_name))) {
                                snprintf(sender_name, sizeof(sender_name),
                                    "%.8s...", sender_id->valuestring);
                            }
                        }
                    }
                    /* Save to array */
                    strncpy(msg_entries[msg_count].text, text, 223);
                    msg_entries[msg_count].text[223] = '\0';
                    strncpy(msg_entries[msg_count].sender, sender_name, 47);
                    msg_entries[msg_count].sender[47] = '\0';
                    strncpy(msg_entries[msg_count].chat_name, chat_name, 95);
                    msg_entries[msg_count].chat_name[95] = '\0';
                    msg_entries[msg_count].chat_idx = chat_idx;
                    msg_count++;
                    total_matched++;
                }
                free(text);
                total_scanned++;
            }

            cJSON_Delete(msg_root);
            page++;

            if (last_create_sec == 0) break; /* no valid timestamps */

            /* Advance cursor to last message time + 1s */
            time_t next_cursor = last_create_sec + 1;
            if (next_cursor <= cursor) break; /* safety: no progress */
            cursor = next_cursor;
        }

        syslog(LOG_INFO, "[%s] msg_today: chat '%s' fetched %d pages\n", TAG, chat_name, page);
        chat_idx++;
    }

    cJSON_Delete(chats_root);

    /* Step 3: Output all messages that passed keyword scoring,
     * in original chronological order grouped by chat. */
    if (msg_count == 0) {
        snprintf(output, output_size, "今日消息中没有重要信息");
        free(msg_entries);
        return OK;
    }

    /* Output in original order, grouped by chat */
    size_t out_off = 0;
    int last_chat_idx = -1;
    for (int i = 0; i < msg_count; i++) {
        if (out_off + 320 >= output_size) break;

        if (msg_entries[i].chat_idx != last_chat_idx) {
            out_off += snprintf(output + out_off, output_size - out_off,
                "\n群聊: %s \n", msg_entries[i].chat_name);
            last_chat_idx = msg_entries[i].chat_idx;
        }
        if (msg_entries[i].sender[0]) {
            out_off += snprintf(output + out_off, output_size - out_off,
                "[%s] %s\n", msg_entries[i].sender, msg_entries[i].text);
        } else {
            out_off += snprintf(output + out_off, output_size - out_off,
                "%s\n", msg_entries[i].text);
        }
    }

    free(msg_entries);

    syslog(LOG_INFO, "[%s] msg_today: scanned %d msgs, %d passed scoring (threshold=%d)\n",
        TAG, total_scanned, total_matched, MSG_SCORE_THRESHOLD);
    return OK;
}
