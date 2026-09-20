/****************************************************************************
 * apps/examples/lvgldemo/meeting_page.h
 *
 * Meeting transcription page - displays real-time meeting transcript
 * with speaker labels and a meeting timer.
 *
 ****************************************************************************/

#ifndef MEETING_PAGE_H
#define MEETING_PAGE_H

#include <lvgl/lvgl.h>
#include <stdbool.h>

void create_meeting_page(lv_obj_t *parent);
void meeting_page_start(void);
void meeting_page_stop(void);
void meeting_page_add_transcript(const char *speaker, const char *text);
void meeting_page_update_asr_transcript(const char *text, int sentence_id);
void meeting_page_update_status(const char *status);
void meeting_page_cleanup(void);
void meeting_page_deinit(void);

/* 查询会议是否活跃中（供语言切换拦截用） */
bool meeting_is_active(void);

#endif /* MEETING_PAGE_H */