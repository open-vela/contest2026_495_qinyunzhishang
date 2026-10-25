/****************************************************************************
 * apps/examples/lvgldemo/i18n.c
 *
 * 中英文切换模块 - 字符串表 + 语言状态机 + 字体管理
 *
 * 方案 B（字符串表）+ 局部方案 C（主页原地刷新）
 *
 ****************************************************************************/

#include "i18n.h"
#include "lvgldemo_common.h"
#include "menu_page.h"
#include "settings_page.h"
#include "wifi_page.h"
#include "ai_page.h"
#include "camera_page.h"
#include "meeting_page.h"
#include "recorder_page.h"
#include "player_page.h"
#include "call_page.h"
#include "calling_page.h"
#include "camera_ai_page.h"

#include <nuttx/config.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <syslog.h>
#include "dashscope_asr.h"
#include "dashscope_tts.h"
#include "agent_config.h"
#include "voice_assistant.h"

/* 字体路径（与其他页面一致） */
#define I18N_FONT_PATH        "/emmc/font/MiSans-Normal.ttf"
#define I18N_FONT_SIZE        28
#define I18N_FONT_SIZE_SMALL  24   /* 小字号，用于正文/UI提示（参考 camera_page 24px） */

/* 语言配置文件 */
#define LANG_CONFIG_FILE      "/emmc/lang_config"

/* 语言变更回调最大数量（主页等常驻页注册） */
#define MAX_LANG_CBS          4

/* 英文月份缩写表 */
const char *const i18n_en_months[12] = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun",
    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
};

/* 当前语言（开机时从文件读取，默认中文） */
static lang_t g_current_lang = LANG_ZH_CN;

/* 共享的中文 FreeType 字体（28px，懒加载，用于标题） */
static lv_font_t *s_cjk_font = NULL;
/* 共享的中文 FreeType 字体（24px，懒加载，用于正文/UI提示） */
static lv_font_t *s_cjk_font_small = NULL;

/* 语言变更回调列表 */
static lang_changed_cb s_lang_cbs[MAX_LANG_CBS];
static bool s_lang_pending = false;  /* 延迟广播标记 */

