# SPI 大图传输校验机制演进技术文档

> **项目背景**：2N 侧（Linux，Python）通过 SPI 总线向 AP 侧（BES2800，NuttX RTOS）传输摄像头采集的缩略图（88KB）和大图（1~1.3MB）。SPI 工作在从机模式，DMA 传输，AP 侧接收数据后通过 UART（921600bps）反馈 ACK 给 2N 侧。
>
> **文档目的**：记录从"每帧校验 → 窗口校验 → 末尾汇总校验"的完整技术演进过程，沉淀经验，为后续项目（如视频流传输）提供参考。

---

## 一、演进历程概述

整个演进过程分为三个阶段，每个阶段都是对前一阶段瓶颈的直接回应：

| 阶段 | 机制 | 核心思想 | 触发演进的痛点 |
|------|------|----------|----------------|
| 一 | 每帧校验（Per-Frame ACK） | 每收一帧立即回 ACK | 1MB 大图需 9 分钟，UART 往返延迟成为瓶颈 |
| 二 | 窗口校验（Windowed ACK, W=6） | 批量发送 6 帧后累积确认 | Go-Back-N 整窗重传浪费带宽，窗口受缓冲限制 |
| 三 | 末尾 Bitmap 校验（End Bitmap ACK） | 全量发完后用 bitmap 精确指示缺失帧 | 选择性重传零浪费，UART 交互降至 1~3 次 |

### 1.1 帧结构基础（贯穿三种机制的公共定义）

无论采用哪种校验机制，系统都使用两套独立的帧格式：**UART 命令帧**（控制信道，2N↔AP 双向）和 **SPI 数据帧**（数据信道，2N→AP 单向）。此外，阶段三还引入了 **Bitmap ACK 负载格式**（承载在 UART 命令帧的 payload 中）。

#### 1.1.1 UART 命令帧格式

UART 帧用于 2N 与 AP 之间的命令交互（如拍照请求、SPI 就绪、ACK 反馈），采用"同步头 + 长度 + 命令 + 序列号 + 负载 + CRC"的结构。所有多字节字段均采用**大端序**。

```
字节偏移:  0    1    2    3    4    5    6         6+N-1  6+N  6+N+1
        +----+----+----+----+----+----+-----------+------+------+--+
        | sync       | frame_len  | cmd| seq| payload    | crc        |
        | 0xAA 0x55  | (2B, BE)   | (1)| (1)| (N B)       | (2B, BE)   |
        +----+----+----+----+----+----+-----------+------+------+--+
        |<--2B------>|<---2B----->|<1B>|<1B>|<---N B----->|<---2B---->|

总长度 = 2(sync) + 2(frame_len) + 1(cmd) + 1(seq) + N(payload) + 2(crc)
       = 6 + N + 2 = N + 8 字节
frame_len 字段值 = 2 + 1 + 1 + N = N + 4（不含 sync 和 crc）
```

**字段详解：**

| 偏移 | 长度 | 字段 | 取值/含义 | 说明 |
|------|------|------|-----------|------|
| 0 | 2 | sync | 固定 `0xAA 0x55` | 帧同步标志，接收方逐字节扫描直到匹配此模式才开始解析 |
| 2 | 2 | frame_len | `N + 4`（大端） | 后续字节数（cmd+seq+payload），不含 sync 和 crc。接收方据此预读剩余字节 |
| 4 | 1 | cmd | 见命令类型表 | 命令类型，决定 payload 的解析方式 |
| 5 | 1 | seq | 当前固定为 0 | 序列号，预留用于未来协议扩展（如命令去重、流水线） |
| 6 | N | payload | 变长 | 命令负载，N = frame_len - 4。不同 cmd 有不同 payload 结构 |
| 6+N | 2 | crc | CRC16-CCITT（大端） | 校验范围：frame_len + cmd + seq + payload（即偏移 2 到 6+N-1） |

**命令类型定义：**

| 命令 | 值 | 方向 | payload 结构 | 说明 |
|------|-----|------|--------------|------|
| `CAM_CMD_CAPTURE_REQ` | 0x01 | AP→2N | 空 | 请求 2N 拍照 |
| `CAM_CMD_CAPTURE_ACK` | 0x02 | 2N→AP | 文件名(变长) + thumb_w(2B) + thumb_h(2B) | 拍照完成，含缩略图尺寸 |
| `CAM_CMD_CAPTURE_FAIL` | 0x03 | 2N→AP | 空 | 拍照失败 |
| `CAM_CMD_GET_IMAGE_REQ` | 0x04 | AP→2N | 空 | 请求获取大图 |
| `CAM_CMD_GET_IMAGE_ACK` | 0x05 | 2N→AP | 文件名(变长) + big_size(4B) | 大图信息（文件名+大小） |
| `CAM_CMD_SPI_READY` | 0x07 | AP→2N | session_id(4B) | AP 已就绪，2N 可开始发缩略图 |
| `CAM_CMD_FRAME_ACK` | 0x08 | AP→2N | 见 §1.1.3 Bitmap ACK 负载 | 帧确认（含 bitmap，阶段三使用） |
| `CAM_CMD_SPI_READY_BIG` | 0x09 | AP→2N | session_id(4B) | AP 已就绪，2N 可开始发大图 |

**CRC 算法：** CRC16-CCITT，初始值 `0xFFFF`，多项式 `0x1021`，2N 侧（Python）和 AP 侧（C）实现完全一致。

#### 1.1.2 SPI 数据帧格式

SPI 帧用于实际图像数据传输（2N→AP 单向），采用"magic + 帧头 + 负载 + CRC"的结构。所有多字节字段均采用**大端序**。

```
字节偏移:  0         4    5    6         9        13        17   18        19+len  19+len+1
        +----------+----+----+---------+---------+---------+----+---------+--------+
        | magic    |type|sess| total   | offset  | len(2B) | payload       | crc(2B)   |
        | (4B)     |(1B)|(4B)| (4B)    | (4B)    | (BE)    | (len B)       | (BE)      |
        +----------+----+----+---------+---------+---------+---------------+-----------+
        |<--4B---->|<1B>|<4B>|<--4B--->|<--4B--->|<--2B--->|<---len B----->|<---2B---->|

CAM_SPI_HDR_SIZE = 19  （前19字节：magic+type+session+total+offset+len）
CAM_SPI_MAX_PAYLOAD = 256
CAM_SPI_FRAME_SIZE = 19 + 256 + 2 = 277  （最大帧长）
```

**字段详解：**

| 偏移 | 长度 | 字段 | 取值/含义 | 说明 |
|------|------|------|-----------|------|
| 0 | 4 | magic | `0x494D4731`（ASCII "IMG1"） | 帧边界对齐标志。AP 侧 stream_buf 中逐字节扫描此 magic 定位帧起始。若 CRC 错误，pos++ 继续找下一个 magic |
| 4 | 1 | type | 1~4，见帧类型表 | 帧类型，区分缩略图/大图的数据帧和结束帧 |
| 5 | 4 | session | 会话 ID（大端） | 区分不同传输会话，由 AP 侧在 SPI_READY 命令中分配。ACK 帧需携带相同 session 供 2N 匹配 |
| 9 | 4 | total | 图像总字节数（大端） | 整张图片大小。AP 据此计算 `expected_nframes = (total + 255) / 256` |
| 13 | 4 | offset | 当前帧数据在图像中的起始偏移（大端） | `offset = frame_index × 256`。AP 据此计算文件写入位置和 bitmap 索引 `fidx = offset / 256` |
| 17 | 2 | len | payload 字节数（大端），≤256 | 最后一帧可能不足 256。`len = 0` 表示 END 帧 |
| 19 | len | payload | 实际图像数据 | END 帧 payload 为空 |
| 19+len | 2 | crc | CRC16-CCITT（大端） | 校验范围：hdr(19B) + payload(lenB)，即偏移 0 到 19+len-1 |

**帧类型定义：**

| 类型 | 值 | 含义 | payload | 触发动作 |
|------|-----|------|---------|----------|
| `CAM_SPI_TYPE_THUMB_DATA` | 1 | 缩略图数据帧 | 256B（或末帧不足） | AP 写入 thumb_buf，标记 bitmap |
| `CAM_SPI_TYPE_THUMB_END` | 2 | 缩略图结束帧 | 空 | AP 遍历 bitmap 生成 ACK，判断完整性 |
| `CAM_SPI_TYPE_BIG_DATA` | 3 | 大图数据帧 | 256B（或末帧不足） | AP lseek+write 到 big_fd，标记 bitmap |
| `CAM_SPI_TYPE_BIG_END` | 4 | 大图结束帧 | 空 | AP 遍历 bitmap 生成 ACK，判断完整性 |

**magic 字段的作用详解：**
- SPI 从机 DMA 接收是流式的，AP 侧 `read()` 返回的字节数可能不是整帧倍数
- stream_buf 中可能包含：半帧 + 整帧 + 半帧 的混合
- magic `0x494D4731` 是帧边界对齐的唯一依据
- 解析流程：从 pos 位置读 4 字节，若 ≠ magic 则 `pos++` 继续扫描
- CRC 错误时也用 `pos++` 恢复（而非清空缓冲区），靠 magic 重新对齐

#### 1.1.3 Bitmap ACK 负载格式（阶段三引入）

Bitmap ACK 承载在 `CAM_CMD_FRAME_ACK`（0x08）命令的 payload 中，AP 侧在收到 END 帧时构建并发送：

```
字节偏移:  0         4         8    9        11          11+bmp_len-1
        +----------+---------+----+--------+--------------+
        | session  | offset  | ok | nframes| bitmap       |
        | (4B, BE) | (4B,BE) |(1B)| (2B,BE)| (bmp_len B)  |
        +----------+---------+----+--------+--------------+
        |<--4B---->|<--4B--->|<1B>|<--2B-->|<--bmp_len--->|

总长度 = 4 + 4 + 1 + 2 + bmp_len = 11 + bmp_len 字节
bmp_len = (nframes + 7) / 8  （向上取整到字节）
```

| 偏移 | 长度 | 字段 | 说明 |
|------|------|------|------|
| 0 | 4 | session | 会话 ID（大端），2N 用于匹配当前传输会话 |
| 4 | 4 | offset | AP 已收数据最大偏移（大端），辅助判断 |
| 8 | 1 | ok | 1=全部收到，0=有缺失帧。**注意**：ok=0 但 missing=0 时数据也可能完整（最后帧 payload 不足导致 offset<total） |
| 9 | 2 | nframes | 总帧数（大端），`nframes = (total + 255) / 256` |
| 11 | bmp_len | bitmap | 每个 bit 对应一帧：**1=已收，0=缺失**。bit 0 对应 frame 0，bit 7 对应 frame 7，byte 1 bit 0 对应 frame 8，依此类推 |

**bitmap 初始化与更新：**
- AP 侧 `cam_recv_bitmap[1024]` 初始化为全 0（1KB，支持最多 8192 帧 ≈ 2MB）
- 每收到一帧 CRC 校验通过：`cam_recv_bitmap[fidx >> 3] |= (1 << (fidx & 7))`
- END 时遍历 bitmap：若有任意 bit=0，则 ok=0，2N 需重传对应帧

**2N 侧解析 bitmap 计算缺失帧：**
```python
for fi in range(nframes):
    byte_idx = fi >> 3
    bit_idx = fi & 7
    if not (bitmap[byte_idx] & (1 << bit_idx)):
        missing_offsets.append(fi * CAM_SPI_MAX_PAYLOAD)  # 计算缺失帧的 offset
```

---

### 1.2 阶段一：每帧校验机制（Per-Frame ACK）

#### 1.2.1 设计初衷与背景

最初协议设计追求"最强可靠性"——每收一帧立即校验并回 ACK。这是最直观的停等协议（Stop-and-Wait）：

