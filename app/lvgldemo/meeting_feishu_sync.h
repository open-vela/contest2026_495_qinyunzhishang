/* c:\Users\100746713\Desktop\workspace\lvgldemo\meeting_feishu_sync.h */
#ifndef MEETING_FEISHU_SYNC_H
#define MEETING_FEISHU_SYNC_H

#include <time.h>

/**
 * 将会议转写记录保存到飞书文档
 *
 * 调用时机：会议转写完成后（在ASR线程内调用）
 * 行为：
 *   1. 创建飞书文档，标题格式："会议记录_YYYYMMDD_HHMMSS"
 *      时间使用会议开始时间，而非保存时间
 *   2. 将完整转写文本按行写入文档（每行一个文本block）
 *   3. 文档保存到"天勤"共享文件夹
 *
 * @param transcription 完整的会议转写文本（可含换行符）
 * @param meeting_start_time 会议开始时间（用于文档标题）
 * @return 0 成功, -1 失败
 */
int meeting_save_to_feishu(const char *transcription, time_t meeting_start_time);

#endif /* MEETING_FEISHU_SYNC_H */