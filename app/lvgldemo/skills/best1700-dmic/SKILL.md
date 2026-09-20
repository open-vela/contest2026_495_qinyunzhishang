---
name: "best1700-dmic"
description: "best1700 平台 DMIC 配置与音量优化指南（当前开发板 AMIC 外围电路 NC，仅支持 DMIC）。当用户需要配置 DMIC、解决录音音量偏低、调整 codec 增益或排查 DMIC 录音问题时调用。"
---

# best1700 平台 DMIC 配置与音量优化

> **⚠️ 重要**：当前开发板的 AMIC 外围电路全部 NC（未连接），**无法使用 AMIC**，只能使用开发板默认的 DMIC。任何将通道掩码改为 `0x1`（AMIC）的配置都会导致录音无声音。

本 skill 涵盖 best1700 平台上 DMIC 的完整配置流程，以及录音音量优化方案。

---

## 1. 涉及的代码路径

| 文件 | 作用 |
|------|------|
| `boards/best1700_ep/glass_demo/src/board_bes1700_evb.c` | 音频输入路径配置、通道映射 |
| `boards/best1700_ep/glass_demo/include/board_bes1700_evb.h` | `CODEC_SADC_VOL` 等增益宏定义 |
| `framework/services/platform/hal/best1700/hal_iomux_best1700.c` | GPIO pinmux 配置（CLK/DAT） |
| `framework/services/platform/hal/best1700/hal_codec_best1700.c` | codec 音量控制、ADC 增益设置 |
| `framework/services/platform/hal/hal_codec_common.c` | `codec_mic_chan_vol` 通道增益映射表 |
| `rtos/apps/examples/lvgldemo/wakeup_detector.cpp` | 唤醒词检测器（实际音频采集路径） |
| `rtos/apps/examples/lvgldemo/recorder_page.c` | 录音页面（UI 层录音功能） |
| `rtos/frameworks/multimedia/media/custom/smf_media_graph.arecord.c` | SMF 媒体图录音流管理 |

---

## 2. SMF JSON 配置文件（关键）

### 2.1 必须使用 DMIC 通道掩码

在 `framework/services/prebuild/smf_glass_demo.json` 中，**所有音频采集流的 `chmap` 必须设置为 `0x1000`（DMIC）**，绝对不能使用 `0x1`（AMIC）。

**正确配置示例**（所有 `chmap` 均为 `0x1000`）：

```json
// 录音流（arec0）
"mic,src-dma,0,thread=#1,devName=codec2,chmap=#0x1000,..."

// SCO 上行流（scoup）
"mic,src-dma,0,thread=#1,devName=codec2,chmap=#0x1000,..."

// VAD 流
"src,src-dma,0,devName=codec2,chmap=#0x1000,..."

// 麦克风流（mic0/mic1/mic2）
"mic,src-dma,0,devName=codec,chmap=#0x1000,..."
```

**⚠️ 错误配置**（会导致无声）：

```json
// ❌ 错误：chmap=#0x1 是 AMIC，开发板上已 NC
"mic,src-dma,0,thread=#1,devName=codec2,chmap=#0x1,..."
```

**常见错误场景**：
- 从其他项目或旧版本复制 `smf_glass_demo.json` 时，可能包含 `chmap=#0x1` 的配置
- Git merge 或 rebase 时可能引入错误的通道掩码修改
- **排查无声问题时，首先检查 JSON 文件中的所有 `chmap` 是否都是 `0x1000`**

### 2.2 需要修改的 JSON 配置位置

在 `smf_glass_demo.json` 中，以下位置的 `chmap` 都必须确保为 `0x1000`：

| 位置 | 流名称 | 行号（参考） |
|------|--------|-------------|
| `arec0` pipeline | 录音流 | ~371 |
| `scoup` pipeline | SCO 上行流 | ~389 |
| `mic0` src | 麦克风流 0 | ~595 |
| `mic1` pipeline | 麦克风流 1（+回声+算法） | ~600 |
| `mic2` pipeline | 麦克风流 2（+I2S 回声+算法） | ~609 |
| `vad` pipeline | VAD 检测流 | ~817 |

---