- **设计目标**：零数据丢失，每帧即确认
- **适用假设**：传输量小（初期只传缩略图 88KB）、链路质量未知、需要实时反馈
- **选型依据**：实现最简单，AP 侧不需要维护 bitmap 或窗口状态，2N 侧串行发送逻辑清晰

#### 1.2.2 2800 与 2N 的完整交互流程

以下是缩略图传输的完整交互时序，标注了每一步的方向和耗时：

```
AP(2800, NuttX)                         2N(Linux, Python)
   |                                         |
   |  ① 用户点击拍照按钮                      |
   |  ② AP 通过 UART 发送拍照请求             |
   |----- CAPTURE_REQ (UART 0x01) ---------->|  2N: 解析命令，调用摄像头拍照
   |                                         |  2N: 生成缩略图(88KB) + 大图(1.3MB)
   |                                         |  2N: 通过 UART 回复拍照完成
   |<---- CAPTURE_ACK (UART 0x02) -----------|  payload: 文件名 + thumb_w + thumb_h
   |                                         |
   |  ③ AP 初始化 SPI 从机接收                |
   |     - 分配 stream_buf                    |
   |     - 启动 SPI DMA 接收                  |
   |  ④ AP 通过 UART 通知 2N 可以发送         |
   |----- SPI_READY (UART 0x07) ------------>|  payload: session_id
   |                                         |  2N: 收到 SPI_READY，开始发送
   |                                         |
   |  ⑤ [SPI 传输开始，每帧一次 ACK 交互]      |
   |                                         |  2N: 构建 SPI 帧
   |                                         |  2N: spi.xfer2(frame) 发送
   |<==== frame#1 (SPI THUMB_DATA) =========|  2N: time.sleep(3ms)
   |                                         |  2N: 阻塞调用 wait_uart_ack()
   |  AP: DMA 接收完成，read() 取数据          |       ↓ 轮询 ser.read() 每10ms一次
   |  AP: 解析 magic + 帧头                    |
   |  AP: CRC 校验                             |
   |  AP: memcpy 到 thumb_buf                  |
   |  AP: 构建 UART ACK 帧                     |
   |  AP: cam_uart_send_frame() 含 usleep(5ms)|
   |----- FRAME_ACK (UART 0x08, ok=1) ------>|  2N: ser.read() 读到 ACK 字节
   |                                         |  2N: Python 解析 ACK 帧（转义/sync/CRC）
   |                                         |  2N: 提取 ok=1，准备发下一帧
   |                                         |
   |<==== frame#2 (SPI THUMB_DATA) =========|  2N: spi.xfer2(frame2)
   |                                         |  ... 重复上述流程 ...
   |----- FRAME_ACK (UART 0x08, ok=1) ------>|
   |                                         |
   |          ... 共 346 帧（88KB/256）...     |
   |                                         |
   |<==== frame#346 (SPI THUMB_END) ========|  2N: 发送 END 帧
   |----- FRAME_ACK (UART 0x08, ok=1) ------>|
   |                                         |
   |  ⑥ AP 显示缩略图                          |  2N: 传输完成
   |     lv_async_call(show_thumbnail)        |
```

**关键交互细节：**
- 每帧都是"SPI 发送 → AP 处理 → UART ACK → 2N 解析"的串行流水线
- 2N 侧 `wait_uart_ack()` 是**阻塞调用**，发送线程在此等待，无法做其他工作
- AP 侧 `cam_uart_send_frame()` 内部有 `usleep(5000)`（5ms），是为了给 UART 发送留缓冲时间
- 2N 侧 `ser.read()` 是非阻塞读取，需要轮询（polling），轮询间隔约 10ms

#### 1.2.3 核心实现逻辑

**2N 侧（Python）—— 串行发送 + 阻塞等待 ACK：**
```python
def spi_send_image_perframe(ser, spi, data, session_id, frame_type_data):
    """阶段一：每帧发送后阻塞等待 ACK"""
    total = len(data)
    frame_type_end = frame_type_data + 1
    offset = 0
    frame_idx = 0
    
    while offset < total:
        chunk = data[offset:offset + CAM_SPI_MAX_PAYLOAD]
        frame = build_spi_frame(frame_type_data, session_id, total, offset, chunk)
        frame_idx += 1
        
        # 发送一帧
        spi.xfer2(list(frame))           # SPI 传输（含 Python→C 调用开销）
        time.sleep(0.003)                # 帧间延时 3ms，等 AP DMA 接收完成
        
        # 阻塞等待 UART ACK
        ack = wait_uart_ack(ser, session_id, timeout=1.0)
        if ack is None:
            # 超时，重发当前帧
            print(f"[WARN] frame#{frame_idx} ACK timeout, resend")
            continue
        if not ack.ok:
            # NACK，重发当前帧
            print(f"[WARN] frame#{frame_idx} NACK, resend")
            continue
        
        offset += len(chunk)             # ACK 成功，发下一帧
    
    # 发送 END 帧
    end_frame = build_spi_frame(frame_type_end, session_id, total, total, b"")
    spi.xfer2(list(end_frame))
    wait_uart_ack(ser, session_id, timeout=1.0)
```

**AP 侧（C）—— 每帧即回 ACK：**
```c
// SPI 接收线程主循环
while (cam_spi_recv_active) {
    // 从 SPI 驱动读取数据到 stream_buf
    int n = read(spi_fd, stream_buf + stream_len, 
                 sizeof(stream_buf) - stream_len);
    if (n > 0) stream_len += n;
    
    // 解析 stream_buf 中的帧
    int pos = 0;
    while (stream_len - pos >= CAM_SPI_HDR_SIZE) {
        uint32_t magic = /* 读取 4 字节 */;
        if (magic != CAM_SPI_MAGIC) { pos++; continue; }
        
        uint8_t type = stream_buf[pos + 4];
        uint32_t session = /* 读取 */;
        uint32_t total = /* 读取 */;
        uint32_t offset = /* 读取 */;
        uint16_t len = /* 读取 2 字节大端 */;
        
        uint16_t frame_total = CAM_SPI_HDR_SIZE + len + 2;
        if (stream_len - pos < frame_total) break;  // 数据不完整，等下次
        
        // CRC 校验
        uint16_t crc_calc = crc16_ccitt(stream_buf + pos, CAM_SPI_HDR_SIZE + len);
        uint16_t crc_recv = /* 读取最后 2 字节 */;
        
        if (crc_calc != crc_recv) {
            // CRC 错误：回 NACK，清空缓冲区（阶段一的实现）
            cam_send_frame_ack(session, offset, 0);  // NACK
            stream_len = 0;  // 清空缓冲区
            break;
        }
        
        // 处理数据
        if (type == CAM_SPI_TYPE_THUMB_DATA) {
            memcpy(cam_thumb_buf + offset, stream_buf + pos + CAM_SPI_HDR_SIZE, len);
            cam_thumb_offset += len;
        } else if (type == CAM_SPI_TYPE_BIG_DATA) {
            write(cam_big_fd, stream_buf + pos + CAM_SPI_HDR_SIZE, len);
            cam_big_offset += len;
        }
        
        // 回 ACK
        cam_send_frame_ack(session, offset, 1);  // ACK
        
        pos += frame_total;
    }
    // 迁移残留数据到 buffer 开头
    memmove(stream_buf, stream_buf + pos, stream_len - pos);
    stream_len -= pos;
}
```

#### 1.2.4 传输时间详细计算（含 2N Python 转义时间）

**单帧传输耗时逐项分解（大图模式）：**

| 序号 | 耗时项 | 所在侧 | 耗时 | 说明 |
|------|--------|--------|------|------|
| ① | SPI 传输 277B @ 24MHz | 物理 | ~0.1ms | 277×8/24M = 92μs |
| ② | 2N `spi.xfer2()` Python→C 调用开销 | 2N | ~5ms | list 转换、C 扩展调用、返回值处理 |
| ③ | 2N `time.sleep(0.003)` | 2N | 3ms | 帧间延时，等 AP DMA 接收完成 |
| ④ | AP `read()` 从 SPI 驱动取数据 | AP | ~1ms | 系统调用 + memcpy |
| ⑤ | AP 解析帧头 + CRC 计算 | AP | ~1ms | 19 字节头解析 + CRC16 运算 |
| ⑥ | AP `write()` 到文件系统 | AP | **~20ms** | littlefs 写放大：256B 数据写入触发元数据更新+擦除 |
| ⑦ | AP 构建 UART ACK 帧 | AP | ~1ms | 打包 sync+len+cmd+seq+crc |
| ⑧ | AP `cam_uart_send_frame()` 中 `usleep(5000)` | AP | **5ms** | UART 发送后强制延时，防止缓冲溢出 |
| ⑨ | UART 传输 ACK 帧 11B @ 921600bps | 物理 | ~0.1ms | 11×8/921600 = 96μs |
| ⑩ | **2N `ser.read()` 轮询等待** | 2N | **~15ms** | 非阻塞读取，轮询间隔 10ms，可能需 1-2 次轮询才读到完整 ACK |
| ⑪ | **2N Python 解析 ACK 帧（转义时间）** | 2N | **~8ms** | 逐字节扫描 sync(0xAA55) + 提取 frame_len + 累积 payload + CRC 校验 + 字段解包。Python 解释执行，比 C 慢 10-50 倍 |
| ⑫ | 2N 判断 ok 并准备下一帧 | 2N | ~2ms | if 分支 + 下一帧 chunk 切片 + build_spi_frame |
| ⑬ | Python GC + Linux 调度开销 | 2N | **~78ms** | 大量帧传输时 Python GC 频繁触发 + Linux 进程调度延迟。这是理论与实际的差距来源 |
| | **单帧总耗时（大图）** | | **~135ms** | ①~⑬ 累加 |

**单帧传输耗时（缩略图模式，无文件写入）：**

| 耗时项 | 缩略图 | 大图 | 差异原因 |
|--------|--------|------|----------|
| ①~⑤ | ~8ms | ~8ms | 相同 |
| ⑥ 文件写入 | 0（memcpy） | ~20ms | 缩略图用内存 memcpy，大图用 littlefs write |
| ⑦~⑨ | ~6ms | ~6ms | 相同 |
| ⑩~⑪ 2N 轮询+解析 | ~23ms | ~23ms | 相同 |
| ⑫~⑬ 系统开销 | ~7ms | ~78ms | 大图帧数多，GC 和调度开销更大 |
| **总计** | **~38ms** | **~135ms** | |

**关键瓶颈分析——为什么 2N Python 转义时间不可忽略：**

很多分析只计算了 SPI 传输时间（0.1ms）和 UART 传输时间（0.1ms），得出"应该很快"的错误结论。实际上，**2N 侧 Python 解析 UART ACK 帧的时间（⑩+⑪ ≈ 23ms）是隐藏的最大瓶颈之一**：

1. **`ser.read()` 轮询粒度**：pyserial 的非阻塞 `read()` 依赖 Linux termios，最小轮询间隔约 10ms。即使 ACK 在 0.1ms 内到达，2N 也可能需要等 10ms 才发现
2. **Python 帧解析开销**：每收到一批字节，需要：
   - 逐字节扫描找 sync `0xAA55`（Python for 循环，每次迭代~1μs）
   - 读取 frame_len，判断是否收齐完整帧
   - 若不完整，保留 leftover 等下次 read
   - 收齐后做 CRC16 校验（Python 实现，比 C 慢 20 倍）
   - `struct.unpack()` 解包 cmd/seq/payload
3. **GIL 限制**：Python 全局解释器锁，帧解析期间无法并行发送下一帧

**总传输时间计算：**

