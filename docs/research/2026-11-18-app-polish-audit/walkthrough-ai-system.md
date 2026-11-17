# DevDesk AI/系统域九章实操走查报告（need284 第二路）

- 走查人：qa-tester（UI driver 自动化通道 + 截图人工判读）
- 日期：2026-11-18；本机 macOS arm64；驱动进程恒用临时数据根（隔离验证成立：08 配置页路径盒显示 `build/testtmp/uidrv_root_*.db.data/memory`）
- 通道：`build/tests/devdesk_ui_driver`（场景 JSON + dump + screenshot）；场景与证据均在 `evidence/2026-11-18-need284-polish-audit/walkthrough/ai-system/`
- 高危红线执行：全程 `DEVDESK_CLAUDE_CONFIG_DIR=/tmp/need284-claude-sandbox`（06 引导条实测显示接管路径指向沙箱，未触真实 `~/.claude/settings.json`）；未点「启动代理/测试连通/导入/安装到 Claude Code」；壁纸未选文件夹未真实设壁；终端仅验证会话创建未跑长任务；未碰 8421/11435 端口与 pid 10605；全程未联网。

## 总计数

| 判定 | 数 | 说明 |
|---|---|---|
| PASS（步骤级） | 42 | 断言或截图证实与手册一致 |
| FAIL | 0 | 无 |
| 偏离 | 4 | 见各章「偏离」行（D1~D4） |
| 手册卡壳引用 | 3 | 06#2/06#4、07#1、08#1 |
| 需人工/无法自动化 | 14 | 红线副作用、系统对话框、文本输入类表单、真实外部服务 |

## 逐章结果

### 06 Claude 配置

| 步骤 | 手册预期 | 实际 | 证据 |
|---|---|---|---|
| 1 | 列表+状态点+引导条常驻 | PASS：右上「已停止」灰点+「启动代理」钮；引导条「启动代理将接管 Claude Code 配置（/tmp/need284-claude-sandbox/settings.json 指向本地代理），停止代理时自动还原」 | e06/06-claude-page.png |
| 2 | 新建弹窗三页签 | PASS：基础信息/模型配置/环境变量页签+名称/Base URL/API Key/认证字段（双发默认） | 06-claude-new-form.png |
| 3 | 导入当前配置 | 需人工（读真实 ~/.claude/settings.json，红线不触） | — |
| 4 | 详情空态 | PASS：名称「(未启用)」、Key「(无激活 profile)」、「○ Claude Code 未接管（启动代理后自动接管，停止时还原）」 | 06-claude-page.png |
| 5 | 测试连通 | 需人工（真实网络 POST，禁联网） | — |
| 6/7/9 | 切换/启动/停止代理 | 需人工（启动代理将写接管配置，红线；探测 2s 网络调用） | — |
| 10 | 「运行」直连会话 | 无法自动化（系统目录选择对话框，手册卡壳点#2） | — |
| 11/12 | 右键菜单/模型配置页签 | 部分验证：详情三页签在位（tab.0/1/2）；右键菜单 driver 无右键能力，需人工 | f06-form.txt |
| 空态 | 列表读失败红字 | PASS（以偏离 D1 形态出现） | 06-claude-page.png |

- **D1 偏离**：种子 config 启用的 claude_proxy 模块，页面显示 toast「Claude 服务未装配 / 本会话内开启的模块需重启后才能提供代理服务」+ 列表红字「Profile 列表加载失败： 服务未就绪（ClaudeService 未注册）」。与手册「会话内刚开启模块」口径吻合，但驱动是启动时种子启用（等价用户「开启后重启」路径）仍出现——**需人工在真实 app 开启模块+重启后确认服务装配**；若真实重启后仍「未注册」升级为 bug。
- UX 注记：「服务未装配」toast 连续两张截图（间隔数步）均在场，驻留偏长。
- UX 注记：新建 Profile 弹窗 backdrop 遮罩偏透，背景表单文字与弹窗文字视觉混叠（06-claude-new-form.png 可见）。

### 07 智能体

