# Camera 拍照及数据传输模块设计文档

> **关联文档**：
> - [BES2800BP 与 RK3568 双机通信总结.md](file:///home/lzw/code/si8658ca/openvela_003_0623/openvela_si8658ca/doc/BES2800BP 与 RK3568 双机通信总结.md)
> - [SPI校验机制演进总结.md](file:///home/lzw/code/si8658ca/openvela_003_0623/openvela_si8658ca/doc/SPI校验机制演进总结.md)

---

## 一、模块设计概述

### 1.1 模块功能简介

本模块实现 **BES2800BP（2800，SPI 从机 + UART）控制 Lubancat 2N（2N，SPI 主机 + UART）完成拍照与图像传输** 的完整业务功能。核心能力包括：

- **拍照控制**：2800 通过 UART 下发拍照命令，2N 调用 GStreamer 控制 USB 摄像头采集 JPEG 图像。
- **缩略图传输与显示**：2N 将照片缩放为 280×158 RGB565 缩略图，通过 SPI 发送给 2800，2800 在 LVGL UI 上实时预览。
- **原图获取与存储**：用户可按需请求原图，2N 通过 SPI 将 1~1.3MB JPEG 传给 2800，存入 eMMC。
- **双向通信**：UART 传输控制信令（命令/ACK），SPI 传输图像数据，末尾 Bitmap 汇总校验保证可靠性。
- **UI 交互**：LVGL 实现拍照按钮、获取按钮、预览区、日志栏，支持手势导航（AI 页 ↔ Camera 页）。

### 1.2 模块应用场景

| 场景 | 触发条件 | 运行时机 |
|------|---------|---------|
| 用户拍照 | 点击"拍照"按钮 | 任意时刻，`cam_busy=false` 时 |
| 缩略图预览 | 拍照 ACK 返回后自动触发 | 拍照完成后立即执行 |
| 原图获取 | 点击"获取"按钮 | 缩略图显示完成后 |
| 页面切换 | 右滑进入 / 左滑退出 | 用户手势触发 |
| 异常恢复 | 超时/DMA 错误 | 自动触发，复位 `cam_busy` |

**系统位置**：本模块位于 LVGL Demo 应用内，作为独立页面（Camera Page）存在，与 AI 页面并列，是嵌入式设备人机交互的核心功能之一。

### 1.3 设计目标与原则

| 原则 | 实现方式 |
|------|---------|
| **可靠性** | CRC16-CCITT 双向校验 + 末尾 Bitmap 汇总重传 + DMA 错误自动恢复 + 多级超时保护 |
| **实时性** | SPI 24MHz 高速传输 + DMA 零拷贝接收 + poll 异步唤醒 + lv_async_call 跨线程 UI 更新 |
| **低耦合** | UART/SPI 通信逻辑与 UI 逻辑分离；硬件驱动与应用层通过字符设备接口隔离 |
| **可移植性** | 2N 侧 Python 脚本跨平台；2800 侧基于 NuttX 标准 SPI Slave 驱动框架 |
| **模块化** | 按功能拆分：UI 层 / 通信层 / 协议层 / 驱动层，各层职责清晰 |
| **容错性** | 最多 3 次重传 + 硬件复位（close/reopen SPI）+ 状态标志保护防误触发 |

### 1.4 运行环境

| 项目 | 2800 侧 | 2N 侧 |
|------|---------|--------|
| 硬件平台 | BES2800BP 开发板 | Lubancat 2N 开发板 |
| MCU/SoC | BES2800BP | RK3568 |
| 操作系统 | NuttX RTOS | Linux |
| 编译环境 | GCC (ARM) + NuttX build | Python 3 + spidev + pyserial |
| UI 框架 | LVGL 9 | 无（命令行） |
| SPI 角色 | 从机（DMA 接收） | 主机（DMA 发送） |
| UART | `/dev/ttyS1` @ 921600 | `/dev/ttyS3` @ 921600 |
| SPI 设备 | `/dev/spislv0` | `/dev/spidev3.0` |

---

## 二、模块需求分析

### 2.1 功能需求

| 编号 | 功能点 | 说明 |
|------|--------|------|
| F-01 | 拍照命令下发 | 2800 通过 UART 发送 CAPTURE_REQ，2N 接收后执行拍照 |
| F-02 | 拍照结果回传 | 2N 拍照后回传 CAPTURE_ACK（含文件名、缩略图尺寸）或 CAPTURE_FAIL |
| F-03 | 缩略图生成 | 2N 将 JPEG 缩放为 280×158 RGB565 格式（88480 字节） |
| F-04 | 缩略图 SPI 传输 | 2N 通过 SPI 分帧发送缩略图，2800 接收并组装 |
| F-05 | 缩略图显示 | 2800 将 RGB565 数据送入 LVGL 图像控件显示 |
| F-06 | 原图请求与传输 | 2800 请求原图，2N 通过 SPI 传输 1~1.3MB JPEG |
| F-07 | 原图存储 | 2800 将原图写入 eMMC 文件系统 |
| F-08 | 末尾 Bitmap 汇总校验 | 2800 收到 END 帧后用 bitmap 指示缺失帧，2N 选择性重传 |
| F-09 | UI 交互 | 拍照/获取按钮、预览区、日志栏、手势导航 |
| F-10 | 超时与异常恢复 | UART 10s 超时、SPI 50s 超时、DMA 错误自动复位 |

### 2.2 性能需求

| 指标 | 要求 | 实测 |
|------|------|------|
| UART 命令往返延迟 | < 100ms | ~50ms |
| 拍照完成时间 | < 5s | ~3s（含预热） |
| 缩略图传输时间（88KB） | < 15s | ~13s（346 帧） |
| 原图传输时间（1MB） | < 60s | ~30s（4096 帧，末尾汇总校验） |
| SPI 帧间间隔 | ≥ 3ms | 3ms（`SPI_INTER_FRAME_DELAY`） |
| SPI 最大重试次数 | 3 | 3（`max_attempts`） |
| 2800 RAM 占用 | < 200KB | ~150KB（含 88KB 缩略图缓冲 + 4KB DMA 缓冲） |
| 2800 Flash 占用 | < 50KB | ~30KB（camera_page.o） |

### 2.3 接口需求

#### 2.3.1 硬件接口

| 接口 | 2800 | 2N | 连接方式 |
|------|------|-----|---------|
| UART | `/dev/ttyS1` | `/dev/ttyS3` | TX↔RX, RX↔TX, GND↔GND |
| SPI | MOSI/MISO/SCLK/CS | MOSI/MISO/SCLK/CS | 4 线直连，CS 由 2N 控制 |
| 摄像头 | 无 | USB `/dev/video0` | UVC 兼容摄像头 |
| 显示 | LCD + LVGL | 无 | 2800 本地屏 |

#### 2.3.2 软件函数接口

**2800 侧对外接口**（`camera_page.c`）：

| 函数 | 功能 | 调用时机 |
|------|------|---------|
| `create_camera_page(parent)` | 创建 Camera 页面，初始化设备与线程 | LVGL 启动时 |
| `cam_capture_btn_cb(e, event)` | 拍照按钮回调 | 用户点击 |
| `cam_get_btn_cb(e, event)` | 获取按钮回调 | 用户点击 |

**2N 侧对外接口**（`camera_hub_2n.py`）：

| 函数 | 功能 | 调用时机 |
|------|------|---------|
| `main()` | 主循环，分发命令 | 脚本启动 |
| `do_capture()` | 执行拍照 | 收到 CAPTURE_REQ |
| `spi_send_image(data, sid, type)` | SPI 发送图像 | 收到 SPI_READY |

### 2.4 可靠性与容错需求

| 需求 | 实现机制 |
|------|---------|
| 数据完整性 | CRC16-CCITT 双向校验（UART + SPI） |
| 帧丢失恢复 | 末尾 Bitmap 汇总 + 选择性重传（最多 3 次） |
| 超时保护 | UART 10s + SPI 50s + Bitmap ACK 3s |
| DMA 故障恢复 | `des_cur_addr` 有效性检查 + close/reopen SPI 硬件 |
| 误触发防护 | `cam_busy` 标志位 + 按钮置灰 |
| 状态残留防护 | close 后 50ms 延时再 open，确保 UNBIND 完成 |
| 线程安全 | LVGL UI 更新通过 `lv_async_call` 投递到主线程 |
| 缓冲污染防护 | 每次接收前 `memset(rx_dma_buffer, 0, len)` + `tcflush` 清空 UART RX |

---

## 三、总体架构设计

### 3.1 软件分层结构

```
┌─────────────────────────────────────────────────────────────────┐
│  应用层 (Application Layer)                                     │
│  ├─ 2800: camera_page.c (LVGL UI + 业务编排)                    │
│  └─ 2N:   camera_hub_2n.py (摄像头控制 + 通信编排)              │
├─────────────────────────────────────────────────────────────────┤
│  协议层 (Protocol Layer)                                        │
│  ├─ UART 帧编解码 (sync + len + cmd + seq + payload + crc)     │
│  ├─ SPI 帧编解码 (magic + type + session + total + offset + crc)│
│  └─ Bitmap ACK 生成与解析                                       │
├─────────────────────────────────────────────────────────────────┤
│  抽象层 (Abstraction Layer)                                     │
│  ├─ 2800: spi_slave_driver.c (NuttX SPI Slave 字符设备)         │
│  └─ 2N:   spidev (Linux SPI 字符设备)                           │
├─────────────────────────────────────────────────────────────────┤
│  驱动层 (Driver Layer)                                          │
│  ├─ 2800: bes_spi_slave.c (BES SPI Slave LHF + DMA + CS IRQ)   │
│  └─ 2N:   spi-rockchip (RK3568 SPI 主机驱动)                   │
├─────────────────────────────────────────────────────────────────┤
│  HAL 层 (Hardware Abstraction Layer)                            │
│  ├─ 2800: hal_spi.c + hal_iomux.c (寄存器操作 + 引脚复用)      │
│  └─ 2N:   Linux SPI 子系统                                      │
├─────────────────────────────────────────────────────────────────┤
│  硬件层 (Hardware)                                              │
│  SPI 控制器 + DMA 控制器 + GPIO/IOExpander (CS 中断)            │
└─────────────────────────────────────────────────────────────────┘
```

### 3.2 模块整体框图

```
                    ┌────────────────────────────────────┐
                    │          用户交互 (LVGL)            │
                    │  拍照按钮 / 获取按钮 / 预览区 / 日志│
                    └──────────────┬─────────────────────┘
                                   │
                    ┌──────────────▼─────────────────────┐
                    │       业务逻辑层 (camera_page.c)    │
                    │  ├─ 拍照流程编排                   │
                    │  ├─ 传输流程编排                   │
                    │  ├─ 超时管理 (10s/50s)             │
                    │  └─ 状态管理 (cam_busy)            │
                    └──┬───────────────────────────┬─────┘
                       │                           │
            ┌──────────▼──────────┐    ┌──────────▼──────────┐
            │   UART 通信模块     │    │   SPI 通信模块      │
            │  ├─ 帧发送          │    │  ├─ 帧接收          │
            │  ├─ 帧接收 (线程)   │    │  ├─ 帧解析 (线程)   │
            │  ├─ CRC16 校验      │    │  ├─ CRC16 校验      │
            │  └─ 命令分发        │    │  └─ Bitmap 维护    │
            └──────────┬──────────┘    └──────────┬──────────┘
                       │                           │
            ┌──────────▼──────────┐    ┌──────────▼──────────┐
            │  /dev/ttyS1         │    │  /dev/spislv0       │
            │  (NuttX UART 驱动)  │    │  (NuttX SPI Slave)  │
            └─────────────────────┘    └─────────────────────┘
                       │                           │
                       ▼                           ▼
              ┌──────────────────────────────────────────┐
              │        物理通道 (UART + SPI 4 线)         │
              └──────────────────┬───────────────────────┘
                                 │
              ┌──────────────────▼───────────────────────┐
              │           2N 侧 (camera_hub_2n.py)        │
              │  ├─ 命令接收 (ack_reader_thread)         │
              │  ├─ 拍照执行 (GStreamer)                 │
              │  ├─ 缩略图生成 (PIL)                     │
              │  ├─ SPI 发送 (spi_send_image)            │
              │  ├─ Bitmap ACK 解析与重传                │
              │  └─ UART ACK 回复                        │
              └──────────────────────────────────────────┘
```

### 3.3 模块运行机制

本模块采用**多线程 + 事件驱动**混合运行机制：

| 线程/上下文 | 作用 | 触发方式 |
|------------|------|---------|
| **LVGL 主线程** | UI 渲染、按钮回调、`lv_async_call` 回调 | 事件循环 |
| **UART RX 线程** (2800) | 解析 UART 帧，处理 CAPTURE_ACK 等 | `read()` 阻塞唤醒 |
| **SPI RX 线程** (2800) | `poll/read` SPI 数据，解析帧，发 Bitmap ACK | `poll()` POLLIN 唤醒 |
| **CS 中断工作队列** (2800) | 处理 CS 上升沿，调度 `schedule_transfer(RX_DONE)` | 硬件中断 → work_queue |
| **ACK 读取线程** (2N) | 后台读 UART，分发 FRAME_ACK 到队列 | `ser.read()` 轮询 |
| **主线程** (2N) | 等待命令，执行拍照/SPI 发送 | `cmd_queue.get()` 阻塞 |

### 3.4 模块依赖关系

**2800 侧依赖**：
- NuttX SPI Slave 驱动框架（`spi_slave_driver.c`）
- BES SPI Slave LHF 驱动（`bes_spi_slave.c`）
- NuttX UART 驱动
- LVGL 9 图形库
- POSIX 线程（pthread）
- 文件系统（eMMC，存储原图）

**2N 侧依赖**：
- Linux spidev 驱动
- Linux serial 驱动（pyserial）
- GStreamer 1.0（v4l2src + jpegenc）
- Python PIL（缩略图生成）
- Python `struct`（帧编解码）

---

## 四、模块详细功能设计

### 4.1 子功能划分

本模块拆分为 7 个子功能单元：

| 编号 | 子功能 | 所属侧 | 说明 |
|------|--------|--------|------|
| SF-01 | 初始化 | 双侧 | 设备打开、DMA 预启动、线程创建 |
| SF-02 | UI 管理 | 2800 | 页面创建、按钮回调、缩略图显示 |
| SF-03 | 拍照控制 | 双侧 | 命令下发、GStreamer 拍照、ACK 回复 |
| SF-04 | UART 通信 | 双侧 | 帧编解码、收发管理、命令分发 |
| SF-05 | SPI 通信 | 双侧 | 帧编解码、DMA 收发、CS 中断处理 |
| SF-06 | Bitmap 校验 | 双侧 | bitmap 维护、END 帧汇总、选择性重传 |
| SF-07 | 异常处理 | 双侧 | 超时、DMA 错误、状态复位 |

### 4.2 各子功能流程设计

#### 4.2.1 初始化流程（SF-01）

详见 [BES2800BP 与 RK3568 双机通信总结.md §2.1](file:///home/lzw/code/si8658ca/openvela_003_0623/openvela_si8658ca/doc/BES2800BP 与 RK3568 双机通信总结.md) 的初始化流程。

```
[2800 板级启动]
  bes_spi_driver_prob() → board_spislavedev_initialize(0)
    → bes_spislave_initialize() → 注册 /dev/spislv0
    (DMA 未启动)

[2800 应用启动 create_camera_page()]
  ├─ cam_uart_open()           // 打开 /dev/ttyS1 @ 921600
  ├─ open("/dev/spislv0")      // ★ 触发 spislave_bind() → DMA 预启动
  ├─ pthread_create(uart_rx)   // 启动 UART 接收线程
  └─ pthread_create(spi_rx)    // 启动 SPI 接收线程

[2N 脚本启动]
  ├─ check_uart_baudrate()     // 确保波特率 921600
  ├─ serial.Serial(...)        // 打开 UART
  └─ Thread(ack_reader)        // 启动 ACK 读取线程
```

#### 4.2.2 拍照控制流程（SF-03）

```
[2800 用户点击拍照]
  cam_capture_btn_cb()
  ├─ if (cam_busy) return              // 防误触发
  ├─ cam_busy = true
  ├─ 按钮置灰
  ├─ tcflush(TCIFLUSH)                 // 清空 RX 残留
  ├─ cam_uart_send_frame(CAPTURE_REQ)
  └─ lv_timer_start(cam_timeout, 10s)  // 启动 10s 超时

[2N 收到命令]
  ack_reader_thread → cmd_queue.put(CAPTURE_REQ)
  main() → do_capture()
  ├─ gst-launch v4l2src num-buffers=8 → fakesink   // 预热
  ├─ gst-launch v4l2src num-buffers=1 → jpegenc → filesink  // 拍照
  ├─ make_thumbnail_rgb565()           // 生成 88480 字节缩略图
  └─ send_capture_ack(filename, w, h, size)

[2800 收到 ACK]
  uart_rx_thread → 解析 CAPTURE_ACK
  ├─ cam_stop_timeout()
  ├─ cam_add_log("Capture done!")
  ├─ cam_session_id++
  ├─ cam_uart_send_frame(SPI_READY, session_id)
  ├─ cam_spi_recv_active = true        // 允许 SPI 解析
  └─ lv_timer_start(cam_spi_timeout, 50s)
```

#### 4.2.3 SPI 数据传输流程（SF-05 + SF-06）

详见 [BES2800BP 与 RK3568 双机通信总结.md §2.2](file:///home/lzw/code/si8658ca/openvela_003_0623/openvela_si8658ca/doc/BES2800BP 与 RK3568 双机通信总结.md) 的业务运行阶段。

**2N 发送流程**：
```
spi_send_image()
  for attempt in range(3):
    ├─ 连续发送所有数据帧 (不等 ACK)
    │   for offset in range(0, total, 256):
    │     spi.xfer2(frame)
    │     sleep(3ms)                  // 帧间延时
    ├─ 发送 END 帧
    ├─ wait_bitmap_ack(timeout=3s)
    ├─ if ok: return True
    └─ else: _send_frames_by_offsets(missing)  // 选择性重传
  return False
```

**2800 接收流程**：
```
cam_spi_rx_thread()
  while (cam_spi_recv_active):
    poll(spislv0, POLLIN, 1000)
    read(spislv0, stream_buf)
    解析帧:
      ├─ 查找 magic 0x494D4731
      ├─ 校验 CRC16
      ├─ CRC OK: memcpy + bitmap[fidx] |= bit
      └─ CRC FAIL: pos++ (等 END 汇总)
    if (type == END):
      ├─ 遍历 bitmap
      ├─ 构建 Bitmap ACK
      └─ cam_uart_send_frame(FRAME_ACK)
```

### 4.3 状态机设计

#### 4.3.1 2800 业务状态机

```
                  ┌─────────────┐
                  │   IDLE      │ ← cam_busy=false
                  │ (空闲)      │   按钮可用
                  └──────┬──────┘
                         │ 点击拍照
                         ▼
                  ┌─────────────┐
                  │ CAPTURING   │ ← cam_busy=true
                  │ (拍照中)    │   10s 超时计时
                  └──────┬──────┘
                         │ 收到 CAPTURE_ACK
                         ▼
                  ┌─────────────┐
                  │ TRANSFER    │ ← cam_spi_recv_active=true
                  │ (传输中)    │   50s 超时计时
                  └──────┬──────┘
                         │ 收到 Bitmap ACK ok=1
                         ▼
                  ┌─────────────┐
                  │ DISPLAY     │ ← lv_async_call 显示
                  │ (显示中)    │
                  └──────┬──────┘
                         │ 显示完成
                         ▼
                  ┌─────────────┐
                  │   IDLE      │
                  └─────────────┘

  异常路径：任意状态 → 超时/DMA 错误 → IDLE (复位 cam_busy)
```

#### 4.3.2 2N 发送状态机

```
  WAIT_CMD ──CAPTURE_REQ──► CAPTURING ──ACK_SENT──► WAIT_SPI_READY
       ▲                                              │
       │                                              │ SPI_READY
       │                                              ▼
       │                                         SENDING_FRAMES
       │                                              │
       │                                              │ END 帧已发
       │                                              ▼
       │                                         WAIT_BITMAP_ACK
       │                                              │
       │                           ┌──────────────────┤
       │                           │                  │
       │                      ok=1 │             ok=0 │
       │                           │                  │
       │                           ▼                  ▼
       └─────────────────  TRANSFER_DONE      RETRANSMIT_MISSING
                                              (最多 3 次)
                                                   │
                                                   │ 重传完成
                                                   ▼
                                              WAIT_BITMAP_ACK
```

### 4.4 数据结构设计

#### 4.4.1 2800 侧关键数据结构

```c
/* SPI 帧头（19 字节） */
#define CAM_SPI_MAGIC          0x494D4731  /* "IMG1" */
#define CAM_SPI_HDR_SIZE       19
#define CAM_SPI_MAX_PAYLOAD    256
#define CAM_SPI_FRAME_SIZE     277

/* 命令码 */
#define CAM_CMD_CAPTURE_REQ    0x01
#define CAM_CMD_CAPTURE_ACK    0x02
#define CAM_CMD_CAPTURE_FAIL   0x03
#define CAM_CMD_GET_IMAGE_REQ  0x04
#define CAM_CMD_GET_IMAGE_ACK  0x05
#define CAM_CMD_SPI_READY      0x07
#define CAM_CMD_FRAME_ACK      0x08
#define CAM_CMD_SPI_READY_BIG  0x09

/* 全局状态变量 */
static int      cam_uart_fd = -1;          /* UART 设备描述符 */
static int      cam_spi_fd  = -1;          /* SPI 设备描述符 */
static volatile bool cam_busy = false;      /* 业务忙标志（防误触发） */
static volatile bool cam_spi_recv_active = false;  /* SPI 接收使能 */
static uint32_t cam_session_id = 0;        /* 会话 ID（每次拍照递增） */
static uint8_t  cam_thumb_buf[280*158*2];  /* 缩略图缓冲 (88480B) */
static uint32_t cam_thumb_total = 0;       /* 缩略图总字节数 */
static uint32_t cam_thumb_offset = 0;      /* 已收偏移 */
static uint8_t  cam_recv_bitmap[1024];     /* 帧接收位图 (支持 8192 帧) */
static uint16_t cam_recv_nframes = 0;      /* 已收帧数 */
static int      cam_big_fd = -1;           /* 原图文件描述符 */
static lv_timer_t *cam_timeout_timer;      /* 10s 拍照超时 */
static lv_timer_t *cam_spi_timeout_timer;  /* 50s SPI 超时 */
```

#### 4.4.2 2N 侧关键数据结构

```python
# 全局配置
UART_DEV = "/dev/ttyS3"
UART_BAUD = 921600
SPI_DEV = "/dev/spidev3.0"
SPI_SPEED = 24000000
SPI_MODE = 0
SPI_INTER_FRAME_DELAY = 0.003   # 3ms
SPI_RETRANSMIT_DELAY = 0.005    # 5ms
CAM_SPI_MAX_PAYLOAD = 256
MAX_RETRY = 3

# 全局状态
cmd_queue = queue.Queue()       # 命令队列（ack_reader → main）
ack_queue = queue.Queue()       # ACK 队列（ack_reader → spi_send_image）
```

#### 4.4.3 帧格式定义

**UART 帧格式**：

```
偏移  长度  字段      说明
0     2    SYNC     0xAA 0x55
2     2    LEN      CMD(1)+SEQ(1)+PAYLOAD(N) 大端
4     1    CMD      命令码
5     1    SEQ      序列号（固定 0）
6     N    PAYLOAD  负载
6+N   2    CRC16    CRC16-CCITT 大端（覆盖 LEN~PAYLOAD）
```

**SPI 帧格式**：

```
偏移  长度  字段      说明
0     4    MAGIC    0x494D4731 大端
4     1    TYPE     帧类型 (1=THUMB_DATA, 2=THUMB_END, 3=BIG_DATA, 4=BIG_END)
5     4    SESSION  会话 ID 大端
9     4    TOTAL    图像总字节 大端
13    4    OFFSET   本帧偏移 大端
17    2    LEN      payload 字节数 大端 (≤256)
19    N    PAYLOAD  实际数据
19+N  2    CRC16    CRC16-CCITT 大端（覆盖 MAGIC~PAYLOAD）
```

**Bitmap ACK 负载格式**：

```
偏移  长度  字段      说明
0     4    session   会话 ID 大端
4     4    offset    已收最大偏移 大端
8     1    ok        1=完整, 0=有缺失
9     2    nframes   总帧数 大端
11    M    bitmap    每位对应一帧 (1=已收, 0=缺失)
```

---

## 五、接口设计

### 5.1 硬件接口

#### 5.1.1 UART 接口

| 参数 | 2800 | 2N |
|------|------|-----|
| 设备路径 | `/dev/ttyS1` | `/dev/ttyS3` |
| 波特率 | 921600 | 921600 |
| 数据位 | 8 | 8 |
| 停止位 | 1 | 1 |
| 校验 | 无 | 无 |
| 流控 | 无 | 无 |
| 模式 | raw (ICANON/ECHO/ISIG 关闭) | raw |
| 连接 | TX↔RX, RX↔TX, GND↔GND | 同左 |

**IOMUX 配置**（2800 侧）：通过 `hal_iomux_set_uart()` 将物理引脚复用为 UART 功能。

#### 5.1.2 SPI 接口

| 参数 | 2800 (从机) | 2N (主机) |
|------|-------------|-----------|
| 设备路径 | `/dev/spislv0` | `/dev/spidev3.0` |
| 时钟频率 | 被动接收 | 24MHz |
| SPI 模式 | Mode 0 (CPOL=0, CPHA=0) | Mode 0 |
| 数据位宽 | 8 bit | 8 bit |
| CS 控制 | 被动（2N 控制） | 主动（GPIO 控制） |
| DMA 缓冲 | 4096 字节 (`SPI_SLAVE_BUFSIZE`) | 内核管理 |
| 信号线 | MOSI/MISO/SCLK/CS | MOSI/MISO/SCLK/CS |

**IOMUX 配置**（2800 侧）：通过 `hal_iomux_set_spi_slave()` 将引脚 32-35 复用为 SPI 从机功能，驱动强度 0xF，无上下拉。

**CS 中断配置**（2800 侧）：CS 引脚通过 IOExpander 配置为上升沿中断，传输结束时触发 `cs_interrupt_handler()`。

### 5.2 软件对外接口

#### 5.2.1 2800 侧对外接口

```c
/**
 * @brief  创建 Camera 页面
 * @param  parent 父容器
 * @retval Camera 页面对象指针
 * @note   LVGL 启动时调用，内部完成设备打开、DMA 预启动、线程创建
 */
lv_obj_t *create_camera_page(lv_obj_t *parent);

/**
 * @brief  拍照按钮回调
 * @param  e LVGL 事件对象
 * @note   用户点击触发，发送 CAPTURE_REQ，启动 10s 超时
 */
void cam_capture_btn_cb(lv_event_t *e);

/**
 * @brief  获取按钮回调
 * @param  e LVGL 事件对象
 * @note   用户点击触发，发送 GET_IMAGE_REQ
 */
void cam_get_btn_cb(lv_event_t *e);
```

#### 5.2.2 2N 侧对外接口

```python
def main():
    """主循环，从 cmd_queue 获取命令并分发"""

def do_capture():
    """执行拍照，生成缩略图，回复 CAPTURE_ACK"""

def spi_send_image(spi, data, session_id, frame_type_data):
    """
    通过 SPI 发送图像数据（末尾 Bitmap 汇总校验）
    @param spi: spidev 对象
    @param data: 图像字节数据
    @param session_id: 会话 ID
    @param frame_type_data: 数据帧类型 (1=THUMB, 3=BIG)
    @return: True 成功, False 失败
    """

def build_spi_frame(frame_type, session_id, total, offset, payload):
    """构造 SPI 帧（magic + hdr + payload + crc）"""

def build_frame(cmd, seq, payload):
    """构造 UART 帧（sync + len + cmd + seq + payload + crc）"""
```

### 5.3 内部接口

#### 5.3.1 2800 侧内部函数调用关系

```
create_camera_page()
  ├─ cam_uart_open()
  ├─ cam_uart_send_frame()
  ├─ cam_uart_rx_thread_func()  ──► cam_handle_capture_ack()
  │                                    ├─ cam_uart_send_frame(SPI_READY)
  │                                    └─ cam_start_spi_timeout()
  ├─ cam_spi_rx_thread_func()   ──► cam_handle_spi_frame()
  │                                    ├─ cam_crc16_ccitt()
  │                                    ├─ cam_update_bitmap()
  │                                    └─ cam_send_bitmap_ack()
  ├─ cam_capture_btn_cb()       ──► cam_uart_send_frame(CAPTURE_REQ)
  ├─ cam_get_btn_cb()           ──► cam_uart_send_frame(GET_IMAGE_REQ)
  ├─ cam_timeout_cb()           ──► cam_reset_state()
  ├─ cam_spi_timeout_cb()       ──► cam_spi_reopen()
  └─ cam_show_thumbnail_async_cb()  (LVGL 主线程执行)
```

#### 5.3.2 2N 侧内部函数调用关系

```
main()
  ├─ check_uart_baudrate()
  ├─ ack_reader_thread()  ──► cmd_queue / ack_queue
  └─ cmd_queue.get()
       ├─ CAPTURE_REQ ──► do_capture()
       │                    ├─ gst_capture()
       │                    ├─ make_thumbnail_rgb565()
       │                    └─ send_capture_ack()
       ├─ SPI_READY ────► spi_send_image()
       │                    ├─ build_spi_frame()
       │                    ├─ spi.xfer2()
       │                    ├─ wait_bitmap_ack()
       │                    └─ _send_frames_by_offsets()
       └─ SPI_READY_BIG ► spi_send_image() (原图)
```

### 5.4 数据交互协议

#### 5.4.1 UART 命令码定义

| CMD | 名称 | 方向 | Payload | 说明 |
|-----|------|------|---------|------|
| 0x01 | CAPTURE_REQ | 2800→2N | 空 | 请求拍照 |
| 0x02 | CAPTURE_ACK | 2N→2800 | filename[32]+w[2]+h[2]+size[4] | 拍照成功 |
| 0x03 | CAPTURE_FAIL | 2N→2800 | err_code[1] | 拍照失败 |
| 0x04 | GET_IMAGE_REQ | 2800→2N | 空 | 请求原图 |
| 0x05 | GET_IMAGE_ACK | 2N→2800 | filename[32]+size[4] | 原图信息 |
| 0x07 | SPI_READY | 2800→2N | session_id[4] | 就绪传缩略图 |
| 0x08 | FRAME_ACK | 2800→2N | Bitmap ACK 负载 | 帧确认（含 bitmap） |
| 0x09 | SPI_READY_BIG | 2800→2N | session_id[4] | 就绪传原图 |

#### 5.4.2 SPI 帧类型定义

| TYPE | 名称 | Payload | 触发动作 |
|------|------|---------|----------|
| 1 | THUMB_DATA | 256B（或末帧不足） | 2800 写入 thumb_buf，标记 bitmap |
| 2 | THUMB_END | 空 | 2800 遍历 bitmap，发 FRAME_ACK |
| 3 | BIG_DATA | 256B（或末帧不足） | 2800 write 到 cam_big_fd |
| 4 | BIG_END | 空 | 2800 遍历 bitmap，发 FRAME_ACK |

#### 5.4.3 时序要求

| 参数 | 值 | 说明 |
|------|-----|------|
| UART 命令超时 | 10s | `cam_timeout_timer` |
| SPI 整体超时 | 50s | `cam_spi_timeout_timer` |
| Bitmap ACK 超时 | 3s | 2N 侧 `wait_bitmap_ack` |
| 帧间延时 | 3ms | `SPI_INTER_FRAME_DELAY` |
| 重传延时 | 5ms | `SPI_RETRANSMIT_DELAY` |
| 最大重试 | 3 | `max_attempts` |
| UART 发送延时 | 5ms | `cam_uart_send_frame` 内部 |
| SPI 重开延时 | 50ms | `cam_spi_reopen` 中 close→open 间 |

---

## 六、关键算法与核心逻辑设计

### 6.1 核心算法原理

#### 6.1.1 CRC16-CCITT 校验算法

用于 UART 帧和 SPI 帧的数据完整性校验，多项式 `0x1021`，初值 `0xFFFF`。

```c
uint16_t cam_crc16_ccitt(const uint8_t *data, uint32_t len)
{
    uint16_t crc = 0xFFFF;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int j = 0; j < 8; j++) {
            if (crc & 0x8000) crc = (crc << 1) ^ 0x1021;
            else              crc <<= 1;
        }
    }
    return crc;
}
```

#### 6.1.2 末尾 Bitmap 汇总校验算法

详见 [SPI校验机制演进总结.md](file:///home/lzw/code/si8658ca/openvela_003_0623/openvela_si8658ca/doc/SPI校验机制演进总结.md) 的演进对比。

**原理**：每帧接收成功后置位对应 bit，END 帧到来时遍历 bitmap 找出缺失帧，一次性回传给发送方选择性重传，避免逐帧 ACK 的 UART 通信开销。

**Bitmap 维护**：
```c
/* 每帧 CRC 校验成功后 */
uint16_t fidx = offset / CAM_SPI_MAX_PAYLOAD;
cam_recv_bitmap[fidx >> 3] |= (1 << (fidx & 7));
if (fidx + 1 > cam_recv_nframes) cam_recv_nframes = fidx + 1;

/* END 帧到来时生成 Bitmap ACK */
uint16_t expected_nframes = (cam_thumb_total + CAM_SPI_MAX_PAYLOAD - 1) 
                            / CAM_SPI_MAX_PAYLOAD;
uint8_t thumb_ok = 1;
for (uint16_t fi = 0; fi < expected_nframes; fi++) {
    if (!(cam_recv_bitmap[fi >> 3] & (1 << (fi & 7)))) {
        thumb_ok = 0;
        break;
    }
}
```

#### 6.1.3 RGB565 缩略图生成算法

2N 侧通过 Python PIL 将 JPEG 解码 → 缩放为 280×158 → 转 RGB565。

```python
def make_thumbnail_rgb565(jpeg_path, w=280, h=158):
    img = Image.open(jpeg_path).convert('RGB').resize((w, h))
    pixels = img.load()
    rgb565 = bytearray(w * h * 2)
    for y in range(h):
        for x in range(w):
            r, g, b = pixels[x, y]
            # R:5 G:6 B:5
            rgb565[(y * w + x) * 2]     = ((r & 0xF8) | (g >> 5))
            rgb565[(y * w + x) * 2 + 1] = ((g << 3) & 0xE0 | (b >> 3))
    return bytes(rgb565)
```

#### 6.1.4 SPI 帧 Magic 字节对齐算法

由于 SPI Slave DMA 接收可能首字节错位，2800 接收线程在 stream_buf 中**循环查找 magic `0x494D4731`**：

```c
uint32_t pos = 0;
while (pos + CAM_SPI_HDR_SIZE <= nread) {
    uint32_t magic = ntohl(*(uint32_t*)(stream_buf + pos));
    if (magic != CAM_SPI_MAGIC) {
        pos++;  /* 错位，前移 1 字节继续找 */
        continue;
    }
    /* 校验 CRC、处理帧... */
    pos += frame_total_len;
}
```

#### 6.1.5 DMA 接收完成检测算法

2800 SPI Slave 通过 CS 上升沿中断判断传输结束：
- CS 拉低：传输开始
- CS 拉高（上升沿）：传输结束，触发 IRQ → work_queue → `schedule_transfer(RX_DONE)`
- 读取 `des_cur_addr` 计算 `trans_len = des_cur_addr - dma_buf_phys`

### 6.2 算法参数设计与配置

| 参数 | 值 | 文件 | 说明 |
|------|-----|------|------|
| `CAM_SPI_MAX_PAYLOAD` | 256 | camera_page.c / camera_hub_2n.py | 每帧 payload 上限 |
| `CAM_SPI_FRAME_SIZE` | 277 | camera_page.c | 单帧总长（19+256+2） |
| `SPI_SLAVE_BUFSIZE` | 4096 | defconfig | DMA 单次接收缓冲 |
| `cam_recv_bitmap` | 1024B | camera_page.c | 支持 8192 帧（足够 2MB 数据） |
| SPI 重试次数 | 3 | camera_hub_2n.py | 单图最大重传次数 |
| 帧间延时 | 3ms | camera_hub_2n.py | 防止 2800 处理不及 |

### 6.3 优化策略

| 策略 | 收益 |
|------|------|
| **末尾汇总校验替代逐帧 ACK** | UART 通信次数从 N 降到 1，传输时间减少 40%+ |
| **DMA 零拷贝接收** | SPI 数据直接写入应用缓冲，避免 CPU 搬运 |
| **Bitmap 选择性重传** | 只重传缺失帧，不重传完整图，重传开销最小化 |
| **stream_buf 累积解析** | 单次 read 可能跨多帧，循环解析充分利用每次 DMA 数据 |
| **DMA 预启动** | open 时即启动 DMA，SPI 帧到达零延迟接收 |
| **lv_async_call 跨线程 UI** | 避免 SPI 线程直接操作 LVGL，保证线程安全 |
| **tcflush 清空 RX** | 拍照前清空 UART 残留，避免误解析旧帧 |
| **memset(rx_dma_buffer, 0)** | 每次 DMA 接收前清零，避免残留数据干扰 magic 查找 |

---

## 七、异常处理与可靠性设计

### 7.1 超时异常处理

| 场景 | 超时值 | 触发条件 | 处理动作 |
|------|--------|---------|---------|
| UART 拍照超时 | 10s | 发 CAPTURE_REQ 后无 ACK | `cam_timeout_cb`：复位 `cam_busy`，日志提示，按钮恢复 |
| SPI 整体超时 | 50s | 收到 SPI_READY 后无完整数据 | `cam_spi_timeout_cb`：复位状态，`cam_spi_reopen` 重启 DMA |
| Bitmap ACK 超时 | 3s | 2N 发 END 帧后无 FRAME_ACK | 2N 重传缺失帧（最多 3 次） |

**2800 SPI 超时处理实现**：
```c
static void cam_spi_timeout_cb(lv_timer_t *timer)
{
    cam_spi_recv_active = false;
    cam_busy = false;
    lv_label_set_text(cam_info_label, "SPI transfer timeout");
    cam_spi_reopen();  /* close + 50ms + open 重置 DMA */
    lv_obj_clear_state(cam_capture_btn, LV_STATE_DISABLED);
}
```

### 7.2 数据错误、校验失败处理

#### 7.2.1 UART CRC 校验失败

```c
/* uart_rx_thread 中 */
if (crc_calc != crc_recv) {
    cam_add_log("UART CRC fail");
    continue;  /* 丢弃该帧，不响应 */
}
```

#### 7.2.2 SPI CRC 校验失败

```c
/* spi_rx_thread 中 */
if (crc_calc != crc_recv) {
    pos++;  /* 不置位 bitmap，等待 END 帧汇总 */
    continue;
}
```

**关键设计**：CRC 失败的帧**不立即要求重传**，而是依赖末尾 Bitmap 汇总一次性处理，减少 UART 通信开销。

#### 7.2.3 Magic 字节错位

DMA 接收可能首字节错位（CS 时序抖动），通过 `pos++` 循环查找 magic 恢复同步。

### 7.3 硬件故障、信号丢失处理

#### 7.3.1 SPI DMA 地址异常

**现象**：2800 日志 `dmaexchange port=0 dma_recv ret=-5`，`trans_len` 异常。

**根因**：CS 中断在 DMA 未就绪时触发，`des_cur_addr` 为 NULL。

**处理**（`bes_spi_slave.c`）：
```c
if (priv->des_cur_addr == NULL) {
    /* 硬件状态异常，关闭再打开 SPI 复位 DMA */
    hal_spi_slave_close(priv);
    hal_spi_slave_open(priv);
    spislave_prepare_next_rx(priv);  /* 重新启动 DMA */
    return;
}
```

#### 7.3.2 SPI 设备重启（cam_spi_reopen）

```c
static void cam_spi_reopen(void)
{
    if (cam_spi_fd >= 0) {
        close(cam_spi_fd);       /* 触发 spislave_unbind()，停止 DMA */
        usleep(50 * 1000);       /* 等 UNBIND 完成 */
    }
    cam_spi_fd = open("/dev/spislv0", O_RDWR);  /* 触发 bind + DMA 预启动 */
}
```

#### 7.3.3 UART 波特率不匹配

2N 侧启动时强制校验波特率：
```python
def check_uart_baudrate():
    os.system(f"stty -F {UART_DEV} {UART_BAUD}")
    # 回读确认
    with open(f"/sys/class/tty/ttyS3/device/baud_rate", "r") as f:
        actual = int(f.read().strip())
    if actual != UART_BAUD:
        raise RuntimeError(f"Baudrate mismatch: {actual}")
```

### 7.4 重复触发、误触发防护

| 防护点 | 机制 |
|--------|------|
| 拍照按钮连击 | `cam_busy` 标志 + 按钮 `LV_STATE_DISABLED` |
| 拍照中再拍照 | 入口处 `if (cam_busy) return;` |
| 传输中重发 SPI_READY | `cam_spi_recv_active` 标志，未激活时不解析 SPI 数据 |
| 旧 ACK 误处理 | 每次 `cam_session_id++`，ACK 中携带 session 校验 |
| 旧 Bitmap ACK 误用 | 2N `wait_bitmap_ack` 校验 `sid == session_id` |

### 7.5 死机、跑飞、复位保护机制

| 机制 | 说明 |
|------|------|
| 看门狗 | NuttX 系统级 watchdog，应用卡死触发复位 |
| 线程隔离 | UART/SPI 线程独立，单线程异常不影响其他 |
| 资源释放 | 异常路径 `close(fd)` 释放设备，避免 fd 泄漏 |
| 状态复位 | 超时/DMA 错误后 `cam_busy=false`、`cam_spi_recv_active=false`，恢复 IDLE |
| 重启 SPI 硬件 | DMA 异常时 `hal_spi_slave_close/open` 硬件复位 |

---

## 八、资源占用与性能评估

### 8.1 Flash/ROM 占用

| 模块 | 2800 侧 | 2N 侧 |
|------|---------|--------|
| camera_page.o | ~30KB | - |
| spi_slave_driver.o | ~8KB | - |
| bes_spi_slave.o | ~12KB | - |
| Python 脚本 | - | ~15KB |
| **合计** | **~50KB** | **~15KB** |

### 8.2 RAM 内存占用

#### 2800 侧

| 项目 | 大小 | 说明 |
|------|------|------|
| `cam_thumb_buf` | 88480B | 缩略图缓冲（280×158×2） |
| `cam_recv_bitmap` | 1024B | 帧接收位图 |
| SPI DMA 缓冲 | 4096B | `SPI_SLAVE_BUFSIZE` |
| UART RX 线程栈 | 4096B | pthread 默认 |
| SPI RX 线程栈 | 4096B | pthread 默认 |
| stream_buf | 8192B | SPI 累积解析缓冲 |
| LVGL 控件 | ~20KB | 预览图、按钮、标签 |
| **合计** | **~130KB** | - |

#### 2N 侧

| 项目 | 大小 | 说明 |
|------|------|------|
| 缩略图数据 | 88480B | RGB565 缓冲 |
| 原图数据 | 1~1.3MB | JPEG 全量加载 |
| ACK 队列 | <1KB | - |
| **合计** | **~1.4MB** | Python 进程占用 |

### 8.3 CPU 耗时、任务占用率

| 任务 | CPU 占用 | 说明 |
|------|---------|------|
| LVGL 主线程 | ~5% | 空闲时低，刷新预览时短暂峰值 |
| UART RX 线程 | <1% | 阻塞在 `read()`，事件驱动 |
| SPI RX 线程 | 10~20% | 传输期间 `poll/read/解析` |
| 2N GStreamer | 短暂 30% | 拍照瞬间 |
| 2N SPI 发送 | ~15% | 传输期间 |

### 8.4 实时性、延迟指标评估

| 指标 | 实测 | 评估 |
|------|------|------|
| UART 命令往返 | ~50ms | 满足 <100ms 要求 |
| 拍照完成（含 GStreamer 预热） | ~3s | 满足 <5s 要求 |
| 缩略图传输（88KB, 346 帧） | ~13s | 满足 <15s 要求 |
| 原图传输（1MB, 4096 帧） | ~30s | 满足 <60s 要求 |
| SPI 帧处理延时 | <2ms | 单帧解析 + memcpy |
| UI 更新延时 | <50ms | `lv_async_call` 投递后下一帧渲染 |

---

## 九、模块测试方案

### 9.1 单元测试

| 测试项 | 方法 | 预期 |
|--------|------|------|
| CRC16 校验 | 输入已知数据，对比标准值 | 一致 |
| UART 帧编解码 | 构造/解析回环 | 字段一致 |
| SPI 帧编解码 | 构造/解析回环 | 字段一致 |
| Bitmap 置位/查询 | 模拟乱序帧 | bit 正确 |
| RGB565 转换 | 输入 RGB(255,0,0) | 输出 0xF800 |
| Magic 查找 | 构造错位 buffer | 正确找到 magic |

### 9.2 功能测试

#### 9.2.1 正常场景

| 用例 | 步骤 | 预期 |
|------|------|------|
| TC-01 拍照+缩略图 | 点击拍照 | 10s 内显示缩略图 |
| TC-02 原图获取 | 缩略图后点获取 | 60s 内 eMMC 存入原图 |
| TC-03 连续拍照 | 拍照完成后再拍 | 第二次正常 |
| TC-04 页面切换 | 右滑进入、左滑退出 | 切换流畅 |
| TC-05 日志显示 | 全程观察日志栏 | 实时更新 |

#### 9.2.2 边界场景

| 用例 | 步骤 | 预期 |
|------|------|------|
| TC-06 缩略图末帧 | offset 恰好为 256 整数倍 | END 帧正确触发 |
| TC-07 原图 1.3MB | 拍大图 | 4096+ 帧，bitmap 足够 |
| TC-08 拍照中切换页面 | 拍照时左滑 | 不影响传输，返回后正常显示 |
| TC-09 连续点击拍照 | 快速连点 | 仅第一次生效 |

### 9.3 异常测试

| 用例 | 模拟方式 | 预期 |
|------|---------|------|
| TC-10 UART 断线 | 拔掉 UART 线 | 10s 超时，按钮恢复 |
| TC-11 SPI 断线 | 拔掉 SPI 线 | 50s 超时，DMA 重启 |
| TC-12 CRC 注入错误 | 修改 payload 字节 | CRC 失败，bitmap 缺失，重传成功 |
| TC-13 DMA 地址异常 | CS 抖动模拟 | `hal_spi_slave_close/open` 自动恢复 |
| TC-14 2N 重启 | 传输中 kill 脚本 | 50s 超时，2800 恢复 IDLE |
| TC-15 摄像头拔出 | 拍照时拔 USB | 2N 回 CAPTURE_FAIL，2800 日志提示 |
| TC-16 Bitmap ACK 丢失 | 2N 发 END 后不处理 ACK | 3s 超时，重传最多 3 次后失败 |

### 9.4 整体联调测试

| 测试项 | 说明 |
|--------|------|
| 端到端拍照 | 2800 UI → 2N GStreamer → SPI → 2800 显示 |
| 长时间稳定性 | 连续拍照 50 次，无内存泄漏、无死锁 |
| 双机复位同步 | 2800 复位后 2N 状态清理，反之亦然 |
| 与 AI 页面共存 | Camera 页与 AI 页切换无相互干扰 |
| 多用户操作模拟 | 快速切换页面 + 连续拍照，无崩溃 |

---

## 十、本章小结

### 10.1 设计思路总结

本模块基于 **"UART 控制通道 + SPI 数据通道"双通道分离架构**，充分发挥两条总线的优势：
- **UART**：低带宽、高可靠，用于命令/ACK 等小数据量控制信令
- **SPI**：高带宽、DMA 加速，用于图像数据批量传输

通过**末尾 Bitmap 汇总校验**替代传统逐帧 ACK，将 UART 通信开销从 O(N) 降至 O(1)，传输效率提升 40%+，同时通过**选择性重传**保证数据完整性。

### 10.2 实现能力

| 能力 | 指标 |
|------|------|
| 拍照响应 | 10s 内完成（含 GStreamer 预热） |
| 缩略图传输 | 88KB / 13s |
| 原图传输 | 1.3MB / 30s |
| 可靠性 | CRC16 + Bitmap 双重校验 + 3 次重传 |
| 异常恢复 | 超时/DMA 错误自动复位，无需人工干预 |
| UI 体验 | 按钮防误触、实时日志、手势导航 |

### 10.3 稳定性与优势

**稳定性保障**：
1. **多级超时保护**（10s/50s/3s）确保任何异常都能恢复
2. **DMA 硬件复位**（close/open）应对底层硬件状态异常
3. **状态机 + 标志位**双重防护误触发
4. **跨线程 `lv_async_call`** 保证 LVGL 线程安全

**核心优势**：
1. **末尾 Bitmap 汇总校验**：业界少见的精巧设计，兼顾可靠性与效率
2. **DMA 预启动 + 零拷贝**：SPI 接收零延迟、零 CPU 搬运
3. **双机协议对称设计**：2800 与 2N 帧格式一致，便于维护
4. **模块化分层**：UI/协议/驱动清晰分离，便于移植与扩展

### 10.4 后续演进方向

| 方向 | 说明 |
|------|------|
| 流式传输 | 缩略图边收边显示，无需等全部完成 |
| 压缩传输 | SPI 传输 JPEG 而非 RGB565，2N 解码显示 |
| 多摄像头支持 | 扩展 session_id 区分不同摄像头 |
| 断点续传 | 持久化 bitmap，支持跨重启续传 |
| 加密传输 | SPI 帧加密，防止图像泄露 |

---

> **文档版本**：v1.0
> **最后更新**：2026-06-27
> **关联代码**：
> - 2800 侧：`rtos/apps/examples/lvgldemo/camera_page.c`
> - 2N 侧：`rtos/apps/examples/lvgldemo/camera_hub_2n.py`
> - SPI 驱动：`rtos/nuttx/drivers/spi/spi_slave_driver.c`、`chips/bes/bes_spi_slave.c`
> - 配置：`rtos/vendor/bes/boards/best1700_ep/glass_demo/configs/ap/defconfig`