/* 字符串表（两套语言，编译期确定，存 rodata） */
static const char *const s_strings[LANG_COUNT][STR_COUNT] = {
    [LANG_ZH_CN] = {
        /* 主界面 */
        [STR_HOME_DATE_FMT]       = "%d月%d日",
        [STR_HOME_WEEK_SUN]       = "星期日",
        [STR_HOME_WEEK_MON]       = "星期一",
        [STR_HOME_WEEK_TUE]       = "星期二",
        [STR_HOME_WEEK_WED]       = "星期三",
        [STR_HOME_WEEK_THU]       = "星期四",
        [STR_HOME_WEEK_FRI]       = "星期五",
        [STR_HOME_WEEK_SAT]       = "星期六",
        /* 菜单页 */
        [STR_MENU_AI]             = "语音对话",
        [STR_MENU_MEETING]        = "会议模式",
        [STR_MENU_CAMERA]         = "图文智录",
        [STR_MENU_RECORDER]       = "录音",
        [STR_MENU_PLAYER]         = "媒体播放器",
        [STR_MENU_SETTINGS]       = "设置",
        /* 设置页 */
        [STR_SETTINGS_WIFI]       = "WiFi",
        [STR_SETTINGS_LANGUAGE]   = "语言",
        /* 语言选择弹窗 */
        [STR_LANG_DIALOG_TITLE]   = "语言",
        [STR_LANG_ZH_CN_LABEL]    = "简体中文",
        [STR_LANG_EN_LABEL]       = "English",
        [STR_LANG_JA_LABEL]       = "日本語",
        [STR_LANG_DE_LABEL]       = "Deutsch",
        [STR_LANG_FR_LABEL]       = "Français",
        [STR_LANG_RU_LABEL]       = "Русский",
        [STR_LANG_CONFIRM]        = "确认",
        [STR_LANG_SWITCH_OK]      = "切换成功",
        [STR_LANG_SWITCH_FAIL]    = "切换失败",
        [STR_LANG_ALREADY_ZH]     = "当前已是简体中文",
        [STR_LANG_ALREADY_EN]     = "当前已是English",
        [STR_LANG_ALREADY_JA]     = "当前已是日本語",
        /* 拦截提示 */
        [STR_BLOCK_CALL]          = "通话中无法切换语言",
        [STR_BLOCK_RECORDER]      = "录音中无法切换语言",
        [STR_BLOCK_MEETING]       = "会议中无法切换语言",
        [STR_BLOCK_OK]            = "确定",
        /* AI 页 */
        [STR_AI_TITLE]            = "语音对话",
        [STR_AI_WAITING]          = "等待唤醒...",
        [STR_AI_NOT_CONNECTED]    = "AI: 未连接",
        [STR_AI_CONNECTED]        = "AI: 已连接",
        [STR_AI_DISCONNECTED]     = "AI: 未连接",
        [STR_AI_CONNECTING]       = "连接中...",
        [STR_AI_SOCKET_ERROR]     = "Socket 错误",
        [STR_AI_CONNECT_FAIL]     = "连接失败",
        [STR_AI_HANDSHAKE_FAIL]   = "WS 握手失败",
        [STR_AI_LISTENING]        = "正在监听...",
        [STR_AI_REPLYING]         = "AI回复中...",
        [STR_AI_NET_CONNECTING]   = "网络连接中...",
        [STR_AI_NET_DISCONNECTED] = "网络已断开",
        [STR_FEISHU_MSG_FILTER]   = "消息筛选",
        [STR_FEISHU_CONV]         = "飞书对话",
        [STR_FEISHU_DOC_QUERY]    = "文档查询",
        [STR_AI_WAKEWORD_USER]    = "小Q，小Q!",
        [STR_AI_WAKEWORD_REPLY]   = "你好，请说！",
        /* Camera 页 */
        [STR_CAM_TITLE]           = "图文智录",
        [STR_CAM_READY]           = "就绪",
        [STR_CAM_CAPTURING]       = "拍照中...",
        [STR_CAM_CAPTURE_FAIL]    = "拍照失败，请重试",
        [STR_CAM_PLEASE_CAPTURE]  = "请先拍照，谢谢",
        [STR_CAM_AI_STARTING]     = "AI启动中，请重试",
        [STR_CAM_AI_CONNECTING]   = "AI连接中，请重试",
        [STR_CAM_KEEP_STEADY]     = "请保持相机平稳...",
        [STR_CAM_PROCESSING]      = "处理中，请稍候...",
        /* Camera AI 页 */
        [STR_CAI_TITLE]           = "图文智录",
        [STR_CAI_ANALYZING]       = "正在分析图片...",
        /* Meeting 页 */
        [STR_MEET_TITLE_IDLE]     = "会议模式",
        [STR_MEET_TITLE]          = "会议记录",
        [STR_MEET_RECORDING]      = "会议录音中...",
        [STR_MEET_RECORDING_PROMPT] = "会议录音中，请开始发言",
        [STR_MEET_DONE]           = "转写完成，右滑返回",
        [STR_MEET_DONE_FEISHU_FAIL] = "转写完成，飞书保存失败",
        [STR_MEET_GENERATING]     = "正在生成会议记录，请稍候...",
        [STR_MEET_START_FAIL]     = "转写启动失败",
        [STR_MEET_SPEAKER_SYSTEM] = "系统",
        [STR_MEET_SUMMARY_TITLE]  = "【会议纪要】",
        [STR_MEET_STOP_FIRST]     = "请先停止录制",
        [STR_MEET_PAUSED]         = "已暂停",
        [STR_MEET_PAUSE_FAIL]     = "操作失败，请重试",
        [STR_MEET_CONN_HINT]      = "正在连接服务器，请开始发言...",
        /* Recorder 页 */
        [STR_REC_TITLE]           = "录音",
        [STR_REC_DELETE_TITLE]    = "删除文件?",
        [STR_REC_DELETE]          = "删除",
        [STR_REC_CANCEL]          = "取消",
        [STR_REC_EMPTY]           = "暂无录音",
        [STR_REC_TOTAL_FMT]       = "共 %d 个",
        /* Player 页 */
        [STR_PLAY_TITLE]          = "播放器",
        [STR_PLAY_NO_FILE]        = "无文件",
        [STR_PLAY_EMPTY]          = "暂无音频",
        [STR_PLAY_ERROR]          = "播放错误",
        [STR_PLAY_TOTAL_FMT]      = "共 %d 个",
        /* Call 页 */
        [STR_CALL_INCOMING]       = "来电",
        [STR_CALL_DECLINE]        = "拒接",
        [STR_CALL_ACCEPT]         = "接听",
        [STR_CALL_UNKNOWN]        = "未知号码",
        /* Calling 页 */
        [STR_CALLING_ON_CALL]     = "通话中",
        [STR_CALLING_DIALING]     = "拨号中...",
        [STR_CALLING_RINGING]     = "响铃中...",
        [STR_CALLING_CALLING]     = "呼叫中...",
    },
    [LANG_EN] = {
        /* 主界面 */
        [STR_HOME_DATE_FMT]       = "%s %d",   /* 用 i18n_en_months[mon] 填充 */
        [STR_HOME_WEEK_SUN]       = "Sun",
        [STR_HOME_WEEK_MON]       = "Mon",
        [STR_HOME_WEEK_TUE]       = "Tue",
        [STR_HOME_WEEK_WED]       = "Wed",
        [STR_HOME_WEEK_THU]       = "Thu",
        [STR_HOME_WEEK_FRI]       = "Fri",
        [STR_HOME_WEEK_SAT]       = "Sat",
        /* 菜单页 */
        [STR_MENU_AI]             = "AI Assistant",
        [STR_MENU_MEETING]        = "Meeting",
        [STR_MENU_CAMERA]         = "AI Camera",
        [STR_MENU_RECORDER]       = "Recorder",
        [STR_MENU_PLAYER]         = "Player",
        [STR_MENU_SETTINGS]       = "Settings",
        /* 设置页 */
        [STR_SETTINGS_WIFI]       = "WiFi",
        [STR_SETTINGS_LANGUAGE]   = "Language",
        /* 语言选择弹窗 */
        [STR_LANG_DIALOG_TITLE]   = "Language",
        [STR_LANG_ZH_CN_LABEL]    = "简体中文",
        [STR_LANG_EN_LABEL]       = "English",
        [STR_LANG_JA_LABEL]       = "日本語",
        [STR_LANG_DE_LABEL]       = "Deutsch",
        [STR_LANG_FR_LABEL]       = "Français",
        [STR_LANG_RU_LABEL]       = "Русский",
        [STR_LANG_CONFIRM]        = "Confirm",
        [STR_LANG_SWITCH_OK]      = "Switch success",
        [STR_LANG_SWITCH_FAIL]    = "Failed",
        [STR_LANG_ALREADY_ZH]     = "Already Simplified Chinese",
        [STR_LANG_ALREADY_EN]     = "Already English",
        [STR_LANG_ALREADY_JA]     = "Already Japanese",
        /* 拦截提示 */
        [STR_BLOCK_CALL]          = "Cannot switch during call",
        [STR_BLOCK_RECORDER]      = "Cannot switch during recording",
        [STR_BLOCK_MEETING]       = "Cannot switch during meeting",
        [STR_BLOCK_OK]            = "OK",
        /* AI 页 */
        [STR_AI_TITLE]            = "AI Assistant",
        [STR_AI_WAITING]          = "Waiting...",
        [STR_AI_NOT_CONNECTED]    = "AI: Not connected",
        [STR_AI_CONNECTED]        = "AI: Connected",
        [STR_AI_DISCONNECTED]     = "AI: Disconnected",
        [STR_AI_CONNECTING]       = "Connecting...",
        [STR_AI_SOCKET_ERROR]     = "Socket error",
        [STR_AI_CONNECT_FAIL]     = "Connect failed",
        [STR_AI_HANDSHAKE_FAIL]   = "WS handshake failed",
        [STR_AI_LISTENING]        = "Listening...",
        [STR_AI_REPLYING]         = "AI replying...",
        [STR_AI_NET_CONNECTING]   = "Connecting network...",
        [STR_AI_NET_DISCONNECTED] = "Network disconnected",
        [STR_FEISHU_MSG_FILTER]   = "Msg filter",
        [STR_FEISHU_CONV]         = "Lark chat",
        [STR_FEISHU_DOC_QUERY]    = "Doc query",
        [STR_AI_WAKEWORD_USER]    = "Hi buddy!",
        [STR_AI_WAKEWORD_REPLY]   = "Hello, speaking!",
        /* Camera 页 */
        [STR_CAM_TITLE]           = "AI Camera",
        [STR_CAM_READY]           = "Ready",
        [STR_CAM_CAPTURING]       = "Capturing...",
        [STR_CAM_CAPTURE_FAIL]    = "Capture failed, please retry",
        [STR_CAM_PLEASE_CAPTURE]  = "Please capture first",
        [STR_CAM_AI_STARTING]     = "AI starting, retry",
        [STR_CAM_AI_CONNECTING]   = "AI connecting, retry",
        [STR_CAM_KEEP_STEADY]     = "Keep the camera steady...",
        [STR_CAM_PROCESSING]      = "Processing...",
        /* Camera AI 页 */
        [STR_CAI_TITLE]           = "AI Camera",
        [STR_CAI_ANALYZING]       = "Analyzing image...",
        /* Meeting 页 */
        [STR_MEET_TITLE_IDLE]     = "Meeting",
        [STR_MEET_TITLE]          = "Meeting Notes",
        [STR_MEET_RECORDING]      = "Recording...",
        [STR_MEET_RECORDING_PROMPT] = "Recording, please speak",
        [STR_MEET_DONE]           = "Done, swipe right",
        [STR_MEET_DONE_FEISHU_FAIL] = "Done, Lark save failed",
        [STR_MEET_GENERATING]     = "Generating notes...",
        [STR_MEET_START_FAIL]     = "Transcription start failed",
        [STR_MEET_SPEAKER_SYSTEM] = "System",
        [STR_MEET_SUMMARY_TITLE]  = "[Meeting Notes]",
        [STR_MEET_STOP_FIRST]     = "Please stop recording first",
        [STR_MEET_PAUSED]         = "Paused",
        [STR_MEET_PAUSE_FAIL]     = "Operation failed, retry",
        [STR_MEET_CONN_HINT]      = "Connecting to server, please speak...",
        /* Recorder 页 */
        [STR_REC_TITLE]           = "Recorder",
        [STR_REC_DELETE_TITLE]    = "Delete File?",
        [STR_REC_DELETE]          = "Delete",
        [STR_REC_CANCEL]          = "Cancel",
        [STR_REC_EMPTY]           = "No recordings",
        [STR_REC_TOTAL_FMT]       = "Total: %d",
        /* Player 页 */
        [STR_PLAY_TITLE]          = "Player",
        [STR_PLAY_NO_FILE]        = "No file",
        [STR_PLAY_EMPTY]          = "No audio files",
        [STR_PLAY_ERROR]          = "Play error",
        [STR_PLAY_TOTAL_FMT]      = "Total: %d",
        /* Call 页 */
        [STR_CALL_INCOMING]       = "Incoming Call",
        [STR_CALL_DECLINE]        = "Decline",
        [STR_CALL_ACCEPT]         = "Accept",
        [STR_CALL_UNKNOWN]        = "Unknown",
        /* Calling 页 */
        [STR_CALLING_ON_CALL]     = "On Call",
        [STR_CALLING_DIALING]     = "Dialing...",
        [STR_CALLING_RINGING]    = "Ringing...",
        [STR_CALLING_CALLING]    = "Calling...",
    },
    [LANG_JA] = {
        /* 主界面 */
        [STR_HOME_DATE_FMT]       = "%d月%d日",
        [STR_HOME_WEEK_SUN]       = "日曜日",
        [STR_HOME_WEEK_MON]       = "月曜日",
        [STR_HOME_WEEK_TUE]       = "火曜日",
        [STR_HOME_WEEK_WED]       = "水曜日",
        [STR_HOME_WEEK_THU]       = "木曜日",
        [STR_HOME_WEEK_FRI]       = "金曜日",
        [STR_HOME_WEEK_SAT]       = "土曜日",
        /* 菜单页 */
        [STR_MENU_AI]             = "音声対話",
        [STR_MENU_MEETING]        = "会議モード",
        [STR_MENU_CAMERA]         = "AIカメラ",
        [STR_MENU_RECORDER]       = "レコーダー",
        [STR_MENU_PLAYER]         = "プレーヤー",
        [STR_MENU_SETTINGS]       = "設定",
        /* 设置页 */
        [STR_SETTINGS_WIFI]       = "WiFi",
        [STR_SETTINGS_LANGUAGE]   = "言語",
        /* 语言选择弹窗 */
        [STR_LANG_DIALOG_TITLE]   = "言語",
        [STR_LANG_ZH_CN_LABEL]    = "简体中文",
        [STR_LANG_EN_LABEL]       = "English",
        [STR_LANG_JA_LABEL]       = "日本語",
        [STR_LANG_DE_LABEL]       = "Deutsch",
        [STR_LANG_FR_LABEL]       = "Français",
        [STR_LANG_RU_LABEL]       = "Русский",
        [STR_LANG_CONFIRM]        = "確認",
        [STR_LANG_SWITCH_OK]      = "切り替え完了",
        [STR_LANG_SWITCH_FAIL]    = "切り替え失敗",
        [STR_LANG_ALREADY_ZH]     = "現在簡体中文です",
        [STR_LANG_ALREADY_EN]     = "現在Englishです",
        [STR_LANG_ALREADY_JA]     = "現在日本語です",
        /* 拦截提示 */
        [STR_BLOCK_CALL]          = "通話中は言語を切替できません",
        [STR_BLOCK_RECORDER]      = "録音中は言語を切替できません",
        [STR_BLOCK_MEETING]       = "会議中は言語を切替できません",
        [STR_BLOCK_OK]            = "OK",
        /* AI 页 */
        [STR_AI_TITLE]            = "音声対話",
        [STR_AI_WAITING]          = "待機中...",
        [STR_AI_NOT_CONNECTED]    = "AI: 未接続",
        [STR_AI_CONNECTED]        = "AI: 接続済み",
        [STR_AI_DISCONNECTED]     = "AI: 未接続",
        [STR_AI_CONNECTING]       = "接続中...",
        [STR_AI_SOCKET_ERROR]     = "ソケットエラー",
        [STR_AI_CONNECT_FAIL]     = "接続失敗",
        [STR_AI_HANDSHAKE_FAIL]   = "WSハンドシェイク失敗",
        [STR_AI_LISTENING]        = "音声を聞いています...",
        [STR_AI_REPLYING]         = "AI応答中...",
        [STR_AI_NET_CONNECTING]   = "ネットワーク接続中...",
        [STR_AI_NET_DISCONNECTED] = "ネットワーク未接続",
        [STR_FEISHU_MSG_FILTER]   = "メッセージ絞込",
        [STR_FEISHU_CONV]         = "Lark対話",
        [STR_FEISHU_DOC_QUERY]    = "ドキュメント検索",
        [STR_AI_WAKEWORD_USER]    = "小Q、小Q!",
        [STR_AI_WAKEWORD_REPLY]   = "こんにちは、どうぞ！",
        /* Camera 页 */
        [STR_CAM_TITLE]           = "AIカメラ",
        [STR_CAM_READY]           = "準備完了",
        [STR_CAM_CAPTURING]       = "撮影中...",
        [STR_CAM_CAPTURE_FAIL]    = "撮影に失敗しました。もう一度お試しください",
        [STR_CAM_PLEASE_CAPTURE]  = "先に撮影してください",
        [STR_CAM_AI_STARTING]     = "AI起動中です。もう一度お試しください",
        [STR_CAM_AI_CONNECTING]   = "AI接続中です。もう一度お試しください",
        [STR_CAM_KEEP_STEADY]     = "カメラを安定に保ってください...",
        [STR_CAM_PROCESSING]      = "処理中です。お待ちください...",
        /* Camera AI 页 */
        [STR_CAI_TITLE]           = "AIカメラ",
        [STR_CAI_ANALYZING]       = "画像を分析中...",
        /* Meeting 页 */
        [STR_MEET_TITLE_IDLE]     = "会議モード",
        [STR_MEET_TITLE]          = "会議録",
        [STR_MEET_RECORDING]      = "会議録音中...",
        [STR_MEET_RECORDING_PROMPT] = "会議を録音しています。話してください",
        [STR_MEET_DONE]           = "文字起こし完了。右にスワイプして戻る",
        [STR_MEET_DONE_FEISHU_FAIL] = "文字起こし完了。Lark保存失敗",
        [STR_MEET_GENERATING]     = "会議録を生成中です。お待ちください...",
        [STR_MEET_START_FAIL]     = "文字起こし開始失敗",
        [STR_MEET_SPEAKER_SYSTEM] = "システム",
        [STR_MEET_SUMMARY_TITLE]  = "【会議録】",
        [STR_MEET_STOP_FIRST]     = "先に録音を停止してください",
        [STR_MEET_PAUSED]         = "一時停止中",
        [STR_MEET_PAUSE_FAIL]     = "操作に失敗しました。もう一度お試しください",
        [STR_MEET_CONN_HINT]      = "サーバーに接続中です。話してください...",
        /* Recorder 页 */
        [STR_REC_TITLE]           = "レコーダー",
        [STR_REC_DELETE_TITLE]    = "ファイルを削除しますか？",
        [STR_REC_DELETE]          = "削除",
        [STR_REC_CANCEL]          = "キャンセル",
        [STR_REC_EMPTY]           = "録音がありません",
        [STR_REC_TOTAL_FMT]       = "合計 %d 件",
        /* Player 页 */
        [STR_PLAY_TITLE]          = "プレーヤー",
        [STR_PLAY_NO_FILE]        = "ファイルなし",
        [STR_PLAY_EMPTY]          = "音声がありません",
        [STR_PLAY_ERROR]          = "再生エラー",
        [STR_PLAY_TOTAL_FMT]      = "合計 %d 件",
        /* Call 页 */
        [STR_CALL_INCOMING]       = "着信",
        [STR_CALL_DECLINE]        = "拒否",
        [STR_CALL_ACCEPT]         = "応答",
        [STR_CALL_UNKNOWN]        = "不明な番号",
        /* Calling 页 */
        [STR_CALLING_ON_CALL]     = "通話中",
        [STR_CALLING_DIALING]     = "発信中...",
        [STR_CALLING_RINGING]    = "呼び出し中...",
        [STR_CALLING_CALLING]    = "通話中...",
    },
    [LANG_DE] = {
        /* 主界面 */
        [STR_HOME_DATE_FMT]       = "%d.%d.",
        [STR_HOME_WEEK_SUN]       = "So",
        [STR_HOME_WEEK_MON]       = "Mo",
        [STR_HOME_WEEK_TUE]       = "Di",
        [STR_HOME_WEEK_WED]       = "Mi",
        [STR_HOME_WEEK_THU]       = "Do",
        [STR_HOME_WEEK_FRI]       = "Fr",
        [STR_HOME_WEEK_SAT]       = "Sa",
        /* 菜单页 */
        [STR_MENU_AI]             = "Sprachdialog",
        [STR_MENU_MEETING]        = "Besprechung",
        [STR_MENU_CAMERA]         = "KI-Kamera",
        [STR_MENU_RECORDER]       = "Recorder",
        [STR_MENU_PLAYER]         = "Medienplayer",
        [STR_MENU_SETTINGS]       = "Einstellungen",
        /* 设置页 */
        [STR_SETTINGS_WIFI]       = "WLAN",
        [STR_SETTINGS_LANGUAGE]   = "Sprache",
        /* 语言选择弹窗 */
        [STR_LANG_DIALOG_TITLE]   = "Sprache",
        [STR_LANG_ZH_CN_LABEL]    = "简体中文",
        [STR_LANG_EN_LABEL]       = "English",
        [STR_LANG_JA_LABEL]       = "日本語",
        [STR_LANG_DE_LABEL]       = "Deutsch",
        [STR_LANG_FR_LABEL]       = "Français",
        [STR_LANG_RU_LABEL]       = "Русский",
        [STR_LANG_CONFIRM]        = "Bestätigen",
        [STR_LANG_SWITCH_OK]      = "Sprache gewechselt",
        [STR_LANG_SWITCH_FAIL]    = "Fehlgeschlagen",
        [STR_LANG_ALREADY_ZH]     = "Bereits vereinfachtes Chinesisch",
        [STR_LANG_ALREADY_EN]     = "Bereits Englisch",
        [STR_LANG_ALREADY_JA]     = "Bereits Japanisch",
        /* 拦截提示 */
        [STR_BLOCK_CALL]          = "Wechsel während Anruf nicht möglich",
        [STR_BLOCK_RECORDER]      = "Wechsel während Aufnahme nicht möglich",
        [STR_BLOCK_MEETING]       = "Wechsel während Besprechung nicht möglich",
        [STR_BLOCK_OK]            = "OK",
        /* AI 页 */
        [STR_AI_TITLE]            = "Sprachdialog",
        [STR_AI_WAITING]          = "Warten...",
        [STR_AI_NOT_CONNECTED]    = "KI: Nicht verbunden",
        [STR_AI_CONNECTED]        = "KI: Verbunden",
        [STR_AI_DISCONNECTED]     = "KI: Getrennt",
        [STR_AI_CONNECTING]       = "Verbinde...",
        [STR_AI_SOCKET_ERROR]     = "Socket-Fehler",
        [STR_AI_CONNECT_FAIL]     = "Verbindung fehlgeschlagen",
        [STR_AI_HANDSHAKE_FAIL]   = "WS-Handshake fehlgeschlagen",
        [STR_AI_LISTENING]        = "Ich höre zu...",
        [STR_AI_REPLYING]         = "KI antwortet...",
        [STR_AI_NET_CONNECTING]   = "Netzwerk wird verbunden...",
        [STR_AI_NET_DISCONNECTED] = "Netzwerk getrennt",
        [STR_FEISHU_MSG_FILTER]   = "Nachr.-Filter",
        [STR_FEISHU_CONV]         = "Lark-Chat",
        [STR_FEISHU_DOC_QUERY]    = "Dok-Suche",
        [STR_AI_WAKEWORD_USER]    = "Hi buddy!",
        [STR_AI_WAKEWORD_REPLY]  = "Hallo, ich höre zu!",
        /* Camera 页 */
        [STR_CAM_TITLE]           = "KI-Kamera",
        [STR_CAM_READY]           = "Bereit",
        [STR_CAM_CAPTURING]       = "Aufnahme...",
        [STR_CAM_CAPTURE_FAIL]    = "Aufnahme fehlgeschlagen, erneut versuchen",
        [STR_CAM_PLEASE_CAPTURE]  = "Bitte zuerst aufnehmen",
        [STR_CAM_AI_STARTING]     = "KI startet, erneut versuchen",
        [STR_CAM_AI_CONNECTING]   = "KI verbindet, erneut versuchen",
        [STR_CAM_KEEP_STEADY]     = "Kamera ruhig halten...",
        [STR_CAM_PROCESSING]      = "Verarbeitung, bitte warten...",
        /* Camera AI 页 */
        [STR_CAI_TITLE]           = "KI-Kamera",
        [STR_CAI_ANALYZING]       = "Bild wird analysiert...",
        /* Meeting 页 */
        [STR_MEET_TITLE_IDLE]     = "Besprechung",
        [STR_MEET_TITLE]          = "Besprechungsnotizen",
        [STR_MEET_RECORDING]      = "Aufnahme läuft...",
        [STR_MEET_RECORDING_PROMPT] = "Aufnahme läuft, bitte sprechen",
        [STR_MEET_DONE]           = "Fertig, nach rechts wischen",
        [STR_MEET_DONE_FEISHU_FAIL] = "Fertig, Lark-Speichern fehlgeschlagen",
        [STR_MEET_GENERATING]     = "Notizen werden erstellt...",
        [STR_MEET_START_FAIL]     = "Übertragung Start fehlgeschlagen",
        [STR_MEET_SPEAKER_SYSTEM] = "System",
        [STR_MEET_SUMMARY_TITLE]  = "【Besprechungsnotizen】",
        [STR_MEET_STOP_FIRST]     = "Bitte zuerst Aufnahme stoppen",
        [STR_MEET_PAUSED]         = "Pausiert",
        [STR_MEET_PAUSE_FAIL]     = "Vorgang fehlgeschlagen, erneut versuchen",
        [STR_MEET_CONN_HINT]      = "Server wird verbunden, bitte sprechen...",
        /* Recorder 页 */
        [STR_REC_TITLE]           = "Recorder",
        [STR_REC_DELETE_TITLE]    = "Datei löschen?",
        [STR_REC_DELETE]          = "Löschen",
        [STR_REC_CANCEL]          = "Abbrechen",
        [STR_REC_EMPTY]           = "Keine Aufnahmen",
        [STR_REC_TOTAL_FMT]       = "Gesamt: %d",
        /* Player 页 */
        [STR_PLAY_TITLE]          = "Medienplayer",
        [STR_PLAY_NO_FILE]        = "Keine Datei",
        [STR_PLAY_EMPTY]          = "Keine Audiodateien",
        [STR_PLAY_ERROR]          = "Wiedergabefehler",
        [STR_PLAY_TOTAL_FMT]      = "Gesamt: %d",
        /* Call 页 */
        [STR_CALL_INCOMING]       = "Eingehender Anruf",
        [STR_CALL_DECLINE]        = "Ablehnen",
        [STR_CALL_ACCEPT]         = "Annehmen",
        [STR_CALL_UNKNOWN]        = "Unbekannte Nummer",
        /* Calling 页 */
        [STR_CALLING_ON_CALL]     = "Im Gespräch",
        [STR_CALLING_DIALING]     = "Wählt...",
        [STR_CALLING_RINGING]    = "Klingelt...",
        [STR_CALLING_CALLING]    = "Verbindet...",
    },
    [LANG_FR] = {
        /* 主界面 */
        [STR_HOME_DATE_FMT]       = "%d/%d",
        [STR_HOME_WEEK_SUN]       = "Dim",
        [STR_HOME_WEEK_MON]       = "Lun",
        [STR_HOME_WEEK_TUE]       = "Mar",
        [STR_HOME_WEEK_WED]       = "Mer",
        [STR_HOME_WEEK_THU]       = "Jeu",
        [STR_HOME_WEEK_FRI]       = "Ven",
        [STR_HOME_WEEK_SAT]       = "Sam",
        /* 菜单页 */
        [STR_MENU_AI]             = "Chat vocal",
        [STR_MENU_MEETING]        = "Réunion",
        [STR_MENU_CAMERA]         = "Caméra IA",
        [STR_MENU_RECORDER]       = "Enregistreur",
        [STR_MENU_PLAYER]         = "Lecteur",
        [STR_MENU_SETTINGS]       = "Paramètres",
        /* 设置页 */
        [STR_SETTINGS_WIFI]       = "Wi-Fi",
        [STR_SETTINGS_LANGUAGE]   = "Langue",
        /* 语言选择弹窗 */
        [STR_LANG_DIALOG_TITLE]   = "Langue",
        [STR_LANG_ZH_CN_LABEL]    = "简体中文",
        [STR_LANG_EN_LABEL]       = "English",
        [STR_LANG_JA_LABEL]       = "日本語",
        [STR_LANG_DE_LABEL]       = "Deutsch",
        [STR_LANG_FR_LABEL]       = "Français",
        [STR_LANG_RU_LABEL]       = "Русский",
        [STR_LANG_CONFIRM]        = "Confirmer",
        [STR_LANG_SWITCH_OK]      = "Langue modifiée",
        [STR_LANG_SWITCH_FAIL]    = "Échec",
        [STR_LANG_ALREADY_ZH]     = "Déjà en chinois simplifié",
        [STR_LANG_ALREADY_EN]     = "Déjà en anglais",
        [STR_LANG_ALREADY_JA]     = "Déjà en japonais",
        /* 拦截提示 */
        [STR_BLOCK_CALL]          = "Impossible pendant un appel",
        [STR_BLOCK_RECORDER]      = "Impossible pendant l'enregistrement",
        [STR_BLOCK_MEETING]       = "Impossible pendant une réunion",
        [STR_BLOCK_OK]            = "OK",
        /* AI 页 */
        [STR_AI_TITLE]            = "Chat vocal",
        [STR_AI_WAITING]          = "En attente...",
        [STR_AI_NOT_CONNECTED]    = "IA : Non connectée",
        [STR_AI_CONNECTED]        = "IA : Connectée",
        [STR_AI_DISCONNECTED]     = "IA : Déconnectée",
        [STR_AI_CONNECTING]       = "Connexion...",
        [STR_AI_SOCKET_ERROR]     = "Erreur socket",
        [STR_AI_CONNECT_FAIL]     = "Échec de connexion",
        [STR_AI_HANDSHAKE_FAIL]   = "Échec handshake WS",
        [STR_AI_LISTENING]        = "J'écoute...",
        [STR_AI_REPLYING]         = "IA répond...",
        [STR_AI_NET_CONNECTING]   = "Connexion réseau...",
        [STR_AI_NET_DISCONNECTED] = "Réseau déconnecté",
        [STR_FEISHU_MSG_FILTER]   = "Filtre msg",
        [STR_FEISHU_CONV]         = "Chat Lark",
        [STR_FEISHU_DOC_QUERY]    = "Rech. doc",
        [STR_AI_WAKEWORD_USER]    = "Salut l'ami !",
        [STR_AI_WAKEWORD_REPLY]  = "Bonjour, je vous écoute !",
        /* Camera 页 */
        [STR_CAM_TITLE]           = "Caméra IA",
        [STR_CAM_READY]           = "Prêt",
        [STR_CAM_CAPTURING]       = "Prise de vue...",
        [STR_CAM_CAPTURE_FAIL]    = "Échec de prise de vue, réessayez",
        [STR_CAM_PLEASE_CAPTURE]  = "Veuillez d'abord prendre une photo",
        [STR_CAM_AI_STARTING]     = "Démarrage IA, réessayez",
        [STR_CAM_AI_CONNECTING]   = "Connexion IA, réessayez",
        [STR_CAM_KEEP_STEADY]     = "Gardez l'appareil stable...",
        [STR_CAM_PROCESSING]      = "Traitement, veuillez patienter...",
        /* Camera AI 页 */
        [STR_CAI_TITLE]           = "Caméra IA",
        [STR_CAI_ANALYZING]       = "Analyse de l'image...",
        /* Meeting 页 */
        [STR_MEET_TITLE_IDLE]     = "Réunion",
        [STR_MEET_TITLE]          = "Compte rendu",
        [STR_MEET_RECORDING]      = "Enregistrement...",
        [STR_MEET_RECORDING_PROMPT] = "Enregistrement en cours, parlez",
        [STR_MEET_DONE]           = "Terminé, balayez vers la droite",
        [STR_MEET_DONE_FEISHU_FAIL] = "Terminé, échec de sauvegarde Lark",
        [STR_MEET_GENERATING]     = "Génération du compte rendu...",
        [STR_MEET_START_FAIL]     = "Échec du démarrage de transcription",
        [STR_MEET_SPEAKER_SYSTEM] = "Système",
        [STR_MEET_SUMMARY_TITLE]  = "【Compte rendu】",
        [STR_MEET_STOP_FIRST]     = "Veuillez d'abord arrêter l'enregistrement",
        [STR_MEET_PAUSED]         = "En pause",
        [STR_MEET_PAUSE_FAIL]     = "Échec de l'opération, réessayez",
        [STR_MEET_CONN_HINT]      = "Connexion au serveur, parlez...",
        /* Recorder 页 */
        [STR_REC_TITLE]           = "Enregistreur",
        [STR_REC_DELETE_TITLE]    = "Supprimer le fichier ?",
        [STR_REC_DELETE]          = "Supprimer",
        [STR_REC_CANCEL]          = "Annuler",
        [STR_REC_EMPTY]           = "Aucun enregistrement",
        [STR_REC_TOTAL_FMT]       = "Total : %d",
        /* Player 页 */
        [STR_PLAY_TITLE]          = "Lecteur",
        [STR_PLAY_NO_FILE]        = "Aucun fichier",
        [STR_PLAY_EMPTY]          = "Aucun audio",
        [STR_PLAY_ERROR]          = "Erreur de lecture",
        [STR_PLAY_TOTAL_FMT]      = "Total : %d",
        /* Call 页 */
        [STR_CALL_INCOMING]       = "Appel entrant",
        [STR_CALL_DECLINE]        = "Refuser",
        [STR_CALL_ACCEPT]         = "Accepter",
        [STR_CALL_UNKNOWN]        = "Numéro inconnu",
        /* Calling 页 */
        [STR_CALLING_ON_CALL]     = "En appel",
        [STR_CALLING_DIALING]     = "Numérotation...",
        [STR_CALLING_RINGING]    = "Sonnerie...",
        [STR_CALLING_CALLING]    = "Appel...",
    },
    [LANG_RU] = {
        /* 主界面 */
        [STR_HOME_DATE_FMT]       = "%d.%d",
        [STR_HOME_WEEK_SUN]       = "Вс",
        [STR_HOME_WEEK_MON]       = "Пн",
        [STR_HOME_WEEK_TUE]       = "Вт",
        [STR_HOME_WEEK_WED]       = "Ср",
        [STR_HOME_WEEK_THU]       = "Чт",
        [STR_HOME_WEEK_FRI]       = "Пт",
        [STR_HOME_WEEK_SAT]       = "Сб",
        /* 菜单页 */
        [STR_MENU_AI]             = "Голосовой чат",
        [STR_MENU_MEETING]        = "Совещание",
        [STR_MENU_CAMERA]         = "ИИ-камера",
        [STR_MENU_RECORDER]       = "Диктофон",
        [STR_MENU_PLAYER]         = "Плеер",
        [STR_MENU_SETTINGS]       = "Настройки",
        /* 设置页 */
        [STR_SETTINGS_WIFI]       = "Wi-Fi",
        [STR_SETTINGS_LANGUAGE]   = "Язык",
        /* 语言选择弹窗 */
        [STR_LANG_DIALOG_TITLE]   = "Язык",
        [STR_LANG_ZH_CN_LABEL]    = "简体中文",
        [STR_LANG_EN_LABEL]       = "English",
        [STR_LANG_JA_LABEL]       = "日本語",
        [STR_LANG_DE_LABEL]       = "Deutsch",
        [STR_LANG_FR_LABEL]       = "Français",
        [STR_LANG_RU_LABEL]       = "Русский",
        [STR_LANG_CONFIRM]        = "Подтвердить",
        [STR_LANG_SWITCH_OK]      = "Язык изменён",
        [STR_LANG_SWITCH_FAIL]    = "Ошибка",
        [STR_LANG_ALREADY_ZH]     = "Уже упрощённый китайский",
        [STR_LANG_ALREADY_EN]     = "Уже английский",
        [STR_LANG_ALREADY_JA]     = "Уже японский",
        /* 拦截提示 */
        [STR_BLOCK_CALL]          = "Нельзя переключить во время звонка",
        [STR_BLOCK_RECORDER]      = "Нельзя переключить во время записи",
        [STR_BLOCK_MEETING]       = "Нельзя переключить во время совещания",
        [STR_BLOCK_OK]            = "OK",
        /* AI 页 */
        [STR_AI_TITLE]            = "Голосовой чат",
        [STR_AI_WAITING]          = "Ожидание...",
        [STR_AI_NOT_CONNECTED]    = "ИИ: Не подключено",
        [STR_AI_CONNECTED]        = "ИИ: Подключено",
        [STR_AI_DISCONNECTED]     = "ИИ: Отключено",
        [STR_AI_CONNECTING]       = "Подключение...",
        [STR_AI_SOCKET_ERROR]     = "Ошибка сокета",
        [STR_AI_CONNECT_FAIL]     = "Не удалось подключиться",
        [STR_AI_HANDSHAKE_FAIL]   = "Сбой рукопожатия WS",
        [STR_AI_LISTENING]        = "Слушаю...",
        [STR_AI_REPLYING]         = "ИИ отвечает...",
        [STR_AI_NET_CONNECTING]   = "Подключение к сети...",
        [STR_AI_NET_DISCONNECTED] = "Сеть отключена",
        [STR_FEISHU_MSG_FILTER]   = "Фильтр сообщ.",
        [STR_FEISHU_CONV]         = "Чат Lark",
        [STR_FEISHU_DOC_QUERY]    = "Поиск док.",
        [STR_AI_WAKEWORD_USER]    = "Привет, друг!",
        [STR_AI_WAKEWORD_REPLY]  = "Здравствуйте, я вас слушаю!",
        /* Camera 页 */
        [STR_CAM_TITLE]           = "ИИ-камера",
        [STR_CAM_READY]           = "Готово",
        [STR_CAM_CAPTURING]       = "Съёмка...",
        [STR_CAM_CAPTURE_FAIL]    = "Не удалось снять, попробуйте ещё раз",
        [STR_CAM_PLEASE_CAPTURE]  = "Сначала сделайте снимок",
        [STR_CAM_AI_STARTING]     = "ИИ запускается, попробуйте ещё раз",
        [STR_CAM_AI_CONNECTING]   = "ИИ подключается, попробуйте ещё раз",
        [STR_CAM_KEEP_STEADY]     = "Держите камеру неподвижно...",
        [STR_CAM_PROCESSING]      = "Обработка, подождите...",
        /* Camera AI 页 */
        [STR_CAI_TITLE]           = "ИИ-камера",
        [STR_CAI_ANALYZING]       = "Анализ изображения...",
        /* Meeting 页 */
        [STR_MEET_TITLE_IDLE]     = "Совещание",
        [STR_MEET_TITLE]          = "Протокол совещания",
        [STR_MEET_RECORDING]      = "Идёт запись...",
        [STR_MEET_RECORDING_PROMPT] = "Идёт запись, говорите",
        [STR_MEET_DONE]           = "Готово. Смахните вправо",
        [STR_MEET_DONE_FEISHU_FAIL] = "Готово. Не удалось сохранить в Lark",
        [STR_MEET_GENERATING]     = "Создание протокола...",
        [STR_MEET_START_FAIL]     = "Не удалось запустить расшифровку",
        [STR_MEET_SPEAKER_SYSTEM] = "Система",
        [STR_MEET_SUMMARY_TITLE]  = "【Протокол совещания】",
        [STR_MEET_STOP_FIRST]     = "Сначала остановите запись",
        [STR_MEET_PAUSED]         = "Пауза",
        [STR_MEET_PAUSE_FAIL]     = "Ошибка операции, попробуйте ещё раз",
        [STR_MEET_CONN_HINT]      = "Подключение к серверу, говорите...",
        /* Recorder 页 */
        [STR_REC_TITLE]           = "Диктофон",
        [STR_REC_DELETE_TITLE]    = "Удалить файл?",
        [STR_REC_DELETE]          = "Удалить",
        [STR_REC_CANCEL]          = "Отмена",
        [STR_REC_EMPTY]           = "Нет записей",
        [STR_REC_TOTAL_FMT]       = "Всего: %d",
        /* Player 页 */
        [STR_PLAY_TITLE]          = "Плеер",
        [STR_PLAY_NO_FILE]        = "Нет файла",
        [STR_PLAY_EMPTY]          = "Нет аудио",
        [STR_PLAY_ERROR]          = "Ошибка воспроизведения",
        [STR_PLAY_TOTAL_FMT]      = "Всего: %d",
        /* Call 页 */
        [STR_CALL_INCOMING]       = "Входящий звонок",
        [STR_CALL_DECLINE]        = "Отклонить",
        [STR_CALL_ACCEPT]         = "Принять",
        [STR_CALL_UNKNOWN]        = "Неизвестный номер",
        /* Calling 页 */
        [STR_CALLING_ON_CALL]     = "Разговор",
        [STR_CALLING_DIALING]     = "Набор номера...",
        [STR_CALLING_RINGING]    = "Гудки...",
        [STR_CALLING_CALLING]    = "Вызов...",
    },
};

