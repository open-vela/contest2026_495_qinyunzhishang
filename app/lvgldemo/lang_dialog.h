/****************************************************************************
 * apps/examples/lvgldemo/lang_dialog.h
 *
 * 语言设置页面（独立页面，参考 camera_page 布局）：
 *   - 顶部标题栏：返回 + 标题 + 时间
 *   - 语言选项列表：圆点标记选中状态
 *   - 结果提示区：内嵌在页面中（3秒自动消失）
 *   - 底部确认/取消按钮
 *   - 全程在页面内操作，右滑退出到设置页
 *
 ****************************************************************************/

#ifndef LANG_DIALOG_H
#define LANG_DIALOG_H

#include <lvgl/lvgl.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 进入语言设置页面（用户点击"语言"项时调用） */
void lang_page_enter(void);

/* 销毁语言设置页面资源 */
void lang_page_deinit(void);

/* 显示拦截提示弹窗（手动关闭）
 * msg_id 为 STR_BLOCK_CALL / STR_BLOCK_RECORDER / STR_BLOCK_MEETING 之一
 */
void lang_show_block(int msg_id);

#ifdef __cplusplus
}
#endif

#endif /* LANG_DIALOG_H */
