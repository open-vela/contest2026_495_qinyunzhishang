/*
 * Copyright (C) 2026 Xiaomi Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "voice/voice_channel.h"
#ifdef CONFIG_AI_AGENT_AUDIO_PREPROCESS
#include <compexp.h>
#include <echo_canceller.h>
#include <heap_api.h>
#endif
#include "core/message_bus.h"
#include "infra/config_store.h"
#include "agent_compat.h"
#include "agent_config.h"
#include "voice/audio_capture.h"
#include "voice/audio_playback.h"
#include "voice/voice_asr.h"
#include "voice/voice_tts.h"
#include "voice/volc_asr.h"
#include "voice/volc_tts.h"
#include "voice/dashscope_asr.h"
#include "voice/dashscope_tts.h"
#include "voice/ws_conn_pool.h"
#include "voice/tts_cache.h"
#include "voice/voice_quick_path.h"
#include "voice/voice_perf.h"
#include "channels/ws_server.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

static const char* TAG = "voice";
static int s_backends_registered;

/* ── WiFi power-save hook (refcounted) ─────────────────────
 * During active voice (PTT recording/ASR, TTS pipeline, speak), WiFi PS
 * is turned OFF to avoid AP-side buffering jitter. A refcount allows
 * overlapping sessions (e.g. speak during pipeline) without premature
 * restoration. The weak default is no-op; platforms override to call
 * wapi_set_power_save / bwifi_set_ps_cfg. */
__attribute__((weak)) void voice_wifi_ps_set_hook(int off)
{
    (void)off;
}

static int s_wifi_ps_refs;
static pthread_mutex_t s_wifi_ps_lock = PTHREAD_MUTEX_INITIALIZER;

static void voice_wifi_ps_acquire(void)
{
    pthread_mutex_lock(&s_wifi_ps_lock);
    if (s_wifi_ps_refs++ == 0)
        voice_wifi_ps_set_hook(1); /* 1 = OFF (active voice) */
    pthread_mutex_unlock(&s_wifi_ps_lock);
}

static void voice_wifi_ps_release(void)
{
    pthread_mutex_lock(&s_wifi_ps_lock);
    if (s_wifi_ps_refs > 0 && --s_wifi_ps_refs == 0)
        voice_wifi_ps_set_hook(0); /* 0 = ON (idle, PS restored) */
    pthread_mutex_unlock(&s_wifi_ps_lock);
}

/* ── Audio preprocessing (NS + AGC via BES ec2float) ────── */

#ifdef CONFIG_AI_AGENT_AUDIO_PREPROCESS

#define PREPROC_HEAP_SIZE (150 * 1024)
#define PREPROC_FRAME_MS 15
#define PREPROC_FRAME_SIZE \
    (AGENT_VOICE_SAMPLE_RATE / 1000 * PREPROC_FRAME_MS)

static const Ec2FloatConfig s_ns_cfg = {
    .bypass = 0,
    .hpf_enabled = 1,
    .af_enabled = 0, /* no AEC */
    .adprop_enabled = 0,
    .varistep_enabled = 0,
    .nlp_enabled = 0, /* no NLP */
    .clip_enabled = 0,
    .stsupp_enabled = 0,
    .hfsupp_enabled = 0,
    .constrain_enabled = 0,
    .ns_enabled = 1, /* noise suppression ON */
    .cng_enabled = 0,
    .blocks = 1,
    .delay = 0,
    .gamma = 0.9f,
    .echo_band_start = 300,
    .echo_band_end = 1800,
    .min_ovrd = 2,
    .target_supp = -40,
    .highfre_band_start = 4000,
    .highfre_supp = 8.f,
    .noise_supp = -15,
    .cng_type = 0,
    .cng_level = -60,
    .clip_threshold = -20.f,
    .banks = 64,
};

static const CompexpConfig s_agc_cfg = {
    .bypass = 0,
    .type = 0,
    .comp_threshold = -20.f,
    .comp_ratio = 2.f,
    .expand_threshold = -45.f,
    .expand_ratio = 0.5556f,
    .attack_time = 0.001f,
    .release_time = 0.006f,
    .makeup_gain = 6,
    .delay = 32,
    .tav = 0.2f,
};

typedef struct {
    void* heap;
    Ec2FloatState* ns;
    CompexpState* agc;
} audio_preproc_t;

static audio_preproc_t* preproc_create(void)
{
    audio_preproc_t* pp = calloc(1, sizeof(*pp));
    if (!pp) {
        return NULL;
    }

    pp->heap = malloc(PREPROC_HEAP_SIZE);
    if (!pp->heap) {
        free(pp);
        return NULL;
    }
    med_heap_init(pp->heap, PREPROC_HEAP_SIZE);

    pp->ns = ec2float_create(AGENT_VOICE_SAMPLE_RATE,
        PREPROC_FRAME_SIZE, 0, &s_ns_cfg);
    pp->agc = compexp_create(AGENT_VOICE_SAMPLE_RATE,
        PREPROC_FRAME_SIZE, &s_agc_cfg);

    syslog(LOG_INFO, "[%s] preproc: NS=%p AGC=%p heap=%dKB\n",
        TAG, (void*)pp->ns, (void*)pp->agc,
        PREPROC_HEAP_SIZE / 1024);
    return pp;
}

static void preproc_destroy(audio_preproc_t* pp)
{
    if (!pp) {
        return;
    }
    if (pp->agc) {
        compexp_destroy(pp->agc);
    }
    if (pp->ns) {
        ec2float_destroy(pp->ns);
    }
    free(pp->heap);
    free(pp);
}

static void preproc_chunk(audio_preproc_t* pp,
    unsigned char* buf, int bytes)
{
    if (!pp || (!pp->ns && !pp->agc)) {
        return;
    }

    int16_t* samples = (int16_t*)buf;
    int total = bytes / 2;
    int fsz = PREPROC_FRAME_SIZE;
    int32_t in32[PREPROC_FRAME_SIZE];
    int32_t ref32[PREPROC_FRAME_SIZE];
    int32_t out32[PREPROC_FRAME_SIZE];

    memset(ref32, 0, sizeof(ref32));

    for (int off = 0; off < total; off += fsz) {
        int n = (off + fsz <= total) ? fsz : (total - off);

        for (int i = 0; i < n; i++) {
            in32[i] = samples[off + i];
        }

        if (pp->ns) {
            ec2float_process(pp->ns, in32, ref32, n, out32);
        } else {
            memcpy(out32, in32, n * sizeof(int32_t));
        }

        if (pp->agc) {
            for (int i = 0; i < n; i++) {
                out32[i] <<= 8;
            }
            compexp_process_int24(pp->agc, out32, n);
            for (int i = 0; i < n; i++) {
                out32[i] >>= 8;
            }
        }

        for (int i = 0; i < n; i++) {
            int32_t v = out32[i];
            if (v > 32767) {
                v = 32767;
            } else if (v < -32768) {
                v = -32768;
            }
            samples[off + i] = (int16_t)v;
        }
    }
}

#endif /* CONFIG_AI_AGENT_AUDIO_PREPROCESS */

/* ── Recording state machine (PTT mode) ─────────────────── */

enum voice_state {
    VOICE_IDLE = 0,
    VOICE_RECORDING,
    VOICE_STOPPING
};

static struct {
    enum voice_state state;
    pthread_mutex_t lock;
    audio_capture_t* cap;
    unsigned char* pcm_buf; /* accumulated PCM data */
    size_t pcm_len; /* bytes recorded so far */
    size_t pcm_cap; /* buffer capacity */
    pthread_t rec_thread;
    voice_asr_stream_t* asr_stream; /* streaming ASR handle */
    sem_t rec_ready; /* posted by recording_thread when ready to consume */
    volatile int tts_abort; /* set to 1 to stop active TTS playback */
    audio_playback_t* tts_pb; /* active TTS playback handle (or NULL) */
    pthread_mutex_t speak_lock; /* serialize concurrent speak calls */
    uint32_t speak_count; /* total speak calls for periodic dump */
    volatile int tts_speaking; /* 1 while multi-segment speak loop is running */
} s_voice = {
    .state = VOICE_IDLE,
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .speak_lock = PTHREAD_MUTEX_INITIALIZER,
};

/* ── File-based helpers (kept for QEMU test commands) ──── */

static int read_pcm_file(const char* path,
    unsigned char** out, size_t* out_len)
{
    int fd = open(path, O_RDONLY);

    if (fd < 0) {
        syslog(LOG_ERR, "[%s] Cannot open %s: %d\n",
            TAG, path, errno);
        return -errno;
    }

    off_t fsize = lseek(fd, 0, SEEK_END);

    if (fsize <= 0 || (size_t)fsize > AGENT_VOICE_PCM_BUF_SIZE) {
        syslog(LOG_ERR, "[%s] File too large or empty: %ld\n",
            TAG, (long)fsize);
        close(fd);
        return -EFBIG;
    }

    lseek(fd, 0, SEEK_SET);

    unsigned char* buf = malloc((size_t)fsize);

    if (!buf) {
        close(fd);
        return -ENOMEM;
    }

    ssize_t nread = read(fd, buf, (size_t)fsize);

    close(fd);

    if (nread != (ssize_t)fsize) {
        free(buf);
        return -EIO;
    }

    *out = buf;
    *out_len = (size_t)fsize;
    return 0;
}

static int write_pcm_file(const char* path,
    const unsigned char* data, size_t len)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);

    if (fd < 0) {
        syslog(LOG_ERR, "[%s] Cannot create %s: %d\n",
            TAG, path, errno);
        return -errno;
    }

    ssize_t nw = write(fd, data, len);

    close(fd);
    return (nw == (ssize_t)len) ? 0 : -EIO;
}

/* ── Recording cleanup helper ────────────────────────────── */

static void recording_cleanup_locked(voice_asr_stream_t* stream)
{
    /* Abort streaming ASR if active */
    if (stream) {
        voice_asr_stream_abort(stream);
        s_voice.asr_stream = NULL;
    }

    /* Close capture device */
    if (s_voice.cap) {
        audio_capture_close(s_voice.cap);
        s_voice.cap = NULL;
    }

    /* Free PCM buffer */
    free(s_voice.pcm_buf);
    s_voice.pcm_buf = NULL;
    s_voice.pcm_len = 0;

    /* Reset state to IDLE */
    s_voice.state = VOICE_IDLE;
}

/* Allocate fallback PCM buffer (must hold lock) */
static int alloc_fallback_buffer_locked(void)
{
    if (s_voice.pcm_buf) {
        return 0;
    }

    s_voice.pcm_buf = malloc(s_voice.pcm_cap);
    if (!s_voice.pcm_buf) {
        syslog(LOG_ERR, "[%s] fallback buffer alloc failed\n", TAG);
        return -ENOMEM;
    }

    s_voice.pcm_len = 0;
    return 0;
}

