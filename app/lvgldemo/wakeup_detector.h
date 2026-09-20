/****************************************************************************
 * apps/examples/lvgldemo/wakeup_detector.h
 *
 * Voice wake word detector — uses TFLite Micro KWS engine to detect
 * the wake phrase "xiaoQxiaoQ!" and prints "Resume by xiaoQ!".
 *
 ****************************************************************************/

#ifndef WAKEUP_DETECTOR_H
#define WAKEUP_DETECTOR_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Wakeup action returned by the callback */
typedef enum {
    WAKEUP_ACTION_CONTINUE = 0,  /* Continue VAD after wake word */
    WAKEUP_ACTION_STOP,          /* Stop VAD after wake word (mic handed to cloud) */
} wakeup_action_t;

/* Callback invoked when a wake word is detected.
 * Returns the action to take after the callback returns. */
typedef wakeup_action_t (*wakeup_callback_t)(void);

int  wakeup_detector_init(void);
void wakeup_detector_start(void);
void wakeup_detector_stop(void);
void wakeup_detector_deinit(void);
bool wakeup_detector_is_running(void);

/* Register wake word callback (called from VAD thread) */
void wakeup_detector_set_callback(wakeup_callback_t cb);

/* Play a prompt sound (call from VAD thread callback only).
 * Stops the VAD recorder, plays the prompt, does NOT reopen recorder.
 * The VAD thread will reopen the recorder after the callback returns. */
void wakeup_detector_play_prompt(const char *path);

/* Request prompt playback from an external thread.
 * Signals the VAD thread to stop recorder, play the prompt, and reopen recorder. */
void wakeup_detector_request_prompt(const char *path);

#ifdef __cplusplus
}
#endif

#endif /* WAKEUP_DETECTOR_H */
