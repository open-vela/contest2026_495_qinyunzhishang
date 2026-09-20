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

/* audio_playback.c — Streaming audio playback via media_player buffer mode.
 *
 * Uses an internal ring buffer + drain thread so that the TTS callback
 * (tts_stream_cb) never blocks on media_player_write_data().  Without
 * this, send() inside write_data() blocks the TTS WebSocket receive
 * loop, preventing subsequent audio deltas from being read. */

#include "voice/audio_playback.h"
#include "agent_config.h"

#include <errno.h>
#include <media_player.h>
#include <media_policy.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <syslog.h>
#include <unistd.h>

static const char* TAG = "audio_pb";

#define PB_OPTIONS_LEN 128
#define RING_BUF_SIZE  (256 * 1024)
/* Pre-buffer threshold: wait until this much PCM has accumulated in the
 * ring before starting the media player, so the driver has enough data
 * to chew on during/after the prepare+start gap and does not underrun
 * on the first audio segment. Sized to ~0.67s at 24kHz/16bit/mono so the
 * ring has enough water level to absorb transient network throughput
 * dips (e.g. cloud WS RX dropping from 121 kB/s to 23 kB/s) without
 * immediately draining out and causing playback stutter. Trade-off:
 * first audio is delayed by ~0.67s, acceptable for streaming TTS.
 * Note: 真正的抗抖动缓冲在 emmc 文件层（OMNI_FILE_PREBUFFER_BYTES），
 * ring 层只负责平滑 consumer→drain 的短期波动。 */
#define PREBUFFER_BYTES (32 * 1024)
/* Max wait for pre-buffer before forcing a start. If audio deltas are
 * sparse and the threshold is not reached within this window, start the
 * player with whatever is available so the user hears sound instead of
 * a long silence. 800ms 给 ring 层快速启动，文件层预缓冲已保证数据充足。 */
#define PREBUFFER_TIMEOUT_MS 800
/* Max bytes per media_player_write_data() call. Larger chunks reduce
 * syscall overhead and let the driver buffer more per iteration. */
#define WRITE_CHUNK     4096

static void* s_active_player;

struct audio_playback {
    void* player;
    size_t total_written;
    unsigned int sample_rate;
    unsigned int channels;
    unsigned int bits_per_sample;
    unsigned int output_sample_rate;
    volatile int stopped;
    volatile int started;

    unsigned char* ring;
    volatile size_t ring_head;
    volatile size_t ring_tail;
    pthread_mutex_t ring_mtx;
    sem_t ring_sem;
    pthread_t drain_tid;
    volatile int drain_running;
    volatile int eos;
    long long play_start_ms;
    long long prebuffer_start_ms;
    long long total_underrun_ms;
    long long underrun_start_ms;

    int resample_needed;
    int16_t resample_prev;
    int resample_phase;
};

static size_t ring_used(const audio_playback_t* pb)
{
    size_t h = pb->ring_head;
    size_t t = pb->ring_tail;
    return (h >= t) ? (h - t) : (RING_BUF_SIZE - t + h);
}

static size_t ring_free(const audio_playback_t* pb)
{
    return RING_BUF_SIZE - 1 - ring_used(pb);
}

static size_t ring_read(audio_playback_t* pb, unsigned char* dst, size_t len)
{
    size_t done = 0;
    size_t t = pb->ring_tail;

    while (done < len && t != pb->ring_head) {
        size_t contig = (pb->ring_head >= t)
            ? (pb->ring_head - t)
            : (RING_BUF_SIZE - t);
        size_t n = len - done;
        if (n > contig) n = contig;
        memcpy(dst + done, pb->ring + t, n);
        t += n;
        if (t >= RING_BUF_SIZE) t = 0;
        done += n;
    }

    pb->ring_tail = t;
    return done;
}

