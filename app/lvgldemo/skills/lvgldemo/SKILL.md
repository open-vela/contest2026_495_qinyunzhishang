---
name: "lvgldemo"
description: "NuttX LVGL 多模态智能手表应用，支持端云协同语音助手（本地VAD唤醒+云端DashScope对话）、摄像头AI图片分析、蓝牙通话、会议转写、WiFi、录音播放器。在修改 lvgldemo 页面、语音助手、唤醒检测、图片分析或处理多线程问题时调用。"
---

# lvgldemo 技能

NuttX LVGL 多模态智能手表应用，运行在 BES 平台 (best1700/si8658) 上。采用 454px 圆形显示屏（可选无 LCD 路径），基于手势的页面导航，集成端云协同语音助手、摄像头AI分析、蓝牙通话、会议转写等功能。

## 项目位置

`rtos/apps/examples/lvgldemo/`

## 架构概览

### 端云协同语音助手（双模式）

```
┌─────────────────────────────────────────────────────────┐
│                    lvgldemo.c (入口)                      │
│  ┌─────────────┐  ┌──────────────────┐  ┌────────────┐ │
│  │ WiFi 监控    │  │ 系统状态机        │  │ 提示音管理  │ │
│  │ (timer/thread)│  │ IDLE→NET→ONLINE │  │            │ │
│  └──────┬───────┘  │    →AI_RESPONSE │  └──────┬─────┘ │
│         │ network   └────────┬─────────┘          │      │
│         │ ready               │                    │      │
│         ▼                    ▼                    ▼      │
│  ┌──────────────┐  ┌──────────────────┐  ┌────────────┐ │
│  │ wakeup_       │  │ voice_assistant   │  │ dashscope_  │ │
│  │ detector.cpp  │  │ .c               │  │ asr.c *     │ │
│  │ (本地VAD+KWS) │  │ (空闲/对话模式)    │  │ (WSS协议)   │ │
│  └──────────────┘  └──────────────────┘  └────────────┘ │
│                           │                    ↑         │
│                           └─ ai_page.c (UI) ───┘         │
│                             (show_user_message /         │
│                              show_omni_response)          │
└─────────────────────────────────────────────────────────┘

* dashscope_asr.c 位于 rtos/apps/packages/ai_agent/src/voice/
```

**空闲模式**：WiFi 连接后，DashScope WebSocket 保持连接（仅 ping 保活），本地 VAD 持续运行检测唤醒词。不发送麦克风音频。

**对话模式**：唤醒词触发后，停止本地 VAD，打开云端麦克风，音频流发送到 DashScope Qwen-Omni-Realtime 进行实时 ASR + AI + TTS。300 秒静音超时后自动退出。

### 系统状态机

```c
typedef enum {
    SYSTEM_STATE_IDLE,           // 空闲（网络未就绪）
    SYSTEM_STATE_NETWORKING,     // 网络连接中
    SYSTEM_STATE_ONLINE_WAKEUP,  // 在线唤醒（网络就绪，等待唤醒词）
    SYSTEM_STATE_AI_RESPONSE,    // AI响应中（对话模式）
} system_state_t;
```

状态转换：`IDLE → (WiFi连接) → ONLINE_WAKEUP → (唤醒词) → AI_RESPONSE → (静音超时/断连) → ONLINE_WAKEUP`

### 屏幕导航（环形）

```
  播放器 ←── 录音 ←── WiFi ←── 主页 ──→ AI ──→ Camera
  (右滑)   (右滑)    (右滑)           (右滑)   (左滑回AI)
                              ↓
                          Meeting (语音"开会"关键词触发)
```

- **主屏幕** (xiaoq_page)：主页，带动画宇航员角色"小Q"，显示系统状态
- **WiFi 屏幕** (wifi_page)：WiFi 扫描/连接/断开，密码输入弹窗，自动保存凭证
- **录音屏幕** (recorder_page)：WAV 录音，保存到 `/emmc/audio/`
- **播放器屏幕** (player_page)：音频播放，从 `/emmc/audio/` 读取
- **AI 屏幕** (ai_page)：云端对话界面，显示用户消息和 AI 回复。左滑回主页，右滑进 Camera
- **Camera 屏幕** (camera_page)：远程拍照（UART控制2N）、SPI下载原图、AI图片分析。左滑回 AI
- **Meeting 屏幕** (meeting_page)：会议实时转写，带说话人标签和计时器。右滑回主页
- **来电页面** (call_page)：蓝牙来电，接听/拒接按钮（事件触发，非手势导航）
- **通话页面** (calling_page)：通话中，挂断按钮（事件触发，非手势导航）
- **蓝牙调试页** (bt_debug_page)：蓝牙状态调试

