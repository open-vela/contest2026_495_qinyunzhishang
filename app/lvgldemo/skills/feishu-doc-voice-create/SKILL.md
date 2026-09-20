---
name: "feishu-doc-voice-create"
description: "Voice-based Feishu cloud document and message skill for OpenVela. Supports create, query (read content), search (keyword), and today's message summary via voice. Invoke when user asks about Feishu doc creation/query/search/message summary via voice, intent detection, title/keyword extraction, tenant_access_token, mic suppression during TTS playback, fast path state management, TTS segmentation, or debugging related issues."
---

# 语音飞书云文档技能

## 功能概述

通过语音指令在OpenVela嵌入式AI设备上操作飞书云文档，支持四种意图：
- **创建文档**：语音创建新飞书文档，提取标题
- **查询文档内容**：语音读取指定文档内容，TTS播报
- **搜索关键词**：语音搜索关键词，跨文档检索，返回匹配片段
- **今日消息**：语音获取今日飞书群聊消息，LLM归纳摘要后TTS播报

核心特点：
- **使用`tenant_access_token`（应用身份）**：自动刷新，有效期2小时但提前5分钟自动续期，无需用户手动授权
- **两层意图匹配**：第一层语音侧快速拦截抑制云端音频，第二层agent侧精确匹配和参数提取
- **快速路径（Fast Path）**：不走LLM，直接本地工具调用，响应速度<2秒
- **mic上传抑制**：快速路径激活期间停止mic上传，TTS播放完成+500ms回声消散后才恢复，避免TTS声音被云端识别导致自问自答
- **TTS分段播放**：长文本按标点切分多段合成播放，段间通过`tts_speaking`标志保持mic抑制
- **文档内容净化**：读取飞书文档后过滤非法字符（控制字符、无效UTF-8、BOM、零宽字符、方向控制符、对象替换字符U+FFFC、ASCII空格等），避免TTS播报异常字符和空格断句
- **编译时配置**：folder_token、App ID、App Secret通过宏定义在编译时配置，无需运行时设置

**不支持**：多轮对话确认，必须一句话包含动作+对象+标题/关键词信息。

## 完整架构流程

### 创建文档流程

```
语音识别文本(DashScope Omni)
       ↓
[第一层: voice_assistant.c]
  - 匹配动作词(创建/新建/...) + 对象词(飞书文档/文档/...)？
  - 是 → omni_pb_close() 抑制云端音频，避免AI抢答
       → s_feishu_fast_path=true, s_feishu_fast_path_start_ts=now
       → ai_page_show_user_message() UI显示用户消息
       → message_bus_push_inbound() 推送到本地agent总线
       → return (绕过正常云端对话流程)
  - 否 → 检查查询/搜索意图
       ↓
[第二层: agent_loop.c - handle_nl_fast_path()]
  - 再次校验动作词+对象词
  - 否定词过滤？（待实现）
  - 按优先级提取标题
  - 构造tool_input JSON
  - 调用 tool_registry_execute("feishu_doc_create", ...)
       ↓
[工具层: tool_feishu_doc.c]
  - 解析title和folder_token（无则用默认宏）
  - 调用 feishu_api_post() → 自动获取/刷新tenant_access_token
  - POST /open-apis/docx/v1/documents
  - 解析响应，构造返回JSON（包含document_id、title、url、status）
       ↓
[回到agent_loop.c]
  - 解析tool_result
  - 生成TTS回复话术
  - 返回reply给TTS播放
       ↓
[TTS播放: voice_channel.c]
  - s_fast_path_tts_started=true (TTS开始播放)
  - mic上传持续抑制 (!s_feishu_fast_path为false)
  - 播放完成+500ms回声消散后，检查持有时间>=12秒
  - s_feishu_fast_path=false → mic上传恢复
```

### 查询文档内容流程

```
语音识别文本(DashScope Omni)
       ↓
[第一层: voice_assistant.c]
  - 匹配查询动作词(读一下/查看/打开/...) + 对象词(飞书文档/文档/...)
  - 是 → 抑制云端音频 → s_feishu_fast_path=true → 推送到本地agent总线 → return
       ↓
[第二层: agent_loop.c - handle_nl_fast_path()]
  - 提取文档标题（引号优先，fallback取对象词后内容）
  - 调用 feishu_doc_list 列出所有文档
  - 按标题匹配找到 doc_id（支持忽略标点的模糊匹配）
  - 调用 feishu_doc_read 读取文档内容
  - 读取完整内容（最大8192字节，约2700中文字）
  - sanitize_doc_content() 过滤非法字符和空格（tool_feishu_doc.c定义，agent_loop.c复用）
  - 传递净化后完整内容给TTS（不截断）
       ↓
[TTS分段播放: voice_channel.c]
  - 文本>300字节或包含\n → 分段播放
  - 切分标点：\n . ! ? ; 空格 + 中文。！？；…
  - 英文冒号(:)不作为拆分符
  - 英文句号(.)前后都是数字时不拆分（如3.14）
  - tts_speaking=1 (标记多段播放循环开始)
  - 每段独立TTS合成+缓存(256KB)
  - 段间动态等待缓冲区<16KB
  - mic上传持续抑制 (tts_speaking保持true)
  - 所有段播放完成 → tts_speaking=0
  - +500ms回声消散 → s_tts_echo_active=false
  - 持有时间>=12秒 → s_feishu_fast_path=false → mic恢复
       ↓
[TTS回复]
  - 成功："飞书文档内容：{完整内容}"
  - 未找到："未找到标题包含XXX的飞书文档"
  - 失败："读取飞书文档失败，请稍后再试"
```

### 搜索关键词流程

