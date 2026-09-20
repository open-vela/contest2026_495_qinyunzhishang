/****************************************************************************
 * apps/examples/lvgldemo/ai_page.h
 *
 * AI chat page — connects to ai_agent WebSocket server for
 * simultaneous voice playback + on-screen text display.
 *
 ****************************************************************************/

#ifndef AI_PAGE_H
#define AI_PAGE_H

#include <lvgl/lvgl.h>


void create_ai_page(lv_obj_t *parent);
void ai_page_start(void);
void ai_page_stop(void);
void ai_page_deinit(void);

/* 查询 AI 页面异步清理是否进行中。
 * 返回 true 时 create_ai_page 会拒绝创建新页面，调用方应提示用户等待。 */
bool ai_page_is_deinit_in_progress(void);

/* Boot-time LED default: drive both red (GPIO34) and green (GPIO35)
 * indicators low. Call once from lvgldemo main() before UI creation. */
void ai_page_led_boot_init(void);

/* Wakeup status update (called from lvgldemo) */
void ai_page_update_wakeup_status(const char *status);

/* Send a user message (e.g. ASR transcript) to the AI agent.
 * Called by voice_assistant when the cloud LLM has produced a
 * transcript that should start or continue a conversation. */
int ai_page_send_message(const char *text);

/* Display Omni multimodal response text on screen.
 * Called by voice_assistant when the Omni model returns
 * response.audio_transcript.done.
 * A "HH:MM:" local time prefix is prepended automatically. */
void ai_page_show_omni_response(const char *text);

/* Display user speech transcript on screen (Omni mode).
 * Called by voice_assistant when ASR transcript is received.
 * A "HH:MM:" local time prefix is prepended automatically. */
void ai_page_show_user_message(const char *text);

/* Display feishu conversation message with sender-based styling.
 * 小Q助手: green bg + black text; Others: gray bg + white text. */
void ai_page_show_feishu_msg(const char *text, const char *sender_name);

#endif /* AI_PAGE_H */
