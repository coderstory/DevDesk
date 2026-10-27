# memcpp P0.5 探针审计（阶段 2 关卡）

- 审计对象：`d08f4ac7` feat(memcpp): P0.5 探针验证 llama.cpp 静态链接+量化模型进单 exe
- 实现者：Claude Agent（feat/memcpp 分支，worktree D:/wt-memcpp）
- 审计者独立性：审计者未参与本轮改动、非实现者延续会话，接受派单
- 审计方式：只读 + 本机复跑（build/ 与 models/ 均在位），唯一写动作为本文件
- 日期：2026-10-27

## 六维度逐条结论

### 1. 正确性 —— 已核，1 finding
- probe_main.cpp:50-54 加载 GGUF、:40 默认 prompt「今天天气」UTF-8 字节硬编码（已验字节正确）、:122-123 打印 N_EMBD 与 EMB[0..3]，与声称行为一致。
- probe_main.cpp:114-118 PeakWorkingSetSize 为进程高水位标记，decode 后读取可捕捉全程峰值，打点位置正确。
- 三数字是「测出来的」，本机复跑全部命中：
  - exe 10,423,028 B（9.94 MiB）；模型 26,472,640 B（25.25 MiB）
  - EMB[0..3] = 0.345589 0.101084 -0.121297 0.365586（与声称逐位一致），exit 0
  - PowerShell Measure-Command 7 次外部墙钟：首跑 111.6ms，其余 89.8~94.3ms，p50≈92.2ms —— 与声称 p50≈92ms 吻合
  - PEAK_RSS 31.7MB（区间 31.6~31.9 内）；N_EMBD=512；cos(今天天气,今天天气真好)=0.8414（与 commit 声称一致）
- **F1 [medium] d08f4ac7 commit message（对照 tools/memcpp-probe/README.md:18）**：模型「24.82 MiB」与其同句自引字节数 26,472,640 B（=25.25 MiB）矛盾；实际包体合计 35.19 MiB 而非声称的 34.76 MiB。README 写 25.2 MiB 是对的，错在 commit message 的换算。影响：可行性报告若引用三数字将低估包体 0.43 MiB（1.7%），量级结论不变。建议：报告口径更正为 exe 9.94 + 模型 25.25 = 35.19 MiB（或统一记字节数）。
- 处置（阶段 3）：已修，见 6a8f8e0——README 写入实测字节数与正确换算（exe 10,423,028 B = 9.94 MiB + 模型 26,472,640 B = 25.25 MiB = 总包体 36,895,668 B = 35.19 MiB）；commit message 错数字不重写历史（fix-forward），以 README 为准。

### 2. 回归面 —— 已核，未发现问题
- git show --stat：仅 4 个新文件（+195，零删改），全在 tools/memcpp-probe/ 下；根 CMakeLists.txt、src/、tools/memory-lab/experiments/（冻结资产）均未触碰。
- git ls-files tools/memcpp-probe/ = 仅 .gitignore / CMakeLists.txt / probe_main.cpp / README.md 四源文件。
- git check-ignore 实证 .gitignore:2-4 把 llama.cpp/、models/、build/ 全部挡在库外；git status 无大文件入库。
- 观察（非 finding）：工作树有未跟踪 tools/memory-lab/sanitize/，非本提交产物，后续提交勿顺手 add。

### 3. 平台对称性 —— 已核，1 finding
- commit message 标注「本机 Win11 / MSYS2 MinGW64 gcc 16.2.0」，未把 Windows 数字写成跨平台结论。
- **F2 [low] tools/memcpp-probe/README.md:27-30**：「代表性边界」只声明了模型选型边界，未显式写「仅 Windows/MinGW 实测、macOS/Linux 未验」；本工程经 windows.h/psapi（probe_main.cpp:6-7）天然 Windows-only 但无 guard 无声明。影响：跨平台读者可能把三数字当通用结论。建议：README 边界段加一行平台声明。
- 处置（阶段 3）：已修，见 6a8f8e0——README 边界段补「三数字仅 Windows 11 / MSYS2 MinGW64 实测，macOS 未验证，不可当跨平台结论引用」。