| 数据类型 | 大小 | 帧数 | 单帧耗时 | 总耗时 | 实测验证 |
|----------|------|------|----------|--------|----------|
| 缩略图 | 88KB | 346 | ~38ms | 346×38ms = **13.1s** | ✅ 实测 ~13s |
| 大图 | 1MB | 4096 | ~135ms | 4096×135ms = **553s ≈ 9.2min** | ✅ 实测 ~9min |
| 大图 | 1.3MB | 5265 | ~135ms | 5265×135ms = **711s ≈ 11.9min** | ✅ 实测 ~12min |

> **结论**：每帧 ACK 机制下，1MB 大图需要 9 分钟，完全不可用。瓶颈不是 SPI 带宽（0.1ms/帧），而是**串行流水线中 13 个环节的累加延迟**，其中 2N Python 轮询+解析（23ms）、littlefs 写入（20ms）、系统调度开销（78ms）是三大主因。UART ACK 往返本身（0.2ms）不是瓶颈，但"每帧都等 ACK"的串行模式放大了所有延迟。

---

### 1.3 阶段二：滑动窗口 + 累积 ACK（Windowed Cumulative ACK）

#### 1.3.1 设计初衷与演进动机

阶段一分析表明，**根本矛盾是"每帧 ACK"的串行流水线**。即使优化单个环节，只要"发一帧→等一帧 ACK"的模式不变，单帧 135ms 的延迟就无法消除。

借鉴 TCP 滑动窗口思想，引入"批量发送 + 累积确认"：

- **核心改进 1**：2N 侧一次性发送 W=6 帧，不等 ACK
- **核心改进 2**：AP 侧每收到 W 帧回一次累积 ACK
- **核心改进 3**：2N 侧用独立线程收 ACK，消除 10ms 轮询粒度

**窗口大小 W=6 的选择依据：**
- AP 侧 `stream_buf = CAM_SPI_FRAME_SIZE * 2 = 554B`（仅 2 帧缓冲）
- AP 侧 `read()` 一次最多取 554B，即 2 帧
- 为留余量，W=6 时 AP 侧会多次 read() 累积处理，实测稳定
- W > 6 时 stream_buf 溢出风险增大，需同步扩大缓冲

#### 1.3.2 2800 与 2N 的完整交互流程

```
AP(2800, NuttX)                         2N(Linux, Python)
   |                                         |
   |----- SPI_READY (UART 0x07) ------------>|  2N: 收到就绪信号
   |                                         |  2N: 启动 ack_reader_thread 后台线程
   |                                         |
   |  [窗口1: 2N 连续发 6 帧，不等 ACK]        |
   |                                         |  2N 主线程: for i in range(6):
   |<==== frame#1 (SPI) ====================>|    spi.xfer2(frame1); sleep(3ms)
   |<==== frame#2 (SPI) ====================>|    spi.xfer2(frame2); sleep(3ms)
   |<==== frame#3 (SPI) ====================>|    spi.xfer2(frame3); sleep(3ms)
   |<==== frame#4 (SPI) ====================>|    spi.xfer2(frame4); sleep(3ms)
   |<==== frame#5 (SPI) ====================>|    spi.xfer2(frame5); sleep(3ms)
   |<==== frame#6 (SPI) ====================>|    spi.xfer2(frame6); sleep(3ms)
   |                                         |  2N 主线程: 等待累积 ACK
   |  AP: 逐帧处理（CRC+写入），不回 ACK       |    ↓ ack_queue.get(timeout=3.0)
   |  AP: 收到第 6 帧，触发累积 ACK            |
   |  AP: 构建 ACK(off=1536, ok=1)            |
   |----- FRAME_ACK (UART 0x08) ------------>|  2N ACK线程: ser.read() + 解析
   |    payload: off=1536, ok=1              |  2N ACK线程: ack_queue.put((sid,1536,1))
   |                                         |  2N 主线程: 收到 ACK，ok=1
   |                                         |  2N 主线程: base += 6, 窗口滑动
   |                                         |
   |  [窗口2: 连续发 frame#7~#12]              |
   |<==== frame#7~#12 (SPI) =================>|  ...
   |                                         |  2N: 等 ACK
   |  AP: 第 10 帧 CRC 错误                    |
   |  AP: 收到第 12 帧，触发累积 NACK          |
   |----- FRAME_ACK (UART 0x08) ------------>|  2N ACK线程: 解析 NACK
   |    payload: off=2560, ok=0              |  2N 主线程: ok=0, base = 2560/256 = 10
   |                                         |  2N 主线程: 从第 10 帧重传
   |                                         |
   |  [重传窗口: frame#10~#15]                 |
   |<==== frame#10~#15 (SPI) ================>|  2N: 发 6 帧
   |----- FRAME_ACK (UART 0x08, off=3840) --->|  2N: 收到 ACK, ok=1, base=15
   |                                         |
   |          ... 重复直到 END ...             |
   |<==== THUMB_END (SPI) ===================>|  2N: 发 END 帧
   |----- FRAME_ACK (UART 0x08, ok=1) ------>|  2N: 收到最终 ACK
   |  [显示缩略图]                             |  [传输完成]
```

**关键交互细节：**
- 2N 侧**双线程模型**：主线程负责发送，`ack_reader_thread` 后台线程持续读 UART
- 线程间通过 `queue.Queue` 通信，线程安全
- AP 侧每 6 帧回一次 ACK，UART 交互减少 6 倍
- NACK 时 2N 侧 `base = ack_off // 256`，从错误帧开始重传（Go-Back-N）

#### 1.3.3 核心实现逻辑

**2N 侧（Python）—— 双线程：发送线程 + ACK 读取线程：**
```python
import threading, queue

ack_queue = queue.Queue()  # ACK 线程 → 发送线程的通信队列

def ack_reader_thread(ser):
    """后台线程：持续读 UART，解析 ACK/命令帧放入 queue
    
    关键改进：消除主线程中 ser.read() 的 10ms 轮询粒度，
    ACK 一到就被解析并放入 queue，主线程从 queue 取即可。
    """
    leftover = b""  # 跨 read() 的残留字节（帧可能被拆分到多次 read）
    while True:
        data = ser.read(128)  # 非阻塞读取，最多 128 字节
        if not data:
            continue
        leftover += data
        # 尝试从 leftover 中解析完整帧
        frames, leftover = parse_frames(leftover)  # 返回已解析帧列表 + 剩余字节
        for cmd, seq, payload in frames:
            if cmd == CMD_FRAME_ACK and len(payload) >= 9:
                sid = struct.unpack(">I", payload[0:4])[0]
                off = struct.unpack(">I", payload[4:8])[0]
                ok = payload[8]
                ack_queue.put((sid, off, ok))  # 放入队列，主线程取
            # ... 其他命令处理 ...

def spi_send_windowed(ser, spi, data, session_id, W=6):
    """阶段二：滑动窗口发送"""
    total = len(data)
    nframes = (total + CAM_SPI_MAX_PAYLOAD - 1) // CAM_SPI_MAX_PAYLOAD
    base = 0  # 窗口起始帧索引
    
    # 启动 ACK 读取线程（只需启动一次）
    t = threading.Thread(target=ack_reader_thread, args=(ser,), daemon=True)
    t.start()
    
    while base < nframes:
        # 一次性发送窗口内 W 帧
        end = min(base + W, nframes)
        for i in range(base, end):
            offset = i * CAM_SPI_MAX_PAYLOAD
            chunk = data[offset:offset + CAM_SPI_MAX_PAYLOAD]
            frame = build_spi_frame(frame_type_data, session_id, total, offset, chunk)
            spi.xfer2(list(frame))
            time.sleep(0.003)  # 帧间延时
        
        # 等待累积 ACK
        try:
            sid, off, ok = ack_queue.get(timeout=3.0)
        except queue.Empty:
            # 超时，重发当前窗口
            print(f"[WARN] window base={base} ACK timeout, resend")
            continue
        
        if ok:
            base = end  # 窗口滑动到下一组
        else:
            base = off // CAM_SPI_MAX_PAYLOAD  # NACK: 从错误帧重传
            print(f"[WARN] NACK at off={off}, base reset to {base}")
    
    # 发送 END 帧
    end_frame = build_spi_frame(frame_type_end, session_id, total, total, b"")
    spi.xfer2(list(end_frame))
    ack_queue.get(timeout=3.0)  # 等最终 ACK
```

**AP 侧（C）—— 每 W 帧回一次累积 ACK：**
```c
static uint32_t cam_window_count = 0;
static uint32_t cam_window_size = 6;

// 帧处理逻辑中（每收到一帧 DATA）：
cam_window_count++;
if (cam_window_count >= cam_window_size) {
    // 累积 6 帧，回一次 ACK
    cam_send_frame_ack(session, offset + len, 1);  // off = 当前帧末尾
    cam_window_count = 0;
}

// CRC 错误时：
if (crc_calc != crc_recv) {
    // 立即回 NACK，指示错误帧 offset
    cam_send_frame_ack(session, offset, 0);  // NACK
    stream_len = 0;  // 清空缓冲区（阶段二的实现，后续阶段三改为 pos++）
    break;
}
```

#### 1.3.4 性能分析与实测

**窗口模式下的单帧均摊耗时：**

| 耗时项 | 阶段一（每帧） | 阶段二（均摊到每帧） | 改进 |
|--------|---------------|---------------------|------|
| 2N 轮询+解析 ACK | 23ms | 23ms / 6 ≈ 3.8ms | 6 倍 |
| AP `usleep(5000)` | 5ms | 5ms / 6 ≈ 0.8ms | 6 倍 |
| UART 交互 | 0.2ms | 0.03ms | 6 倍 |
| SPI 传输 | 0.1ms | 0.1ms | 不变 |
| 2N sleep(3ms) | 3ms | 3ms | 不变 |
| 2N xfer2 开销 | 5ms | 5ms | 不变 |
| AP 文件写入 | 20ms | 20ms | 不变（瓶颈未消除） |
| 系统开销 | 78ms | ~30ms | 缓解（帧间无需完全串行） |
| **单帧总耗时** | **~135ms** | **~62ms** | **2.2 倍** |

**实测数据：**

| 数据类型 | 帧数 | 窗口数 | 每窗口耗时 | 总耗时（实测） |
|----------|------|--------|------------|----------------|
| 缩略图 | 346 | 58 | ~120ms | **~7s** |
| 大图 | 5265 | 878 | ~70ms | **~60s** |

#### 1.3.5 遗留问题

1. **Go-Back-N 浪费**：窗口内 1 帧 CRC 错误，6 帧全部重传，已收的 5 帧白白重发
2. **NACK 频繁回退**：SPI 初始化不稳定时，前 100 帧 CRC 错误率高，base 反复回退
3. **窗口受限**：AP 侧 `stream_buf = 554B`（2 帧），无法增大 W。W=6 时已靠多次 `read()` 累积
4. **stream_len=0 连坐问题**：CRC 错误时清空整个缓冲区，已正确帧数据丢失（详见 §3.2）

---

### 1.4 阶段三：末尾汇总 Bitmap ACK（End Bitmap ACK）

#### 1.4.1 设计初衷与演进动机

阶段二的窗口机制仍有两大根本问题：
1. **Go-Back-N 浪费**：1 帧错误导致整窗 6 帧重传，带宽利用率低
2. **窗口大小受限**：AP 侧缓冲只有 2 帧，W 无法增大，UART 交互次数仍多

彻底重构思路——**"先全量发完，最后用 bitmap 精确指示缺失帧，只重传缺失部分"**：

- **发送策略**：2N 连续发送所有帧，中途不等 ACK，仅帧间 3ms 延时
- **校验策略**：AP 侧每帧标记 bitmap（1=已收），END 时遍历 bitmap 生成缺失列表
- **重传策略**：Selective 重传——只重传 bitmap 中 bit=0 的帧，不重传已收帧
- **ACK 策略**：仅在 END 后回一次 bitmap ACK，UART 交互从 878 次降至 1~3 次

#### 1.4.2 2800 与 2N 的完整交互流程

