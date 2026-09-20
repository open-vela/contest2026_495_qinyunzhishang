# 图片AI分析完整流程总结

## 目录

1. [整体架构概述](#1-整体架构概述)
2. [通信流程分析](#2-通信流程分析)
3. [协议层运行逻辑详解](#3-协议层运行逻辑详解)
4. [关键函数详解](#4-关键函数详解)
5. [调试过程记录](#5-调试过程记录)

---

## 1. 整体架构概述

### 1.1 设备角色定位

| 设备 | 角色 | 操作系统 | 主控芯片 | 职责 |
|------|------|----------|----------|------|
| **BES2800BP 开发板** | 客户端 | NuttX RTOS | BES2800BP | UI 显示、用户交互、拍照控制、图片上传、AI 分析请求发起 |
| **DashScope 云端** | 服务端 | - | - | 接收图片 + 音频，Qwen-Omni-Realtime 多模态模型分析，返回文字 + 语音回复 |

### 1.2 通信接口功能分配

```
┌──────────────────────────┐         ┌──────────────────────────┐
│       BES2800BP          │         │     DashScope 云端        │
│                          │         │                          │
│  ┌────────────────────┐  │  WSS    │  ┌────────────────────┐  │
│  │ dashscope_asr.c    │◄─┼─────────┼─►│ Qwen-Omni-Realtime │  │
│  │ WebSocket 客户端    │  │ TLS加密 │  │ 多模态模型          │  │
│  └────────────────────┘  │         │  └────────────────────┘  │
│                          │         │                          │
│  ┌────────────────────┐  │         │                          │
│  │ voice_assistant.c  │  │         │                          │
│  │ 业务逻辑/状态机     │  │         │                          │
│  └────────────────────┘  │         │                          │
│                          │         │                          │
│  ┌────────────────────┐  │         │                          │
│  │ camera_page.c      │  │         │                          │
│  │ LVGL UI + 用户交互  │  │         │                          │
│  └────────────────────┘  │         │                          │
└──────────────────────────┘         └──────────────────────────┘
```

**接口分工设计考量**：

- **WebSocket over TLS (WSS)**：全双工、长连接，适合实时多模态交互。DashScope Qwen-Omni-Realtime API 基于 OpenAI Realtime 协议，使用 WebSocket 作为传输层。
- **单一连接复用**：语音识别（ASR）、大模型对话（LLM）、语音合成（TTS）、图片分析全部通过同一条 WebSocket 连接完成，无需多连接管理。

### 1.3 系统分层架构

```
┌─────────────────────────────────────────────────────────────────┐
│                     应用层 (Application)                         │
│  camera_page.c (LVGL UI + 按钮回调 + 图片文件检查)               │
│  voice_assistant.c (业务逻辑 + 图片分析状态机 + 事件分发)         │
├─────────────────────────────────────────────────────────────────┤
│                     协议层 (Protocol)                            │
│  dashscope_asr.c (WebSocket 帧编解码 + TLS 传输 + DashScope API) │
├─────────────────────────────────────────────────────────────────┤
│                     传输层 (Transport)                           │
│  mbedtls (TLS 1.2/1.3 加密传输)                                 │
│  BSD Socket (TCP 连接)                                          │
├─────────────────────────────────────────────────────────────────┤
│                     硬件层 (Hardware)                            │
│  WiFi 模组 → 互联网 → DashScope 云端                            │
└─────────────────────────────────────────────────────────────────┘
```

---

## 2. 通信流程分析

### 2.1 启动阶段：初始化与连接建立

#### 2.1.1 应用启动流程

系统上电后，LVGL 应用启动，依次初始化各模块：

```
[LVGL 应用启动]
  │
  ├─ create_camera_page(parent)                         [camera_page.c]
  │    作用：创建 Camera 页面 UI，打开 UART/SPI 设备，启动接收线程
  │    ├─ 创建 UI 界面（预览区、状态标签、拍照/下载/AI分析按钮）
  │    ├─ 打开 /dev/ttyS1（UART 控制通道）
  │    ├─ 启动 UART 接收线程
  │    ├─ 打开 /dev/spislv0（SPI 数据通道）
  │    └─ 启动 SPI 接收线程
  │
  ├─ voice_assistant_init()                            [voice_assistant.c]
  │    作用：标记语音助手已初始化
  │    └─ s_initialized = true
  │
  └─ voice_assistant_start()                           [voice_assistant.c]
       作用：启动 DashScope 工作线程
       ├─ s_running = true
       └─ pthread_create(dashscope_thread)
```

#### 2.1.2 WebSocket 连接建立流程

`dashscope_thread` 启动后，建立与 DashScope 云端的 WebSocket 连接：

```
dashscope_thread()                                     [voice_assistant.c]
  │
  └─ dashscope_asr_stream_open()                       [dashscope_asr.c]
       作用：建立 TLS + WebSocket 连接，完成会话初始化
       │
       ├─ 1. ds_asr_tls_connect(host, port)
       │     作用：TLS 握手，建立加密 TCP 连接
       │     ├─ mbedtls_net_connect()                  // TCP 连接
       │     ├─ mbedtls_ssl_config_defaults()          // TLS 客户端配置
       │     ├─ mbedtls_ssl_set_hostname()             // SNI 主机名
       │     └─ mbedtls_ssl_handshake()                // TLS 握手
       │
       ├─ 2. ds_ws_upgrade(host, path, api_key)
       │     作用：HTTP Upgrade 到 WebSocket 协议
       │     ├─ 生成随机 Sec-WebSocket-Key (Base64)
       │     ├─ 发送 HTTP GET 请求（含 Upgrade/Authorization 头）
       │     ├─ 等待 HTTP 101 Switching Protocols 响应
       │     └─ 保存响应体后的残留数据到 pending 缓冲区
       │
       ├─ 3. 等待 session.created 事件
       │     作用：确认服务端已创建会话
       │     └─ ws_recv_text() → 解析 JSON → 确认 type="session.created"
       │
       ├─ 4. 发送 session.update（配置会话参数）
       │     作用：配置多模态模型参数
       │     ├─ input_audio_format: "pcm"              // 音频格式
       │     ├─ modalities: ["text", "audio"]           // 输出模态
       │     ├─ voice: 语音合成音色
       │     ├─ output_audio_format: "pcm"              // 输出音频格式
       │     ├─ instructions: 系统提示词（"你是小Q..."）
       │     ├─ enable_input_audio_transcription: true  // 启用语音转文字
       │     └─ turn_detection: { type: "server_vad", threshold: 0.5,
       │                          silence_duration_ms: 800 }
       │          作用：启用服务端 VAD，自动检测语音起止
       │
       └─ 5. 等待 session.updated 事件
             作用：确认会话配置已生效
             └─ ws_recv_text() → 解析 JSON → 确认 type="session.updated"
```

**连接建立完成标志**：
- `s->connected = 1`：WebSocket 连接就绪
- `s_asr_stream != NULL`：流对象可用
- 麦克风录音已启动（`media_recorder_start`）
- 进入主循环：持续发送 PCM 音频 + 接收服务端事件

### 2.2 业务运行阶段：图片AI分析完整流程

#### 2.2.1 前置条件：拍照 → 下载原图

在进行 AI 分析之前，需要先拍照并下载原图到 emmc：

```
[阶段0: 拍照 + 下载原图]
用户点击"拍照"按钮
  │
  ├─ cam_capture_btn_cb()
  │    ├─ cam_busy = true，按钮置灰
  │    ├─ tcflush(cam_uart_fd, TCIFLUSH)              // 清空 UART RX 缓冲
  │    ├─ cam_uart_send_frame(CAPTURE_REQ)  ──UART──► 2N 拍照
  │    └─ cam_start_timeout(10s)
  │
  ◄──UART── CAPTURE_ACK(filename, thumb_w, thumb_h, thumb_size)
  │
  ├─ cam_uart_send_frame(SPI_READY, session_id) ──UART──► 2N
  ├─ cam_spi_recv_active = true
  │
  │  [SPI 缩略图传输] → 显示缩略图预览
  │
用户点击"下载"按钮
  │
  ├─ cam_get_btn_cb()
  │    ├─ cam_uart_send_frame(GET_IMAGE_REQ) ──UART──► 2N
  │
  ◄──UART── GET_IMAGE_ACK(filename, file_size)
  │
  ├─ cam_big_fd = open("/emmc/xxx.jpg", O_WRONLY|O_CREAT|O_TRUNC)
  ├─ cam_uart_send_frame(SPI_READY_BIG, session) ──UART──► 2N
  │
  │  [SPI 原图传输] → 写入 /emmc/xxx.jpg
  │
  └─ cam_big_filename = "/emmc/xxx.jpg"  ★ 保存路径，供 AI 分析使用
```

#### 2.2.2 AI 分析完整流程（5 阶段状态机）

```
2800 (UI/主控)                              DashScope 云端
═══════════════════════════════════════════════════════════════

[阶段1: 用户触发]
用户点击"AI分析"按钮
  │
  ├─ cam_ai_btn_cb()                                  [camera_page.c]
  │    作用：检查前置条件，发起 AI 分析请求
  │    ├─ 检查 cam_busy（是否正在拍照/下载）
  │    ├─ 检查 cam_big_filename[0] != '\0'（是否有已下载的照片）
  │    ├─ stat(cam_big_filename) 检查文件是否存在且非空
  │    ├─ 检查 voice_assistant_is_running()
  │    ├─ 检查 voice_assistant_is_connected()
  │    ├─ 检查 voice_assistant_is_image_busy()
  │    ├─ cam_busy = true，按钮置灰
  │    ├─ voice_assistant_set_image_status_callback(cam_image_status_cb)
  │    └─ voice_assistant_analyze_image(cam_big_filename)
  │
  ├─ voice_assistant_analyze_image(image_path)         [voice_assistant.c]
  │    作用：读取图片文件，加入发送队列
  │    ├─ 检查 s_running / s_asr_stream 状态
  │    ├─ pthread_mutex_lock(&s_img_ctx.lock)
  │    ├─ 检查 s_img_ctx.state == IMG_STATE_IDLE（不忙）
  │    ├─ s_img_ctx.state = IMG_STATE_QUEUED（预占状态）
  │    ├─ pthread_mutex_unlock(&s_img_ctx.lock)
  │    ├─ open(image_path, O_RDONLY)                  // 打开图片文件
  │    ├─ fstat(fd, &st) → 检查文件大小（≤500KB）
  │    ├─ malloc(file_size) + read()                  // 读入内存
  │    ├─ close(fd)
  │    ├─ pthread_mutex_lock(&s_img_ctx.lock)
  │    ├─ s_img_ctx.data = img_data                   // 设置图片数据
  │    ├─ s_img_ctx.data_len = file_size
  │    ├─ s_img_ctx.state = IMG_STATE_QUEUED          // ★ 数据就绪，标记入队
  │    └─ pthread_mutex_unlock(&s_img_ctx.lock)
  │
  │  [返回 0，控制权交还给 UI 线程]
  │
[阶段2: 主循环检测 + 发送]
dashscope_thread 主循环检测到 QUEUED 状态
  │
  ├─ pthread_mutex_lock(&s_img_ctx.lock)
  ├─ if (s_img_ctx.state == IMG_STATE_QUEUED):
  │    s_img_ctx.state = IMG_STATE_SENDING
  │    s_img_sending = true                           // ★ 暂停麦克风音频发送
  ├─ pthread_mutex_unlock(&s_img_ctx.lock)
  │
  └─ do_image_send_and_commit()                       [voice_assistant.c]
       作用：执行 4 步发送流程（禁用VAD → 静音PCM → 图片 → commit）
       │
       ├─ Step 0: dashscope_asr_stream_disable_vad()  ──WSS──►
       │    作用：禁用 server_vad，切换到 Manual 模式
       │    发送: {"type":"session.update","session":{"turn_detection":null}}
       │                                                      │
       │                                              ◄──WSS── session.updated
       │                                                      │
       ├─ Step 1: dashscope_asr_stream_send_silent(100ms) ──WSS──►
       │    作用：发送 100ms 静音 PCM（满足图片上传前置条件）
       │    发送: {"type":"input_audio_buffer.append","audio":"<base64>"}
       │
       ├─ Step 2: dashscope_asr_stream_send_image(data, len) ──WSS──►
       │    作用：渐进式发送图片（Base64 编码后分块写入 TLS）
       │    发送: {"type":"input_image_buffer.append","image":"<base64>"}
       │    │
       │    └─ [渐进式发送细节，见 §3.2]
       │
       ├─ Step 3: dashscope_asr_stream_commit_buffer() ──WSS──►
       │    作用：提交音频+图片缓冲区
       │    发送: {"type":"input_audio_buffer.commit"}
       │                                                      │
       │  s_img_ctx.state = IMG_STATE_COMMITTING              │
       │                                                      │
       │                                              ◄──WSS── input_audio_buffer.committed
       │                                                      │
[阶段3: 触发模型响应]                                         │
  │                                                           │
  ├─ process_dashscope_event("input_audio_buffer.committed")
  │    ├─ s_img_ctx.state == IMG_STATE_COMMITTING ✓
  │    ├─ img_notify_status("Analyzing image...", false)
  │    └─ dashscope_asr_stream_commit()  ──WSS──►
  │         作用：发送 response.create，触发模型分析
  │         发送: {"type":"response.create","response":{"modalities":["text","audio"]}}
  │                                                      │
  │  s_img_ctx.state = IMG_STATE_WAITING                 │
  │                                                      │
[阶段4: 接收模型响应]                                      │
  │                                                      │
  │                                              ◄──WSS── response.created
  │                                                      │
  │  process_dashscope_event("response.created")         │
  │    ├─ s_response_active = true
  │    ├─ s_echo_suppress = true（抑制回声）
  │    ├─ dashscope_asr_stream_clear_buffer()            // 清空音频缓冲
  │    └─ omni_pb_open()                                 // 打开音频播放
  │                                                      │
  │                                              ◄──WSS── response.audio_transcript.delta
  │                                                      │  (文字增量)
  │  process_dashscope_event("response.audio_transcript.delta")
  │    └─ 累积文字到 s_transcript_buf
  │                                                      │
  │                                              ◄──WSS── response.audio.delta
  │                                                      │  (音频增量, Base64 PCM)
  │  process_dashscope_event("response.audio.delta")
  │    ├─ Base64 解码 → PCM
  │    ├─ omni_pb_write(pcm, len)                        // 播放音频
  │    └─ s_last_audio_time = get_time_ms()              // 更新心跳时间
  │                                                      │
  │  ... (多个 delta 事件) ...                            │
  │                                                      │
  │                                              ◄──WSS── response.audio_transcript.done
  │                                                      │  (文字完整结果)
  │  process_dashscope_event("response.audio_transcript.done")
  │    ├─ 获取完整文字（优先用 text 字段，fallback 用累积的 delta）
  │    ├─ syslog(LOG_INFO, "Omni transcript done: %s", full_text)
  │    └─ ai_page_show_omni_response(full_text)          // 显示到 AI 页面
  │                                                      │
  │                                              ◄──WSS── response.done
  │                                                      │
[阶段5: 清理 + 恢复]                                      │
  │                                                      │
  │  process_dashscope_event("response.done")
  │    ├─ s_response_active = false
  │    ├─ omni_pb_close()                                // 关闭音频播放
  │    ├─ dashscope_asr_stream_clear_buffer()
  │    ├─ usleep(ECHO_SETTLE_MS * 1000)                  // 等待回声消散
  │    ├─ s_echo_suppress = false
  │    │
  │    ├─ [图片分析完成清理]
  │    │   s_img_ctx.state == IMG_STATE_WAITING ✓
  │    │   ├─ free(s_img_ctx.data)                       // 释放图片内存
  │    │   ├─ s_img_ctx.data_len = 0
  │    │   ├─ s_img_ctx.state = IMG_STATE_IDLE
  │    │   ├─ s_img_sending = false                      // 恢复麦克风发送
  │    │   └─ img_notify_status("Analysis done", true)
  │    │
  │    └─ dashscope_asr_stream_enable_vad()  ──WSS──►
  │         作用：恢复 server_vad 模式
  │         发送: {"type":"session.update","session":{"turn_detection":
  │                {"type":"server_vad","threshold":0.5,"silence_duration_ms":800}}}
  │                                                      │
  │                                              ◄──WSS── session.updated
  │                                                      │
  └─ voice_assistant_update_status("正在监听...")        // 恢复监听状态
```

---

## 3. 协议层运行逻辑详解

### 3.1 WebSocket 帧格式

DashScope API 使用标准 WebSocket 协议（RFC 6455），所有事件均为 **JSON 文本帧**（opcode=0x01），客户端发送需 **mask**，服务端发送 **不 mask**。

#### 3.1.1 发送帧（ws_send_text）

```
ws_send_text(ctx, payload, plen)                       [dashscope_asr.c]
  作用：构建并发送 WebSocket 文本帧（客户端→服务端，需 mask）
  │
  ├─ 1. 构建帧头
  │    hdr[0] = 0x81 (FIN=1, opcode=TEXT)
  │    hdr[1] = MASK_BIT | payload_len (根据长度选择 1/2/8 字节编码)
  │    ├─ plen < 126:     hdr_len=2, hdr[1]=MASK|plen
  │    ├─ plen ≤ 65535:   hdr_len=4, hdr[1]=MASK|126, hdr[2..3]=plen
  │    └─ plen > 65535:   hdr_len=10, hdr[1]=MASK|127, hdr[6..9]=plen
  │
  ├─ 2. 生成随机 4 字节 mask key
  │    ds_asr_entropy(NULL, mask, 4)
  │    memcpy(hdr + hdr_len, mask, 4)
  │    hdr_len += 4
  │
  ├─ 3. 发送帧头
  │    tls_write_all(ctx, hdr, hdr_len)
  │
  └─ 4. 分块 mask + 发送 payload
       while (sent < plen):
           chunk[i] = payload[sent+i] ^ mask[(sent+i) % 4]
           tls_write_all(ctx, chunk, clen)
```

#### 3.1.2 接收帧（ws_recv_text）

```
ws_recv_text(ctx, buf, cap)                            [dashscope_asr.c]
  作用：接收并解析 WebSocket 文本帧（服务端→客户端，不 mask）
  │
  ├─ 1. 读取帧头 2 字节
  │    opcode = hdr[0] & 0x0F
  │    plen = hdr[1] & 0x7F
  │
  ├─ 2. 处理特殊帧
  │    ├─ opcode == CLOSE (0x08):
  │    │    ├─ 读取 close code (2 字节) + reason (plen-2 字节)
  │    │    ├─ 日志输出: "WS close frame: code=%u reason=%.500s"
  │    │    └─ return 0（连接关闭）
  │    │
  │    └─ opcode == PING (0x09):
  │         ├─ 读取 ping payload
  │         ├─ 发送 pong 帧 (0x8A)
  │         └─ return -2（继续接收）
  │
  ├─ 3. 处理扩展长度
  │    ├─ plen == 126: 读取 2 字节扩展长度
  │    └─ plen == 127: 读取 8 字节扩展长度（限制 ≤ 1MB）
  │
  ├─ 4. 读取 payload
  │    tls_read_all(ctx, buf, plen)
  │    buf[plen] = '\0'
  │    return plen
  │
  └─ 非文本帧: return -2（跳过）
```

### 3.2 图片渐进式发送机制

由于嵌入式设备内存有限，图片不能一次性 Base64 编码后发送。采用**渐进式分块发送**策略：

```
dashscope_asr_stream_send_image(img_data, img_len)     [dashscope_asr.c]
  作用：构建单个 input_image_buffer.append WS 帧，但 payload 分块写入 TLS
  内存占用固定约 2KB，可处理任意大小图片（受限于 500KB API 限制）
  │
  ├─ 1. 计算 Base64 长度
  │    b64_len = (img_len + 2) / 3 * 4
  │
  ├─ 2. 构建 JSON 前缀和后缀
  │    prefix = {"type":"input_image_buffer.append","event_id":"evt_img_N","image":"
  │    suffix = "}
  │    total_payload = len(prefix) + b64_len + len(suffix)
  │
  ├─ 3. 构建 WS 帧头（含总长度 total_payload）
  │    hdr[0] = 0x81
  │    hdr[1..] = MASK_BIT | total_payload 编码
  │    hdr[hlen..] = 随机 mask key (4 字节)
  │    tls_write_all(hdr, hdr_len)                    // ★ 先发帧头
  │
  ├─ 4. 发送 JSON 前缀（mask 后写入 TLS）
  │    for i in prefix: masked[i] = prefix[i] ^ mask[mask_off++ % 4]
  │    tls_write_all(masked_prefix, prefix_len)
  │
  ├─ 5. 分块 Base64 编码 + mask + 发送
  │    in_off = 0
  │    while (in_off < img_len):
  │        chunk = min(img_len - in_off, 768)          // 每次 768 字节输入
  │        if (非最后一块 && chunk % 3 != 0):
  │            chunk -= (chunk % 3)                    // 中间块保持 3 对齐
  │        mbedtls_base64_encode(b64_out, 1080, &out_len, img_data+in_off, chunk)
  │        for i in b64_out: b64_out[i] ^= mask[mask_off++ % 4]
  │        tls_write_all(b64_out, out_len)
  │        in_off += chunk
  │
  └─ 6. 发送 JSON 后缀（mask 后写入 TLS）
       for i in suffix: masked[i] = suffix[i] ^ mask[mask_off++ % 4]
       tls_write_all(masked_suffix, suffix_len)
```

**为什么中间块必须 3 字节对齐**：Base64 编码以 3 字节为一组，输出 4 字符。如果中间块不是 3 的倍数，mbedtls 会在输出中插入 `=` padding，导致服务端解析失败（Base64 字符串中间出现 `=` 是无效的）。只有最后一块允许非 3 对齐。

### 3.3 VAD 模式切换机制

DashScope Qwen-Omni-Realtime API 支持两种 turn detection 模式：

| 模式 | turn_detection 配置 | 行为 |
|------|---------------------|------|
| **server_vad**（默认） | `{"type":"server_vad","threshold":0.5,"silence_duration_ms":800}` | 服务端自动检测语音起止，自动触发 `response.create` |
| **Manual**（手动） | `null` | 客户端显式发送 `input_audio_buffer.commit` + `response.create` |

**为什么图片分析需要切换到 Manual 模式**：

在 `server_vad` 模式下，服务端自动管理对话轮次。如果客户端在 VAD 模式下发送 `conversation.item.create` 或手动 `response.create`，会与服务端的自动管理冲突，导致错误。

切换到 Manual 模式后，客户端完全控制对话流程：发送静音 PCM → 发送图片 → `input_audio_buffer.commit` → 等待 `committed` → `response.create`。

```
VAD 模式切换流程:

[正常语音对话: server_vad 模式]
  │
  ├─ 用户说话 → VAD 检测 speech_started
  ├─ 用户停止 → VAD 检测 speech_stopped
  ├─ 服务端自动 commit + response.create
  └─ 服务端返回文字 + 语音回复

[图片分析: 切换到 Manual 模式]
  │
  ├─ dashscope_asr_stream_disable_vad()
  │    发送: session.update { turn_detection: null }
  │    等待: session.updated
  │
  ├─ [手动控制流程]
  │    ├─ 发送静音 PCM (input_audio_buffer.append)
  │    ├─ 发送图片 (input_image_buffer.append)
  │    ├─ 提交缓冲区 (input_audio_buffer.commit)
  │    ├─ 等待 committed 事件
  │    └─ 触发响应 (response.create)
  │
  └─ [分析完成后恢复]
       dashscope_asr_stream_enable_vad()
       发送: session.update { turn_detection: { type: "server_vad", ... } }
       等待: session.updated
```

### 3.4 事件处理分发机制

`process_dashscope_event()` 是所有服务端事件的统一处理入口：

```
process_dashscope_event(json)                          [voice_assistant.c]
  作用：解析 JSON 事件，根据 type 字段分发到不同处理逻辑
  │
  ├─ cJSON_Parse(json) → 获取 type 字段
  │
  ├─ [关键事件日志过滤]
  │   只对 response* / image* / error / session.created 打日志
  │   避免 input_audio_buffer.* 等高频事件刷屏
  │
  ├─ session.created          → 无操作（连接建立时已处理）
  ├─ session.updated          → 无操作（VAD 切换时已处理）
  ├─ input_audio_buffer.speech_started → on_dashscope_vad_event("speech_started")
  │    ├─ voice_assistant_update_status("正在监听...")
  │    └─ 清空 transcript 缓冲区
  ├─ input_audio_buffer.speech_stopped → on_dashscope_vad_event("speech_stopped")
  │    └─ voice_assistant_update_status("AI回复中...")
  │
  ├─ input_audio_buffer.committed → ★ 图片分析关键事件
  │    ├─ if (s_img_ctx.state == IMG_STATE_COMMITTING):
  │    │    ├─ img_notify_status("Analyzing image...", false)
  │    │    ├─ dashscope_asr_stream_commit()          // 发送 response.create
  │    │    └─ s_img_ctx.state = IMG_STATE_WAITING
  │    └─ else: 忽略（正常语音对话的 committed）
  │
  ├─ response.created         → 响应开始
  │    ├─ s_response_active = true
  │    ├─ s_echo_suppress = true（抑制回声）
  │    ├─ dashscope_asr_stream_clear_buffer()
  │    └─ omni_pb_open()（打开音频播放）
  │
  ├─ response.audio_transcript.delta → 文字增量
  │    └─ 累积到 s_transcript_buf
  │
  ├─ response.audio_transcript.done → 文字完整结果
  │    ├─ 获取完整文字（优先 text 字段，fallback 累积的 delta）
  │    ├─ syslog 输出完整文字
  │    └─ ai_page_show_omni_response(full_text)
  │
  ├─ response.audio.delta     → 音频增量
  │    ├─ Base64 解码 → PCM
  │    ├─ omni_pb_write(pcm, len)
  │    └─ s_last_audio_time = get_time_ms()
  │
  ├─ response.done            → ★ 响应完成（图片分析结束）
  │    ├─ s_response_active = false
  │    ├─ omni_pb_close()
  │    ├─ [图片分析清理]
  │    │   if (s_img_ctx.state == IMG_STATE_WAITING):
  │    │       free(s_img_ctx.data)
  │    │       s_img_ctx.state = IMG_STATE_IDLE
  │    │       s_img_sending = false
  │    │       img_notify_status("Analysis done", true)
  │    │       dashscope_asr_stream_enable_vad()      // 恢复 VAD
  │    └─ voice_assistant_update_status("正在监听...")
  │
  ├─ error                    → ★ 错误处理
  │    ├─ 日志输出错误信息
  │    └─ [图片分析期间出错]
  │         if (state == WAITING/SENDING/COMMITTING):
  │             清理图片状态，恢复 VAD，通知失败
  │
  └─ 其他事件                 → 忽略
```

---

## 4. 关键函数详解

### 4.1 camera_page.c - UI 层

#### `cam_ai_btn_cb()` - AI 分析按钮回调

```c
static void cam_ai_btn_cb(lv_event_t *e)
```

**作用**：用户点击 AI 分析按钮时触发，检查所有前置条件后发起分析请求。

**执行流程**：
1. 检查 `cam_busy`：如果正在拍照或下载，直接返回
2. 检查 `cam_big_filename[0]`：如果没有已下载的照片路径，提示 "No photo downloaded yet"
3. `stat(cam_big_filename)`：检查照片文件是否存在且非空
4. 检查 `voice_assistant_is_running()`：语音助手是否已启动
5. 检查 `voice_assistant_is_connected()`：WebSocket 是否已连接
6. 检查 `voice_assistant_is_image_busy()`：是否已有图片在处理
7. 设置 `cam_busy = true`，按钮置灰
8. 注册状态回调 `voice_assistant_set_image_status_callback(cam_image_status_cb)`
9. 调用 `voice_assistant_analyze_image(cam_big_filename)` 发起分析

#### `cam_image_status_cb()` - 图片分析状态回调

```c
static void cam_image_status_cb(const char *status, bool done)
```

**作用**：接收图片分析的状态更新（通过 `lvgl_dispatch_async` 在 LVGL 线程执行）。

**执行流程**：
1. 如果 status 非空，调用 `cam_add_log("[AI_IMG] %s", status)` 显示状态
2. 如果 `done == true`，恢复按钮：`cam_busy = false`，`cam_set_buttons_enabled(true)`

### 4.2 voice_assistant.c - 业务逻辑层

#### `voice_assistant_analyze_image()` - 图片分析入口

```c
int voice_assistant_analyze_image(const char *image_path)
```

**作用**：读取图片文件到内存，加入发送队列，由主循环异步发送。

**执行流程**：
1. 检查 `s_running` 和 `s_asr_stream` 状态
2. 加锁检查 `s_img_ctx.state == IMG_STATE_IDLE`（不忙），预占为 `IMG_STATE_QUEUED`
3. `open(image_path, O_RDONLY)` 打开图片文件
4. `fstat(fd, &st)` 获取文件大小，检查 ≤ 500KB（API 限制）
5. `malloc(file_size)` 分配内存，`read()` 读取全部数据
6. `close(fd)`
7. 加锁设置 `s_img_ctx.data`、`s_img_ctx.data_len`、`s_img_ctx.filename`
8. 设置 `s_img_ctx.state = IMG_STATE_QUEUED`（数据就绪，等待主循环发送）
9. 返回 0（成功入队）

**返回值**：
- `0`：成功加入发送队列
- `-EINVAL`：参数无效
- `-ENOTCONN`：未连接
- `-EBUSY`：正在处理其他图片
- `-EFBIG`：文件过大（>500KB）
- `-ENOMEM`：内存不足
- `-EIO`：文件读取失败

#### `do_image_send_and_commit()` - 4 步发送流程

```c
static int do_image_send_and_commit(void)
```

**作用**：在 dashscope_thread 主循环中调用，执行图片发送的 4 个步骤。

**执行流程**：
1. **Step 0**：`dashscope_asr_stream_disable_vad(s_asr_stream)` - 禁用 VAD，切换到 Manual 模式
2. **Step 1**：`dashscope_asr_stream_send_silent(s_asr_stream, 100)` - 发送 100ms 静音 PCM
3. **Step 2**：`dashscope_asr_stream_send_image(s_asr_stream, s_img_ctx.data, s_img_ctx.data_len)` - 渐进式发送图片
4. **Step 3**：`dashscope_asr_stream_commit_buffer(s_asr_stream)` - 提交音频+图片缓冲区

**错误处理**：任一步骤失败，立即返回错误码，由主循环清理状态。

#### `process_dashscope_event()` - 事件分发

```c
static void process_dashscope_event(const char *json)
```

**作用**：解析服务端 JSON 事件，根据 `type` 字段分发到不同处理逻辑。

**关键事件处理**：
- `input_audio_buffer.committed`：Manual 模式下服务端确认缓冲区提交，触发 `response.create`
- `response.audio_transcript.delta`：累积文字增量
- `response.audio_transcript.done`：输出完整文字到日志和 AI 页面
- `response.audio.delta`：Base64 解码后播放音频
- `response.done`：清理图片分析状态，恢复 VAD 模式
- `error`：错误处理，清理图片状态

#### `dashscope_thread()` - 主循环

```c
static void *dashscope_thread(void *arg)
```

**作用**：语音助手主线程，管理 WebSocket 连接、音频发送/接收、图片分析调度。

**主循环逻辑**：
1. 建立 WebSocket 连接（`dashscope_asr_stream_open`）
2. 启动麦克风录音（`media_recorder_open` + `media_recorder_start`）
3. **循环**：
   - 检测图片分析请求（`s_img_ctx.state == IMG_STATE_QUEUED`）
   - 如果 `s_img_sending`，执行 `do_image_send_and_commit()`
   - 否则：读取麦克风 PCM → 发送到云端（`dashscope_asr_stream_send`）
   - 接收服务端事件（`dashscope_asr_stream_recv`）→ `process_dashscope_event()`
   - 定期发送 WebSocket ping 保活（15s 间隔）
   - **Stall 看门狗**：如果响应活跃但 15s 无音频，强制关闭播放
4. 连接断开后清理资源，2s 后重连

### 4.3 dashscope_asr.c - 协议层

#### `dashscope_asr_stream_open()` - 建立连接

```c
ds_asr_stream_t* dashscope_asr_stream_open(void)
```

**作用**：建立与 DashScope 的完整 WebSocket 连接，配置会话参数。

**执行流程**：
1. 检查 API Key 是否配置
2. `calloc(1, sizeof(ds_asr_stream_t))` 分配流结构体
3. `ds_asr_tls_connect()` - TLS 握手
4. `ds_ws_upgrade()` - WebSocket 协议升级
5. 等待 `session.created` 事件
6. 发送 `session.update` 配置多模态参数（含 `server_vad`）
7. 等待 `session.updated` 确认
8. 设置 `s->connected = 1`

#### `dashscope_asr_stream_send_silent()` - 发送静音 PCM

```c
int dashscope_asr_stream_send_silent(ds_asr_stream_t* s, size_t ms)
```

**作用**：发送一段全 0 的 PCM 数据，满足"发送 `input_image_buffer.append` 前至少发送过一次 `input_audio_buffer.append`"的前置条件。

**实现细节**：
- 使用静态 `silent_pcm[3200]` 缓冲区（BSS 段，自动清零），避免每次 malloc
- 16kHz/16bit/mono：100ms = 16000 × 2 × 0.1 = 3200 字节
- Base64 编码后通过 `input_audio_buffer.append` 事件发送
- 静音不含任何语音信息，零干扰图片分析

#### `dashscope_asr_stream_send_image()` - 渐进式图片发送

```c
int dashscope_asr_stream_send_image(ds_asr_stream_t* s,
    const unsigned char* img_data, size_t img_len)
```

**作用**：构建单个 `input_image_buffer.append` WebSocket 帧，但 payload 分块写入 TLS，内存占用固定约 2KB。

**实现细节**（详见 §3.2）：
- 计算 Base64 长度，构建 JSON 前缀/后缀
- 先发送 WS 帧头（含总长度）
- 发送 JSON 前缀（mask 后）
- 分块 Base64 编码（每块 768 字节输入 → 1024 字节输出），mask 后写入 TLS
- 中间块保持 3 字节对齐，避免 Base64 padding
- 发送 JSON 后缀（mask 后）

#### `dashscope_asr_stream_disable_vad()` - 禁用 VAD

```c
int dashscope_asr_stream_disable_vad(ds_asr_stream_t* s)
```

**作用**：发送 `session.update` 将 `turn_detection` 设为 `null`，切换到 Manual 模式。

**执行流程**：
1. 构建 JSON：`{"type":"session.update","session":{"turn_detection":null}}`
2. `ws_send_text()` 发送
3. `ws_recv_text()` 等待 `session.updated` 确认
4. 检查响应是否为 error

#### `dashscope_asr_stream_enable_vad()` - 恢复 VAD

```c
int dashscope_asr_stream_enable_vad(ds_asr_stream_t* s)
```

**作用**：图片分析完成后，发送 `session.update` 恢复 `server_vad` 模式。

**执行流程**：
1. 构建 JSON：`{"type":"session.update","session":{"turn_detection":{"type":"server_vad","threshold":0.5,"silence_duration_ms":800}}}`
2. `ws_send_text()` 发送
3. `ws_recv_text()` 等待 `session.updated` 确认

#### `dashscope_asr_stream_commit_buffer()` - 提交缓冲区

```c
int dashscope_asr_stream_commit_buffer(ds_asr_stream_t* s)
```

**作用**：发送 `input_audio_buffer.commit`，告知服务端本轮用户输入（音频+图片）已全部发送完毕。

**关键说明**：**不存在 `input_image_buffer.commit` 事件！** `input_audio_buffer.commit` 同时提交音频缓冲区和图像缓冲区。这是 DashScope API 的设计，与 OpenAI Realtime 协议一致。

#### `dashscope_asr_stream_commit()` - 触发模型响应

```c
int dashscope_asr_stream_commit(ds_asr_stream_t* s)
```

**作用**：发送 `response.create`，触发模型开始分析并生成回复。

**执行流程**：
1. 构建 JSON：`{"type":"response.create","response":{"modalities":["text","audio"]}}`
2. `ws_send_text()` 发送

**调用时机**：必须在收到 `input_audio_buffer.committed` 事件后调用。

#### `ws_send_text()` - WebSocket 文本帧发送

```c
static int ws_send_text(ds_asr_tls_t* ctx, const char* payload, size_t plen)
```

**作用**：构建标准 WebSocket 文本帧（masked），通过 TLS 发送。

#### `ws_recv_text()` - WebSocket 文本帧接收

```c
static int ws_recv_text(ds_asr_tls_t* ctx, char* buf, size_t cap)
```

**作用**：接收并解析 WebSocket 文本帧（unmasked），处理 close/ping 帧。

**返回值**：
- `> 0`：接收到的 payload 长度
- `0`：收到 close 帧（连接关闭）
- `-2`：收到 ping 帧（已自动回复 pong，调用者应 continue）
- `< 0`：错误（-EIO, -ECONNRESET, -ETIMEDOUT, -EOVERFLOW）

### 4.4 状态机定义

```c
typedef enum {
  IMG_STATE_IDLE = 0,     /* 空闲，可接受新请求 */
  IMG_STATE_QUEUED,       /* 已入队，等待主循环发送 */
  IMG_STATE_SENDING,      /* 主循环正在发送（disable_vad+PCM+图片+commit） */
  IMG_STATE_COMMITTING,   /* 已发input_audio_buffer.commit，等待committed事件 */
  IMG_STATE_WAITING,      /* 已发response.create，等待response.done */
  IMG_STATE_ERROR         /* 发送/分析失败 */
} img_state_t;
```

**状态转换图**：

```
IDLE ──[analyze_image()]──► QUEUED ──[主循环检测]──► SENDING
  ▲                                                    │
  │                                          [do_image_send_and_commit() 成功]
  │                                                    │
  │                                                    ▼
  │                                              COMMITTING ──[收到 committed]──► WAITING
  │                                                    │                            │
  │                                          [发送失败] │                            │
  │                                                    ▼                            │
  ├────────────────────────────────────────────── ERROR                      [收到 response.done]
  │                                                    │                            │
  │                                                    │                            ▼
  └────────────────────────────────────────────────────┘                          IDLE
                              [清理 + 恢复 VAD]
```

---

## 5. 调试过程记录

### 5.1 问题 1：UI 按钮重叠

**现象**：Camera 页面中，拍照按钮和 AI 分析按钮与预览区域重叠。

**原因**：按钮容器使用绝对定位，预览区域尺寸固定，在小屏幕上发生重叠。

**解决方案**：
- 预览区域使用动态尺寸计算（`screen_w - 20`，最大 240px）
- 按钮容器对齐到 `LV_ALIGN_BOTTOM_MID, 0, -5`
- 使用 flex 布局排列按钮，间距 60px

### 5.2 问题 2：页面滑动卡死

**现象**：从 Camera page 往左滑进入 AI page 时死机。

**原因**：按钮的触摸事件被按钮自身消费，没有向上传播给父页面的手势检测器。LVGL 默认情况下，对象会消费触摸事件，阻止手势识别。

**解决方案**：为所有按钮和容器添加 `LV_OBJ_FLAG_GESTURE_BUBBLE` 标志，允许手势事件向上冒泡传播到父页面。

```c
lv_obj_add_flag(btn_container, LV_OBJ_FLAG_GESTURE_BUBBLE);
lv_obj_add_flag(cam_preview_img, LV_OBJ_FLAG_GESTURE_BUBBLE);
```

### 5.3 问题 3：中文字体显示方格

**现象**：AI 分析时 UI 显示全是方格，不显示中文字体。

**原因**：LVGL 默认字体不包含中文字符，需要使用包含中文的字体文件。

**解决方案**：将日志中的中文替换为英文，避免依赖中文字体。后续可配置 LVGL 中文字体支持。

### 5.4 问题 4：云端未收到图片（核心问题）

**现象**：云端回复"需要上传图片才能回答"，日志显示图片数据已发送但服务端未识别。

**根因分析**（多轮调试）：

| 尝试 | 做法 | 结果 | 原因 |
|------|------|------|------|
| 第1轮 | 发送 `input_image_buffer.append` 后直接发 `conversation.item.create` | 失败 | VAD 模式下不允许手动 `conversation.item.create` |
| 第2轮 | 发送图片后发 `input_image_buffer.commit` | 失败 | **`input_image_buffer.commit` 不是有效的 API 事件！** |
| 第3轮 | 发送图片后发 `input_audio_buffer.commit` | 失败 | VAD 模式自动管理，手动 commit 被忽略 |
| 第4轮 | 禁用 VAD → 发静音 PCM → 发图片 → `input_audio_buffer.commit` → 等 committed → `response.create` | **成功** | 完全对齐官方 API 文档 |

**最终正确方案**：
1. 禁用 VAD（`session.update` 设 `turn_detection: null`）
2. 发送静音 PCM（满足图片上传前置条件）
3. 渐进式发送图片（`input_image_buffer.append`）
4. 提交缓冲区（`input_audio_buffer.commit`，同时提交音频+图片）
5. 等待 `input_audio_buffer.committed` 事件
6. 发送 `response.create` 触发模型分析

### 5.5 问题 5：`input_image_buffer.commit` 不存在

**现象**：服务端返回错误 `Invalid value: 'input_image_buffer.commit'`。

**原因**：DashScope Qwen-Omni-Realtime API 中不存在 `input_image_buffer.commit` 事件类型。图片缓冲区通过 `input_audio_buffer.commit` 一并提交。

**解决方案**：删除所有 `input_image_buffer.commit` 相关代码，统一使用 `input_audio_buffer.commit`。

### 5.6 问题 6：VAD 模式冲突

**现象**：服务端拒绝手动 `conversation.item.create`，提示 VAD 模式冲突。

**原因**：`server_vad` 模式下，服务端自动管理对话轮次（自动检测语音起止、自动 commit、自动 response.create）。客户端手动发送 `conversation.item.create` 或 `response.create` 会与服务端自动管理冲突。

**解决方案**：
- 图片分析前：`dashscope_asr_stream_disable_vad()` 切换到 Manual 模式
- 图片分析后：`dashscope_asr_stream_enable_vad()` 恢复 `server_vad` 模式

### 5.7 问题 7：WebSocket Close 帧缓冲区溢出

**现象**：服务端返回的错误信息被截断，无法看到完整的错误原因。

**原因**：`ws_recv_text()` 中 `close_payload[128]` 缓冲区太小，而日志格式 `%.400s` 试图读取 400 字符，导致缓冲区溢出。

**解决方案**：
- 将 `close_payload` 从 128 字节扩大到 512 字节
- 修正读取逻辑：`read_len >= sizeof(close_payload)` 时截断为 `sizeof(close_payload) - 1`
- 修正条件判断：原来 `plen < sizeof(close_payload)` 导致大 payload 被跳过，改为始终尝试读取

### 5.8 问题 8：云端回复文字未记录

**现象**：云端回复了语音，但无法确认文字内容是否正确。

**原因**：`response.audio_transcript.done` 事件中的文字没有输出到日志。

**解决方案**：在 `process_dashscope_event()` 中处理 `response.audio_transcript.done` 事件时，添加日志输出：

```c
syslog(LOG_INFO, "[%s] Omni transcript done: %s\n", TAG, full_text);
```

### 5.9 问题 9：日志检索困难

**现象**：AI 图片分析相关日志分散在多个文件中，没有统一前缀，难以检索。

**解决方案**：为所有 AI 图片分析相关日志添加 `[AI_IMG]` 前缀，覆盖以下文件：
- `dashscope_asr.c`：`send_image`、`disable_vad`、`enable_vad`、`commit_buffer`、`commit`
- `voice_assistant.c`：`analyze_image`、`do_image_send_and_commit`、状态机转换、错误处理
- `camera_page.c`：`cam_ai_btn_cb`、`cam_image_status_cb`

### 5.10 问题 10：调试效率低

**现象**：每次调试 AI 分析都需要先拍照、再下载、再分析，流程繁琐。

**临时解决方案**：修改 `cam_ai_btn_cb()` 固定上传 `photo_20260702_153912.jpg`，跳过拍照和下载步骤。

**最终方案**：调试完成后恢复为使用 `cam_big_filename`（下载到 emmc 的照片路径），并添加空路径检查。

### 5.11 问题 11：响应卡死（Stall 看门狗）

**现象**：AI 分析过程中，如果服务端响应中断（如 `response.done` 丢失），播放会一直处于活跃状态，用户无法进行下一次交互。

**解决方案**：添加 Stall 看门狗机制：
- 在 `response.audio.delta` 事件中更新 `s_last_audio_time`
- 主循环检查：如果 `s_response_active` 且距上次音频超过 `RESPONSE_STALL_TIMEOUT_MS`（15s），强制关闭播放
- 图片分析期间 stall 时，同时清理图片状态，避免状态机卡死

### 5.12 问题 12：连接断开时图片状态残留

**现象**：WebSocket 连接意外断开时，图片分析状态未清理，导致下次连接后状态机卡在非 IDLE 状态。

**解决方案**：在 `dashscope_thread` 主循环的连接断开处理中，添加图片状态清理：

```c
pthread_mutex_lock(&s_img_ctx.lock);
if (s_img_ctx.state != IMG_STATE_IDLE) {
    free(s_img_ctx.data);
    s_img_ctx.data = NULL;
    s_img_ctx.data_len = 0;
    s_img_ctx.state = IMG_STATE_IDLE;
    s_img_sending = false;
    img_notify_status("Disconnected, analysis aborted", true);
}
pthread_mutex_unlock(&s_img_ctx.lock);
```

---

## 附录：关键文件清单

| 文件 | 路径 | 职责 |
|------|------|------|
| camera_page.c | `rtos/apps/examples/lvgldemo/camera_page.c` | Camera 页面 UI、按钮回调、图片文件检查 |
| voice_assistant.c | `rtos/apps/examples/lvgldemo/voice_assistant.c` | 语音助手主循环、图片分析状态机、事件分发 |
| voice_assistant.h | `rtos/apps/examples/lvgldemo/voice_assistant.h` | 语音助手 API 声明 |
| dashscope_asr.c | `rtos/apps/packages/ai_agent/src/voice/dashscope_asr.c` | WebSocket 协议实现、DashScope API 封装 |
| dashscope_asr.h | `rtos/apps/packages/ai_agent/src/voice/dashscope_asr.h` | DashScope ASR API 声明 |
