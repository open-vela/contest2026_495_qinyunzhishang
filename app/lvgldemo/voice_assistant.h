/****************************************************************************
 * apps/examples/lvgldemo/voice_assistant.h
 *
 * Voice assistant manager - controls cloud-based AI dialogue
 * and manages idle/dialogue mode switching.
 *
 * Idle mode: Cloud connection maintained (pings only, no mic audio).
 *            Local VAD continues running for wake word detection.
 * Dialogue mode: Cloud mic active, audio streamed to cloud for
 *                real-time ASR + AI + TTS response.
 *
 ****************************************************************************/

#ifndef VOICE_ASSISTANT_H
#define VOICE_ASSISTANT_H

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize voice assistant (DashScope connection, etc.) */
int voice_assistant_init(void);

/* Start voice assistant (connect to DashScope, idle mode — no mic)
 * Local VAD stays running. Cloud connection maintained with pings. */
int voice_assistant_start(void);

/* Stop voice assistant (disconnect from DashScope, close mic if open)
 * If was in dialogue mode, restarts local VAD. */
int voice_assistant_stop(void);

/* Register callback for ASR stream network disconnect notification.
 * When the DashScope thread detects a connection loss (ECONNRESET etc.),
 * it calls this callback to notify the caller immediately, without
 * waiting for the polling timer to detect the WiFi disconnect. */
typedef void (*va_disconnect_cb_t)(void);
void voice_assistant_set_disconnect_callback(va_disconnect_cb_t cb);

/* Deinitialize voice assistant */
void voice_assistant_deinit(void);

/* Check if voice assistant is running */
bool voice_assistant_is_running(void);

/* Check if voice assistant is connected to DashScope (WebSocket established) */
bool voice_assistant_is_connected(void);

/* Enter dialogue mode: stop local VAD, open cloud mic, start streaming audio.
 * Called after local wake word detection (WiFi must be connected). */
int voice_assistant_enter_dialogue(void);

/* Exit dialogue mode: close cloud mic, restart local VAD.
 * Called on silence timeout or WiFi disconnect. */
int voice_assistant_exit_dialogue(void);

/* Suspend all voice listening: stop local VAD (wake word) and exit dialogue
 * (close cloud mic). Used when entering the camera page so photo capture is
 * not interfered by mic recording or wake-word triggers. While suspended, VAD
 * restart paths inside the assistant are suppressed until resume is called. */
void voice_assistant_suspend_listening(void);

/* Resume voice listening: clear the suspend flag only, does NOT restart
 * local VAD. After calling this both cloud mic and local VAD remain off;
 * other pages (e.g. voice assistant page) should enable mic on their own
 * to avoid cross-page mic state confusion.
 * Used when leaving the camera page back to the menu. */
void voice_assistant_resume_listening(void);

/* 查询 listening 是否被 suspend（即 ai_page 不活跃）。
 * 返回 true 表示 ai_page 已退出，本地 VAD 与云端 mic 均应保持关闭。
 * 用于 on_network_disconnected 等回调判断是否该自动重启 VAD。 */
bool voice_assistant_is_listening_suspended(void);

/* Check if dialogue mode is active */
bool voice_assistant_is_dialogue_active(void);

/* Set meeting mode callback - triggered when user says "meeting" keywords */
void voice_assistant_set_meeting_callback(void (*callback)(void));

/* Update AI page wakeup status */
void voice_assistant_update_status(const char *status);

/* 图片AI分析状态回调
 * status: 当前状态描述（如"正在上传图片..."）
 * done: 是否分析完成（true=完成或失败，false=进行中） */
typedef void (*image_status_cb_t)(const char *status, bool done);

/* 启动图片AI分析
 * image_path: 图片文件路径（如"/emmc/xxx.jpg"）
 * 返回 0=已加入发送队列，<0=失败（-ENOTCONN未连接/-EBUSY忙/-EFBIG过大等） */
int voice_assistant_analyze_image(const char *image_path);

/* 查询图片分析是否进行中 */
bool voice_assistant_is_image_busy(void);

/* 强制复位图片分析状态机（state→IDLE，释放未发送数据）。
 * 供 UI 层在检测到状态卡死（如 is_image_busy 恒为 true 但无实际分析进展）时调用。
 * 注意：可能中断正在进行中的分析，仅在确认无有效分析时使用。 */
void voice_assistant_reset_image_state(void);

/* 设置当前活跃的飞书文档，后续图片分析结果自动追加到该文档
 * doc_id: 飞书文档ID，传NULL清空活跃文档
 * title: 文档标题，仅用于日志
 * 副作用：会置 s_need_skip_doc_prefix=true（语音创建文档场景用，
 *         用于截断 LLM 返回的"好的，正在创建文档..."前缀） */
void voice_assistant_set_active_feishu_doc(const char *doc_id, const char *title);

/* 静默版设置活跃飞书文档：仅设置 doc_id + 重置图片计数，
 * 不动 s_need_skip_doc_prefix / s_last_user_question / s_fwd_* 等状态。
 * 用于本地代码直接创建文档（如 camera 页进入时）——无 LLM 介入，
 * 下一条 AI 回复不含创建确认语，无需截断前缀。
 * doc_id 为 NULL 或空串时与 set_active_feishu_doc(NULL,NULL) 等价（清空）。 */
