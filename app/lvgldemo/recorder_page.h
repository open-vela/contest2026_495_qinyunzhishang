/****************************************************************************
 * apps/examples/lvgldemo/recorder_page.h
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

#ifndef RECORDER_PAGE_H
#define RECORDER_PAGE_H

#include <lvgl/lvgl.h>

#define RECORDER_PATH_PREFIX "/emmc/audio/"
#define RECORDER_MAX_FILES  5

void create_recorder_page(lv_obj_t *parent);
void recorder_page_deinit(void);
int recorder_open(void);
int recorder_close(void);

/* 查询录音是否进行中（含暂停状态），供 idle 超时检测使用 */
bool recorder_is_recording(void);

#endif /* RECORDER_PAGE_H */