```
AP(2800, NuttX)                         2N(Linux, Python)
   |                                         |
   |----- SPI_READY (UART 0x07) ------------>|  2N: 收到就绪信号
   |                                         |  2N: 确认 ack_reader_thread 运行中
   |                                         |
   |  [第1轮: 全量发送，中途不回 ACK]          |
   |                                         |  2N: for offset in range(0, total, 256):
   |<==== frame#1 (SPI) ====================>|    spi.xfer2(frame); sleep(3ms)
   |<==== frame#2 (SPI) ====================>|    (日志采样: 仅打印前3帧+最后1帧)
   |<==== frame#3 (SPI) ====================>|
   |          ... 346 帧 ...                  |
   |<==== frame#346 (SPI) ===================>|
   |                                         |  2N: 发送 END 帧
   |<==== THUMB_END (SPI) ===================>|  2N: _drain_ack_queue() 清空旧 ACK
   |                                         |  2N: wait_bitmap_ack(timeout=3.0)
   |  AP: 收到 END，计算 expected_nframes      |       ↓ 阻塞等待 ack_queue
   |  AP: 遍历 bitmap 检查完整性              |
   |  AP: 构建 bitmap ACK payload             |
   |  AP: ok=1 if bitmap全1 else ok=0        |
   |----- FRAME_ACK (UART 0x08) ------------>|  2N ACK线程: 解析 bitmap ACK
   |    payload: session+off+ok+nframes      |  2N ACK线程: 解析 bitmap 计算缺失帧
   |           +bitmap(44B for 346帧)        |  2N ACK线程: ack_queue.put((sid,off,ok,missing))
   |                                         |  2N 主线程: 从 queue 取到 ACK
   |                                         |
   |  [情况A: ok=1, missing=0 → 成功]         |  2N: if missing is None or len==0:
   |                                         |          return True  ✓ 传输完成
   |                                         |
   |  [情况B: ok=0, missing=6 → 重传]         |  2N: else:
   |                                         |      _send_frames_by_offsets(missing)
   |  [重传: 仅发送 6 个缺失帧]                |
   |<==== retrans off=0 (SPI) ==============>|  2N: 只发 bitmap 中 bit=0 的帧
   |<==== retrans off=768 (SPI) ============>|  2N: (缺失帧 offset = fidx × 256)
   |<==== retrans off=1280 (SPI) ===========>|
   |          ... 共 6 帧 ...                  |
   |<==== THUMB_END (SPI) ===================>|  2N: 重传完再发 END
   |                                         |  2N: wait_bitmap_ack(timeout=3.0)
   |  AP: 收到重传帧，lseek+write 到正确位置   |
   |  AP: bitmap 对应 bit 置 1                |
   |  AP: 收到 END，重新遍历 bitmap            |
   |----- FRAME_ACK (UART 0x08, ok=1) ------>|  2N: missing=0 → return True ✓
   |                                         |
   |  [显示缩略图]                             |  [传输完成]
```

**关键交互细节：**
- 全量发送阶段 2N **完全不等待 ACK**，仅帧间 3ms 延时
- AP 侧每帧处理时**标记 bitmap**，但不回 ACK
- 仅在收到 END 帧时，AP 侧才构建 bitmap ACK 并通过 UART 发送
- 2N 侧 `ack_reader_thread` 解析 bitmap，计算缺失帧 offset 列表
- 重传时 2N 只发缺失帧 + END，AP 侧 `lseek` 到正确位置写入
- 最多 3 轮 attempt（全量→重传→重传），每轮后 bitmap 越来越满

#### 1.4.3 核心实现逻辑

**2N 侧（Python）—— `spi_send_image()` 完整流程：**
```python
def spi_send_image(ser, spi, data, session_id, frame_type_data):
    """阶段三：全量发送 + bitmap ACK + 选择性重传"""
    total = len(data)
    frame_type_end = frame_type_data + 1
    max_attempts = 3  # 最多 3 轮尝试
    
    for attempt in range(max_attempts):
        # === 第1步：全量发送（第1轮）或重传缺失帧（第2+轮）===
        if attempt == 0:
            # 第1轮：全量发送所有帧
            t_start = time.time()
            offset = 0
            frame_idx = 0
            while offset < total:
                chunk = data[offset:offset + CAM_SPI_MAX_PAYLOAD]
                frame = build_spi_frame(frame_type_data, session_id, total, offset, chunk)
                frame_idx += 1
                is_last = (offset + len(chunk) >= total)
                # 日志采样：仅打印前3帧+最后1帧，避免刷屏
                if frame_idx <= 3 or is_last:
                    print(f"[DBG] frame#{frame_idx} off={offset} sz={len(frame)} pay={len(chunk)}")
                spi.xfer2(list(frame))
                offset += len(chunk)
                time.sleep(SPI_INTER_FRAME_DELAY)  # 3ms
            
            # 发送 END 帧
            end_frame = build_spi_frame(frame_type_end, session_id, total, total, b"")
            spi.xfer2(list(end_frame))
            t_send = time.time() - t_start
            print(f"[INFO] Sent {frame_idx} frames + END in {t_send:.3f}s (attempt {attempt+1})")
        else:
            # 第2+轮：仅重传缺失帧（missing 是 offset 列表）
            print(f"[INFO] Retransmit {len(missing)} frames (attempt {attempt+1})")
            for off in missing:
                chunk = data[off:off + CAM_SPI_MAX_PAYLOAD]
                frame = build_spi_frame(frame_type_data, session_id, total, off, chunk)
                spi.xfer2(list(frame))
                time.sleep(SPI_INTER_FRAME_DELAY)
            # 重传后发 END
            end_frame = build_spi_frame(frame_type_end, session_id, total, total, b"")
            spi.xfer2(list(end_frame))
        
        # === 第2步：等待 bitmap ACK ===
        _drain_ack_queue()  # 清空 queue 中的旧 ACK，避免读到上一轮的
        ok, ack_off, missing = wait_bitmap_ack(session_id, timeout=3.0)
        
        if ok:
            return True  # 全部收到，成功
        
        if missing is None:
            # ACK 超时，重试
            print(f"[WARN] ACK timeout, attempt {attempt+1}")
            time.sleep(SPI_RETRANSMIT_DELAY)
            continue
        
        n_missing = len(missing)
        print(f"[WARN] Bitmap NACK, offset={ack_off}, missing={n_missing} frames")
        
        if n_missing == 0:
            # missing=0 但 ok=0：数据实际完整（ok=0 只是 offset 计算问题）
            return True
        
        # 有缺失帧，进入下一轮重传
        time.sleep(SPI_RETRANSMIT_DELAY)
    
    # 3 次尝试后仍有缺失
    print(f"[ERROR] Failed after {max_attempts} attempts, still missing {len(missing)} frames")
    return False
```

**2N 侧 —— ACK 线程解析 bitmap：**
```python
def ack_reader_thread(ser):
    """后台线程：持续读 UART，解析 ACK/命令帧"""
    leftover = b""
    while True:
        try:
            data = ser.read(128)
            if not data:
                continue
            leftover += data
            frames, leftover = parse_frames(leftover)
            for cmd, seq, payload in frames:
                if cmd == CMD_FRAME_ACK and len(payload) >= 9:
                    sid = struct.unpack(">I", payload[0:4])[0]
                    off = struct.unpack(">I", payload[4:8])[0]
                    ok = payload[8]
                    missing_offsets = None
                    nframes = 0
                    if len(payload) >= 11:
                        # Bitmap ACK 格式
                        nframes = struct.unpack(">H", payload[9:11])[0]
                        bmp_len = (nframes + 7) // 8
                        if len(payload) >= 11 + bmp_len and nframes > 0:
                            bitmap = payload[11:11 + bmp_len]
                            missing_offsets = []
                            # 遍历 bitmap，收集 bit=0 的帧 offset
                            for fi in range(nframes):
                                byte_idx = fi >> 3
                                bit_idx = fi & 7
                                if not (bitmap[byte_idx] & (1 << bit_idx)):
                                    missing_offsets.append(fi * CAM_SPI_MAX_PAYLOAD)
                        else:
                            print(f"[WARN] bitmap truncated")
                    ack_queue.put((sid, off, ok, missing_offsets))
                else:
                    cmd_queue.put((cmd, seq, payload))
        except Exception as e:
            print(f"[ERROR] ack_reader_thread exception: {e}")
            import traceback
            traceback.print_exc()
            time.sleep(0.01)
```

**AP 侧（C）—— bitmap 生成与 ACK 发送：**
```c
// SPI 接收线程中，每收到一帧 DATA：
if (type == CAM_SPI_TYPE_THUMB_DATA) {
    // 写入缩略图缓冲
    memcpy(cam_thumb_buf + offset, stream_buf + pos + CAM_SPI_HDR_SIZE, len);
    
    // 标记 bitmap（1=已收）
    uint16_t fidx = (uint16_t)(offset / CAM_SPI_MAX_PAYLOAD);
    if (fidx < CAM_RECV_BITMAP_SIZE * 8) {
        cam_recv_bitmap[fidx >> 3] |= (1 << (fidx & 7));
    }
    // 更新已收偏移和帧数
    if (offset + len > cam_thumb_offset)
        cam_thumb_offset = offset + len;
    if (fidx + 1 > cam_recv_nframes)
        cam_recv_nframes = fidx + 1;
}

// 大图 DATA 帧处理（关键：lseek 写入）
if (type == CAM_SPI_TYPE_BIG_DATA) {
    // 重传帧 offset 可能 < cam_big_offset，必须 lseek
    lseek(cam_big_fd, offset, SEEK_SET);
    ssize_t wn = write(cam_big_fd, stream_buf + pos + CAM_SPI_HDR_SIZE, len);
    if (wn == len) {
        // 只在 offset+len > 当前最大时更新（处理重传场景）
        if (offset + len > cam_big_offset)
            cam_big_offset = offset + len;
    }
    // 标记 bitmap
    uint16_t fidx = (uint16_t)(offset / CAM_SPI_MAX_PAYLOAD);
    if (fidx < CAM_RECV_BITMAP_SIZE * 8) {
        cam_recv_bitmap[fidx >> 3] |= (1 << (fidx & 7));
    }
    if (fidx + 1 > cam_recv_nframes)
        cam_recv_nframes = fidx + 1;
}

// CRC 错误处理（关键改进：pos++ 而非 stream_len=0）
if (crc_calc != crc_recv) {
    syslog(LOG_WARNING, "[camera_page] SPI CRC error (off=%lu), skip frame\n",
           (unsigned long)offset);
    pos++;       // 只跳过 1 字节，继续找下一个 magic
    continue;    // 不清空缓冲区，保留其他帧数据
}

// 收到 END 帧时：
if (type == CAM_SPI_TYPE_THUMB_END) {
    // 修正 nframes（避免最后几帧 CRC 错误导致 nframes 偏小）
    uint16_t expected_nframes = (uint16_t)((cam_thumb_total + CAM_SPI_MAX_PAYLOAD - 1) 
                                / CAM_SPI_MAX_PAYLOAD);
    if (expected_nframes > cam_recv_nframes)
        cam_recv_nframes = expected_nframes;
    
    // 遍历 bitmap 判断完整性
    uint8_t thumb_ok = 0;
    if (cam_thumb_offset >= cam_thumb_total && cam_recv_nframes > 0) {
        thumb_ok = 1;
        for (uint16_t fi = 0; fi < cam_recv_nframes; fi++) {
            if (!(cam_recv_bitmap[fi >> 3] & (1 << (fi & 7)))) {
                thumb_ok = 0;  // 有缺失帧
                break;
            }
        }
    }
    
    // 构建 bitmap ACK payload
    // 格式: session(4B) + offset(4B) + ok(1B) + nframes(2B) + bitmap(NB)
    uint16_t bmp_len = (cam_recv_nframes + 7) / 8;
    uint16_t ack_payload_len = 4 + 4 + 1 + 2 + bmp_len;
    uint8_t *ack_payload = malloc(ack_payload_len);
    if (ack_payload) {
        // 填充 session（大端）
        ack_payload[0] = (session >> 24) & 0xFF;
        ack_payload[1] = (session >> 16) & 0xFF;
        ack_payload[2] = (session >> 8) & 0xFF;
        ack_payload[3] = session & 0xFF;
        // 填充 offset（大端）
        ack_payload[4] = (cam_thumb_offset >> 24) & 0xFF;
        ack_payload[5] = (cam_thumb_offset >> 16) & 0xFF;
        ack_payload[6] = (cam_thumb_offset >> 8) & 0xFF;
        ack_payload[7] = cam_thumb_offset & 0xFF;
        // 填充 ok
        ack_payload[8] = thumb_ok;
        // 填充 nframes（大端）
        ack_payload[9] = (cam_recv_nframes >> 8) & 0xFF;
        ack_payload[10] = cam_recv_nframes & 0xFF;
        // 填充 bitmap
        memcpy(ack_payload + 11, cam_recv_bitmap, bmp_len);
        
        // 通过 UART 发送
        cam_uart_send_frame(CAM_CMD_FRAME_ACK, ack_payload, ack_payload_len);
        free(ack_payload);
    }
    
    if (thumb_ok) {
        // 全部收到：关闭 SPI 接收，显示缩略图
        cam_spi_recv_active = false;
        lv_async_call(cam_show_thumbnail_async_cb, ...);
    } else {
        // 有缺失：保持 SPI 接收开启，等重传
        // 关键：不重置 bitmap 和 nframes（累积状态保留）
        cam_ack_next_offset = 0;  // 只重置增量状态
    }
}
```