## 3. AMIC 转 DMIC 修改步骤（已废弃，仅供参考）

> **注意**：由于开发板 AMIC 已 NC，此章节仅作为历史参考。当前项目**不应**再执行 AMIC 转 DMIC 的切换操作。

### 3.1 GPIO Pinmux 配置

在 `hal_iomux_best1700.c` 中将 GPIO82 配置为 DMIC CLK，GPIO83 配置为 DMIC DAT。

关键 API：
```c
hal_iomux_set_dmic_clk(HAL_IOMUX_FUNC_GPIO_82);
hal_iomux_set_dmic_dat(HAL_IOMUX_FUNC_GPIO_83);
```

### 3.2 音频输入路径修改

在 `board_bes1700_evb.c` 的 `audio_input_path_cfg` 或等效配置中，将通道映射从模拟麦克风改为数字麦克风：

```c
// 修改前（AMIC）
.channel_map = AUD_CHANNEL_MAP_CH0,

// 修改后（DMIC）
.channel_map = AUD_CHANNEL_MAP_DIGMIC_CH0,   // 值为 (1<<12) = 0x1000
```

### 3.3 验证 DMIC 初始化

编译烧录后，通过日志确认 DMIC 是否正确初始化。正常日志应包含：

```
[hal_codec_enable_dig_mic] user=0, mic_map=1000, set=1
```

`mic_map=1000`（即 0x1000）表示 `AUD_CHANNEL_MAP_DIGMIC_CH0` 已启用。

---

## 4. 音量优化

### 4.1 音量控制架构

best1700 平台的音频采集有两个层级：

1. **board 层**：`CODEC_SADC_VOL` 宏定义默认 ADC 增益，在 codec 流初始化时应用
2. **运行时层**：通过 `hal_codec_set_chan_vol()` 直接操作 codec HAL 寄存器

**重要**：音频采集如果使用 SMF media graph（如 wakeup_detector），则不经过 audioflinger。`af_stream_set_chan_vol()` 会因为 stream status 缺少 `AF_STATUS_STREAM_OPEN_CLOSE` 标志而报错：

```
[af_stream_set_chan_vol] ERROR: status = 1
```

**必须使用 `hal_codec_set_chan_vol()` 绕过 audioflinger。**

### 4.2 提升默认增益（board 层）

在 `board_bes1700_evb.h` 中设置：

```c
#define CODEC_SADC_VOL (15)              // 最大 ADC 增益索引
#define CODEC_DIGMIC_CH0_SADC_VOL  CODEC_SADC_VOL
```

> **说明**：`CODEC_DIGMIC_CH0_SADC_VOL` 必须显式定义，否则 `hal_codec_common.c` 中的默认值 `CODEC_SADC_VOL` 会在某些构建配置下不生效。

增益映射表在 `board_bes1700_evb.c` 中定义（覆盖 `hal_codec_best1700.c` 中的 weak 默认值）：

```c
const CODEC_ADC_VOL_T codec_adc_vol[TGT_ADC_VOL_LEVEL_QTY] = {
    -99, 0, 2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 22, 26, 33, 39,
};
// 索引 15 = 39dB（推荐最大值，兼顾音量和防破音）
// 索引 10 = 20dB（SMF 默认 vol=100 时对应的增益）
```

> **增益调优说明**：`codec_adc_vol[15]` 控制 DMIC 最终数字增益。AMIC 和 DMIC 共享同一个数字 ADC 增益级（`hal_codec_set_dig_adc_gain()`），修改此表同时影响两种麦克风。
>
> 增益选取需要在**远场音量**和**近场破音**之间平衡：
> - 28dB（原始）：远场偏轻，近场安全
> - 36dB：远场够用，近场安全
> - 39dB（推荐）：远场充足，近场大概率安全
> - 42dB：近场破音
>
> 调优方法：以 2~3dB 为步长迭代，编译烧录后实机测试远/近场。近场破音则降低，远场偏轻则提高。硬件极限为 42dB（`__SSAT(val, 20)` 8.12 定点格式上限）。

### 4.3 运行时设置增益（wakeup_detector 路径）

在 `wakeup_detector.cpp` 的 `recorder_prepare_and_start()` 函数中，`media_recorder_start()` 之后调用。