/* Accumulate PCM chunk to fallback buffer (must hold lock) */
static void accumulate_pcm_locked(const unsigned char* chunk, size_t len)
{
    if (s_voice.pcm_buf && s_voice.pcm_len + len <= s_voice.pcm_cap) {
        memcpy(s_voice.pcm_buf + s_voice.pcm_len, chunk, len);
        s_voice.pcm_len += len;
    }
}

/* ── ASR pre-connect pool ────────────────────────────────── */

static voice_asr_stream_t* s_asr_preconnect;
static pthread_mutex_t s_asr_preconnect_lock = PTHREAD_MUTEX_INITIALIZER;

static void asr_preconnect_invalidate(void)
{
    pthread_mutex_lock(&s_asr_preconnect_lock);
    if (s_asr_preconnect) {
        voice_asr_stream_abort(s_asr_preconnect);
        s_asr_preconnect = NULL;
    }
    pthread_mutex_unlock(&s_asr_preconnect_lock);
}

static voice_asr_stream_t* asr_preconnect_take(void)
{
    pthread_mutex_lock(&s_asr_preconnect_lock);
    voice_asr_stream_t* s = s_asr_preconnect;
    s_asr_preconnect = NULL;
    pthread_mutex_unlock(&s_asr_preconnect_lock);
    return s;
}

static void asr_preconnect_store(voice_asr_stream_t* s)
{
    pthread_mutex_lock(&s_asr_preconnect_lock);
    if (s_asr_preconnect) {
        voice_asr_stream_abort(s_asr_preconnect);
    }
    s_asr_preconnect = s;
    pthread_mutex_unlock(&s_asr_preconnect_lock);
}

static void* asr_preconnect_thread(void* arg)
{
    (void)arg;
    syslog(LOG_INFO, "[%s] ASR preconnect: starting\n", TAG);
    voice_asr_stream_t* stream = voice_asr_stream_open();
    if (stream) {
        asr_preconnect_store(stream);
        syslog(LOG_INFO, "[%s] ASR preconnect: ready\n", TAG);
    } else {
        syslog(LOG_WARNING, "[%s] ASR preconnect: failed\n", TAG);
    }
    return NULL;
}

void voice_channel_asr_preconnect(void)
{
    pthread_mutex_lock(&s_asr_preconnect_lock);
    if (s_asr_preconnect) {
        pthread_mutex_unlock(&s_asr_preconnect_lock);
        return;
    }
    pthread_mutex_unlock(&s_asr_preconnect_lock);

    pthread_t tid;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 24 * 1024);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&tid, &attr, asr_preconnect_thread, NULL) != 0) {
        syslog(LOG_WARNING, "[%s] ASR preconnect thread create failed\n", TAG);
    }
    pthread_attr_destroy(&attr);
}

/* ── ASR background connector thread ─────────────────────── */

static void* asr_connector_thread(void* arg)
{
    syslog(LOG_INFO, "[%s] ASR connector: starting background connect\n", TAG);

    voice_asr_stream_t* stream = asr_preconnect_take();
    if (stream) {
        syslog(LOG_INFO, "[%s] ASR connector: using preconnected stream\n", TAG);
    } else {
        stream = voice_asr_stream_open();
    }

    if (stream) {
        pthread_mutex_lock(&s_voice.lock);
        if (s_voice.state == VOICE_RECORDING && !s_voice.asr_stream) {
            s_voice.asr_stream = stream;
            syslog(LOG_INFO, "[%s] ASR connector: stream stored\n", TAG);
        } else {
            voice_asr_stream_abort(stream);
            syslog(LOG_INFO, "[%s] ASR connector: stream discarded (state=%d)\n",
                TAG, s_voice.state);
        }
        pthread_mutex_unlock(&s_voice.lock);
    } else {
        syslog(LOG_WARNING, "[%s] ASR connector: connect failed\n", TAG);
    }
    return NULL;
}

/* ── Recording thread (PTT mode — streaming ASR) ─────────── */

static void* recording_thread(void* arg)
{
    (void)arg;
    unsigned char chunk[AGENT_ASR_CHUNK_SIZE];
    int need_fallback = 1;
    size_t total_bytes_read = 0;
    size_t total_bytes_sent = 0;
    int chunk_count = 0;
    int abnormal_exit = 0;
    voice_asr_stream_t* stream = NULL;

    syslog(LOG_INFO, "[%s] recording thread started (streaming)\n", TAG);

    pthread_mutex_lock(&s_voice.lock);
    if (alloc_fallback_buffer_locked() != 0) {
        s_voice.state = VOICE_IDLE;
        pthread_mutex_unlock(&s_voice.lock);
        sem_post(&s_voice.rec_ready);
        return NULL;
    }
    pthread_mutex_unlock(&s_voice.lock);

    sem_post(&s_voice.rec_ready);

    while (1) {
        pthread_mutex_lock(&s_voice.lock);
        enum voice_state st = s_voice.state;
        if (!stream && s_voice.asr_stream) {
            stream = s_voice.asr_stream;
            s_voice.asr_stream = NULL;
            syslog(LOG_INFO, "[%s] recording: got ASR stream from connector\n", TAG);
        }
        pthread_mutex_unlock(&s_voice.lock);

        if (st != VOICE_RECORDING) {
            break;
        }

        int n = audio_capture_read(s_voice.cap,
            chunk, sizeof(chunk));

        if (n <= 0) {
            syslog(LOG_WARNING,
                "[%s] capture read returned %d, stopping\n", TAG, n);
            abnormal_exit = 1;
            break;
        }

        total_bytes_read += (size_t)n;
        chunk_count++;

        if (chunk_count <= 3 || chunk_count % 50 == 0) {
            int16_t* samples = (int16_t*)chunk;
            int sample_count = n / 2;
            int32_t peak = 0;

            for (int i = 0; i < sample_count; i++) {
                int32_t v = samples[i] < 0 ? -samples[i] : samples[i];
                if (v > peak) {
                    peak = v;
                }
            }
            syslog(LOG_INFO,
                "[%s] chunk#%d: %d bytes, peak=%ld\n",
                TAG, chunk_count, n, (long)peak);
        }

#ifdef CONFIG_AI_AGENT_AUDIO_PREPROCESS
        preproc_chunk(pp, chunk, n);
#endif

        if (need_fallback) {
            pthread_mutex_lock(&s_voice.lock);
            accumulate_pcm_locked(chunk, (size_t)n);
            pthread_mutex_unlock(&s_voice.lock);
        }

        if (stream) {
            if (need_fallback) {
                pthread_mutex_lock(&s_voice.lock);
                if (s_voice.pcm_buf && s_voice.pcm_len > 0) {
                    int ret = voice_asr_stream_send(stream,
                        s_voice.pcm_buf, s_voice.pcm_len);
                    if (ret == 0) {
                        total_bytes_sent += s_voice.pcm_len;
                    }
                }
                free(s_voice.pcm_buf);
                s_voice.pcm_buf = NULL;
                s_voice.pcm_len = 0;
                pthread_mutex_unlock(&s_voice.lock);
                need_fallback = 0;
                syslog(LOG_INFO,
                    "[%s] ASR stream active, flushed buffer\n", TAG);
            }

            int ret = voice_asr_stream_send(stream, chunk, (size_t)n);
            if (ret != 0) {
                syslog(LOG_ERR,
                    "[%s] stream send failed: %d, batch fallback\n", TAG, ret);
                voice_asr_stream_abort(stream);
                stream = NULL;
                need_fallback = 1;

                pthread_mutex_lock(&s_voice.lock);
                alloc_fallback_buffer_locked();
                pthread_mutex_unlock(&s_voice.lock);
            } else {
                total_bytes_sent += (size_t)n;
            }

            if (stream && chunk_count > 5
                && voice_asr_stream_vad_done(stream) == 1) {
                syslog(LOG_INFO,
                    "[%s] VAD: server detected end of speech, auto-stopping\n",
                    TAG);
                pthread_mutex_lock(&s_voice.lock);
                if (s_voice.state == VOICE_RECORDING)
                    s_voice.state = VOICE_STOPPING;
                pthread_mutex_unlock(&s_voice.lock);
                break;
            }
        }
    }

#ifdef CONFIG_AI_AGENT_AUDIO_PREPROCESS
    preproc_destroy(pp);
#endif

    pthread_mutex_lock(&s_voice.lock);
    if (s_voice.asr_stream && s_voice.asr_stream != stream) {
        if (stream) {
            voice_asr_stream_abort(s_voice.asr_stream);
        } else {
            stream = s_voice.asr_stream;
        }
    }
    s_voice.asr_stream = stream;

    if (abnormal_exit && s_voice.state == VOICE_RECORDING) {
        syslog(LOG_WARNING, "[%s] abnormal exit, cleaning up\n", TAG);
        recording_cleanup_locked(stream);
    }
    pthread_mutex_unlock(&s_voice.lock);

    syslog(LOG_INFO,
        "[%s] recording thread exit: %d chunks, %zu read, %zu sent%s\n",
        TAG, chunk_count, total_bytes_read, total_bytes_sent,
        abnormal_exit ? " (abnormal)" : "");
    return NULL;
}

/* ── ASR worker thread ──────────────────────────────────── */

#define ASR_THREAD_STACK (24 * 1024)

static void* asr_and_dispatch(void* arg)
{
    unsigned char* pcm = (unsigned char*)arg;
    size_t pcm_len;

    memcpy(&pcm_len, pcm, sizeof(size_t));
    const unsigned char* audio = pcm + sizeof(size_t);

    syslog(LOG_INFO, "[%s] ASR: recognizing %zu bytes\n",
        TAG, pcm_len);

    {
        extern void llm_pool_preconnect(void);
        llm_pool_preconnect();
    }

    char text[512];
    int ret = voice_asr_recognize(audio, pcm_len,
        text, sizeof(text));

    if (ret == 0 && text[0] != '\0') {
        syslog(LOG_INFO, "[%s] ASR result: %s\n", TAG, text);
        printf("ASR: %s\n", text);

        ws_server_broadcast_typed("asr", text, AGENT_CHAN_VOICE);

        agent_msg_t msg;

        memset(&msg, 0, sizeof(msg));
        strncpy(msg.channel, AGENT_CHAN_VOICE,
            sizeof(msg.channel) - 1);
        strncpy(msg.chat_id, "voice",
            sizeof(msg.chat_id) - 1);
        msg.content = strdup(text);
        if (msg.content) {
            int mret = message_bus_push_inbound(&msg);
            syslog(LOG_INFO, "[%s] batch ASR result pushed to bus: %d\n", TAG, mret);
        }
    } else {
        syslog(LOG_WARNING, "[%s] ASR failed or empty: %d\n",
            TAG, ret);
        agent_msg_t msg;
        memset(&msg, 0, sizeof(msg));
        strncpy(msg.channel, AGENT_CHAN_VOICE,
            sizeof(msg.channel) - 1);
        strncpy(msg.chat_id, "voice",
            sizeof(msg.chat_id) - 1);
        msg.content = strdup("[ASR failed: credentials not configured]");
        if (msg.content) {
            int mret = message_bus_push_inbound(&msg);
            syslog(LOG_INFO, "[%s] ASR error pushed to bus: %d\n", TAG, mret);
        }
    }

    free(pcm);
    return NULL;
}

