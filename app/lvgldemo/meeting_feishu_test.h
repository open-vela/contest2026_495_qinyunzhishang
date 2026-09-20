#ifndef MEETING_FEISHU_TEST_H
#define MEETING_FEISHU_TEST_H

/**
 * 阶段1验证：创建飞书文档并写入3行测试内容
 * 验证 tenant_access_token 能否用于 docx blocks children API
 *
 * 调用时机：meeting_page_start 开头（带一次性保护，仅第一次执行）
 * 预期结果：飞书"天勤"共享文件夹出现2个文档：
 *   - [测试中]飞书写入验证_时间戳（含3行内容）
 *   - [成功]或[失败]飞书写入验证_时间戳（状态指示器）
 *
 * @return 0 成功, -1 失败
 */
int meeting_feishu_test_run(void);

#endif /* MEETING_FEISHU_TEST_H */