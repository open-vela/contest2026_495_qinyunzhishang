/****************************************************************************
 * apps/examples/lvgldemo/lvgl_dispatch.c
 *
 * 跨线程 UI 派发器实现。详见 lvgl_dispatch.h。
 *
 ****************************************************************************/

#include "lvgl_dispatch.h"

#include <pthread.h>
#include <string.h>
#include <syslog.h>

/* 队列容量：32 槽 × 8 字节 = 256 字节静态内存。
 * 足够覆盖 AI 对话高峰（一轮对话通常产生 2-4 条消息 + 状态更新）。
 * 队列满时丢弃新项并打日志，避免无限增长。 */
#define LVGL_DISPATCH_Q_SIZE 32

typedef struct {
    lvgl_dispatch_cb_t cb;
    void              *data;
} dispatch_item_t;

static dispatch_item_t s_queue[LVGL_DISPATCH_Q_SIZE];

/* 环形队列索引：
 *   s_head — 出队位置，仅由 LVGL 主线程在 drain() 中写
 *   s_tail — 入队位置，仅由生产者线程在 async() 中写
 * 用互斥锁保护，因为 head/tail 的读改写不是原子的。 */
static volatile int s_head = 0;
static volatile int s_tail = 0;

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;

/* 累计丢弃计数，便于调试观察。 */
static int s_dropped = 0;

bool lvgl_dispatch_async(lvgl_dispatch_cb_t cb, void *data)
{
    if (!cb) {
        return false;
    }

    pthread_mutex_lock(&s_lock);

    int next_tail = (s_tail + 1) % LVGL_DISPATCH_Q_SIZE;
    if (next_tail == s_head) {
        /* 队列满：丢弃新项，调用者负责释放 data */
        s_dropped++;
        pthread_mutex_unlock(&s_lock);
        syslog(LOG_WARNING,
               "[dispatch] queue full, drop (total dropped=%d)\n",
               s_dropped);
        return false;
    }

    s_queue[s_tail].cb   = cb;
    s_queue[s_tail].data = data;
    s_tail = next_tail;

    pthread_mutex_unlock(&s_lock);
    return true;
}

void lvgl_dispatch_drain(void)
{
    /* 预算限制：单次最多消费队列容量项，防止 cb 内再次入队
     * 导致主循环被饿死（虽然实际场景罕见）。 */
    int budget = LVGL_DISPATCH_Q_SIZE;

    while (budget-- > 0) {
        pthread_mutex_lock(&s_lock);
        if (s_head == s_tail) {
            pthread_mutex_unlock(&s_lock);
            return;
        }

        /* 拷贝出条目后立刻释放锁，cb 在锁外执行：
         *   1. 避免 cb 执行时间长阻塞生产者入队
         *   2. 避免 cb 内再次调用 dispatch_async 死锁 */
        dispatch_item_t item = s_queue[s_head];
        s_head = (s_head + 1) % LVGL_DISPATCH_Q_SIZE;
        pthread_mutex_unlock(&s_lock);

        item.cb(item.data);
    }
}