/* ── TTS streaming callback ──────────────────────────────── */

static struct timespec s_tts_start;
static int s_tts_first_chunk;
static struct timespec s_asr_done_ts;
static volatile int s_quick_path_hit;

/* ── TTS 文件缓存架构（参考 Omni，三层解耦）───────────────
 * TTS 合成线程(WS recv) → emmc 文件 → consumer 线程 → ring buffer → drain
 * 合成线程回调调 tts_pb_enqueue 写文件后立即返回，不被 ring buffer 反压阻塞。
 * consumer 线程读取文件写入 ring buffer，可能被反压但只阻塞自己。
 *
 * 文件级预缓冲：等文件累计 TTS_FILE_PREBUFFER_BYTES (1.5s) 再开始播放，
 * 抗网络首包慢+抖动。emmc 容量无限，不占 RAM ring buffer。
 * 阈值与 Omni 一致（72KB / 3s 超时）。 */
#define TTS_CACHE_FILE   "/emmc/tts_audio.pcm"
#define TTS_READ_CHUNK   8192   /* consumer 每次从文件读取 8KB */
#define TTS_FILE_PREBUFFER_BYTES    (72 * 1024)  /* 1.5s @24kHz/16bit/mono，与 Omni 一致 */
#define TTS_FILE_PREBUFFER_TIMEOUT_MS 3000  /* 3s 超时强制开播，与 Omni 一致 */

static int s_tts_file_fd = -1;
static off_t s_tts_file_write_pos = 0;  /* 合成线程写入位置 */
static off_t s_tts_file_read_pos = 0;   /* consumer 线程读取位置 */
static pthread_mutex_t s_tts_file_mtx = PTHREAD_MUTEX_INITIALIZER;
static sem_t s_tts_file_sem;            /* 通知 consumer 有新数据 */
static volatile bool s_tts_file_eof = false;  /* 合成完成标记 */
static volatile bool s_tts_consumer_running = false;
static pthread_t s_tts_consumer_tid;
static audio_playback_t *s_tts_pb = NULL;  /* consumer 线程访问的 playback 句柄 */
static pthread_mutex_t s_tts_pb_lock = PTHREAD_MUTEX_INITIALIZER;

/* ── TTS 文件缓存接口实现（参考 omni_pb_*）──────────────── */

/* Consumer 线程：从文件读取 PCM 写入 audio_playback。
 * 此线程可能被 ring buffer 反压，但不再阻塞 TTS 合成线程。
 * 退出条件：consumer_running=false 且文件已读完（read_pos>=write_pos），
 * 确保不丢数据。
 *
 * 文件级预缓冲：启动后先等文件累计达到 TTS_FILE_PREBUFFER_BYTES
 * （0.67s 数据）再开始读取播放，抗网络首包慢+抖动。超时 2s 强制开播。 */
static void* tts_pb_consumer_thread(void *arg)
{
    (void)arg;
    unsigned char *read_buf = malloc(TTS_READ_CHUNK);
    if (!read_buf) {
        syslog(LOG_ERR, "[%s] tts consumer malloc read_buf failed\n", TAG);
        return NULL;
    }

    bool file_prebuffer_done = false;
    long long prebuffer_start_ms = 0;

    while (1) {
        pthread_mutex_lock(&s_tts_file_mtx);
        off_t avail = s_tts_file_write_pos - s_tts_file_read_pos;
        off_t file_total = s_tts_file_write_pos;  /* 文件累计写入量 */
        bool eof = s_tts_file_eof;
        pthread_mutex_unlock(&s_tts_file_mtx);

        /* ═══ 文件级预缓冲阶段：等文件攒够 0.67s 数据再开始读取 ═══ */
        if (!file_prebuffer_done) {
            if (file_total >= TTS_FILE_PREBUFFER_BYTES || eof) {
                file_prebuffer_done = true;
                syslog(LOG_INFO, "[%s] TTS file prebuffer done (%ld bytes)\n",
                       TAG, (long)file_total);
            } else {
                /* 检查超时 */
                struct timeval tv;
                gettimeofday(&tv, NULL);
                long long now_ms = (long long)tv.tv_sec * 1000
                    + tv.tv_usec / 1000;
                if (prebuffer_start_ms == 0) {
                    prebuffer_start_ms = now_ms;
                } else if (now_ms - prebuffer_start_ms
                           >= TTS_FILE_PREBUFFER_TIMEOUT_MS) {
                    file_prebuffer_done = true;  /* 超时强制开播 */
                    syslog(LOG_INFO, "[%s] TTS file prebuffer timeout (%ld bytes)\n",
                           TAG, (long)file_total);
                }
                /* 等待新数据 */
                sem_wait(&s_tts_file_sem);
                continue;
            }
        }

        if (avail <= 0) {
            /* 无数据可读 */
            if (!s_tts_consumer_running && eof) {
                /* 合成完成且文件已读完，退出 */
                break;
            }
            /* 等待新数据 */
            sem_wait(&s_tts_file_sem);
            continue;
        }

        size_t to_read = (avail < (off_t)TTS_READ_CHUNK) ? (size_t)avail : TTS_READ_CHUNK;

        /* 从文件读取 */
        if (s_tts_file_fd >= 0) {
            lseek(s_tts_file_fd, s_tts_file_read_pos, SEEK_SET);
            ssize_t n = read(s_tts_file_fd, read_buf, to_read);
            if (n > 0) {
                s_tts_file_read_pos += n;
                /* 写入 audio_playback ring（可能反压，只阻塞 consumer） */
                pthread_mutex_lock(&s_tts_pb_lock);
                if (s_tts_pb) {
                    int wret = audio_playback_write(s_tts_pb, read_buf, (size_t)n);
                    if (wret < 0) {
                        syslog(LOG_ERR, "[%s] TTS playback write failed: %d\n", TAG, wret);
                    }
                }
                pthread_mutex_unlock(&s_tts_pb_lock);
            }
        }
    }

    free(read_buf);
    return NULL;
}

/* 打开 TTS 文件缓存播放：清空文件 → 打开 playback → 启动 consumer 线程。
 * 返回 playback 句柄（用于 s_voice.tts_pb 状态跟踪）。 */
static audio_playback_t* tts_pb_open(void)
{
    /* 若旧 consumer 仍在运行，先停止并等待其退出 */
    if (s_tts_consumer_running) {
        s_tts_consumer_running = false;
        s_tts_file_eof = true;
        sem_post(&s_tts_file_sem);
        pthread_join(s_tts_consumer_tid, NULL);
        sem_destroy(&s_tts_file_sem);
    }

    /* 打开/清空临时缓存文件 */
    if (s_tts_file_fd >= 0) {
        close(s_tts_file_fd);
    }
    s_tts_file_fd = open(TTS_CACHE_FILE, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (s_tts_file_fd < 0) {
        syslog(LOG_ERR, "[%s] Failed to open TTS cache file %s: %d\n",
               TAG, TTS_CACHE_FILE, errno);
    }
    s_tts_file_write_pos = 0;
    s_tts_file_read_pos = 0;
    s_tts_file_eof = false;

    pthread_mutex_lock(&s_tts_pb_lock);
    if (s_tts_pb) {
        audio_playback_close(s_tts_pb);
        s_tts_pb = NULL;
    }

    audio_playback_t *pb = audio_playback_open(AGENT_AUDIO_PLAYBACK_DEV,
                                                AGENT_TTS_WS_SAMPLE_RATE,
                                                AGENT_VOICE_CHANNELS,
                                                AGENT_VOICE_BITS);
    if (!pb) {
        syslog(LOG_ERR, "[%s] Failed to open TTS audio playback\n", TAG);
        pthread_mutex_unlock(&s_tts_pb_lock);
        return NULL;
    }
    s_tts_pb = pb;
    syslog(LOG_INFO, "[%s] TTS audio playback opened (24kHz, file cache)\n", TAG);
    /* 启动 consumer 线程 */
    sem_init(&s_tts_file_sem, 0, 0);
    s_tts_consumer_running = true;
    pthread_create(&s_tts_consumer_tid, NULL,
                   tts_pb_consumer_thread, NULL);
    pthread_mutex_unlock(&s_tts_pb_lock);

    return pb;
}

/* 合成线程调用：将 PCM 追加写入缓存文件，不阻塞。
 * TTS 回调（tts_stream_cb / tts_cache_aware_cb）调用此函数。 */
static void tts_pb_enqueue(const unsigned char *pcm, size_t len)
{
    if (!pcm || len == 0 || s_tts_file_fd < 0) {
        return;
    }

    /* 追加写入文件（循环写完，防止部分写） */
    size_t written = 0;
    while (written < len) {
        pthread_mutex_lock(&s_tts_file_mtx);
        lseek(s_tts_file_fd, s_tts_file_write_pos, SEEK_SET);
        ssize_t n = write(s_tts_file_fd, pcm + written, len - written);
        if (n <= 0) {
            pthread_mutex_unlock(&s_tts_file_mtx);
            syslog(LOG_ERR, "[%s] TTS cache file write failed: %d (written=%zu/%zu)\n",
                   TAG, errno, written, len);
            break;
        }
        s_tts_file_write_pos += n;
        pthread_mutex_unlock(&s_tts_file_mtx);
        written += (size_t)n;
    }

    /* 通知 consumer 有新数据 */
    sem_post(&s_tts_file_sem);
}

/* 关闭 TTS 文件缓存播放：标记 eof → 等 consumer 读完文件 → drain playback。
 * 阻塞调用直到音频播放完毕（同 omni_pb_close）。 */
static void tts_pb_close(void)
{
    /* 标记合成完成，让 consumer 读完文件剩余数据后退出 */
    s_tts_file_eof = true;
    sem_post(&s_tts_file_sem);

    /* 等 consumer 读完所有数据并退出（不丢数据） */
    if (s_tts_consumer_running) {
        s_tts_consumer_running = false;
        sem_post(&s_tts_file_sem);
        pthread_join(s_tts_consumer_tid, NULL);
        sem_destroy(&s_tts_file_sem);
    }

    /* consumer 已退出，安全关闭 playback（会 drain ring buffer） */
    pthread_mutex_lock(&s_tts_pb_lock);
    if (s_tts_pb) {
        audio_playback_close(s_tts_pb);
        s_tts_pb = NULL;
    }
    pthread_mutex_unlock(&s_tts_pb_lock);

    /* 关闭并删除临时文件 */
    if (s_tts_file_fd >= 0) {
        close(s_tts_file_fd);
        s_tts_file_fd = -1;
        unlink(TTS_CACHE_FILE);
    }
}

/* 立即停止 TTS 播放（用于 abort）：标记 eof + 停止 playback。
 * 不设置 consumer_running=false，consumer 线程的 join 由 tts_pb_close 统一负责，
 * 避免重复管理线程生命周期导致竞争。
 * 调用后 voice_channel_speak 会继续执行 tts_pb_close 完成清理。 */
static void tts_pb_stop(void)
{
    /* 标记 eof，consumer 不再等待新数据 */
    s_tts_file_eof = true;
    sem_post(&s_tts_file_sem);  /* 唤醒可能阻塞在 sem_wait 的 consumer */

    /* 立即停止 playback（不等 drain），consumer 的 write 会返回 -ECANCELED */
    pthread_mutex_lock(&s_tts_pb_lock);
    if (s_tts_pb) {
        audio_playback_stop(s_tts_pb);
    }
    pthread_mutex_unlock(&s_tts_pb_lock);
}

typedef struct {
    audio_playback_t* pb;
    unsigned char* cache_buf;
    size_t cache_len;
    size_t cache_cap;
    const char* cache_text;
} tts_cache_cb_ctx_t;

static void tts_stream_cb(const unsigned char* pcm_data,
    size_t pcm_len, int is_last, void* user_data)
{
    (void)user_data;  /* 文件缓存架构下不再使用 pb，统一写 emmc 文件 */

    if (s_voice.tts_abort) {
        syslog(LOG_INFO, "[%s] TTS cb aborted\n", TAG);
        return;
    }

    if (pcm_data && pcm_len > 0) {
        if (!s_tts_first_chunk) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            long ms = (now.tv_sec - s_tts_start.tv_sec) * 1000
                + (now.tv_nsec - s_tts_start.tv_nsec) / 1000000;
            syslog(LOG_INFO, "[%s] TTS first chunk: %ldms, %zu bytes\n",
                TAG, ms, pcm_len);
            s_tts_first_chunk = 1;
        }
        /* 写入 emmc 缓存文件，不阻塞合成线程 */
        tts_pb_enqueue(pcm_data, pcm_len);
    } else if (is_last) {
        syslog(LOG_INFO, "[%s] TTS last chunk signal\n", TAG);
    }

    (void)is_last;
}