### ai_page 双重角色

`ai_page.c` 同时承担两个角色：
1. **本地 ai_agent 客户端**：通过 WebSocket 连接 `127.0.0.1:28789`，支持文字聊天和语音控制命令
2. **云端对话 UI 界面**：`voice_assistant.c` 调用 `ai_page_show_user_message()` 和 `ai_page_show_omni_response()` 显示云端对话内容

### 无 LCD 路径

`lvgldemo.c` 检测 `result.disp != NULL`，无 LCD 时：
- 跳过所有 UI 创建和 LVGL 定时器
- 使用 `network_monitor_thread`（3秒轮询）替代 LVGL timer 监控 WiFi 状态
- 使用 `wifi_auto_connect_start()` 自动连接已保存的 WiFi
- 语音助手和唤醒检测正常工作，提示音通过 VAD 线程播放
- 跨线程 UI 派发器跳过（队列项将被丢弃，不影响核心功能）

### 时间持久化

- `time_save_thread` 每 10 秒保存系统时间到 `/emmc/time_stamp`
- 启动时 `persist_time_load()` 恢复时间（若保存的时间大于当前时间）
- 线程栈大小 2048 字节，分离线程

### WiFi 自动保存与连接

- `wifi_save_config(ssid, password)` — 连接成功后保存凭证到 `/emmc/wifi_config`
- `wifi_auto_connect_start()` — 启动后台线程，加载已保存凭证并自动连接
- `wifi_load_configs()` — 从文件加载已保存的 WiFi 列表
- 无 LCD 时在 `main()` 中直接调用；有 LCD 时在 `wifi_page` 中触发

### 共享屏幕指针

定义在 `lvgldemo.c`，声明在 `lvgldemo_common.h`：

```c
extern lv_obj_t *main_screen;
extern lv_obj_t *wifi_screen;
extern lv_obj_t *recorder_screen;
extern lv_obj_t *player_screen;
extern lv_obj_t *ai_screen;
extern lv_obj_t *camera_screen;
extern lv_obj_t *meeting_screen;
```

### 懒加载屏幕创建模式

屏幕在首次导航时才创建，离开时销毁：

```c
if (camera_screen == NULL) {
    camera_screen = lv_obj_create(NULL);
    create_camera_page(camera_screen);
}
lv_scr_load_anim(camera_screen, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 300, 0, false);
```

### 屏幕生命周期

1. **创建**：`create_xxx_page(screen)` 构建 UI 并注册回调
2. **活跃**：页面处理用户交互
3. **卸载**：`LV_EVENT_SCREEN_UNLOADED` → `lv_async_call(xxx_deinit_async_cb)` → `xxx_page_deinit()`
4. **销毁**：在 deinit 中调用 `lv_obj_del(screen)`，将指针置为 NULL

**重要**：`screen_unloaded_cb` 中调用了 `lv_indev_reset(NULL, NULL)` 以防止悬空的 indev 引用。**不要删除这些调用**。

## 文件参考

| 文件 | 用途 |
|------|------|
| `lvgldemo.c` | 入口点，LVGL 初始化，系统状态机，网络监控，手势处理，共享屏幕指针，无LCD路径，时间持久化 |
| `lvgldemo_common.h` | 共享屏幕指针声明 |
| `lvgl_dispatch.c/h` | 跨线程 UI 派发器（环形队列+互斥锁，替代 `lv_async_call`） |
| `voice_assistant.c/h` | 语音助手核心：DashScope WebSocket 连接、空闲/对话双模式、图片AI分析状态机、静音超时、VAD模式切换 |
| `wakeup_detector.cpp/h` | 本地唤醒词检测器：TFLite Micro KWS + VAD，检测"小Q小Q"，提示音播放 |
| `xiaoq_wakeup_model_data.h` | 唤醒词 TFLite 模型数据 |
| `xiaoq_wakeup_preprocessor_data.h` | 唤醒词音频预处理器数据 |
| `xiaoq_page.c/h` | 主屏幕，带动画"小Q"宇航员角色，状态显示，`xiaoq_update_status()` |
| `wifi_page.c/h` | WiFi 设置：扫描、连接 (WPA2)、断开、密码输入弹窗、自动保存/加载凭证 |
| `recorder_page.c/h` | WAV 录音器，使用 `media_recorder` API，文件列表 |
| `player_page.c/h` | 音频播放器，使用 `media_player` API，播放列表，进度条 |
| `ai_page.c/h` | AI 对话界面 + 本地 ai_agent WebSocket 客户端（`127.0.0.1:28789`） |
| `camera_page.c/h` | 摄像头页面：UART控制2N拍照、SPI下载原图、AI图片分析 |
| `camera_hub_2n.py` | 2N摄像头模拟脚本（调试用，Python） |
| `bt_call_handler.c/h` | 蓝牙来电处理：状态机管理来电/通话/挂断，状态查询API |
| `bt_debug_page.c/h` | 蓝牙调试页面：显示蓝牙状态信息 |
| `call_page.c/h` | 来电页面：接听/拒接按钮，来电号码显示 |
| `calling_page.c/h` | 通话中页面：挂断按钮，通话计时 |
| `meeting_page.c/h` | 会议转写页面：实时文字、说话人标签、计时器、录音控制 |
| `meeting_asr.c/h` | 会议 ASR：实时语音转写引擎，连接云端 ASR 服务 |
| `CMakeLists.txt` | CMake 构建配置（依赖 lvgl, netutils, media, ai_agent） |
| `Makefile` | Make 构建配置（CSRCS 列表，DashScope/mbedtls include 路径） |
| `Make.defs` | 构建定义 |
| `Kconfig` | 配置项：优先级、栈大小、输入设备路径、唤醒使能、冷却时间 |

