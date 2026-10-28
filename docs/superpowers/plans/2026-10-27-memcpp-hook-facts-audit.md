# 审计：memcpp hook 协议契约事实钉版（43a4d75f）

- 日期：2026-10-27；审计者：独立审计子代理（未参与本轮撰写）
- 审计对象：`43a4d75f` 唯一新文件 `docs/superpowers/research/2026-10-27-memcpp-hook-contract-facts.md`（115 行）
- 审计方法：git show 核改动面；下载官方 CHANGELOG raw（9,578 行，顶部 2.1.311，与文档自称一致）与 code.claude.com 两页文档 raw markdown（hooks reference 4,240 行 / hooks-guide 1,080 行）+ schemastore settings schema（142 个顶层属性）本地逐字检索。以下"现行原文"均指 2026-10-27 抓取的 raw 页（.md 后缀直出，行号会漂移，以引文文字为准）。

## 六维度逐条结论

### 1. 正确性（4 条关键引用抽查，均有外网复核）—— 通过，2 条带 finding

- ① additionalContext 必须嵌 hookSpecificOutput，顶层放被静默忽略：**坐实**。hooks-guide 现行原文逐字含 "if you place it at the top level of the JSON, Claude Code silently ignores it"；changelog v1.0.59 段内逐字含 "Hooks: UserPromptSubmit now supports additionalContext in advanced JSON output"。引文与出处页均准确（钉版文档 :12、:17）。
- ② 10k 上限 + 超限落盘 + 2,000 字预览（钉版 v2.1.89）：**坐实**。hooks reference 现行原文 :1050/:1053 与钉版文档 :20-:21 逐字一致（含 "this cap has no setting or environment variable to raise it"）；changelog v2.1.89 段内逐字含 "Changed hook output over 10,000 characters to be saved to disk with a file path + a 2,000-character preview..."。
- ③ matcher 只按工具名、文件过滤走 if（v2.1.85+）：**主体坐实**。reference 表格 "tool name | `Bash`, `Edit\|Write`, `mcp__.*`" 逐字一致；changelog v2.1.85 的 if 字段引文逐字一致（原文尾部多 "to filter when they run, reducing process spawning overhead"，属截断引用不失真）。但 :37 "Bash 解析不出具体命令时照常触发（v2.1.245 起文档注记）"中，**现行两页文档均检索不到 "2.1.245" 注记，changelog 2.1.245 段亦无 hook 条目**；行为本身有现行原文支撑（"When Claude Code can't determine which commands the Bash input runs, it runs your hook regardless of the pattern"）。→ Finding F1。
- ④ async 输出下一轮投递（全文最重的翻案结论）：**坐实，且证据强度口径正确**。hooks reference 现行原文逐字含 "After the background process exits, Claude Code delivers the `additionalContext` and `systemMessage` fields from the hook's JSON response to Claude on the next conversation turn"（钉版 :104 引文逐字一致）；这是**现行官方 reference 页**而非 changelog 推断，Limitations 节还补充 "If the session is idle, the response waits until the next user interaction"（只会让翻案结论更硬）。文档 :104 明确把它标为"现官方文档"，与 changelog 推断区分——结论强度匹配正确（维度 6 一并通过）。
- 附带抽查（均通过）：system reminder 机制引文、多 hook 全投递引文、30 秒默认与超时丢弃引文、async 不受 timeout、exit 2/非阻塞通知、v2.1.110/163/280/292/295/309/310 changelog 锚点全部在对应版本段逐字命中；PreToolUse decision/reason 废弃引文逐字命中；schema 142 个顶层属性、minimumVersion/requiredMinimumVersion/requiredMaximumVersion、allowedHttpHookUrls/httpHookAllowedEnvVars 精确命中；配置热加载（settings 页 file watcher）命中。
- 发现 F2（:38 "Windows 反斜杠会被归一化"）：现行文档口径相反——"always absolute, with the platform's native separators, so backslashes on Windows"，且 :1761-1762 明确警告"按 /src/ 这类正斜杠比较永远匹配不到反斜杠路径，工具调用照常放行"。页内无任何"归一化"表述。→ Finding F2。
- 发现 F3（:39 "@-referenced files don't trigger PreToolUse Read hooks" 标为"官方警告原文"）：事实成立（现行 reference 原文："files you reference with `@` in your prompt are added without any tool call... so no PreToolUse hook fires for them, including hooks matching `Read`"），但**所引"原文"在现行页面逐字检索不到，属转写当原文**。→ Finding F3。

### 2. 回归面 —— 通过（已核）

git show 43a4d75f --stat：仅 1 个新文件、115 行插入、无删除；可行性报告、handoff、索引零触碰。父提交 db09ac35/1b7d1f91 均为 docs 提交，无代码面影响。

### 3. 版本口径风险 —— 通过，1 条观察

- 30 秒特例：changelog 全文检索 "30 second" 仅 3 处且均与 hook 无关，"无引入记录、以当前文档为准"的披露（:32、:63、:101）**如实且三处口径一致**。10k 数字"本身更早、未钉引入版"的表述同属如实分级；风险表 :62 稳定/:63 易变的强度分级与证据强度对应。
- 观察 O1：:63 "v2.1.280/295/302 连续动周边"中 302 的 hook 改动实为 prompt/agent hook 并发上限（每事件最多 12 个），与超时默认值无关；"周边"成立但易被读成"UserPromptSubmit 超时是活跃改动区"，建议加限定词。

### 4. 静默失败面（第三节 9 条断言清单）—— 通过，1 条观察

