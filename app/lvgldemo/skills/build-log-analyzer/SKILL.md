---
name: "build-log-analyzer"
description: "分析 openvela 编译/运行日志并定位代码问题。当用户遇到编译失败、运行崩溃、报错需要排查时调用。先分析日志列出过程，再根据日志定位代码，不编译不使用 rm/git。"
---

# openvela 日志分析与代码定位

本 skill 用于在**不编译、不执行 rm/git 等文件操作**的前提下，通过分析日志来定位 openvela 项目（best1700 平台，NuttX 内核）中的编译错误与运行问题。

---

## 1. 核心约束（必须严格遵守）

1. **禁止编译**：不得执行 `build.sh`、`make`、`cmake`、`ninja`、`scons` 等任何构建命令。本 skill 只做"读日志 → 分析 → 定位代码"，不重新构建。
2. **禁止文件操作命令**：不得使用 `rm`、`git`、`mv`、`cp`（批量覆盖）、`chmod` 等可能修改/删除文件或改变仓库状态的命令。只允许**只读**操作（Read / Grep / Glob / LS / SearchCodebase）。
3. **分析顺序不可颠倒**：必须**先分析日志、列出分析过程**，**再**根据日志线索去定位/分析代码。不得跳过日志直接看代码。
4. **只读保护**：始终以只读方式工作，保护仓库源码与构建产物。如需修改代码，仅给出建议，待用户确认。

---

## 2. 日志文件位置

openvela_si8658ca 项目的日志存放在以下两个位置（按优先级查找）：

| 位置 | 路径 | 说明 |
|------|------|------|
| 上一级目录的 txt | `../agent.txt`（即 `openvela_si8658ca` 父目录中的 `agent.txt`） | 综合日志，通常包含运行/启动信息，也可能包含构建输出 |
| build_logs 文件夹 | `build_logs/`（项目根或父目录下） | 编译日志目录，存放各次构建的详细日志 |

> **绝对路径参考**（当前环境）：
> - `agent.txt` → `/home/u100679224/workspace/agent/agent.txt`
> - 项目根 → `/home/u100679224/workspace/agent/openvela_si8658ca`
> - `build_logs/` → 在项目根或父目录下查找

**查找日志的方法**：
- 用 Glob 查找 `../agent.txt` 与 `build_logs/` 下的 `*.txt` / `*.log` 文件。
- 若 `build_logs/` 不存在，说明尚未产生编译日志，应提示用户先在外部完成编译后再分析。

---

## 3. 分析工作流

### 第一步：定位并读取日志
1. 用 Glob / LS 确认 `../agent.txt` 与 `build_logs/` 下的日志文件是否存在。
2. 用 Read 读取日志内容。大文件优先读取**末尾**（错误堆栈通常在末尾），再按需向前回溯上下文。
3. 日志很大时，先用 Grep 过滤关键字，快速定位错误：
   `error|Error|ERROR|fatal|undefined reference|No such file|crash|assert|panic|Failed|warning:`

### 第二步：分析日志，列出分析过程（必须输出）
在回复中**明确列出**分析过程，结构如下：
- **日志来源**：来自哪个文件、哪一段（行号范围）。
- **关键错误信息**：摘录报错原文（含文件名、行号、错误类型）。
- **错误分类**：
  - 编译错误（链接缺失 / 头文件缺失 / 语法错误 / 依赖缺失）
  - 运行错误（崩溃 / 断言 / 看门狗 / 初始化失败）
- **初步推断**：根据错误信息推断可能的原因与涉及的模块/文件。

### 第三步：根据日志定位代码
1. 从日志中提取的**文件路径、函数名、行号**出发。
2. 用 SearchCodebase / Grep / Read 在 `openvela_si8658ca` 仓库中定位对应代码。
3. 结合上下文分析根因，给出代码层面的解释。

### 第四步：输出结论
- **根因总结**：一句话说明问题本质。
- **涉及代码位置**：用 Code Reference 链接（`[文件名](file:///绝对路径#L行号)`）。
- **修复建议**：说明改哪个文件、怎么改（仅建议，不擅自修改）。

---

## 4. 常见日志特征与排查方向

### 4.1 编译错误（多见于 build_logs/）
| 日志特征 | 排查方向 |
|----------|----------|
| `undefined reference to 'xxx'` | 链接缺失，检查 Makefile/Kconfig 是否启用对应模块、源文件是否参与编译 |
| `fatal error: xxx.h: No such file or directory` | 头文件路径缺失，检查 include 路径与模块依赖 |
| `error: 'xxx' undeclared` | 变量/函数未声明，检查定义与宏开关（Kconfig CONFIG_） |
| `No rule to make target 'xxx'` | Makefile 目标缺失，检查 Make.defs / Makefile |
| `multiple definition of 'xxx'` | 重复定义，检查源文件是否被重复包含 |

### 4.2 运行错误（多见于 agent.txt）
| 日志特征 | 排查方向 |
|----------|----------|
| `Crash happened from bes ap wdt` | 看门狗超时，排查死循环/阻塞/中断未喂狗 |
| `assert` / `panic` / `Backtrace` | 查看调用栈，定位断言点与触发条件 |
| `ERROR: Failed to ...` | 资源初始化失败，按模块（SPI/I2C/FLASH/codec）排查 |
| `[ap]` / `[cp]` 标签 | 区分 AP（应用核）与 CP（通讯核）日志，定位责任核 |

### 4.3 agent.txt 结构注意
- 文件开头可能有非 UTF-8 乱码字符，读取时忽略即可。
- `agent.txt` 可能包含**多次启动**的日志，以 `CHIP=best1700` / `KERNEL=NUTTX` 段头分隔，注意区分是哪一次启动。
- 段头含 `BUILD_DATE`、`REV_INFO`，可用来对应代码版本。

---

## 5. 输出规范

回复必须包含以下部分（按顺序）：

1. **日志定位**：说明找到了哪些日志文件、路径。
2. **分析过程**：按第 3 步"第二步"的结构列出。
3. **代码定位**：引用具体代码位置（Code Reference 链接）。
4. **结论与建议**：根因 + 修复建议。

> 整个过程**不得编译**、**不得使用 rm/git 等文件操作命令**，仅做只读分析与定位。
