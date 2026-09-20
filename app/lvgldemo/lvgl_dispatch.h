/****************************************************************************
 * apps/examples/lvgldemo/lvgl_dispatch.h
 *
 * 跨线程 UI 派发器：把其他线程的回调安全地投递到 LVGL 主线程执行。
 *
 * 背景：LVGL 不是线程安全的，lv_async_call 内部直接修改全局 timer
 * 链表，多线程并发调用会与主线程的 lv_timer_handler 竞态，导致
 * 节点损坏、跳转到野指针触发 Usage Fault (INVSTATE)。
 *
 * 本模块用一个固定大小的环形队列 + 互斥锁实现"投递-消费"模型：
 *   - 其他线程调用 lvgl_dispatch_async(cb, data) 入队
 *   - LVGL 主线程通过 lvgl_dispatch_drain() 出队并执行 cb
 * 所有 LVGL API 调用因此都发生在主线程，彻底消除竞态。
 *
 * data 的所有权和释放由 cb 自行管理（与 lv_async_call 一致）。
 *
 ****************************************************************************/

#ifndef LVGLDEMO_DISPATCH_H
#define LVGLDEMO_DISPATCH_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 回调签名：与 lv_async_cb_t 兼容，便于直接替换。 */
typedef void (*lvgl_dispatch_cb_t)(void *user_data);

/* 从任意线程调用：把 (cb, data) 投递到队列，等待 LVGL 主线程消费。
 * 返回 true 表示入队成功；false 表示队列满，调用者需自行释放 data。
 * 线程安全。 */
bool lvgl_dispatch_async(lvgl_dispatch_cb_t cb, void *data);

/* 在 LVGL 主线程周期性调用：消费队列中所有待执行项。
 * 必须在 LVGL 主线程（lv_timer_handler 上下文）调用，否则无效。
 * 单次最多消费队列容量项，避免主循环被饿死。 */
void lvgl_dispatch_drain(void);

#ifdef __cplusplus
}
#endif

#endif /* LVGLDEMO_DISPATCH_H */
