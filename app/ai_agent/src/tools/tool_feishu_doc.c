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

#include "tools/tool_feishu_doc.h"
#include "channels/feishu_bot.h"
#include "channels/feishu_internal.h"
#include "agent_compat.h"
#include "agent_config.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *TAG = "tool_feishu_doc";

#define RESP_BUF_SIZE (16 * 1024)

/* Default folder token for document creation - build time configured shared folder */
#define FEISHU_DEFAULT_FOLDER_TOKEN AGENT_SECRET_FEISHU_DOC_FOLDER_TOKEN

/* ── Helper: check Feishu API response code ──────────────────── */

static int parse_feishu_response(const char *resp, cJSON **out_data,
                                 char *output, size_t output_size)
{
    cJSON *root = cJSON_Parse(resp);
    if (!root) {
        snprintf(output, output_size, "Error: invalid JSON from Feishu API");
        return -1;
    }

    cJSON *code = cJSON_GetObjectItem(root, "code");
    if (!cJSON_IsNumber(code) || code->valueint != 0) {
        cJSON *msg = cJSON_GetObjectItem(root, "msg");
        snprintf(output, output_size, "Error: Feishu API code=%d msg=%s",
                 cJSON_IsNumber(code) ? (int)code->valueint : -1,
                 (msg && cJSON_IsString(msg)) ? msg->valuestring : "unknown");
        cJSON_Delete(root);
        return -1;
    }

    if (out_data) {
        *out_data = root;   /* caller must cJSON_Delete */
    } else {
        cJSON_Delete(root);
    }
    return 0;
}

/* ── feishu_doc_create ───────────────────────────────────────── */