static void* drain_thread(void* arg)
{
    audio_playback_t* pb = (audio_playback_t*)arg;
    unsigned char tmp[WRITE_CHUNK];

    /* Continue draining while running, or when eos is set and there's
     * still data in the ring. This prevents premature exit when
     * audio_playback_close sets drain_running=0 before the thread has
     * processed ring data (e.g. TTS data arrives all at once and the
     * consumer fills the ring faster than the 50ms prebuffer poll). */
    while (!pb->stopped &&
           (pb->drain_running || (pb->eos && ring_used(pb) > 0))) {
        /* Pre-buffer phase: poll with usleep instead of sem_wait so
         * the PREBUFFER_TIMEOUT_MS can actually fire even when audio
         * deltas are sparse. Once started, use sem_wait for efficiency. */
        if (!pb->started && !pb->eos) {
            pthread_mutex_lock(&pb->ring_mtx);
            size_t pending = ring_used(pb);
            pthread_mutex_unlock(&pb->ring_mtx);

            if (pending < PREBUFFER_BYTES) {
                struct timeval tv;
                gettimeofday(&tv, NULL);
                long long now_ms = (long long)tv.tv_sec * 1000
                    + tv.tv_usec / 1000;
                if (pb->prebuffer_start_ms == 0) {
                    pb->prebuffer_start_ms = now_ms;
                    usleep(50 * 1000);
                    continue;
                } else if (now_ms - pb->prebuffer_start_ms
                           < PREBUFFER_TIMEOUT_MS) {
                    usleep(50 * 1000);
                    continue;
                }
                pb->prebuffer_start_ms = 0;
            } else {
                pb->prebuffer_start_ms = 0;
            }
            goto drain_process;
        }

        sem_wait(&pb->ring_sem);

        /* End underrun tracking if we were in one */
        if (pb->underrun_start_ms > 0) {
            struct timeval uvt;
            gettimeofday(&uvt, NULL);
            long long now_ms = (long long)uvt.tv_sec * 1000
                + uvt.tv_usec / 1000;
            pb->total_underrun_ms += now_ms - pb->underrun_start_ms;
            pb->underrun_start_ms = 0;
        }

        if (pb->stopped) break;

    drain_process:

        for (;;) {
            pthread_mutex_lock(&pb->ring_mtx);
            size_t avail = ring_used(pb);
            if (avail == 0) {
                pthread_mutex_unlock(&pb->ring_mtx);
                /* Start underrun tracking if player has started */
                if (pb->started && !pb->stopped) {
                    struct timeval uvt;
                    gettimeofday(&uvt, NULL);
                    pb->underrun_start_ms = (long long)uvt.tv_sec * 1000
                        + uvt.tv_usec / 1000;
                }
                break;
            }
            size_t want = avail > sizeof(tmp) ? sizeof(tmp) : avail;
            size_t got = ring_read(pb, tmp, want);
            pthread_mutex_unlock(&pb->ring_mtx);

            if (got == 0) break;

            if (!pb->started) {
                char opts[PB_OPTIONS_LEN];
                snprintf(opts, sizeof(opts),
                    "oMediaScript=[codec=pcm,rate=#%u,ch=#%u,bits=#%u]",
                    pb->output_sample_rate, pb->channels, pb->bits_per_sample);

                int ret = media_player_prepare(pb->player, NULL, opts);
                if (ret < 0) {
                    syslog(LOG_ERR, "[%s] prepare failed: %d\n", TAG, ret);
                    pb->stopped = 1;
                    break;
                }

                ret = media_player_start(pb->player);
                if (ret < 0) {
                    syslog(LOG_ERR, "[%s] start failed: %d\n", TAG, ret);
                    pb->stopped = 1;
                    break;
                }
                pb->started = 1;
                {
                    struct timeval tv;
                    gettimeofday(&tv, NULL);
                    pb->play_start_ms = (long long)tv.tv_sec * 1000
                        + tv.tv_usec / 1000;
                }
                syslog(LOG_INFO, "[%s] player started\n", TAG);
                usleep(500000);
            }

            size_t off = 0;
            while (off < got) {
                size_t chunk = got - off;
                if (chunk > WRITE_CHUNK) chunk = WRITE_CHUNK;
                ssize_t n = media_player_write_data(pb->player, tmp + off, chunk);
                if (n > 0) {
                    off += (size_t)n;
                    pb->total_written += (size_t)n;
                } else {
                    syslog(LOG_ERR, "[%s] write_data failed: %zd\n", TAG, n);
                    pb->stopped = 1;
                    break;
                }
            }

            if (pb->stopped) break;
        }

        if (pb->stopped) break;
    }

    if (pb->eos && !pb->stopped && pb->started) {
        pthread_mutex_lock(&pb->ring_mtx);
        size_t avail = ring_used(pb);
        pthread_mutex_unlock(&pb->ring_mtx);

        while (avail > 0 && !pb->stopped) {
            size_t want = avail > sizeof(tmp) ? sizeof(tmp) : avail;
            size_t got;
            pthread_mutex_lock(&pb->ring_mtx);
            got = ring_read(pb, tmp, want);
            avail = ring_used(pb);
            pthread_mutex_unlock(&pb->ring_mtx);

            if (got == 0) break;

            size_t off = 0;
            while (off < got) {
                size_t chunk = got - off;
                if (chunk > WRITE_CHUNK) chunk = WRITE_CHUNK;
                ssize_t n = media_player_write_data(pb->player, tmp + off, chunk);
                if (n > 0) {
                    off += (size_t)n;
                    pb->total_written += (size_t)n;
                } else {
                    break;
                }
            }
        }
    }

    return NULL;
}

