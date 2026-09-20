/****************************************************************************
 * apps/examples/lvgldemo/wakeup_detector.cpp
 *
 * Voice wake word detector — uses platform VAD recorder source to detect
 * the wake phrase "xiaoQxiaoQ!".
 *
 * When a wake word is detected, the registered callback is invoked.
 * The callback decides what prompt to play and whether to continue
 * or stop VAD (e.g. hand the mic to the cloud dialogue mode).
 *
 * The VAD recorder source ("Vad") handles keyword spotting internally.
 * Wake-up events are delivered through the media_recorder event callback.
 * The main thread waits on a semaphore for wake-up notification.
 *
 ****************************************************************************/

#include <nuttx/config.h>

#include <cerrno>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdbool.h>
#include <cstdio>
#include <cstdlib>
#include <syslog.h>
#include <unistd.h>

#include <media_recorder.h>
#include <media_player.h>
#include <media_policy.h>
#include <media_defs.h>

#include "wakeup_detector.h"

#define TAG "wakeup"

#define WAKEUP_SAMPLE_RATE  16000
#define WAKEUP_CHANNELS     1
#define WAKEUP_BITS         16
#define WAKEUP_STACK_SIZE   (8 * 1024)

#ifndef CONFIG_EXAMPLES_LVGLDEMO_WAKEUP_COOLDOWN_MS
#define CONFIG_EXAMPLES_LVGLDEMO_WAKEUP_COOLDOWN_MS 3000
#endif

#define RECORDER_OPEN_RETRIES   10
#define RECORDER_RETRY_INTERVAL 200000

/* 静音预热：100ms PCM数据量（16kHz/16bit/mono = 3200 bytes） */
#define WARMUP_SILENCE_MS     100
#define WARMUP_SILENCE_BYTES  (WAKEUP_SAMPLE_RATE * WAKEUP_BITS / 8 * WAKEUP_CHANNELS * WARMUP_SILENCE_MS / 1000)

#define WAKEUP_RECORDER_SOURCE "Vad"

static volatile bool s_wk_running = false;
static volatile bool s_wk_initialized = false;
static volatile bool s_wk_thread_active = false;
static pthread_t s_wk_thread;
static void *s_recorder;
static void *g_player = nullptr;

static sem_t s_prompt_sem;
static sem_t s_wakeup_sem;

/* Callback */
static wakeup_callback_t s_callback = nullptr;

/* External prompt request (from other threads) */
static char s_prompt_request_path[256] = {0};
static volatile bool s_prompt_request_pending = false;
static volatile bool s_warmup_mode = false;    /* 静音预热模式：音量设为0 */
static volatile bool s_warmup_error = false;   /* 预热 player 事件出错 */
static volatile bool s_warmup_running = false; /* 预热线程运行中 */
static pthread_t s_warmup_thread;              /* 预热线程句柄 */

static void vad_recorder_on_event(void *cookie, int event, int result,
                                  const char *extra)
{
  (void)extra;
  (void)cookie;

  switch (event)
    {
      case MEDIA_EVENT_PREPARED:
        syslog(LOG_INFO, "[%s] VAD recorder prepared\n", TAG);
        break;

      case MEDIA_EVENT_STARTED:
        if (result)
          {
            syslog(LOG_ERR, "[%s] VAD recorder start error %d\n",
                   TAG, result);
          }
        else
          {
            syslog(LOG_INFO, "[%s] VAD recorder started\n", TAG);
          }
        break;

      case MEDIA_EVENT_STOPPED:
        syslog(LOG_INFO, "[%s] VAD recorder stopped\n", TAG);
        break;

      case MEDIA_EVENT_COMPLETED:
        syslog(LOG_INFO, "[%s] VAD wake word detected (event=COMPLETED)\n",
               TAG);
        sem_post(&s_wakeup_sem);
        break;

      default:
        syslog(LOG_INFO, "[%s] VAD event %d result %d\n",
               TAG, event, result);
        break;
    }
}

