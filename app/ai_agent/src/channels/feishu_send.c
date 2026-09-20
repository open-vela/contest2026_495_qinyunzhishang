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

#include "channels/feishu_internal.h"
#include "agent_config.h"
#include "cJSON.h"

#include <stdlib.h>
#include <string.h>

static const char* TAG = "feishu_send";

/* ── Outgoing @mention conversion ──────────────────────────────── */

static char* convert_outgoing_mentions(const char* text, size_t* clean_end)
{
    typedef struct {
        char name[64];
        char oid[64];
    } mention_entry_t;

    mention_entry_t mmap[8];
    int mcount = 0;

    const char* block = strstr(text, "\n[mentioned_users:");
    if (!block) {
        block = strstr(text, "[mentioned_users:");
    }

    size_t text_end = strlen(text);

    if (block) {
        text_end = (size_t)(block - text);
        const char* start = strchr(block, ':');
        const char* end = strchr(block, ']');

        if (start && end && start < end) {
            start++;
            while (start < end && mcount < 8) {
                while (start < end && *start == ' ') {
                    start++;
                }

                const char* eq = memchr(start, '=', (size_t)(end - start));
                if (!eq) {
                    break;
                }

                const char* comma = memchr(eq, ',', (size_t)(end - eq));
                if (!comma) {
                    comma = end;
                }

                size_t nlen = (size_t)(eq - start);
                size_t olen = (size_t)(comma - eq - 1);

                if (nlen > 0 && nlen < 64 && olen > 0 && olen < 64) {
                    memcpy(mmap[mcount].name, start, nlen);
                    mmap[mcount].name[nlen] = '\0';
                    memcpy(mmap[mcount].oid, eq + 1, olen);
                    mmap[mcount].oid[olen] = '\0';
                    mcount++;
                }
                start = comma < end ? comma + 1 : end;
            }
        }
    }

    *clean_end = text_end;

    bool need_convert = (mcount > 0) || strstr(text, "(open_id:");
    if (!need_convert) {
        return NULL;
    }

    size_t alloc = text_end * 3 + 1;
    char* out = malloc(alloc);
    if (!out) {
        return NULL;
    }

    size_t out_off = 0;
    const char* p = text;
    const char* p_end = text + text_end;

    while (p < p_end && out_off < alloc - 1) {
        if (*p == '@') {
            /* Try inline: @name(open_id:ou_xxx) */
            const char* ns = p + 1;
            const char* paren = NULL;

            for (const char* s = ns; s < p_end && *s != ' ' && *s != '\n'; s++) {
                if (*s == '(') {
                    paren = s;
                    break;
                }
            }

            if (paren && (size_t)(p_end - paren) > 9 &&
                strncmp(paren, "(open_id:", 9) == 0) {
                const char* id_s = paren + 9;
                const char* id_e = strchr(id_s, ')');

                if (id_e && id_e <= p_end && (size_t)(id_e - id_s) < 64) {
                    int n = snprintf(out + out_off, alloc - out_off,
                        "<at user_id=\"%.*s\">%.*s</at>",
                        (int)(id_e - id_s), id_s,
                        (int)(paren - ns), ns);
                    if (n > 0) {
                        out_off += (size_t)n;
                    }
                    p = id_e + 1;
                    continue;
                }
            }

            /* Try map-based: @name from mention map */
            if (mcount > 0) {
                bool matched = false;

                for (int i = 0; i < mcount; i++) {
                    size_t nlen = strlen(mmap[i].name);

                    if (ns + nlen <= p_end &&
                        strncmp(ns, mmap[i].name, nlen) == 0) {
                        char next = (ns + nlen < p_end) ? ns[nlen] : ' ';
                        if (next == ' ' || next == '\n' || next == ',' ||
                            next == '\0' || (unsigned char)next >= 0x80) {
                            int n = snprintf(out + out_off, alloc - out_off,
                                "<at user_id=\"%s\">%s</at>",
                                mmap[i].oid, mmap[i].name);
                            if (n > 0) {
                                out_off += (size_t)n;
                            }
                            p = ns + nlen;
                            matched = true;
                            break;
                        }
                    }
                }
                if (matched) {
                    continue;
                }
            }
        }
        out[out_off++] = *p++;
    }
    out[out_off] = '\0';
    return out;
}

/* ── Message sending with chunking ─────────────────────────────── */

