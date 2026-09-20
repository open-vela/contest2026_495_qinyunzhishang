############################################################################
# apps/examples/lvgldemo/Makefile
#
# Licensed to the Apache Software Foundation (ASF) under one or more
# contributor license agreements.  See the NOTICE file distributed with
# this work for additional information regarding copyright ownership.  The
# ASF licenses this file to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance with the
# License.  You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
# WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
# License for the specific language governing permissions and limitations
# under the License.
#
############################################################################

include $(APPDIR)/Make.defs

PROGNAME = lvgldemo
PRIORITY = $(CONFIG_EXAMPLES_LVGLDEMO_PRIORITY)
STACKSIZE = $(CONFIG_EXAMPLES_LVGLDEMO_STACKSIZE)
MODULE = $(CONFIG_EXAMPLES_LVGLDEMO)

# LVGL demo Example

MAINSRC = lvgldemo.c

CSRCS = lvgl_dispatch.c xiaoq_page.c i18n.c lang_dialog.c wifi_page.c recorder_page.c player_page.c ai_page.c camera_page.c camera_ai_page.c voice_assistant.c call_page.c calling_page.c bt_call_handler.c bt_debug_page.c meeting_asr.c meeting_page.c settings_page.c

ifeq ($(CONFIG_EXAMPLES_LVGLDEMO_WAKEUP_ENABLE),y)
CSRCS += wakeup_detector.cpp
endif

# Add include paths for DashScope ASR
CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/packages/ai_agent/include
CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/packages/ai_agent/src/voice
CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/packages/ai_agent/src/infra
CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/packages/ai_agent/src/channels
CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/netutils/cjson/cJSON
CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/crypto/mbedtls/mbedtls/include

include $(APPDIR)/Application.mk