```
语音识别文本(DashScope Omni)
       ↓
[第一层: voice_assistant.c]
  - 匹配搜索动作词(搜索/查找/检索/...) + 对象词(关键词/内容/文档/...)
  - 是 → 抑制云端音频 → s_feishu_fast_path=true → 推送到本地agent总线 → return
       ↓
[第二层: agent_loop.c - handle_nl_fast_path()]
  - 提取关键词（引号优先，fallback取"关键词"后或搜索词后内容）
  - 调用 feishu_doc_list 列出所有文档
  - 遍历每个文档：
    - 调用 feishu_doc_read 读取内容
    - 用 strcasestr 搜索关键词
    - 提取匹配位置上下文（前50字+关键词+后100字）
    - 最多收集3个匹配文档
       ↓
[TTS分段播放: voice_channel.c]
  - 同查询流程，mic上传持续抑制
  - 播放完成+500ms+12秒后释放mic
       ↓
[TTS回复]
  - 有匹配："在文档《XXX》中找到关键词：{上下文}。在文档《YYY》中：{上下文}"
  - 无匹配："未找到包含XXX的飞书文档"
```

### 今日消息流程

```
语音识别文本(DashScope Omni)
       ↓
[第一层: voice_assistant.c]
  - 匹配关键词(今日消息/今日总结/...)？
  - 是 → 抑制云端音频 → s_feishu_fast_path=true → 推送到本地agent总线 → return
       ↓
[第二层: agent_loop.c - handle_nl_fast_path()]
  - 匹配今日消息关键词
  - 调用 tool_registry_execute("feishu_message_today", ...) 获取今日群聊消息
  - LLM归纳摘要（sys_prompt指定按群分组总结，网络失败自动重试最多3次）
  - sanitize_doc_content() 过滤返回文本中的非法字符和空格
  - 返回摘要文本给TTS播放
       ↓
[TTS播放: voice_channel.c]
  - 同查询流程，mic上传持续抑制
  - 播放完成+500ms+12秒后释放mic
       ↓
[TTS回复]
  - LLM成功：摘要文本（按群分组）
  - LLM重试仍失败：返回原始消息文本（已净化）
  - 空/错误："今日暂无消息"
```

## 触发关键词（两层意图匹配）

### 第一层（语音侧，voice_assistant.c）

#### 创建意图
**动作词（doc_actions）**：`创建`、`新建`、`帮我写`、`帮我建`、`写一个`、`建一个`、`写入`、`写到`、`写进`、`保存到`、`存到`、`create`
**对象词（doc_objects）**：`飞书文档`、`飞书`、`文档`、`document`、`doc`

匹配规则：必须同时包含**至少一个动作词** AND **至少一个对象词**。

#### 查询意图
**动作词（query_actions）**：`读一下`、`查看`、`看一下`、`打开`、`读取`、`查询`、`看看`、`read`
**对象词**：复用 doc_objects（`飞书文档`、`飞书`、`文档`、`document`、`doc`）

匹配规则：创建意图不匹配时，检查查询动作词 + 对象词。

#### 搜索意图
**动作词（search_actions）**：`搜索`、`查找`、`检索`、`找一下`、`搜一下`、`search`、`find`
**对象词（search_objects）**：`关键词`、`内容`、`飞书文档`、`文档`、`飞书`

匹配规则：创建和查询意图都不匹配时，检查搜索动作词 + 搜索对象词。

#### 今日消息意图
**关键词**：`今日消息`、`今天的消息`、`今日总结`、`消息总结`、`今日飞书消息`、`今天飞书消息`、`飞书今日消息`、`todays message`

匹配规则：创建、查询、搜索意图都不匹配时，检查今日消息关键词。不需要动作词+对象词组合，直接关键词匹配。

#### 优先级
创建 > 查询 > 搜索 > 今日消息（互斥匹配，先匹配到的不再检查后续）

匹配成功后执行操作：
1. 打日志：`[FEISHU_DOC] Layer1 matched action="xxx" object="xxx", suppressing cloud audio`
2. `#ifdef CONFIG_MEDIA omni_pb_close(); #endif` - 立即关闭DashScope云端音频播放，防止AI抢答
3. **重置云端响应状态**：`s_response_active=false`、`s_last_audio_time=0`、`s_echo_suppress=false`
   - 关键：`response.created` 可能先于 Layer1 匹配到达并设置 `s_response_active=true`，若不重置，后续快速路径重置条件 `!s_response_active` 永不满足，导致mic被90秒硬超时卡死
4. `s_feishu_fast_path = true` - 设置快速路径标志，阻止后续云端omni响应事件
5. `s_feishu_fast_path_start_ts = get_time_ms()` - 记录启动时间戳，用于最小持有时间检查
6. `ai_page_show_user_message(data)` - UI上显示用户说的话
7. 构造`agent_msg_t`消息，channel=`AGENT_CHAN_VOICE`，chat_id=`"voice"`，content=识别文本
8. `message_bus_push_inbound(&msg)` - 推送到本地agent消息总线
9. 打日志：`[FEISHU_DOC] Pushed to local agent bus, ret=xxx`
10. `return` - **直接返回，绕过正常云端对话流程**

**mic上传抑制机制**：`s_feishu_fast_path=true`期间，mic音频上传循环中的条件`!s_feishu_fast_path`为false，停止向云端发送音频数据。这防止了TTS声音被云端VAD识别为新语音输入导致自问自答。

### 第二层（agent侧，agent_loop.c:610-1420）

handle_nl_fast_path() 先扫描 **table-driven 通用意图**（s_intents[]），再处理飞书专用意图：

**Table-driven 通用意图**（简单关键词→工具映射，无参数提取）：

| 关键词 | 工具名 |
|--------|--------|
| 几点/what time | get_current_time |
| 电量/battery | get_battery |
| 心率/heartrate | get_heartrate |
| 步数/steps | get_steps |
| 暂停播放/pause | music_pause |
| 停止播放/stop | music_stop |
| 继续播放/resume | music_resume |

**特殊意图**（需要参数提取）：
- **天气**：关键词`天气怎么样`/`天气如何`，提取城市名
- **音乐播放**：关键词`播放`/`放一首`，提取歌曲名→music_search→music_play
- **技能列表**：关键词`技能列表`/`有什么技能`，调用skill_loader_build_summary()

**飞书创建意图**：
**动作词（kw_doc_action）**：`创建`、`新建`、`帮我写`、`帮我建`、`写一个`、`建一个`、`写入`、`写到`、`写进`、`保存到`、`存到`、`create`
**对象词（kw_doc_object）**：`飞书文档`、`文档`、`doc`、`document`、`飞书`