**前置声明**（文件顶部，`#define TAG` 之后）：

```cpp
/* --- DMIC gain control ------------------------------------------------- */
extern "C" {
extern int hal_codec_set_chan_vol(uint32_t stream, uint32_t ch_map,
                                 uint8_t vol) __attribute__((weak));
}

#define BES_AUD_STREAM_CAPTURE          1       /* AUD_STREAM_CAPTURE */
#define BES_AUD_CHANNEL_MAP_DIGMIC_CH0  (1<<12) /* AUD_CHANNEL_MAP_DIGMIC_CH0 */
#define BES_MIC_GAIN_MAX                15      /* codec_adc_vol[15] = max gain */
```

**增益设置代码**（`recorder_prepare_and_start()` 中 `media_recorder_start()` 成功后）：

```cpp
  /* Set codec ADC digital gain to maximum.
     Using hal_codec_set_chan_vol() instead of af_stream_set_chan_vol()
     because the SMF media graph capture path bypasses audioflinger. */
  if (hal_codec_set_chan_vol)
    {
      int ret = hal_codec_set_chan_vol(BES_AUD_STREAM_CAPTURE,
                                       BES_AUD_CHANNEL_MAP_DIGMIC_CH0,
                                       BES_MIC_GAIN_MAX);
      syslog(LOG_INFO, "[%s] mic gain set to max (index %u), ret=%d\n",
             TAG, BES_MIC_GAIN_MAX, ret);
    }
```

### 4.4 运行时设置增益（recorder_page 路径）

`recorder_page.c` 的录音功能同样需要设置增益。在 `recorder_start()` 中 `media_recorder_start()` 之后调用相同的 `hal_codec_set_chan_vol()`：

```c
/* Set hardware mic gain to maximum AFTER the audio stream has started. */
if (hal_codec_set_chan_vol) {
    int ret = hal_codec_set_chan_vol(
        BES_AUD_STREAM_CAPTURE,
        BES_AUD_CHANNEL_MAP_DIGMIC_CH0,
        BES_MIC_GAIN_MAX);
    LV_LOG_USER("mic gain set to max (~39dB), ret=%d", ret);
}
```

> **注意**：`recorder_page.c` 和 `wakeup_detector.cpp` 都需要各自调用 `hal_codec_set_chan_vol()`，两条音频采集路径是独立的。

### 4.5 SMF Media Graph 音量设置

SMF 录音流在 `smf_media_graph.arecord.c` 中通过 `smf_media_audio_recorder_set_volume()` 设置音量。SMF 设置 `vol=100` 时对应 codec 增益索引 10（20dB）。

如需在 SMF 层面提升音量，应将 `vol` 值映射到更高的增益索引。但更推荐的方式是在 `wakeup_detector.cpp` 中直接调用 `hal_codec_set_chan_vol()` 设置最大增益。

---

## 5. 常见问题排查

### 5.1 录音完全没有声音

**最高优先级检查**：
1. **检查 `smf_glass_demo.json` 中所有 `chmap` 是否为 `0x1000`**（DMIC），绝对不能是 `0x1`（AMIC）
2. 检查 GPIO pinmux 是否正确配置（GPIO82=CLK, GPIO83=DAT）
3. 检查日志中是否有 `[hal_codec_enable_dig_mic]` 且 `mic_map=0x1000`
4. 确认 DMIC 硬件连接正常

**快速诊断命令**：
```bash
# 检查 JSON 配置中是否有错误的 AMIC 通道掩码
grep -n "chmap=#0x1[^0]" framework/services/prebuild/smf_glass_demo.json
# 如果有任何输出，说明存在错误的 AMIC 配置，必须改为 0x1000
```

### 5.2 录音音量偏小

1. 检查 `CODEC_SADC_VOL` 是否设置为最大值 15
2. 检查 `CODEC_DIGMIC_CH0_SADC_VOL` 是否**显式定义**为 `CODEC_SADC_VOL`
3. 确认 `SINGLE_CODEC_ADC_VOL` 未被定义（定义后禁止单通道增益调节）
4. 确认 `codec_adc_vol[15]` 的值是否足够（推荐 39dB，可逐步上调）
5. 在 `wakeup_detector.cpp` 和 `recorder_page.c` 的音频采集路径中都调用 `hal_codec_set_chan_vol()`
6. 查看日志中的 `rms` 值判断实际音量水平，16kHz/16bit 下 RMS 应在 3000~15000 之间