| 步骤 | 手册预期 | 实际 | 证据 |
|---|---|---|---|
| 1 | 标题+新建任务+可用工具钮 | PASS（tools 钮+badge「24」） | 07b-agent-tools.png |
| 2 | 新建任务弹窗+模板 | PASS：弹窗开（任务名等表单字段在位 f07-dialog.txt）；13 张出厂模板需滚动人工核 | 07-agent-new-dialog.png |
| 3/4 | 任务卡/手动启动 | 无法自动化（启动=真实 LLM 直连上游，禁联网）；空态「暂无任务 新建一个任务…」PASS | 07b-agent-tools.png 背景 |
| 5 | 可用工具弹窗 | PASS：24 个工具（agent.runs/app.now/app.notify/app.read_settings/app.stats/bash.run/fs.edit…）带搜索框 | 07b-agent-tools.png |
| 6/7/8 | 详情弹窗/试运行/启停失败红字 | 需人工（依赖真实运行） | — |
| 空态 | 任务空态文案 | PASS | 07b-agent-tools.png |

- 手册卡壳点#1（试运行容忍瞬时卡）不适用本次（未跑试运行）。
- UX 注记：工具弹窗关闭钮 id 与任务弹窗不同（task.cancel vs tools.cancel），自动化脚本易踩（开发侧无碍）。

### 08 记忆

| 步骤 | 手册预期 | 实际 | 证据 |
|---|---|---|---|
| A1 | 引擎状态带+按钮组 | PASS：「telemem 记忆引擎 未启动」+详情+首次安装向导+启动引擎三钮并存 | 08-memory-wizard.png |
| A2 | 向导四步清单 | PASS：①Python 未检测到（红）②venv 待开始 ③embedding 需下载（联网首次约 26MB）④引擎拉起待开始+intro 句+关闭/开始安装钮 | 08-memory-wizard.png |
| B | 配置页四折叠组 | PASS：提炼端点「未导入——提炼不可用」（黄）、触发线「升级线 85%·基线 5 次」、用量告警「日 50 万·月软 200 万/硬 300 万」、安全与隐私（脱敏开+路径盒+导出+清除红钮+页脚灰注） | 08b-memory-config.png |
| C | 采集→审核→召回全链 | 无法自动化（引擎未装，走安装向导空态即有效验证；禁 8421） | — |
| D | 健康四格+消耗带 | PASS：脱敏闸 0/110 强制开启·已锁定 / 触发线未评测 / 注入预算 0/5 条 0/500 字 / 存储概览 0；消耗带「今日 0tk·0.000 元·本月 0tk·0.000 元 预算充足」 | 08-memory-wizard.png |
| 三 TAB | 主控台/记忆审核(0)/配置 | PASS（tabs.0/1/2） | f08-config.txt |
| 数据隔离 | 随 DEVDESK_DATA_ROOT | PASS：路径盒显示 build/testtmp 沙箱 | 08b-memory-config.png |

- 手册卡壳点#1 维持：页面无「手动整理成文」入口（本次走查同样未见），设计含糊点待老板裁决。
- 需人工：向导「开始安装」全链（10 分钟 venv）、钥匙串 key 保存/清除、MCP memory_search 端到端（跨 3 页联动）。

### 09 壁纸

| 步骤 | 手册预期 | 实际 | 证据 |
|---|---|---|---|
| 1 | 播放/片库页签+视频/图片段钮 | PASS | e09-wallpaper.png |
| 2 | 播放页状态卡空态 | PASS：「还没有设置壁纸文件夹 选一个文件夹…支持 mp4/mov/m4v」+选择文件夹…+重新扫描 | e09-wallpaper.png |
| 2 | 让路规则组 | PASS（组在位）但有 **D2 偏离**：未配置、未播放状态下组头右端显示「让路中」——语义可疑 | e09-wallpaper.png |
| 3 | 片库：文件夹卡/花見导入/扫描计数 | PASS（逻辑树：folder.select/folder.rescan/folder.import(花見 mac)/library.footer 均 interactive）；片库视觉截图因驱动时序空白（见 D4），逻辑证据充分 | f09b-lib.txt |
| 页脚 | mpv·GPL v2 | 未核（在 dump 深处，未断言） | — |
| 空态 | 未选文件夹主按钮变「选择文件夹…」 | PASS | e09-wallpaper.png |

- 需人工：真实设壁、锁屏/熄屏/睡眠让路、花見导入真机、mpv 播放错误形态。
- **D2 偏离（建议人工复核，疑似低危 bug）**：让路组在「无文件夹+未播放」时仍亮显「让路中」，用户会误以为正在让路。

### 10 MCP