void voice_assistant_set_active_feishu_doc_silent(const char *doc_id, const char *title);

/* 设置文档创建回调的静默模式。
 * silent=true 时，后续 tool_feishu_doc_create 触发的 doc_created 回调
 * 将改用 set_active_feishu_doc_silent（不置 s_need_skip_doc_prefix）。
 * 用于本地代码直接创建文档（如 camera 页进入时）——无 LLM 介入，
 * 下一条 AI 回复不含创建确认语，无需截断前缀。
 * 调用者须在 create 调用返回后立即恢复 silent=false。
 * 线程安全：camera 进入时语音监听已 suspend，不会与语音创建文档并发。 */
void voice_assistant_set_doc_create_silent(bool silent);

/* 查询是否有活跃的飞书文档（用于决定 AI 回复写入文档还是转发聊天） */
bool voice_assistant_has_active_feishu_doc(void);

/* 仅关闭飞书聊天文本转发，不影响 s_fwd_img_analysis_started 等文档写入状态。
 * 用于有活跃文档时确保聊天转发关闭，避免残留的 s_fwd_enabled 导致文本泄露到聊天。 */
void voice_assistant_disable_feishu_forward(void);

/* 注册图片分析状态回调（可在任一线程调用，回调通过lv_async投递） */
void voice_assistant_set_image_status_callback(image_status_cb_t cb);

/* 开启/关闭飞书文本转发。
 * enable=true 时注入 receive_id/id_type，之后 AI 回复(前缀"AI：")与
 * 用户转文字(前缀"我：")会异步转发到该飞书目标。
 * 计数规则：转发 5 条 AI 回复后自动关闭（=图片分析回复 + 4 轮对话回复）。
 * enable=false 或再次调用可重置。receive_id/id_type 可为 NULL（仅关闭时）。
 * 仅当编译开启 CONFIG_AI_AGENT_FEISHU 时生效。 */
void voice_assistant_set_feishu_forward(const char *receive_id,
                                         const char *id_type, bool enable);

/* 对话状态枚举 - 用于麦克风状态指示 */
typedef enum {
    VA_STATE_IDLE = 0,      /* 空闲等待，麦克风白色 */
    VA_STATE_USER_SPEAKING, /* 用户说话/录音中，绿色 */
    VA_STATE_AI_SPEAKING    /* AI TTS播放中，灰色 */
} va_dialog_state_t;

/* 用户语音ASR识别完成回调 */
typedef void (*va_user_asr_cb_t)(const char *text);

/* AI文字回复完成回调（每一句回复完成时调用） */
typedef void (*va_ai_reply_cb_t)(const char *text);

/* 对话状态变化回调（麦克风变色用） */
typedef void (*va_state_cb_t)(va_dialog_state_t state);

/* 注册用户ASR文本回调 */
void voice_assistant_set_user_asr_callback(va_user_asr_cb_t cb);

/* 注册AI回复文本回调 */
void voice_assistant_set_ai_reply_callback(va_ai_reply_cb_t cb);

/* 注册对话状态回调 */
void voice_assistant_set_dialog_state_callback(va_state_cb_t cb);

/* Reset the Feishu fast-path flag.
 * Called by agent_loop when Layer2 does NOT match a Feishu intent
 * (i.e. handle_nl_fast_path returned NULL for a voice message that
 * Layer1 had matched).  This unblocks cloud omni audio processing
 * and allows silence timeout to fire normally. */
void voice_assistant_reset_feishu_fast_path(void);

/* 请求刷新 dashscope 会话指令（语言切换场景）。
 * 线程安全：仅置标志位，实际 session.update 由 voice_assistant 主循环
 * 在 dashscope WS 接收线程发送。若 WS 离线则标志丢失，下次重连时
 * dashscope_asr_stream_open 会用新语言发 session.update 兑底。 */
void voice_assistant_request_lang_refresh(void);

/* 查询当前是否禁止 ai_page 右滑退出。
 * 4种场景：
 * 1. 普通对话：AI回复中
 * 2. 今日消息：飞书快速路径激活（消息归纳中）
 * 3. 飞书查询：飞书快速路径激活（文档查询/搜索中）
 * 4. 飞书对话：飞书对话模式 + TTS播报中
 * 返回 true 表示应阻止右滑退出 */
bool voice_assistant_is_exit_blocked(void);

/* ── 飞书对话模式接口 ─────────────────────────────────────────── */
/* 进入飞书对话模式：设置 chat_id，注册消息回调，用户语音直接发飞书群 */
void voice_assistant_enter_feishu_conversation(const char *chat_id);
/* 退出飞书对话模式：清除回调，重置状态 */
void voice_assistant_exit_feishu_conversation(void);
/* 查询飞书对话模式是否激活 */
bool voice_assistant_is_feishu_conversation_active(void);
/* 飞书消息回调（由 feishu_recv 调用，TTS播报+UI显示） */
void voice_assistant_on_feishu_msg(const char *chat_id,
                                    const char *sender_name,
                                    const char *text);

#ifdef __cplusplus
}
#endif

#endif /* VOICE_ASSISTANT_H */