static void tts_cache_aware_cb(const unsigned char* pcm_data,
    size_t pcm_len, int is_last, void* user_data)
{
    tts_cache_cb_ctx_t* ctx = (tts_cache_cb_ctx_t*)user_data;

    tts_stream_cb(pcm_data, pcm_len, is_last, ctx->pb);

    if (pcm_data && pcm_len > 0 && ctx->cache_buf) {
        if (ctx->cache_len + pcm_len <= ctx->cache_cap) {
            memcpy(ctx->cache_buf + ctx->cache_len, pcm_data, pcm_len);
            ctx->cache_len += pcm_len;
        } else {
            syslog(LOG_WARNING, "[%s] TTS cache buffer full (%zu/%zu), audio truncated\n",
                TAG, ctx->cache_len, ctx->cache_cap);
        }
    }

    if (is_last && ctx->cache_buf && ctx->cache_len > 0
        && ctx->cache_text) {
        tts_cache_store(ctx->cache_text, ctx->cache_buf, ctx->cache_len);
    }
}

/* ── TTS Pipeline: sentence-level streaming ──────────────── */

static void tts_strip_markdown(char* s);

#define PIPELINE_QUEUE_SIZE 8
#define PIPELINE_SENTENCE_MAX 512

typedef struct {
    char text[PIPELINE_SENTENCE_MAX];
    int is_final;
} pipeline_item_t;

static struct {
    pipeline_item_t queue[PIPELINE_QUEUE_SIZE];
    int head;
    int tail;
    int count;
    pthread_mutex_t lock;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
    pthread_t thread;
    audio_playback_t* pb;
    int running;
    int active;
    int abort_flag;
    struct timespec start_ts;
    int first_chunk_received;
    int sentence_count;
} s_pipeline = {
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .not_empty = PTHREAD_COND_INITIALIZER,
    .not_full = PTHREAD_COND_INITIALIZER,
};

static void pipeline_tts_cb(const unsigned char* pcm_data,
    size_t pcm_len, int is_last, void* user_data)
{
    (void)user_data;

    if (s_pipeline.abort_flag)
        return;

    if (pcm_data && pcm_len > 0 && s_pipeline.pb) {
        if (!s_pipeline.first_chunk_received) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            long ms = (now.tv_sec - s_pipeline.start_ts.tv_sec) * 1000
                + (now.tv_nsec - s_pipeline.start_ts.tv_nsec) / 1000000;
            syslog(LOG_INFO, "[%s] pipeline TTS first chunk: %ldms\n",
                TAG, ms);
            s_pipeline.first_chunk_received = 1;
        }
        audio_playback_write(s_pipeline.pb, pcm_data, pcm_len);
    }
}

static void pipeline_tts_cache_cb(const unsigned char* pcm_data,
    size_t pcm_len, int is_last, void* user_data)
{
    tts_cache_cb_ctx_t* ctx = (tts_cache_cb_ctx_t*)user_data;

    pipeline_tts_cb(pcm_data, pcm_len, is_last, NULL);

    if (pcm_data && pcm_len > 0 && ctx->cache_buf) {
        if (ctx->cache_len + pcm_len <= ctx->cache_cap) {
            memcpy(ctx->cache_buf + ctx->cache_len, pcm_data, pcm_len);
            ctx->cache_len += pcm_len;
        }
    }

    if (is_last && ctx->cache_buf && ctx->cache_len > 0
        && ctx->cache_text) {
        tts_cache_store(ctx->cache_text, ctx->cache_buf, ctx->cache_len);
    }
}

static void pipeline_process_sentence(const char* text)
{
    char clean[PIPELINE_SENTENCE_MAX];
    strncpy(clean, text, sizeof(clean) - 1);
    clean[sizeof(clean) - 1] = '\0';
    tts_strip_markdown(clean);

    const char* p = clean;
    while (*p == '\n' || *p == '\r' || *p == ' ' || *p == '\t')
        p++;
    if (*p == '\0')
        return;
    if (p != clean)
        memmove(clean, p, strlen(p) + 1);

    int cache_hit = 0;
    int ret = tts_cache_lookup_stream(clean, pipeline_tts_cb, NULL);
    if (ret == 0) {
        cache_hit = 1;
        syslog(LOG_INFO, "[%s] pipeline TTS cache HIT: \"%s\"\n", TAG, clean);
    } else {
        unsigned char* cache_buf = malloc(TTS_CACHE_MAX_PCM_LEN);
        tts_cache_cb_ctx_t cache_ctx = {
            .pb = NULL,
            .cache_buf = cache_buf,
            .cache_len = 0,
            .cache_cap = cache_buf ? TTS_CACHE_MAX_PCM_LEN : 0,
            .cache_text = clean,
        };

        ret = voice_tts_speak_stream(clean,
            pipeline_tts_cache_cb, &cache_ctx);

        if (cache_buf && cache_ctx.cache_len > 0 && cache_ctx.cache_text[0])
            tts_cache_store(cache_ctx.cache_text, cache_buf, cache_ctx.cache_len);
        free(cache_buf);
    }

    if (ret != 0)
        syslog(LOG_WARNING, "[%s] pipeline TTS failed for \"%s\": %d\n",
            TAG, clean, ret);

    (void)cache_hit;
}

static void* pipeline_thread(void* arg)
{
    (void)arg;

    syslog(LOG_INFO, "[%s] pipeline thread started\n", TAG);

    while (s_pipeline.running) {
        pthread_mutex_lock(&s_pipeline.lock);

        while (s_pipeline.count == 0 && s_pipeline.running)
            pthread_cond_wait(&s_pipeline.not_empty, &s_pipeline.lock);

        if (!s_pipeline.running && s_pipeline.count == 0) {
            pthread_mutex_unlock(&s_pipeline.lock);
            break;
        }

        pipeline_item_t item = s_pipeline.queue[s_pipeline.head];
        s_pipeline.head = (s_pipeline.head + 1) % PIPELINE_QUEUE_SIZE;
        s_pipeline.count--;
        pthread_cond_signal(&s_pipeline.not_full);
        pthread_mutex_unlock(&s_pipeline.lock);

        if (s_pipeline.abort_flag) {
            syslog(LOG_INFO, "[%s] pipeline: aborted, skipping remaining\n",
                TAG);
            break;
        }

        if (item.is_final) {
            syslog(LOG_INFO, "[%s] pipeline: final signal, %d sentences\n",
                TAG, s_pipeline.sentence_count);
            break;
        }

        if (item.text[0] == '\0')
            continue;

        s_pipeline.sentence_count++;
        pipeline_process_sentence(item.text);
    }

    syslog(LOG_INFO, "[%s] pipeline thread done (%d sentences)\n",
        TAG, s_pipeline.sentence_count);
    return NULL;
}

int voice_pipeline_start(void)
{
    pthread_mutex_lock(&s_pipeline.lock);

    if (s_pipeline.active) {
        pthread_mutex_unlock(&s_pipeline.lock);
        return -EBUSY;
    }

    while (s_pipeline.count > 0) {
        s_pipeline.head = 0;
        s_pipeline.tail = 0;
        s_pipeline.count = 0;
    }

    s_pipeline.pb = audio_playback_open(
        AGENT_AUDIO_PLAYBACK_DEV,
        AGENT_TTS_WS_SAMPLE_RATE,
        AGENT_VOICE_CHANNELS,
        AGENT_VOICE_BITS);

    if (!s_pipeline.pb) {
        pthread_mutex_unlock(&s_pipeline.lock);
        syslog(LOG_ERR, "[%s] pipeline: playback open failed\n", TAG);
        return -EIO;
    }

    s_pipeline.abort_flag = 0;
    s_pipeline.first_chunk_received = 0;
    s_pipeline.sentence_count = 0;
    s_pipeline.running = 1;
    clock_gettime(CLOCK_MONOTONIC, &s_pipeline.start_ts);

    if (!ws_conn_pool_tts_is_connected()) {
        pthread_t pre_tid;
        pthread_attr_t pre_attr;
        pthread_attr_init(&pre_attr);
        pthread_attr_setstacksize(&pre_attr, 24 * 1024);
        pthread_attr_setdetachstate(&pre_attr, PTHREAD_CREATE_DETACHED);
        if (pthread_create(&pre_tid, &pre_attr,
                (void* (*)(void*))ws_conn_pool_preconnect_tts, NULL) != 0) {
            syslog(LOG_WARNING,
                "[%s] pipeline: TTS preconnect thread failed\n", TAG);
        }
        pthread_attr_destroy(&pre_attr);
    }

    pthread_mutex_unlock(&s_pipeline.lock);

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 24 * 1024);
    int ret = pthread_create(&s_pipeline.thread, &attr, pipeline_thread, NULL);
    pthread_attr_destroy(&attr);

    if (ret != 0) {
        audio_playback_close(s_pipeline.pb);
        s_pipeline.pb = NULL;
        s_pipeline.running = 0;
        syslog(LOG_ERR, "[%s] pipeline: thread create failed: %d\n", TAG, ret);
        return -ret;
    }

    s_pipeline.active = 1;
    /* WiFi PS OFF during streaming TTS pipeline (network-heavy downlink) */
    voice_wifi_ps_acquire();
    syslog(LOG_INFO, "[%s] pipeline: started\n", TAG);
    return 0;
}

