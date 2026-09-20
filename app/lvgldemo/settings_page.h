/****************************************************************************
 * apps/examples/lvgldemo/settings_page.h
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

#ifndef SETTINGS_PAGE_H
#define SETTINGS_PAGE_H

#include <lvgl/lvgl.h>

/* 设置页面对象（全局，由 lvgldemo.c 创建和管理） */
extern lv_obj_t *settings_screen;

/* 创建设置页面（功能菜单页的子页面）
 * 包含设置项列表，当前项：
 *   WiFi
 * 右滑返回功能菜单页；点击 WiFi 项进入 wifi_page。
 */
void create_settings_page(lv_obj_t *parent);
void settings_page_deinit(void);

#endif /* SETTINGS_PAGE_H */
