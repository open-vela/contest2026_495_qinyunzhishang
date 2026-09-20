#ifndef CAMERA_FEISHU_DOC_H
#define CAMERA_FEISHU_DOC_H

/* camera 页进入时自动创建飞书文档，用于本会话内所有拍照的图片+AI回复追加。
 *
 * 生命周期：
 *   - 进入 camera 拍照界面（从 menu 进入，非从 AI 页返回）→ create_async()
 *   - 退出 camera 回 menu / camera_page_deinit → cancel()
 *   - camera ↔ camera_ai 之间切换不触发，文档保留以支持多图追加
 *
 * 文档标题："图文智录_YYYYMMDD_HHMMSS"（北京时间，用 agent_tz_offset_sec）
 * 文件夹：folder_token 硬编码（camera 专属，区别于 meeting 默认文件夹）
 *
 * 线程模型：DETACHED 后台线程，不阻塞 UI/拍照；cancel 不 join，仅置停止标志。
 * 失败处理：飞书未配置或创建失败时静默退出，不设活跃文档（AI 按钮自动降级走聊天）。 */

/* 异步创建飞书文档（进入 camera 页时调用）。
 * 若上一轮创建线程仍在跑，会先停止它再启动新的。
 * 用户无感：不弹任何 UI，仅 syslog 到内核日志。 */
void camera_feishu_doc_create_async(void);

/* 取消正在跑的创建线程并清空活跃飞书文档（退出 camera 页时调用）。
 * - 置停止标志让线程自行退出（不 join，避免阻塞 UI）
 * - 清 voice_assistant 的活跃文档状态，使后续 AI 回复降级走聊天
 *   （正常退出场景下不会触发降级，仅作为兜底防护） */
void camera_feishu_doc_cancel(void);

#endif /* CAMERA_FEISHU_DOC_H */
