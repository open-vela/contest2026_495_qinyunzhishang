/****************************************************************************
 * apps/examples/lvgldemo/i18n.h
 *
 * 中英文切换模块 - 字符串表 + 语言状态机 + 字体管理
 *
 ****************************************************************************/

#ifndef I18N_H
#define I18N_H

#include <lvgl/lvgl.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 语言类型 */
typedef enum {
    LANG_ZH_CN = 0,
    LANG_EN,
    LANG_COUNT
} lang_t;

/* 字符串 ID 枚举（按页面分组） */
typedef enum {
    /* 主界面 */
    STR_HOME_DATE_FMT,          /* "%d月%d日" / "%b %d" */
    STR_HOME_WEEK_SUN,          /* 星期日 / Sun */
    STR_HOME_WEEK_MON,
    STR_HOME_WEEK_TUE,
    STR_HOME_WEEK_WED,
    STR_HOME_WEEK_THU,
    STR_HOME_WEEK_FRI,
    STR_HOME_WEEK_SAT,

    /* 菜单页 */
    STR_MENU_AI,                /* 语音对话 / AI Assistant */
    STR_MENU_MEETING,           /* 会议模式 / Meeting */
    STR_MENU_CAMERA,            /* 图文智录 / AI Camera */
    STR_MENU_RECORDER,          /* 录音 / Recorder */
    STR_MENU_PLAYER,            /* 媒体播放器 / Player */
    STR_MENU_SETTINGS,          /* 设置 / Settings */

    /* 设置页 */
    STR_SETTINGS_WIFI,          /* WiFi / WiFi */
    STR_SETTINGS_LANGUAGE,      /* 语言 / Language */

    /* 语言选择弹窗 */
    STR_LANG_DIALOG_TITLE,      /* 选择语言 / Select Language */
    STR_LANG_ZH_CN_LABEL,       /* 简体中文 */
    STR_LANG_EN_LABEL,          /* English */
    STR_LANG_CONFIRM,           /* 确认 / Confirm */
    STR_LANG_SWITCH_OK,         /* 切换成功 / Switched */
    STR_LANG_SWITCH_FAIL,       /* 切换失败 / Failed */
    STR_LANG_ALREADY_ZH,        /* 当前已是简体中文 / Already Simplified Chinese */
    STR_LANG_ALREADY_EN,        /* 当前已是English / Already English */

    /* 拦截提示 */
    STR_BLOCK_CALL,             /* 通话中无法切换语言 / Cannot switch during call */
    STR_BLOCK_RECORDER,         /* 录音中无法切换语言 / Cannot switch during recording */
    STR_BLOCK_MEETING,          /* 会议中无法切换语言 / Cannot switch during meeting */
    STR_BLOCK_OK,               /* 确定 / OK */

    /* AI 页 */
    STR_AI_TITLE,               /* 语音对话 / AI Assistant */
    STR_AI_WAITING,             /* 等待唤醒... / Waiting... */
    STR_AI_NOT_CONNECTED,       /* AI: 未连接 / AI: Not connected */
    STR_AI_CONNECTED,           /* AI: 已连接 / AI: Connected */
    STR_AI_DISCONNECTED,        /* AI: 未连接 / AI: Disconnected */
    STR_AI_CONNECTING,          /* 连接中... / Connecting... */
    STR_AI_SOCKET_ERROR,        /* Socket 错误 / Socket error */
    STR_AI_CONNECT_FAIL,        /* 连接失败 / Connect failed */
    STR_AI_HANDSHAKE_FAIL,      /* WS 握手失败 / WS handshake failed */
    STR_AI_LISTENING,          /* 正在监听... / Listening... */
    STR_AI_REPLYING,           /* AI回复中... / AI replying... */
    STR_AI_NET_CONNECTING,     /* 网络连接中... / Connecting network... */
    STR_AI_NET_DISCONNECTED,   /* 网络已断开 / Network disconnected */
    STR_FEISHU_MSG_FILTER,     /* 消息筛选 / Msg filter */
    STR_FEISHU_CONV,           /* 飞书对话 / Feishu chat */
    STR_FEISHU_DOC_QUERY,      /* 文档查询 / Doc query */
    STR_AI_WAKEWORD_USER,      /* 唤醒词用户气泡: 小Q，小Q! / Hi buddy! */
    STR_AI_WAKEWORD_REPLY,     /* 唤醒词AI回复气泡: 你好，请说！ / Hello, speaking! */

    /* Camera 页 */
    STR_CAM_TITLE,              /* 图文智录 / AI Camera */
    STR_CAM_READY,              /* 就绪 / Ready */
    STR_CAM_CAPTURING,          /* 拍照中... / Capturing... */
    STR_CAM_CAPTURE_FAIL,       /* 拍照失败，请重试 / Capture failed, please retry */
    STR_CAM_PLEASE_CAPTURE,     /* 请先拍照，谢谢 / Please capture first */
    STR_CAM_AI_STARTING,        /* AI启动中，请重试 / AI starting, retry */
    STR_CAM_AI_CONNECTING,      /* AI连接中，请重试 / AI connecting, retry */
    STR_CAM_KEEP_STEADY,        /* 请保持平稳... / Hold still... */
    STR_CAM_PROCESSING,         /* 处理中，请稍候... / Processing... */

    /* Camera AI 页（图文智录 AI 分析页） */
    STR_CAI_TITLE,              /* 图文智录 / AI Camera */
    STR_CAI_ANALYZING,          /* 正在分析图片... / Analyzing image... */

    /* Meeting 页 */
    STR_MEET_TITLE_IDLE,        /* 会议模式 / Meeting */
    STR_MEET_TITLE,             /* 会议记录 / Meeting Notes */
    STR_MEET_RECORDING,         /* 会议录音中... / Recording... */
    STR_MEET_RECORDING_PROMPT,  /* 会议录音中，请开始发言 / Recording, please speak */
    STR_MEET_DONE,              /* 转写完成，左滑返回 / Done, swipe left */
    STR_MEET_DONE_FEISHU_FAIL,  /* 转写完成，飞书保存失败 / Done, Feishu save failed */
    STR_MEET_GENERATING,        /* 正在生成会议记录，请稍候... / Generating notes... */
    STR_MEET_START_FAIL,        /* 转写启动失败 / Transcription start failed */
    STR_MEET_SPEAKER_SYSTEM,    /* 系统 / System (显示用，判断仍用"系统") */
    STR_MEET_SUMMARY_TITLE,    /* 【会议纪要】 / [Meeting Notes] */
    STR_MEET_STOP_FIRST,       /* 请先停止录制 / Please stop recording first */
    STR_MEET_PAUSED,           /* 已暂停 / Paused */
    STR_MEET_PAUSE_FAIL,       /* 操作失败，请重试 / Operation failed, retry */
    STR_MEET_CONN_HINT,        /* 正在连接服务器，请开始发言... / Connecting to server, please speak... */

    /* Recorder 页 */
    STR_REC_TITLE,              /* 录音 / Recorder */
    STR_REC_DELETE_TITLE,       /* 删除文件？ / Delete File? */
    STR_REC_DELETE,             /* 删除 / Delete */
    STR_REC_CANCEL,             /* 取消 / Cancel */
    STR_REC_EMPTY,              /* 暂无录音 / No recordings */
    STR_REC_TOTAL_FMT,          /* 共 %d 个 / Total: %d */

    /* Player 页 */
    STR_PLAY_TITLE,             /* 播放器 / Player */
    STR_PLAY_NO_FILE,           /* 无文件 / No file */
    STR_PLAY_EMPTY,             /* 暂无音频 / No audio files */
    STR_PLAY_ERROR,             /* 播放错误 / Play error */
    STR_PLAY_TOTAL_FMT,         /* 共 %d 个 / Total: %d */

    /* Call 页 */
    STR_CALL_INCOMING,          /* 来电 / Incoming Call */
    STR_CALL_DECLINE,           /* 拒接 / Decline */
    STR_CALL_ACCEPT,            /* 接听 / Accept */
    STR_CALL_UNKNOWN,           /* 未知号码 / Unknown */

    /* Calling 页 */
    STR_CALLING_ON_CALL,        /* 通话中 / On Call */
    STR_CALLING_DIALING,        /* 拨号中... / Dialing... */
    STR_CALLING_RINGING,        /* 响铃中... / Ringing... */
    STR_CALLING_CALLING,        /* 呼叫中... / Calling... */

    STR_COUNT
} str_id_t;

