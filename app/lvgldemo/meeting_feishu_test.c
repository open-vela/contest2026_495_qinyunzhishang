#include "meeting_feishu_test.h"
#include "tools/tool_feishu_doc.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <syslog.h>

static const char *TAG = "feishu_test";

int meeting_feishu_test_run(void)
{
    static bool s_tested = false;
    if (s_tested) {
        return 0;
    }
    s_tested = true;

    /* Step 1: 生成时间戳字符串（用于文档标题） */
    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct tm tm;
    localtime_r(&tv.tv_sec, &tm);

    char time_str[32];
    snprintf(time_str, sizeof(time_str), "%04d%02d%02d_%02d%02d%02d",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec);

    /* Step 2: 创建"[测试中]"文档 */
    char title[128];
    snprintf(title, sizeof(title), "[测试中]飞书写入验证_%s", time_str);

    char create_input[256];
    snprintf(create_input, sizeof(create_input), "{\"title\":\"%s\"}", title);

    char create_output[2048];
    memset(create_output, 0, sizeof(create_output));

    int ret = tool_feishu_doc_create_execute(create_input,
                                             create_output, sizeof(create_output));
    if (ret != 0) {
        syslog(LOG_ERR, "[%s] create [测试中] failed: %s\n", TAG, create_output);
        return -1;
    }

    /* Step 3: 用 cJSON 解析 document_id */
    cJSON *create_resp = cJSON_Parse(create_output);
    if (!create_resp) {
        syslog(LOG_ERR, "[%s] parse create output failed\n", TAG);
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

    syslog(LOG_INFO, "[%s] [测试中] document created: %s\n", TAG, doc_id);

    /* Step 4: 写入测试内容（3行文本）
     * 关键点：C源码中用 \\n（两字符：反斜杠+n），写入JSON后是 \n 转义序列，
     * cJSON解析后还原为真实换行符，tool_feishu_doc.c 按换行分块为3个文本block */
    char test_content[256];
    snprintf(test_content, sizeof(test_content),
             "飞书文档写入测试成功\\n"
             "document_id: %s\\n"
             "时间: %04d-%02d-%02d %02d:%02d:%02d",
             doc_id,
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec);

    char write_input[768];
    snprintf(write_input, sizeof(write_input),
             "{\"document_id\":\"%s\",\"content\":\"%s\"}",
             doc_id, test_content);

    char write_output[1024];
    memset(write_output, 0, sizeof(write_output));

    ret = tool_feishu_doc_write_execute(write_input,
                                        write_output, sizeof(write_output));

    /* Step 5: 创建最终状态文档（[成功]或[失败]）
     * 用户在飞书端只需看这个文档名就知道结果，无需查串口日志 */
    char final_title[128];
    if (ret == 0) {
        snprintf(final_title, sizeof(final_title), "[成功]飞书写入验证_%s", time_str);
        syslog(LOG_INFO, "[%s] ====== FEISHU DOC WRITE TEST SUCCESS ======\n", TAG);
    } else {
        snprintf(final_title, sizeof(final_title), "[失败]飞书写入验证_%s", time_str);
        syslog(LOG_ERR, "[%s] write failed: %s\n", TAG, write_output);
    }

    char final_input[256];
    snprintf(final_input, sizeof(final_input), "{\"title\":\"%s\"}", final_title);

    char final_output[1024];
    memset(final_output, 0, sizeof(final_output));
    tool_feishu_doc_create_execute(final_input, final_output, sizeof(final_output));

    return ret;
}