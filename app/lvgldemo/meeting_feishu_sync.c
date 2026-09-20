/* c:\Users\100746713\Desktop\workspace\lvgldemo\meeting_feishu_sync.c */
#include "meeting_feishu_sync.h"
#include "tools/tool_feishu_doc.h"
#include "cJSON.h"
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <syslog.h>
#include "agent_config.h"

static const char *TAG = "feishu_sync";

int meeting_save_to_feishu(const char *transcription, time_t meeting_start_time)
{
    if (!transcription || !transcription[0]) {
        syslog(LOG_WARNING, "[%s] empty transcription, skip\n", TAG);
        return -1;
    }

    syslog(LOG_INFO, "[%s] ====== SAVING MEETING TO FEISHU ======\n", TAG);
    syslog(LOG_INFO, "[%s] transcription length=%zu\n", TAG, strlen(transcription));

    /* Step 1: 生成带会议开始时间戳的标题（使用agent_tz_offset_sec确保北京时间） */
    struct tm tm;
    time_t local_epoch = meeting_start_time + agent_tz_offset_sec();
    gmtime_r(&local_epoch, &tm);

    char title[128];
    snprintf(title, sizeof(title), "会议记录_%04d%02d%02d_%02d%02d%02d",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec);

    syslog(LOG_INFO, "[%s] title=%s\n", TAG, title);

    /* Step 2: 创建飞书文档（显式传入会议专用 folder_token，与同事功能隔离） */
    char create_input[256];
    snprintf(create_input, sizeof(create_input),
             "{\"title\":\"%s\",\"folder_token\":\"%s\"}",
             title, AGENT_SECRET_FEISHU_MEETING_FOLDER_TOKEN);

    char create_output[2048];
    memset(create_output, 0, sizeof(create_output));

    int ret = tool_feishu_doc_create_execute(create_input,
                                             create_output, sizeof(create_output));
    if (ret != 0) {
        syslog(LOG_ERR, "[%s] create failed: %s\n", TAG, create_output);
        return -1;
    }
    syslog(LOG_INFO, "[%s] document created: %.200s\n", TAG, create_output);

    /* Step 3: 解析 document_id */
    cJSON *create_resp = cJSON_Parse(create_output);
    if (!create_resp) {
        syslog(LOG_ERR, "[%s] parse response failed\n", TAG);
        return -1;
    }
    cJSON *doc_id_item = cJSON_GetObjectItem(create_resp, "document_id");
    if (!doc_id_item || !cJSON_IsString(doc_id_item) ||
        !doc_id_item->valuestring || !doc_id_item->valuestring[0]) {
        syslog(LOG_ERR, "[%s] no document_id in response\n", TAG);
        cJSON_Delete(create_resp);
        return -1;
    }

    char doc_id[64];
    strncpy(doc_id, doc_id_item->valuestring, sizeof(doc_id) - 1);
    doc_id[sizeof(doc_id) - 1] = '\0';
    cJSON_Delete(create_resp);

    syslog(LOG_INFO, "[%s] document_id=%s\n", TAG, doc_id);

    /* Step 4: 写入转写内容
     * 关键点：transcription 中的真实换行符需要转义为 \\n，
     * 因为要通过 JSON 传递给 tool_feishu_doc_write_execute */
    size_t content_len = strlen(transcription) * 2 + 64; /* 转义后可能翻倍 */
    char *write_input = malloc(content_len);
    if (!write_input) {
        syslog(LOG_ERR, "[%s] malloc failed for write_input\n", TAG);
        return -1;
    }

    /* 手动转义换行符：\n → \\n（在C字符串中是 \\n）*/
    char *escaped_content = malloc(content_len);
    if (!escaped_content) {
        free(write_input);
        syslog(LOG_ERR, "[%s] malloc failed for escaped_content\n", TAG);
        return -1;
    }
    
    size_t j = 0;
    for (size_t i = 0; transcription[i] && j < content_len - 2; i++) {
        if (transcription[i] == '\n') {
            escaped_content[j++] = '\\';
            escaped_content[j++] = 'n';
        } else {
            escaped_content[j++] = transcription[i];
        }
    }
    escaped_content[j] = '\0';

    snprintf(write_input, content_len,
             "{\"document_id\":\"%s\",\"content\":\"%s\"}",
             doc_id, escaped_content);

    char write_output[1024];
    memset(write_output, 0, sizeof(write_output));

    ret = tool_feishu_doc_write_execute(write_input,
                                        write_output, sizeof(write_output));

    free(escaped_content);
    free(write_input);

    if (ret == 0) {
        syslog(LOG_INFO, "[%s] ====== MEETING SAVED TO FEISHU SUCCESS ======\n", TAG);
        syslog(LOG_INFO, "[%s] 请去飞书查看: %s\n", TAG, title);
    } else {
        syslog(LOG_ERR, "[%s] write failed: %s\n", TAG, write_output);
    }

    return ret;
}