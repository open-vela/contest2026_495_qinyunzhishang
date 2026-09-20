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

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ds_asr_stream ds_asr_stream_t;

int dashscope_asr_register(void);

ds_asr_stream_t* dashscope_asr_stream_open(void);
int dashscope_asr_stream_send(ds_asr_stream_t* s,
    const unsigned char* pcm, size_t len);
int dashscope_asr_stream_recv(ds_asr_stream_t* s,
    char* buf, size_t cap);
int dashscope_asr_stream_send_ping(ds_asr_stream_t* s);
int dashscope_asr_stream_finish(ds_asr_stream_t* s,
    char* text_out, size_t text_cap);
void dashscope_asr_stream_abort(ds_asr_stream_t* s);
int dashscope_asr_stream_vad_done(ds_asr_stream_t* s);
int dashscope_asr_stream_clear_buffer(ds_asr_stream_t* s);

/* 图片AI分析：发送静音PCM（满足input_image_buffer.append前置条件）
 * ms：静音时长（毫秒），16kHz/16bit/mono */
int dashscope_asr_stream_send_silent(ds_asr_stream_t* s, size_t ms);

/* 图片AI分析：渐进式发送图片数据（input_image_buffer.append）
 * 图片数据为原始JPEG字节流，内部Base64编码后分块写入TLS缓冲区
 * 内存占用固定约2KB，可处理大图片 */
int dashscope_asr_stream_send_image(ds_asr_stream_t* s,
    const unsigned char* img_data, size_t img_len);

/* 图片AI分析：禁用VAD，切换到Manual模式
 * Manual模式下客户端显式发送commit和response.create */
int dashscope_asr_stream_disable_vad(ds_asr_stream_t* s);

/* 图片AI分析：恢复VAD模式（server_vad）
 * 图片分析完成后调用，恢复语音自动检测 */
int dashscope_asr_stream_enable_vad(ds_asr_stream_t* s);

/* 图片AI分析：提交音频+图片缓冲区（Manual模式）
 * 发送 input_audio_buffer.commit，服务端回复 input_audio_buffer.committed
 * 收到committed后再调用 dashscope_asr_stream_commit 发送response.create */
int dashscope_asr_stream_commit_buffer(ds_asr_stream_t* s);

/* 图片AI分析：发送response.create，触发云端响应
 * 必须在收到 input_audio_buffer.committed 事件后调用
 * 内部在 response.create 中携带图片分析专用 instructions（响应级覆盖），
 * 控制模型输出行为（一句话概括主体 + 2-3 句细节） */
int dashscope_asr_stream_commit(ds_asr_stream_t* s);

/* 设置回复语言（国际化支持）
 * lang 形如 "Chinese" / "English" / "Japanese" / "Spanish"，默认 "Chinese"
 * 同时作用于 session.update（普通语音对话）和 response.create（图片分析），
 * 保证整个会话语言一致。建议在 session 建立前调用；
 * 运行时切换需在下次重连或 restore_omni 后生效。 */
void dashscope_asr_set_image_reply_lang(const char *lang);

/* 飞书对话模式：切换为纯ASR模式（仅语音转文字，不触发LLM回复）
 * 通过 session.update 将 modalities 改为 ["text"]，
 * 清空 instructions，禁用 turn_detection，
 * 云端仅做语音转写，不生成 AI 回复。 */
int dashscope_asr_stream_set_asr_only(ds_asr_stream_t* s);

/* 飞书对话模式：恢复为 Omni 多模态模式（ASR+LLM+TTS）
 * 退出飞书对话模式后调用，恢复正常语音助手功能。 */
int dashscope_asr_stream_restore_omni(ds_asr_stream_t* s);

/* 运行时刷新会话指令（重发 session.update，复用当前 g_img_reply_lang）。
 * 用于语言切换后让在线会话立即生效，无需断开重连。
 * 必须在 dashscope WS 接收线程（主循环）调用，线程安全。 */
int dashscope_asr_stream_update_session_instructions(ds_asr_stream_t* s);

#ifdef __cplusplus
}
#endif
