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

/*
 * timezone.c — Timezone utility functions for AI Agent
 *
 * Reads timezone configuration from /data/misc/tz_config file
 * and provides helper functions for local time conversion.
 */

#include "agent_config.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

/* Read timezone string from /data/misc/tz_config file.
 * Falls back to AGENT_TIMEZONE if file doesn't exist or is empty.
 * Returns pointer to a static buffer. */
const char *agent_get_timezone(void)
{
    static char tz_buf[32];
    FILE *f = fopen(AGENT_TZ_CONFIG_FILE, "r");
    if (f) {
        if (fgets(tz_buf, sizeof(tz_buf), f)) {
            fclose(f);
            /* Strip trailing newline / whitespace */
            char *nl = strchr(tz_buf, '\n');
            if (nl) *nl = '\0';
            nl = strchr(tz_buf, '\r');
            if (nl) *nl = '\0';
            if (tz_buf[0] != '\0') return tz_buf;
        } else {
            fclose(f);
        }
    }
    return AGENT_TIMEZONE;
}

/* Parse UTC offset (in seconds) from the current timezone string.
 * Reads from /data/misc/tz_config at call time, so runtime changes
 * are picked up automatically.  Falls back to AGENT_TIMEZONE.
 *
 * POSIX TZ format: "STDoffset" where offset sign is INVERTED:
 *   "CST-8" → UTC+8, "EST5" → UTC-5.
 * Returns 0 if parsing fails (falls back to UTC). */
int agent_tz_offset_sec(void)
{
    const char *tz = agent_get_timezone();
    /* Skip alphabetic timezone abbreviation */
    while (*tz && ((*tz >= 'A' && *tz <= 'Z') || (*tz >= 'a' && *tz <= 'z')))
        tz++;
    if (*tz == '\0') return 0;
    /* POSIX: positive number = west of Greenwich = UTC-negative */
    int val = 0;
    int sign = 1;
    if (*tz == '-') { sign = -1; tz++; }
    else if (*tz == '+') { tz++; }
    while (*tz >= '0' && *tz <= '9') { val = val * 10 + (*tz - '0'); tz++; }
    if (*tz == ':') {
        tz++;
        int mm = 0;
        while (*tz >= '0' && *tz <= '9') { mm = mm * 10 + (*tz - '0'); tz++; }
        val = val * 3600 + mm * 60;
        if (*tz == ':') {
            tz++;
            int ss = 0;
            while (*tz >= '0' && *tz <= '9') { ss = ss * 10 + (*tz - '0'); tz++; }
            val += ss;
        }
    } else {
        val *= 3600;
    }
    return -sign * val;
}

/* Get current local time as struct tm.
 * Shortcut for: time() → +agent_tz_offset_sec() → gmtime_r().
 * Avoids NuttX localtime_r zoneinfo lookup issues. */
struct tm agent_localtime(void)
{
    time_t now = time(NULL);
    time_t local_epoch = now + agent_tz_offset_sec();
    struct tm tm;
    gmtime_r(&local_epoch, &tm);
    return tm;
}
