#!/usr/bin/env python3
"""
Camera Hub - Lubancat 2N side
Receives UART commands from BES2800BP, executes camera capture,
saves photos to /home/cat/lzw/camera/

UART protocol:
  Frame: [0xAA][0x55][LEN_H][LEN_L][CMD][SEQ][PAYLOAD...][CRC16_H][CRC16_L]
  LEN = 2 + payload_len (includes CMD + SEQ)
  CRC16-CCITT covers LEN ~ PAYLOAD

Commands:
  0x01 CAPTURE_REQ  (2800->2N, payload: empty)
  0x02 CAPTURE_ACK  (2N->2800, payload: filename[32] + thumb_size[4] BE)
  0x03 CAPTURE_FAIL (2N->2800, payload: err_code[1])
"""

import os
import sys
import time
import glob
import struct
import binascii
import subprocess
import threading
import queue
from datetime import datetime
from io import BytesIO

import fcntl
import mmap
import ctypes
import select
import serial
import spidev
from PIL import Image, ImageFilter
try:
    import numpy as np
    _HAS_NUMPY = True
except ImportError:
    _HAS_NUMPY = False
    print("[WARN] numpy not available, UYVY->RGB will use slow Python loop (~5s/frame). "
          "Install with: apt install python3-numpy  OR  pip3 install numpy")

CAMERA_SAVE_DIR = "/home/cat/lzw/camera"
UART_DEV = "/dev/ttyS3"
UART_BAUD = 921600

SPI_BUS = 3
SPI_DEV = 0
SPI_SPEED = 24000000
SPI_MODE = 0
SPI_INTER_FRAME_DELAY = 0.0015
SPI_RETRANSMIT_DELAY = 0.003

THUMB_W = 280
THUMB_H = 158

CMD_CAPTURE_REQ  = 0x01
CMD_CAPTURE_ACK  = 0x02
CMD_CAPTURE_FAIL = 0x03
CMD_SPI_READY      = 0x07
CMD_FRAME_ACK      = 0x08
CMD_GET_IMAGE_REQ  = 0x04
CMD_GET_IMAGE_ACK  = 0x05
CMD_SPI_READY_BIG  = 0x09

CAM_SPI_MAGIC       = 0x494D4731
CAM_SPI_HDR_SIZE    = 19
CAM_SPI_MAX_PAYLOAD = 256
CAM_SPI_TYPE_THUMB_DATA = 1
CAM_SPI_TYPE_THUMB_END  = 2
CAM_SPI_TYPE_BIG_DATA   = 3
CAM_SPI_TYPE_BIG_END    = 4

last_photo_path = ""


def crc16_ccitt(data: bytes) -> int:
    # binascii.crc_hqx 是 C 实现, 比纯 Python 快 50 倍
    # CCITT-FALSE: poly=0x1021, init=0xFFFF, no final XOR
    # 直接传 init=0xFFFF, 验证: crc_hqx(b"123456789", 0xFFFF) == 0x29B1
    return binascii.crc_hqx(data, 0xFFFF)


def build_frame(cmd: int, payload: bytes = b"") -> bytes:
    frame_len = 2 + len(payload)
    header = struct.pack(">HBB", frame_len, cmd, 0)
    crc_data = header + payload
    crc = crc16_ccitt(crc_data)
    return b"\xAA\x55" + crc_data + struct.pack(">H", crc)


def parse_frames(buf: bytes):
    frames = []
    i = 0
    while i < len(buf):
        if i + 1 >= len(buf):
            break
        if buf[i] != 0xAA or buf[i + 1] != 0x55:
            i += 1
            continue
        if i + 4 > len(buf):
            break
        frame_len = struct.unpack(">H", buf[i + 2:i + 4])[0]
        if frame_len < 2 or frame_len > 1078:
            i += 2
            continue
        total = 2 + 2 + frame_len + 2
        if i + total > len(buf):
            break
        cmd = buf[i + 4]
        seq = buf[i + 5]
        payload_len = frame_len - 2
        payload = buf[i + 6:i + 6 + payload_len]
        crc_recv = struct.unpack(">H", buf[i + 6 + payload_len:i + 6 + payload_len + 2])[0]
        crc_calc = crc16_ccitt(buf[i + 2:i + 6 + payload_len])
        if crc_calc != crc_recv:
            print(f"[WARN] CRC mismatch: calc=0x{crc_calc:04X} recv=0x{crc_recv:04X}")
            i += 2
            continue
        frames.append((cmd, seq, payload))
        i += total
    return frames, buf[i:]


TEST_COLOR_MODE = None  # None=正常, "RED", "GREEN", "BLUE", "BGR565"

def make_thumbnail_rgb565(src_path):
    """Generate 280x158 RGB565 thumbnail, keep 16:9 aspect ratio."""
    if TEST_COLOR_MODE == "RED":
        img = Image.new('RGB', (THUMB_W, THUMB_H), (255, 0, 0))
    elif TEST_COLOR_MODE == "GREEN":
        img = Image.new('RGB', (THUMB_W, THUMB_H), (0, 255, 0))
    elif TEST_COLOR_MODE == "BLUE":
        img = Image.new('RGB', (THUMB_W, THUMB_H), (0, 0, 255))
    else:
        img = Image.open(src_path).convert("RGB")
        img = img.resize((THUMB_W, THUMB_H), Image.LANCZOS)
    pixels = img.load()
    buf = bytearray(THUMB_W * THUMB_H * 2)
    idx = 0
    for y in range(THUMB_H):
        for x in range(THUMB_W):
            r, g, b = pixels[x, y]
            if TEST_COLOR_MODE == "BGR565":
                bgr565 = ((b & 0xF8) << 8) | ((g & 0xFC) << 3) | (r >> 3)
                buf[idx]     = bgr565 & 0xFF
                buf[idx + 1] = (bgr565 >> 8) & 0xFF
            else:
                rgb565 = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
                buf[idx]     = rgb565 & 0xFF
                buf[idx + 1] = (rgb565 >> 8) & 0xFF
            idx += 2
    return bytes(buf)


def build_spi_frame(frame_type, session_id, total, offset, data):
    hdr = struct.pack(">IBIIIH", CAM_SPI_MAGIC, frame_type, session_id,
                      total, offset, len(data))
    body = hdr + data
    crc = crc16_ccitt(body)
    return body + struct.pack(">H", crc)


ack_queue = queue.Queue()
cmd_queue = queue.Queue()


def _spi_send(spi, data: bytes):
    """优先用 writebytes2 (零拷贝, spidev>=3.4), 老版本降级 xfer2(list)。

    writebytes2 直接传 bytes, 跳过 list(frame) 转换 (412帧×~1ms ≈ -400ms)。
    注意: writebytes2 不读 MISO, 与 xfer2 在全双工上等价 (我们只用 MOSI 发送)。
    """
    try:
        spi.writebytes2(data)
    except (AttributeError, TypeError):
        spi.xfer2(list(data))


def ack_reader_thread(ser):
    """Background thread: continuously read UART, dispatch FRAME_ACK and commands."""
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
                        nframes = struct.unpack(">H", payload[9:11])[0]
                        bmp_len = (nframes + 7) // 8
                        print(f"[DBG] bitmap ACK: sid={sid} off={off} ok={ok} nframes={nframes} payload_len={len(payload)} bmp_len={bmp_len}")
                        if len(payload) >= 11 + bmp_len and nframes > 0:
                            bitmap = payload[11:11 + bmp_len]
                            missing_offsets = []
                            for fi in range(nframes):
                                byte_idx = fi >> 3
                                bit_idx = fi & 7
                                if byte_idx < len(bitmap):
                                    if not (bitmap[byte_idx] & (1 << bit_idx)):
                                        missing_offsets.append(fi * CAM_SPI_MAX_PAYLOAD)
                            print(f"[DBG] bitmap parsed: missing={len(missing_offsets)} frames")
                        else:
                            print(f"[WARN] bitmap truncated: payload={len(payload)} need={11+bmp_len}")
                    else:
                        print(f"[DBG] legacy ACK: sid={sid} off={off} ok={ok} payload_len={len(payload)}")
                    ack_queue.put((sid, off, ok, missing_offsets))
                else:
                    cmd_queue.put((cmd, seq, payload))
        except Exception as e:
            print(f"[ERROR] ack_reader_thread exception: {e}")
            import traceback
            traceback.print_exc()
            time.sleep(0.01)