audio_playback_t* audio_playback_open(const char* dev_path,
    unsigned int sample_rate, unsigned int channels,
    unsigned int bits_per_sample)
{
    (void)dev_path;

    if (s_active_player) {
        media_player_stop(s_active_player);
        media_player_close(s_active_player, 0);
        s_active_player = NULL;
        usleep(100000);
    }

    void* player = media_player_open(MEDIA_STREAM_MUSIC);

    if (!player) {
        syslog(LOG_ERR, "[%s] media_player_open failed\n", TAG);
        return NULL;
    }

    int mute = 0;
    media_policy_get_mute_mode(&mute);
    if (mute) {
        media_policy_set_mute_mode(0);
    }

    int vol_min = 0, vol_max = 15;
    media_policy_get_range(MEDIA_STREAM_MEDIA MEDIA_POLICY_VOLUME,
                           &vol_min, &vol_max);
    media_policy_set_stream_volume(MEDIA_STREAM_MEDIA, vol_max);

    vol_min = 0; vol_max = 15;
    media_policy_get_range(MEDIA_STREAM_MUSIC MEDIA_POLICY_VOLUME,
                           &vol_min, &vol_max);
    media_policy_set_stream_volume(MEDIA_STREAM_MUSIC, vol_max);

    usleep(200000);

    media_player_set_volume(player, 1.0f);

    audio_playback_t* pb = calloc(1, sizeof(*pb));
    if (!pb) {
        media_player_close(player, 0);
        return NULL;
    }

    pb->ring = malloc(RING_BUF_SIZE);
    if (!pb->ring) {
        media_player_close(player, 0);
        free(pb);
        return NULL;
    }

    pb->player = player;
    pb->sample_rate = sample_rate;
    pb->channels = channels;
    pb->bits_per_sample = bits_per_sample;
    pb->resample_needed = (sample_rate == 24000) ? 1 : 0;
    pb->output_sample_rate = pb->resample_needed ? 16000 : sample_rate;
    pb->resample_prev = 0;
    pb->resample_phase = 0;
    pb->stopped = 0;
    pb->started = 0;
    pb->ring_head = 0;
    pb->ring_tail = 0;
    pb->drain_running = 1;
    pb->eos = 0;
    pb->play_start_ms = 0;
    pb->prebuffer_start_ms = 0;
    pb->total_underrun_ms = 0;
    pb->underrun_start_ms = 0;
    pthread_mutex_init(&pb->ring_mtx, NULL);
    sem_init(&pb->ring_sem, 0, 0);

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 8192);
    pthread_create(&pb->drain_tid, &attr, drain_thread, pb);
    pthread_attr_destroy(&attr);

    s_active_player = player;

    syslog(LOG_INFO, "[%s] opened (%uHz->%uHz%s)\n", TAG, sample_rate,
           pb->output_sample_rate, pb->resample_needed ? " resample" : "");

    return pb;
}