int voice_pipeline_push(const char* sentence)
{
    if (!sentence || sentence[0] == '\0')
        return -EINVAL;

    pthread_mutex_lock(&s_pipeline.lock);

    if (!s_pipeline.active) {
        pthread_mutex_unlock(&s_pipeline.lock);
        return -ENOENT;
    }

    while (s_pipeline.count >= PIPELINE_QUEUE_SIZE) {
        pthread_cond_wait(&s_pipeline.not_full, &s_pipeline.lock);
    }

    pipeline_item_t* item = &s_pipeline.queue[s_pipeline.tail];
    strncpy(item->text, sentence, sizeof(item->text) - 1);
    item->text[sizeof(item->text) - 1] = '\0';
    item->is_final = 0;
    s_pipeline.tail = (s_pipeline.tail + 1) % PIPELINE_QUEUE_SIZE;
    s_pipeline.count++;

    pthread_cond_signal(&s_pipeline.not_empty);
    pthread_mutex_unlock(&s_pipeline.lock);
    return 0;
}

int voice_pipeline_finish(void)
{
    pthread_mutex_lock(&s_pipeline.lock);

    if (!s_pipeline.active) {
        pthread_mutex_unlock(&s_pipeline.lock);
        return -ENOENT;
    }

    while (s_pipeline.count >= PIPELINE_QUEUE_SIZE) {
        pthread_cond_wait(&s_pipeline.not_full, &s_pipeline.lock);
    }

    pipeline_item_t* item = &s_pipeline.queue[s_pipeline.tail];
    item->text[0] = '\0';
    item->is_final = 1;
    s_pipeline.tail = (s_pipeline.tail + 1) % PIPELINE_QUEUE_SIZE;
    s_pipeline.count++;

    pthread_cond_signal(&s_pipeline.not_empty);
    pthread_mutex_unlock(&s_pipeline.lock);

    pthread_join(s_pipeline.thread, NULL);

    pthread_mutex_lock(&s_pipeline.lock);
    if (s_pipeline.pb) {
        audio_playback_close(s_pipeline.pb);
        s_pipeline.pb = NULL;
    }
    s_pipeline.active = 0;
    s_pipeline.running = 0;
    s_pipeline.head = 0;
    s_pipeline.tail = 0;
    s_pipeline.count = 0;
    pthread_mutex_unlock(&s_pipeline.lock);

    /* Release WiFi PS OFF acquired in voice_pipeline_start */
    voice_wifi_ps_release();

    syslog(LOG_INFO, "[%s] pipeline: finished\n", TAG);

    voice_channel_asr_preconnect();

    return 0;
}

void voice_pipeline_abort(void)
{
    pthread_mutex_lock(&s_pipeline.lock);
    if (!s_pipeline.active) {
        pthread_mutex_unlock(&s_pipeline.lock);
        return;
    }
    s_pipeline.abort_flag = 1;
    s_pipeline.running = 0;
    if (s_pipeline.pb)
        audio_playback_stop(s_pipeline.pb);
    pthread_cond_signal(&s_pipeline.not_empty);
    pthread_mutex_unlock(&s_pipeline.lock);

    pthread_join(s_pipeline.thread, NULL);

    pthread_mutex_lock(&s_pipeline.lock);
    if (s_pipeline.pb) {
        audio_playback_close(s_pipeline.pb);
        s_pipeline.pb = NULL;
    }
    s_pipeline.active = 0;
    s_pipeline.head = 0;
    s_pipeline.tail = 0;
    s_pipeline.count = 0;
    pthread_mutex_unlock(&s_pipeline.lock);

    /* Release WiFi PS OFF acquired in voice_pipeline_start */
    voice_wifi_ps_release();

    syslog(LOG_INFO, "[%s] pipeline: aborted\n", TAG);

    voice_channel_asr_preconnect();
}

int voice_pipeline_is_active(void)
{
    pthread_mutex_lock(&s_pipeline.lock);
    int active = s_pipeline.active;
    pthread_mutex_unlock(&s_pipeline.lock);
    return active;
}

/* Abort active TTS playback: set tts_abort flag, stop audio playback,
 * and abort the pipeline.  Called by voice_assistant_stop() to interrupt
 * any TTS blocked in mbedtls_ssl_read() before joining the dashscope thread. */
void voice_channel_abort_tts(void)
{
    pthread_mutex_lock(&s_voice.lock);
    if (s_voice.tts_pb || s_voice.tts_speaking) {
        syslog(LOG_INFO, "[%s] abort_tts: stopping active TTS\n", TAG);
        s_voice.tts_abort = 1;
        s_voice.tts_speaking = 0;
        if (s_voice.tts_pb)
            tts_pb_stop();  /* 停止 consumer + playback（文件缓存架构） */
    } else {
        /* Even with no active TTS, set abort flag to prevent a race
         * with voice_channel_speak() about to start. */
        s_voice.tts_abort = 1;
    }
    pthread_mutex_unlock(&s_voice.lock);

    /* Abort pipeline outside s_voice.lock to avoid blocking the speak
     * thread from calling audio_playback_close() during cleanup. */
    if (voice_pipeline_is_active())
        voice_pipeline_abort();

    syslog(LOG_INFO, "[%s] abort_tts: done\n", TAG);
}

/* Check if any TTS playback is active (either s_voice.tts_pb or pipeline).
 * Thread-safe, intended for echo suppression by the cloud mic sender. */
int voice_channel_is_speaking(void)
{
    audio_playback_t* pb = NULL;
    pthread_mutex_lock(&s_voice.lock);
    pb = s_voice.tts_pb;
    pthread_mutex_unlock(&s_voice.lock);

    /* 多段TTS播放循环还在进行中（合成线程在写文件或 consumer 在读文件），
     * 视为仍在播放。 */
    if (s_voice.tts_speaking)
        return 1;

    if (pb) {
        /* 文件缓存架构：consumer 线程可能还在读取文件写入 ring buffer，
         * 或 ring buffer 中还有未播放数据。检查两者。 */
        pthread_mutex_lock(&s_tts_file_mtx);
        off_t file_avail = s_tts_file_write_pos - s_tts_file_read_pos;
        pthread_mutex_unlock(&s_tts_file_mtx);
        if (file_avail > 1024 || s_tts_consumer_running)
            return 1;
        /* ring buffer 中还有未播放数据 */
        if (audio_playback_pending_bytes(pb) > 1024)
            return 1;
    }

    return voice_pipeline_is_active();
}

/* ── Public API ──────────────────────────────────────────── */

int voice_channel_init(void)
{
    syslog(LOG_INFO, "[%s] Voice channel initializing\n", TAG);

    /* Reset state on init — static vars may persist across process restarts
     * in NuttX depending on how the binary is loaded. */
    s_voice.state = VOICE_IDLE;
    s_voice.cap = NULL;
    s_voice.pcm_buf = NULL;
    s_voice.pcm_len = 0;
    s_voice.asr_stream = NULL;
    s_voice.tts_abort = 0;
    s_voice.tts_pb = NULL;
    sem_init(&s_voice.rec_ready, 0, 0);

    if (!s_backends_registered) {
        dashscope_tts_register();
        dashscope_asr_register();
        volc_tts_register();
        volc_asr_register();
        s_backends_registered = 1;
    }

    ws_conn_pool_init();
    tts_cache_init();
    voice_quick_path_init();
    voice_perf_init();

    {
        static const voice_quick_rule_t default_rules[] = {
            { "几点了",       "让我查一下时间。" },
            { "什么时间",     "让我查一下时间。" },
            { "现在几点",     "让我查一下时间。" },
            { "你好",         "你好！有什么可以帮你的吗？" },
            { "你是谁",       "我是你的AI助手。" },
            { "你叫什么",     "我是你的AI助手。" },
            { "谢谢",         "不客气！" },
            { "感谢",         "不客气！" },
            { "再见",         "再见！有需要随时叫我。" },
            { "拜拜",         "再见！有需要随时叫我。" },
            { "今天天气",     "我暂时无法查询天气，但你可以问我其他问题。" },
            { "讲个笑话",     "好的！为什么程序员喜欢暗色模式？因为光会吸引bug！" },
            { "讲笑话",       "好的！为什么程序员喜欢暗色模式？因为光会吸引bug！" },
            { NULL,           NULL }
        };
        for (int i = 0; default_rules[i].pattern; i++)
            voice_quick_path_register(&default_rules[i]);
    }

    voice_channel_asr_preconnect();

    /* 根据 UI 语言配置(/emmc/lang_config)设置 TTS 音色。
     * i18n_init 在 lvgldemo 进程中先执行，但此时 ai_agent 尚未启动，
     * ws_pool/tts_cache 未初始化，不能调用 dashscope_tts_set_voice。
     * 此处 ai_agent 初始化完成后，统一应用音色。
     * 运行期切换语言时由 i18n_set_lang 直接调用 dashscope_tts_set_voice。 */
    {
        int lang_fd = open("/emmc/lang_config", O_RDONLY);
        if (lang_fd >= 0) {
            char lang_buf[8] = {0};
            ssize_t rn = read(lang_fd, lang_buf, sizeof(lang_buf) - 1);
            close(lang_fd);
            if (rn > 0 && strncmp(lang_buf, "en", 2) == 0)
                dashscope_tts_set_voice(AGENT_DASHSCOPE_TTS_VOICE_EN);
            else
                dashscope_tts_set_voice(AGENT_DASHSCOPE_TTS_VOICE);
        } else {
            /* 文件不存在，默认中文 */
            dashscope_tts_set_voice(AGENT_DASHSCOPE_TTS_VOICE);
        }
    }

    return 0;
}

