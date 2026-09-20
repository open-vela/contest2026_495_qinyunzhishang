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

#include "agent_compat.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifdef CONFIG_AI_AGENT_FEISHU

int feishu_bot_init(void);
int feishu_bot_start(void);
int feishu_send_message(const char *chat_id, const char *text);
int feishu_set_app(const char *app_id, const char *app_secret);
int feishu_api_post(const char *path, const char *json_body,
                    char *resp_buf, size_t resp_cap);
int feishu_api_request(const char *method, const char *path,
                       const char *body, size_t body_len,
                       char *resp_buf, size_t resp_cap);
const char *feishu_get_app_id(void);
int feishu_api_post_as_user(const char *path, const char *json_body,
                            char *resp_buf, size_t resp_cap);
int feishu_set_user_token(const char *token);
int feishu_api_request_as_user(const char *method, const char *path,
                               const char *body, size_t body_len,
                               char *resp_buf, size_t resp_cap);

/* 搜索群聊，按名称精确匹配，返回 chat_id */
int feishu_search_chats(const char *keyword, char *chat_id_out, size_t cap);

/* 通过手机号获取用户 open_id（POST /contact/v3/users/batch_get_id） */
int feishu_get_user_open_id(const char *mobile, char *open_id_out, size_t cap);

/* 上传图片到飞书，返回 image_key */
int feishu_upload_image(const char *image_path, char *image_key_out, size_t key_cap);

/* 发送图片消息（通用，id_type: "chat_id" 或 "open_id"） */
int feishu_send_image_message_ex(const char *receive_id, const char *image_key,
                                 const char *id_type);

/* 发送图片消息到指定群聊（向后兼容） */
int feishu_send_image_message(const char *chat_id, const char *image_key);

/* 发送纯文本消息（通用，id_type: "chat_id" 或 "open_id"）。
 * 与 feishu_send_message 区别：不经过 @mention 转换，支持任意 id_type。
 * 用于将 AI 回复/用户语音转文字转发到飞书用户或群聊。 */
int feishu_send_text_message_ex(const char *receive_id, const char *text,
                                 const char *id_type);

/* 组合：上传图片 + 发送到群聊 */
int feishu_send_image_to_chat(const char *chat_id, const char *image_path);

/* ── 飞书文档（docx）操作 API ────────────────────────────────── */
/* 追加文字块（自动按换行分段） */
int feishu_doc_append_text(const char *doc_id, const char *text);
/* 追加分割线块 */
int feishu_doc_append_divider(const char *doc_id);
/* 追加标题块（block_type=5 heading3），用于图片分组标识 */
int feishu_doc_append_heading1(const char *doc_id, const char *title);
/* 创建空图片块，返回block_id */
int feishu_doc_create_image_block(const char *doc_id, char *image_block_id_out, size_t out_cap);
/* 上传图片到docx_image类型素材库，返回file_token */
int feishu_doc_upload_image_media(const char *doc_id, const char *image_block_id, const char *image_path, char *file_token_out, size_t out_cap);
/* 更新图片块关联file_token */
int feishu_doc_patch_image_block(const char *doc_id, const char *image_block_id, const char *file_token);
/* 一站式追加图片：创建块→上传→patch */
int feishu_doc_append_image(const char *doc_id, const char *image_path);

/* ── 文档创建回调：上层（如voice_assistant）可注册此回调，在文档创建成功后得到通知 ── */
typedef void (*feishu_doc_created_cb_t)(const char *doc_id, const char *title, void *user_data);
int feishu_set_doc_created_callback(feishu_doc_created_cb_t cb, void *user_data);

/* ── 飞书对话模式：实时消息回调与群成员缓存 ────────────────────── */
/* 对话模式回调：feishu_recv 收到消息时调用，而非推送到 agent_bus */
typedef void (*feishu_conversation_cb_t)(const char *chat_id,
                                          const char *sender_name,
                                          const char *text);
/* 注册/注销对话模式回调（传 NULL 注销） */
void feishu_recv_set_conversation_callback(feishu_conversation_cb_t cb);
/* 缓存群成员列表（JSON 格式，由 feishu_chat_members 工具返回） */
void feishu_recv_cache_members(const char *members_json);
/* 按名字模糊匹配群成员，返回 open_id 和显示名；成功返回 true */
bool feishu_recv_find_member_by_name(const char *name,
                                      char *open_id_out, size_t oid_cap,
                                      char *display_name_out, size_t dn_cap);