int audio_playback_write(audio_playback_t* pb,
    const void* buf, size_t len)
{
    if (!pb || !pb->player || !buf || len == 0) {
        return -EINVAL;
    }

    if (pb->stopped) {
        syslog(LOG_WARNING, "[%s] write(%zu) while stopped\n", TAG, len);
        return -ECANCELED;
    }

    const unsigned char* src = (const unsigned char*)buf;
    size_t src_len = len;

    if (pb->resample_needed && pb->bits_per_sample == 16 && pb->channels == 1) {
        size_t in_frames = src_len / 2;
        size_t out_frames = (in_frames * 2 + 2) / 3;
        size_t out_bytes = out_frames * 2;

        int16_t* out_buf = (int16_t*)malloc(out_bytes);
        if (!out_buf) {
            syslog(LOG_WARNING, "[%s] resample malloc failed, passthrough\n", TAG);
            goto passthrough;
        }

        const int16_t* in = (const int16_t*)src;
        int16_t* out = out_buf;
        int phase = pb->resample_phase;
        int16_t prev = pb->resample_prev;
        size_t oi = 0;

        for (size_t i = 0; i < in_frames; i++) {
            if (phase == 0) {
                out[oi++] = in[i];
            } else if (phase == 2) {
                out[oi++] = (int16_t)(((int32_t)prev + (int32_t)in[i]) / 2);
            }
            prev = in[i];
            phase = (phase + 1) % 3;
        }

        pb->resample_phase = phase;
        pb->resample_prev = prev;

        src = (const unsigned char*)out_buf;
        src_len = oi * 2;

        const unsigned char* resample_ptr = src;
        size_t remaining = src_len;

        while (remaining > 0) {
            pthread_mutex_lock(&pb->ring_mtx);
            size_t free_sz = ring_free(pb);
            if (free_sz == 0) {
                pthread_mutex_unlock(&pb->ring_mtx);
                usleep(5000);
                if (pb->stopped) { free(out_buf); return -ECANCELED; }
                continue;
            }

            size_t want = remaining;
            if (want > free_sz) want = free_sz;

            size_t h = pb->ring_head;
            size_t first = (RING_BUF_SIZE - h >= want) ? want : (RING_BUF_SIZE - h);
            memcpy(pb->ring + h, resample_ptr, first);
            if (first < want) {
                memcpy(pb->ring, resample_ptr + first, want - first);
            }
            h += want;
            if (h >= RING_BUF_SIZE) h -= RING_BUF_SIZE;
            pb->ring_head = h;
            pthread_mutex_unlock(&pb->ring_mtx);

            resample_ptr += want;
            remaining -= want;
            sem_post(&pb->ring_sem);
        }

        free(out_buf);
        return (int)len;
    }

passthrough:
    {
        const unsigned char* remaining_ptr = src;
        size_t remaining = src_len;

        while (remaining > 0) {
            pthread_mutex_lock(&pb->ring_mtx);
            size_t free_sz = ring_free(pb);
            if (free_sz == 0) {
                pthread_mutex_unlock(&pb->ring_mtx);
                usleep(5000);
                if (pb->stopped) return -ECANCELED;
                continue;
            }

            size_t want = remaining;
            if (want > free_sz) want = free_sz;

            size_t h = pb->ring_head;
            size_t first = (RING_BUF_SIZE - h >= want) ? want : (RING_BUF_SIZE - h);
            memcpy(pb->ring + h, remaining_ptr, first);
            if (first < want) {
                memcpy(pb->ring, remaining_ptr + first, want - first);
            }
            h += want;
            if (h >= RING_BUF_SIZE) h -= RING_BUF_SIZE;
            pb->ring_head = h;
            pthread_mutex_unlock(&pb->ring_mtx);

            remaining_ptr += want;
            remaining -= want;
            sem_post(&pb->ring_sem);
        }
    }

    return (int)len;
}

void audio_playback_stop(audio_playback_t* pb)
{
    if (pb) {
        pb->stopped = 1;
        sem_post(&pb->ring_sem);
        if (pb->player) {
            media_player_stop(pb->player);
        }
    }
}

size_t audio_playback_pending_bytes(audio_playback_t* pb)
{
    if (!pb) return 0;
    pthread_mutex_lock(&pb->ring_mtx);
    size_t used = ring_used(pb);
    pthread_mutex_unlock(&pb->ring_mtx);
    return used;
}

void audio_playback_close(audio_playback_t* pb)
{
    if (!pb) {
        return;
    }

    pb->eos = 1;
    sem_post(&pb->ring_sem);

    if (pb->drain_running) {
        pb->drain_running = 0;
        sem_post(&pb->ring_sem);
        pthread_join(pb->drain_tid, NULL);
    }

    if (pb->player) {

        if (pb->started && pb->total_written > 0
            && pb->output_sample_rate > 0 && pb->play_start_ms > 0) {
            unsigned int bytes_per_sec = pb->output_sample_rate
                * pb->channels * (pb->bits_per_sample / 8);
            if (bytes_per_sec > 0) {
                unsigned int audio_ms =
                    (unsigned int)(pb->total_written * 1000ULL / bytes_per_sec);
                struct timeval tv;
                gettimeofday(&tv, NULL);
                long long now_ms = (long long)tv.tv_sec * 1000
                    + tv.tv_usec / 1000;
                long long elapsed_ms = now_ms - pb->play_start_ms;
                /* Account for underrun time (gaps between TTS segments
                 * where the ring buffer was empty and the media player
                 * had no new data). Without this correction, elapsed_ms
                 * overestimates actual playback time, making remain_ms
                 * too small or negative, which cuts off the last segment. */
                long long remain_ms = (long long)audio_ms - elapsed_ms
                    + pb->total_underrun_ms + 500;
                syslog(LOG_INFO, "[%s] drain wait: %lldms (audio=%u "
                    "elapsed=%lld underrun=%lld)\n", TAG,
                    remain_ms, audio_ms, elapsed_ms,
                    pb->total_underrun_ms);
                if (remain_ms > 0) {
                    usleep((useconds_t)remain_ms * 1000);
                }
            }
        }

        media_player_close_socket(pb->player);
        usleep(100 * 1000);
        media_player_stop(pb->player);
        usleep(50 * 1000);
        media_player_close(pb->player, 0);

        s_active_player = NULL;
    }

    if (pb->ring) {
        free(pb->ring);
        pb->ring = NULL;
    }
    pthread_mutex_destroy(&pb->ring_mtx);
    sem_destroy(&pb->ring_sem);
    free(pb);
}