匹配规则：同样必须同时包含动作词和对象词。
- 目前**没有否定词过滤**，疑问句/抱怨句（如"什么情况？怎么自动创建文档？"）只要同时包含动作+对象也会被误触发
- 匹配成功后进入标题提取流程

## 快速路径状态管理与mic抑制机制

### 核心变量

| 变量 | 类型 | 文件 | 作用 |
|------|------|------|------|
| `s_feishu_fast_path` | `volatile bool` | voice_assistant.c:256 | 快速路径激活标志，为true时mic不上传、跳过云端omni响应 |
| `s_feishu_fast_path_start_ts` | `volatile uint64_t` | voice_assistant.c:258 | 快速路径启动时间戳，用于最小持有时间检查 |
| `s_fast_path_tts_started` | `volatile bool` | voice_assistant.c:261 | 标记TTS已开始播放，防止TTS未开始就提前重置 |
| `s_tts_echo_active` | `volatile bool` | voice_assistant.c:251 | TTS回声抑制标志，TTS播放期间+500ms |
| `s_tts_echo_end_ts` | `volatile uint64_t` | voice_assistant.c:252 | TTS停止时间戳，用于计算回声消散延时 |
| `s_voice.tts_speaking` | `volatile int` | voice_channel.c:228 | 多段TTS播放循环标志，段间也保持true |
| `s_voice.tts_pb` | `audio_playback_t*` | voice_channel.c:223 | 活动TTS播放句柄，drain线程可能仍在播放 |

### 关键超时常量

| 常量 | 值 | 作用 |
|------|------|------|
| `FEISHU_FAST_PATH_MIN_HOLD_MS` | 12000ms | 快速路径最小持有时间，防止过早释放mic |
| `FEISHU_FAST_PATH_TIMEOUT_MS` | 90000ms | 快速路径硬超时，兜底强制重置 |
| `SILENCE_TIMEOUT_MS` | 30000ms | 双向静音超时，所有播放结束后30秒退出对话 |
| `ECHO_SETTLE_MS` | 500ms | TTS回声消散延时 |
| `TTS_MAX_SEGMENT_BYTES` | 300 | TTS单段最大字节数，超过则分段 |
| `TTS_CACHE_MAX_PCM_LEN` | 256KB | TTS缓存最大PCM长度（约8秒音频） |

### mic上传抑制条件

mic音频上传循环中的判断条件（voice_assistant.c:1369）：
```c
if (!s_echo_suppress && !s_img_sending && !s_tts_echo_active
    && !s_feishu_fast_path) {
    // 上传mic音频到云端
}
```
任一条件为true即抑制mic上传：
- `s_echo_suppress`: 云端omni音频播放期间
- `s_tts_echo_active`: 本地TTS播放期间+500ms回声消散
- `s_feishu_fast_path`: 飞书快速路径激活期间

### s_feishu_fast_path 重置时机

| 时机 | 条件 | 说明 |
|------|------|------|
| TTS播放完成后释放 | `s_fast_path_tts_started && !voice_channel_is_speaking() && !s_tts_echo_active && !s_response_active && 持有时间>=12秒` | 正常释放路径 |
| 新语音输入时重置 | transcript事件且非空且非TTS播放期间 | 用户说话时重置 |
| 硬超时强制重置 | 持有时间>=90秒 | 兜底保护 |
| agent侧Layer2不匹配 | `voice_assistant_reset_feishu_fast_path()` | 第二层未匹配时由agent调用 |
| silence timeout | 双向静音30秒后 | 退出对话时清理 |

### TTS播放状态判断（voice_channel_is_speaking）

```c
int voice_channel_is_speaking(void)
{
    // 1. 多段播放循环进行中 → true（段间也保持）
    if (s_voice.tts_speaking) return 1;
    
    // 2. tts_pb存在且缓冲区数据>1KB → true（drain线程还在播放）
    if (pb && audio_playback_pending_bytes(pb) > 1024) return 1;
    
    // 3. voice_pipeline活跃 → true
    return voice_pipeline_is_active();
}
```

**关键设计**：`tts_speaking`标志在多段播放循环开始时设为1，所有段播放完成后才清零。这确保段间缓冲区暂时低于1KB时也保持mic抑制，避免TTS声音被云端识别。

### 云端事件抑制

快速路径激活期间，以下云端事件被抑制：

| 事件 | 抑制条件 | 处理 |
|------|----------|------|
| `response.created` | `s_feishu_fast_path` 或 `voice_channel_is_speaking()` | 跳过，避免omni_pb_open冲突 |
| `response.audio_transcript.delta` | `s_feishu_fast_path` | 丢弃云端转录文本，避免显示云端LLM回复 |
| `response.audio_transcript.done` | `s_feishu_fast_path` | 丢弃云端完整转录，跳过ai_page显示和飞书转发 |
| `response.audio.delta` | `s_feishu_fast_path` | 丢弃云端音频数据 |
| `transcript` | `voice_channel_is_speaking()` | 忽略TTS回声识别的transcript |
| `response.done` | `s_feishu_fast_path` | 仅更新dialogue activity时间，不重置标志 |

## 文档内容净化机制

`tool_feishu_doc_read_execute` 读取文档内容后，调用 `sanitize_doc_content()` 就地过滤非法字符，避免TTS播报异常字符导致语音混乱或崩溃。

### 过滤规则

**保留的合法字符**：
- ASCII 可打印字符（0x21-0x7E）：英文、英文标点、数字（不含空格）
- 空白符：`\n` `\r` `\t`
- 所有合法 UTF-8 多字节字符：汉字、汉字标点、其他语言（日韩阿拉伯等）及其标点

**清除的非法字符**：