9 条断言逐条有明确判据且可落地（1/2/3/4/5/8 直接对协议行为，6/7/9 对配置与版本闸门）；断言 4 明确"memcpp hook 二进制任何内部异常路径都不产生退出码 2"，与 :51 "内部错误必须吞掉并 exit 0"配套，memcpp 侧可据此写检查。观察 O2：断言 4 的负向断言"任何内部异常路径"未给枚举方法（错误注入点清单/exit 包装静态检查），P1 落地时若只测 4 个正例会成假绿，建议补故障注入口径。

### 5. 文档口径 —— 通过，1 条观察

零黑话：:6 术语节已解 hook/matcher/非阻塞错误/exit 2；"失败开放"在 :30 随文释义，"MCP recall"在 :78 括注释义。对照表述抽查 :61/:136/:137/:229/:230/:327/:445 七处 file:行**全部与可行性报告现行文本吻合，引文无失真**；:104 对 :136 后半句"已过时"的翻案定性准确（:230 同句、:327"注入必须同步仍成立"的判读也准确）。观察 O3：:80/:82 的 P0/P1 未随文定义（依赖报告 §6 上下文），独立阅读会断。

### 6. 结论强度匹配 —— 通过（已核）

翻案结论（async 已变化）锚在现行官方文档原文，非 changelog 推断，文档显式区分两种效力（:104 vs :32）；9,578 行/2.1.311 两个数字与官方 changelog 实测一致。

## Findings 汇总（3 条需修 + 3 条观察）

| # | 位置（钉版文档 file:行） | 问题 | 影响 | 建议 |
|---|---|---|---|---|
| F1（中） | research/2026-10-27-memcpp-hook-contract-facts.md:37 | "Bash 解析不出具体命令时照常触发（v2.1.245 起文档注记）"——现行 hooks 两页均无 2.1.245 注记，changelog 2.1.245 无 hook 条目，版本锚点不可验证（行为本身有现行原文支撑） | 一篇以"钉版"为卖点的文档混入幻影锚点；P1 契约测试按此写 version_gate 会钉错版本 | 改为"现行文档载明该行为、无版本注记"，或核实真实引入版本后再钉（changelog 2.1.163 修过 if-Bash 匹配 bug，疑相关） |
| F2（中） | research/2026-10-27-memcpp-hook-contract-facts.md:38 | "Windows 反斜杠会被归一化"与现行文档相反：路径为平台原生分隔符（Windows=反斜杠），且官方明确警告正斜杠比较会失配放行 | memcpp 按文件过滤在 Windows 解析 file_path 时若按"已归一化"假设写比较逻辑，会静默失配——正落在本文档自己强调的静默失败面上，维护期难排查 | 改为"file_path 一律绝对路径；Windows 下为反斜杠原生分隔符，正斜杠比较失配（官方有专节警告）"，并把该警告列入契约测试断言 5 的判据 |
| F3（低） | research/2026-10-27-memcpp-hook-contract-facts.md:39 | "@-referenced files don't trigger PreToolUse Read hooks" 被标为"官方警告原文"，但现行页面逐字无此句（现行措辞为 "...no PreToolUse hook fires for them, including hooks matching `Read`"） | 事实与设计结论不受影响，但转写当原文会削弱全文引文可信度，后续复核者会误判其余引文 | 换成现行原文短引，注明抓取日期 |
| O1（观察） | :63 | "v2.1.280/295/302 连续动周边"中 302 实为 prompt/agent hook 并发改动，与超时默认无关 | 易被读成"超时默认值活跃改动区"，放大 F1 的锚点失准 | 加限定："302 动的是 hook 并发，非超时默认" |
| O2（观察） | :86 | 断言 4 负向断言"任何内部异常路径都不产生 exit 2"无枚举方法 | P1 落地易退化为只测 4 个正例的假绿 | 补故障注入清单或编译期 exit() 包装静态检查口径 |
| O3（观察） | :80、:82 | P0/P1 未随文定义 | 独立阅读门槛 | 首现处加括注"（P0=最小可用交付，P1=检索+注入，见报告 §6.1）" |

## 总裁决

**有 finding 需修（3 条：中 2 / 低 1）——不阻塞合入，但 F1/F2 必须在 P1 契约测试立项前更正**：文档整体质量高（四条关键引用三条逐字坐实、changelog 锚点 15 个全部命中、"未查到"披露如实、翻案结论证据强度口径正确），两处中危均集中在"版本/原文钉版"这一文档自身卖点上，属自伤型缺陷，修正成本低。

## 处置栏（2026-10-27 回填）

- F1（中）：已修（见本提交）——:37 删去幻影锚点 v2.1.245，改为"现行文档行为、changelog 无引入记录，不钉版本"；文末引用清单同步移除该锚点。
- F2（中）：已修（见本提交）——:38 按官方口径更正：file_path 一律绝对路径且为平台原生分隔符（Windows=反斜杠，附现行原文短引），正斜杠比较失配放行（官方专节警告）；并加 memcpp 实现提醒一行（路径比较必须按平台原生分隔符，跨平台配置不得混写正斜杠）。
- F3（低）：已修（见本提交）——:39 改为如实表述（事实成立但为转写，现行页面无逐字原句），换现行措辞短引并注明抓取日期。
- O1（观察）：已落实——:63 加限定"302 动的是 prompt/agent hook 并发上限，非超时默认"。
- O2（观察）：已落实——:86 断言 4 补负向断言枚举口径（编译期静态检查 exit(2) + 内部异常路径故障注入清单）。
- O3（观察）：已落实——:80 首现处加括注"（P0=最小可用交付，P1=检索+注入，见报告 §6.1）"。
- 连带：报告 :136/:230/:327 的 async 旧表述按钉版文档交叉核对表结论回改；钉版文档交叉核对表补处置注记。