int tool_feishu_doc_create_execute(const char *input_json,
                                   char *output, size_t output_size)
{
    cJSON *input = cJSON_Parse(input_json);
    if (!input) {
        snprintf(output, output_size, "Error: invalid JSON input");
        return ERROR;
    }

    const char *title_tmp = cJSON_GetStringValue(
                            cJSON_GetObjectItem(input, "title"));
    const char *folder_tmp = cJSON_GetStringValue(
                            cJSON_GetObjectItem(input, "folder_token"));

    /* 在cJSON_Delete之前拷贝字符串到本地缓冲区，避免悬空指针 */
    char title_buf[256] = {0};
    char folder_buf[256] = {0};
    if (title_tmp && title_tmp[0]) {
        strncpy(title_buf, title_tmp, sizeof(title_buf) - 1);
    }
    if (folder_tmp && folder_tmp[0]) {
        strncpy(folder_buf, folder_tmp, sizeof(folder_buf) - 1);
    } else {
        strncpy(folder_buf, FEISHU_DEFAULT_FOLDER_TOKEN, sizeof(folder_buf) - 1);
    }
    const char *title = title_buf[0] ? title_buf : NULL;
    const char *folder = folder_buf;

    /* Build request body */
    cJSON *body = cJSON_CreateObject();
    if (title && title[0]) {
        cJSON_AddStringToObject(body, "title", title);
    }
    cJSON_AddStringToObject(body, "folder_token", folder);
    char *body_str = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    cJSON_Delete(input);  /* 删除后所有从input获取的指针失效，使用上面拷贝的本地副本 */

    if (!body_str) {
        snprintf(output, output_size, "Error: out of memory");
        return ERROR;
    }

    char *resp = malloc(RESP_BUF_SIZE);
    if (!resp) { free(body_str); snprintf(output, output_size, "Error: OOM"); return ERROR; }

    syslog(LOG_INFO, "[%s] [FEISHU_DOC] Creating document: title=%s, folder=%s\n", TAG,
           title ? title : "(untitled)", folder);

    /* Create document using tenant_access_token (app identity, auto-refreshed) */
    int status = feishu_api_post("/open-apis/docx/v1/documents",
                                 body_str, resp, RESP_BUF_SIZE);

    syslog(LOG_INFO, "[%s] [FEISHU_DOC] HTTP response status=%d, body=%.500s\n", TAG, status, resp);
    free(body_str);

    if (status != 200) {
        syslog(LOG_ERR, "[%s] [FEISHU_DOC] HTTP request failed, status=%d\n", TAG, status);
        snprintf(output, output_size, "Error: HTTP %d from Feishu: %.200s", status, resp);
        free(resp);
        return ERROR;
    }

    /* Parse response: { "code":0, "data":{"document":{"document_id":"...", "title":"..."}} } */
    cJSON *root = NULL;
    if (parse_feishu_response(resp, &root, output, output_size) < 0) {
        syslog(LOG_ERR, "[%s] [FEISHU_DOC] Failed to parse Feishu API response\n", TAG);
        free(resp);
        return ERROR;
    }
    free(resp);

    /* Check Feishu business error code */
    cJSON *code_item = cJSON_GetObjectItem(root, "code");
    if (code_item && cJSON_IsNumber(code_item) && code_item->valueint != 0) {
        cJSON *msg_item = cJSON_GetObjectItem(root, "msg");
        const char *err_msg = (msg_item && cJSON_IsString(msg_item)) ? msg_item->valuestring : "unknown";
        syslog(LOG_ERR, "[%s] [FEISHU_DOC] Feishu business error: code=%lld, msg=%s\n", TAG, (long long)code_item->valueint, err_msg);
    }

    cJSON *data = cJSON_GetObjectItem(root, "data");
    cJSON *doc  = data ? cJSON_GetObjectItem(data, "document") : NULL;

    if (!doc) {
        syslog(LOG_ERR, "[%s] [FEISHU_DOC] No document object in response data\n", TAG);
        snprintf(output, output_size, "Error: no document in response");
        cJSON_Delete(root);
        return ERROR;
    }

    /* Build output */
    cJSON *result = cJSON_CreateObject();
    cJSON *doc_id_j = cJSON_GetObjectItem(doc, "document_id");
    cJSON *doc_title_j = cJSON_GetObjectItem(doc, "title");
    const char *doc_id_tmp = doc_id_j && cJSON_IsString(doc_id_j) ? doc_id_j->valuestring : NULL;
    const char *doc_title_tmp = doc_title_j && cJSON_IsString(doc_title_j) ? doc_title_j->valuestring : "(untitled)";

    /* 必须在cJSON_Delete(root)之前拷贝字符串到本地缓冲区，避免悬空指针 */
    char doc_id_str[128] = {0};
    char doc_title_str[256] = {0};
    if (doc_id_tmp) {
        strncpy(doc_id_str, doc_id_tmp, sizeof(doc_id_str) - 1);
        cJSON_AddStringToObject(result, "document_id", doc_id_str);
    }
    if (doc_title_tmp) {
        strncpy(doc_title_str, doc_title_tmp, sizeof(doc_title_str) - 1);
        cJSON_AddStringToObject(result, "title", doc_title_str);
    }

    /* Construct URL for convenience */
    if (doc_id_str[0] != '\0') {
        char url[256];
        snprintf(url, sizeof(url), "https://feishu.cn/docx/%s", doc_id_str);
        cJSON_AddStringToObject(result, "url", url);
    }

    cJSON_AddStringToObject(result, "status", "created");

    char *out_str = cJSON_PrintUnformatted(result);
    cJSON_Delete(result);
    cJSON_Delete(root);  /* 释放后所有原JSON内部指针失效，必须使用上面拷贝的本地副本 */

    if (out_str) {
        strncpy(output, out_str, output_size - 1);
        output[output_size - 1] = '\0';
        free(out_str);
    }

    if (doc_id_str[0] != '\0') {
        syslog(LOG_INFO, "[%s] [FEISHU_DOC] Create SUCCESS: document_id=%s, title=%s\n",
               TAG, doc_id_str, doc_title_str);
        /* 通过内部接口通知上层（voice_assistant）文档已创建，避免ai_agent库反向依赖上层 */
        feishu_doc_notify_created(doc_id_str, doc_title_str);
    } else {
        syslog(LOG_INFO, "[%s] [FEISHU_DOC] Create SUCCESS (no document_id in response)\n", TAG);
    }
    return OK;
}

/* ── feishu_doc_write ────────────────────────────────────────── */

/* 飞书 API 单次请求 block 数量上限（官方限制 50，留余量用 40）*/
#define FEISHU_BLOCK_BATCH_SIZE 40

/* 辅助函数：将一批 block children 写入飞书文档。
 * 不传 index 参数：默认追加到文档末尾，确保分批写入时顺序正确。
 * 注意：children 所有权转移给本函数，内部会释放。*/