## 外部依赖

| 外部模块 | 路径 | 用途 |
|----------|------|------|
| `dashscope_asr.c/h` | `rtos/apps/packages/ai_agent/src/voice/` | WebSocket over TLS 协议层，DashScope API 封装 |
| `ai_agent` | `rtos/apps/packages/ai_agent/` | AI Agent 包（含 DashScope、infra 等） |
| `cJSON` | `rtos/apps/netutils/cjson/cJSON/` | JSON 解析库 |
| `mbedtls` | `rtos/apps/crypto/mbedtls/` | TLS 加密传输 |

## 语音助手核心流程

### 启动流程

```
main() → wakeup_detector_init/start → voice_assistant_init
         ↓ (WiFi连接后)
on_network_ready() → voice_assistant_start()
         ↓
dashscope_thread → dashscope_asr_stream_open()
         ↓ TLS握手 → WebSocket升级 → session.update(配置VAD/模型参数)
空闲模式（仅 ping 保活，不发送 mic 音频，VAD 持续运行）
```

### 唤醒→对话切换

```
wakeup_detector 检测到"小Q小Q"
  ↓ on_local_wakeup() 回调 (VAD线程)
  ├─ 播放"你好，请说"提示音 (wakeup_detector_play_prompt)
  ├─ voice_assistant_enter_dialogue() → s_dialogue_active = true
  ├─ 主循环检测到 s_dialogue_active:
  │   ├─ wakeup_detector_stop()（释放麦克风给云端）
  │   ├─ dialogue_open_mic()（打开 media_recorder，设麦克风增益最大）
  │   └─ 开始发送 PCM 音频到 DashScope (16kHz/16bit/mono)
  └─ lvgl_dispatch_async(enter_ai_page)（跳转UI到AI页面）
```

### 退出对话

- **静音超时**（300秒无对话活动）：播放"没有问题我先下了"→ 关闭 mic → 重启 VAD
- **WiFi 断开**：`on_network_disconnected` → 后台线程 `voice_assistant_stop()` → 重启 VAD
- **主动退出**：`voice_assistant_exit_dialogue()` → 关闭 mic → 重启 VAD

### 静音超时机制

- `SILENCE_TIMEOUT_MS = 300000`（300秒）
- `s_last_dialogue_activity` 更新来源：
  1. 云端 VAD `speech_started` 事件（用户说话）
  2. `response.audio.delta` 事件（AI 说话）
  3. 进入对话模式时初始化
- 超时后播放 `/emmc/xiaoqxiaoq/meiyouwentiwoxianxiale.wav`

### 图片AI分析

独立于对话模式，空闲模式也可用。流程：禁用VAD → 发静音PCM → 发图片 → commit → 等待committed → response.create → 接收回复 → 恢复VAD。详见 `doc/图片AI分析完整流程总结.md`。

### 会议功能