/* 提示音路径表（两套语言，编译期确定，存 rodata） */
static const char *const s_prompts[LANG_COUNT][PROMPT_COUNT] = {
    [LANG_ZH_CN] = {
        [PROMPT_NIHAO_QING_SHUO]   = "/emmc/xiaoqxiaoq/nihaoqingshuo.wav",
        [PROMPT_QING_LIANJIE_WIFI] = "/emmc/xiaoqxiaoq/qinglianjiewifi.wav",
        [PROMPT_WIFI_YI_LIANJIE]   = "/emmc/xiaoqxiaoq/wifiyilianjie.wav",
        [PROMPT_WIFI_YI_DUANKAI]   = "/emmc/xiaoqxiaoq/wifiyiduankai.wav",
        [PROMPT_SILENCE_TIMEOUT]   = "/emmc/xiaoqxiaoq/meiyouwentiwoxianxiale.wav",
    },
    [LANG_EN] = {
        [PROMPT_NIHAO_QING_SHUO]   = "/emmc/xiaoqxiaoq/hello_go_ahead.wav",
        [PROMPT_QING_LIANJIE_WIFI] = "/emmc/xiaoqxiaoq/connect_to_wifi_please.wav",
        [PROMPT_WIFI_YI_LIANJIE]   = "/emmc/xiaoqxiaoq/wifi_connected.wav",
        [PROMPT_WIFI_YI_DUANKAI]   = "/emmc/xiaoqxiaoq/wifi_disconnected.wav",
        [PROMPT_SILENCE_TIMEOUT]   = "/emmc/xiaoqxiaoq/no_problem_ill_step_back.wav",
    },
    [LANG_JA] = {
        [PROMPT_NIHAO_QING_SHUO]   = "/emmc/xiaoqxiaoq/hello_go_ahead_ja.wav",
        [PROMPT_QING_LIANJIE_WIFI] = "/emmc/xiaoqxiaoq/connect_to_wifi_please_ja.wav",
        [PROMPT_WIFI_YI_LIANJIE]   = "/emmc/xiaoqxiaoq/wifi_connected_ja.wav",
        [PROMPT_WIFI_YI_DUANKAI]   = "/emmc/xiaoqxiaoq/wifi_disconnected_ja.wav",
        [PROMPT_SILENCE_TIMEOUT]   = "/emmc/xiaoqxiaoq/no_problem_ill_step_back_ja.wav",
    },
    [LANG_DE] = {
        [PROMPT_NIHAO_QING_SHUO]   = "/emmc/xiaoqxiaoq/hello_go_ahead_de.wav",
        [PROMPT_QING_LIANJIE_WIFI] = "/emmc/xiaoqxiaoq/connect_to_wifi_please_de.wav",
        [PROMPT_WIFI_YI_LIANJIE]   = "/emmc/xiaoqxiaoq/wifi_connected_de.wav",
        [PROMPT_WIFI_YI_DUANKAI]   = "/emmc/xiaoqxiaoq/wifi_disconnected_de.wav",
        [PROMPT_SILENCE_TIMEOUT]   = "/emmc/xiaoqxiaoq/no_problem_ill_step_back_de.wav",
    },
    [LANG_FR] = {
        [PROMPT_NIHAO_QING_SHUO]   = "/emmc/xiaoqxiaoq/hello_go_ahead_fr.wav",
        [PROMPT_QING_LIANJIE_WIFI] = "/emmc/xiaoqxiaoq/connect_to_wifi_please_fr.wav",
        [PROMPT_WIFI_YI_LIANJIE]   = "/emmc/xiaoqxiaoq/wifi_connected_fr.wav",
        [PROMPT_WIFI_YI_DUANKAI]   = "/emmc/xiaoqxiaoq/wifi_disconnected_fr.wav",
        [PROMPT_SILENCE_TIMEOUT]   = "/emmc/xiaoqxiaoq/no_problem_ill_step_back_fr.wav",
    },
    [LANG_RU] = {
        [PROMPT_NIHAO_QING_SHUO]   = "/emmc/xiaoqxiaoq/hello_go_ahead_ru.wav",
        [PROMPT_QING_LIANJIE_WIFI] = "/emmc/xiaoqxiaoq/connect_to_wifi_please_ru.wav",
        [PROMPT_WIFI_YI_LIANJIE]   = "/emmc/xiaoqxiaoq/wifi_connected_ru.wav",
        [PROMPT_WIFI_YI_DUANKAI]   = "/emmc/xiaoqxiaoq/wifi_disconnected_ru.wav",
        [PROMPT_SILENCE_TIMEOUT]   = "/emmc/xiaoqxiaoq/no_problem_ill_step_back_ru.wav",
    }
};

