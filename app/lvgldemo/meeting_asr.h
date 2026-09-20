/****************************************************************************
 * apps/examples/lvgldemo/meeting_asr.h
 *
 * Meeting ASR module - transcribe WAV file via DashScope Paraformer
 * realtime WebSocket API.
 *
 * Stage 3.2: Real-time streaming transcription (no speaker diarization).
 *
 ****************************************************************************/

#ifndef MEETING_ASR_H
#define MEETING_ASR_H

#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Transcribe a WAV file synchronously.
 *
 * Calls callback for each transcript segment received from the server.
 * Blocks until transcription completes or fails.
 *
 * @param wav_path   Path to WAV file (16kHz/16bit/mono).
 * @param callback   Called for each transcript segment (text is UTF-8,
 *                   NUL-terminated, valid only during callback).
 *                   sentence_id identifies the sentence; same id means
 *                   same sentence being refined, UI should update line.
 * @param user_data  Opaque pointer passed to callback.
 *
 * @return 0 on success, negative errno on failure.
 */
typedef void (*meeting_asr_result_cb)(const char *text,
                                      int sentence_id,
                                      void *user_data);

/* 进度回调：转写过程中周期性调用，percent取值0-100。
 * 调用方可在UI显示进度条。允许传NULL表示不需要进度。 */
typedef void (*meeting_asr_progress_cb)(int percent, void *user_data);

int meeting_asr_transcribe_file(const char *wav_path,
                                meeting_asr_result_cb callback,
                                meeting_asr_progress_cb progress_cb,
                                void *user_data);

/* 流式转写：从 media_recorder 实例实时读 PCM 数据送云端 ASR。
 * 用于方案B（边录边转）：录音开始时调用，录音停止时通过置位 *stop_flag 退出。
 * 数据源为 media_recorder_read_data(handle, ...)，自然 1:1 节奏，无需 usleep。
 *
 * 暂停支持：*pause_flag 置 1 期间不读不发音频（recorder 已 stop，
 * 读会静默超时误判为结束），等恢复后继续。
 *
 * @param recorder_handle  media_recorder_open 返回的实例（prepare 时 path=NULL）
 * @param callback         同 meeting_asr_transcribe_file
 * @param progress_cb      同上，可传 NULL
 * @param stop_flag        指向调用方维护的停止标志位，置1时函数退出循环
 * @param pause_flag       指向暂停标志位（可NULL）：置1期间休眠等待
 * @param user_data        同上
 * @return 0 成功，负值失败
 */
int meeting_asr_transcribe_stream(void *recorder_handle,
                                  meeting_asr_result_cb callback,
                                  meeting_asr_progress_cb progress_cb,
                                  volatile int *stop_flag,
                                  volatile bool *pause_flag,
                                  void *user_data);

/* 预热连接：后台建立 TLS+WS（不发 run-task）。进入会议页时调用，
 * 点击开始后 transcribe_stream 优先复用，消除连接建立期间
 * （数秒）音频管道积压导致的并头内容丢失 */
int meeting_asr_prepare_conn(void);

/* 释放未被消费的预热连接（页面销毁时调用，防 socket/TLS 泄漏） */
void meeting_asr_discard_warm_conn(void);

/* 连接是否非立即可用（预热未就绪）：调用方据此显示"连接中"提示 */
int meeting_asr_conn_pending(void);

#ifdef __cplusplus
}
#endif

#endif /* MEETING_ASR_H */