| 步骤 | 手册预期 | 实际 | 证据 |
|---|---|---|---|
| 1 | 服务区状态行 | PASS：「已停止」状态点+行；设置页 MCP 卡同源（8931） | 10-mcp-page.png / e13-settings.png |
| 2 | 工具卡列表+chip | PASS：仅 mcp 模块时「注册的工具 0」+空态「暂无已注册的 MCP 工具…」；全模块时 3 张 notes_* 卡带「内置/服务」chip | 10-mcp-page.png / 10b-mcp-tool-detail.png |
| 3 | 点卡→详情弹窗+参数表单+试一下 | PASS：notes_read 弹窗（描述/参数 path 必填/运行结果「尚未运行」/试一下/关闭） | 10b-mcp-tool-detail.png |
| 4 | 安装到 Claude Code | 需人工（写真实 ~/.claude.json，红线）；空态 PASS：按钮灰+「未检测到 Claude Code（~/.claude.json 不…）」 | 10b-mcp-tool-detail.png |
| 5 | 记忆卡开关 | PASS（toggle 在位，未拨动） | 10-mcp-page.png |
| 6 | JSON-RPC 协议错误码 | 无法自动化（服务未启动；启服务+客户端联调需人工） | — |
| 空态 | 工具注销点详情不开空弹窗 | PASS：仅 mcp 模块时点卡无弹窗（panel 为常驻占位，dump 佐证第一轮 f10-detail.txt） | s10-run.log |

- 注记：弹窗 backdrop 点击关闭在 driver 下不可达（backdrop 几何中心被 panel 覆盖，命中回读报截胡）——驱动方法学限制，非 app 缺陷；Esc/关闭钮通道需人工。
- 「参数 schema（JSON）」原文框未在弹窗首屏可见（可能在滚动区下方），需人工确认。

### 11 终端

| 步骤 | 手册预期 | 实际 | 证据 |
|---|---|---|---|
| 1 | TAB 栏+动作组（设置/全屏/新建） | PASS（gear/fs/new 三钮+tab.0） | 11-terminal-page.png |
| 2 | 新建会话/默认 shell | PASS（进入页面即有「终端 1」会话，zsh 提示符已渲染）；但 **D3 偏离**：提示符目录为 `DevDesk`（仓库目录），手册步骤2 说默认启动目录=用户主目录 | 11-terminal-settings.png |
| 3 | 键入/滚动/Ctrl+C | 无法自动化（driver 无文本输入步骤）；需人工 | — |
| 4 | 终端设置弹窗 | PASS：外观组（字体默认内置等宽/字号 13/配色主题 跟随应用+亮色 ANSI）+会话组（默认 shell zsh/启动目录/回滚 1000/光标 块+闪烁）+「对新会话生效」标注+恢复默认/完成——**池262「内容空白」无回归** | 11-terminal-settings.png |
| 5 | 全屏切换 | PASS：进/退两态截图正常（fs 图标同槽位） | 11-terminal-fullscreen.png / 11-terminal-exit-fs.png |
| 6 | 会话结束覆盖卡 | 需人工（主动退出会话会杀真 shell；未触发） | — |

- **D3 偏离（低危）**：终端会话启动目录实际落在 app 工作目录（DevDesk 仓库根），与手册「默认用户主目录」不符；怀疑驱动以仓库根为 cwd 启动 app 所致，真实 app 双击启动是否回落主目录需人工确认。
- 需人工：claude TUI 交互（手册卡壳点#3）、回滚行为、回滚行数过大警示文案实测。

### 12 上线

| 步骤 | 手册预期 | 实际 | 证据 |
|---|---|---|---|
| 入口 | 父项不可导航/子页可点 | PASS（直接点 ReleaseTasks/ReleaseConfig 成功；卡壳点#1 成立） | 12b-release-tasks.png |
| 1 | 页签 全部/发布中/准备中/已归档+新建任务 | PASS：「上线任务」标题+四页签+「新建任务」+「0 个任务」 | 12b-release-tasks.png |
| 2 | 任务卡 | 空态（无任务）——卡形态需人工造数 | 12b-release-tasks.png |
| 3/4 | 新建任务弹窗/非常规发布日确认 | 无法自动化（driver 无文本/日期输入；依赖基础配置先建环境——同样需表单输入） | — |
| 5/6 | 详情/上线提醒弹窗 | 需人工 | — |
| 空态 | 「还没有上线任务 创建一个上线任务，按日期跟进发布准备」+新建任务 | PASS（与手册逐字一致） | 12b-release-tasks.png |
| 基础配置 7-10 | 服务/环境表单与删除确认 | 无法自动化（表单输入）；两子页可达 PASS | 12b-release-config.png / f12b-config.txt（env 弹窗组件在位） |