/* 语言↔持久化编码映射（写入 /emmc/lang_config 的字符串） */
static const char *i18n_lang_code(lang_t lang)
{
    switch (lang)
    {
    case LANG_EN:  return "en";
    case LANG_JA:  return "ja";
    case LANG_DE:  return "de";
    case LANG_FR:  return "fr";
    case LANG_RU:  return "ru";
    case LANG_ZH_CN:
    default:       return "zh";
    }
}

/* 语言→AI 回复语言（写入 session instructions 的 Reply in {LANG}）
 * 日语用 "Japanese"（dashscope_asr.h 约定形如 "Chinese"/"English"） */
const char *i18n_lang_to_ai_reply(lang_t lang)
{
    switch (lang)
    {
    case LANG_EN:  return "english";
    case LANG_JA:  return "japanese";
    case LANG_DE:  return "german";
    case LANG_FR:  return "french";
    case LANG_RU:  return "russian";
    case LANG_ZH_CN:
    default:       return "chinese";
    }
}

/* 语言→TTS 音色。Cherry 原生支持日英等 10 语种（Qwen-TTS 音色列表），
 * 故日语沿用中文音色 Cherry；仅英文切换 Serena 与中文区分。 */
const char *i18n_lang_to_tts_voice(lang_t lang)
{
    switch (lang)
    {
    case LANG_EN:  return AGENT_DASHSCOPE_TTS_VOICE_EN;
    case LANG_JA:
    case LANG_ZH_CN:
    default:       return AGENT_DASHSCOPE_TTS_VOICE;
    }
}