/* 提示音 ID 枚举（按当前语言返回 wav 路径） */
typedef enum {
    PROMPT_NIHAO_QING_SHUO = 0,  /* 唤醒成功"你好请说" / hello_go_ahead */
    PROMPT_QING_LIANJIE_WIFI,    /* 无WiFi唤醒"请连接wifi" / connect_to_wifi_please */
    PROMPT_WIFI_YI_LIANJIE,      /* WiFi已连接 / wifi_connected */
    PROMPT_WIFI_YI_DUANKAI,      /* WiFi已断开 / wifi_disconnected */
    PROMPT_SILENCE_TIMEOUT,      /* 静音超时"没有问题我先下了" / no_problem_ill_step_back */
    PROMPT_COUNT
} prompt_id_t;

/* 初始化（开机调用，从 /emmc/lang_config 读取） */
void i18n_init(void);

/* 获取当前语言对应字符串 */
const char *i18n_get(str_id_t id);

/* 获取当前语言对应提示音 wav 路径 */
const char *i18n_get_prompt(prompt_id_t id);

/* 获取当前语言 */
lang_t i18n_get_lang(void);

/* 设置语言（保存到文件，广播事件）
 * 返回 true=成功，false=保存失败
 */
bool i18n_set_lang(lang_t lang);

