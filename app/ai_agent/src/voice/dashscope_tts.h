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

/* Register the DashScope Qwen3-TTS backend with the voice_tts framework. */
int dashscope_tts_register(void);

/* 运行时切换 TTS 音色。
 * voice 形如 "Cherry" / "Ethan" / "Serena"，需为 qwen3-tts-flash-realtime 支持的音色。
 * 更新内存变量，下次 TTS 合成（每次新建 WS 连接）自动使用新音色，无需主动刷新会话。
 * 线程安全：仅更新内存变量，不涉及 WS 连接操作。
 * 若 voice 与当前相同则跳过（避免重复清缓存）。 */
void dashscope_tts_set_voice(const char *voice);

/* Streaming TTS via DashScope Qwen3 WebSocket. */
int dashscope_tts_ws_synthesize_stream(const char *text,
                                        void (*cb)(const unsigned char *,
                                                   size_t, int, void *),
                                        void *user_data);

#ifdef __cplusplus
}
#endif