/* 从 /emmc/lang_config 读取语言配置 */
static lang_t i18n_load_lang_from_file(void)
{
    int fd = open(LANG_CONFIG_FILE, O_RDONLY);
    if (fd < 0)
    {
        /* 文件不存在，首次开机默认中文 */
        return LANG_ZH_CN;
    }

    char buf[8] = {0};
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);

    if (n <= 0)
    {
        return LANG_ZH_CN;
    }

    /* 简单文本格式："en"/"ja" 或 "zh"，损坏内容默认中文 */
    if (strncmp(buf, "en", 2) == 0)
    {
        return LANG_EN;
    }
    if (strncmp(buf, "ja", 2) == 0)
    {
        return LANG_JA;
    }
    if (strncmp(buf, "de", 2) == 0)
    {
        return LANG_DE;
    }
    if (strncmp(buf, "fr", 2) == 0)
    {
        return LANG_FR;
    }
    if (strncmp(buf, "ru", 2) == 0)
    {
        return LANG_RU;
    }

    return LANG_ZH_CN;
}

/* 懒加载中文 FreeType 字体 */
static void i18n_ensure_cjk_font(void)
{
#if LV_USE_FREETYPE
    /* 去掉语言限制：中英文模式都创建 CJK 字体。
     * 原因：英文模式下语言页仍需显示"简体中文"，提示文字也可能含中文，
     *       必须有有效 CJK 字体可用，否则变方框或渲染崩溃。 */
    if (s_cjk_font == NULL)
    {
        s_cjk_font = lv_freetype_font_create(I18N_FONT_PATH,
            LV_FREETYPE_FONT_RENDER_MODE_BITMAP, I18N_FONT_SIZE,
            LV_FREETYPE_FONT_STYLE_NORMAL);
        if (!s_cjk_font)
        {
            syslog(LOG_WARNING, "[i18n] CJK font load failed: %s\n",
                   I18N_FONT_PATH);
        }
    }
    /* 同时加载 24px 小字号字体（用于正文/UI提示，参考 camera_page 24px） */
    if (s_cjk_font_small == NULL)
    {
        s_cjk_font_small = lv_freetype_font_create(I18N_FONT_PATH,
            LV_FREETYPE_FONT_RENDER_MODE_BITMAP, I18N_FONT_SIZE_SMALL,
            LV_FREETYPE_FONT_STYLE_NORMAL);
        if (!s_cjk_font_small)
        {
            syslog(LOG_WARNING, "[i18n] CJK small font load failed: %s\n",
                   I18N_FONT_PATH);
        }
    }
#endif
}