### 4. 静默失败面 —— 已核，1 finding
- 错误路径齐全：usage→exit 2（:35-38）；模型加载失败→stderr+exit 3（:51-54）；上下文失败（:66-69）、tokenize 失败（:77-80）、decode 失败（:94-97）、emb 为空（:99-102）均 stderr+exit 3。llama 日志被 :45 关闭，但返回值全查，无吞错返默认值。已核。
- **F3 [low] tools/memcpp-probe/probe_main.cpp:106-112**：MEMCPP_PROBE_DUMP 的 fopen 失败被静默忽略——实测传无效路径（/tmp/...，Windows exe 语义解析为 D:\tmp）时 exe 不写且无任何提示。影响：复核辅助失灵且无从察觉。建议：fopen 失败时向 stderr 报一行。
- 处置（阶段 3）：已修，见 6a8f8e0——fopen 失败向 stderr 报 errno 原因并 exit 3（对齐既有错误口径，不设 MEMCPP_PROBE_DUMP 时零开销契约不变）；复跑正常路径 EMB[0..3] = 0.345589 0.101084 -0.121297 0.365586 exit 0，无效路径报「dump open failed: … (errno=2 No such file or directory)」exit 3。
- 观察（非 finding）：:74-81 tokenize 缓冲固定 32 token，长 prompt 走 stderr+exit 3（非静默），探针定位短输入可接受，产品化前需按返值重试。

### 5. 测试有效性（复现证据）—— 已核，未发现问题
- 实际复跑探针（非推演）：EMB[0..3]=0.345589 0.101084 -0.121297 0.365586、exit 0、N_EMBD=512、dump 全量 512 维、PEAK_RSS 31.7MB、进程内 TOTAL_MS≈35.0 与 commit「≈35ms」一致、cos=0.8414。断言锚定可复现的确定值，改动回退（如去 -static 或换模型）会立即体现于 DLL 依赖/向量/体积。
- DLL 依赖实测：仅 ADVAPI32/KERNEL32/SHELL32/msvcrt，零 libgcc/libstdc++/winpthread —— 静态链接声称属实。
- 观察（非 finding）：commit 引用 exe 10,422,516 B，当前产物 10,423,028 B（+512 B），提交后 relink 漂移，MiB 取整无差。
- 本提交为新探针，无「先红后绿」适用场景。

### 6. 文档口径 —— 已核，未发现问题（平台声明缺口归 F2）
- README.md:5 声明独立于 `scripts/build-test-run.sh`；commit message 首段有完整例外声明（脚本管不到独立 CMake 工程，故手敲独立构建目录）。已核。
- README.md:27-30 边界如实：模型是可行性报告 §8.2 拍板项、探针只证体积/启动/内存量级、不回答检索质量与 pooling 口径。已核。
- README.md:10/15/19 复现命令含本机代理端口与 /d/SoftWare 个人路径，可执行但不可移植；属一次性验证工程惯例，不立 finding。

## 总裁决

**通过，附 4 条非阻断 finding（F1 建议在可行性报告引用前更正数字）。**

- F1 [medium] 模型 MiB 换算错：26,472,640 B = 25.25 MiB，非 24.82 MiB；真实包体 35.19 MiB 而非 34.76 MiB（commit message，量级结论不变）
- F2 [low] README 边界段缺「仅 Windows 实测、macOS 未验」显式声明（README.md:27-30）
- F3 [low] MEMCPP_PROBE_DUMP fopen 失败静默（probe_main.cpp:106-112）
- F4 [low] tokenize 32 token 上限走报错退出而非重试（probe_main.cpp:74-81，产品化前处理）
  - 处置（阶段 3）：不修——按本条原建议「产品化前处理」：探针定位短输入一次性验证，超限输入已走 stderr+exit 3 非静默报错；记为产品化前待办。

复跑证据：EMB[0..3] = 0.345589 0.101084 -0.121297 0.365586；p50 外部墙钟≈92.2ms；PEAK_RSS 31.7MB；静态链接仅系统 DLL。
