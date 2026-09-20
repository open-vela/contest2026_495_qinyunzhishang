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

/* audio_capture.c — Audio capture via Vela media_recorder API.
 *
 * Wraps media_recorder in buffer mode into the simple
 * open / start / read / close API defined in audio_capture.h. */

#include "voice/audio_capture.h"
#include "agent_config.h"

#include <errno.h>
#include <media_recorder.h>
#include <media_policy.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

static const char* TAG = "audio_cap";

#define CAP_OPTIONS_LEN 128

/* Audio gain multiplier to compensate for multi-channel DMIC mixing.
 * When board configures 6 DMIC channels but only 1 has a physical mic,
 * the mono mix dilutes the signal by ~6x. Set gain to compensate.
 * Configurable via CONFIG_AI_AGENT_AUDIO_CAPTURE_GAIN in Kconfig. */
#ifdef CONFIG_AI_AGENT_AUDIO_CAPTURE_GAIN
#define AGENT_AUDIO_CAPTURE_GAIN CONFIG_AI_AGENT_AUDIO_CAPTURE_GAIN
#else
#define AGENT_AUDIO_CAPTURE_GAIN 6
#endif

struct audio_capture {
    void* recorder;
};

static void* s_active_recorder;

audio_capture_t* audio_capture_open(const char* dev_path,
    unsigned int sample_rate, unsigned int channels,
    unsigned int bits_per_sample)
{
    (void)dev_path; /* media framework handles routing */

    /* Guard against stale recorder from a previous session that was
     * not fully closed.  media_recorder_close is asynchronous — the
     * underlying pcm0c stream may still be releasing when we get
     * here.  Force-close the stale recorder and give the audio
     * framework time to finish cleanup. */
    if (s_active_recorder) {
        syslog(LOG_WARNING, "[%s] force closing stale recorder\n", TAG);
        media_recorder_stop(s_active_recorder);
        media_recorder_close(s_active_recorder);
        s_active_recorder = NULL;
        usleep(100000); /* 100ms — let pcm0c stream fully release */
    }

    audio_capture_t* cap = calloc(1, sizeof(*cap));

    if (!cap) {
        return NULL;
    }

    media_policy_set_mic_mute(1);

    int vol_min = 0, vol_max = 15;
    media_policy_get_range(MEDIA_SCENARIO_RECORD MEDIA_POLICY_VOLUME,
                           &vol_min, &vol_max);
    media_policy_set_stream_volume(MEDIA_SCENARIO_RECORD, vol_max);

    media_policy_include("SelCap", "mic1", 1);

    cap->recorder = media_recorder_open(MEDIA_SOURCE_MIC);
    if (!cap->recorder) {
        syslog(LOG_ERR, "[%s] media_recorder_open failed\n", TAG);
        free(cap);
        return NULL;
    }

    /* Build options string for raw PCM capture */
    char opts[CAP_OPTIONS_LEN];

    snprintf(opts, sizeof(opts),
        "fmt=[rate=#%u,ch=#%u,bits=#%u,width=#2],enc=[keys=pcm,imin=#1920]",
        sample_rate, channels, bits_per_sample);

    syslog(LOG_INFO, "[%s] prepare opts: %s\n", TAG, opts);

    int ret = media_recorder_prepare(cap->recorder, NULL, opts);

    if (ret < 0) {
        syslog(LOG_ERR, "[%s] prepare failed: %d\n", TAG, ret);
        media_recorder_close(cap->recorder);
        free(cap);
        return NULL;
    }

    syslog(LOG_INFO,
        "[%s] opened (%uHz %uch %ubit)\n",
        TAG, sample_rate, channels, bits_per_sample);
    s_active_recorder = cap->recorder;
    return cap;
}

int audio_capture_start(audio_capture_t* cap)
{
    if (!cap || !cap->recorder) {
        return -EINVAL;
    }

    int ret = media_recorder_start(cap->recorder);

    if (ret < 0) {
        syslog(LOG_ERR, "[%s] start failed: %d\n", TAG, ret);
        return ret;
    }

    syslog(LOG_INFO, "[%s] capture started\n", TAG);
    return 0;
}

int audio_capture_read(audio_capture_t* cap, void* buf, size_t len)
{
    if (!cap || !cap->recorder || !buf || len == 0) {
        return -EINVAL;
    }

    ssize_t n = media_recorder_read_data(cap->recorder, buf, len);

    /* Apply gain to compensate for multi-channel DMIC mixing.
     * When 6 DMIC channels are configured but only 1 has a physical mic,
     * the mono downmix dilutes the signal. Amplify to restore level. */
#if AGENT_AUDIO_CAPTURE_GAIN > 1
    if (n > 0) {
        int16_t* samples = (int16_t*)buf;
        int sample_count = (int)n / 2;

        for (int i = 0; i < sample_count; i++) {
            int32_t amplified = (int32_t)samples[i] * AGENT_AUDIO_CAPTURE_GAIN;

            /* Clamp to 16-bit range to prevent overflow */
            if (amplified > 32767) {
                amplified = 32767;
            } else if (amplified < -32768) {
                amplified = -32768;
            }

            samples[i] = (int16_t)amplified;
        }
    }
#endif

    return (int)n;
}

void audio_capture_close(audio_capture_t* cap)
{
    if (!cap) {
        return;
    }

    if (cap->recorder) {
        int ret;

        /* media_recorder_stop() internally closes the audio stream
         * (af_stream_stop + af_stream_close).  media_recorder_close()
         * then tries af_stream_close again, which logs a harmless
         * "ERROR: status = 1" because the stream is already closed.
         *
         * This double-close warning is benign.  Removing the stop()
         * call is NOT safe — media_recorder_close() alone does not
         * properly release the pcm0c stream, leaving it in a stale
         * state that blocks the next recorder open. */
        ret = media_recorder_stop(cap->recorder);
        if (ret < 0) {
            syslog(LOG_WARNING, "[%s] media_recorder_stop: %d\n", TAG, ret);
        }

        /* Brief delay to let media framework's internal thread
         * finish processing the stop command before we close.
         * Without this, af_stream_close races with the recorder
         * thread's own close, causing "status = 1" errors on
         * BES platforms. */
        usleep(50 * 1000);

        ret = media_recorder_close(cap->recorder);
        if (ret < 0) {
            syslog(LOG_WARNING, "[%s] media_recorder_close: %d\n", TAG, ret);
        }

        cap->recorder = NULL;
        s_active_recorder = NULL;
    }

    free(cap);
}