static int feishu_write_batch(const char *path, cJSON *children,
                              int batch_num, int block_count,
                              char *output, size_t output_size)
{
    cJSON *body = cJSON_CreateObject();
    cJSON_AddItemToObject(body, "children", children);
    /* 不传 index：飞书 API 默认追加到文档末尾 */

    char *body_str = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);

    if (!body_str) {
        snprintf(output, output_size, "Error: out of memory (batch %d)", batch_num);
        return ERROR;
    }

    char *resp = malloc(RESP_BUF_SIZE);
    if (!resp) {
        free(body_str);
        snprintf(output, output_size, "Error: OOM (batch %d)", batch_num);
        return ERROR;
    }

    syslog(LOG_INFO, "[%s] Writing batch %d (%d blocks)\n", TAG, batch_num, block_count);

    /* QPS 限流：飞书 API 限制单应用每秒 3 次、单文档每秒 3 次。
     * 第二批及以后请求前等待 350ms（>1/3 秒），避免触发 HTTP 429。
     * 放在发请求前：①第一批不延时，短会议无开销；②请求失败时不浪费时间。 */
    if (batch_num > 0) {
        usleep(350 * 1000);
    }

    int status = feishu_api_post(path, body_str, resp, RESP_BUF_SIZE);
    free(body_str);

    if (status != 200) {
        snprintf(output, output_size, "Error: HTTP %d (batch %d): %.200s",
                 status, batch_num, resp);
        free(resp);
        return ERROR;
    }

    cJSON *root = NULL;
    if (parse_feishu_response(resp, &root, output, output_size) < 0) {
        free(resp);
        return ERROR;
    }
    free(resp);
    cJSON_Delete(root);
    return OK;
}

int tool_feishu_doc_write_execute(const char *input_json,
                                  char *output, size_t output_size)
{
    cJSON *input = cJSON_Parse(input_json);
    if (!input) {
        snprintf(output, output_size, "Error: invalid JSON input");
        return ERROR;
    }

    const char *doc_id  = cJSON_GetStringValue(
                              cJSON_GetObjectItem(input, "document_id"));
    const char *content = cJSON_GetStringValue(
                              cJSON_GetObjectItem(input, "content"));

    if (!doc_id || !doc_id[0]) {
        snprintf(output, output_size, "Error: missing 'document_id'");
        cJSON_Delete(input);
        return ERROR;
    }
    if (!content || !content[0]) {
        snprintf(output, output_size, "Error: missing 'content'");
        cJSON_Delete(input);
        return ERROR;
    }

    /* Split content by newlines into paragraph blocks.
     * Each non-empty line becomes a text block child of the document root. */

    /* First, get the document to find the root block_id (== document_id) */
    char path[256];
    snprintf(path, sizeof(path),
             "/open-apis/docx/v1/documents/%s/blocks/%s/children",
             doc_id, doc_id);

    /* Build block children array — each paragraph is a block of type 2 (text).
     * Feishu block API (from Go SDK Block struct):
     *   block_type=1: page, block_type=2: text, block_type=3: heading1, ...
     *   { "children": [ { "block_type": 2, "text": { "elements": [
     *       { "text_run": { "content": "..." } }
     *   ] } } ], "index": 0 }
     */
    cJSON *children = cJSON_CreateArray();
    const char *p = content;
    int batch_count = 0;    /* 当前批次已累积的 block 数 */
    int total_written = 0;  /* 已成功写入的 block 总数 */
    int batch_num = 0;      /* 当前批次编号（0-based） */

    while (*p) {
        /* Find end of line */
        const char *eol = strchr(p, '\n');
        size_t line_len = eol ? (size_t)(eol - p) : strlen(p);

        /* Skip empty lines — Feishu rejects empty text_run content */
        if (line_len == 0) {
            p++;
            continue;
        }

        /* Create a text block for each line */
        char *line = malloc(line_len + 1);
        if (!line) break;
        memcpy(line, p, line_len);
        line[line_len] = '\0';

        /* Build: { "block_type": 2, "text": { "elements": [
         *           { "text_run": { "content": "line" } } ] } } */
        cJSON *block = cJSON_CreateObject();
        cJSON_AddNumberToObject(block, "block_type", 2);

        cJSON *text_obj = cJSON_CreateObject();
        cJSON *elements = cJSON_CreateArray();
        cJSON *elem = cJSON_CreateObject();
        cJSON *text_run = cJSON_CreateObject();
        cJSON_AddStringToObject(text_run, "content", line);
        cJSON_AddItemToObject(elem, "text_run", text_run);
        cJSON_AddItemToArray(elements, elem);
        cJSON_AddItemToObject(text_obj, "elements", elements);
        cJSON_AddItemToObject(block, "text", text_obj);

        cJSON_AddItemToArray(children, block);
        free(line);
        batch_count++;

        p += line_len;
        if (*p == '\n') p++;

        /* 分批写入：累积满 FEISHU_BLOCK_BATCH_SIZE 个 block 就刷新一次 */
        if (batch_count >= FEISHU_BLOCK_BATCH_SIZE) {
            if (feishu_write_batch(path, children, batch_num, batch_count,
                                   output, output_size) != OK) {
                syslog(LOG_ERR, "[%s] batch %d failed, %d blocks written before failure\n",
                       TAG, batch_num, total_written);
                cJSON_Delete(input);
                return ERROR;
            }
            total_written += batch_count;
            batch_num++;
            children = cJSON_CreateArray();
            batch_count = 0;
        }
    }

    cJSON_Delete(input);

    /* 刷新剩余不足一批的 block */
    if (batch_count > 0) {
        if (feishu_write_batch(path, children, batch_num, batch_count,
                               output, output_size) != OK) {
            return ERROR;
        }
        total_written += batch_count;
        batch_num++;
    } else {
        cJSON_Delete(children);
    }

    syslog(LOG_INFO, "[%s] Wrote %d blocks in %d batches to %s\n",
           TAG, total_written, batch_num, doc_id);

    snprintf(output, output_size,
             "{\"status\":\"ok\",\"document_id\":\"%s\",\"message\":\"Written %d blocks in %d batches\"}",
             doc_id, total_written, batch_num);
    return OK;
}