- 触发方式：语音助手检测到"开会/会议记录/开始会议"关键词
- 回调：`on_meeting_keyword_detected()` → `enter_meeting_page_async_cb()` 跳转 UI
- 架构：双录音器（rec1 写 WAV 存档 + rec2 读 PCM 实时 ASR），TLS/WS 连接预热 + 积压音频追赶补发，sentence_id 字幕分行，渐进式进度条
- `meeting_asr.c` 连接云端 ASR 服务进行实时转写（rec2 失败时降级为会后文件转录）
- `meeting_page.c` 显示转写文字、说话人标签、会议计时器
- 详见 `docs/meeting_transcription_feature.md` 第 11-15 章（v2.0 实时转录：架构设计、踩坑实录、最佳实践、性能指标）

### 蓝牙通话

- `bt_call_handler` 监听蓝牙来电事件，维护通话状态机
- 来电时 UI 自动切换到 `call_page`（接听/拒接）
- 通话中切换到 `calling_page`（挂断/计时）
- 通话状态可通过 `bt_call_handler_get_call_state()` 查询

## 提示音文件

| 文件路径 | 触发时机 |
|----------|----------|
| `/emmc/xiaoqxiaoq/qinglianjiewifi.wav` | 无 WiFi 时唤醒词触发 |
| `/emmc/xiaoqxiaoq/wifiyilianjie.wav` | WiFi 连接成功 |
| `/emmc/xiaoqxiaoq/nihaoqingshuo.wav` | 唤醒词触发（WiFi 已连接） |
| `/emmc/xiaoqxiaoq/wifiyiduankai.wav` | WiFi 断开 |
| `/emmc/xiaoqxiaoq/meiyouwentiwoxianxiale.wav` | 300秒静音超时 |

播放方式：
- `wakeup_detector_play_prompt(path)`：VAD 线程回调中调用（停止录音器→播放→不重开）
- `wakeup_detector_request_prompt(path)`：外部线程调用（信号 VAD 线程处理）

## 关键约定

### 跨线程 UI 派发

- LVGL **不是**线程安全的。`lv_async_call` 直接修改全局 timer 链表，多线程并发会竞态导致崩溃
- **必须使用** `lvgl_dispatch_async(cb, data)` 替代 `lv_async_call`，通过环形队列安全投递
- LVGL 主线程通过 5ms 定时器调用 `lvgl_dispatch_drain()` 消费队列
- 无 LCD 时跳过派发器（队列项将被丢弃）
- `data` 的所有权和释放由回调自行管理

### UI 风格
- 背景色：`0xF0F8FF`（爱丽丝蓝）用于 WiFi/主页，`0x1A1A2E`（深色）用于 AI/录音
- 强调色：`0x0088FF`（蓝色），`0xFF8800`（橙色）
- 圆形屏幕：使用约 60px 侧边内边距，保持内容在内切圆内
- 标题栏：36px 高度，蓝色背景，白色文字，左侧返回按钮

### 手势处理
- 在屏幕对象上注册 `LV_EVENT_GESTURE` 事件
- **不要给屏幕对象添加 `LV_OBJ_FLAG_GESTURE_BUBBLE`** —— LVGL 从触摸的子对象向上查找没有此标志的对象来发送手势事件
- 子页面列表应设置 `LV_OBJ_FLAG_GESTURE_BUBBLE`，使手势冒泡到屏幕

### 内存管理
- LVGL 堆有限；分配大对象前检查 `lv_mem_monitor()`
- WiFi 扫描限制结果为 64 个 AP，空闲内存 < 4096 字节时停止
- SSID 字符串使用 `lv_malloc` 存储为按钮用户数据，在 deinit 中释放
- 录音器/播放器共享 SMF 音频管线 —— 播放前必须关闭录音器
- 图片数据通过 `malloc` 分配，分析完成后在 `response.done` 或错误路径释放

### 条件编译
- WiFi 功能由 `#ifdef CONFIG_WIRELESS_WAPI` 保护
- 媒体录音器/播放器由 `#ifdef CONFIG_MEDIA` 保护
- 语音助手由 `#ifdef CONFIG_EXAMPLES_AI_AGENT_VELA` 保护
- 唤醒检测由 `#ifdef CONFIG_EXAMPLES_LVGLDEMO_WAKEUP_ENABLE` 保护
- FreeType 字体加载由 `#if LV_USE_FREETYPE` 保护
- BES 麦克风增益控制使用 `__attribute__((weak))` 声明 `af_stream_set_chan_vol`
- LCD 显示由运行时 `result.disp != NULL` 检测（非编译时）

## 添加新页面

1. 创建 `xxx_page.c` 和 `xxx_page.h`，遵循现有模式：
   - `create_xxx_page(lv_obj_t *parent)` —— 构建 UI
   - `xxx_page_deinit(void)` —— 清理资源，删除屏幕对象
   - `xxx_screen_unloaded_cb()` —— 调用 `lv_indev_reset(NULL, NULL)` 然后 `lv_async_call(deinit_async_cb)`
   - `xxx_gesture_cb()` —— 处理左/右滑动导航

