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

/* Initialize voice channel (load config keys). */
int voice_channel_init(void);

/* Pre-connect ASR WebSocket in background (call after init). */
void voice_channel_asr_preconnect(void);

/* Start voice recording, ASR, then push text to agent via message_bus. */
int voice_channel_start(void);

/* Stop an active voice session. */
int voice_channel_stop(void);

/* Stop recording and return ASR text to caller (does NOT push inbound).
 * text_out: buffer to receive ASR text, text_cap: buffer capacity.
 * Returns 0 on success, negative errno on failure.
 * If ASR returns empty text, text_out[0] is set to '\0' (not an error). */
int voice_channel_stop_with_text(char *text_out, size_t text_cap);

/* Synthesize text and play back (called from outbound dispatcher). */
int voice_channel_speak(const char *text);

/* Abort active TTS playback (set tts_abort flag + stop audio). */
void voice_channel_abort_tts(void);

/* Check if TTS playback is currently active.
 * Returns 1 if speaking, 0 if idle. */
int voice_channel_is_speaking(void);

/* Abort all active TTS playback immediately (both s_voice.tts_pb and pipeline).
 * Sets tts_abort flag, stops the media player via audio_playback_stop(),
 * and aborts the multi-segment pipeline if active.
 * Does NOT block on audio drain — playback stops instantly. */
void voice_channel_abort_tts(void);

/* Mark that the current request was served via quick path (no LLM). */
void voice_channel_set_quick_path(int hit);

/* ── TTS Pipeline: low-latency sentence-level streaming ─── */

int voice_pipeline_start(void);
int voice_pipeline_push(const char* sentence);
int voice_pipeline_finish(void);
void voice_pipeline_abort(void);
int voice_pipeline_is_active(void);

/* Test TTS: synthesize text, save PCM to file. */
int voice_channel_test_tts(const char *text, const char *out_path);

/* Test ASR: read PCM file, recognize, print result. */
int voice_channel_test_asr(const char *pcm_path);

#ifdef __cplusplus
}
#endif