/* ── feishu_doc_read ─────────────────────────────────────────── */

/* 过滤飞书文档内容中的非法字符：保留汉字、汉字标点、英文、英文标点、
 * 其他语言及其标点等合法可打印UTF-8字符；清除控制字符（\n\r\t除外）、
 * 无效UTF-8序列、BOM、零宽字符及方向控制符等不可见字符。
 * 就地修改字符串，返回过滤掉的字节数。 */
size_t sanitize_doc_content(char *s)
{
    if (!s) return 0;

    unsigned char *src = (unsigned char *)s;
    unsigned char *dst = (unsigned char *)s;
    size_t removed = 0;

    while (*src) {
        unsigned char c = *src;

        /* ASCII: 0x00-0x7F */
        if (c < 0x80) {
            /* 保留可打印ASCII（0x21-0x7E）和 \n \r \t
             * 空格(0x20)智能保留：仅当前后字符都是ASCII字母/数字时保留
             * 这样英文单词间的空格保留，中文间的空格删除避免TTS断句异常 */
            if (c == 0x20) {
                /* 判断前一个保留字符和后一个字符是否都是ASCII字母/数字 */
                int prev_is_ascii_alnum = 0;
                int next_is_ascii_alnum = 0;

                /* 检查dst中前一个保留的字符（需从已写入的dst回溯） */
                if (dst > (unsigned char *)s) {
                    unsigned char prev = *(dst - 1);
                    if ((prev >= '0' && prev <= '9') ||
                        (prev >= 'A' && prev <= 'Z') ||
                        (prev >= 'a' && prev <= 'z')) {
                        prev_is_ascii_alnum = 1;
                    }
                }

                /* 检查src中下一个字符是否是ASCII字母/数字 */
                unsigned char next = src[1];
                if ((next >= '0' && next <= '9') ||
                    (next >= 'A' && next <= 'Z') ||
                    (next >= 'a' && next <= 'z')) {
                    next_is_ascii_alnum = 1;
                }

                if (prev_is_ascii_alnum && next_is_ascii_alnum) {
                    *dst++ = c;  /* 英文单词间空格，保留 */
                } else {
                    removed++;   /* 中文间或中英交界的空格，删除 */
                }
            } else if ((c > 0x20 && c <= 0x7E) || c == '\n' || c == '\r' || c == '\t') {
                *dst++ = c;
            } else {
                removed++;
            }
            src++;
            continue;
        }

        /* 解析多字节UTF-8序列起始字节 */
        int len = 0;
        unsigned int cp = 0;  /* code point */

        if ((c & 0xE0) == 0xC0) {        /* 2字节: 110xxxxx */
            len = 2;
            cp = c & 0x1F;
        } else if ((c & 0xF0) == 0xE0) { /* 3字节: 1110xxxx */
            len = 3;
            cp = c & 0x0F;
        } else if ((c & 0xF8) == 0xF0) { /* 4字节: 11110xxx */
            len = 4;
            cp = c & 0x07;
        } else {
            /* 非法UTF-8起始字节，跳过 */
            src++;
            removed++;
            continue;
        }

        /* 校验后续字节是否都是 10xxxxxx，并累积 code point */
        int valid = 1;
        int i;
        for (i = 1; i < len; i++) {
            if ((src[i] & 0xC0) != 0x80) {
                valid = 0;
                break;
            }
            cp = (cp << 6) | (src[i] & 0x3F);
        }

        if (!valid) {
            /* 非法UTF-8序列，跳过首字节（后续字节下轮重新判定） */
            src++;
            removed++;
            continue;
        }

        /* 过滤不可见格式字符（零宽字符、BOM、方向控制符等） */
        if (cp == 0xFEFF ||       /* BOM / 零宽不换行空格 */
            cp == 0x200B ||       /* 零宽空格 */
            cp == 0x200C ||       /* 零宽不连接符 */
            cp == 0x200D ||       /* 零宽连接符 */
            cp == 0x200E ||       /* 左至右标记 */
            cp == 0x200F ||       /* 右至左标记 */
            cp == 0x202A ||       /* 左至右嵌入 */
            cp == 0x202B ||       /* 右至左嵌入 */
            cp == 0x202C ||       /* 弹出方向格式 */
            cp == 0x202D ||       /* 左至右覆盖 */
            cp == 0x202E ||       /* 右至左覆盖 */
            cp == 0x2060 ||       /* 字词连接符 */
            cp == 0x2061 ||       /* 函数应用 */
            cp == 0x2062 ||       /* 不可见乘号 */
            cp == 0x2063 ||       /* 不可见分隔符 */
            cp == 0x2064 ||       /* 不可见加号 */
            cp == 0xFFF9 ||       /* 行内注释锚点 */
            cp == 0xFFFA ||       /* 行内注释分隔符 */
            cp == 0xFFFB ||       /* 行内注释终止符 */
            cp == 0xFFFC ||       /* 对象替换字符（飞书图片/附件/@提及占位） */
            cp == 0xFFFD) {       /* 替换字符（无效Unicode占位） */
            src += len;
            removed += len;
            continue;
        }

        /* 保留合法多字节字符 */
        for (i = 0; i < len; i++) {
            *dst++ = src[i];
        }
        src += len;
    }

    *dst = '\0';
    return removed;
}