static void prompt_player_on_event(void *cookie, int event, int result,
                                   const char *extra)
{
  (void)cookie;
  (void)extra;

  if (result < 0)
    {
      s_warmup_error = true;
      syslog(LOG_WARNING, "[%s] prompt player event %d error %d\n",
             TAG, event, result);
      sem_post(&s_prompt_sem);
      return;
    }

  switch (event)
    {
      case MEDIA_EVENT_PREPARED:
        {
          void *handle = (void *)cookie;
          media_player_start(handle);
        }
        break;

      case MEDIA_EVENT_STARTED:
        {
          void *handle = (void *)cookie;
          media_player_set_volume(handle, s_warmup_mode ? 0.0f : 1.0f);
          /* 预热模式：STARTED 即唤醒等待者；do_play_prompt 不受影响
           *（其运行时 s_warmup_mode=false，仍等 COMPLETED/STOPPED） */
          if (s_warmup_mode)
            sem_post(&s_prompt_sem);
        }
        break;

      case MEDIA_EVENT_COMPLETED:
      case MEDIA_EVENT_STOPPED:
        sem_post(&s_prompt_sem);
        break;

      default:
        break;
    }
}

/* Internal: play a prompt sound by path.
 * Stops/closes the VAD recorder if open, plays the prompt, does NOT reopen. */
static void do_play_prompt(const char *path)
{
  syslog(LOG_INFO, "[%s] playing prompt: %s\n", TAG, path);

  /* 若后台预热尚未完成，等待其结束后再操作 g_player，避免并发冲突。
   * 预热最坏约9s（10次×800ms），VAD 线程阻塞等待可接受。 */
  if (s_warmup_running)
    {
      syslog(LOG_INFO, "[%s] waiting for warm-up thread to finish...\n", TAG);
      pthread_join(s_warmup_thread, NULL);
      s_warmup_running = false;
    }

  if (s_recorder)
    {
      media_recorder_stop(s_recorder);
      usleep(50000);
      media_recorder_close(s_recorder);
      usleep(100000);
      s_recorder = NULL;
    }

  if (g_player)
    {
      media_player_stop(g_player);
      media_player_close(g_player, 0);
      g_player = NULL;
      usleep(100000);
    }

  g_player = media_player_open(MEDIA_STREAM_MUSIC);
  if (!g_player)
    {
      syslog(LOG_ERR, "[%s] prompt: media_player_open failed\n", TAG);
      return;
    }

  sem_init(&s_prompt_sem, 0, 0);

  media_player_set_event_callback(g_player, g_player, prompt_player_on_event);

  int mute = 0;
  media_policy_get_mute_mode(&mute);
  if (mute)
    {
      media_policy_set_mute_mode(0);
    }

  int vol_min = 0;
  int vol_max = 10;
  media_policy_get_range(MEDIA_STREAM_MEDIA MEDIA_POLICY_VOLUME,
                         &vol_min, &vol_max);
  media_policy_set_stream_volume(MEDIA_STREAM_MEDIA, vol_max);

  vol_min = 0;
  vol_max = 10;
  media_policy_get_range(MEDIA_STREAM_MUSIC MEDIA_POLICY_VOLUME,
                         &vol_min, &vol_max);
  media_policy_set_stream_volume(MEDIA_STREAM_MUSIC, vol_max);

  usleep(200000);

  if (media_player_prepare(g_player, path, NULL) != 0)
    {
      syslog(LOG_ERR, "[%s] prompt: media_player_prepare failed\n", TAG);
      media_player_close(g_player, 0);
      usleep(100000);
      g_player = NULL;
      sem_destroy(&s_prompt_sem);
      return;
    }

  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  ts.tv_sec += 10;

  int sret = sem_timedwait(&s_prompt_sem, &ts);
  if (sret != 0)
    {
      syslog(LOG_WARNING, "[%s] prompt: timed out waiting for completion\n",
             TAG);
    }

  media_player_stop(g_player);
  media_player_close(g_player, 0);
  usleep(100000);
  g_player = NULL;
  sem_destroy(&s_prompt_sem);

  syslog(LOG_INFO, "[%s] prompt playback done\n", TAG);
}