/* 按 open_id 查找群成员显示名；成功返回 true */
bool feishu_recv_find_member_by_open_id(const char *open_id,
                                         char *display_name_out, size_t dn_cap);

#else /* !CONFIG_AI_AGENT_FEISHU — stubs */

static inline int feishu_bot_init(void) { return 0; }
static inline int feishu_bot_start(void) { return 0; }
static inline int feishu_send_message(const char *c, const char *t) { (void)c; (void)t; return -1; }
static inline int feishu_set_app(const char *a, const char *s) { (void)a; (void)s; return -1; }
static inline int feishu_api_post(const char *p, const char *b, char *r, size_t c) { (void)p; (void)b; (void)r; (void)c; return -1; }
static inline int feishu_api_request(const char *m, const char *p, const char *b, size_t l, char *r, size_t c) { (void)m; (void)p; (void)b; (void)l; (void)r; (void)c; return -1; }
static inline const char *feishu_get_app_id(void) { return ""; }
static inline int feishu_api_post_as_user(const char *p, const char *b, char *r, size_t c) { (void)p; (void)b; (void)r; (void)c; return -1; }
static inline int feishu_set_user_token(const char *t) { (void)t; return -1; }
static inline int feishu_api_request_as_user(const char *m, const char *p, const char *b, size_t l, char *r, size_t c) { (void)m; (void)p; (void)b; (void)l; (void)r; (void)c; return -1; }
static inline int feishu_search_chats(const char *k, char *o, size_t c) { (void)k; (void)o; (void)c; return -1; }
static inline int feishu_get_user_open_id(const char *m, char *o, size_t c) { (void)m; (void)o; (void)c; return -1; }
static inline int feishu_upload_image(const char *p, char *o, size_t c) { (void)p; (void)o; (void)c; return -1; }
static inline int feishu_send_image_message_ex(const char *r, const char *k, const char *t) { (void)r; (void)k; (void)t; return -1; }
static inline int feishu_send_image_message(const char *c, const char *k) { (void)c; (void)k; return -1; }
static inline int feishu_send_text_message_ex(const char *r, const char *t, const char *it) { (void)r; (void)t; (void)it; return -1; }
static inline int feishu_send_image_to_chat(const char *c, const char *p) { (void)c; (void)p; return -1; }
static inline int feishu_doc_append_text(const char *d, const char *t) { (void)d; (void)t; return -1; }
static inline int feishu_doc_append_divider(const char *d) { (void)d; return -1; }
static inline int feishu_doc_append_heading1(const char *d, const char *t) { (void)d; (void)t; return -1; }
static inline int feishu_doc_create_image_block(const char *d, char *o, size_t c) { (void)d; (void)o; (void)c; return -1; }
static inline int feishu_doc_upload_image_media(const char *d, const char *b, const char *p, char *o, size_t c) { (void)d; (void)b; (void)p; (void)o; (void)c; return -1; }
static inline int feishu_doc_patch_image_block(const char *d, const char *b, const char *f) { (void)d; (void)b; (void)f; return -1; }
static inline int feishu_doc_append_image(const char *d, const char *p) { (void)d; (void)p; return -1; }
typedef void (*feishu_doc_created_cb_t)(const char *doc_id, const char *title, void *user_data);
static inline int feishu_set_doc_created_callback(feishu_doc_created_cb_t cb, void *ud) { (void)cb; (void)ud; return 0; }
typedef void (*feishu_conversation_cb_t)(const char *chat_id, const char *sender_name, const char *text);
static inline void feishu_recv_set_conversation_callback(feishu_conversation_cb_t cb) { (void)cb; }
static inline void feishu_recv_cache_members(const char *j) { (void)j; }
static inline bool feishu_recv_find_member_by_name(const char *n, char *o, size_t oc, char *d, size_t dc) { (void)n; (void)o; (void)oc; (void)d; (void)dc; return false; }
static inline bool feishu_recv_find_member_by_open_id(const char *oid, char *d, size_t dc) { (void)oid; (void)d; (void)dc; return false; }

#endif /* CONFIG_AI_AGENT_FEISHU */

#ifdef __cplusplus
}
#endif