int tool_feishu_doc_read_execute(const char *input_json,
                                 char *output, size_t output_size)
{
    cJSON *input = cJSON_Parse(input_json);
    if (!input) {
        snprintf(output, output_size, "Error: invalid JSON input");
        return ERROR;
    }

    const char *doc_id = cJSON_GetStringValue(
                             cJSON_GetObjectItem(input, "document_id"));
    if (!doc_id || !doc_id[0]) {
        snprintf(output, output_size, "Error: missing 'document_id'");
        cJSON_Delete(input);
        return ERROR;
    }

    char path[256];
    snprintf(path, sizeof(path),
             "/open-apis/docx/v1/documents/%s/raw_content", doc_id);
    cJSON_Delete(input);

    char *resp = malloc(RESP_BUF_SIZE);
    if (!resp) { snprintf(output, output_size, "Error: OOM"); return ERROR; }

    syslog(LOG_INFO, "[%s] Reading document %s\n", TAG, doc_id);

    /* 使用 tenant_access_token（应用身份），自动刷新，无需 user_access_token */
    int status = feishu_api_request("GET", path, NULL, 0, resp, RESP_BUF_SIZE);

    if (status != 200) {
        snprintf(output, output_size, "Error: HTTP %d: %.200s", status, resp);
        free(resp);
        return ERROR;
    }

    cJSON *root = NULL;
    if (parse_feishu_response(resp, &root, output, output_size) < 0) {
        free(resp);
        return ERROR;
    }
    free(resp);

    cJSON *data = cJSON_GetObjectItem(root, "data");
    cJSON *content = data ? cJSON_GetObjectItem(data, "content") : NULL;

    if (content && cJSON_IsString(content)) {
        strncpy(output, content->valuestring, output_size - 1);
        output[output_size - 1] = '\0';
    } else {
        /* Fallback: return the whole data object */
        char *data_str = data ? cJSON_PrintUnformatted(data) : NULL;
        if (data_str) {
            strncpy(output, data_str, output_size - 1);
            output[output_size - 1] = '\0';
            free(data_str);
        } else {
            snprintf(output, output_size, "Error: no content in response");
            cJSON_Delete(root);
            return ERROR;
        }
    }

    cJSON_Delete(root);

    /* 过滤非法字符：清除控制字符、无效UTF-8序列、BOM、零宽字符等，
     * 保留汉字、汉字标点、英文、英文标点、其他语言及其标点等合法字符。 */
    size_t before_len = strlen(output);
    size_t removed = sanitize_doc_content(output);
    if (removed > 0) {
        syslog(LOG_INFO, "[%s] Doc content sanitized: %zu -> %zu bytes (removed %zu)\n",
               TAG, before_len, before_len - removed, removed);
    }

    return OK;
}