static int send_message_chunks_ex(const char* receive_id, const char* text,
                                    size_t text_len, const char* id_type)
{
    FEISHU_AUTH_HDR(hdrs, s_access_token);

    char* resp = malloc(4096);
    if (!resp) {
        syslog(LOG_ERR, "[%s] OOM for send resp buffer\n", TAG);
        return ERROR;
    }

    char api_path[256];
    snprintf(api_path, sizeof(api_path),
        "/open-apis/im/v1/messages?receive_id_type=%s", id_type);

    size_t offset = 0;

    while (offset < text_len) {
        size_t chunk = text_len - offset;

        if (chunk > AGENT_FEISHU_MAX_MSG_LEN) {
            chunk = AGENT_FEISHU_MAX_MSG_LEN;
            /* Avoid splitting UTF-8 sequences */
            while (chunk > 0 &&
                   ((unsigned char)text[offset + chunk] & 0xC0) == 0x80) {
                chunk--;
            }
            if (chunk == 0) {
                chunk = AGENT_FEISHU_MAX_MSG_LEN;
            }
        }

        char* seg = malloc(chunk + 1);
        if (!seg) {
            free(resp);
            return ERROR;
        }

        memcpy(seg, text + offset, chunk);
        seg[chunk] = '\0';

        cJSON* content_obj = cJSON_CreateObject();
        cJSON_AddStringToObject(content_obj, "text", seg);
        free(seg);
        char* content_str = cJSON_PrintUnformatted(content_obj);
        cJSON_Delete(content_obj);

        if (!content_str) {
            offset += chunk;
            continue;
        }

        cJSON* body_obj = cJSON_CreateObject();
        cJSON_AddStringToObject(body_obj, "receive_id", receive_id);
        cJSON_AddStringToObject(body_obj, "msg_type", "text");
        cJSON_AddStringToObject(body_obj, "content", content_str);
        free(content_str);
        char* body_str = cJSON_PrintUnformatted(body_obj);
        cJSON_Delete(body_obj);

        if (!body_str) {
            offset += chunk;
            continue;
        }

        int status = feishu_https_post(api_path, hdrs, body_str, resp, 4096);

        if (status == 400 && strstr(resp, "access token")) {
            syslog(LOG_INFO, "[%s] Token invalid on send, refreshing\n", TAG);
            if (feishu_get_app_token() == OK) {
                char ra[520];
                snprintf(ra, sizeof(ra), "Bearer %s", s_access_token);
                vela_header_t rh[2] = { { "Authorization", ra }, { NULL, NULL } };
                status = feishu_https_post(api_path, rh, body_str, resp, 4096);
            }
        }
        free(body_str);

        if (status != 200 && status != 201) {
            syslog(LOG_WARNING, "[%s] send_message HTTP %d: %.120s\n",
                TAG, status, resp);
        }

        offset += chunk;
    }

    free(resp);
    return OK;
}

static int send_message_chunks(const char* chat_id, const char* text, size_t text_len)
{
    return send_message_chunks_ex(chat_id, text, text_len, "chat_id");
}

/* ── Public API ────────────────────────────────────────────────── */

int feishu_send_message(const char* chat_id, const char* text)
{
    if (s_access_token[0] == '\0' || feishu_token_expired()) {
        syslog(LOG_INFO, "[%s] Token expired/missing, refreshing before send\n", TAG);
        if (feishu_get_app_token() != OK) {
            syslog(LOG_WARNING, "[%s] Cannot send: token refresh failed\n", TAG);
            return ERROR;
        }
    }

    size_t clean_end;
    char* converted = convert_outgoing_mentions(text, &clean_end);

    if (converted) {
        text = converted;
    } else if (clean_end < strlen(text)) {
        /* No @mentions but strip metadata block */
        converted = malloc(clean_end + 1);
        if (converted) {
            memcpy(converted, text, clean_end);
            converted[clean_end] = '\0';
            text = converted;
        }
    }

    int ret = send_message_chunks(chat_id, text, strlen(text));

    free(converted);
    return ret;
}

/* 发送纯文本消息（通用，id_type: "chat_id" 或 "open_id"）。
 * 与 feishu_send_message 的区别：不经过 @mention 转换，支持任意 id_type。
 * 请求体结构遵循飞书官方消息创建 API：
 *   POST /open-apis/im/v1/messages?receive_id_type={id_type}
 *   body = {"receive_id":..., "msg_type":"text", "content":"{\"text\":\"...\"}"} */
int feishu_send_text_message_ex(const char *receive_id, const char *text,
                                 const char *id_type)
{
    if (!receive_id || !text || !id_type) return ERROR;

    if (s_access_token[0] == '\0' || feishu_token_expired()) {
        syslog(LOG_INFO, "[%s] Token expired/missing, refreshing before text send\n", TAG);
        if (feishu_get_app_token() != OK) {
            syslog(LOG_WARNING, "[%s] Cannot send text: token refresh failed\n", TAG);
            return ERROR;
        }
    }

    return send_message_chunks_ex(receive_id, text, strlen(text), id_type);
}

/* ── multipart/form-data body 构造 ──────────────────────────── */