int voice_channel_start(void)
{
    pthread_mutex_lock(&s_voice.lock);
    if (s_voice.state != VOICE_IDLE) {
        pthread_mutex_unlock(&s_voice.lock);
        syslog(LOG_WARNING, "[%s] already recording\n", TAG);
        return -EBUSY;
    }

    /* Drain stale sem posts from previous sessions to prevent
     * sem_wait returning immediately before thread is ready. */
    while (sem_trywait(&s_voice.rec_ready) == 0)
        ;

    /* ── AEC workaround: stop active TTS to prevent echo ── */
    if (s_voice.tts_pb || s_voice.tts_speaking || voice_pipeline_is_active()) {
        syslog(LOG_INFO,
            "[%s] PTT: stopping active TTS to prevent echo\n", TAG);
        s_voice.tts_abort = 1;
        s_voice.tts_speaking = 0;
        if (s_voice.tts_pb)
            tts_pb_stop();  /* 停止 consumer + playback（文件缓存架构） */
        if (voice_pipeline_is_active())
            voice_pipeline_abort();

        /* Release lock so the speak thread can finish closing the
         * playback device.  Without this, we hold the lock through
         * the entire ASR pre-connect (300-800ms), blocking the speak
         * thread from calling audio_playback_close().  The playback
         * device stays open while we open the capture device, and on
         * BES hardware the shared codec (ci2s) gets confused, causing
         * incomplete or corrupted recording. */
        pthread_mutex_unlock(&s_voice.lock);

        /* Wait for the speak thread to finish — speak_lock is held
         * for the entire duration of voice_channel_speak(). */
        pthread_mutex_lock(&s_voice.speak_lock);
        pthread_mutex_unlock(&s_voice.speak_lock);

        syslog(LOG_INFO,
            "[%s] PTT: TTS playback fully closed\n", TAG);

        /* Re-acquire state lock and re-check — another thread may
         * have started recording while we released the lock. */
        pthread_mutex_lock(&s_voice.lock);
        if (s_voice.state != VOICE_IDLE) {
            pthread_mutex_unlock(&s_voice.lock);
            syslog(LOG_WARNING, "[%s] state changed during TTS stop\n", TAG);
            return -EBUSY;
        }
    }

    /* PCM buffer is lazy-allocated only when streaming ASR fails
     * (batch fallback). This saves 128KB RAM on the happy path. */
    s_voice.pcm_cap = AGENT_VOICE_PCM_BUF_SIZE;
    s_voice.pcm_buf = NULL;
    s_voice.pcm_len = 0;
    s_voice.asr_stream = NULL;

    /* Open capture device (but do NOT start yet — start after the
     * recording thread is spawned so the consumer is ready before
     * audio frames begin flowing, preventing media_recorder queue
     * overflow "data queue is more than max count(4)"). */
    s_voice.cap = audio_capture_open(
        AGENT_AUDIO_CAPTURE_DEV,
        AGENT_VOICE_SAMPLE_RATE,
        AGENT_VOICE_CHANNELS,
        AGENT_VOICE_BITS);

    if (!s_voice.cap) {
        pthread_mutex_unlock(&s_voice.lock);
        syslog(LOG_ERR, "[%s] capture open failed\n", TAG);
        return -EIO;
    }

    s_voice.state = VOICE_RECORDING;
    pthread_mutex_unlock(&s_voice.lock);

    /* WiFi PS OFF during PTT recording + ASR (network-heavy) */
    voice_wifi_ps_acquire();

    /* Spawn recording thread BEFORE starting capture */
    pthread_attr_t attr;

    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, AGENT_VOICE_STACK);

    int ret = pthread_create(&s_voice.rec_thread, &attr,
        recording_thread, NULL);

    pthread_attr_destroy(&attr);

    if (ret != 0) {
        pthread_mutex_lock(&s_voice.lock);
        s_voice.state = VOICE_IDLE;
        audio_capture_close(s_voice.cap);
        s_voice.cap = NULL;
        if (s_voice.asr_stream) {
            voice_asr_stream_abort(s_voice.asr_stream);
            s_voice.asr_stream = NULL;
        }
        free(s_voice.pcm_buf);
        s_voice.pcm_buf = NULL;
        pthread_mutex_unlock(&s_voice.lock);
        voice_wifi_ps_release();
        return -ret;
    }

    /* Now start capture — wait for recording thread to be ready first */
    sem_wait(&s_voice.rec_ready);

    if (audio_capture_start(s_voice.cap) < 0) {
        pthread_mutex_lock(&s_voice.lock);
        s_voice.state = VOICE_IDLE;
        if (s_voice.asr_stream) {
            voice_asr_stream_abort(s_voice.asr_stream);
            s_voice.asr_stream = NULL;
        }
        pthread_mutex_unlock(&s_voice.lock);
        pthread_join(s_voice.rec_thread, NULL);
        audio_capture_close(s_voice.cap);
        s_voice.cap = NULL;
        voice_wifi_ps_release();
        return -EIO;
    }

    /* Spawn background ASR connector thread — TLS handshake takes
     * several seconds; recording starts immediately and buffers
     * audio until the ASR stream becomes available. */
    {
        pthread_t asr_tid;
        pthread_attr_t asr_attr;
        pthread_attr_init(&asr_attr);
        pthread_attr_setstacksize(&asr_attr, ASR_THREAD_STACK);
        pthread_attr_setdetachstate(&asr_attr, PTHREAD_CREATE_DETACHED);
        if (pthread_create(&asr_tid, &asr_attr,
                asr_connector_thread, NULL) != 0) {
            syslog(LOG_WARNING,
                "[%s] ASR connector thread create failed\n", TAG);
        }
        pthread_attr_destroy(&asr_attr);
    }

    {
        pthread_t tts_tid;
        pthread_attr_t tts_attr;
        pthread_attr_init(&tts_attr);
        pthread_attr_setstacksize(&tts_attr, 24 * 1024);
        pthread_attr_setdetachstate(&tts_attr, PTHREAD_CREATE_DETACHED);
        if (pthread_create(&tts_tid, &tts_attr,
                (void* (*)(void*))ws_conn_pool_preconnect_tts, NULL) != 0) {
            syslog(LOG_WARNING,
                "[%s] TTS preconnect thread create failed\n", TAG);
        }
        pthread_attr_destroy(&tts_attr);
    }

    {
        extern void llm_pool_preconnect(void);
        llm_pool_preconnect();
    }

    syslog(LOG_INFO, "[%s] PTT recording started\n", TAG);
    printf("Voice: recording... (use voice_stop to finish)\n");
    return 0;
}

int voice_channel_stop(void)
{
    pthread_mutex_lock(&s_voice.lock);
    syslog(LOG_INFO, "[%s] voice_channel_stop: state=%d\n", TAG, s_voice.state);
    if (s_voice.state != VOICE_RECORDING) {
        pthread_mutex_unlock(&s_voice.lock);
        syslog(LOG_WARNING, "[%s] voice_channel_stop: not recording (state=%d)\n", TAG, s_voice.state);
        return -EINVAL;
    }
    s_voice.state = VOICE_STOPPING;
    pthread_mutex_unlock(&s_voice.lock);

    /* WiFi PS OFF was acquired in voice_channel_start; release now that
     * recording is stopping (ASR finish may still use network, but PS
     * jitter impact is less critical for one-shot ASR than streaming). */
    voice_wifi_ps_release();

    /* Close capture device BEFORE joining — this unblocks
     * audio_capture_read() in the recording thread so it can
     * observe the STOPPING state and exit.  Otherwise pthread_join
     * deadlocks because the recording thread is stuck in a
     * blocking media_recorder_read_data() call. */
    audio_capture_close(s_voice.cap);

    syslog(LOG_INFO, "[%s] voice_channel_stop: capture closed, joining rec_thread...\n", TAG);

    /* Wait for recording thread to finish */
    pthread_join(s_voice.rec_thread, NULL);

    syslog(LOG_INFO, "[%s] voice_channel_stop: rec_thread joined\n", TAG);

    pthread_mutex_lock(&s_voice.lock);
    s_voice.cap = NULL;
    size_t pcm_len = s_voice.pcm_len;
    voice_asr_stream_t* stream = s_voice.asr_stream;
    s_voice.asr_stream = NULL;
    s_voice.state = VOICE_IDLE;
    pthread_mutex_unlock(&s_voice.lock);

    syslog(LOG_INFO, "[%s] recorded %zu bytes (stream=%p)\n",
        TAG, pcm_len, (void*)stream);

    /* If streaming ASR was active, finish it to get result.
     * Note: in streaming mode pcm_len is always 0 because audio
     * goes directly to the ASR server, not to pcm_buf. */
    if (stream) {
        printf("Voice: streaming ASR active, finishing...\n");
        struct timespec asr_t0;
        clock_gettime(CLOCK_MONOTONIC, &asr_t0);
        char text[512];
        int ret = voice_asr_stream_finish(stream, text,
            sizeof(text));
        struct timespec asr_t1;
        clock_gettime(CLOCK_MONOTONIC, &asr_t1);
        long asr_ms = (asr_t1.tv_sec - asr_t0.tv_sec) * 1000
            + (asr_t1.tv_nsec - asr_t0.tv_nsec) / 1000000;
        syslog(LOG_INFO, "[%s] ASR finish: %ldms\n", TAG, asr_ms);

        free(s_voice.pcm_buf); /* may be NULL */
        s_voice.pcm_buf = NULL;

        if (ret == 0 && text[0] != '\0') {
            syslog(LOG_INFO, "[%s] stream ASR: %s\n", TAG, text);
            printf("ASR: %s\n", text);
            clock_gettime(CLOCK_MONOTONIC, &s_asr_done_ts);

            ws_server_broadcast_typed("asr", text, AGENT_CHAN_VOICE);

            agent_msg_t msg;

            memset(&msg, 0, sizeof(msg));
            strncpy(msg.channel, AGENT_CHAN_VOICE,
                sizeof(msg.channel) - 1);
            strncpy(msg.chat_id, "voice",
                sizeof(msg.chat_id) - 1);
            msg.content = strdup(text);
            if (msg.content) {
                int mret = message_bus_push_inbound(&msg);
                syslog(LOG_INFO, "[%s] ASR result pushed to bus: %d\n", TAG, mret);
            }
        } else {
            syslog(LOG_WARNING,
                "[%s] stream ASR failed: %d\n", TAG, ret);
            agent_msg_t emsg;
            memset(&emsg, 0, sizeof(emsg));
            strncpy(emsg.channel, AGENT_CHAN_VOICE,
                sizeof(emsg.channel) - 1);
            strncpy(emsg.chat_id, "voice",
                sizeof(emsg.chat_id) - 1);
            emsg.content = strdup("[ASR failed: credentials not configured]");
            if (emsg.content) {
                message_bus_push_inbound(&emsg);
            }
        }
        return 0;
    }

    /* Batch fallback: pcm_buf must exist if we reach here */
    if (!s_voice.pcm_buf || pcm_len == 0) {
        free(s_voice.pcm_buf);
        s_voice.pcm_buf = NULL;
        syslog(LOG_ERR, "[%s] no PCM data for batch ASR\n", TAG);
        return -ENODATA;
    }

    /* Batch fallback: pack pcm_len + audio for ASR thread */
    size_t total = sizeof(size_t) + pcm_len;
    unsigned char* pkg = malloc(total);

    if (!pkg) {
        free(s_voice.pcm_buf);
        s_voice.pcm_buf = NULL;
        return -ENOMEM;
    }

    memcpy(pkg, &pcm_len, sizeof(size_t));
    memcpy(pkg + sizeof(size_t), s_voice.pcm_buf, pcm_len);
    free(s_voice.pcm_buf);
    s_voice.pcm_buf = NULL;

    /* Spawn ASR worker thread */
    int ret = agent_task_create(asr_and_dispatch, "asr_worker",
        ASR_THREAD_STACK, pkg, AGENT_VOICE_PRIO);

    if (ret != OK) {
        free(pkg);
        return -EIO;
    }

    return 0;
}

