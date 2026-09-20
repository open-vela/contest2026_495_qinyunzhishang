# ##############################################################################
# apps/examples/lvgldemo/CMakeLists.txt
#
# Licensed to the Apache Software Foundation (ASF) under one or more contributor
# license agreements.  See the NOTICE file distributed with this work for
# additional information regarding copyright ownership.  The ASF licenses this
# file to you under the Apache License, Version 2.0 (the "License"); you may not
# use this file except in compliance with the License.  You may obtain a copy of
# the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
# WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
# License for the specific language governing permissions and limitations under
# the License.
#
# ##############################################################################

if(CONFIG_EXAMPLES_LVGLDEMO)

  set(LVGLDEMO_INCDIR)
  set(LVGLDEMO_SRCS)

  list(APPEND LVGLDEMO_INCDIR
    ${CMAKE_CURRENT_LIST_DIR}/../../packages/ai_agent/include
    ${CMAKE_CURRENT_LIST_DIR}/../../packages/ai_agent/src
    ${CMAKE_CURRENT_LIST_DIR}/../../packages/ai_agent/src/voice
    ${CMAKE_CURRENT_LIST_DIR}/../../packages/ai_agent/src/channels
    ${CMAKE_CURRENT_LIST_DIR}/../../packages/ai_agent/src/infra
    ${CMAKE_CURRENT_LIST_DIR}/../../netutils/cjson/cJSON
    ${CMAKE_CURRENT_LIST_DIR}/../../../include
    ${CMAKE_CURRENT_LIST_DIR}/../../../vendor/bes/framework/services/platform/hal
    ${CMAKE_CURRENT_LIST_DIR}/../../../vendor/bes/framework/services/platform/hal/best1700)

  set(LVGLDEMO_COMPILE_OPTIONS)

  list(APPEND LVGLDEMO_SRCS
    lvgldemo.c
    lvgl_dispatch.c
    xiaoq_page.c
    i18n.c
    lang_dialog.c
    menu_page.c
    wifi_page.c
    recorder_page.c
    player_page.c
    ai_page.c
    camera_page.c
    camera_ai_page.c
    icons/image_preview.c
    icons/mic_white.c
    icons/mic_blue.c
    icons/mic_green.c
    icons/camera_page_photo_on.c
    icons/camera_page_photo_off.c
    icons/camera_page_ai_on.c
    icons/camera_page_ai_off.c
    icons/up_up.c
    icons/menu_ai.c
    icons/menu_capture.c
    icons/menu_meeting.c
    icons/menu_player.c
    icons/menu_recorder.c
    icons/menu_setting.c
    icons/setting_language.c
    icons/setting_wifi.c
    call_page.c
    calling_page.c
    bt_call_handler.c
    bt_debug_page.c
    meeting_page.c
    meeting_asr.c
    meeting_feishu_sync.c
    camera_feishu_doc.c
    settings_page.c
    wakeup_detector.cpp)

  if(CONFIG_EXAMPLES_AI_AGENT_VELA)
    list(APPEND LVGLDEMO_SRCS voice_assistant.c)
  endif()

  nuttx_add_application(
    NAME
    lvgldemo
    PRIORITY
    ${CONFIG_EXAMPLES_LVGLDEMO_PRIORITY}
    STACKSIZE
    ${CONFIG_EXAMPLES_LVGLDEMO_STACKSIZE}
    MODULE
    ${CONFIG_EXAMPLES_LVGLDEMO}
    DEPENDS
    lvgl
    ${CONFIG_WIRELESS_WAPI+netutils}
    ${CONFIG_EXAMPLES_LVGLDEMO_WAKEUP_ENABLE+media}
    ${CONFIG_EXAMPLES_AI_AGENT_VELA+ai_agent}
    ${CONFIG_LIB_JPEG_TURBO+turbojpeg-static}
    INCLUDE_DIRECTORIES
    ${LVGLDEMO_INCDIR}
    ${CMAKE_CURRENT_LIST_DIR}/../../external/libjpeg-turbo/libjpeg-turbo
    COMPILE_FLAGS
    ${LVGLDEMO_COMPILE_OPTIONS}
    SRCS
    ${LVGLDEMO_SRCS})
endif()