static char *build_multipart_body(const char *filename,
                                   const uint8_t *file_data, size_t file_size,
                                   const char *boundary, size_t *out_len)
{
    const char *fmt1 =
        "--%s\r\n"
        "Content-Disposition: form-data; name=\"image_type\"\r\n"
        "\r\n";
    const char *val1 = "message";
    const char *fmt2 =
        "--%s\r\n"
        "Content-Disposition: form-data; name=\"image\"; filename=\"%s\"\r\n"
        "Content-Type: image/jpeg\r\n"
        "\r\n";
    const char *fmt3 = "--%s--\r\n";

    size_t h1 = snprintf(NULL, 0, fmt1, boundary);
    size_t v1 = strlen(val1);
    size_t h2 = snprintf(NULL, 0, fmt2, boundary, filename);
    size_t h3 = snprintf(NULL, 0, fmt3, boundary);

    *out_len = h1 + v1 + 2 + h2 + file_size + 2 + h3;

    char *body = malloc(*out_len + 1);
    if (!body) return NULL;

    char *p = body;
    p += sprintf(p, fmt1, boundary);
    memcpy(p, val1, v1); p += v1;
    memcpy(p, "\r\n", 2); p += 2;
    p += sprintf(p, fmt2, boundary, filename);
    memcpy(p, file_data, file_size); p += file_size;
    memcpy(p, "\r\n", 2); p += 2;
    p += sprintf(p, fmt3, boundary);

    return body;
}

/* ── 上传图片到飞书 ─────────────────────────────────────────── */

int feishu_upload_image(const char *image_path, char *image_key_out, size_t key_cap)
{
    if (!image_path || !image_key_out || key_cap == 0) return ERROR;

    syslog(LOG_INFO, "[%s] upload_image: path=%s\n", TAG, image_path);

    FILE *fp = fopen(image_path, "rb");
    if (!fp) {
        syslog(LOG_ERR, "[%s] upload_image: cannot open %s\n", TAG, image_path);
        return ERROR;
    }
    fseek(fp, 0, SEEK_END);
    size_t file_size = (size_t)ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (file_size == 0 || file_size > 10 * 1024 * 1024) {
        syslog(LOG_ERR, "[%s] upload_image: invalid size %zu\n", TAG, file_size);
        fclose(fp);
        return ERROR;
    }

    uint8_t *file_data = malloc(file_size);
    if (!file_data) {
        syslog(LOG_ERR, "[%s] upload_image: OOM for %zu bytes\n", TAG, file_size);
        fclose(fp);
        return ERROR;
    }
    size_t read_len = fread(file_data, 1, file_size, fp);
    fclose(fp);

    if (read_len != file_size) {
        syslog(LOG_ERR, "[%s] upload_image: read mismatch %zu/%zu\n",
            TAG, read_len, file_size);
        free(file_data);
        return ERROR;
    }

    const char *boundary = "----VelAFormBoundary7MA4YWxkTrZu0gW";
    const char *filename = strrchr(image_path, '/');
    filename = filename ? filename + 1 : image_path;

    size_t body_len = 0;
    char *body = build_multipart_body(filename, file_data, file_size,
                                       boundary, &body_len);
    free(file_data);

    if (!body) {
        syslog(LOG_ERR, "[%s] upload_image: build body failed\n", TAG);
        return ERROR;
    }

    syslog(LOG_INFO, "[%s] upload_image: body_len=%zu\n", TAG, body_len);

    if (s_access_token[0] == '\0' || feishu_token_expired()) {
        if (feishu_get_app_token() != OK) {
            syslog(LOG_ERR, "[%s] upload_image: token refresh failed\n", TAG);
            free(body);
            return ERROR;
        }
    }

    char auth_val[520];
    snprintf(auth_val, sizeof(auth_val), "Bearer %s", s_access_token);
    char ct[128];
    snprintf(ct, sizeof(ct), "multipart/form-data; boundary=%s", boundary);
    vela_header_t headers[] = {
        { "Authorization", auth_val },
        { "Content-Type", ct },
        { NULL, NULL }
    };

    char *resp = malloc(4096);
    if (!resp) {
        free(body);
        return ERROR;
    }
    memset(resp, 0, 4096);
    size_t resp_len = 0;
    int status = feishu_https_request("POST", "/open-apis/im/v1/images",
        headers, body, body_len, resp, 4096, &resp_len);
    free(body);

    syslog(LOG_INFO, "[%s] upload_image: HTTP %d, resp_len=%zu\n",
        TAG, status, resp_len);

    if (status != 200) {
        syslog(LOG_ERR, "[%s] upload_image: HTTP %d: %.200s\n", TAG, status, resp);
        free(resp);
        return ERROR;
    }

    cJSON *root = cJSON_Parse(resp);
    free(resp);
    if (!root) {
        syslog(LOG_ERR, "[%s] upload_image: JSON parse failed\n", TAG);
        return ERROR;
    }

    cJSON *code_j = cJSON_GetObjectItem(root, "code");
    if (!cJSON_IsNumber(code_j) || code_j->valueint != 0) {
        cJSON *msg_j = cJSON_GetObjectItem(root, "msg");
        syslog(LOG_ERR, "[%s] upload_image: API error code=%d msg=%s\n", TAG,
            (int)code_j->valuedouble,
            msg_j && cJSON_IsString(msg_j) ? msg_j->valuestring : "?");
        cJSON_Delete(root);
        return ERROR;
    }

    cJSON *data = cJSON_GetObjectItem(root, "data");
    cJSON *ik = cJSON_GetObjectItem(data, "image_key");
    if (cJSON_IsString(ik)) {
        strncpy(image_key_out, ik->valuestring, key_cap - 1);
        image_key_out[key_cap - 1] = '\0';
        syslog(LOG_INFO, "[%s] upload_image: OK, image_key=%s\n",
            TAG, image_key_out);
    } else {
        syslog(LOG_ERR, "[%s] upload_image: no image_key in response\n", TAG);
        cJSON_Delete(root);
        return ERROR;
    }

    cJSON_Delete(root);
    return OK;
}