int voice_channel_stop_with_text(char* text_out, size_t text_cap)
{
    if (!text_out || text_cap == 0) {
        return -EINVAL;
    }

    text_out[0] = '\0';

    pthread_mutex_lock(&s_voice.lock);
    if (s_voice.state != VOICE_RECORDING) {
        pthread_mutex_unlock(&s_voice.lock);
        return -EINVAL;
    }
    s_voice.state = VOICE_STOPPING;
    pthread_mutex_unlock(&s_voice.lock);

    /* Release WiFi PS OFF acquired in voice_channel_start */
    voice_wifi_ps_release();

    /* Wait for recording thread to finish */
    pthread_join(s_voice.rec_thread, NULL);

    /* Close capture device */
    audio_capture_close(s_voice.cap);

    pthread_mutex_lock(&s_voice.lock);
    s_voice.cap = NULL;
    size_t pcm_len = s_voice.pcm_len;
    voice_asr_stream_t* stream = s_voice.asr_stream;
    s_voice.asr_stream = NULL;
    s_voice.state = VOICE_IDLE;
    pthread_mutex_unlock(&s_voice.lock);

    syslog(LOG_INFO, "[%s] recorded %zu bytes (with_text, stream=%p)\n",
        TAG, pcm_len, (void*)stream);

    /* Streaming ASR path: finish and copy text to caller.
     * In streaming mode audio goes directly to server, so
     * pcm_len may be 0 — that's OK, don't abort. */
    if (stream) {
        char text[512];
        int ret = voice_asr_stream_finish(stream, text,
            sizeof(text));

        free(s_voice.pcm_buf);
        s_voice.pcm_buf = NULL;

        if (ret == 0 && text[0] != '\0') {
            syslog(LOG_INFO, "[%s] stream ASR (with_text): %s\n",
                TAG, text);
            strncpy(text_out, text, text_cap - 1);
            text_out[text_cap - 1] = '\0';
        }
        return 0;
    }

    /* Batch fallback: synchronous ASR inline (no thread) */
    if (!s_voice.pcm_buf || pcm_len == 0) {
        free(s_voice.pcm_buf);
        s_voice.pcm_buf = NULL;
        syslog(LOG_ERR, "[%s] no PCM data for batch ASR\n", TAG);
        return -ENODATA;
    }

    syslog(LOG_INFO, "[%s] batch ASR (with_text): %zu bytes\n",
        TAG, pcm_len);

    char text[512];
    int ret = voice_asr_recognize(s_voice.pcm_buf, pcm_len,
        text, sizeof(text));

    free(s_voice.pcm_buf);
    s_voice.pcm_buf = NULL;

    if (ret == 0 && text[0] != '\0') {
        syslog(LOG_INFO, "[%s] batch ASR result: %s\n", TAG, text);
        strncpy(text_out, text, text_cap - 1);
        text_out[text_cap - 1] = '\0';
    } else if (ret != 0) {
        syslog(LOG_WARNING, "[%s] batch ASR failed: %d\n",
            TAG, ret);
    }

    return 0;
}

/* Strip Markdown formatting that TTS would read aloud.
 * Removes: **bold**, *italic*, # headings, - list bullets.
 * Operates in-place, result is always <= original length. */
static void tts_strip_markdown(char* s)
{
    char* r = s; /* read pointer */
    char* w = s; /* write pointer */

    while (*r) {
        /* Skip ** (bold markers) */
        if (r[0] == '*' && r[1] == '*') {
            r += 2;
            continue;
        }

        /* Skip lone * (italic markers) \xe2\x80\x94 but keep * in
         * contexts like "3*4" (digit before and after) */
        if (r[0] == '*') {
            if ((r == s || !isdigit((unsigned char)r[-1]))
                || !isdigit((unsigned char)r[1])) {
                r++;
                continue;
            }
        }

        /* Skip # at line start (headings) */
        if (r[0] == '#' && (r == s || r[-1] == '\n')) {
            while (*r == '#' || *r == ' ') {
                r++;
            }
            continue;
        }

        /* Skip "- " at line start (list bullets) */
        if (r[0] == '-' && r[1] == ' '
            && (r == s || r[-1] == '\n')) {
            r += 2;
            continue;
        }

        /* 删除 \r（Windows换行残留），保留 \n 供分段循环按行切分。
         * \n 不会直接发给TTS：has_newlines检测到\n后会启用分段，
         * 分段循环按\n切分后每段内部不含\n，TTS收到的文本无换行符。
         * 与 sanitize_doc_content 保持一致：保留\n作为自然分段点。 */
        if (*r == '\r') {
            r++;
            continue;
        }

        *w++ = *r++;
    }

    *w = '\0';
}

void voice_channel_set_quick_path(int hit)
{
    s_quick_path_hit = hit;
}

#define TTS_MAX_SEGMENT_BYTES 300

static int is_cjk_sentence_end(const char* p, int bytes)
{
    if (bytes == 3) {
        unsigned int cp = ((unsigned char)p[0] & 0x0F) << 12
            | ((unsigned char)p[1] & 0x3F) << 6
            | ((unsigned char)p[2] & 0x3F);
        /* 切分标点：句号、感叹号、问号、省略号、
         * 中文分号，用于TTS断句切分。 */
        return (cp == 0x3002 || cp == 0xFF01 || cp == 0xFF1F
            || cp == 0xFF1B || cp == 0x2026);
    }
    return 0;
}

static int utf8_char_bytes(const char* p)
{
    if ((p[0] & 0x80) == 0x00) return 1;
    if ((p[0] & 0xE0) == 0xC0) return 2;
    if ((p[0] & 0xF0) == 0xE0) return 3;
    if ((p[0] & 0xF8) == 0xF0) return 4;
    return 1;
}

static int speak_single_segment(const char* seg, audio_playback_t* pb)
{
    int cache_hit = 0;
    int ret = tts_cache_lookup_stream(seg, tts_stream_cb, pb);
    if (ret == 0) {
        cache_hit = 1;
        syslog(LOG_INFO, "[%s] TTS cache HIT (seg): \"%.*s\"\n",
            TAG, 40, seg);
    } else {
        unsigned char* cache_buf = malloc(TTS_CACHE_MAX_PCM_LEN);
        tts_cache_cb_ctx_t cache_ctx = {
            .pb = pb,
            .cache_buf = cache_buf,
            .cache_len = 0,
            .cache_cap = cache_buf ? TTS_CACHE_MAX_PCM_LEN : 0,
            .cache_text = seg,
        };

        ret = voice_tts_speak_stream(seg,
            tts_cache_aware_cb, &cache_ctx);

        if (cache_buf && cache_ctx.cache_len > 0 && cache_ctx.cache_text[0])
            tts_cache_store(cache_ctx.cache_text, cache_buf, cache_ctx.cache_len);
        free(cache_buf);

        syslog(LOG_INFO, "[%s] TTS seg speak_stream returned: %d\n", TAG, ret);
    }
    (void)cache_hit;
    return ret;
}