#### 1.4.4 性能数据

| 数据类型 | 大小 | 全量发送 | 重传帧数 | 重传耗时 | 总耗时 |
|----------|------|----------|----------|----------|--------|
| 缩略图 | 88KB | ~2s | 0~6帧 | ~0.02s | **~2s** |
| 大图 | 1MB | ~21s | 14~22帧 | ~0.1s | **~21s** |
| 大图 | 1.3MB | ~24s | 14~22帧 | ~0.1s | **~24s** |

**相比阶段一的提升：**
- 缩略图：13s → 2s（6.5 倍）
- 大图 1MB：9min → 21s（25.7 倍）
- 大图 1.3MB：12min → 24s（30 倍）

**相比阶段二的提升：**
- 缩略图：7s → 2s（3.5 倍）
- 大图：60s → 24s（2.5 倍）

---

## 二、方案对比分析

### 2.1 多维度对比表

| 维度 | 每帧校验 | 窗口校验 (W=6) | 末尾 Bitmap 校验 |
|------|----------|----------------|------------------|
| **UART 交互次数（88KB）** | 346 次 | 58 次 | 1~3 次 |
| **UART 交互次数（1.3MB）** | 5265 次 | 878 次 | 1~3 次 |
| **缩略图传输耗时** | ~13s | ~7s | ~2s |
| **大图传输耗时（1MB）** | ~9min | ~60s | ~21s |
| **大图传输耗时（1.3MB）** | ~12min | ~60s | ~24s |
| **错误检测粒度** | 单帧 | 窗口（6 帧） | 全局精确到帧 |
| **重传策略** | 立即重传当前帧 | Go-Back-N（整窗重传） | Selective（仅缺失帧） |
| **重传带宽浪费** | 低 | 高（窗口内已收帧也重传） | 极低 |
| **AP 侧缓冲需求** | 1 帧 | W 帧（2 帧） | 全部帧的 bitmap |
| **2N 侧线程模型** | 单线程阻塞 | 双线程（发送+ACK 线程） | 双线程 |
| **实时性（首帧可见）** | 优（每帧即确认） | 中 | 差（需全量发完） |
| **实现复杂度** | 低 | 中 | 高（bitmap 编解码） |
| **抗突发错误能力** | 强 | 中 | 中（依赖重传轮次） |
| **适用场景** | 低延迟小数据 | 平衡型 | 大批量数据传输 |

### 2.2 资源占用对比

| 资源 | 每帧校验 | 窗口校验 | 末尾 Bitmap 校验 |
|------|----------|----------|------------------|
| AP RAM（bitmap） | 0 | 0 | `(nframes+7)/8` 字节（1.3MB≈660B） |
| AP RAM（stream_buf） | 1 帧（277B） | 2 帧（554B） | 2 帧（554B） |
| 2N 线程数 | 1 | 2 | 2 |
| UART 带宽占用 | 高（每帧11B ACK） | 中（每窗口11B ACK） | 极低（仅END后1次ACK） |

### 2.3 误差检测能力对比

| 错误类型 | 每帧校验 | 窗口校验 | 末尾 Bitmap 校验 |
|----------|----------|----------|------------------|
| 单帧 CRC 错误 | ✅ 立即检测 | ✅ 窗口末检测 | ✅ END 时检测 |
| 帧丢失 | ✅ 超时检测 | ⚠️ 可能误判 | ✅ bitmap 精确 |
| 帧乱序 | ✅ offset 校验 | ⚠️ | ✅ offset + bitmap |
| 末尾帧丢失 | ⚠️ 依赖超时 | ⚠️ 依赖超时 | ✅ nframes 校验 |
| 数据空洞 | ✅ 无（每帧即写） | ⚠️ 可能 | ✅ bitmap遍历 |

---

## 三、问题与解决方案

本章详细记录每个阶段实施过程中遇到的具体技术问题，按"现象 → 分析过程 → 根因 → 解决方案 → 验证结果"的结构展开。

### 3.1 阶段一：每帧校验的问题

#### 问题 1：大图传输耗时过长（9分钟/1MB）

**现象：**
- 1MB 大图传输需要约 9 分钟，1.3MB 需要约 12 分钟
- 2800 侧 UART 日志显示每帧处理间隔约 135ms
- 2N 侧日志显示每帧发送后等待 ACK 约 38ms（缩略图）或 135ms（大图）
- 用户体验：点击查看大图后，屏幕长时间无响应

**分析过程：**
1. **排查 SPI 速率**：24MHz，单帧 277B 理论仅需 92μs，排除 SPI 带宽瓶颈
2. **排查 2N 帧间延时**：`time.sleep(0.003)` = 3ms，346帧=1s，5265帧=16s，非主要瓶颈
3. **排查 UART ACK 往返**：11B@921600bps=0.1ms，单看传输时间可忽略
4. **排查 AP 侧 `usleep(5000)`**：5ms/帧，5265帧=26s，占比显著但非全部
5. **排查 2N `ser.read()` 轮询**：10ms 粒度，每次 ACK 等待需 1-2 次轮询 = 10-20ms
6. **排查 AP 大图 `write()`**：littlefs 涉及擦除/写入，约 20ms/次（写放大严重）
7. **关键发现——2N Python 转义时间被忽略**：
   - `ser.read()` 返回字节后，Python 需要逐字节扫描 sync `0xAA55`
   - 提取 frame_len，判断是否收齐完整帧
   - CRC16 校验（Python 实现，比 C 慢 20 倍）
   - `struct.unpack()` 解包字段
   - 整个解析过程约 8ms/帧，之前分析完全遗漏
8. **系统调度开销**：大图 5265 帧长时间传输，Python GC 频繁触发 + Linux 调度，约 78ms/帧

**根因：**
每帧 ACK 机制下，单帧延迟由 13 个串行环节累加（详见 §1.2.4），其中：
- 2N Python 轮询+解析 ACK：23ms（被遗漏的最大隐藏瓶颈）
- littlefs 写放大：20ms
- 系统调度开销：78ms
- 三者合计 121ms，占单帧 135ms 的 90%

**解决方案：**
演进到窗口校验机制（阶段二），通过批量发送 6 帧减少 UART 交互，将 2N 轮询+解析时间均摊到 6 帧（23ms → 3.8ms/帧）

**验证结果：**
大图传输从 9min 降至 60s，提升 9 倍

---

### 3.2 阶段二：窗口校验的问题

#### 问题 1：NACK 导致整窗重传，效率低下

**现象：**
- 窗口内 1 帧 CRC 错误，6 帧全部重传
- SPI 初始化不稳定时，NACK 频繁，base 反复回退
- 缩略图传输实测 ~7s，但偶尔出现 14s（多轮重传）
- 2N 日志可见 `base reset to X` 频繁出现

**分析过程：**
1. 检查 2N 侧窗口逻辑：`if ok: base += 6; else: base = ack_off // 256`
2. 检查 AP 侧 ACK 逻辑：窗口内任意一帧 CRC 错误，回 `NACK(off=错误帧offset)`
3. 检查 CRC 错误分布：主要集中在传输前 100 帧（SPI 初始化阶段），错误率约 0.4%
4. **关键发现**：Go-Back-N 策略下，1 帧错误导致 6 帧重传，浪费 5 帧带宽。前 100 帧若有 4 次 CRC 错误，则额外重传 20 帧

**根因：**
Go-Back-N 简化策略——窗口内任意一帧错误，整个窗口重传。虽然实现简单，但在 CRC 错误率较高（0.4%）时，重传开销显著

**解决方案：**
演进到末尾 Bitmap 校验（Selective 重传），只重传缺失帧

**验证结果：**
重传帧数从"每错误6帧"降至"每错误1帧"，大图传输从 60s 降至 24s

#### 问题 2：stream_buf 清空导致数据空洞

**现象：**
- CRC 错误时 AP 侧 `stream_len = 0` 清空整个缓冲区
- 已正确接收的帧数据丢失，但 bitmap 已标记为已收
- 2N 不重传这些帧，文件中出现全 0 空洞
- 大图表现为颜色发红、上边和右边错位

**分析过程：**
1. 检查 AP 侧 CRC 错误处理代码：
   ```c
   if (crc_calc != crc_recv) {
       cam_send_frame_ack(session, offset, 0);  // NACK
       stream_len = 0;   // ← 清空整个缓冲区！
       break;
   }
   ```
2. 检查 stream_buf 中可能包含多个帧：AP 侧 `read()` 一次可能取 554B（2 帧），stream_buf 中可能有"半帧+整帧+半帧"
3. **关键场景重现**：
   - stream_buf = [frame_A(完整) | frame_B(CRC错) | frame_C(半帧)]
   - CRC 错误时 `stream_len = 0` 清空全部
   - frame_A 的数据虽然已处理（bitmap 已标记），但 frame_C 的半帧数据丢失
   - 下次 `read()` 重新开始，frame_C 的后半部分永远收不到 → 帧丢失
4. 更严重的情况：如果 frame_A 尚未被处理（pos 还没走到），frame_A 数据也丢失

**根因：**
CRC 错误处理过于激进——`stream_len = 0` 连坐了整个缓冲区的数据，包括 CRC 错误帧之前已正确解析但尚未处理的帧，以及后续半帧数据

**解决方案：**
改为 `pos++; continue;`，只跳过当前字节，继续找下一个 magic：
```c
if (crc_calc != crc_recv) {
    syslog(LOG_WARNING, "[camera_page] SPI CRC error (off=%lu), skip frame\n",
           (unsigned long)offset);
    pos++;       // 只跳过 1 字节
    continue;    // 继续从 pos+1 找下一个 magic
}
```

**验证结果：**
- CRC 错误后不再丢失其他帧数据
- 通过逐字节扫描 magic 自动恢复帧边界
- 大图花屏问题解决

#### 问题 3：独立 ACK 线程的 IndentationError

**现象：**
```
File "/home/cat/lzw/camera_hub_2n.py", line 426
    if cmd == CMD_CAPTURE_REQ:
IndentationError: unexpected indent
```
2N 侧脚本无法启动，整个传输功能不可用