2. 在 `lvgldemo_common.h` 中添加 `extern lv_obj_t *xxx_screen;`

3. 在 `lvgldemo.c` 中定义 `lv_obj_t *xxx_screen = NULL;`，添加 `enter_xxx_page()` 和手势导航

4. 在相邻页面添加手势导航

5. 添加到 `CMakeLists.txt` 的 `LVGLDEMO_SRCS` 和 `Makefile` 的 `CSRCS`

6. 在 `main()` 清理部分调用 `xxx_page_deinit()`

## 添加新语音助手功能

1. 在 `voice_assistant.c` 中实现，使用 `s_asr_stream` 发送/接收
2. 跨线程 UI 更新使用 `lvgl_dispatch_async()`，**不要用 `lv_async_call`**
3. 在 `voice_assistant.h` 中暴露 API
4. 如需回调通知 UI，参考 `img_notify_status()` 模式（malloc msg → dispatch → free in cb）
5. 确保所有异常路径（断连、超时、循环退出、stall）都清理状态
6. 协议层操作使用 `dashscope_asr.c` 中的 API（`dashscope_asr_stream_*` 系列）

## 已知问题和注意事项

1. **页面切换崩溃**：长按定时器可能在活动对象被删除后触发。`screen_unloaded_cb` 中的 `lv_indev_reset(NULL, NULL)` 可防止此问题。**不要删除它**。

2. **录音器/播放器资源冲突**：SMF 共享音频管线。必须在 `player_open()` 之前调用 `recorder_close()`，播放停止后调用 `recorder_open()`。

3. **音量衰减**：多个音量控制层（MediaVolume、MusicVolume、RecordVolume、MuteMode）都必须设为最大值。

4. **WiFi 状态持久化**：`wifi_selected_ssid`、`wifi_is_on`、`wifi_is_connected` 在 `wifi_page_deinit()` 中**不会**重置。

5. **DashScope 服务器 300 秒空闲超时**：空闲模式下服务器可能关闭 WebSocket（`code=1007, no response for 300 seconds`）。客户端自动重连。

6. **云端 VAD `speech_started` 误判**：环境噪音可能触发云端 VAD 的 `speech_started` 事件，重置静音计时器，导致 300 秒超时无法触发。

7. **DMA 数据丢失**：`DmaSource.cpp generateFrame() data lost(7200), mic0` 表示麦克风 DMA 缓冲区溢出，每次丢失 7200 字节约 225ms 音频。

8. **跨线程 UI 安全**：`lv_async_call` 在多线程下不安全，**必须用 `lvgl_dispatch_async`**。详见 `lvgl_dispatch.h` 注释。

9. **无 LCD 设备**：Frame Buffer 初始化失败是预期行为。WiFi 自动连接和语音助手在此路径下正常工作。

10. **蓝牙通话优先级**：来电时 `bt_call_handler` 管理通话状态，UI 自动切换到 `call_page` / `calling_page`。

11. **麦克风硬件切换**：本地 VAD 和云端对话共享同一麦克风硬件。切换时必须先停一方再开另一方（`wakeup_detector_stop()` → `dialogue_open_mic()`，`dialogue_close_mic()` → `wakeup_detector_start()`）。

## 构建系统

- NuttX 应用通过 `Kconfig` 注册（`CONFIG_EXAMPLES_LVGLDEMO`）
- 默认栈大小：32768 字节
- 默认优先级：100
- 输入设备路径：`/dev/input0`（可配置，`CONFIG_EXAMPLES_LVGLDEMO_INPUT_DEVPATH`）
- 唤醒冷却时间：3000ms（`CONFIG_EXAMPLES_LVGLDEMO_WAKEUP_COOLDOWN_MS`）
- 依赖：`GRAPHICS_LVGL`，可选 `WIRELESS_WAPI`、`MEDIA`、`AI_AGENT_VELA`、`LVGLDEMO_WAKEUP_ENABLE`
- `voice_assistant.c` 在 Makefile 中无条件编译，在 CMakeLists 中仅 `CONFIG_EXAMPLES_AI_AGENT_VELA` 时编译
- `wakeup_detector.cpp` 是唯一的 C++ 文件（TFLite Micro 需要 C++），仅 `WAKEUP_ENABLE` 时编译
- Makefile 中额外包含 ai_agent 和 mbedtls 的 include 路径