| 类别 | Code Point | 说明 | 典型来源 |
|------|-----------|------|--------|
| **ASCII空格** | **0x20** | **空格字符** | **TTS断句异常** |
| 控制字符 | 0x00-0x1F（除`\n\r\t`）、0x7F | 不可打印控制字符 | 文档复制污染 |
| 无效UTF-8 | 非法起始字节、后续字节不匹配`10xxxxxx` | 损坏的字节序列 | 编码转换错误 |
| BOM/零宽不换行空格 | U+FEFF | 字节顺序标记 | Windows编辑器 |
| 零宽空格 | U+200B | 不可见空格 | 网页复制 |
| 零宽不连接符 | U+200C | ZWNJ | 阿拉伯文等 |
| 零宽连接符 | U+200D | ZWJ | Emoji组合 |
| 左/右至左/右标记 | U+200E、U+200F | 方向标记 | RTL语言 |
| 方向控制符 | U+202A-202E | 嵌入/覆盖/弹出 | RTL语言排版 |
| 字词连接符等 | U+2060-2064 | 不可见连接/运算符 | 数学排版 |
| 行内注释符 | U+FFF9-FFFB | 锚点/分隔/终止 | 古籍数字化 |
| **对象替换字符** | **U+FFFC** | **图片/附件/@提及占位** | **飞书富文本元素** |
| 替换字符 | U+FFFD | 无效Unicode占位 | 编码转换失败 |

### 实现位置