void i18n_init(void)
{
    g_current_lang = i18n_load_lang_from_file();
    syslog(LOG_INFO, "[i18n] init, lang=%s\n",
           i18n_lang_code(g_current_lang));
    /* 同步底层 AI 回复语言（仅设置全局变量，不依赖 dashscope 连接）
     * 开机首次连接 session.update 即用此语言。
     * 注意：dashscope_tts_set_voice 不在此处调用，因为 ai_agent 进程
     *       尚未启动（ws_pool/tts_cache 未初始化），改由 voice_channel_init
     *       末尾根据 /emmc/lang_config 应用音色。 */
    dashscope_asr_set_image_reply_lang(
        i18n_lang_to_ai_reply(g_current_lang));
    /* 不预加载字体，懒加载 */
}

const char *i18n_get(str_id_t id)
{
    if (id < 0 || id >= STR_COUNT)
    {
        return "";
    }
    const char *s = s_strings[g_current_lang][id];
    if (s == NULL)
    {
        s = s_strings[LANG_EN][id];
    }
    return s ? s : "";
}

const char *i18n_get_prompt(prompt_id_t id)
{
    if (id < 0 || id >= PROMPT_COUNT)
    {
        return "";
    }
    const char *s = s_prompts[g_current_lang][id];
    if (s == NULL)
    {
        s = s_prompts[LANG_EN][id];
    }
    return s ? s : "";
}