/* ── feishu_doc_list ─────────────────────────────────────────── */

int tool_feishu_doc_list_execute(const char *input_json,
                                 char *output, size_t output_size)
{
    cJSON *input = cJSON_Parse(input_json);
    const char *folder = NULL;
    if (input) {
        folder = cJSON_GetStringValue(
                     cJSON_GetObjectItem(input, "folder_token"));
    }

    /* 与 feishu_doc_create 一致，未传 folder_token 时使用默认配置 */
    if (!folder || !folder[0]) {
        folder = FEISHU_DEFAULT_FOLDER_TOKEN;
    }

    char path[256];
    if (folder && folder[0]) {
        snprintf(path, sizeof(path),
                 "/open-apis/drive/v1/files?folder_token=%s&order_by=EditedTime&direction=DESC&page_size=50",
                 folder);
    } else {
        snprintf(path, sizeof(path),
                 "/open-apis/drive/v1/files?order_by=EditedTime&direction=DESC&page_size=50");
    }
    cJSON_Delete(input);

    char *resp = malloc(RESP_BUF_SIZE);
    if (!resp) { snprintf(output, output_size, "Error: OOM"); return ERROR; }

    syslog(LOG_INFO, "[%s] Listing documents (folder=%s)\n", TAG,
           folder ? folder : "root");

    /* 使用 tenant_access_token（应用身份），自动刷新，无需 user_access_token */
    int status = feishu_api_request("GET", path, NULL, 0, resp, RESP_BUF_SIZE);

    if (status != 200) {
        snprintf(output, output_size, "Error: HTTP %d: %.200s", status, resp);
        free(resp);
        return ERROR;
    }

    cJSON *root = NULL;
    if (parse_feishu_response(resp, &root, output, output_size) < 0) {
        free(resp);
        return ERROR;
    }
    free(resp);

    cJSON *data  = cJSON_GetObjectItem(root, "data");
    cJSON *files = data ? cJSON_GetObjectItem(data, "files") : NULL;

    if (!files || !cJSON_IsArray(files)) {
        snprintf(output, output_size, "[]");
        cJSON_Delete(root);
        return OK;
    }

    /* Build compact output: [{name, token, type, url}, ...] */
    cJSON *result = cJSON_CreateArray();
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, files) {
        cJSON *entry = cJSON_CreateObject();
        cJSON *name  = cJSON_GetObjectItem(item, "name");
        cJSON *token = cJSON_GetObjectItem(item, "token");
        cJSON *type  = cJSON_GetObjectItem(item, "type");
        cJSON *url   = cJSON_GetObjectItem(item, "url");

        if (name && cJSON_IsString(name))
            cJSON_AddStringToObject(entry, "name", name->valuestring);
        if (token && cJSON_IsString(token))
            cJSON_AddStringToObject(entry, "token", token->valuestring);
        if (type && cJSON_IsString(type))
            cJSON_AddStringToObject(entry, "type", type->valuestring);
        if (url && cJSON_IsString(url))
            cJSON_AddStringToObject(entry, "url", url->valuestring);

        cJSON_AddItemToArray(result, entry);
    }

    char *out_str = cJSON_PrintUnformatted(result);
    cJSON_Delete(result);
    cJSON_Delete(root);

    if (out_str) {
        strncpy(output, out_str, output_size - 1);
        output[output_size - 1] = '\0';
        free(out_str);
    }

    return OK;
}