int voice_channel_speak(const char* text)
{
    if (!text || text[0] == '\0') {
        return -EINVAL;
    }

    ws_conn_pool_idle_check();

    extern void llm_pool_idle_check(void);
    llm_pool_idle_check();

    if ((s_voice.speak_count % 10) == 0 && s_voice.speak_count > 0)
        voice_perf_dump();
    s_voice.speak_count++;

    pthread_mutex_lock(&s_voice.lock);
    if (s_voice.tts_pb || s_voice.tts_speaking) {
        syslog(LOG_INFO, "[%s] speak: aborting previous TTS\n", TAG);
        s_voice.tts_abort = 1;
        s_voice.tts_speaking = 0;
        if (s_voice.tts_pb)
            tts_pb_stop();  /* 停止 consumer + playback */
    }
    pthread_mutex_unlock(&s_voice.lock);

    pthread_mutex_lock(&s_voice.speak_lock);

    pthread_mutex_lock(&s_voice.lock);
    if (s_voice.state != VOICE_IDLE) {
        pthread_mutex_unlock(&s_voice.lock);
        pthread_mutex_unlock(&s_voice.speak_lock);
        syslog(LOG_INFO,
            "[%s] speak: skipped, recording active\n", TAG);
        return -EBUSY;
    }
    pthread_mutex_unlock(&s_voice.lock);

    size_t text_len = strlen(text);
    char* clean = malloc(text_len + 1);

    if (!clean) {
        pthread_mutex_unlock(&s_voice.speak_lock);
        return -ENOMEM;
    }

    memcpy(clean, text, text_len + 1);

    /* 将换行符合并为单个空格，避免 has_newlines 触发分段模式
     * （段间 session 衔接会产生怪音）。换行符在屏幕显示侧保留
     * （reply 原文不变），这里只在 TTS 合成前处理。 */
    {
        char *rp = clean;
        char *wp = clean;
        int prev_space = 0;
        while (*rp) {
            if (*rp == '\n' || *rp == '\r') {
                if (!prev_space) { *wp++ = ' '; prev_space = 1; }
            } else {
                *wp++ = *rp;
                prev_space = 0;
            }
            rp++;
        }
        *wp = '\0';
    }

    tts_strip_markdown(clean);

    {
        const char* p = clean;
        while (*p == '\n' || *p == '\r' || *p == ' ' || *p == '\t')
            p++;
        if (*p == '\0') {
            free(clean);
            pthread_mutex_unlock(&s_voice.speak_lock);
            return 0;
        }
        if (p != clean)
            memmove(clean, p, strlen(p) + 1);
    }

    if (clean[0] == '\0') {
        free(clean);
        pthread_mutex_unlock(&s_voice.speak_lock);
        return 0;
    }

    syslog(LOG_INFO, "[%s] speak: \"%.*s\" (%zu bytes)\n",
        TAG, 60, clean, strlen(clean));

    clock_gettime(CLOCK_MONOTONIC, &s_tts_start);
    s_tts_first_chunk = 0;

    if (s_asr_done_ts.tv_sec > 0) {
        long llm_ms = (s_tts_start.tv_sec - s_asr_done_ts.tv_sec) * 1000
            + (s_tts_start.tv_nsec - s_asr_done_ts.tv_nsec) / 1000000;
        syslog(LOG_INFO, "[%s] LLM latency: %ldms\n", TAG, llm_ms);
    }

    /* 使用文件缓存架构：emmc 文件 + consumer 线程，合成线程不阻塞。
     * tts_pb_open 内部打开 playback + 缓存文件 + 启动 consumer 线程。 */
    audio_playback_t* pb = tts_pb_open();

    if (!pb) {
        syslog(LOG_ERR, "[%s] playback open failed\n", TAG);
        free(clean);
        pthread_mutex_unlock(&s_voice.speak_lock);
        return -EIO;
    }

    pthread_mutex_lock(&s_voice.lock);
    s_voice.tts_abort = 0;
    s_voice.tts_pb = pb;
    s_voice.tts_speaking = 1;  /* 标记TTS播放循环开始 */
    pthread_mutex_unlock(&s_voice.lock);

    /* WiFi PS OFF during TTS streaming playback (network-heavy downlink) */
    voice_wifi_ps_acquire();

    int ret = 0;
    int cache_hit = 0;
    size_t clean_len = strlen(clean);

    /* 判断是否需要分段：仅根据文本长度判断。
     * 换行符已在前面合并为空格，不再触发强制分段。
     * 避免多段合成时段间 session 衔接产生怪音。 */
    int need_split = (clean_len > TTS_MAX_SEGMENT_BYTES);

    if (!need_split) {
        ret = tts_cache_lookup_stream(clean, tts_stream_cb, pb);
        if (ret == 0) {
            cache_hit = 1;
            syslog(LOG_INFO, "[%s] TTS cache HIT: \"%s\"\n", TAG, clean);
        } else {
            syslog(LOG_INFO, "[%s] TTS cache MISS, calling voice_tts_speak_stream (backend=%s) ...\n",
                TAG, voice_tts_get_backend() ? voice_tts_get_backend() : "NULL");

            unsigned char* cache_buf = malloc(TTS_CACHE_MAX_PCM_LEN);
            tts_cache_cb_ctx_t cache_ctx = {
                .pb = pb,
                .cache_buf = cache_buf,
                .cache_len = 0,
                .cache_cap = cache_buf ? TTS_CACHE_MAX_PCM_LEN : 0,
                .cache_text = clean,
            };

            ret = voice_tts_speak_stream(clean,
                tts_cache_aware_cb, &cache_ctx);

            if (cache_buf && cache_ctx.cache_len > 0 && cache_ctx.cache_text[0])
                tts_cache_store(cache_ctx.cache_text, cache_buf, cache_ctx.cache_len);
            free(cache_buf);

            syslog(LOG_INFO, "[%s] voice_tts_speak_stream returned: %d\n", TAG, ret);
        }
    } else {
        syslog(LOG_INFO, "[%s] TTS text too long (%zu > %d), splitting by punctuation\n",
            TAG, clean_len, TTS_MAX_SEGMENT_BYTES);

        const char* p = clean;
        int seg_count = 0;

        while (*p) {
            const char* seg_start = p;
            size_t seg_len = 0;

            /* Scan until the next punctuation mark (inclusive) or until
             * segment reaches TTS_MAX_SEGMENT_BYTES.
             * 在逗号、句号、换行、空格等标点处切分，让TTS自然断句。
             * 注意：空格不作为切分符，避免英文单词被拆分到不同段导致TTS断句异常 */
            while (*p) {
                int cb = utf8_char_bytes(p);
                char c = p[0];
                seg_len += cb;

                int is_punct = 0;
                if (c == '\n' || c == '!' || c == '?'
                    || c == ';') {
                    is_punct = 1;
                } else if (c == '.') {
                    /* 前面是数字时不拆分（如 3.14、2. 等），
                     * 避免 "2." 这样的序号被单独切分成过短段 */
                    if (p > seg_start &&
                        (unsigned char)p[-1] >= '0' &&
                        (unsigned char)p[-1] <= '9') {
                        /* not a sentence boundary */
                    } else {
                        is_punct = 1;
                    }
                } else if (cb == 3 && is_cjk_sentence_end(p, cb)) {
                    is_punct = 1;
                }

                p += cb;

                if (is_punct) {
                    /* Break right after this punctuation */
                    break;
                }

                /* Safety: hard limit to avoid one segment growing unbounded */
                if (seg_len >= TTS_MAX_SEGMENT_BYTES)
                    break;
            }

            if (seg_len == 0)
                break;

            char* seg = malloc(seg_len + 1);
            if (!seg) break;
            memcpy(seg, seg_start, seg_len);
            seg[seg_len] = '\0';

            if (seg[0] == '\0') {
                free(seg);
                continue;
            }

            /* 跳过纯空白段，避免发给TTS产生异常声音 */
            int all_blank = 1;
            for (size_t i = 0; i < seg_len; i++) {
                char ch = seg[i];
                if (ch != ' ' && ch != '\n' && ch != '\r' && ch != '\t') {
                    all_blank = 0;
                    break;
                }
            }
            if (all_blank) {
                free(seg);
                continue;
            }

            seg_count++;
            syslog(LOG_INFO, "[%s] TTS segment %d (%zu bytes): \"%.*s\"\n",
                TAG, seg_count, seg_len, 40, seg);

            /* 文件缓存架构下无需等待 ring buffer 水位：合成线程将 PCM
             * 写入 emmc 文件后立即返回，consumer 线程独立读取播放。
             * 多段合成可连续进行，整体合成时间缩短。 */

            int seg_ret = speak_single_segment(seg, pb);
            free(seg);

            if (seg_ret != 0) {
                syslog(LOG_WARNING, "[%s] TTS segment %d failed: %d\n",
                    TAG, seg_count, seg_ret);
                ret = seg_ret;
                break;
            }

            pthread_mutex_lock(&s_voice.lock);
            if (s_voice.tts_abort) {
                pthread_mutex_unlock(&s_voice.lock);
                ret = -EINTR;
                break;
            }
            pthread_mutex_unlock(&s_voice.lock);
        }

        syslog(LOG_INFO, "[%s] TTS segmentation done: %d segments\n",
            TAG, seg_count);
    }

    struct timespec tts_net;
    clock_gettime(CLOCK_MONOTONIC, &tts_net);
    long net_ms = (tts_net.tv_sec - s_tts_start.tv_sec) * 1000
        + (tts_net.tv_nsec - s_tts_start.tv_nsec) / 1000000;
    syslog(LOG_INFO, "[%s] TTS network done: %ldms%s\n", TAG, net_ms,
        cache_hit ? " (cache)" : "");

    /* 文件缓存架构：tts_pb_close 等 consumer 读完文件 + drain playback。
     * 阻塞直到音频播放完毕，确保不丢尾包。 */
    tts_pb_close();

    pthread_mutex_lock(&s_voice.lock);
    s_voice.tts_pb = NULL;
    s_voice.tts_abort = 0;
    s_voice.tts_speaking = 0;  /* 标记TTS播放循环结束 */
    pthread_mutex_unlock(&s_voice.lock);

    /* Release WiFi PS OFF acquired at playback open */
    voice_wifi_ps_release();

    free(clean);

    if (ret != 0) {
        syslog(LOG_ERR, "[%s] TTS stream failed: %d\n", TAG, ret);
        pthread_mutex_unlock(&s_voice.speak_lock);
        return ret;
    }

    struct timespec tend;
    clock_gettime(CLOCK_MONOTONIC, &tend);
    long total_ms = (tend.tv_sec - s_tts_start.tv_sec) * 1000
        + (tend.tv_nsec - s_tts_start.tv_nsec) / 1000000;
    syslog(LOG_INFO, "[%s] speak done: total %ldms (play wait %ldms)%s\n",
        TAG, total_ms, total_ms - net_ms,
        cache_hit ? " [CACHE]" : "");

    {
        long llm_ms = 0;
        if (s_asr_done_ts.tv_sec > 0) {
            llm_ms = (s_tts_start.tv_sec - s_asr_done_ts.tv_sec) * 1000
                + (s_tts_start.tv_nsec - s_asr_done_ts.tv_nsec) / 1000000;
        }
        voice_perf_entry_t perf = {
            .asr_latency_ms = 0,
            .llm_latency_ms = (uint32_t)(llm_ms > 0 ? llm_ms : 0),
            .tts_latency_ms = (uint32_t)net_ms,
            .total_latency_ms = (uint32_t)total_ms,
            .tts_first_chunk_ms = 0,
            .tts_cache_hit = cache_hit,
            .quick_path_hit = s_quick_path_hit,
            .llm_stream_used = 0,
        };
        voice_perf_record(&perf);
        s_quick_path_hit = 0;
    }

    pthread_mutex_unlock(&s_voice.speak_lock);
    return 0;
}

/* ── Test commands (file-based, for QEMU) ───────────────── */

int voice_channel_test_tts(const char* text, const char* out_path)
{
    if (!text || !out_path) {
        printf("Usage: voice_test_tts <text> [output_path]\n");
        return -EINVAL;
    }

    printf("TTS: synthesizing \"%s\" ...\n", text);

    unsigned char* pcm = malloc(AGENT_VOICE_PCM_BUF_SIZE);

    if (!pcm) {
        printf("Error: out of memory\n");
        return -ENOMEM;
    }

    size_t pcm_len;
    int ret = voice_tts_speak(text, pcm,
        AGENT_VOICE_PCM_BUF_SIZE, &pcm_len);

    if (ret != 0) {
        free(pcm);
        printf("TTS failed: %d\n", ret);
        return ret;
    }

    ret = write_pcm_file(out_path, pcm, pcm_len);
    free(pcm);

    if (ret == 0) {
        printf("TTS OK: %zu bytes -> %s\n", pcm_len, out_path);
    } else {
        printf("Failed to write %s: %d\n", out_path, ret);
    }

    return ret;
}

typedef struct {
    char pcm_path[256];
} asr_task_args_t;

static void* asr_file_worker(void* arg)
{
    asr_task_args_t* a = (asr_task_args_t*)arg;
    unsigned char* pcm = NULL;
    size_t pcm_len = 0;

    int ret = read_pcm_file(a->pcm_path, &pcm, &pcm_len);

    if (ret != 0) {
        printf("Failed to read %s: %d\n", a->pcm_path, ret);
        free(a);
        return NULL;
    }

    printf("ASR: recognizing %zu bytes from %s ...\n",
        pcm_len, a->pcm_path);

    char text[512];

    ret = voice_asr_recognize(pcm, pcm_len, text, sizeof(text));
    free(pcm);

    if (ret == 0) {
        printf("ASR result: %s\n", text);

        ws_server_broadcast_typed("asr", text, AGENT_CHAN_VOICE);

        agent_msg_t msg;

        memset(&msg, 0, sizeof(msg));
        strncpy(msg.channel, AGENT_CHAN_VOICE,
            sizeof(msg.channel) - 1);
        strncpy(msg.chat_id, "voice",
            sizeof(msg.chat_id) - 1);
        msg.content = strdup(text);
        if (msg.content) {
            message_bus_push_inbound(&msg);
            printf("Sent to agent.\n");
        }
    } else {
        printf("ASR failed: %d\n", ret);
    }

    free(a);
    return NULL;
}

int voice_channel_test_asr(const char* pcm_path)
{
    if (!pcm_path) {
        printf("Usage: voice_test_asr <pcm_file>\n");
        return -EINVAL;
    }

    asr_task_args_t* args = malloc(sizeof(asr_task_args_t));

    if (!args) {
        printf("Error: out of memory\n");
        return -ENOMEM;
    }

    strncpy(args->pcm_path, pcm_path, sizeof(args->pcm_path) - 1);
    args->pcm_path[sizeof(args->pcm_path) - 1] = '\0';

    int ret = agent_task_create(asr_file_worker, "asr_test",
        ASR_THREAD_STACK, args, AGENT_VOICE_PRIO);

    if (ret != OK) {
        printf("Error: failed to create ASR thread\n");
        free(args);
        return -EIO;
    }

    printf("ASR test started (background thread).\n");
    return 0;
}