### 5.3 近场说话破音（数字削波）

`codec_adc_vol[15]` 增益过高时，近场大声说话会导致 PCM 采样值超过 int16 上限（±32767），产生削波失真。

1. 降低 `codec_adc_vol[15]` 的值，以 2~3dB 为步长迭代
2. 实机测试：近距离正常说话不应听到"滋滋"破音
3. 参考安全范围：28~39dB，超过 42dB 必定削波（硬件寄存器上限）
4. 最终值需兼顾远场音量和近场防破音，通过迭代实机测试确定

### 5.4 `af_stream_set_chan_vol` 报错 `status = 1`

此错误表示 audioflinger 的 stream 状态不满足要求。改用 `hal_codec_set_chan_vol()` 直接操作 codec HAL。

### 5.5 `hal_codec_set_chan_vol` 未被调用

该函数声明为 `__attribute__((weak))`，如果链接时未找到实际定义则为 NULL。确认 `hal_codec_best1700.c` 已编译链接。

---

## 6. 关键常量速查

| 常量 | 值 | 说明 |
|------|-----|------|
| `AUD_STREAM_CAPTURE` | 1 | 采集流 |
| `AUD_CHANNEL_MAP_DIGMIC_CH0` | 0x1000 (1<<12) | 数字麦克风通道0（**必须使用**） |
| `AUD_CHANNEL_MAP_CH0` | 0x1 | 模拟麦克风通道0（**开发板已 NC，禁用**） |
| `CODEC_SADC_VOL` | 15 | 最大 ADC 增益索引 |
| `codec_adc_vol[15]` | 39 dB | 索引 15 推荐增益值（28~42dB 可调范围） |
| `codec_adc_vol[10]` | 20 dB | 索引 10 对应的增益值（SMF 默认 vol=100） |
| `WAKEUP_SAMPLE_RATE` | 16000 | 唤醒词检测采样率 |
| `WAKEUP_CHANNELS` | 1 | 唤醒词检测通道数 |
| `WAKEUP_BITS` | 16 | 唤醒词检测采样位深 |

---

## 7. 修改操作流程

1. **验证 SMF JSON 配置**（**最关键**）：
   - 检查 `framework/services/prebuild/smf_glass_demo.json` 中所有 `chmap` 必须为 `#0x1000`（DMIC）
   - 绝对不能使用 `#0x1`（AMIC），开发板 AMIC 已 NC
   - 快速检查命令：`grep -n "chmap=#0x1[^0]" framework/services/prebuild/smf_glass_demo.json`
2. **配置 GPIO**：在 `hal_iomux_best1700.c` 中将 GPIO82/GPIO83 配置为 DMIC 功能
3. **修改音频路径**：在 `board_bes1700_evb.c` 中确认 `channel_map` 为 `AUD_CHANNEL_MAP_DIGMIC_CH0`
4. **设置 board 层增益**：
   - `board_bes1700_evb.h`：`CODEC_SADC_VOL=15`，显式定义 `CODEC_DIGMIC_CH0_SADC_VOL`
   - `board_bes1700_evb.c`：调优 `codec_adc_vol[15]`（推荐从 39dB 开始）
5. **运行时增益（wakeup_detector）**：在 `wakeup_detector.cpp` 的 `recorder_prepare_and_start()` 中 `media_recorder_start()` 后调用 `hal_codec_set_chan_vol()`
6. **运行时增益（recorder_page）**：在 `recorder_page.c` 的 `recorder_start()` 中同样调用
7. **编译验证**：烧录后检查日志中的 `[hal_codec_enable_dig_mic]`、`mic gain set to max` 和 `rms=` 确认增益生效
8. **实机调优**：根据远/近场测试反馈，以 2~3dB 步长微调 `codec_adc_vol[15]`，直到远场够响且近场不破音