/* 统一的字体应用函数
 * 中英文模式均应用 MiSans FreeType 28px（懒加载，含拉丁字符）
 */
void i18n_apply_font(lv_obj_t *obj);

/* 应用小字号字体（24px）：中文用 24px CJK 字体，英文用 montserrat_24。
 * 用于录音/播放器/wifi 等界面的非标题文字，避免中文 28px 过大挤压。 */
void i18n_apply_font_small(lv_obj_t *obj);

/* 获取共享的 MiSans FreeType 字体（28px）
 * 总是确保字体已加载并返回（中英文模式均有效）
 */
lv_font_t *i18n_get_cjk_font(void);

/* 获取 24px 共享 CJK 字体（用于正文/气泡等小字号场景，参考 camera_page 24px） */
lv_font_t *i18n_get_cjk_font_small(void);

/* 延迟广播语言变更（在退出语言页、页面重建后调用）
 * 通知 main_screen 等常驻页刷新字体/文字。
 * 设计原因：i18n_set_lang 不同步广播，避免切语言时
 *           FreeType 字体重建引发渲染崩溃 */
void i18n_flush_lang_changed(void);

/* 方案 C：语言变更回调注册（仅 main_screen 等常驻页用） */
typedef void (*lang_changed_cb)(lang_t lang);
void i18n_register_lang_cb(lang_changed_cb cb);
void i18n_unregister_lang_cb(lang_changed_cb cb);

/* 重建当前页（切语言后调用）
 * 删除所有非 main 的残留屏幕，重建 settings_screen
 */
void i18n_rebuild_current_page(void);

/* 英文月份缩写表（供主页日期格式化用） */
extern const char *const i18n_en_months[12];

#ifdef __cplusplus
}
#endif

#endif /* I18N_H */