**分析过程：**
1. 检查 line 426 附近代码，发现 `if cmd == CMD_CAPTURE_REQ:` 缩进为 16 空格
2. 检查上层结构：该代码块原属于 `for cmd, seq, payload in frames:` 循环内部
3. 回顾重构历史：在引入 `ack_reader_thread` 时，删除了 `for cmd, seq, payload in frames:` 循环（改为直接在 `for` 外处理），但内部命令分发代码缩进未同步减少（应从 16 空格减至 12 空格）

**根因：**
删除 `for cmd, seq, payload in frames:` 循环后，内部命令分发代码缩进未同步调整，Python 对缩进敏感导致语法错误

**解决方案：**
将命令分发块（`if cmd == CMD_CAPTURE_REQ:` 至 `print(f"[WARN] Unknown cmd...")`）缩进从 16 空格调整为 12 空格，与上层 `print` 语句对齐

**验证结果：**
脚本正常启动，ACK 线程正常运行

#### 问题 4：2N 侧 ACK 线程异常静默吞掉

**现象：**
- 偶发情况下 2N 侧收不到 ACK，传输卡死
- `ack_reader_thread` 似乎停止工作，但无错误日志
- `wait_bitmap_ack()` 始终超时，触发重传死循环

**分析过程：**
1. 检查 `ack_reader_thread` 代码，发现 `while True` 循环内有 `try/except`：
   ```python
   while True:
       try:
           data = ser.read(128)
           # ... 解析逻辑 ...
       except Exception as e:
           pass  # ← 静默吞掉异常！
   ```
2. 若 `parse_frames()` 或 `struct.unpack()` 因数据异常抛错，except 捕获后 `pass`，无任何日志
3. 线程本身没退出（`while True` 继续），但每次 read 都异常被吞，ACK 永远进不了 queue

**根因：**
`except Exception: pass` 静默吞掉所有异常，导致 ACK 线程"假活"——线程在运行但实际不工作

**解决方案：**
```python
except Exception as e:
    print(f"[ERROR] ack_reader_thread exception: {e}")
    import traceback
    traceback.print_exc()
    time.sleep(0.01)  # 避免异常时 CPU 空转
```

**验证结果：**
异常不再被吞，可通过日志定位解析问题，ACK 线程稳定运行

---

### 3.3 阶段三：末尾 Bitmap 校验的问题

#### 问题 1：littlefs 磁盘空间不足（ENOSPC）

**现象：**
- 2800 屏幕显示 "disk full"
- 大图只写入 368KB（占 1.3MB 的 27%）
- 2N 侧 3 次 attempt 全部 ACK timeout 失败
- 2800 串口日志大量 `lfs_alloc: No more free space 631`

**分析过程：**
1. 检查 2800 串口日志，发现大量 `lfs_alloc: No more free space 631` 错误
2. 检查 AP 侧写入代码：`write(cam_big_fd, ..., 256)` 返回 -1，`errno=28`（ENOSPC）
3. 检查文件保存路径：`/data/`（littlefs 3MB 分区）
4. 检查 `/data/` 分区使用情况：理论 3MB，但大量小文件残留
5. **关键发现——littlefs 写放大效应**：
   - littlefs 每次写入 256B 小数据，会触发元数据更新（块状态、CRC、配对块）
   - 实际写放大比约 4-8 倍：写 256B 数据消耗 1-2KB 闪存空间
   - 3MB 分区实际只能存约 400KB 有效数据
   - 大图 1.3MB 远超 400KB 容量
6. AP 侧因 `write()` 失败不更新 bitmap，但也不发 NACK，导致 2N 侧 ACK 超时

**根因：**
大图保存到 `/data/`（littlefs 3MB 分区），写放大 + 残留文件导致空间耗尽，`write()` 返回 ENOSPC，bitmap 无法完整生成

**解决方案：**
大图路径从 `/data/` 改为 `/emmc/`（fatfs 3.7GB 分区），彻底规避 littlefs 的空间限制和写放大问题

**验证结果：**
- `/emmc/` 分区 3.7GB，写放大极小
- 大图成功写入完整 1.3MB
- bitmap 完整生成，ACK 正常返回

#### 问题 2：BIG_END ok=0 时关闭 SPI 接收，重传帧全部丢失

**现象：**
- 2N 侧 attempt 1 发送 5265 帧，收到 NACK（missing=16 帧）
- 2N 侧重传 16 帧，但 AP 侧无任何响应
- 2N 侧 attempt 2、attempt 3 全量重发，AP 仍然无响应
- 最终 3 次 attempt 全失败

**分析过程：**
1. 检查 2N 侧日志：attempt 1 正常发送 + 重传 16 帧 + ACK timeout
2. 检查 2800 侧日志：`big image complete (ok=0)`，之后无任何帧接收日志
3. 检查 AP 侧 BIG_END 处理代码：
   ```c
   if (big_ok) {
       cam_spi_recv_active = false;  // 关闭接收
       close(cam_big_fd);            // 关闭文件
   } else {
       cam_spi_recv_active = false;  // BUG! ok=0 也关闭了接收
       // ...
   }
   ```
4. **关键发现**：ok=0 分支也执行了 `cam_spi_recv_active = false`，关闭了 SPI 接收
5. 重传帧到达时，AP 侧 SPI 接收线程已退出循环（`while (cam_spi_recv_active)` 条件为 false），不再读取 SPI 数据
6. 2N 侧重传的 16 帧全部丢失，`wait_bitmap_ack()` 超时

**根因：**
ok=0 分支错误地关闭了 SPI 接收（`cam_spi_recv_active = false`），重传帧无法被接收

**解决方案：**
ok=0 时保持 `cam_spi_recv_active = true`，保留文件句柄 `cam_big_fd` 不关闭，只重置 `cam_ack_next_offset = 0` 等待重传：
```c
if (big_ok) {
    cam_spi_recv_active = false;
    close(cam_big_fd);
    cam_big_fd = -1;
} else {
    // 有缺失：保持资源开启，等重传
    cam_ack_next_offset = 0;
    // 不重置 cam_recv_bitmap 和 cam_recv_nframes（累积状态保留）
}
```

**验证结果：**
- 重传帧可被正常接收
- bitmap 对应 bit 置 1，最终全 1 触发 ok=1

#### 问题 3：missing=0 但 ok=0，2N 不 return 导致全量重发

**现象：**
- 重传后 bitmap 显示 missing=0（所有帧已收到），但 ok=0
- 2N 侧不 return，继续 attempt 2 全量重发 4179 帧
- AP 侧再次接收 4179 帧，数据重复写入同一文件
- 最终文件大小翻倍（2134738 vs 1069673），内容完全错乱 → 花屏

**分析过程：**
1. 检查 2N 侧重传后逻辑：
   ```python
   ok2, ack_off2, missing2 = wait_bitmap_ack(session_id, timeout=3.0)
   if ok2:          # ok=0，不进入
       return True
   n2 = len(missing2)  # missing=0，n2=0
   # 不return，继续下一轮 attempt
   ```
2. 检查 AP 侧 ok 判断：`big_ok = (cam_big_offset >= cam_big_total) ? 1 : 0`
3. 检查 `cam_big_offset` 更新逻辑：`if (offset + len > cam_big_offset) cam_big_offset = offset + len;`
4. **关键发现**：最后几帧的 payload 不足 256 字节
   - 假设 total=1052930，最后帧 offset=1052674, len=256 → offset+len=1052930 = total ✓
   - 但若最后帧 offset=1052930, len=0（END 帧）→ 不更新 offset
   - 实际上某些边界情况下 `cam_big_offset` 可能略小于 `cam_big_total`
5. 此时 bitmap 已全 1（所有帧已收到），missing=0，数据实际完整
6. 但 ok=0 导致 2N 不 return，全量重发造成数据覆盖

**根因：**
2N 侧 `if ok2: return True` 判断，ok=0 时不 return。但 ok=0 只是 offset 计算的边界问题（最后帧 payload 不足），数据完整性应以 bitmap 为准

**解决方案：**
2N 侧 `missing=0`（或 `n2 == 0`）时直接 `return True`，不管 ok 值：
```python
ok2, ack_off2, missing2 = wait_bitmap_ack(session_id, timeout=3.0)
n2 = len(missing2) if missing2 else 0
if n2 == 0:
    print(f"[INFO] Retrans bitmap all received, offset={ack_off2}")
    return True  # 数据完整性以 bitmap 为准
```

**验证结果：**
- 重传后 missing=0 即退出，不再全量重发
- 文件大小正确，无数据覆盖

#### 问题 4：重传帧 write 未 lseek，数据写错位置

**现象：**
- 大图颜色发红、上边和右边错位
- JPEG 文件可打开但内容错误
- 文件大小正确，但像素位置错乱

**分析过程：**
1. 检查 2N 侧日志：attempt 1 missing=14 帧，重传 14 帧后 ok=1
2. 检查 2800 侧日志：`big image complete (1052418/1052930 bytes, ok=0)`，但最终 ok=1
3. 检查 AP 侧大图写入代码：
   ```c
   ssize_t wn = write(cam_big_fd, stream_buf + pos + CAM_SPI_HDR_SIZE, len);
   if (wn == len) {
       cam_big_offset += len;  // ← 错误！
   }
   ```
4. **关键发现——write() 是顺序写入**：
   - 首次传输：frame#1(off=0) → frame#2(off=256) → ... → 顺序写入，文件指针顺序前进 ✓
   - 重传场景：frame#1(off=0) 已写，但 frame#10(off=2304) CRC 错误未写
   - 首轮写完后文件指针在 off=1048834（最后成功帧末尾）
   - 重传 frame#10(off=2304) 时，`write()` 写到文件指针当前位置（1048834），而非 2304！
   - `cam_big_offset += len` 错误递增到 1052418
5. 结果：2304 位置的数据写到了 1048834 位置，文件内容完全错乱

**根因：**
重传帧是随机位置写入，但 `write()` 是顺序写入（写到文件指针当前位置），没有先 `lseek()` 到正确位置

**解决方案：**
每次 write 前 `lseek(cam_big_fd, offset, SEEK_SET)`，并且 `cam_big_offset` 用 max 更新：
```c
lseek(cam_big_fd, offset, SEEK_SET);  // 定位到正确位置
ssize_t wn = write(cam_big_fd, stream_buf + pos + CAM_SPI_HDR_SIZE, len);
if (wn == len) {
    if (offset + len > cam_big_offset)  // 只在更大时更新
        cam_big_offset = offset + len;
}
```

**验证结果：**
- 重传帧写入正确位置
- 大图显示正常，无花屏/错位

#### 问题 5：最后几帧 CRC 错误导致 nframes 不准

**现象：**
- 最后一帧 CRC 错误，`cam_recv_nframes` 未更新到正确值
- bitmap 检查范围遗漏末尾帧，big_ok=1（错误判断为完整）
- 2N 不重传最后 1-2 帧 → 图片底部花屏/条纹

**分析过程：**
1. 检查 `cam_recv_nframes` 更新逻辑：
   ```c
   if (fidx + 1 > cam_recv_nframes)
       cam_recv_nframes = fidx + 1;
   ```
2. 该更新只在 CRC 校验通过后执行，如果最后一帧 CRC 错误，nframes 不会更新
3. 检查 bitmap 遍历范围：`for (fi = 0; fi < cam_recv_nframes; fi++)`
4. **关键场景重现**：
   - 总帧数 4114，最后一帧（fidx=4113）CRC 错误
   - `cam_recv_nframes` 停在 4113（倒数第二帧）
   - bitmap 只检查 0~4112，第 4113 帧不在检查范围内
   - big_ok=1（错误），2N 不重传第 4113 帧
5. 2N 侧根据 AP 发来的 nframes 计算缺失帧，也不会检查第 4113 帧 → 永远不重传