static int open_vad_recorder(void)
{
  int retries = 0;
  while (retries < RECORDER_OPEN_RETRIES)
    {
      s_recorder = media_recorder_open(WAKEUP_RECORDER_SOURCE);
      if (s_recorder) break;
      syslog(LOG_WARNING, "[%s] VAD recorder open retry %d/%d\n",
             TAG, retries + 1, RECORDER_OPEN_RETRIES);
      usleep(RECORDER_RETRY_INTERVAL);
      retries++;
    }

  if (!s_recorder)
    {
      syslog(LOG_ERR, "[%s] VAD recorder open failed after %d retries\n",
             TAG, RECORDER_OPEN_RETRIES);
      return -1;
    }

  media_recorder_set_event_callback(s_recorder, s_recorder,
                                    vad_recorder_on_event);

  return 0;
}

static int recorder_prepare_and_start(void)
{
  char opts[128];
  snprintf(opts, sizeof(opts),
           "fmt=[rate=#%u,ch=#%u,bits=#%u,width=#2],enc=[keys=pcm,imin=#256]",
           WAKEUP_SAMPLE_RATE, WAKEUP_CHANNELS, WAKEUP_BITS);

  if (media_recorder_prepare(s_recorder, NULL, opts) < 0)
    {
      syslog(LOG_ERR, "[%s] media_recorder_prepare failed\n", TAG);
      media_recorder_close(s_recorder);
      s_recorder = NULL;
      return -1;
    }

  if (media_recorder_start(s_recorder) < 0)
    {
      syslog(LOG_ERR, "[%s] media_recorder_start failed\n", TAG);
      media_recorder_close(s_recorder);
      s_recorder = NULL;
      return -1;
    }

  return 0;
}

static void wakeup_detected(void)
{
  syslog(LOG_INFO, "[%s] *** WAKE WORD DETECTED: 'xiaoQxiaoQ!' ***\n", TAG);
  printf("Resume by xiaoQ!\n");

  if (s_callback)
    {
      wakeup_action_t action = s_callback();
      if (action == WAKEUP_ACTION_STOP)
        {
          syslog(LOG_INFO, "[%s] callback requested STOP, halting VAD\n", TAG);
          s_wk_running = false;
        }
    }
  else
    {
      /* No callback registered — do nothing, just log */
      syslog(LOG_INFO, "[%s] no callback registered for wake word\n", TAG);
    }
}

