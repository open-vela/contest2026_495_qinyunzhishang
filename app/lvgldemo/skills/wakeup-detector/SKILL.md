---
name: "wakeup-detector"
description: "语音唤醒词检测系统完整指南。涵盖模型训练、参数调优和运行流程。当用户要修改唤醒词功能、重训模型、调整唤醒阈值或排查唤醒异常时调用。"
---

# 语音唤醒词检测系统 (wakeup-detector)

## 系统概述

本系统是一个运行在嵌入式设备 (best1700_ep) 上的语音唤醒词检测器，基于 TFLite Micro 两阶段流水线：
1. **Audio Preprocessor** 将 PCM 音频转换为 Mel 频谱特征 (int8, zp=-128)
2. **Micro Speech** 对特征进行分类，识别 "xiaoQxiaoQ" 唤醒词

**当前状态：经过 5 轮训练迭代，量化模型精度 99.1%，已部署到设备。第5轮模型在测试集上精度高，但实际运行中仍存在任意语音误唤醒问题（模型过拟合导致输出饱和），正在排查修复。备份模型（第3轮，14,976 bytes）工作正常，仅对 "xiaoqxiaoq" 唤醒。**

相关文件清单：

| 文件 | 用途 |
|------|------|
| [wakeup_detector.cpp](file://rtos/apps/examples/lvgldemo/wakeup_detector.cpp) | 唤醒检测器主实现 (1038行) |
| [wakeup_detector.h](file://rtos/apps/examples/lvgldemo/wakeup_detector.h) | 公共 API 声明 |
| [Kconfig](file://rtos/apps/examples/lvgldemo/Kconfig) | 唤醒参数 Kconfig 定义 |
| [defconfig](file://boards/best1700_ep/glass_demo/configs/ap/defconfig) | 当前参数配置 |
| [xiaoq_wakeup_model_data.h](file://rtos/apps/examples/lvgldemo/xiaoq_wakeup_model_data.h) | 训练好的 TFLite 语音模型 (15048 bytes, int8, 第5轮) |
| [xiaoq_wakeup_model_data_back.h](file://rtos/apps/examples/lvgldemo/xiaoq_wakeup_model_data_back.h) | 备份 TFLite 语音模型 (14976 bytes, int8, 第3轮，工作正常) |
| [xiaoq_wakeup_preprocessor_data.h](file://rtos/apps/examples/lvgldemo/xiaoq_wakeup_preprocessor_data.h) | 预处理器 TFLite 模型 C 头文件 |
| [train_xiaoq_model_colab.py](file://train_xiaoq_model_colab.py) | 统一训练脚本（唯一训练入口） |
| `./project/xiaoQxiaoQ/*.wav` | 训练用唤醒词语音数据（各人录制的 "xiaoQxiaoQ" 音频，共 2,864 条） |
| `./project/xiaoQxiaoQ_optimized/*.wav` | 优化后的训练音频（已裁剪 pop 音和静音，共 2,864 条，**训练必须使用**） |
| [optimize_wav_files.py](file://optimize_wav_files.py) | 音频优化脚本（裁剪开头 70ms pop 音 + 裁剪前后静音保留 2s） |
| `./project/_background_noise_/*.wav` | 原始背景噪声数据（46 个文件，包含 Google 6 种噪声 + 本地录制噪声） |
| `./project/_background_noise_optimized/*.wav` | 优化后的背景噪声（已裁剪开头 100ms pop 音，46 个文件，**训练必须使用**） |
| [optimize_noise_files.py](file://optimize_noise_files.py) | 噪声优化脚本（裁剪开头 100ms pop 音） |
| `./project/negative_chinese_wakewords/*.wav` | 中文唤醒词负样本原始录音（"小爱同学"、"小度小度"、"小智小智" 等，共 359 条） |
| `./project/negative_chinese_wakewords_optimized/*.wav` | 优化后的中文唤醒词负样本（已裁剪 pop 音和静音，共 357 条，**训练必须使用**） |
| [optimize_negative_wakewords.py](file://optimize_negative_wakewords.py) | 负样本优化脚本（裁剪开头 70ms pop 音 + 裁剪前后静音保留 2s，策略同 optimize_wav_files.py） |
| `./models/xiaoq_micro_speech.tflite` | 量化后的 TFLite 模型文件 |
| `./models/float_model.tflite` | 浮点 TFLite 模型文件（精度对比用） |

---

## 一、训练唤醒词模型

### 1.1 训练演进历史

模型经过 5 轮迭代训练：

| 轮次 | 脚本 | TL;DR | 关键决策与教训 |
|------|------|-------|---------------|
| 第1轮 | `retrain.py`（已删除） | 引入 Google Speech Commands v0.02 的 35 个词作为 unknown 负样本，`TRAINING_STEPS="15000,3000"`，TF v2.13.0 | **优**：35 类 unknown 词汇带来极佳的负样本多样性，泛化好。**劣**：依赖外部 2.3GB 数据集下载，无法离线运行 |
| 第2轮 | `auto_train.py`（已删除） | 改用纯本地音频，不依赖 Google 数据集，`TRAINING_STEPS="12000,3000"`，TF v2.13.0 | **优**：完全离线，白噪声+粉红噪声合成背景。**劣**：缺少 unknown 词汇导致 false positive 偏高 |
| 第3轮 | `train_xiaoq_model_colab.py`（旧版） | Colab GPU，TF v2.15.0，`TRAINING_STEPS="12000,3000"`，含精度测试 Cell | **优**：GPU 加速 + float/quant 模型精度对比验证，**识别率 ~70%**。**劣**：需 Colab 环境，未知词仍缺失。**此模型为当前备份模型（14,976 bytes），实际运行中仅对 "xiaoqxiaoq" 唤醒，误唤醒率低** |
| 第4轮 | `train_xiaoq_model_colab.py`（统一脚本） | 合并 retrain.py + auto_train.py + continue_train.py 为单一脚本，支持离线/Colab 双模式，`TRAINING_STEPS="12000,3000"`，int8 量化 | **优**：一站式训练+导出+部署，支持 Google Commands 可选，**模型 15048 bytes**。**劣**：preprocessor 导出有已知限制，只部署 speech 模型 |
| **第5轮** | `train_xiaoq_model_colab.py`（当前） | `DATA_MODE="google_dataset"` 默认模式，34 类 Google Speech Commands unknown 词 + 1,814 条唤醒词，训练 15,000 步 | **优**：**Float 97.9% / Quant 99.1% 精度**，3 分类（silence/unknown/xiaoqxiaoq）。**劣**：**实际运行中任意语音误唤醒**，模型过拟合导致输出饱和（`out=[-128,-128,127]`），需 2.3GB Google 数据集联网下载 |

### 1.1.1 第5轮过拟合问题诊断

**症状**：设备运行日志显示，模型对任意语音输出饱和值 `out=[-128,-128,127]`，keyword 得分 0.996（int8 最大值），6 次推理中 4 次误唤醒。

**根因**：
1. **训练步数过多**：15,000 步对 tiny_conv（仅 8 个 Conv2D 滤波器，~15KB）来说过大，导致检验集精度虽高但过拟合
2. **类别不均衡**：34% 关键词 vs 33% unknown（但 unknown 来自 34 个不同类别，每类仅约 1%），关键词在训练中类内权重过高
3. **Google Speech Commands 的 unknown 词都是清晰的短指令**（"yes", "no", "up" 等），与真实场景中的随意对话差异大，导致模型在真实语音上泛化差
4. **输入 scale 不匹配**：当前 speech 模型输入 scale=0.10171568，而 preprocessor 模型输出 scale 与备份模型（0.10140932）匹配。`generate_features()` 中 preprocessor 输出直接 `memcpy` 到 speech 输入，**无重新量化**。scale 差异导致 speech 模型以错误的量化参数解释特征值，相当于输入了一批"变形"的频谱数据，模型自然给出错误的高置信度输出

**修复方向**：
- **最优先**：确保 speech 模型输入 scale 与 preprocessor 输出 scale 一致。部署新模型时，检查 `wakeup_detector.cpp` 启动日志中 `preprocessor output: ... scale=X` 与 `speech: input=... s=Y` 是否匹配，偏差应 < 0.0001
- 减少训练步数到 5,000~8,000
- 调整类别比例：`UNKNOWN_PERCENTAGE=50`, `SILENT_PERCENTAGE=30`，让关键词降至 ~20%
- 在训练脚本中固定量化输入 scale（通过 `--representative_dataset` 或 `--default_ranges_min/max` 参数），确保与 preprocessor 一致
- 直接使用备份模型（第3轮，已验证工作正常，scale 与 preprocessor 匹配）

### 1.2 当前部署模型与备份模型对比

当前部署的是第5轮训练的量化模型，存在过拟合问题。备份模型（第3轮）工作正常。

| 属性 | 第5轮（当前，异常） | 第3轮（备份，正常） |
|------|-------------------|-------------------|
| 文件 | [xiaoq_wakeup_model_data.h](file://rtos/apps/examples/lvgldemo/xiaoq_wakeup_model_data.h) | [xiaoq_wakeup_model_data_back.h](file://rtos/apps/examples/lvgldemo/xiaoq_wakeup_model_data_back.h) |
| 大小 | 15,048 bytes | 14,976 bytes |
| TF 版本 | 2.21.0 | 2.15.0 |
| 输入 scale | 0.10171568 | 0.10140932 |
| 输入 zp | -128 | -128 |
| 输出 scale | 0.00390625 | 0.00390625 |
| 输出 zp | -128 | -128 |
| MatMul 输出 zp | -15 | -4 |
| Unknown 词汇 | Google 34 类 | 无（仅 silence） |
| 实际误唤醒 | **高（饱和输出）** | **低（正常）** |

两个模型使用相同的 `tiny_conv` 架构：

```
Input (49x40 int8, zp=-128)
  → Conv2D (8 filters, 内核 8x10, stride 2x2, ReLU)
  → Flatten → Reshape
  → FullyConnected (3 classes)
  → Softmax
```

三个分类：silence(0), unknown(1), xiaoqxiaoq(2)

量化方式：int8 全整数量化（input/output 均为 int8）

### 1.3 训练脚本说明

#### `train_xiaoq_model_colab.py` — 统一训练脚本（当前唯一训练入口）

**位置**：[train_xiaoq_model_colab.py](file://train_xiaoq_model_colab.py)

**功能**：整合了 retrain.py、auto_train.py、continue_train.py 的所有功能，是唯一的训练入口。旧的独立脚本已全部删除。

**模式选择**：
- `DATA_MODE = "google_dataset"`：**默认推荐模式**，自动下载 Google Speech Commands v0.02（约 2.3GB），提供 34 类 unknown 词汇作为负样本
- `DATA_MODE = "local_dirs"`：纯本地模式，使用 `AUDIO_DIR = "./project/xiaoQxiaoQ_optimized"` 目录下的 WAV 文件（**必须使用优化后的音频**），仅 silence 作为负样本（不推荐，会导致 false positive 偏高）
- `DATA_MODE = "colab_upload"`：Colab 环境，上传 zip 文件后自动使用 Google Speech Commands

**训练参数**：
- `WANTED_WORDS = "xiaoqxiaoq"`
- `MODEL_ARCHITECTURE = "tiny_conv"`（输入 49x40，8 个 Conv2D 滤波器，3 分类）
- `TRAINING_STEPS = "12000,3000"`（总计 15,000 步）
  - ⚠️ **注意**：15,000 步对 tiny_conv（~15KB）过大，建议改为 `"5000,3000"`（总计 8,000 步）避免过拟合。脚本默认值未修改以保持向后兼容，训练时应手动调整
- `LEARNING_RATE = "0.001,0.0001"`
- `SAMPLE_RATE = 16000`，`WINDOW_SIZE_MS = 30`，`WINDOW_STRIDE_MS = 20`
- `FEATURE_BIN_COUNT = 40`，`CLIP_DURATION_MS = 1000`
- `SILENT_PERCENTAGE = 33`，`UNKNOWN_PERCENTAGE = 33`（三分类均分）
- `BACKGROUND_FREQUENCY = 0.8`，`BACKGROUND_VOLUME_RANGE = 0.1`
- `TIME_SHIFT_MS = 100.0`
- `VERBOSITY = "WARN"`，`EVAL_STEP_INTERVAL = "1000"`

**训练流程（7 个步骤）**：
1. 数据准备（自动下载 Google Speech Commands + 扫描本地优化后的 WAV + 合成背景噪声）
   - ⚠️ **重要**：本地唤醒词音频必须使用 `./project/xiaoQxiaoQ_optimized/` 目录（已优化）
   - ⚠️ **重要**：背景噪声必须使用 `./project/_background_noise_optimized/` 目录（已优化）
   - ❌ **禁止**：不允许使用原始未优化的音频文件进行训练
2. 环境检查（GPU 可用性）
3. 训练完整模型
4. 冻结模型（frozen graph / saved_model）
5. 转换为 TFLite（float + int8 量化 + 代表数据集）
6. 精度测试（float + quant 模型对比验证）
7. 导出 preprocessor + 转换为 C 源码（xxd，含 `alignas(16)` 对齐）

**训练产物**：
- `xiaoq_micro_speech.tflite` — 量化后的 TFLite 语音模型（约 15KB）
- `float_model.tflite` — 浮点 TFLite 模型（约 52KB，用于精度对比）
- `xiaoq_micro_speech.cc` — C 源码数组（通过 xxd 生成）
- `xiaoq_audio_preprocessor.tflite` — 预处理器模型
- `xiaoq_audio_preprocessor.cc` — 预处理器 C 源码

**部署步骤**：
1. 将 `xiaoq_micro_speech.cc` 内容复制替换 [xiaoq_wakeup_model_data.h](file://rtos/apps/examples/lvgldemo/xiaoq_wakeup_model_data.h)
   - ⚠️ 必须在数组声明前添加 `alignas(16)` 前缀
   - ⚠️ 必须确保每行 hex 值末尾有逗号（`xxd_fallback` 已修复）
2. Preprocessor 通常不需要替换（`--preprocess_only` 有已知限制，产出的 TFLite 是完整分类器而非特征提取器）
3. 确认 `wakeup_detector.cpp` 中 preprocessor output 与 speech model input 的 scale 一致
4. 将 TFLite 文件同步到 `./models/` 目录备份

### 1.4 训练参数

#### 共同参数（多轮一致）

| 参数 | 值 | 说明 |
|------|-----|------|
| `WANTED_WORDS` | `xiaoqxiaoq` | 目标唤醒词 |
| `SAMPLE_RATE` | 16000 | 16kHz 采样率 |
| `CLIP_DURATION_MS` | 1000 | 每段音频 1 秒 |
| `WINDOW_SIZE_MS` | 30.0 | Mel 频谱窗口 30ms |
| `WINDOW_STRIDE` | 20 | 窗口步长 20ms |
| `FEATURE_BIN_COUNT` | 40 | Mel 频谱 40 bins |
| `PREPROCESS` | `micro` | micro 预处理模式 |
| `MODEL_ARCHITECTURE` | `tiny_conv` | 模型架构 |
| `LEARNING_RATE` | `"0.001,0.0001"` | 两阶段学习率 |
| `SILENT_PERCENTAGE` | 33 | silence 类占比（三分类均分） |
| `UNKNOWN_PERCENTAGE` | 33 | unknown 类占比（三分类均分） |
| `BACKGROUND_FREQUENCY` | 0.8 | 80% 背景混合概率 |
| `BACKGROUND_VOLUME_RANGE` | 0.1 | 背景音量范围 |
| `TIME_SHIFT_MS` | 100.0 | 时间位移增强 ±100ms |
| `VALIDATION_PERCENTAGE` | 10 | 验证集 10% |
| `TESTING_PERCENTAGE` | 10 | 测试集 10% |

#### 每轮差异参数

| 参数 | 第3轮 | 第4轮 | **第5轮（当前）** |
|------|-------|-------|------------------|
| TF 版本 | 2.15.0 | 2.15.0+ | 2.21.0 |
| TRAINING_STEPS | "12000,3000" | "12000,3000" | **"12000,3000"** |
| VERBOSITY | WARN | WARN | WARN |
| EVAL_STEP_INTERVAL | 1000 | 1000 | 1000 |
| Unknown 词源 | 无（仅 silence） | Google 35 类（可选） | **Google 34 类（默认）** |
| 背景噪声源 | 合成白/粉红 | 合成白/粉红 | **Google 6 种 + 合成白/粉红** |
| 训练数据量 | ~1000+ 条 | ~1500+ 条 | **2,864 唤醒词（优化后）+ 104,254 未知词** |
| 输出格式 | .cc 源码 | .cc 源码 (alignas(16)) | .cc 源码 (alignas(16)) |
| 精度测试 | **有** | **有** | **有**（含 dtype 修复） |
| xxd 逗号修复 | 无 | 无 | **已修复**（每行末尾加逗号） |
| Float 精度 | ~70% | - | **97.9% (324/331)** |
| Quant 精度 | - | - | **99.1% (328/331)** |
| 模型大小 | 14,976 bytes | 15,048 bytes | **15,048 bytes** |

### 1.5 重要约束：量化匹配与两阶段 Scale 对齐

**绝对不可以破坏**：预处理器输出 int8 必须与语音模型输入量化完全匹配。

[wakeup_detector.cpp](file://rtos/apps/examples/lvgldemo/wakeup_detector.cpp) 中 `generate_features()` 采用直接 `memcpy` 传输特征 — **preprocessor 输出的 int8 值被原封不动地复制到 speech 模型输入，不存在任何反量化/重新量化步骤**。如果 preprocessor 输出 scale 为 0.10140932，而 speech 模型输入 scale 为 0.10171568，两个模型对相同 int8 特征值的浮点解释不同，相当于 speech 模型收到了"变形"的频谱数据。

**两阶段 Scale 对齐检查清单**：
1. 部署后查看 syslog 中的两行日志：
   ```
   preprocessor output: type=int8 zp=-128 scale=0.101409
   speech: input=49x40 (int8, zp=-128, s=0.101409) ...
   ```
2. **两个 scale 值的偏差必须 < 0.0001**，否则特征传输会产生语义偏差
3. 如果 scale 不匹配：
   - **方案 A**：使用与 preprocessor 匹配的旧模型（备份模型）
   - **方案 B**：重新生成 preprocessor 模型（使用训练脚本的 `--preprocess_only` 模式，但该模式有已知限制）
   - **方案 C**：在训练时固定量化范围，使 speech 模型 input scale 与 preprocessor 一致

**第5轮 scale 不匹配的具体表现**：
- Preprocessor 输出 scale：约 0.10140932（与备份模型匹配）
- Speech 模型输入 scale：0.10171568（偏差 ~0.0003）
- 后果：speech 模型以错误量化参数解释特征，加之过拟合，导致任意语音输出饱和 `out=[-128,-128,127]`

### 1.6 背景噪声策略

| 噪声类型 | 来源 | 使用轮次 | 说明 |
|----------|------|---------|------|
| Google 真实噪声 | doing_the_dishes, dude_miaowing, exercise_bike, pink_noise, running_tap, white_noise | 第1轮、第5轮 | 来自 Google Speech Commands，6 种真实环境/合成噪声，多样性最佳 |
| 合成白噪声 | `np.random.randn() * 0.005` | 全部轮次 | 高斯分布随机噪声，补充高频背景多样性 |
| 合成粉红噪声 | `sin(2πft) * 0.002`, f∈[100,4000] | 第2轮及以后 | 模拟低频背景（风扇、空调等），频率随机从 100-4000Hz 均匀采样 |

**第5轮噪声配置**：Google 6 种噪声 + 100 段合成白噪声（每段 1s）+ 50 段合成粉红噪声（每段 1s），提供丰富细粒度的混合变化。

**第6轮新增中文负样本**：357 条中文唤醒词负样本（"小爱同学"、"小度小度"、"小智小智" 等），作为 unknown 类 `negchinese` 参与训练，有效降低中文唤醒词误唤醒率。

> ⚠️ **重要**：背景噪声**只能混入唤醒词样本**，**不能**放入 unknown 类。否则会因频谱重叠导致模型混淆，unknown→唤醒词误识别率飙升。

---

## 二、唤醒参数配置

### 2.1 配置层级

参数定义在 [Kconfig](file://rtos/apps/examples/lvgldemo/Kconfig) 中，实际生效值在 [defconfig](file://boards/best1700_ep/glass_demo/configs/ap/defconfig) 中。修改步骤：

1. 编辑 `defconfig` 中对应行
2. 同时更新 `Kconfig` 中的 `default` 值保持一致
3. 重新编译

### 2.2 当前生效参数

| 参数 | defconfig | Kconfig default | 当前值 | 作用 |
|------|-----------|-----------------|--------|------|
| `WAKEUP_THRESHOLD` | L224 | L66 | **400** (0.400) | 唤醒词置信度阈值，值越高误唤醒越少 |
| `ENERGY_THRESHOLD` | L227 | L88 | **1200** (RMS) | 音频能量阈值，低于此值跳过推理 |
| `CONSECUTIVE_HITS` | L226 | L77 | **1** | 连续检测到唤醒词的帧数要求 |
| `COOLDOWN_MS` | L225 | L72 | **3000** | 两次唤醒之间的最小间隔 |
| `KEYWORD_INDEX` | L223 | L59 | **2** | 模型输出中唤醒词类别的索引 (xiaoqxiaoq) |
| `SILENCE_INDEX` | L228 | L98 | **0** | silence 类索引 |
| `UNKNOWN_INDEX` | L229 | L106 | **1** | unknown 类索引 |

### 2.3 C++ 代码中的硬编码常量

以下常量在 [wakeup_detector.cpp](file://rtos/apps/examples/lvgldemo/wakeup_detector.cpp) 中定义，不通过 Kconfig 配置：

| 常量 | 值 | 含义 |
|------|---|------|
| `WAKEUP_THRESHOLD_F` (L95) | `CONFIG / 1000.0f` | 阈值浮点值（默认后备=500，即 0.500） |
| `KW_MARGIN_THRESHOLD_F` (L107) | 0.10 | 最高分与次高分的最小间隔 |
| `HIT_TIMEOUT_MS` (L109) | 5000 | 连续命中超时重置时间 |
| `AUDIO_WINDOW_STRIDE` (L108) | 8000 | 滑动窗口步长（半秒） |
| `ENERGY_SETTLE_MS` (L69) | 2000 | 音频启动后的稳定等待时间 |
| `WAKEUP_CHUNK_MS` (L57) | 500 | 每次从录音器读取的音频块 |
| `WAKEUP_STACK_SIZE` (L54) | 8KB | 唤醒线程栈大小 |
| `kPreprocessorArenaSize` | 32768 | 预处理器 TFLite arena |
| `kMicroSpeechArenaSize` | 131072 | 语音模型 TFLite arena |
| `RECORDER_OPEN_RETRIES` (L65) | 10 | 录音器打开重试次数 |

### 2.4 参数调优参考

降低误唤醒（针对桌敲/背景噪声）：
- **首选**：增大 `ENERGY_THRESHOLD` (1200→2000) — ⚠️ 已尝试，会引起无法唤醒问题
- **其次**：增大 `CONSECUTIVE_HITS` (1→3) — ⚠️ 已尝试，会引起无法唤醒问题
- **最后**：增大 `WAKEUP_THRESHOLD` (400→500)

提高唤醒灵敏度：
- 降低 `ENERGY_THRESHOLD`
- 降低 `WAKEUP_THRESHOLD`
- 降低 `CONSECUTIVE_HITS`

> **第5轮模型（当前部署）**：虽然引入了 34 类 unknown 词汇，但由于训练步数过多导致过拟合，实际运行中仍存在任意语音误唤醒问题。参数调整无法解决输出饱和问题，建议使用备份模型（第3轮）或减少训练步数重训。

---

## 三、唤醒完整流程

### 3.1 编译配置

在 `boards/best1700_ep/glass_demo/configs/ap/defconfig` 中启用：

```
CONFIG_EXAMPLES_LVGLDEMO_WAKEUP_ENABLE=y          # 主开关
CONFIG_EXAMPLES_LVGLDEMO_WAKEUP_USE_TFLITE=y      # 启用 TFLite Micro KWS
```

依赖项：
- `MEDIA` — 媒体框架（录音器 + 播放器）
- `TFLITEMICRO` — TFLite Micro 运行时

### 3.2 公共 API

声明于 [wakeup_detector.h](file://rtos/apps/examples/lvgldemo/wakeup_detector.h)：

```c
int  wakeup_detector_init(void);   // 初始化，调用一次
void wakeup_detector_start(void);  // 创建唤醒检测线程
void wakeup_detector_stop(void);   // 停止检测线程
void wakeup_detector_deinit(void); // 反初始化
bool wakeup_detector_is_running(void); // 查询运行状态
```

### 3.3 启动序列

```
wakeup_detector_init()
  └─ 设置 s_initialized = true

wakeup_detector_start()
  └─ pthread_create(wakeup_thread, stack=8KB, detached)
       │
       ├─ tflite_init()
       │   ├─ 加载 g_xiaoq_wakeup_preprocessor_model_data
       │   ├─ 加载 g_xiaoq_wakeup_model_data
       │   ├─ 注册 TFLite ops (18 个 preprocessor + 4 个 speech)
       │   ├─ 分配 arena (32KB preprocessor + 128KB speech)
       │   └─ 验证输入/输出量化匹配
       │
       ├─ media_recorder_open(MEDIA_SOURCE_MIC) — 重试最多10次
       ├─ recorder_prepare_and_start() — 配置 16kHz 16bit mono PCM
       │
       └─ 进入主循环...
```

### 3.4 主循环流程

```
while (s_running)
  │
  ├─ media_recorder_read_data() — 每次读取 500ms 音频 (WAKEUP_CHUNK_MS)
  │
  ├─ 等待音频稳定期 (ENERGY_SETTLE_MS=2000ms)
  │
  ├─ 滑入音频缓冲区 (ring buffer, 1秒窗口, 半秒步进)
  │
  ├─ compute_audio_energy() → 计算 RMS
  │   └─ 若 rms < ENERGY_THRESHOLD → 跳过（静音）
  │
  ├─ generate_features() — 两阶段流水线
  │   ├─ 滑动窗口: 30ms 窗口, 20ms 步长
  │   └─ preprocessor 模型: PCM → int8 特征 [49x40]
  │       └─ 直接 memcpy 到 speech 模型输入
  │
  ├─ classify_features() — 语音模型推理
  │   ├─ 动态读取 input_scale, input_zp, output_scale, output_zp
  │   ├─ 提取 int8 输出分数并反量化为浮点
  │   ├─ 检查 keyword_score >= WAKEUP_THRESHOLD_F
  │   ├─ 检查 max_idx == KEYWORD_INDEX
  │   ├─ 检查 (max_score - second_score) >= KW_MARGIN_THRESHOLD_F (0.10)
  │   └─ 记录非零特征数和特征值范围（调试日志）
  │
  ├─ 连续命中计数器 (consecutive_hits)
  │   ├─ 命中 → consecutive_hits++
  │   ├─ 未命中 → 重置为 0
  │   ├─ 超时 HIT_TIMEOUT_MS → 重置
  │   └─ consecutive_hits >= CONSECUTIVE_HITS → detected = true
  │
  ├─ 音频窗口半秒步进 (AUDIO_WINDOW_STRIDE = 8000 samples)
  │
  └─ 若检测到唤醒...
```

### 3.5 唤醒检测后处理

```cpp
wakeup_detected()
  ├─ syslog: "*** WAKE WORD DETECTED ***"
  ├─ printf: "Resume by xiaoQ!"
  │
  └─ play_prompt_sound()
      ├─ 停止录音器
      ├─ 打开媒体播放器 (MEDIA_STREAM_MUSIC)
      ├─ 设置事件回调
      ├─ 解静音设置最大音量
      ├─ 播放 /emmc/xiaoqxiaoq/nihaoqingshuo.wav
      ├─ 等待播放完成 (sem_timedwait, 10s 超时)
      └─ 关闭播放器

然后重新打开录音器
  ├─ reopen_recorder() — 重新配置 MIC
  ├─ recorder_prepare_and_start()
  └─ 设置 2s 稳定期 → 继续循环
```

### 3.6 停止序列

```
wakeup_detector_stop()
  └─ s_running = false
  └─ 等待录音器关闭 (最多 1s)

wakeup_thread 清理:
  ├─ free(chunk, features, audio_buffer)
  ├─ media_recorder_stop() + close()
  └─ tflite_deinit() — delete 两个 interpreter
```

---

## 四、常见问题排查

### 4.1 任意语音误唤醒（false positive 过高）

**症状**：随便说什么都能被唤醒，包括背景噪声、非关键词人声。

**根因分析**：

有两种不同的根因，需区分：

**类型 A：缺少 unknown 负样本**
模型训练时只有 silence 和 keyword 两类，缺少 unknown（非关键词人声）作为负样本。模型学到的是"有声音 = keyword"，而非"关键词 = keyword"。**特别注意**：中文唤醒词（"小爱同学"、"小度小度"、"小智小智"）是常见的误唤醒源，必须作为负样本参与训练。

**类型 B：模型过拟合导致输出饱和**
模型在训练集/检验集上精度很高（99.1%），但实际运行中输出饱和。典型日志：
```
sc=[0.003,0.000,0.996]  # 任何语音 saturated 到 keyword（索引2）
out=[-128,-128,127]     # int8 输出饱和：kw 达到最大值 127
```
`tiny_conv` 模型很小（~15KB），训练步数过多（15,000+）极易过拟合。模型对训练的 known/unknown 词区分能力很强，但对真实场景中的随意对话泛化差。

**类型 C：preprocessor/speech 输入 scale 不匹配**
两阶段流水线中，preprocessor 输出直接 `memcpy` 到 speech 输入。如果两个模型的 scale 不一致（如 preprocessor 输出 scale=0.101409 但 speech 输入 scale=0.101716），speech 模型收到的是"变形"的特征数据，即使模型本身没有过拟合也会产生错误输出。**这是第5轮模型误唤醒的核心根因之一**。

检查方法：查看启动日志中 `preprocessor output: scale=X` 与 `speech: input=... s=Y`，偏差应 < 0.0001。

**典型日志表现**：
```
sc=[0.000,0.000,0.996]  # 任何语音都被判定为 keyword（索引2）
```

**解决方案**：
- **类型 A**：使用 `DATA_MODE = "google_dataset"` 重训模型，引入 Google Speech Commands v0.02 的 34 类 unknown 词汇作为负样本。**同时引入中文唤醒词负样本**：将 `./project/negative_chinese_wakewords_optimized/` 中的文件作为 `negchinese` unknown 类别（训练脚本已内置支持，通过 `NEGATIVE_DIR` 配置）
- **类型 B**：减少训练步数到 5,000~8,000，调整 `UNKNOWN_PERCENTAGE=50`，或直接使用备份模型
- **类型 C**：确保 speech 模型输入 scale 与 preprocessor 输出 scale 一致。如不一致，优先使用与 preprocessor scale 匹配的模型，或在训练时通过代表数据集固定量化范围

### 4.2 识别率不足

**提升路径（按优先级排序）**：
1. **确保 unknown 词汇充足**：使用 `google_dataset` 模式，34 类 Google Speech Commands 词汇作为负样本
2. **增加唤醒词数据**：收集更多不同人、不同距离、不同环境的 "xiaoQxiaoQ" 录音（当前：2,864 条，已优化）
3. **增加训练步数**：将 `TRAINING_STEPS` 从 `"12000,3000"` 提高到 `"15000,3000"`
4. **尝试更大的模型架构**：`tiny_embedding_conv` 或 `small_conv` 替代 `tiny_conv`（代价：模型体积增大）
5. **量化感知训练**：如果精度测试显示 float 和 quant 差异 >5%

### 4.3 唤醒异常（桌敲/背景噪声误唤醒）

检查清单：
1. `ENERGY_THRESHOLD` 是否偏低 → 尝试 2000 — ⚠️ 已尝试，会引起无法唤醒问题
2. `CONSECUTIVE_HITS` 是否=1 → 尝试 3 — ⚠️ 已尝试，会引起无法唤醒问题
3. 模型是否需重训 → 运行 `train_xiaoq_model_colab.py`（默认 `google_dataset` 模式）
4. 查看 syslog 中的非零特征数和特征值范围是否异常

### 4.4 模型不工作

检查清单：
1. `g_xiaoq_wakeup_model_data.h` 是否最新 → 重新运行训练并部署
2. 量化是否匹配 → 查看启动日志中 preprocessor output 和 speech input 的 scale/zp
3. Arena 是否足够 → 查看启动日志中 `arena_used_bytes`
4. `CONFIG_EXAMPLES_LVGLDEMO_WAKEUP_USE_TFLITE=y` 是否启用
5. TFLite ops 是否全部注册成功（18 preprocessor ops + 4 speech ops）

### 4.5 编译错误

**常见问题**：

1. **Hex 数组缺少逗号**
   - **症状**：C 编译器报语法错误，相邻 hex 值之间缺少分隔符
   - **根因**：`xxd_fallback` 函数生成的每行 hex 值末尾缺少逗号
   - **修复**：已在 [train_xiaoq_model_colab.py:L173](file://train_xiaoq_model_colab.py#L173) 中修复，每行末尾添加逗号（`hex_str,`）
   - **部署后验证**：查看 `.h` 文件每条 hex 行是否以逗号结尾

2. **依赖缺失**：
   - `cmake` → `sudo apt install cmake`
   - `ninja-build` → `sudo apt install ninja-build`
   - `libc++abi1` → `sudo apt install libc++abi1`
   - Git LFS 文件 → `git lfs pull`

3. **模型数据声明缺少 `alignas(16)`**：
   - 部署 `.cc` 到 `.h` 时，需要在数组声明前添加 `alignas(16)` 前缀确保内存对齐

4. **模型与代码中 scale/zp 不匹配**：
   - `wakeup_detector.cpp` 动态读取模型参数，通常无需手动修改
   - 如果模型 scale 发生显著变化，检查 `classify_features()` 中的反量化逻辑

---

## 五、添加新的唤醒词

若要更换唤醒词为其他词语（如 "hello"）：

1. **收集唤醒词数据**：至少 100 条不同人录制的 WAV 文件（16kHz, mono, 16bit），存入 `DATASET_DIR/<唤醒词>/` 目录
2. **选择训练模式**：
   - **推荐**：使用 `train_xiaoq_model_colab.py`，`DATA_MODE = "google_dataset"`（自动下载 34 类 unknown 词汇）
   - **次选**：Colab 环境 GPU 加速
3. **修改 `WANTED_WORDS`**：在训练脚本中将 `WANTED_WORDS` 改为新唤醒词
4. **运行训练**：
   ```bash
   python3 train_xiaoq_model_colab.py
   ```
5. **验证精度**：训练完成后查看 float/quant 模型精度对比，差异 < 5% 为合格
6. **部署模型**：
   - 将生成的 `xiaoq_micro_speech.cc` 内容替换 [xiaoq_wakeup_model_data.h](file://rtos/apps/examples/lvgldemo/xiaoq_wakeup_model_data.h)
   - 在数组声明前添加 `alignas(16)` 前缀
   - 预处理器 `.h` 通常不需要替换（参数一致性）
7. **更新配置**（如需）：
   - [defconfig](file://boards/best1700_ep/glass_demo/configs/ap/defconfig) 中的 `KEYWORD_INDEX`
   - [Kconfig](file://rtos/apps/examples/lvgldemo/Kconfig) 中的默认值
8. **重新编译验证**

> ⚠️ **关键提醒**：更换唤醒词后的第一版模型，务必在真实场景下测试（而非仅依赖训练集/验证集准确率）。务必使用 `google_dataset` 模式包含 unknown 词汇，否则会导致任意语音都能唤醒的问题。

---

## 六、模型训练最佳实践（经验总结）

经过五轮训练迭代的经验教训：

1. **unknown 词多样性至关重要（第5轮核心突破）**
   - 第1轮使用 Google Speech Commands 35 类 unknown 词汇，负样本多样性最好
   - 第2-4轮移除 Google 依赖后只依赖 silence，导致真实场景下任意语音都能唤醒
   - **第5轮**恢复 Google 34 类 unknown 词汇后，Float 精度 97.9%，Quant 精度 99.1%
   - **结论**：unknown 词不能只靠 silence，需要多样化的人声作为负样本。三分类（silence/unknown/keyword）是唤醒模型的最低要求

2. **训练步数控制（第5轮关键教训）**
   - `tiny_conv` 模型极小（~15KB, 8 个 Conv2D 滤波器），极易过拟合
   - 15,000 步对 2,864 个关键词样本来说仍然偏多，可能导致检验集精度高但真实场景泛化差
   - **结论**：建议 `TRAINING_STEPS = "5000,3000"`（总计 8,000 步），而非 "12000,3000"（总计 15,000 步）
   - 如果训练步数已固定，可调整 `UNKNOWN_PERCENTAGE=50` 让模型多看负样本

3. **类别比例平衡**
   - 当前 `SILENT_PERCENTAGE=33, UNKNOWN_PERCENTAGE=33, keyword=34`
   - unknown 来自 34 个不同类别，每类实际占比仅 ~1%，而关键词占 34%
   - 建议尝试 `UNKNOWN_PERCENTAGE=50, SILENT_PERCENTAGE=30`，让 keyword 降至 ~20%
   - 类别不平衡会导致模型偏向关键词，即使引入 unknown 也难纠正

4. **背景噪声混合策略**
   - 背景噪声只能混入唤醒词正样本，绝不能放入 unknown 类
   - **必须使用优化后的噪声文件**：`./project/_background_noise_optimized/`（已裁剪开头 100ms pop 音）
   - ❌ **禁止使用原始噪声文件**：`./project/_background_noise_` 包含 pop 音，会影响训练质量
   - 短段噪声（1s × 100+ 段）比长段噪声（60s × 5 段）提供更细粒度的随机混合
   - TensorFlow speech_commands 框架在 `BACKGROUND_FREQUENCY=0.8` 时会随机截取噪声片段混合

5. **量化精度验证不可跳过**
   - 训练脚本步骤 6 的精度测试同时测试 float 模型和 quant 模型
   - 如果 float 和 quant 差异 >5%，说明量化参数有问题
   - TF v2.21.0 需注意 dtype 兼容（float64→float32 转换）

6. **C 源码生成注意事项**
   - `xxd_fallback` 必须确保每行 hex 值末尾有逗号，否则编译报错
   - 部署到 `.h` 时必须在数组声明前添加 `alignas(16)` 确保内存对齐
   - 使用 `xxd` 命令生成时，需替换数组名和添加对齐前缀

7. **预处理器一致性**
   - preprocessor 模型和 speech 模型必须使用完全相同的音频参数（window_size, stride, feature_bins）
   - `wakeup_detector.cpp` 中 preprocessor 输出直接 `memcpy` 到 speech 输入，无重新量化
   - `classify_features()` 动态读取模型 scale/zp，适配不同训练的模型

8. **数据量建议**
   - 唤醒词：**推荐 3,000~5,000 条**不同说话人、不同距离、不同环境噪声下的录音（当前：2,864 条，已优化处理）
     - 最少 100 条可跑通训练，但泛化能力差
     - 2,864 条是当前实际数量（已从 1,814 条扩充并优化），接近推荐下限，基本可支持训练
     - 建议目标：每个说话人 100~200 条，收集 15~25 个说话人
     - 录音要求：16kHz mono 16bit PCM，每条 1 秒，覆盖近距离（30cm）、中距离（1m）、远距离（3m），安静环境和嘈杂环境各半
     - ⚠️ **必须使用优化后的音频**：`./project/xiaoQxiaoQ_optimized/`（已裁剪 pop 音和静音）
     - ❌ **禁止使用原始音频**：`./project/xiaoQxiaoQ/` 包含 pop 音和多余静音
   - 背景噪声：46 个文件（Google 6 种 + 本地录制 40 个）
     - ⚠️ **必须使用优化后的噪声**：`./project/_background_noise_optimized/`（已裁剪 100ms pop 音）
     - ❌ **禁止使用原始噪声**：`./project/_background_noise_` 包含 pop 音
   - 中文唤醒词负样本：357 个文件（"小爱同学"、"小度小度"、"小智小智" 等）
     - ⚠️ **必须使用优化后的负样本**：`./project/negative_chinese_wakewords_optimized/`（已裁剪 70ms pop 音 + 静音）
     - ❌ **禁止使用原始负样本**：`./project/negative_chinese_wakewords/` 包含 pop 音
     - 训练时作为 unknown 类 `negchinese` 参与训练，有效降低中文唤醒词误唤醒率
   - Unknown 词：推荐 30+ 类 Google Speech Commands 词汇（当前：34 类 + 1 个 negchinese 类，104,254 + 357 条）
   - 数据增强（`TIME_SHIFT_MS=100.0` + `BACKGROUND_FREQUENCY=0.8`）自动扩充训练样本

9. **参数调优优先级**
   - **模型重训（含 unknown 词汇）> 调整阈值参数**
   - 参数调优空间有限：`ENERGY_THRESHOLD` 和 `CONSECUTIVE_HITS` 调高会导致无法唤醒
   - 参数调整无法解决模型过拟合导致的输出饱和问题

10. **TensorFlow 版本兼容**
    - 训练和冻结步骤兼容 TF v2.13-2.21
    - TFLite 转换和精度测试在 TF v2.21.0 需额外的 dtype/format 修复
    - 第5轮已验证 v2.21.0 全流程可运行

11. **两阶段 Scale 对齐（部署前必检项）**
    - preprocessor 模型固定不变（通常只要音频参数不变就不需要重新生成），speech 模型是唯一可变的
    - **每次部署新 speech 模型，必须验证其 input scale 与 preprocessor output scale 一致**
    - 验证方法：启动设备，查看 syslog 中 `preprocessor output: scale=X` 和 `speech: input=... s=Y`，偏差应 < 0.0001
    - 如果 scale 不匹配，特征传输会产生语义偏差，导致即使模型本身精度高也会误唤醒
    - 建议：在训练脚本中记录并固定量化输入范围，使每次训练的 speech 模型 input scale 保持一致

---

## 七、训练数据预处理（音频优化）

### 7.1 问题背景

原始录制的 "xiaoQxiaoQ" 音频文件通常存在以下问题：
1. **开头 pop 音**：录音设备启动时会产生一声 pop 噪声（约 70ms），影响训练质量
2. **前后静音过长**：部分录音文件时长超过 2s，包含大量前后静音，浪费训练时间
3. **时长不一致**：原始文件时长从 0.96s 到 18.6s 不等，不利于模型训练

### 7.2 音频优化脚本

**位置**：[optimize_wav_files.py](file://optimize_wav_files.py)

**功能**：
1. 裁剪每个文件开头的 70ms pop 音
2. 如果录音时长 > 2s，智能裁剪前后静音部分，保留 2s 有声内容
3. 检测并报告异常文件

**使用方法**：
```bash
python3 optimize_wav_files.py
```

**处理逻辑**：
- **步骤 1**：裁剪开头 70ms（1120 samples @ 16kHz）
- **步骤 2**：检测有声部分边界（RMS 阈值 = 500，搜索窗口 1.5s，步长 10ms）
- **步骤 3**：提取有声部分，如果仍 > 2s 则截取中间 2s（唤醒词通常在中间）
- **步骤 4**：保存优化后的文件到 `./project/xiaoQxiaoQ_optimized/` 目录

### 7.3 优化效果

| 指标 | 原始文件 | 优化后文件 | 改善 |
|------|---------|-----------|------|
| **文件总数** | 2,864 个 | 2,864 个 | ✓ 全部处理 |
| **平均时长** | 2.492s | 1.838s | ↓ 减少 26% |
| **最短时长** | 0.960s | 0.890s | ✓ 裁剪了 70ms pop 音 |
| **最长时长** | 18.600s | 2.000s | ✓ 超长文件已裁剪 |
| **时长>2s** | 2,062 个 (72%) | 0 个 (0%) | ✓ 全部处理 |
| **异常文件** | - | **0 个** | ✓ 无异常 |

### 7.4 训练数据准备流程

**推荐流程**：
1. **收集原始录音**：放入 `./project/xiaoQxiaoQ/` 目录
2. **运行优化脚本**：`python3 optimize_wav_files.py`
3. **验证优化结果**：检查 `./project/xiaoQxiaoQ_optimized/` 目录
4. **优化背景噪声**：`python3 optimize_noise_files.py`
5. **验证噪声优化**：检查 `./project/_background_noise_optimized/` 目录
6. **优化中文负样本**：`python3 optimize_negative_wakewords.py`
7. **验证负样本优化**：检查 `./project/negative_chinese_wakewords_optimized/` 目录
8. **修改训练脚本**：将 `AUDIO_DIR` 和噪声路径指向优化后的目录
   ```python
   AUDIO_DIR = "./project/xiaoQxiaoQ_optimized"  # 必须使用优化后的音频
   # 背景噪声路径也需要指向优化后的目录
   BACKGROUND_NOISE_DIR = "./project/_background_noise_optimized"  # 必须使用优化后的噪声
   NEGATIVE_DIR = "./project/negative_chinese_wakewords_optimized"  # 中文唤醒词负样本
   ```
9. **运行训练**：`python3 train_xiaoq_model_colab.py`

**注意事项**：
- ⚠️ **强制要求**：训练时必须使用优化后的文件，禁止使用原始文件
- 优化后的唤醒词音频：`./project/xiaoQxiaoQ_optimized/`（已去除 pop 音和多余静音）
- 优化后的背景噪声：`./project/_background_noise_optimized/`（已去除 100ms pop 音）
- 优化后的中文负样本：`./project/negative_chinese_wakewords_optimized/`（已去除 70ms pop 音 + 静音）
- 原始文件仅供存档和对比使用，不得用于训练
- 所有唤醒词文件时长 ≤ 2s，符合训练脚本的 `CLIP_DURATION_MS=1000` 要求（会自动截断/填充）
- 如果原始文件本身 < 1s，优化后可能更短，训练脚本会自动处理
- 优化脚本会生成 `abnormal_files.txt` 记录任何异常文件（当前无异常）

### 7.5 背景噪声优化

**问题背景**：
- 本地录制的背景噪声文件开头同样存在 pop 音（约 100ms）
- Google Speech Commands 的 6 种标准噪声（doing_the_dishes, white_noise 等）也包含 pop 音
- pop 音会混入唤醒词样本，影响模型训练质量

**优化脚本**：

**位置**：[optimize_noise_files.py](file://optimize_noise_files.py)

**功能**：
1. 裁剪每个噪声文件开头的 100ms pop 音
2. 保持噪声文件其他部分不变（噪声文件不需要裁剪静音）

**使用方法**：
```bash
python3 optimize_noise_files.py
```

**处理逻辑**：
- 裁剪开头 100ms（1600 samples @ 16kHz）
- 保持剩余部分完整保留
- 保存优化后的文件到 `./project/_background_noise_optimized/` 目录

**优化效果**：

| 指标 | 原始文件 | 优化后文件 | 改善 |
|------|---------|-----------|------|
| **文件总数** | 46 个 | 46 个 | ✓ 全部处理 |
| **Google 噪声** | 6 个（60-95s） | 6 个（59.9-95.08s） | ✓ 裁剪 100ms |
| **本地噪声** | 40 个（1.6-5.7s） | 40 个（1.58-5.6s） | ✓ 裁剪 100ms |
| **异常文件** | - | **0 个** | ✓ 无异常 |

**包含的噪声类型**：
- **Google 标准噪声**（6 个）：doing_the_dishes, dude_miaowing, exercise_bike, pink_noise, running_tap, white_noise
- **本地录制噪声**（40 个）：各种环境噪声（文件名格式：20260525_*.wav）

**训练使用要求**：
- ⚠️ **必须使用**：`./project/_background_noise_optimized/` 目录中的优化后噪声
- ❌ **禁止使用**：`./project/_background_noise_` 目录中的原始噪声（包含 pop 音）
- 训练脚本中的背景噪声路径必须指向优化后的目录

### 7.6 中文唤醒词负样本优化

**问题背景**：
- 实际运行中发现中文唤醒词（"小爱同学"、"小度小度"、"小智小智" 等）会引起误唤醒
- 训练时 unknown 类只有英文词汇（Google Speech Commands 34 类），缺少中文负样本
- 录制的负样本文件开头同样存在 pop 音（约 70ms），部分文件时长超过 2s

**优化脚本**：

**位置**：[optimize_negative_wakewords.py](file://optimize_negative_wakewords.py)

**功能**：
1. 裁剪每个负样本文件开头的 70ms pop 音（与唤醒词优化策略一致）
2. 如果时长 > 2s，智能裁剪前后静音部分，保留 2s 有声内容
3. 检测并报告异常文件（太短无法裁剪等）

**使用方法**：
```bash
python3 optimize_negative_wakewords.py
```

**处理逻辑**：
- **步骤 1**：裁剪开头 70ms（1120 samples @ 16kHz）
- **步骤 2**：检测有声部分边界（RMS 阈值 = 500，搜索窗口 1.5s，步长 10ms）
- **步骤 3**：提取有声部分，如果仍 > 2s 则截取中间 2s
- **步骤 4**：保存优化后的文件到 `./project/negative_chinese_wakewords_optimized/` 目录

**优化效果**：

| 指标 | 原始文件 | 优化后文件 | 改善 |
|------|---------|-----------|------|
| **文件总数** | 359 个 | 357 个 | 2 个异常文件太短 |
| **平均时长** | 1.784s | 1.684s | ↓ 减少 5.6% |
| **最短时长** | 1.080s* | 1.010s | ✓ 裁剪了 70ms pop 音 |
| **最长时长** | 3.420s | 2.000s | ✓ 超长文件已裁剪 |
| **时长>2s** | 43 个 (12%) | 0 个 (0%) | ✓ 全部处理 |
| **异常文件** | - | **2 个** | 0.060s 和 0.000s，无法裁剪 70ms |

*不含 2 个异常短文件

**训练集成**：
- 训练脚本通过 `NEGATIVE_DIR` 配置变量指定优化后的负样本目录
- 负样本文件被复制为 `negchinese` unknown 类别参与训练
- 与 Google Speech Commands 34 类英文 unknown 词汇共同构成 35 类负样本

**训练使用要求**：
- ⚠️ **必须使用**：`./project/negative_chinese_wakewords_optimized/` 目录中的优化后负样本
- ❌ **禁止使用**：`./project/negative_chinese_wakewords/` 目录中的原始负样本（包含 pop 音）
- 训练脚本中的 `NEGATIVE_DIR` 必须指向优化后的目录