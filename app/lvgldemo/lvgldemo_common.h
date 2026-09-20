/*
 * apps/examples/lvgldemo/lvgldemo_common.h
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.  The
 * ASF licenses this file to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance with the
 * License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 ****************************************************************************/

#ifndef LVGLDEMO_COMMON_H
#define LVGLDEMO_COMMON_H

#include <nuttx/config.h>
#include <lvgl/lvgl.h>

/* Shared screen pointers - defined in lvgldemo.c */

extern lv_obj_t *main_screen;
extern lv_obj_t *menu_screen;
extern lv_obj_t *wifi_screen;
extern lv_obj_t *recorder_screen;
extern lv_obj_t *player_screen;
extern lv_obj_t *ai_screen;
extern lv_obj_t *camera_screen;
extern lv_obj_t *meeting_screen;
extern lv_obj_t *settings_screen;

/* Returns the currently free heap size in bytes.
 * Works in both builtin-malloc and clib-malloc LVGL builds (clib mode
 * falls back to mallinfo because lv_mem_monitor is a no-op there). */
size_t lvgldemo_get_free_heap(void);

#endif /* LVGLDEMO_COMMON_H */