static void *wakeup_thread(void *arg)
{
  (void)arg;

  syslog(LOG_INFO, "[%s] wakeup thread started\n", TAG);

  sem_init(&s_wakeup_sem, 0, 0);

  if (open_vad_recorder() < 0)
    {
      s_wk_running = false;
      s_wk_thread_active = false;
      sem_destroy(&s_wakeup_sem);
      return NULL;
    }

  if (recorder_prepare_and_start() < 0)
    {
      s_wk_running = false;
      s_wk_thread_active = false;
      sem_destroy(&s_wakeup_sem);
      return NULL;
    }

  syslog(LOG_INFO, "[%s] VAD audio capture started (%uHz %uch %ubit)\n",
         TAG, WAKEUP_SAMPLE_RATE, WAKEUP_CHANNELS, WAKEUP_BITS);

  /* Check for pending prompt request that arrived before thread started */
  if (s_prompt_request_pending)
    {
      s_prompt_request_pending = false;
      syslog(LOG_INFO, "[%s] processing pending prompt request: %s\n",
             TAG, s_prompt_request_path);
      do_play_prompt(s_prompt_request_path);
      if (!s_wk_running)
        {
          goto thread_exit;
        }
      if (open_vad_recorder() < 0)
        {
          syslog(LOG_ERR, "[%s] cannot reopen VAD recorder after prompt\n", TAG);
          goto thread_exit;
        }
      if (recorder_prepare_and_start() < 0)
        {
          syslog(LOG_ERR, "[%s] cannot restart VAD recorder after prompt\n", TAG);
          goto thread_exit;
        }
    }

  while (s_wk_running)
    {
      struct timespec ts;
      clock_gettime(CLOCK_REALTIME, &ts);
      ts.tv_sec += 1;

      int sret = sem_timedwait(&s_wakeup_sem, &ts);
      if (sret != 0)
        {
          /* Timeout — check for pending prompt request */
          if (s_prompt_request_pending)
            {
              s_prompt_request_pending = false;
              syslog(LOG_INFO, "[%s] external prompt request: %s\n",
                     TAG, s_prompt_request_path);
              do_play_prompt(s_prompt_request_path);
              if (!s_wk_running)
                {
                  break;
                }
              if (open_vad_recorder() < 0)
                {
                  syslog(LOG_ERR, "[%s] cannot reopen VAD recorder\n", TAG);
                  break;
                }
              if (recorder_prepare_and_start() < 0)
                {
                  syslog(LOG_ERR, "[%s] cannot restart VAD recorder\n", TAG);
                  break;
                }
              syslog(LOG_INFO, "[%s] VAD audio capture resumed\n", TAG);
            }
          continue;
        }

      if (!s_wk_running)
        {
          break;
        }

      /* Check if this was a prompt request signal */
      if (s_prompt_request_pending)
        {
          s_prompt_request_pending = false;
          syslog(LOG_INFO, "[%s] external prompt request: %s\n",
                 TAG, s_prompt_request_path);
          do_play_prompt(s_prompt_request_path);
          if (!s_wk_running)
            {
              break;
            }
          if (open_vad_recorder() < 0)
            {
              syslog(LOG_ERR, "[%s] cannot reopen VAD recorder\n", TAG);
              break;
            }
          if (recorder_prepare_and_start() < 0)
            {
              syslog(LOG_ERR, "[%s] cannot restart VAD recorder\n", TAG);
              break;
            }
          syslog(LOG_INFO, "[%s] VAD audio capture resumed\n", TAG);
          continue;
        }

      /* Wake word detected */
      wakeup_detected();

      if (!s_wk_running)
        {
          break;
        }

      if (open_vad_recorder() < 0)
        {
          syslog(LOG_ERR, "[%s] cannot reopen VAD recorder, stopping\n", TAG);
          break;
        }

      if (recorder_prepare_and_start() < 0)
        {
          syslog(LOG_ERR, "[%s] cannot restart VAD recorder, stopping\n", TAG);
          break;
        }

      syslog(LOG_INFO, "[%s] VAD audio capture resumed\n", TAG);
    }

thread_exit:
  if (s_recorder)
    {
      media_recorder_stop(s_recorder);
      usleep(50000);
      media_recorder_close(s_recorder);
      s_recorder = NULL;
    }

  sem_destroy(&s_wakeup_sem);

  s_wk_thread_active = false;
  syslog(LOG_INFO, "[%s] wakeup thread exited\n", TAG);
  return NULL;
}

/* 后台预热线程：在临时线程中重试 codec 预热，不阻塞 LVGL 主线程。
 * do_play_prompt 调用前会 pthread_join 等待本线程结束，确保 g_player
 * 无并发访问。 */