lang_t i18n_get_lang(void)
{
    return g_current_lang;
}

bool i18n_set_lang(lang_t lang)
{
    if (lang < 0 || lang >= LANG_COUNT)
    {
        return false;
    }
    if (lang == g_current_lang)
    {
        return true;  /* 无变化 */
    }

    /* 1. 写持久化文件 */
    int fd = open(LANG_CONFIG_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
    {
        syslog(LOG_ERR, "[i18n] failed to write %s\n", LANG_CONFIG_FILE);
        return false;
    }
    const char *data = i18n_lang_code(lang);
    write(fd, data, 2);
    close(fd);

    /* 2. 更新内存状态 */
    g_current_lang = lang;

    /* 同步底层 AI 回复语言和音色：图片分析下次 response.create 立即生效；
     * 普通语音对话请求刷新会话指令，若 WS 在线则主循环发 session.update，
     * 若 WS 离线则标志丢失，下次重连 dashscope_asr_stream_open 用新语言兑底。
     * 普通 TTS 每次合成新建连接，更新 s_voice 后下次合成自动用新音色。 */
    dashscope_asr_set_image_reply_lang(i18n_lang_to_ai_reply(lang));
    dashscope_tts_set_voice(i18n_lang_to_tts_voice(lang));
    voice_assistant_request_lang_refresh();

    /* 3. 语言变更后的字体处理：
     *    - 切中文：确保 CJK 字体已加载（懒加载）
     *    - 切英文：不立即释放 CJK 字体！
     *      原因：若当前页面（如 lang_screen）的 label 正在使用该字体，
     *      删除后 LVGL 渲染会访问已释放内存 → Usage Fault (PC=0x00000000)
     *      字体资源会在页面重建时由各页面自行管理 */
    if (lang == LANG_ZH_CN || lang == LANG_JA)
    {
        i18n_ensure_cjk_font();
    }
    /* LANG_EN 模式下保留 s_cjk_font 不删除，供需要显示中文的页面使用 */

    /* 4. 不立即广播！延迟到退出语言页时再广播（见 i18n_flush_lang_changed）。
     *    原因：同步广播会触发 homepage_lang_changed_cb 删除/重建 FreeType 字体，
     *    此时 lang_screen 仍是活动屏幕，LVGL 异步渲染可能访问已删除字体
     *    → Instruction access violation (PC=0x00000000) 死机。
     *    延迟到退出语言页、页面重建后再广播，字体重建安全。 */
    s_lang_pending = true;

    return true;
}

void i18n_flush_lang_changed(void)
{
    if (!s_lang_pending)
    {
        return;
    }
    s_lang_pending = false;

    lang_t lang = g_current_lang;
    for (int i = 0; i < MAX_LANG_CBS; i++)
    {
        if (s_lang_cbs[i])
        {
            s_lang_cbs[i](lang);
        }
    }
}

void i18n_apply_font(lv_obj_t *obj)
{
    if (obj == NULL)
    {
        return;
    }

    /* 中英文均使用 MiSans 28px（s_cjk_font），保证字号一致。
     * MiSans 含拉丁字符，英文模式下同样可用（参考 xiaoq_page 做法）。
     * Bug2: 原 英文回退到 montserrat_24（24px）偏小。 */
    i18n_ensure_cjk_font();
    if (s_cjk_font)
    {
        lv_obj_set_style_text_font(obj, s_cjk_font, 0);
        return;
    }

    /* CJK 字体加载失败：fallback 到 montserrat_24 */
    lv_obj_set_style_text_font(obj, &lv_font_montserrat_24, 0);
}

/* 应用小字号字体（24px）：中文用 s_cjk_font_small，英文用 montserrat_24。
 * 用于录音/播放器/wifi 等界面的非标题文字（计时、文件列表、状态等），
 * 避免中文 28px 过大导致挤压、文字显示不全。 */
void i18n_apply_font_small(lv_obj_t *obj)
{
    if (obj == NULL)
    {
        return;
    }

    /* 德法俄同用 MiSans：montserrat 无西里尔/变音符字形，
     * 且探针阶段未翻译条目回落英文亦需 MiSans 渲染 */
    if (g_current_lang != LANG_EN)
    {
        i18n_ensure_cjk_font();
        if (s_cjk_font_small)
        {
            lv_obj_set_style_text_font(obj, s_cjk_font_small, 0);
            return;
        }
        /* 24px 加载失败则 fallback 到 28px */
        if (s_cjk_font)
        {
            lv_obj_set_style_text_font(obj, s_cjk_font, 0);
            return;
        }
    }

    lv_obj_set_style_text_font(obj, &lv_font_montserrat_24, 0);
}

lv_font_t *i18n_get_cjk_font(void)
{
    /* 总是确保 CJK 字体已加载并返回。
     * 修复 bug3（英文模式中文方框）和 bug4（死机）：
     * - 之前英文模式返回 NULL，导致选项文字 fallback 到 montserrat（不含中文）→ 方框
     * - 切换语言后渲染访问空字体指针 → lv_font_get_glyph_width 崩溃
     * - 现在始终返回有效字体，中英文混排场景都能正确渲染 */
    i18n_ensure_cjk_font();
    return s_cjk_font;
}

/* 获取 24px 共享 CJK 字体（用于正文/气泡等小字号场景） */
lv_font_t *i18n_get_cjk_font_small(void)
{
    i18n_ensure_cjk_font();
    return s_cjk_font_small;
}

void i18n_register_lang_cb(lang_changed_cb cb)
{
    if (cb == NULL)
    {
        return;
    }
    for (int i = 0; i < MAX_LANG_CBS; i++)
    {
        if (s_lang_cbs[i] == NULL)
        {
            s_lang_cbs[i] = cb;
            return;
        }
    }
    syslog(LOG_WARNING, "[i18n] lang cb list full\n");
}

void i18n_unregister_lang_cb(lang_changed_cb cb)
{
    if (cb == NULL)
    {
        return;
    }
    for (int i = 0; i < MAX_LANG_CBS; i++)
    {
        if (s_lang_cbs[i] == cb)
        {
            s_lang_cbs[i] = NULL;
            return;
        }
    }
}

void i18n_rebuild_current_page(void)
{
    /* 1. 清理所有非 main 的已创建屏幕
     *    menu_screen 没有 deinit，直接删对象（它是 static 全局变量，
     *    通过 menu_page_force_delete 删除并置 NULL） */
    menu_page_force_delete();

    /* 各功能页有 deinit，调用其清理（内部判空） */
    settings_page_deinit();
    wifi_page_deinit();
    ai_page_deinit();
    camera_page_deinit();
    meeting_page_deinit();
    recorder_page_deinit();
    player_page_deinit();
    call_page_deinit();
    calling_page_deinit();
    destroy_camera_ai_page();

    /* 2. main_screen 不销毁，原地刷新已通过 i18n 广播完成 */

    /* 3. 重新创建 settings_screen（停留在设置页） */
    settings_screen = lv_obj_create(NULL);
    if (settings_screen)
    {
        lv_obj_set_style_bg_color(settings_screen, lv_color_hex(0x000000), 0);
        lv_obj_set_style_bg_opa(settings_screen, LV_OPA_COVER, 0);
        lv_obj_clear_flag(settings_screen, LV_OBJ_FLAG_SCROLLABLE);
        create_settings_page(settings_screen);
        lv_scr_load_anim(settings_screen, LV_SCR_LOAD_ANIM_FADE_IN, 300, 0, false);
    }
}