def wait_bitmap_ack(session_id, timeout=3.0):
    """Wait for bitmap summary ACK. Returns (ok, received_offset, missing_offsets_list)."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            sid, off, ok, bitmap_info = ack_queue.get(timeout=0.05)
            if sid == session_id:
                return (ok, off, bitmap_info)
        except queue.Empty:
            continue
    return (False, 0, None)


def _drain_ack_queue():
    while not ack_queue.empty():
        try:
            ack_queue.get_nowait()
        except queue.Empty:
            break


def _send_frames_by_offsets(spi, data, session_id, frame_type_data, offsets):
    """Send specific frames at given offsets, then END frame."""
    total = len(data)
    frame_type_end = frame_type_data + 1
    for i, offset in enumerate(offsets):
        chunk = data[offset:offset + CAM_SPI_MAX_PAYLOAD]
        frame = build_spi_frame(frame_type_data, session_id, total, offset, chunk)
        if i < 3 or i == len(offsets) - 1:
            print(f"[DBG] retrans off={offset} sz={len(frame)} pay={len(chunk)}")
        _spi_send(spi, frame)
        time.sleep(SPI_INTER_FRAME_DELAY)
    end_frame = build_spi_frame(frame_type_end, session_id, total, total, b"")
    _spi_send(spi, end_frame)
    return len(offsets)


def spi_send_image(ser, spi, data, session_id, frame_type_data):
    """Send image via SPI: 首次发全帧, 后续仅选择性重传missing帧。

    重写后绝不再重发全帧, 避免重传风暴。
    """
    total = len(data)
    frame_type_end = frame_type_data + 1
    max_retrans = 6  # 最多选择性重传6轮

    # 第1轮: 发送全部帧
    t_start = time.time()
    frame_idx = 0
    offset = 0
    while offset < total:
        chunk = data[offset:offset + CAM_SPI_MAX_PAYLOAD]
        frame = build_spi_frame(frame_type_data, session_id, total, offset, chunk)
        frame_idx += 1
        is_last = (offset + len(chunk) >= total)
        if frame_idx <= 3 or is_last:
            print(f"[DBG] frame#{frame_idx} off={offset} sz={len(frame)} "
                  f"pay={len(chunk)} first8={frame[:8].hex()}")
        _spi_send(spi, frame)
        offset += len(chunk)
        time.sleep(SPI_INTER_FRAME_DELAY)
    end_frame = build_spi_frame(frame_type_end, session_id, total, total, b"")
    _spi_send(spi, end_frame)
    t_send = time.time() - t_start
    print(f"[INFO] Sent {frame_idx} frames + END in {t_send:.3f}s (initial)")

    # 收bitmap, 选择性重传missing帧, 最多max_retrans轮
    for r in range(max_retrans):
        _drain_ack_queue()
        ok, ack_off, missing = wait_bitmap_ack(session_id, timeout=3.0)
        if ok:
            print(f"[INFO] Bitmap ACK ok, offset={ack_off}")
            return True
        if missing is None:
            print(f"[WARN] ACK timeout, retrans round {r+1}")
            time.sleep(SPI_RETRANSMIT_DELAY)
            continue
        n_missing = len(missing)
        if n_missing == 0:
            print(f"[INFO] No missing frames, done")
            return True
        print(f"[WARN] Bitmap NACK, missing={n_missing} frames, retrans round {r+1}")
        time.sleep(SPI_RETRANSMIT_DELAY)
        t0 = time.time()
        n_sent = _send_frames_by_offsets(spi, data, session_id, frame_type_data, missing)
        t_retrans = time.time() - t0
        print(f"[INFO] Retransmitted {n_sent} frames in {t_retrans:.3f}s")
    print(f"[ERROR] All {max_retrans} retrans rounds failed")
    return False


# ===== 性能日志助手 (非侵入式, 单次 <1ms) =====
_stage_stack = []

def stage_begin(name):
    _stage_stack.append((name, time.time()))
    print(f"[STAGE] >> {name}")

def stage_end():
    if not _stage_stack:
        return
    n, t0 = _stage_stack.pop()
    print(f"[STAGE] << {n} {(time.time()-t0)*1000:.1f}ms")

# ===== V4L2 mmap 快速捕获 (替代 gst-launch 做对焦扫描, ~93ms/帧 vs ~4756ms) =====
# 已在 v4l2_probe.py 验证: sizeof(v4l2_buffer)=88, sizeof(v4l2_format)=204
# 关键: v4l2_buffer struct 必须包含 type 字段, 否则 kernel 读 type=0 返回 EINVAL
# S_FMT 通过 v4l2-ctl 子进程完成 (Python ioctl G_FMT 在 RK ISP 上 errno=22)

def _IOC(d, t, nr, sz):
    return (d << 30) | (sz << 16) | (t << 8) | nr

_V = ord('V')

class _v4l2_requestbuffers(ctypes.Structure):
    _fields_ = [
        ("count", ctypes.c_uint32), ("type", ctypes.c_uint32),
        ("memory", ctypes.c_uint32), ("capabilities", ctypes.c_uint32),
        ("reserved", ctypes.c_uint32),
    ]

class _v4l2_timeval(ctypes.Structure):
    _fields_ = [("tv_sec", ctypes.c_long), ("tv_usec", ctypes.c_long)]

class _v4l2_timecode(ctypes.Structure):
    _fields_ = [
        ("type", ctypes.c_uint32), ("flags", ctypes.c_uint32),
        ("frames", ctypes.c_uint8), ("seconds", ctypes.c_uint8),
        ("minutes", ctypes.c_uint8), ("hours", ctypes.c_uint8),
        ("userbits", ctypes.c_uint8 * 4),
    ]

class _v4l2_plane(ctypes.Structure):
    class _m_union(ctypes.Union):
        _fields_ = [
            ("mem_offset", ctypes.c_uint32),
            ("userptr", ctypes.c_ulong),
            ("fd", ctypes.c_int32),
        ]
    _fields_ = [
        ("bytesused", ctypes.c_uint32), ("length", ctypes.c_uint32),
        ("m", _m_union),
        ("data_offset", ctypes.c_uint32), ("reserved", ctypes.c_uint32 * 11),
    ]

class _v4l2_buffer(ctypes.Structure):
    class _m_union(ctypes.Union):
        _fields_ = [
            ("offset", ctypes.c_uint32),
            ("userptr", ctypes.c_ulong),
            ("planes", ctypes.POINTER(_v4l2_plane)),
            ("fd", ctypes.c_int32),
        ]
    _fields_ = [
        ("index", ctypes.c_uint32), ("type", ctypes.c_uint32),
        ("bytesused", ctypes.c_uint32), ("flags", ctypes.c_uint32),
        ("field", ctypes.c_uint32),
        ("timestamp", _v4l2_timeval), ("timecode", _v4l2_timecode),
        ("sequence", ctypes.c_uint32), ("memory", ctypes.c_uint32),
        ("m", _m_union), ("length", ctypes.c_uint32),
        ("reserved2", ctypes.c_uint32), ("reserved", ctypes.c_uint32),
    ]

VIDIOC_REQBUFS   = _IOC(3, _V, 8, ctypes.sizeof(_v4l2_requestbuffers))
VIDIOC_QUERYBUF  = _IOC(3, _V, 9, ctypes.sizeof(_v4l2_buffer))
VIDIOC_QBUF      = _IOC(3, _V, 15, ctypes.sizeof(_v4l2_buffer))
VIDIOC_DQBUF     = _IOC(3, _V, 17, ctypes.sizeof(_v4l2_buffer))
VIDIOC_STREAMON  = _IOC(1, _V, 18, ctypes.sizeof(ctypes.c_int32))
VIDIOC_STREAMOFF = _IOC(1, _V, 19, ctypes.sizeof(ctypes.c_int32))

_V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE = 9
_V4L2_MEMORY_MMAP = 1


class V4L2MmapCapture:
    """V4L2 Multiplanar mmap 捕获, 替代 gst-launch 子进程做对焦扫描。

    实测: 单帧 ~93ms (vs gst-launch ~4756ms), 加速 ~51x。
    6 点对焦扫描 ~559ms, 远低于 gst-launch 路径的单次开销。
    生命周期: start() -> grab_y_sharpness()*N -> stop()
    """

    def __init__(self, dev="/dev/video0", w=1280, h=720):
        self.dev, self.w, self.h = dev, w, h
        self.fd = None
        self.bufs = []
        self.buf_type = _V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE
        self.plane_arr = None
        self.started = False
        # UYVY 1280x720 硬编码 (v4l2-ctl G_FMT 已验证)
        # UYVY 字节顺序: U0 Y0 V0 Y1 ... -> Y 在奇字节 (col*2+1)
        # YUYV 字节顺序: Y0 U  Y1 V  ... -> Y 在偶字节 (col*2)
        self.actual_w = 1280
        self.actual_h = 720
        self.bytesperline = 2560
        self.pixfmt_is_uyvy = True   # True=UYVY, False=YUYV

    def start(self):
        # 1. v4l2-ctl 做 S_FMT (Python ioctl 在 RK ISP 上 errno=22)
        cc_str = "UYVY"
        r = subprocess.run(
            ["v4l2-ctl", "-d", self.dev,
             f"--set-fmt-video=width={self.w},height={self.h},pixelformat={cc_str}"],
            capture_output=True, timeout=5)
        if r.returncode != 0:
            raise RuntimeError(f"v4l2-ctl set-fmt failed rc={r.returncode}")

        # 2. open fd
        self.fd = os.open(self.dev, os.O_RDWR | os.O_NONBLOCK)

        # 3. REQBUFS
        req = _v4l2_requestbuffers()
        req.count, req.type, req.memory = 4, self.buf_type, _V4L2_MEMORY_MMAP
        fcntl.ioctl(self.fd, VIDIOC_REQBUFS, req)
        n_bufs = req.count

        # 4. QUERYBUF + mmap + QBUF
        self.plane_arr = (_v4l2_plane * 8)()
        for i in range(n_bufs):
            buf = _v4l2_buffer()
            buf.index = i
            buf.type = self.buf_type
            buf.memory = _V4L2_MEMORY_MMAP
            buf.m.planes = ctypes.cast(self.plane_arr, ctypes.POINTER(_v4l2_plane))
            buf.length = 8
            fcntl.ioctl(self.fd, VIDIOC_QUERYBUF, buf)
            off = self.plane_arr[0].m.mem_offset
            ln = self.plane_arr[0].length
            mobj = mmap.mmap(self.fd, ln, mmap.MAP_SHARED,
                             mmap.PROT_READ | mmap.PROT_WRITE, offset=off)
            self.bufs.append((off, ln, mobj))
            self.plane_arr[0].bytesused = 0
            self.plane_arr[0].data_offset = 0
            buf.type = self.buf_type
            buf.memory = _V4L2_MEMORY_MMAP
            fcntl.ioctl(self.fd, VIDIOC_QBUF, buf)

        # 5. STREAMON
        stream_on = ctypes.c_int32(self.buf_type)
        fcntl.ioctl(self.fd, VIDIOC_STREAMON, stream_on)
        self.started = True
        print(f"[INFO] V4L2 mmap capture started: {self.dev} {self.w}x{self.h} "
              f"({n_bufs} bufs)")

    def grab_y_sharpness(self, target_w=320, target_h=180, timeout=2.0,
                         discard=2):
        """捕获一帧, 提取 Y 分量子采样到 target_w x target_h, 计算 Laplacian 方差。
        单次调用 ~93ms (DQBUF 0.1ms + Y提取 50ms + Laplacian 42ms)。

        discard: 默认丢弃2帧旧buffer (VCM移动前/曝光未稳的图像), 确保返回的
                sharpness 对应当前 focus 位置。这显著提升对焦曲线峰值的可见性。
        """
        if not self.started:
            return 0.0
        # 丢弃队列里的旧帧 (VCM 移动前拍的)
        for _ in range(discard):
            if not self._discard_one(timeout=0.05):
                break
        try:
            r, _, _ = select.select([self.fd], [], [], timeout)
            if not r:
                return 0.0
            buf = _v4l2_buffer()
            buf.type = self.buf_type
            buf.memory = _V4L2_MEMORY_MMAP
            buf.m.planes = ctypes.cast(self.plane_arr, ctypes.POINTER(_v4l2_plane))
            buf.length = 8
            fcntl.ioctl(self.fd, VIDIOC_DQBUF, buf)
            _, _, mobj = self.bufs[buf.index]

            # Y 提取
            # UYVY: U0 Y0 V0 Y1 ... -> Y 在奇字节 (col*2+1)
            # YUYV: Y0 U  Y1 V  ... -> Y 在偶字节 (col*2)
            row_stride = self.bytesperline
            step_x = max(1, self.actual_w // target_w)
            step_y = max(1, self.actual_h // target_h)
            y_shift = 1 if self.pixfmt_is_uyvy else 0
            y_data = bytearray(target_w * target_h)
            idx = 0
            for row in range(0, self.actual_h, step_y):
                base = row * row_stride
                for col in range(0, self.actual_w, step_x):
                    y_data[idx] = mobj[base + col * 2 + y_shift]
                    idx += 1
            img = Image.frombytes("L", (target_w, target_h), bytes(y_data))

            # 归还 buffer
            self.plane_arr[0].bytesused = 0
            self.plane_arr[0].data_offset = 0
            buf.type = self.buf_type
            buf.memory = _V4L2_MEMORY_MMAP
            fcntl.ioctl(self.fd, VIDIOC_QBUF, buf)

            # Laplacian 方差
            lap = img.filter(ImageFilter.Kernel((3, 3), [0,1,0,1,-4,1,0,1,0], 1, 0))
            px = list(lap.getdata())
            n = len(px)
            mean = sum(px) / n
            var = sum((p - mean) ** 2 for p in px) / n
            return var
        except Exception as e:
            print(f"[WARN] grab_y_sharpness failed: {e}")
            return 0.0

    def grab_uyvy_to_jpeg(self, filepath, quality=70, timeout=3.0, discard=2):
        """抓一帧 UYVY 转换为 JPEG 保存, 替代 gst-launch 拍照子进程。

        复用对焦扫描时启动的 mmap capture, 节省 gst-launch 启动开销 ~450ms。
        UYVY (YUV422) 转 RGB 用标准 BT.601 公式, 与 gst videoconvert 一致。
        discard=2: 丢弃 VCM 移动到 best_v 后的旧帧, 确保拍照帧是 best_v 位置。
        """
        if not self.started:
            return False
        for _ in range(discard):
            if not self._discard_one(timeout=0.05):
                break
        try:
            r, _, _ = select.select([self.fd], [], [], timeout)
            if not r:
                return False
            buf = _v4l2_buffer()
            buf.type = self.buf_type
            buf.memory = _V4L2_MEMORY_MMAP
            buf.m.planes = ctypes.cast(self.plane_arr, ctypes.POINTER(_v4l2_plane))
            buf.length = 8
            fcntl.ioctl(self.fd, VIDIOC_DQBUF, buf)
            _, _, mobj = self.bufs[buf.index]

            w, h = self.actual_w, self.actual_h
            stride = self.bytesperline
            if _HAS_NUMPY:
                # numpy 向量化路径: ~80ms for 1280x720 (vs Python 循环 ~5300ms)
                # UYVY packed: [U0,Y0,V0,Y1] per 4 bytes, 2 pixels per quartet
                raw = np.frombuffer(mobj, dtype=np.uint8,
                                   count=h * stride).reshape(h, stride)
                uvy = raw[:, :w * 2].reshape(-1, 4)
                u = uvy[:, 0].astype(np.int16) - 128
                y0 = uvy[:, 1].astype(np.int16)
                v = uvy[:, 2].astype(np.int16) - 128
                y1 = uvy[:, 3].astype(np.int16)
                # 扩展 U/V 到全分辨率 (每个 U/V 对应 2 个像素)
                u_pair = np.stack((u, u), axis=1).reshape(h, w)
                v_pair = np.stack((v, v), axis=1).reshape(h, w)
                y_full = np.empty((h, w), dtype=np.int16)
                y_full[:, 0::2] = y0.reshape(h, w // 2)
                y_full[:, 1::2] = y1.reshape(h, w // 2)
                # BT.601 YUV->RGB
                r = np.clip(y_full + 1.402 * v_pair, 0, 255).astype(np.uint8)
                g = np.clip(y_full - 0.344 * u_pair - 0.714 * v_pair, 0, 255).astype(np.uint8)
                b = np.clip(y_full + 1.772 * u_pair, 0, 255).astype(np.uint8)
                rgb = np.dstack((r, g, b))

                self.plane_arr[0].bytesused = 0
                self.plane_arr[0].data_offset = 0
                buf.type = self.buf_type
                buf.memory = _V4L2_MEMORY_MMAP
                fcntl.ioctl(self.fd, VIDIOC_QBUF, buf)

                img = Image.fromarray(rgb, "RGB")
                img.save(filepath, "JPEG", quality=quality)
                return True

            # Slow fallback (numpy unavailable): ~5300ms/frame
            rgb = bytearray(w * h * 3)
            for y in range(h):
                base = y * stride
                for x in range(0, w, 2):
                    off = base + x * 2
                    u = mobj[off] - 128
                    y0 = mobj[off + 1]
                    v = mobj[off + 2] - 128
                    y1 = mobj[off + 3]
                    r0 = int(y0 + 1.402 * v)
                    g0 = int(y0 - 0.344 * u - 0.714 * v)
                    b0 = int(y0 + 1.772 * u)
                    r1 = int(y1 + 1.402 * v)
                    g1 = int(y1 - 0.344 * u - 0.714 * v)
                    b1 = int(y1 + 1.772 * u)
                    p = (y * w + x) * 3
                    rgb[p]   = 0 if r0 < 0 else (255 if r0 > 255 else r0)
                    rgb[p+1] = 0 if g0 < 0 else (255 if g0 > 255 else g0)
                    rgb[p+2] = 0 if b0 < 0 else (255 if b0 > 255 else b0)
                    rgb[p+3] = 0 if r1 < 0 else (255 if r1 > 255 else r1)
                    rgb[p+4] = 0 if g1 < 0 else (255 if g1 > 255 else g1)
                    rgb[p+5] = 0 if b1 < 0 else (255 if b1 > 255 else b1)

            self.plane_arr[0].bytesused = 0
            self.plane_arr[0].data_offset = 0
            buf.type = self.buf_type
            buf.memory = _V4L2_MEMORY_MMAP
            fcntl.ioctl(self.fd, VIDIOC_QBUF, buf)

            img = Image.frombytes("RGB", (w, h), bytes(rgb))
            img.save(filepath, "JPEG", quality=quality)
            return True
        except Exception as e:
            print(f"[WARN] grab_uyvy_to_jpeg failed: {e}")
            return False

    def _discard_one(self, timeout=0.1):
        """丢弃一个buffer里的帧 (下DQBUF拿到的帧太旧, 比如VCM移动前拍的)。"""
        if not self.started:
            return False
        try:
            r, _, _ = select.select([self.fd], [], [], timeout)
            if not r:
                return False
            buf = _v4l2_buffer()
            buf.type = self.buf_type
            buf.memory = _V4L2_MEMORY_MMAP
            buf.m.planes = ctypes.cast(self.plane_arr, ctypes.POINTER(_v4l2_plane))
            buf.length = 8
            fcntl.ioctl(self.fd, VIDIOC_DQBUF, buf)
            self.plane_arr[0].bytesused = 0
            self.plane_arr[0].data_offset = 0
            buf.type = self.buf_type
            buf.memory = _V4L2_MEMORY_MMAP
            fcntl.ioctl(self.fd, VIDIOC_QBUF, buf)
            return True
        except Exception:
            return False

    def stop(self):
        if not self.started:
            return
        try:
            stream_off = ctypes.c_int32(self.buf_type)
            fcntl.ioctl(self.fd, VIDIOC_STREAMOFF, stream_off)
        except Exception:
            pass
        for _, _, mobj in self.bufs:
            try:
                mobj.close()
            except Exception:
                pass
        self.bufs.clear()
        if self.fd is not None:
            try:
                os.close(self.fd)
            except Exception:
                pass
            self.fd = None
        self.started = False
        print("[INFO] V4L2 mmap capture stopped")


# ===== V4L2 IOCTL 对焦控制 (直接IOCTL ~1ms, vs v4l2-ctl ~200ms) =====
# 驱动实际注册ID (v4l2-ctl --list-ctrls 显示), vendor扩展camera-class控件
V4L2_CID_FOCUS_ABSOLUTE = 0x009A090A
VIDIOC_S_CTRL           = 0x4008561C  # _IOW('V', 28, struct v4l2_control)

_focus_fd = None
_pipeline = None
_ioctl_mode = None  # None=未探测, "ioctl"=快速路径, "v4l2"=降级子进程
_last_best_focus = 32  # 上次对焦最佳值, 供do_capture轻量冲刷重设焦用


class CameraPipeline:
    """持久化 GStreamer 管线, 替代每次拍照的 gst-launch 子进程。

    消除冷启动开销(~350ms/次), 单帧抓取 ~55ms。
    脚本启动时常驻, 1280x720 RGB buffer ~2.7MB, 无内存压力。
    """

    def __init__(self, dev="/dev/video0", w=1280, h=720):
        self.dev, self.w, self.h = dev, w, h
        self.pipeline = self.appsink = self._Gst = None
        self._sample_queue = queue.Queue()

    def _on_new_sample(self, appsink):
        # 信号回调: appsink 交付新帧时触发, 存入队列供 pull_rgb_frame 取用
        # 用 emit("pull-sample") 绕过 binding 未暴露 pull_sample 的问题
        sample = appsink.emit("pull-sample")
        if sample is not None:
            try:
                self._sample_queue.put_nowait(sample)
            except queue.Full:
                pass
        return self._Gst.FlowReturn.OK

    def start(self, warmup_frames=2):
        import gi
        gi.require_version('Gst', '1.0')
        from gi.repository import Gst
        Gst.init(None)
        self._Gst = Gst
        desc = (f"v4l2src device={self.dev} ! videoconvert ! videoscale ! "
                f"video/x-raw,width={self.w},height={self.h},format=RGB ! "
                f"appsink name=sink max-buffers=8 drop=true sync=false")
        self.pipeline = Gst.parse_launch(desc)
        self.appsink = self.pipeline.get_by_name("sink")
        self.appsink.set_property("emit-signals", True)
        self.appsink.connect("new-sample", self._on_new_sample)
        self.pipeline.set_state(Gst.State.PLAYING)
        ret = self.pipeline.get_state(Gst.SECOND * 2)
        if ret[0] != Gst.StateChangeReturn.SUCCESS:
            raise RuntimeError(f"Pipeline state change failed: {ret[0]}")
        self.discard_frames(warmup_frames, timeout=2.0)
        print(f"[INFO] Pipeline ready: {self.dev} {self.w}x{self.h} "
              f"(warmed {warmup_frames} frames)")

    def pull_rgb_frame(self, timeout=2.0):
        Gst = self._Gst
        try:
            sample = self._sample_queue.get(timeout=timeout)
        except queue.Empty:
            raise RuntimeError("appsink frame timeout")
        buf = sample.get_buffer()
        ok, info = buf.map(Gst.MapFlags.READ)
        if not ok:
            raise RuntimeError("buffer map failed")
        try:
            data = bytes(info.data)
        finally:
            buf.unmap(info)
        caps = sample.get_caps().get_structure(0)
        return data, caps.get_value("width"), caps.get_value("height")

    def discard_frames(self, n, timeout=1.0):
        for _ in range(n):
            try:
                self.pull_rgb_frame(timeout=timeout)
            except Exception:
                break

    def stop(self):
        if self.pipeline and self._Gst:
            self.pipeline.set_state(self._Gst.State.NULL)
        self.pipeline = self.appsink = None

    def restart(self):
        print("[WARN] Restarting camera pipeline")
        self.stop()
        self.start(warmup_frames=2)


def _open_focus_fd(dev):
    global _focus_fd
    if _focus_fd is not None:
        return _focus_fd
    try:
        _focus_fd = os.open(dev, os.O_RDWR)
        print(f"[INFO] Focus FD opened: {dev} (fd={_focus_fd})")
        return _focus_fd
    except Exception as e:
        print(f"[WARN] Open focus FD failed: {e}")
        return None


def _set_focus_ioctl(fd, value):
    fcntl.ioctl(fd, VIDIOC_S_CTRL,
                struct.pack("=Ii", V4L2_CID_FOCUS_ABSOLUTE, int(value)))


def _set_focus_safe(dev, value):
    global _ioctl_mode
    if _ioctl_mode == "ioctl":
        try:
            _set_focus_ioctl(_focus_fd, value)
            return True
        except Exception as e:
            print(f"[WARN] IOCTL set focus failed: {e}")
            return False
    # v4l2-ctl 降级 (vendor扩展控件可能仅支持 S_EXT_CTRLS, 不支持 legacy S_CTRL)
    subprocess.run(
        ["v4l2-ctl", "-d", dev, "--set-ctrl", f"focus_absolute={value}"],
        capture_output=True, timeout=3)
    return True


def init_camera_subsystem():
    """打开对焦 FD + 探测 IOCTL 可用性。

    NOTE: PyGObject appsink 管线在本环境存在 GIL 死锁 + binding 不全问题,
    暂时禁用持久化管线, 保留 gst-launch 子进程路径。
    IOCTL 降级 + 对焦步数优化(粗扫3+细扫2)仍然生效。
    后续可换 filesink 文件模式重试持久化。
    """
    global _pipeline, _ioctl_mode
    print("[INFO] Initializing camera subsystem (no pipeline, gst-launch mode)...")
    af_dev, ctrl = _find_focus_device()
    if af_dev and ctrl == "focus_absolute":
        _open_focus_fd(af_dev)
        if _focus_fd is not None:
            try:
                _set_focus_ioctl(_focus_fd, 48)
                _ioctl_mode = "ioctl"
                print("[INFO] IOCTL focus control: OK (fast path)")
            except Exception as e:
                _ioctl_mode = "v4l2"
                print(f"[WARN] IOCTL focus unsupported ({e}), fallback v4l2-ctl")
    _pipeline = None
    print("[INFO] Camera subsystem ready (gst-launch fallback)")


def shutdown_camera_subsystem():
    """退出时释放管线与对焦 FD。"""
    global _pipeline, _focus_fd
    print("[INFO] Shutting down camera subsystem...")
    if _pipeline:
        _pipeline.stop()
        _pipeline = None
    if _focus_fd is not None:
        try:
            os.close(_focus_fd)
        except Exception:
            pass
        _focus_fd = None


_AF_DEVICE_CACHE = None
_AF_CTRL_NAME = None


def _find_focus_device():
    """Locate the V4L2 node exposing a focus control.

    VCM (voice-coil motor) drivers commonly register as /dev/v4l-subdev* rather
    than on /dev/video0, so we scan every video and subdev node. Result is cached.
    Returns (device_path, ctrl_name) or (None, None).
    """
    global _AF_DEVICE_CACHE, _AF_CTRL_NAME
    if _AF_DEVICE_CACHE is not None:
        return _AF_DEVICE_CACHE, _AF_CTRL_NAME

    candidates = sorted(glob.glob("/dev/video*")) + sorted(glob.glob("/dev/v4l-subdev*"))
    auto_dev = None
    abs_dev = None
    for dev in candidates:
        try:
            r = subprocess.run(
                ["v4l2-ctl", "-d", dev, "--list-ctrls"],
                capture_output=True, text=True, timeout=3
            )
            if r.returncode != 0:
                continue
            for line in r.stdout.splitlines():
                parts = line.split()
                if not parts:
                    continue
                name = parts[0]
                if name == "focus_auto" and auto_dev is None:
                    print(f"[INFO] Found focus_auto on {dev}")
                    auto_dev = dev
                elif name == "focus_absolute" and abs_dev is None:
                    print(f"[INFO] Found focus_absolute on {dev}")
                    abs_dev = dev
        except Exception:
            continue

    if auto_dev is not None:
        _AF_DEVICE_CACHE = auto_dev
        _AF_CTRL_NAME = "focus_auto"
    elif abs_dev is not None:
        _AF_DEVICE_CACHE = abs_dev
        _AF_CTRL_NAME = "focus_absolute"
    else:
        print("[WARN] No focus control found on any /dev/video* or /dev/v4l-subdev*")
        return None, None

    print(f"[INFO] Using {_AF_CTRL_NAME} on {_AF_DEVICE_CACHE}")
    return _AF_DEVICE_CACHE, _AF_CTRL_NAME


_AF_PROBE_TMP = "/tmp/_af_probe.jpg"


def _probe_sharpness(capture_dev, w=320, h=180):
    """Capture a frame and return Laplacian-variance sharpness metric.

    Laplacian方差是标准对焦度量: 对焦正确时边缘多, 方差大。
    分辨率 320x180 保留足够高频细节。gst-launch 用 num-buffers=2
    丢弃首帧(陈旧/曝光未稳定), filesink 最终保留最后一帧。
    """
    try:
        if _pipeline is not None:
            _pipeline.discard_frames(1, timeout=0.5)
            data, fw, fh = _pipeline.pull_rgb_frame(timeout=2.0)
            img = Image.frombytes("RGB", (fw, fh), data).convert("L")
            if (fw, fh) != (w, h):
                img = img.resize((w, h), Image.BILINEAR)
        else:
            subprocess.run(
                ["gst-launch-1.0", "-q",
                 "v4l2src", f"device={capture_dev}", "num-buffers=1", "!",
                 "videoconvert", "!",
                 "videoscale", "!",
                 f"video/x-raw,width={w},height={h}", "!",
                 "jpegenc", "quality=80", "!",
                 "filesink", f"location={_AF_PROBE_TMP}"],
                capture_output=True, timeout=5
            )
            if not os.path.exists(_AF_PROBE_TMP) or os.path.getsize(_AF_PROBE_TMP) == 0:
                return 0.0
            img = Image.open(_AF_PROBE_TMP).convert("L")
        # Laplacian 算子: [0,1,0; 1,-4,1; 0,1,0]
        lap = img.filter(ImageFilter.Kernel((3, 3), [0, 1, 0, 1, -4, 1, 0, 1, 0], 1, 0))
        px = list(lap.getdata())
        n = len(px)
        mean = sum(px) / n
        var = sum((p - mean) ** 2 for p in px) / n
        return var
    except Exception as e:
        print(f"[WARN] sharpness probe failed: {e}")
        return 0.0
    finally:
        try:
            os.unlink(_AF_PROBE_TMP)
        except Exception:
            pass


def _get_ctrl_max(dev, name):
    """Parse the max value of a V4L2 control from --list-ctrls output."""
    try:
        r = subprocess.run(
            ["v4l2-ctl", "-d", dev, "--list-ctrls"],
            capture_output=True, text=True, timeout=3
        )
        if r.returncode != 0:
            return None
        for line in r.stdout.splitlines():
            if name in line and "max=" in line:
                i = line.index("max=") + 4
                num = ""
                while i < len(line) and line[i].isdigit():
                    num += line[i]
                    i += 1
                if num:
                    return int(num)
    except Exception:
        return None
    return None


def _trigger_autofocus(dev):
    """Trigger AF: prefer continuous AF on the discovered subdev; fall back to
    a focus_absolute sweep so the VCM exercises the lens over its travel range.
    Safe to call when v4l2-ctl is missing or camera has no AF support.
    """
    af_dev, ctrl = _find_focus_device()
    if af_dev is None:
        return False
    try:
        if ctrl == "focus_auto":
            print(f"[INFO] Enabling continuous AF on {af_dev}...")
            subprocess.run(
                ["v4l2-ctl", "-d", af_dev, "--set-ctrl=focus_auto=1"],
                capture_output=True, timeout=3
            )
            time.sleep(1.5)
            print(f"[INFO] Locking AF on {af_dev} before capture...")
            subprocess.run(
                ["v4l2-ctl", "-d", af_dev, "--set-ctrl=focus_auto=0"],
                capture_output=True, timeout=3
            )
            time.sleep(0.2)
            return True

        print(f"[INFO] CDAF sweep on {af_dev} (capture_dev={dev})...")
        max_val = _get_ctrl_max(af_dev, "focus_absolute")
        if not max_val:
            print("[WARN] Cannot determine focus_absolute range, skip")
            return False

        use_ioctl = _ioctl_mode == "ioctl"
        # VCM 物理移动需要 ~150-200ms 稳定, 文字对焦要求更高, 用 0.2s
        t_settle = 0.2

        def _set_focus(v):
            _set_focus_safe(af_dev, v)
            time.sleep(t_settle)

        # 启动 V4L2 mmap 快速捕获路径 (替代 gst-launch)
        # 失败时降级回 _probe_sharpness 的 gst-launch 子进程路径
        mc = V4L2MmapCapture(dev)
        try:
            mc.start()
            t_mc = time.time()
            # 预热: 丢首帧 (sensor 启动延迟, DQBUF ~223ms 仅首次)
            mc.grab_y_sharpness(timeout=2.0)
            print(f"[PERF] mmap warmup: {(time.time()-t_mc)*1000:.1f}ms")
        except Exception as e:
            print(f"[WARN] mmap capture start failed ({e}), fallback gst-launch")
            mc = None

        success = False
        try:
            # 粗扫 5 档: 全行程均匀采样, 比 4 档更易捕获文字峰值
            coarse = [0, max_val // 4, max_val // 2, 3 * max_val // 4, max_val]
            best_s = -1.0
            best_v = max_val // 2
            meas = {}  # pos -> sharpness, 用于抛物线插值
            print(f"[INFO] -- coarse sweep ({len(coarse)} pts, ioctl={use_ioctl}) --")
            for v in coarse:
                _set_focus(v)
                if mc is not None:
                    s = mc.grab_y_sharpness()
                else:
                    s = _probe_sharpness(dev)
                meas[v] = s
                print(f"[INFO]   pos={v:>5d} sharpness={s:.2f}")
                if s > best_s:
                    best_s = s
                    best_v = v

            # 细扫: best_v 两侧 ±step, ±2*step, step=max(1, max//32)
            # 文字对焦对位置敏感, step 取小值精确定位峰值 (max=64 时 step=2)
            fine_step = max(1, max_val // 32)
            fine = []
            for d in (1, 2):
                for sign in (-1, 1):
                    v = best_v + sign * d * fine_step
                    if 0 <= v <= max_val and v not in coarse and v not in fine:
                        fine.append(v)
            # 递增顺序扫描, 消除 VCM 回程差 (hysteresis):
            # VCM 来回跳变会导致实际位置滞后于指令, sharpness 读数失真
            fine = sorted(fine)
            print(f"[INFO] -- fine sweep ({len(fine)} pts, step={fine_step}) around best={best_v} --")
            for v in fine:
                _set_focus(v)
                if mc is not None:
                    s = mc.grab_y_sharpness()
                else:
                    s = _probe_sharpness(dev)
                meas[v] = s
                print(f"[INFO]   fine pos={v:>5d} sharpness={s:.2f}")
                if s > best_s:
                    best_s = s
                    best_v = v

            # 抛物线插值: 用 best_v 及左右邻居 (step 间距) 3 点拟合, 求亚步长峰值
            # 零耗时 (~1ms 计算), 解决 step=2 量化误差, 提升文字对焦精度
            # 邻居可能来自 coarse 或 fine, 都已在 meas 中
            v_l = best_v - fine_step
            v_r = best_v + fine_step
            if v_l in meas and v_r in meas:
                y_l, y_m, y_r = meas[v_l], best_s, meas[v_r]
                a = (y_l + y_r) / 2 - y_m  # 二次项, <0 表示凸峰
                if a < -1e-6:
                    b = (y_r - y_l) / 2
                    x_peak = -b / (2 * a)  # 相对偏移 [-1, 1]
                    v_peak = best_v + x_peak * fine_step
                    v_peak_int = max(0, min(max_val, round(v_peak)))
                    if v_peak_int != best_v:
                        print(f"[INFO] Parabolic peak: {v_peak:.2f} -> {v_peak_int} "
                              f"(meas best={best_v} s={best_s:.2f})")
                        best_v = v_peak_int

            print(f"[INFO] Best focus_absolute={best_v} (sharpness={best_s:.2f})")
            _set_focus(best_v)
            global _last_best_focus
            _last_best_focus = best_v
            success = True
            return mc
        finally:
            # 仅在异常/未成功时停止 mc; 成功路径由 do_capture 复用 mc 拍照
            if not success and mc is not None:
                try:
                    mc.stop()
                except Exception:
                    pass
    except FileNotFoundError:
        print("[WARN] v4l2-ctl not found, skip autofocus")
        return False
    except Exception as e:
        print(f"[WARN] autofocus trigger exception: {e}")
        return False


def do_capture():
    os.makedirs(CAMERA_SAVE_DIR, exist_ok=True)
    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    filename = f"photo_{timestamp}.jpg"
    filepath = os.path.join(CAMERA_SAVE_DIR, filename)

    dev = "/dev/video0"
    print(f"[INFO] Using camera device: {dev}")
    t_total = time.time()
    mc = None

    try:
        if _pipeline is not None:
            t0 = time.time()
            _pipeline.discard_frames(2, timeout=1.0)
            print(f"[PERF] flush_stale: {(time.time()-t0)*1000:.1f}ms")

            t0 = time.time()
            _trigger_autofocus(dev)
            print(f"[PERF] autofocus: {(time.time()-t0)*1000:.1f}ms")

            t0 = time.time()
            _pipeline.discard_frames(1, timeout=1.0)
            print(f"[PERF] af_flush: {(time.time()-t0)*1000:.1f}ms")

            t0 = time.time()
            print("[INFO] Capturing photo via pipeline (1280x720)...")
            data, w, h = _pipeline.pull_rgb_frame(timeout=3.0)
            img = Image.frombytes("RGB", (w, h), data)
            img.save(filepath, "JPEG", quality=70)
            print(f"[PERF] grab_encode: {(time.time()-t0)*1000:.1f}ms")
        else:
            t0 = time.time()
            print("[INFO] Warming up camera (discard first frames)...")
            subprocess.run(
                ["gst-launch-1.0", "-q",
                 "v4l2src", f"device={dev}", "num-buffers=4", "!",
                 "videoconvert", "!",
                 "videoscale", "!",
                 "video/x-raw,width=1280,height=720", "!",
                 "fakesink"],
                capture_output=True, timeout=10
            )
            print(f"[PERF] warmup: {(time.time()-t0)*1000:.1f}ms")

            t0 = time.time()
            mc = _trigger_autofocus(dev)
            print(f"[PERF] autofocus: {(time.time()-t0)*1000:.1f}ms")

            t0 = time.time()
            if mc is not None:
                print("[INFO] Capturing photo via mmap (1280x720 q70)...")
                ok = mc.grab_uyvy_to_jpeg(filepath, quality=70, discard=3)
                print(f"[PERF] grab_encode: {(time.time()-t0)*1000:.1f}ms")
                if not ok:
                    print("[ERROR] mmap capture failed, fallback gst-launch")
                    _af_fb, _ = _find_focus_device()
                    if _af_fb and _last_best_focus is not None:
                        _set_focus_safe(_af_fb, _last_best_focus)
                        time.sleep(0.2)
                    result = subprocess.run(
                        ["gst-launch-1.0", "-q",
                         "v4l2src", f"device={dev}", "num-buffers=2", "!",
                         "videoconvert", "!",
                         "videoscale", "!",
                         "video/x-raw,width=1280,height=720", "!",
                         "jpegenc", "quality=80", "!",
                         "filesink", f"location={filepath}"],
                        capture_output=True, timeout=15
                    )
                    if result.returncode != 0:
                        print(f"[ERROR] gst capture failed: {result.stderr.decode(errors='replace')}")
                        return None, 1
            else:
                print("[INFO] Capturing photo (1280x720 q80)...")
                result = subprocess.run(
                    ["gst-launch-1.0", "-q",
                     "v4l2src", f"device={dev}", "num-buffers=2", "!",
                     "videoconvert", "!",
                     "videoscale", "!",
                     "video/x-raw,width=1280,height=720", "!",
                     "jpegenc", "quality=80", "!",
                     "filesink", f"location={filepath}"],
                    capture_output=True, timeout=15
                )
                print(f"[PERF] grab_encode: {(time.time()-t0)*1000:.1f}ms")
                if result.returncode != 0:
                    print(f"[ERROR] gst capture failed: {result.stderr.decode(errors='replace')}")
                    return None, 1

        if not os.path.exists(filepath) or os.path.getsize(filepath) == 0:
            print("[ERROR] captured file is empty or missing")
            return None, 2

        # 自适应JPEG压缩: AI限制<170KB, 留5KB余量目标≤165KB
        # mmap 路径直接出 q70 (已检查大小), gst 路径才需重压
        MAX_JPG_SIZE = 165000
        file_size = os.path.getsize(filepath)
        if file_size > MAX_JPG_SIZE:
            t0 = time.time()
            try:
                img = Image.open(filepath)
                final_q = 70
                for q in [60, 50, 40, 30, 20]:
                    img.save(filepath, "JPEG", quality=q)
                    final_q = q
                    if os.path.getsize(filepath) <= MAX_JPG_SIZE:
                        break
                file_size = os.path.getsize(filepath)
                print(f"[INFO] Recompressed to q{final_q}: {file_size} bytes "
                      f"({(time.time()-t0)*1000:.1f}ms)")
            except Exception as e:
                print(f"[WARN] recompress failed: {e}")
                file_size = os.path.getsize(filepath)
        elif mc is not None:
            print(f"[INFO] JPEG size OK: {file_size} bytes (mmap direct q70)")

        print(f"[INFO] Captured: {filepath} ({file_size} bytes)")
        print(f"[PERF] capture_total: {(time.time()-t_total)*1000:.1f}ms")
        if mc is not None:
            try:
                mc.stop()
            except Exception:
                pass
        return filename, 0

    except subprocess.TimeoutExpired:
        print("[ERROR] gst capture timed out")
        if mc is not None:
            try:
                mc.stop()
            except Exception:
                pass
        return None, 1
    except Exception as e:
        print(f"[ERROR] capture exception: {e}")
        if mc is not None:
            try:
                mc.stop()
            except Exception:
                pass
        return None, 1


def send_capture_ack(ser, filename, thumb_w, thumb_h, thumb_size):
    payload = filename.encode("utf-8").ljust(32, b"\x00")[:32]
    payload += struct.pack(">HHI", thumb_w, thumb_h, thumb_size)
    frame = build_frame(CMD_CAPTURE_ACK, payload)
    print(f"[DEBUG] ACK frame ({len(frame)}B): {frame.hex()}")
    n = ser.write(frame)
    ser.flush()
    time.sleep(0.05)
    print(f"[INFO] Sent CAPTURE_ACK: {filename} ({n}B)")
    print("[INFO] Waiting for next command...")


def send_capture_fail(ser, err_code):
    frame = build_frame(CMD_CAPTURE_FAIL, bytes([err_code]))
    ser.write(frame)
    ser.flush()
    print(f"[INFO] Sent CAPTURE_FAIL: err={err_code}")


def send_get_image_ack(ser, filename, file_size):
    payload = filename.encode("utf-8").ljust(32, b"\x00")[:32]
    payload += struct.pack(">I", file_size)
    frame = build_frame(CMD_GET_IMAGE_ACK, payload)
    print(f"[INFO] Sent GET_IMAGE_ACK: {filename} ({file_size} bytes)")
    ser.write(frame)
    ser.flush()
    time.sleep(0.05)


def check_uart_baudrate():
    """Check UART baudrate and set to 921600 if not."""
    print(f"[INFO] Checking UART baudrate on {UART_DEV}...")
    try:
        result = subprocess.run(
            ["stty", "-F", UART_DEV],
            capture_output=True, text=True, timeout=3
        )
        if result.returncode != 0:
            print(f"[WARN] stty read failed: {result.stderr.strip()}")
            return

        current = None
        for line in result.stdout.splitlines():
            line = line.strip()
            if line.startswith("speed"):
                parts = line.split()
                if len(parts) >= 2:
                    current = int(parts[1])
                break

        if current is None:
            print("[WARN] Could not parse current baudrate")
            return

        print(f"[INFO] Current baudrate: {current}")
        if current == UART_BAUD:
            print(f"[INFO] Baudrate already {UART_BAUD}, OK")
        else:
            print(f"[WARN] Current baudrate: {current}, expected {UART_BAUD}")
            print(f"[INFO] Setting baudrate to {UART_BAUD}...")
            result = subprocess.run(
                ["stty", "-F", UART_DEV,
                 "ispeed", str(UART_BAUD),
                 "ospeed", str(UART_BAUD)],
                capture_output=True, text=True, timeout=3
            )
            if result.returncode != 0:
                print(f"[ERROR] stty set failed: {result.stderr.strip()}")
                return

            time.sleep(0.1)
            verify = subprocess.run(
                ["stty", "-F", UART_DEV],
                capture_output=True, text=True, timeout=3
            )
            new_baud = None
            for line in verify.stdout.splitlines():
                line = line.strip()
                if line.startswith("speed"):
                    parts = line.split()
                    if len(parts) >= 2:
                        new_baud = int(parts[1])
                    break
            if new_baud == UART_BAUD:
                print(f"[INFO] Baudrate verified: {new_baud} OK")
            else:
                print(f"[ERROR] Baudrate verify failed: got {new_baud}, expected {UART_BAUD}")
    except FileNotFoundError:
        print("[WARN] stty command not found, skip baudrate check")
    except Exception as e:
        print(f"[WARN] baudrate check exception: {e}")


_ts_cap_start = 0.0
_ts_cap_done = 0.0
_ts_thumb_done = 0.0
_ts_spi_thumb_done = 0.0
_ts_big_ack_done = 0.0
_ts_spi_big_done = 0.0


def _print_pipeline_summary():
    """打印从拍照命令到大图传成功的全链路耗时总览。"""
    try:
        cap = (_ts_cap_done - _ts_cap_start) * 1000 if _ts_cap_done else 0
        thumb = (_ts_thumb_done - _ts_cap_done) * 1000 if _ts_thumb_done and _ts_cap_done else 0
        spi_thumb = (_ts_spi_thumb_done - _ts_thumb_done) * 1000 if _ts_spi_thumb_done and _ts_thumb_done else 0
        big_ack = (_ts_big_ack_done - _ts_spi_thumb_done) * 1000 if _ts_big_ack_done and _ts_spi_thumb_done else 0
        spi_big = (_ts_spi_big_done - _ts_big_ack_done) * 1000 if _ts_spi_big_done and _ts_big_ack_done else 0
        total = (_ts_spi_big_done - _ts_cap_start) * 1000 if _ts_spi_big_done and _ts_cap_start else 0
        print("=" * 60)
        print("[SUMMARY] 全链路耗时:")
        print(f"  [1] 拍照(do_capture)     : {cap:8.1f}ms")
        print(f"  [2] 缩略图生成          : {thumb:8.1f}ms")
        print(f"  [3] SPI传缩略图         : {spi_thumb:8.1f}ms")
        print(f"  [4] 等待大图请求+ACK    : {big_ack:8.1f}ms")
        print(f"  [5] SPI传大图           : {spi_big:8.1f}ms")
        print(f"  [TOTAL] 拍照命令→大图传完: {total:8.1f}ms")
        print("=" * 60)
    except Exception as e:
        print(f"[WARN] summary failed: {e}")


def main():
    import sys
    global SPI_SPEED, SPI_MODE
    global last_photo_path
    global _ts_cap_start, _ts_cap_done, _ts_thumb_done, _ts_spi_thumb_done, _ts_big_ack_done, _ts_spi_big_done
    if len(sys.argv) > 1:
        SPI_SPEED = int(sys.argv[1])
        print(f"[INFO] SPI speed overridden to {SPI_SPEED} Hz")
    if len(sys.argv) > 2:
        SPI_MODE = int(sys.argv[2])
        if SPI_MODE not in (0, 1, 2, 3):
            print(f"[ERROR] Invalid SPI mode {SPI_MODE}, must be 0/1/2/3")
            sys.exit(1)
        print(f"[INFO] SPI mode overridden to {SPI_MODE}")
    print(f"[INFO] Camera Hub 2N starting on {UART_DEV} @ {UART_BAUD}, SPI @ {SPI_SPEED} mode={SPI_MODE}")
    os.makedirs(CAMERA_SAVE_DIR, exist_ok=True)

    check_uart_baudrate()

    try:
        ser = serial.Serial(
            UART_DEV, UART_BAUD,
            timeout=0.1,
            rtscts=False,
            dsrdtr=False,
            xonxoff=False
        )
    except Exception as e:
        print(f"[ERROR] Failed to open {UART_DEV}: {e}")
        sys.exit(1)

    print("[INFO] UART opened, waiting for commands...")

    init_camera_subsystem()

    threading.Thread(target=ack_reader_thread, args=(ser,), daemon=True).start()
    print("[INFO] ACK reader thread started")

    while True:
        try:
            try:
                cmd, seq, payload = cmd_queue.get(timeout=0.1)
            except queue.Empty:
                continue

            print(f"[INFO] RX: cmd=0x{cmd:02X} seq={seq} len={len(payload)}")

            if cmd == CMD_CAPTURE_REQ:
                print("[INFO] CAPTURE_REQ received, starting capture...")
                _ts_cap_start = time.time()
                filename, err = do_capture()
                _ts_cap_done = time.time()
                if filename:
                    last_photo_path = os.path.join(CAMERA_SAVE_DIR, filename)
                    # [方案B] 不再生成 RGB565 缩略图，直接发 CAPTURE_ACK（thumb_size=0）
                    send_capture_ack(ser, filename, THUMB_W, THUMB_H, 0)
                else:
                    send_capture_fail(ser, err)
            elif cmd == CMD_SPI_READY:
                # [方案B] 缩略图不再通过 SPI 传输，忽略 SPI_READY
                print("[INFO] SPI_READY ignored (scheme B: no thumbnail)")
            elif cmd == CMD_GET_IMAGE_REQ:
                print("[INFO] GET_IMAGE_REQ received")
                if not last_photo_path or not os.path.exists(last_photo_path):
                    print("[ERROR] no photo available")
                    frame = build_frame(CMD_CAPTURE_FAIL, bytes([3]))
                    ser.write(frame)
                    ser.flush()
                    continue
                file_size = os.path.getsize(last_photo_path)
                filename = os.path.basename(last_photo_path)
                print(f"[INFO] Photo: {filename} ({file_size} bytes)")
                send_get_image_ack(ser, filename, file_size)
                _ts_big_ack_done = time.time()
            elif cmd == CMD_SPI_READY_BIG:
                if len(payload) >= 4:
                    session_id = struct.unpack(">I", payload[0:4])[0]
                    print(f"[INFO] SPI_READY_BIG received, session={session_id}")
                    if not last_photo_path or not os.path.exists(last_photo_path):
                        print("[ERROR] no photo to send")
                        continue
                    try:
                        with open(last_photo_path, "rb") as f:
                            photo_data = f.read()
                        spi = spidev.SpiDev()
                        spi.open(SPI_BUS, SPI_DEV)
                        spi.max_speed_hz = SPI_SPEED
                        spi.mode = SPI_MODE
                        time.sleep(0.05)
                        print(f"[INFO] SPI opened for big image: {len(photo_data)} bytes")
                        print("[INFO] Sending big image via SPI...")
                        ok = spi_send_image(ser, spi, photo_data,
                                            session_id, CAM_SPI_TYPE_BIG_DATA)
                        spi.close()
                        _ts_spi_big_done = time.time()
                        if ok:
                            print("[INFO] Big image sent successfully")
                            _print_pipeline_summary()
                        else:
                            print("[ERROR] Big image send failed")
                    except Exception as e:
                        print(f"[ERROR] Big image send exception: {e}")
            else:
                print(f"[WARN] Unknown cmd: 0x{cmd:02X}")

        except KeyboardInterrupt:
            print("\n[INFO] Interrupted, exiting...")
            break
        except serial.SerialException as e:
            print(f"[ERROR] Serial error: {e}")
            time.sleep(0.5)
        except Exception as e:
            print(f"[ERROR] Main loop exception: {type(e).__name__}: {e}")
            time.sleep(0.5)

    shutdown_camera_subsystem()
    ser.close()
    print("[INFO] UART closed")


if __name__ == "__main__":
    main()