static void *warmup_thread_func(void *arg)
{
  (void)arg;

  sem_init(&s_prompt_sem, 0, 0);
  media_player_set_event_callback(g_player, g_player, prompt_player_on_event);

  s_warmup_mode = true;   /* 静音模式：音量设为0 */

  bool warmup_ok = false;
  const char *opts = "format=s16le,sample_rate=16000,channels=1";

  /* 重试最多10次，每次等待 STARTED 超时800ms。
   * 原先 usleep(300000) 盲等：player start 失败时 write_data 在异常
   * stream 上阻塞，卡死 LVGL 主线程（开机黑屏根因）。改为事件驱动 +
   * 重试，最坏耗时 10×(800ms+100ms)≈9s。 */
  for (int i = 0; i < 10; i++)
    {
      /* drain 上一轮 stop 等触发的残留 sem_post，避免本轮立即返回 */
      while (sem_trywait(&s_prompt_sem) == 0) {}

      s_warmup_error = false;

      if (media_player_prepare(g_player, NULL, opts) != 0)
        {
          syslog(LOG_WARNING, "[%s] warm-up prepare failed (try %d/10)\n",
                 TAG, i + 1);
          usleep(100000);  /* 100ms 后重试 */
          continue;
        }

      /* 等待 STARTED 或 error 事件，800ms 超时。
       * 返回0=收到事件（error 或 STARTED），返回-1=超时。 */
      struct timespec ts;
      clock_gettime(CLOCK_REALTIME, &ts);
      ts.tv_nsec += 800 * 1000 * 1000;
      if (ts.tv_nsec >= 1000000000)
        {
          ts.tv_sec++;
          ts.tv_nsec -= 1000000000;
        }
      int swret = sem_timedwait(&s_prompt_sem, &ts);

      if (swret == 0 && !s_warmup_error)
        {
          warmup_ok = true;  /* 收到 STARTED 且无错误 */
          break;
        }

      /* start 失败或超时：stop 后重试 */
      media_player_stop(g_player);
      syslog(LOG_WARNING,
             "[%s] warm-up start failed (try %d/10, err=%d timeout=%d)\n",
             TAG, i + 1, (int)s_warmup_error, swret != 0);
      usleep(100000);  /* 100ms 后重试 */
    }

  if (warmup_ok)
    {
      /* 写入100ms静音PCM数据（全零缓冲区） */
      char *silence_buf = (char *)malloc(WARMUP_SILENCE_BYTES);
      if (silence_buf)
        {
          memset(silence_buf, 0, WARMUP_SILENCE_BYTES);
          media_player_write_data(g_player, silence_buf, WARMUP_SILENCE_BYTES);

          /* 等待静音数据播放完成 */
          usleep(200000);  /* 200ms */

          media_player_stop(g_player);
          syslog(LOG_INFO, "[%s] codec warm-up (silent PCM %d bytes) done\n",
                 TAG, WARMUP_SILENCE_BYTES);
          free(silence_buf);
        }
      else
        {
          media_player_stop(g_player);
          syslog(LOG_WARNING, "[%s] warm-up malloc failed, codec may pop\n", TAG);
        }
    }
  else
    {
      syslog(LOG_WARNING,
             "[%s] warm-up failed after 10 retries, codec may pop\n", TAG);
    }

  s_warmup_mode = false;
  sem_destroy(&s_prompt_sem);
  s_warmup_running = false;
  /* 保持g_player打开，不close */
  return NULL;
}

int wakeup_detector_init(void)
{
  if (s_wk_initialized) return 0;
  s_wk_initialized = true;

  /* 预热音频codec：BUFFER模式写入100ms静音PCM数据，完整跑通
   * codec_hw_open → prepare → start → SDM unmute → write_data → stop 全链路，
   * 使DAC模拟链路(DACLDO/S1PA/EARPA/LPPA/S4PA)充分预热。
   * 不依赖任何音频文件，直接写入零填充PCM数据。
   * 播放完成后保持player打开（不close），防止ASYNC_CLOSE关闭codec。
   * 后续do_play_prompt调用时close+reopen间隔仅100ms，codec保持热状态。
   *
   * 放到后台临时线程执行，不阻塞 LVGL 主线程（开机黑屏根因）。
   * do_play_prompt 调用时会 pthread_join 等待本线程完成。 */
  g_player = media_player_open(MEDIA_STREAM_MUSIC);
  if (g_player)
    {
      s_warmup_running = true;
      pthread_attr_t attr;
      pthread_attr_init(&attr);
      pthread_attr_setstacksize(&attr, 4096);
      pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);
      if (pthread_create(&s_warmup_thread, &attr, warmup_thread_func, NULL) != 0)
        {
          syslog(LOG_ERR, "[%s] failed to create warm-up thread\n", TAG);
          s_warmup_running = false;
        }
      pthread_attr_destroy(&attr);
    }
  else
    {
      syslog(LOG_WARNING, "[%s] codec pre-init failed, pop音 may occur\n", TAG);
    }

  syslog(LOG_INFO, "[%s] wakeup detector initialized\n", TAG);
  return 0;
}