**根因：**
`cam_recv_nframes` 动态更新依赖 CRC 校验通过，最后几帧 CRC 错误时 nframes 偏小，bitmap 检查范围不完整

**解决方案：**
END 时根据 total 计算 `expected_nframes`，取 `max(cam_recv_nframes, expected_nframes)`：
```c
uint16_t expected_nframes = (uint16_t)((cam_big_total + CAM_SPI_MAX_PAYLOAD - 1) 
                            / CAM_SPI_MAX_PAYLOAD);
if (expected_nframes > cam_recv_nframes)
    cam_recv_nframes = expected_nframes;
```

**验证结果：**
- bitmap 检查范围覆盖所有帧
- 末尾 CRC 错误的帧被正确标记为缺失，触发重传

#### 问题 6：第一次拍照缩略图必现条纹

**现象：**
- 首次拍照缩略图有横线（条纹），后续拍照正常
- 2N 侧日志显示 `bitmap parsed: missing=6 frames`，但 `ok=1`
- 2N 直接 return True，不重传 → 显示不完整缩略图

**分析过程：**
1. 检查 2N 侧日志：首次传输 missing=6，但 ok=1，2N 直接 return True
2. 检查 AP 侧 thumb_ok 判断：
   ```c
   thumb_ok = (cam_thumb_offset >= cam_thumb_total) ? 1 : 0;
   ```
3. 检查 `cam_thumb_offset` 更新：`if (offset + len > cam_thumb_offset) cam_thumb_offset = offset + len;`
4. **关键发现**：
   - 即使 6 帧 CRC 错误（数据未写入 thumb_buf），其他帧的 offset+len 仍能达到 total
   - 例如：frame#3 缺失，但 frame#4 的 offset+len 仍 > frame#3 的 offset+len
   - 最终 cam_thumb_offset >= cam_thumb_total，thumb_ok=1
5. 但 bitmap 中这 6 帧的 bit 为 0，thumb_ok 没有检查 bitmap
6. 后续拍照 SPI 已稳定，CRC 错误少，bitmap 全 1，所以正常

**根因：**
SPI 初始化不稳定，首次传输 CRC 错误多（missing=6），但 thumb_ok 仅看 offset≥total，提前显示不完整缩略图

**解决方案：**
thumb_ok / big_ok 增加 bitmap 遍历检查：
```c
if (cam_thumb_offset >= cam_thumb_total && cam_recv_nframes > 0) {
    thumb_ok = 1;
    for (uint16_t fi = 0; fi < cam_recv_nframes; fi++) {
        if (!(cam_recv_bitmap[fi >> 3] & (1 << (fi & 7)))) {
            thumb_ok = 0;  // 有缺失帧
            break;
        }
    }
}
```

**验证结果：**
- 首次拍照 missing=6 时 thumb_ok=0，触发重传
- 重传后 bitmap 全 1，缩略图显示正常

#### 问题 7：日志刷屏导致关键信息丢失

**现象：**
- 2800 串口日志被 SPI 驱动 `spiinfo` 刷屏，camera_page 关键日志被冲掉
- 5265 帧大图传输产生 7 万+ 行 SPI-SLAVE-DBG 日志
- 无法定位传输失败原因

**分析过程：**
1. 检查 `spi_slave_driver.c`：每次 `read()` 打印 2 行 `spiinfo`（"filep=..." + "All words retrieved!"）
2. 检查 `camera_page.c`：关键日志用 `syslog(LOG_INFO, ...)`，与 spiinfo 同级别
3. 串口日志缓冲区有限，高频率 spiinfo 覆盖了低频率的 camera_page 日志
4. 5265 帧 × 2 行/帧 = 10530 行 spiinfo，远超日志缓冲区

**根因：**
SPI 驱动层 `spiinfo` 日志级别过高（LOG_INFO），与业务日志混在同一级别，高频日志冲掉低频关键日志

**解决方案：**
关闭 spiinfo 日志（保留 spierr/spiwarn）：
```c
// spi_slave_driver.c 顶部
#undef spiinfo
#define spiinfo(...) do{}while(0)
```

**验证结果：**
- 串口日志量从 7 万行降至几百行
- camera_page 关键日志清晰可见

#### 问题 8：2N 侧 frame# 日志刷屏

**现象：**
- 5265 帧每帧打印一行 `[DBG] frame#xxx`，日志爆炸
- 无法看到重传和 ACK 相关的关键信息

**分析过程：**
1. 检查 2N 侧发送代码：每帧都 `print(f"[DBG] frame#{frame_idx} ...")`
2. 5265 帧 × 1 行/帧 = 5265 行日志，淹没关键信息

**根因：**
高频日志无采样，全量打印导致日志噪声过大

**解决方案：**
仅打印前 3 帧 + 最后一帧：
```python
is_last = (offset + len(chunk) >= total)
if frame_idx <= 3 or is_last:
    print(f"[DBG] frame#{frame_idx} off={offset} sz={len(frame)} pay={len(chunk)}")
```

**验证结果：**
- 日志量从 5265 行降至 4 行
- 重传和 ACK 关键信息清晰可见

---

## 四、经验总结

### 4.1 技术洞察

#### 洞察 1：校验粒度与延迟的权衡

在 SPI + UART 混合通信架构中，校验粒度的选择直接影响传输性能：

- **细粒度校验（每帧）**：
  - 优点：实时性好，错误立即检测，数据无空洞
  - 缺点：每帧 UART 往返延迟（~24ms），5265 帧累积 135s（缩略图）/ 9min（大图）
  - 适用场景：低延迟小数据（如传感器读数、控制指令）
  - 本项目教训：缩略图 13s 尚可忍受，但大图 9min 完全不可用

- **中粒度校验（窗口）**：
  - 优点：UART 交互减少 W 倍，延迟可接受
  - 缺点：Go-Back-N 重传浪费带宽，窗口大小受缓冲限制
  - 适用场景：中等数据量、链路质量较好
  - 本项目教训：W=6 时大图 60s，但 NACK 导致整窗重传，效率仍不理想

- **粗粒度校验（末尾 Bitmap）**：
  - 优点：UART 交互极小（1~3 次），选择性重传零浪费
  - 缺点：首帧可见性差（需全量发完），bitmap 内存开销
  - 适用场景：大批量数据传输（图片、文件）
  - 本项目效果：大图 24s，满足需求

**核心结论**：对批量数据传输，**末尾汇总 + 选择性重传**是最优解。校验粒度越粗，UART 延迟分摊越小，但需要 bitmap 来精确指示缺失帧。校验粒度的选择应基于数据量、链路质量和实时性需求的三维权衡。

#### 洞察 2：bitmap 是批量状态压缩的有效手段

bitmap 在本项目中发挥了关键作用，是阶段三性能飞跃的核心：

- **压缩比高**：用 1 bit 表示 1 帧的接收状态，1.3MB 大图（5265 帧）仅需 660 字节
- **传输开销低**：相比逐帧 ACK 列表（每帧 11B × 5265 = 58KB），bitmap 仅 660B，减少 99%
- **缺失帧计算简单**：`missing_offset = fidx * CAM_SPI_MAX_PAYLOAD`，O(1) 计算
- **扩展性好**：支持最多 8192 帧（CAM_RECV_BITMAP_SIZE=1024B × 8），约 2MB 数据

**设计要点：**
- bitmap 中 1 = 已收，0 = 缺失（与常规直觉相反，但便于初始化为全 0）
- AP 侧遍历 bitmap 生成缺失列表，通过 UART 发送给 2N
- 2N 侧根据缺失列表直接计算 offset，精确重传
- bitmap 跨重传轮次累积，每轮只补缺失帧，效率递增

**适用场景判断**：当数据帧数 > 100 且需要可靠性保证时，bitmap 方案的收益显著高于逐帧 ACK。

#### 洞察 3：CRC 错误处理不能"连坐"

这是本项目中踩过的最深的坑之一，直接导致大图花屏：

- **错误做法**：`stream_len = 0; break;` — 清空整个 stream_buf
  - 后果：CRC 错误帧之前的所有已正确接收帧数据全部丢失
  - 但 bitmap 已标记这些帧为已收 → 2N 不重传 → 文件空洞 → 花屏
  - 实测：3 次 CRC 错误导致 19 帧数据丢失（22 - 3 = 19）

- **正确做法**：`pos++; continue;` — 只跳过当前字节
  - 保留 stream_buf 中其他帧数据
  - 继续找下一个 magic，自动恢复帧边界
  - 即使 magic 位置错位，也能通过逐字节扫描找到正确边界

**原理分析**：
- SPI 从机 DMA 接收是流式的，stream_buf 中可能包含多个帧
- `stream_len = 0` 会丢弃整个缓冲区，包括尚未处理的帧
- `pos++` 只跳过 1 字节，最大损失 1 字节，其他帧不受影响
- magic `0x494D4731` 是 4 字节模式，随机数据中误匹配概率极低（1/2^32）

**原则**：错误隔离，最小化影响范围。一个帧的 CRC 错误不应影响其他帧的处理。这是流式协议解析的通用原则。

#### 洞察 4：ok 字段语义需明确

ok 字段在演进过程中经历了多次语义变化，每次都踩了坑：

- **最初**：`ok = (offset >= total) ? 1 : 0` — 只看偏移量
  - 问题：有数据空洞时 offset 仍能达到 total（其他帧的 offset+len 覆盖了缺失帧），ok=1 但数据不完整
  - 表现：缩略图条纹（首次拍照 missing=6 但 ok=1）

- **改进**：`ok = (offset >= total) && bitmap_all_1 ? 1 : 0` — 综合 offset + bitmap
  - 问题：最后帧 payload 不足 256 时，offset < total，ok=0 但数据实际完整
  - 表现：2N 全量重发导致文件覆盖（missing=0 但 ok=0）

- **最终**：2N 侧以 `missing == 0` 为成功标准，不看 ok
  - bitmap 全 1 = 数据完整，ok 只是 offset 的辅助判断
  - ok 字段仅用于日志展示，不参与成功判断

**最佳实践**：数据完整性判断以 bitmap 为准，ok 仅作为快速判断的辅助。接收方应发送完整 bitmap，让发送方自行判断。语义模糊的字段容易导致边界 bug，设计时应明确每个字段的权威性。

#### 洞察 5：重传必须 lseek 到正确位置

文件写入是顺序的（write 从文件指针当前位置写入），但重传帧是随机位置的：

- **错误做法**：直接 `write()`，文件指针顺序前进
  - 重传帧 offset=274688，但文件指针在 1048834，数据写到错误位置
  - `cam_big_offset += len` 错误递增，文件内容完全错乱
  - 表现：大图颜色发红、错位

- **正确做法**：每次 write 前 `lseek(cam_big_fd, offset, SEEK_SET)`
  - 重传帧写到正确位置
  - `cam_big_offset` 用 max 更新：`if (offset + len > cam_big_offset) cam_big_offset = offset + len;`

**原理**：
- 首次传输是顺序写入（frame#1, frame#2, ...），文件指针自然前进
- 重传是随机写入（frame#10, frame#25, frame#100，顺序不定），文件指针位置不可预测
- `lseek(SEEK_SET)` 将文件指针定位到绝对偏移，确保数据写入正确位置

**最佳实践**：随机写入必须 lseek，顺序写入可以不 lseek。重传场景下帧顺序不确定，必须 lseek。这是文件 I/O 的基本常识，但在流式协议中容易被忽略。

### 4.2 最佳实践

#### 实践 1：线程解耦——发送与 ACK 分离