/* ── 发送图片消息（通用，id_type: "chat_id" 或 "open_id"） ──── */

int feishu_send_image_message_ex(const char *receive_id, const char *image_key,
                                  const char *id_type)
{
    if (!receive_id || !image_key || !id_type) return ERROR;

    syslog(LOG_INFO, "[%s] send_image_message: id_type=%s id=%s image_key=%s\n",
        TAG, id_type, receive_id, image_key);

    cJSON *content_obj = cJSON_CreateObject();
    cJSON_AddStringToObject(content_obj, "image_key", image_key);
    char *content_str = cJSON_PrintUnformatted(content_obj);
    cJSON_Delete(content_obj);

    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "receive_id", receive_id);
    cJSON_AddStringToObject(body, "msg_type", "image");
    cJSON_AddStringToObject(body, "content", content_str);
    char *body_str = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    free(content_str);

    char api_path[256];
    snprintf(api_path, sizeof(api_path),
        "/open-apis/im/v1/messages?receive_id_type=%s", id_type);

    char *resp = malloc(4096);
    if (!resp) {
        free(body_str);
        return ERROR;
    }
    memset(resp, 0, 4096);
    int status = feishu_api_post(api_path, body_str, resp, 4096);
    free(body_str);

    syslog(LOG_INFO, "[%s] send_image_message: HTTP %d\n", TAG, status);

    if (status != 200) {
        syslog(LOG_ERR, "[%s] send_image_message: HTTP %d: %.200s\n",
            TAG, status, resp);
        free(resp);
        return ERROR;
    }

    cJSON *root = cJSON_Parse(resp);
    free(resp);
    if (root) {
        cJSON *code_j = cJSON_GetObjectItem(root, "code");
        if (cJSON_IsNumber(code_j) && code_j->valueint != 0) {
            cJSON *msg_j = cJSON_GetObjectItem(root, "msg");
            syslog(LOG_ERR, "[%s] send_image_message: API error code=%d msg=%s\n",
                TAG, (int)code_j->valuedouble,
                msg_j && cJSON_IsString(msg_j) ? msg_j->valuestring : "?");
            cJSON_Delete(root);
            return ERROR;
        }
        cJSON *data = cJSON_GetObjectItem(root, "data");
        cJSON *msg_id = cJSON_GetObjectItem(data, "message_id");
        if (cJSON_IsString(msg_id)) {
            syslog(LOG_INFO, "[%s] send_image_message: OK, message_id=%s\n",
                TAG, msg_id->valuestring);
        }
        cJSON_Delete(root);
    }

    return OK;
}

/* ── 发送图片消息到群聊（向后兼容，等价于 ex(receive_id,key,"chat_id")） */

int feishu_send_image_message(const char *chat_id, const char *image_key)
{
    return feishu_send_image_message_ex(chat_id, image_key, "chat_id");
}

/* ── 组合入口：上传 + 发送 ──────────────────────────────────── */

int feishu_send_image_to_chat(const char *chat_id, const char *image_path)
{
    char image_key[128] = {0};

    syslog(LOG_INFO, "[%s] send_image_to_chat: chat_id=%s path=%s\n",
        TAG, chat_id, image_path);

    if (feishu_upload_image(image_path, image_key, sizeof(image_key)) != OK) {
        syslog(LOG_ERR, "[%s] send_image_to_chat: upload failed\n", TAG);
        return ERROR;
    }

    if (feishu_send_image_message(chat_id, image_key) != OK) {
        syslog(LOG_ERR, "[%s] send_image_to_chat: send failed\n", TAG);
        return ERROR;
    }

    syslog(LOG_INFO, "[%s] send_image_to_chat: all done!\n", TAG);
    return OK;
}