void wakeup_detector_start(void)
{
  /* 防重入：基于 wakeup 资源状态标记，已开启则不允许再开启，只能 stop */
  if (s_wk_running) return;

  int wait_ms = 0;
  while (s_wk_thread_active && wait_ms < 3000)
    {
      usleep(50000);
      wait_ms += 50;
    }

  if (s_wk_thread_active)
    {
      syslog(LOG_WARNING, "[%s] previous thread still active, skip start\n", TAG);
      return;
    }

  s_wk_running = true;
  s_wk_thread_active = true;

  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, WAKEUP_STACK_SIZE);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

  if (pthread_create(&s_wk_thread, &attr, wakeup_thread, NULL) != 0)
    {
      syslog(LOG_ERR, "[%s] failed to create wakeup thread\n", TAG);
      s_wk_running = false;
      s_wk_thread_active = false;
    }

  pthread_attr_destroy(&attr);
  syslog(LOG_INFO, "[%s] wakeup detector started\n", TAG);
}

void wakeup_detector_stop(void)
{
  /* 防重入：基于 wakeup 资源状态标记，已关闭则不允许再关闭，只能 start */
  if (!s_wk_running) return;
  s_wk_running = false;

  sem_post(&s_wakeup_sem);

  int wait_ms = 0;
  while (s_wk_thread_active && wait_ms < 3000)
    {
      usleep(50000);
      wait_ms += 50;
    }

  syslog(LOG_INFO, "[%s] wakeup detector stopped (waited %dms)\n",
         TAG, wait_ms);
}

void wakeup_detector_deinit(void)
{
  wakeup_detector_stop();
  s_wk_initialized = false;
  syslog(LOG_INFO, "[%s] wakeup detector deinitialized\n", TAG);
}

bool wakeup_detector_is_running(void)
{
  return s_wk_running;
}

void wakeup_detector_set_callback(wakeup_callback_t cb)
{
  s_callback = cb;
}

void wakeup_detector_play_prompt(const char *path)
{
  /* Called from VAD thread callback context.
   * Stops recorder, plays prompt, does NOT reopen recorder.
   * The VAD thread will reopen the recorder after the callback returns
   * (if action is CONTINUE). */
  do_play_prompt(path);
}

/* One-shot prompt player thread: used when the VAD thread is not running.
 * do_play_prompt() is synchronous (blocks until playback completes), so we
 * run it in a detached thread to avoid blocking the caller (e.g. the network
 * monitor timer callback). */
static void *prompt_player_thread(void *arg)
{
  char *path = (char *)arg;
  do_play_prompt(path);
  free(path);
  return NULL;
}

void wakeup_detector_request_prompt(const char *path)
{
  /* Called from external thread (e.g. LVGL timer / network monitor).
   * If the VAD thread is running, set the pending flag and signal it to play
   * the prompt.  If the VAD thread is NOT running (e.g. boot-time prompt
   * before ai_page is entered), spawn a detached one-shot thread to play the
   * prompt synchronously without blocking the caller. */
  if (!path) return;

  if (s_wk_running)
    {
      strncpy(s_prompt_request_path, path, sizeof(s_prompt_request_path) - 1);
      s_prompt_request_path[sizeof(s_prompt_request_path) - 1] = '\0';
      s_prompt_request_pending = true;

      /* Signal the VAD thread */
      sem_post(&s_wakeup_sem);
      syslog(LOG_INFO, "[%s] external prompt requested: %s\n", TAG, path);
    }
  else
    {
      /* VAD thread not running: play directly in a detached thread */
      char *path_copy = strdup(path);
      if (!path_copy)
        {
          syslog(LOG_ERR, "[%s] prompt: strdup failed\n", TAG);
          return;
        }

      pthread_attr_t attr;
      pthread_attr_init(&attr);
      pthread_attr_setstacksize(&attr, WAKEUP_STACK_SIZE);
      pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

      pthread_t tid;
      if (pthread_create(&tid, &attr, prompt_player_thread, path_copy) != 0)
        {
          syslog(LOG_ERR, "[%s] prompt: failed to create player thread\n", TAG);
          free(path_copy);
        }
      else
        {
          syslog(LOG_INFO, "[%s] external prompt (direct): %s\n", TAG, path);
        }
      pthread_attr_destroy(&attr);
    }
}