```python
# 2N 侧：发送线程 + ACK 接收线程，通过 queue 通信
ack_queue = queue.Queue()

def ack_reader_thread(ser):
    """后台线程：持续读UART，解析ACK帧放入queue"""
    leftover = b""
    while True:
        data = ser.read(128)
        if not data:
            continue
        leftover += data
        frames, leftover = parse_frames(leftover)
        for cmd, seq, payload in frames:
            if cmd == CMD_FRAME_ACK:
                sid, off, ok, missing = parse_bitmap_ack(payload)
                ack_queue.put((sid, off, ok, missing))

# 主线程：发送数据，从 queue 取 ACK
def spi_send_image(...):
    while offset < total:
        spi.xfer2(list(frame))  # 不等ACK，连续发送
    # 全部发完后，从queue取ACK
    ok, off, missing = ack_queue.get(timeout=3.0)
```

**收益分析**：
- 消除 10ms 轮询延迟（`ser.read()` 非阻塞模式的固有粒度）
- 发送与 ACK 处理解耦，发送线程不被 UART 读取阻塞
- queue 提供线程安全的数据传递，无需手动加锁
- ACK 线程持续运行，ACK 一到就被解析，延迟 < 1ms

**适用场景**：任何"发送 + 等待确认"的通信模式，尤其是确认延迟大于发送延迟时。

#### 实践 2：日志分级与采样

```c
// AP 侧：关键路径用 LOG_INFO，调试用 LOG_DEBUG
syslog(LOG_INFO, "[camera_page] big image complete (%lu/%lu bytes, ok=%u)\n", ...);

// 关闭高频驱动日志
#undef spiinfo
#define spiinfo(...) do{}while(0)
```

```python
# 2N 侧：高频日志采样打印（前3帧+最后1帧）
if frame_idx <= 3 or is_last or (attempt > 0 and frame_idx <= 3):
    print(f"[DBG] frame#{frame_idx} off={offset} sz={len(frame)} ...")
```

**收益分析**：
- 5265 帧日志从 5265 行降至 4 行
- 关键信息不再被刷屏冲掉
- 调试时可通过 `attempt > 0` 条件查看重传帧

**原则**：
- 高频日志（每帧/每事件）必须采样或分级
- 关键路径日志（开始、结束、错误）保留全量
- 驱动层日志默认关闭，需要时通过编译选项开启

#### 实践 3：expected_nframes 校验

```c
// END 时根据 total 计算期望帧数，避免动态 nframes 不准
uint16_t expected_nframes = (uint16_t)((cam_big_total + CAM_SPI_MAX_PAYLOAD - 1) 
                             / CAM_SPI_MAX_PAYLOAD);
if (expected_nframes > cam_recv_nframes)
    cam_recv_nframes = expected_nframes;
```

**收益分析**：
- 避免最后几帧 CRC 错误导致 nframes 偏小
- bitmap 检查范围覆盖所有帧
- 2N 侧重传时不会遗漏末尾帧

**原理**：动态状态（cam_recv_nframes）依赖运行时事件更新，可能因错误而遗漏；静态状态（expected_nframes）由已知量（total）计算，可靠且完整。关键校验应使用静态状态作为下限。

#### 实践 4：错误状态保持

```c
if (big_ok) {
    // 全部收到：关闭资源
    cam_spi_recv_active = false;
    close(cam_big_fd);
    cam_big_fd = -1;
} else {
    // 有缺失：保持资源，等重传
    // 不关闭 cam_spi_recv_active
    // 不关闭 cam_big_fd
    // 只重置增量状态
    cam_ack_next_offset = 0;
    // 保留 cam_recv_bitmap 和 cam_recv_nframes（累积状态）
}
```

**收益分析**：
- 重传帧可以被正确接收和处理
- 已收帧的 bitmap 标记不丢失
- 文件句柄保持打开，重传帧可以继续写入

**原则**：区分"累积状态"（跨重传保留）和"增量状态"（每次重传重置）。错误状态下只重置增量状态，保留累积状态，确保重传能正确接续。

### 4.3 可复用设计模式

#### 模式 1：Bitmap ACK 协议模式

**适用场景**：批量数据传输 + 可靠性要求

```
发送方: 全量发送 N 帧 → END → 等 ACK
接收方: 收帧标记 bitmap → END 时遍历 bitmap 生成缺失列表 → 回 ACK(session, offset, ok, nframes, bitmap)
发送方: 
  if missing == 0: 成功退出
  else: 仅重传缺失帧 → END → 等 ACK（循环）
```

**关键设计点：**
- bitmap 中 1=已收，0=缺失
- 接收方在 END 时计算 expected_nframes，修正 nframes
- 接收方 ok 判断需综合 offset + bitmap
- 发送方以 missing==0 为成功标准
- 最多重传 3 轮，每轮 bitmap 越来越满

**复用建议**：适用于任何"批量发送 + 选择性重传"的场景，如文件传输、固件升级、图片同步。

#### 模式 2：CRC 错误隔离模式

**适用场景**：流式数据解析（stream_buf 中可能包含多个帧）

```
错误时: pos++（而非清空缓冲区）
循环:   继续从 pos 找下一个 magic
优势:   保留已正确数据，自动恢复边界
```

**关键设计点：**
- magic 是帧边界对齐的唯一依据
- CRC 错误不代表 magic 位置错误（可能只是 payload 噪声）
- 逐字节扫描能找到下一个正确 magic
- 错误影响范围最小化（1 字节），不连坐其他帧

**复用建议**：适用于任何基于 magic 同步的流式协议解析，如串口数据帧、网络数据包。

#### 模式 3：累积状态 + 增量重置模式

**适用场景**：分阶段可靠传输（多轮重传）

```
累积状态（跨重传保留）:
  - cam_recv_bitmap: 已收帧标记
  - cam_recv_nframes: 已收帧数
  - cam_thumb_offset / cam_big_offset: 已收数据偏移
  - cam_thumb_buf / cam_big_fd: 数据缓冲/文件句柄

增量状态（每次重传重置）:
  - cam_ack_next_offset: 下一帧期望偏移

原则: 不丢失已正确接收的进度
```

**关键设计点：**
- ok=0 时只重置增量状态，不重置累积状态
- 文件句柄保持打开，bitmap 保留
- 重传帧到达时，bitmap 对应位置被置 1，最终全 1 即成功

**复用建议**：适用于任何需要多轮重传的可靠传输协议，区分"进度状态"和"临时状态"。

---

## 五、未来展望

### 5.1 短期优化方向

#### 优化 1：增大 payload（256 → 512/1024）

- **收益**：帧数减半，传输时间减半
- **前提**：
  - BES 底层 `SPI_SLAVE_BUFSIZE` 从 2048 扩到 4096
  - AP 侧 `stream_buf` 从 2 帧扩到 4 帧
  - 验证 SPI 信号完整性（更长 payload 抗噪能力下降）
- **风险**：单帧 CRC 错误丢失更多数据，需结合 bitmap 重传
- **实测**：单帧 600 字节稳定，payload=512 理论可行

#### 优化 2：多轮 bitmap 并行重传

- 当前：重传 → 等 ACK → 再重传（串行）
- 优化：一次性发送所有缺失帧 + END，单轮完成
- **收益**：减少 END/ACK 往返

#### 优化 3：CRC 错误根因治理

- 当前：CRC 错误率约 0.4%（22/4114 帧）
- 方向：
  - SPI 从机 DMA 预武装（`prepare_next_tx → prepare_next_rx`）
  - 降低 SPI 时钟频率（24MHz → 12MHz）测试稳定性
  - 硬件连线检查（增加去耦电容、缩短走线）

### 5.2 中期架构演进

#### 演进 1：双缓冲流水线

```
AP 侧: buf0 接收 ←→ buf1 解析/写入
       交替工作，消除单缓冲的等待
```
- **收益**：吞吐量提升 ~30%

#### 演进 2：流式 Bitmap（分段 ACK）

- 对超大文件（如 10MB+ 视频），单次 bitmap 过大
- 优化：每 N 帧（如 1024 帧）发一次 bitmap ACK
- 兼顾实时性与内存占用

#### 演进 3：差异化校验策略

| 数据类型 | 策略 |
|----------|------|
| 缩略图（小，需快速可见） | 末尾 Bitmap ACK |
| 大图（中，可靠性优先） | 末尾 Bitmap ACK + 重传 |
| 视频流（大，实时性优先） | 分段 Bitmap + 丢帧容忍 |

### 5.3 长期技术规划

#### 规划 1：视频流传输可行性评估

- **当前**：1.3MB 大图 24 秒（~54KB/s）
- **目标**：10MB 视频
- **挑战**：
  - 带宽：当前 54KB/s，10MB 需 ~185 秒（不可接受）
  - 需提升至 500KB/s 以上（payload 增大 + 流水线 + 降 CRC 错误率）
- **建议**：视频流不适合 SPI 传输，建议改用 USB 或 WiFi

#### 规划 2：协议自适应

- 根据链路质量动态调整：
  - CRC 错误率低 → 增大 payload / 窗口
  - CRC 错误率高 → 减小 payload / 增加重传轮次
- 类似 TCP 拥塞控制

#### 规划 3：硬件加速

- CRC 计算改用硬件 CRC 模块（BES2800 内置）
- DMA 链式传输，减少 CPU 介入

---

## 六、附录

### 6.1 关键文件清单

| 文件 | 作用 |
|------|------|
| `camera_hub_2n.py` | 2N 侧发送逻辑、ACK 线程、bitmap 解析 |
| `camera_page.c` | AP 侧 SPI 接收、帧解析、bitmap 生成、文件写入 |
| `spi_slave_driver.c` | SPI 从机驱动（buffer 4096） |
| `bes_spi_slave.c` | BES 底层 SPI（buffer 2048） |

### 6.2 关键参数

| 参数 | 值 | 说明 |
|------|-----|------|
| `CAM_SPI_MAGIC` | 0x494D4731 | SPI 帧同步标志 |
| `CAM_SPI_HDR_SIZE` | 19 | SPI 帧头长度 |
| `CAM_SPI_MAX_PAYLOAD` | 256 | 单帧最大 payload |
| `CAM_SPI_FRAME_SIZE` | 277 | 单帧最大长度（19+256+2） |
| `CAM_RECV_BITMAP_SIZE` | 1024 | bitmap 最大字节数（8192 帧） |
| `SPI_SLAVE_BUFSIZE` | 2048 | BES 底层 DMA buffer |
| `CONFIG_SPI_SLAVE_DRIVER_BUFFER_SIZE` | 4096 | 驱动层 buffer |
| `SPI_INTER_FRAME_DELAY` | 0.003s | 2N 侧帧间延时 |
| `SPI_RETRANSMIT_DELAY` | 0.005s | 2N 侧重传前延时 |
| `CAM_UART_SYNC` | 0xAA55 | UART 帧同步标志 |
| UART 波特率 | 921600 | ACK 通道 |
| SPI 时钟 | 24MHz | 数据通道 |

### 6.3 性能数据汇总

| 场景 | 每帧校验 | 窗口校验 | 末尾 Bitmap |
|------|----------|----------|-------------|
| 缩略图 88KB | 13s | 7s | 2s |
| 大图 1MB | ~9min | ~60s | ~21s |
| 大图 1.3MB | ~12min | ~60s | ~24s |
| 重传开销 | 低 | 高（整窗） | 极低（仅缺失帧） |
| UART 交互（1.3MB） | 5265次 | 878次 | 1~3次 |

### 6.4 bitmap ACK payload 格式

```
+----------+----------+--------+----------+-----------+
| session  | offset   | ok     | nframes  | bitmap    |
| (4B)     | (4B)     | (1B)   | (2B)     | (bmp_len) |
+----------+----------+--------+----------+-----------+

session: 会话 ID（大端）
offset:  已收数据最大偏移（大端）
ok:      1=全部收到，0=有缺失
nframes: 总帧数（大端）
bitmap:  每bit对应一帧，1=已收，0=缺失
bmp_len: (nframes + 7) / 8
```

---

> **文档版本**：v3.0  
> **最后更新**：2026-06-27  
> **维护者**：摄像头传输协议优化团队