- 函数：`sanitize_doc_content(char *s)` — [tool_feishu_doc.c:342-441](file:///home/xayf/lichuangchuang_openwakeword/openvela_si8658ca/rtos/packages/ai_agent/src/tools/tool_feishu_doc.c#L342-L441)
- 调用点1：`tool_feishu_doc_read_execute` 返回前 — [tool_feishu_doc.c:509](file:///home/xayf/lichuangchuang_openwakeword/openvela_si8658ca/rtos/packages/ai_agent/src/tools/tool_feishu_doc.c#L509)
- 调用点2：`handle_nl_fast_path()` 消息归纳返回前 — [agent_loop.c](file:///home/xayf/lichuangchuang_openwakeword/openvela_si8658ca/rtos/packages/ai_agent/src/core/agent_loop.c)（通过extern声明复用）
- 就地修改字符串，不额外分配内存
- UTF-8 安全：按字节序列解析，不会误切断多字节字符

### 日志输出

过滤掉字符时输出：
```
[tool_feishu_doc] Doc content sanitized: 1234 -> 1230 bytes (removed 4)
```

### 完整状态流转

```
用户语音输入
    ↓
Layer1匹配 → s_feishu_fast_path=true, s_feishu_fast_path_start_ts=now
    ↓
Layer2处理 → 调用飞书API → 获取文档内容
    ↓
TTS开始播放 → s_fast_path_tts_started=true, s_tts_echo_active=true
    ↓  (mic持续抑制)
TTS分段播放中 → tts_speaking=1, voice_channel_is_speaking()=true
    ↓  (段间也保持mic抑制)
所有段播放完成 → tts_speaking=0, voice_channel_is_speaking()=false
    ↓  (等待500ms回声消散)
s_tts_echo_active=false
    ↓  (检查最小持有时间>=12秒)
s_feishu_fast_path=false → mic上传恢复
    ↓  (双向静音30秒)
Silence timeout → 退出对话
```

## 标题提取逻辑（按优先级从高到低）

| 优先级 | 提取方式 | 匹配规则 | 支持句式举例 | 已验证 |
|-------|---------|---------|-------------|--------|
| 1 | 英文双引号 | 查找第一个`"`和最后一个`"`之间的内容 | `创建一个名为"会议纪要"的文档` | - |
| 2 | 中文双引号 | 查找第一个`"`（UTF-8: e2 80 9c）和最后一个`"`（UTF-8: e2 80 9d）之间的内容 | `帮我创建一个名为"项目计划"的飞书文档` | ✓ 验证成功 |
| 3 | 直角引号 | 查找第一个`「`和最后一个`」`之间的内容 | `创建一个「月度计划」的文档` | - |
| 3.5 | 书名号 | 查找第一个`《`和最后一个`》`之间的内容 | `查看文档《会议纪要》` | - |
| 4 | Fallback | 找到第一个对象词，跳过后面的标点/空格，取后面的文字直到句末 | `创建文档 会议纪要`、`新建飞书文档：周报` | ⚠️ 中文句式标题通常在对象词前面，会导致untitled |

### 标题清洗规则（所有提取方式通用）
提取到标题后进行清洗：
1. 移除开头和结尾的空白字符、标点（中英文逗号、句号、冒号、分号、问号、感叹号、引号等）
2. JSON转义：处理双引号、反斜杠等特殊字符，保证构造的JSON安全
3. 如果清洗后标题为空，使用默认标题`"untitled"`

**⚠️ 已知缺陷**：fallback逻辑是英文思维，只从对象词**后面**找标题，但中文最常见句式是`[动作]...[标题]的[对象]`，标题在对象词**前面**，导致大部分不带引号的语音指令提取为`untitled`。

## 支持的工具接口

### 飞书文档工具（tool_feishu_doc.c）

| 工具名 | Token类型 | API接口 | 功能 | 语音快速路径支持 |
|-------|----------|---------|------|----------------|
| `feishu_doc_create` | tenant_access_token | `POST /open-apis/docx/v1/documents` | 创建文档 | ✓ 支持 |
| `feishu_doc_write` | user_access_token | `POST /open-apis/docx/v1/documents/{doc_id}/blocks/{doc_id}/children` | 追加内容到文档 | ✗ 未接入快速路径 |
| `feishu_doc_read` | tenant_access_token | `GET /open-apis/docx/v1/documents/{doc_id}/raw_content` | 读取文档纯文本 | ✓ 支持（查询意图） |
| `feishu_doc_list` | tenant_access_token | `GET /open-apis/drive/v1/files` | 列出文件夹下文档 | ✓ 支持（查询/搜索意图） |

### 飞书消息工具（tool_feishu_chat.c）

| 工具名 | Token类型 | 功能 | 语音快速路径支持 |
|-------|----------|------|----------------|
| `feishu_message_today` | user_access_token | 获取今日飞书群聊消息 | ✓ 支持（今日消息意图） |
| `feishu_chat_members` | user_access_token | 获取群聊成员列表 | ✗ 未接入快速路径 |
| `feishu_send_mention` | user_access_token | 在群聊中发送@提及消息 | ✗ 未接入快速路径 |

### feishu_doc_create 输入输出格式

**输入JSON**：
```json
{
  "title": "文档标题",
  "folder_token": "fldcnxxxxxx"  // 可选，不传则用编译时默认值
}
```

**输出JSON（成功）**：
```json
{
  "document_id": "ZE6bd3XPNoQ9WyxJny3cskp5nDf",
  "title": "项目计划",
  "url": "https://feishu.cn/docx/ZE6bd3XPNoQ9WyxJny3cskp5nDf",
  "status": "created"
}
```

**输出JSON（失败）**：
```json
{
  "error": "Error: HTTP 400 from Feishu: {\"code\":1770039,\"msg\":\"folder not found\"}"
}
```

## tenant_access_token自动刷新机制（feishu_http.c）

- 缓存位置：静态变量`s_access_token`，互斥锁`s_token_lock`保护
- 过期时间：飞书返回`expire`字段（通常7200秒=2小时），本地提前**5分钟（300秒）**判定为过期
- 刷新时机：每次API调用前检查是否过期，过期则自动调用`POST /open-apis/auth/v3/tenant_access_token/internal`刷新
- 刷新参数：`{"app_id": "xxx", "app_secret": "xxx"}`，使用编译时配置的宏
- 日志：刷新成功打`[feishu_http] tenant_access_token refreshed`

## 文档归属说明
- 使用`tenant_access_token`创建的文档，**归属于应用本身**，不是任何个人用户
- 文档创建在配置的`folder_token`对应的文件夹下
- 需要提前在飞书开放平台将应用添加为该文件夹的协作者，并授予**编辑权限**，否则会报1770039错误

## 涉及文件清单

| 文件路径 | 作用 | 关键行 |
|---------|------|--------|
| `rtos/apps/examples/lvgldemo/voice_assistant.c` | 第一层语音意图检测、音频抑制、消息总线推送、mic上传抑制、快速路径状态管理、Layer1匹配后重置响应状态 | 256-261, 826-855, 835-837, 970-1060, 1404-1536 |
| `rtos/packages/ai_agent/src/voice/voice_channel.c` | TTS分段播放、`voice_channel_is_speaking()`、`tts_speaking`标志管理 | 228, 1077-1098, 1591, 1742-1922 |
| `rtos/packages/ai_agent/src/core/agent_loop.c` | 第二层意图检测（table-driven通用+飞书专用）、标题提取、快速路径处理、TTS回复生成、模糊搜索、消息归纳文本净化(复用sanitize_doc_content) | 596-605(s_intents), 610-1420(handle_nl_fast_path) |
| `rtos/packages/ai_agent/src/llm/llm_proxy.c` | LLM API调用封装，网络错误+HTTP 429自动重试（最多3次，指数退避） | 640-745 |
| `rtos/packages/ai_agent/src/tools/tool_feishu_doc.c` | 飞书文档4个工具实现、API调用、响应解析、文档内容净化`sanitize_doc_content()` | 342-439, 509 |
| `rtos/packages/ai_agent/src/tools/tool_feishu_chat.c` | 飞书消息3个工具实现：`feishu_message_today`、`feishu_chat_members`、`feishu_send_mention` | - |
| `rtos/packages/ai_agent/src/channels/feishu_http.c` | HTTP封装，tenant_access_token自动获取/刷新逻辑 | 226-284 |
| `rtos/packages/ai_agent/src/channels/feishu_bot.h` | feishu_api_post / feishu_api_post_as_user 函数声明 | 31-42 |
| `rtos/packages/ai_agent/include/voice/tts_cache.h` | TTS缓存配置，`TTS_CACHE_MAX_PCM_LEN=256KB` | 30 |
| `rtos/packages/ai_agent/include/voice/audio_playback.h` | `audio_playback_pending_bytes()`函数声明 | - |
| `rtos/packages/ai_agent/include/agent_config.h` | 编译时配置宏：App ID、App Secret、folder_token；时区函数声明（实现在timezone.c） | 78-98, 150-173 |

## 编译配置说明

### agent_config.h 必要宏定义
```c
// 飞书应用凭证（从飞书开放平台获取）
#define AGENT_SECRET_FEISHU_APP_ID          "cli_xxxxxxxxxxxxxx"
#define AGENT_SECRET_FEISHU_APP_SECRET      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"

// 文档创建默认目标文件夹token（从飞书文件夹URL中获取）
// URL格式：https://feishu.cn/drive/folder/fldcnxxxxxxxxxxxxxx → 最后一段即为folder_token
#define AGENT_SECRET_FEISHU_DOC_FOLDER_TOKEN "fldcnxxxxxxxxxxxxxx"

// 时区配置（POSIX TZ格式）
#define AGENT_TIMEZONE "CST-8"               // 默认时区
#define AGENT_TZ_CONFIG_FILE "/data/misc/tz_config"  // 运行时配置文件
// 时区函数声明（实现在 src/infra/timezone.c）：
// const char *agent_get_timezone(void);     // 从文件读取时区
// int agent_tz_offset_sec(void);            // 解析UTC偏移秒数
// struct tm agent_localtime(void);          // 获取本地时间
```

### 编译开关
需要在nuttx配置中开启：
```
CONFIG_AI_AGENT_FEISHU=y
CONFIG_MEDIA=y  // 音频抑制功能依赖
```

### 飞书开放平台权限要求
在飞书开放平台 → 应用 → 权限管理中开通：
1. `docx:document` - 查看、编辑、管理文档
2. `drive:drive` - 查看、评论、编辑和管理云空间中所有文件

**重要**：权限开通后需要**发布版本**并**企业管理员审批**才能生效。

### 文件夹权限配置
文档创建前必须：
1. 在飞书云文档中创建目标文件夹
2. 进入文件夹设置 → 协作者 → 添加应用为协作者
3. 授予**可编辑**权限
4. 否则会返回错误码`1770039: folder not found`

## 日志统一规范

所有飞书文档和消息相关相关日志**必须**以`[FEISHU_DOC]`或`[FEISHU_MSG]`开头，方便grep过滤定位问题：

```bash
# 实时串口查看飞书文档相关日志
grep "\[FEISHU_DOC\]" /dev/ttyUSB0

# 实时串口查看飞书消息相关日志
grep "\[FEISHU_MSG\]" /dev/ttyUSB0

# 从保存的日志文件过滤
grep "\[FEISHU_DOC\]\|\[FEISHU_MSG\]" build_logs/*.log

# Windows SSCOM中：勾选“显示日志”→搜索“FEISHU”
```

### 日志关键点说明

#### 1. 第一层匹配（voice_assistant）
| 日志内容 | 说明 |
|---------|------|
| `Layer1 matched intent=create action="创建" object="文档"` | 第一层创建意图匹配 |
| `Layer1 matched intent=query action="查询" object="文档"` | 第一层查询意图匹配 |
| `Layer1 matched intent=search action="搜索" object="关键词"` | 第一层搜索意图匹配 |
| `Layer1 matched intent=msg_today action="今日消息" object="?"` | 第一层今日消息意图匹配 |
| `Pushed to local agent bus, ret=0` | 消息推送到本地总线成功 |
| `Failed to strdup text (OOM)` | 内存不足，复制文本失败 |

#### 2. 第二层匹配与处理（agent_loop）
| 日志内容 | 说明 |
|---------|------|
| `Layer2 intent check: has_action=1, has_object=1` | 创建意图第二层匹配 |
| `Layer2 query intent, text="..."` | 查询意图第二层匹配 |
| `Layer2 search intent, text="..."` | 搜索意图第二层匹配 |
| `Extraction method: chinese_quotes` | 标题/关键词提取方式 |
| `Query title: "xxx"` | 查询意图提取的文档标题 |
| `Search keyword: "xxx"` | 搜索意图提取的关键词 |
| `List result: {...}` | feishu_doc_list 返回的文档列表 |
| `Read result: {...}` | feishu_doc_read 返回的文档内容 |
| `Reply to TTS: "xxx" (fast=N llm=0)` | 最终TTS回复内容 |
| `NL fast path: feishu_doc_query (fast=N llm=0)` | 查询快速路径计数 |
| `NL fast path: feishu_doc_search (fast=N llm=0)` | 搜索快速路径计数 |
| `[FEISHU_MSG] Layer2 today message intent` | 第二层今日消息意图匹配 |
| `[FEISHU_MSG] LLM summary: ...` | LLM归纳摘要内容 |
| `NL fast path: feishu_message_today (fast=N llm=N)` | 今日消息快速路径计数 |

#### 3. API调用（tool_feishu_doc / tool_feishu_chat）
| 日志内容 | 说明 |
|---------|------|
| `Creating document: title=xxx, folder=xxx` | 准备创建文档，显示标题和目标folder |
| `HTTP response status=200, body={...}` | HTTP响应状态码和响应体（最多打印500字节） |
| `HTTP request failed, status=400` | HTTP请求失败，status为HTTP状态码 |
| `Feishu business error: code=1770039, msg=folder not found` | 飞书业务错误码和错误信息 |
| `Create SUCCESS: document_id=xxx, title=xxx` | 创建成功，返回文档ID和标题 |
| `Doc content sanitized: 1234 -> 1230 bytes (removed 4)` | 文档内容净化，显示过滤前后字节数 |
| `[FEISHU_MSG] Tool result: ...` | feishu_message_today 工具返回的原始消息 |
| `message_today: N chats scanned, M messages found` | 今日消息扫描统计 |

#### 4. Token相关（feishu_http）
| 日志内容 | 说明 |
|---------|------|
| `tenant_access_token refreshed` | tenant_access_token刷新成功 |
| `tenant_access_token HTTP XXX` | Token刷新HTTP请求失败 |
| `tenant_access_token code=XXX` | Token刷新业务错误 |

#### 5. 快速路径与mic抑制（voice_assistant）
| 日志内容 | 说明 |
|---------|------|
| `Feishu fast path active, skipping response.created` | 快速路径激活，跳过云端响应 |
| `TTS playing, skipping response.created (echo)` | TTS播放期间跳过云端响应 |
| `Transcript ignored (TTS playing, echo suppression)` | TTS回声被忽略 |
| `Empty transcript ignored` | 空transcript被忽略 |
| `Feishu fast path: omni done, query still in progress` | 云端omni完成但飞书查询还在进行 |
| `Feishu fast path: TTS done, releasing mic (XXX ms, held YYY ms)` | TTS完成释放mic |
| `Feishu fast path hard timeout (XXX ms), force-resetting` | 硬超时强制重置 |
| `Feishu fast path reset by agent (Layer2 no match)` | Layer2不匹配，agent重置 |
| `Silence timeout (XXX ms), exiting dialogue` | 静音超时退出对话 |

#### 6. TTS分段播放（voice_channel）
| 日志内容 | 说明 |
|---------|------|
| `TTS segment 1 (36 bytes): "..."` | 第1段TTS文本 |
| `TTS segment 2 (15 bytes): "..."` | 第2段TTS文本 |
| `TTS segmentation done: N segments` | 分段完成，共N段 |
| `TTS network done: XXXms (cache)` | TTS网络合成完成（cache命中） |
| `TTS cache HIT: "..."` | TTS缓存命中 |
| `TTS cache MISS, calling voice_tts_speak_stream` | TTS缓存未命中 |
| `speak done: total XXXms (play wait YYYms) [CACHE]` | TTS播放完成 |
| `tts_cache stored: "..." (XXXXX bytes)` | TTS缓存存储 |

## TTS回复话术

| 场景 | 话术 | 备注 |
|-----|------|------|
| 创建成功 | `已为你创建飞书文档《{title}》` | 优先使用工具返回的title |
| 创建失败 | `飞书文档创建失败，请稍后再试` | |
| 查询成功 | `飞书文档内容：{完整内容}` | 读取最大8192字节，TTS按标点分段播放 |
| 查询未找到文档 | `未找到标题包含{title}的飞书文档` | |
| 查询失败 | `读取飞书文档失败，请稍后再试` | |
| 搜索有匹配 | `在文档《{name}》中找到关键词：{上下文}` | 最多3个文档，拼接上下文 |
| 搜索无匹配 | `未找到包含{keyword}的飞书文档` | |
| 搜索未提供关键词 | `请告诉我要搜索的关键词` | |
| 今日消息(LLM成功) | LLM生成的摘要文本（按群分组） | fast=N llm=N |
| 今日消息(LLM重试仍失败) | 原始消息文本（已净化） | 网络失败自动重试3次，指数退避(2s/4s/8s) |
| 今日消息(空/错误) | `今日暂无消息` | |

## 常见问题排查

### 问题1：不带引号时所有文档标题都是untitled
- **现象**：说"帮我创建一个会议纪要的文档"，创建出来标题是untitled；加上引号说"帮我创建一个名为"会议纪要"的文档"就正常
- **根因**：fallback标题提取逻辑只从对象词（文档/飞书文档）**后面**找标题，但中文最常见句式是`[修饰词][标题]的[对象]`，标题在对象词**前面**，提取结果为空，fallback到"untitled"
- **临时解决方案**：语音输入时**用中文引号把标题括起来**，例如：`帮我创建一个名为"会议纪要"的飞书文档`
- **根治方案**：修复fallback逻辑，同时支持两种中文句式：
  - Pattern A（标题在前，最常见）：匹配`[修饰词]标题的文档`，从"的"字前面提取标题，跳过"帮我创建一个"等前缀
  - Pattern B（标题在后）：匹配`文档，名称是标题`/`文档，标题叫xxx`，从对象词后面找"名称是/标题是/名字叫"等引导词后的内容
  - 添加否定词过滤，避免疑问句误触发

### 问题2：Feishu返回1770039 "folder not found"
- **现象**：HTTP 200，但响应中`code=1770039, msg="folder not found"`
- **可能原因**：
  1. `AGENT_SECRET_FEISHU_DOC_FOLDER_TOKEN`配置错误（复制错了、多了空格、少了字符）
  2. 文件夹已被删除
  3. **应用没有该文件夹权限**（最常见）：没有把应用添加为文件夹协作者
  4. 权限开通后没有发布应用版本，或者管理员未审批
- **排查步骤**：
  1. 从飞书网页端URL确认folder_token正确
  2. 进入文件夹 → 设置 → 协作者，确认应用已添加，权限为"可编辑"
  3. 检查飞书开放平台，权限已开通，应用版本已发布并审批通过
  4. 重新编译烧录固件

### 问题3：中文日志在串口工具中显示乱码
- **现象**：日志中中文都是类似`闊充箰`、`閿欒`的乱码
- **根因**：系统输出是**UTF-8编码**，但Windows串口工具（如SSCOM5.1）默认使用GBK/GB2312编码
- **解决方案**：
  - SSCOM：菜单→设置→编码→选择"UTF-8"
  - 推荐工具：MobaXterm、SecureCRT、Xshell等默认使用UTF-8的工具，不会乱码
  - 注意：飞书API侧接收的是正确的UTF-8编码，乱码只是显示问题，不影响功能

### 问题4：抱怨句/疑问句被误触发创建
- **现象**：说"什么情况？怎么自动创建文档？"也会触发创建untitled文档
- **根因**：只有简单关键词匹配，没有否定词/疑问词过滤，句子中只要同时出现动作词（创建）和对象词（文档）就会触发
- **临时解决方案**：避免在调试时说包含"创建文档"的疑问句
- **根治方案**：添加否定关键词检测，命中以下词汇时不触发创建：
  - 疑问词：`为什么`、`怎么`、`啥情况`、`什么情况`、`咋回事`、`为啥`
  - 调试相关：`自动创建`、`自动建`、`为什么会`
  - 标点：`?`、`？`

### 问题5：第一层匹配后没有进入第二层，没有创建文档
- **现象**：看到了`Layer1 matched`日志，但没有后续`Layer2 intent check`日志
- **排查步骤**：
  1. 检查`Pushed to local agent bus, ret=`日志，ret是否为0？非0表示消息总线推送失败
  2. 确认agent进程是否正常运行
  3. 检查是否开启了`CONFIG_AI_AGENT_FEISHU`编译选项

### 问题6：返回99991663 "invalid access token"或类似token错误
- **现象**：HTTP 400/401，飞书返回token无效错误
- **排查步骤**：
  1. 检查App ID和App Secret配置是否正确，没有多余空格
  2. 检查系统时间是否正确（token过期判断依赖系统时间，时间不对会导致误判过期）
  3. 重启设备，强制刷新token

### 问题7：TTS播报时崩溃（signal.c:445 assertion failed）
- **现象**：播放飞书文档（如沁园春雪）时，`Assertion failed at file: signal.c:445`，`player stop ret:-88`
- **根因**：TTS播放期间mic上传了TTS声音，云端识别后返回transcript事件，重置了`s_feishu_fast_path`，导致后续`response.created`未被抑制，`omni_pb_open()`与TTS播放器冲突崩溃
- **排查步骤**：
  1. 检查日志是否有 `input_audio_buffer.speech_started` 在TTS播放期间出现
  2. 检查 `voice_channel_is_speaking()` 是否在段间返回false（`tts_speaking`标志未设置）
  3. 检查 `s_fast_path_tts_started` 是否在TTS未开始时就为true导致提前重置
  4. 确认 `s_voice.tts_speaking` 在分段播放循环开始时设置为1，结束时清零

### 问题8：TTS播报后mic长时间无响应
- **现象**：TTS播报完飞书文档后，mic一直无响应，直到90秒超时后才恢复
- **根因**：`response.created` 先于 Layer1 匹配到达并设置了 `s_response_active=true`，Layer1 匹配后只调用 `omni_pb_close()` 但**未重置 `s_response_active=false`**。由于omni_pb被关闭，云端响应中断，`response.done` 事件不会到达，`s_response_active` 一直为 true，快速路径重置条件 `!s_response_active` 永远不满足
- **修复**：Layer1 匹配后 `omni_pb_close()` 之后同步重置 `s_response_active=false`、`s_last_audio_time=0`、`s_echo_suppress=false` — [voice_assistant.c:835-837](file:///home/xayf/lichuangchuang_openwakeword/openvela_si8658ca/rtos/apps/examples/lvgldemo/voice_assistant.c#L835-L837)
- **排查步骤**：
  1. 检查日志是否有 `Feishu fast path: TTS done, releasing mic` 日志
  2. 确认重置条件：`s_fast_path_tts_started && !voice_channel_is_speaking() && !s_tts_echo_active && !s_response_active && 持有时间>=12秒`
  3. 检查 `response.created` 是否先于 Layer1 匹配到达（时序竞态）
  4. 确认 Layer1 匹配后是否重置了 `s_response_active=false`

### 问题9：TTS播报过程中提示"没有问题我先退下了"
- **现象**：TTS正在播报飞书文档（如沁园春雪）时，突然提示"没有问题我先退下了"
- **根因**：TTS播放期间没有更新`s_last_dialogue_activity`时间戳，导致播放完成后`current_time - s_last_dialogue_activity > 30秒`，立即触发silence timeout
- **排查步骤**：
  1. 检查 `voice_channel_is_speaking()` 返回true时是否更新了 `s_last_dialogue_activity`
  2. 确认 silence timeout 条件包含 `!voice_channel_is_speaking() && !s_response_active && !s_tts_echo_active`
  3. 检查飞书文档内容是否较长（如沁园春雪13段播放约35秒），需要TTS播放期间持续更新时间戳

### 问题10：TTS播报只播一小段就没有了
- **现象**：查询飞书文档时，TTS只播报了一小段就停止
- **根因**：TTS缓存大小限制（原64KB只能存约2秒音频），长文档的TTS音频被截断
- **排查步骤**：
  1. 检查 `TTS_CACHE_MAX_PCM_LEN` 是否为256KB（约8秒音频）
  2. 检查多行文本是否触发分段逻辑（包含`\n`的文本会强制分段）
  3. 查看日志 `TTS cache HIT` 后的播放时长，如果只有2-3秒说明缓存不完整

### 问题11：TTS播报中出现`￼`等异常字符
- **现象**：TTS播报飞书文档时，出现"Object Replacement Character"（U+FFFC，显示为`￼`）或其他不可见字符，导致语音异常
- **根因**：飞书文档 `raw_content` 接口会将图片、附件、@提及、链接卡片等富文本元素替换为 `U+FFFC` 对象替换字符作为占位符，TTS引擎无法正确合成这些字符
- **修复**：`sanitize_doc_content()` 过滤函数中已包含 U+FFFC 及相关不可见字符的过滤 — [tool_feishu_doc.c:399-432](file:///home/xayf/lichuangchuang_openwakeword/openvela_si8658ca/rtos/packages/ai_agent/src/tools/tool_feishu_doc.c#L399-L432)
- **排查步骤**：
  1. 检查日志是否有 `Doc content sanitized: xxx -> yyy bytes (removed zzz)`
  2. 如果仍有异常字符，检查 `sanitize_doc_content()` 过滤列表是否需要补充新的 Unicode 占位字符
  3. 参考 Unicode 特殊字符列表：U+FFF9-FFFB（行内注释）、U+FFFC（对象替换）、U+FFFD（替换字符）

### 问题12：今日消息没有LLM归纳，直接播报原文
- **现象**：说“今日消息”后，直接播报原始飞书消息文本，没有LLM摘要
- **根因**：`llm_chat()` 调用 DashScope API 时网络层失败（`ssl_read ret=0x4c`，连接被服务端重置），重试耗尽后 fallback 返回原始消息
- **日志特征**：`HTTP request failed: status=-4`、`LLM summary failed, returning raw messages`
- **重试机制**：`llm_chat()` 对网络错误和HTTP 429均自动重试，最多3次，指数退避(2s/4s/8s) — [llm_proxy.c:696-708](file:///home/xayf/lichuangchuang_openwakeword/openvela_si8658ca/rtos/packages/ai_agent/src/llm/llm_proxy.c#L696-L708)
- **排查步骤**：
  1. 检查日志是否有 `Retry N/3 after Xs` 表示正在重试
  2. 检查 `HTTP request failed (err=X), will retry` 确认网络错误类型
  3. 如果重试后成功，会看到 `LLM summary: ...` 日志
  4. 如果重试耗尽，会看到 `LLM summary failed, returning raw messages`

## 正确的语音测试用例（当前版本）

### 创建文档 — 带引号（推荐）
- ✅ "帮我创建一个名为"会议纪要"的飞书文档" → 标题：会议纪要
- ✅ "新建一个"项目计划"的文档" → 标题：项目计划
- ✅ "创建飞书文档"周报"" → 标题：周报
- ✅ "写一个"月度总结"的飞书文档" → 标题：月度总结

### 创建文档 — 标题在后
- ✅ "创建文档 会议纪要" → 标题：会议纪要
- ✅ "新建飞书文档：项目计划" → 标题：项目计划

### 查询文档内容
- ✅ "读一下飞书文档"会议纪要"" → 读取标题含"会议纪要"的文档内容
- ✅ "查看文档 会议纪要" → 读取文档内容
- ✅ "看一下飞书文档"项目计划"的内容" → 读取文档内容
- ✅ "打开文档 周报" → 读取文档内容
- ✅ "查看飞书文档" → 读取第一个文档内容（无标题时取第一个）

### 搜索关键词
- ✅ "搜索关键词 项目进度" → 跨文档搜索"项目进度"
- ✅ "在飞书文档中查找"预算"" → 搜索关键词"预算"
- ✅ "搜一下文档中包含 会议室 的内容" → 搜索关键词"会议室"
- ✅ "查找关键词 API" → 搜索关键词"API"

### 今日消息
- ✅ "今日消息" → 获取并总结今日飞书群聊消息
- ✅ "今日飞书消息" → 同上
- ✅ "今日总结" → 同上
- ✅ "今天的消息" → 同上

### 会创建untitled（待修复）
- ❌ "帮我创建一个名为会议纪要的文档" → 标题在"文档"前面 → untitled
- ❌ "新建一个月度计划的飞书文档" → 标题在"飞书文档"前面 → untitled

## 未来优化点

1. 修复fallback标题提取逻辑，支持中文常见的"标题在对象前"句式
2. 添加否定关键词过滤，避免疑问句/抱怨句误触发
3. 支持feishu_doc_write快速路径，创建文档后自动追加语音转写内容
4. 支持动态folder_token提取（"创建到XX文件夹"）
5. 创建成功后TTS播报文档链接（可选择通过蓝牙推送到手机打开）
6. 搜索结果支持TTS逐文档播报，而非拼接后截断
7. 查询文档时支持按修改时间排序，读取最近修改的文档