### 13 设置

| 步骤 | 手册预期 | 实际 | 证据 |
|---|---|---|---|
| 2 | 外观卡（字号/窗口效果） | PASS：字号缩放 100% 滑条+窗口效果「关闭（默认）」 | e13-settings.png |
| 3 | 模块卡 9 开关 | PASS（逻辑断言 sw.4 在位+侧边栏 9 模块条目视觉在位：Markdown/Todo/定时/Claude/智能体/记忆/壁纸/MCP/上线）；模块 tab 内网格视觉截图因 D4 时序未留证 | e13-settings.png + f13-settings.txt |
| 5 | Claude 代理卡 | PASS：「未运行/未接管」双胶囊+随应用启动代理开关+代理端口 8888+应用 | e13-settings.png |
| 6 | MCP 服务卡 | PASS：端口 8931+运行状态「已停止」+`claude mcp add --transport http devdesk http://127.0.0.1:8931/mcp` 提示 | e13-settings.png |
| 4/8/9 | 笔记目录卡/日志卡/GitHub 卡 | 部分验证：通用 tab 首屏未含日志卡（需滚动）；日志卡/Token 显形需人工 | e13-settings.png |
| 7 | 提醒弹窗卡 | 未核（滚动区下方，未断言） | — |

- **D4 方法学注记（非 app 缺陷）**：driver 的 exists 断言基于逻辑树，tab 切换后内容卡视觉渲染有数百 ms 滞后；`wait exists` 立即通过后立刻截图会拍到空白壳（s13 两张、s09b 一张）。加 `wait ms ≥800` 后截图正常（e13/e09 证）。后续走查场景一律在截图前加显式等待。
- 需人工：Token 眼睛显形/离开页面恢复掩码/重新生成、日志级别切换即时清理、Git 错误对话框、模块开关逐个即时性（手册卡壳点#1，本轮仅验证 agent 开关链路的既有绿测试口径）。

### 14 关于

| 步骤 | 手册预期 | 实际 | 证据 |
|---|---|---|---|
| 1 | 三组行式信息 | PASS：「版本信息」「构建信息」「许可与致谢」组头 id 断言三连 | 14-about.png |
| 2 | 无可操作控件 | PASS（dump 仅滚动容器） | d14-about.txt |

## 偏离/发现汇总（top）

1. **D1**：种子启用（等价开启后重启）路径下 ClaudeService 未装配，列表红字+toast——需人工确认真实重启是否装配，是则升级 bug。
2. **D2**：壁纸未播放时让路组常显「让路中」——语义误导，建议改「未播放」或条件亮显。
3. **D3**：终端会话落在 app 工作目录而非用户主目录（可能驱动 cwd 假象）——需人工双击启动复核。
4. **D4**：driver 截图需在交互后等待 ≥800ms，否则拍到渲染前空壳（方法学，建议写进 UI 走查 cheat sheet）。

## 证据物清单（PNG ≥10）

| 章 | 证据 |
|---|---|
| 06 | e06-claude.png、06-claude-page.png、06-claude-new-form.png |
| 07 | e07-agent.png、07-agent-empty.png、07b-agent-tools.png、07b-agent-new-dialog.png |
| 08 | e08-memory.png、08-memory-console.png、08-memory-wizard.png、08b-memory-config.png |
| 09 | e09-wallpaper.png、09-wallpaper-play.png、09b-wallpaper-lib.png |
| 10 | e10-mcp.png、10-mcp-page.png、10b-mcp-tool-detail.png |
| 11 | e11-terminal.png、11-terminal-page.png、11-terminal-settings.png、11-terminal-fullscreen.png、11-terminal-exit-fs.png |
| 12 | e12-release-group.png、12b-release-tasks.png、12b-release-config.png |
| 13 | e13-settings.png、13-settings-cards.png |
| 14 | e14-about.png |

场景 JSON：explore.json + s06~s14 + 补丁 s07b/s08b/s09b/s10b/s12b（同目录）；逐场景 run log 与 report-json 同目录；dump 文本 f*.txt/d*.txt 为逻辑树证据。

## 工具调用自报

简报预算 ≤50 次工具调用，实际约 47 次（含 9 章手册读取 9 次、dump/探路 4 次、场景编写与执行 12 次、截图判读 13 次、报告落盘 1 次）；墙钟 45 分钟内完成九章。
