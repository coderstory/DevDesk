# DevDesk 全应用打磨·多维分析综合报告（need#284）

- 日期：2026-11-18｜发起：老板令「打磨整个应用，分析调研不开发」
- 基线：feat/notepad-batch1 @ 64378979（notepad 在建未注册，不收录）；探针/驱动二进制 Nov 17 18:50 构建
- 方法：**手册先行**（15 章步骤级用户手册=脑内功能演练，含异常场景表+卡壳点）→ **按手册实操走查**（ui_driver 真点击链+截图，沙箱数据根）→ **三路走查补维**（UI/UX×2 + 超时盲区矩阵 + 代码静态扫描）
- 产物索引见 §13；全部发现带 file:line 锚点，印象分零容忍。

## §2 总览仪表盘

| 维度 | 产物 | 计数 |
|---|---|---|
| 功能演练（手册） | manual/ 15 章 970 行 | 卡壳点 64｜异常表：未处理 2 / 未知 12 / 半处理 15，其余已处理带锚点 |
| 实操走查（15 章） | walkthrough-prod.md + walkthrough-ai-system.md | PASS 123 / FAIL 3 / 偏离 7 / 需人工≈41；BUG×2；截图 166 张 |
| UI/UX 走查 | uiux-walk-productivity.md（23 条）+ uiux-walk-ai-system.md（30 条） | P0×1 / P1×5 / P2×27 / P3×20 |
| 超时盲区矩阵 | timeout-blindspot-matrix.md | 24 行×7 类调用面：P0=0 / P1=2 / P2=7 / P3=13 / 未知=2 |
| 代码静态扫描 | static-scan.md | P1×3 / P2×6 / P3×7（吞没/硬编码/平台/文案四面） |

## §3 必修清单（P0/P1 合并去重）

**P0（数据损坏级，1 条）**
1. **定时·编辑 cron 规则表达式不回显且被清空，保存即损坏规则**——双击只想改名/启停，保存后表达式清空、规则不再触发（schedule_ui_edit.cpp:393-395）。短期止血：保存时表达式为空且原规则是 cron → 阻止+红字。

**P1（12 条）**

| # | 类 | 条目 | 锚点 |
|---|---|---|---|
| 1 | 崩溃 | BUG-2 定时弹窗保存被拦后操作序列 SIGABRT（std::bad_alloc，exit 134），归属 app/driver 未定位 | walkthrough-prod.md BUG-2 |
| 2 | 挂死 | 流式转发上游读无空闲超时，半开连接 indefinite 挂 | streaming_proxy.h:94 |
| 3 | 挂死 | MCP hub 工具调用无外层超时总闸，慢工具挂死请求线程 | mcp_protocol.cpp:195-290 |
| 4 | 数据 | schedule 域 store 写 fire-and-forget 30 处级：崩溃重启后 delivery 停 retry/pending→重复投递，规则启停态回退复活 | action_runner.cpp:209-372、schedule_engine.cpp:61-672 |
| 5 | 机制 | result.h 未标 [[nodiscard]]——全站丢弃 Result 零告警，吞没面根因 | result.h:73-74/:134-135 |
| 6 | 纪律 | 业务层 `#ifdef __APPLE__` ×2（§三铁律违例，window_layout 三平台族现成零成本迁移） | app_sidebar.cpp:1388/:1441 |
| 7 | 安全 | agent 运行记录（提示词/工具结果）明文落库无脱敏 | agent_store.cpp:310-315 |
| 8 | 规范 | Git Tab 自绘下划线式+active 字重 500，与块面 wtab、两档制双违 | git_views.cpp:174 |
| 9 | 规范 | 定时三弹窗（编辑/历史/删除确认）未迁标准壳，删除=48×20 裸文字热区 | schedule_ui_edit.cpp:471/873-880 |
| 10 | 规范 | 字重两档制漏盘 4 处+药丸估宽(500)/渲染(600)不一致 | wallpaper_ui.cpp:1193、about_module.cpp:276、release_detail_draw.cpp:875、md_assoc_card.cpp:181、agent_module.cpp:802 |
| 11 | 规范 | 记忆主控台健康格②「跑评测」按钮与状态行长文案重叠不可读 | memory_module.cpp:436-465 |
| 12 | 待定 | D1 种子启用路径 ClaudeService 未装配（列表红字+toast 长驻）——驱动「开启后重启」等价路径仍现，真机复核若复现即升级 bug | walkthrough-ai-system.md D1 |

## §4 实操走查录得（BUG 与偏离）

- **BUG-1（P2）**：定时新建选「常驻」→ 红字含乱码（tzId 值未转义）且保存被拦（evidence/…/05-schedule/fail-14.png）。
- **BUG-2（P1 待定级）**：同弹窗后续序列 SIGABRT（§3#1）。
- **D-00-1（裁决项）**：设置页四个 Tab 均无「主题」控件，手册 00-11「深/浅/跟随可切」与实现不符——补控件或改手册（§11）。
- **D-01-1（P2）**：首页「最近编辑」空数据整块不渲染，无空态提示。
- **D2（P2）**：壁纸未配置未播放时让路组常显「让路中」，语义误导。
- **D3（低危待核）**：终端会话落 app 工作目录而非主目录（疑驱动 cwd 假象，真机双击复核）。
- **D4（方法学）**：driver 截图须交互后 ≥800ms（写进 UI 走查 cheat sheet，非 app 缺陷）。

## §5 记忆模块专项（老板点名直答）

1. **闭环了吗？**——页面层「整理→审核→入库→检索」的 IA 已成立（need181 九项全落地）；但**「整理成文」无手动入口**（代码中仅提案来源枚举），全链必须靠对话注入驱动——是否补手动按钮=设计含糊，**待老板裁决**（手册 08 卡壳点#1）。
2. **审计内容乱？**——三形态不打架（待审=卡片流/调用记录=折叠表格/动态=feed）；「乱感」来源=**19 条像素级残留**：窄窗挤压重叠×7（批量条破版/组头摘要越界/磁贴溢出）、健康格重叠（P1）、TAB「记忆审核 (1629)」定宽 132 已溢出、死进度条、chevron 无动效、组头无 hover。逐条见 uiux-walk-ai-system.md §1.3。
3. **弹窗不合理？**——6 弹窗已全走 #156 标准壳；残留 2 条：**全文弹窗用可编辑 input 承载只读内容**（编辑静默丢失，M19）、向导按钮手工偏移 hack（M20）。
4. 硬事实：真库 1629 条待审（徽章溢出已触发）；引擎端口 8421/llama-server 11435；telemem LLM 调用在 Python 进程内**完全不可观测**（超时矩阵行 3，偏 P1）。

## §6 异常场景盲区（老板点名族映射）

| 点名场景 | 现状 | 锚点 |
|---|---|---|
| 大模型调用超时 | 常规面闭环（直连 120s/转发 120s/探测 15s·5s·2s+分类 toast）；洞=流式上游读+MCP 总闸+telemem 不可见 | 矩阵 §二 |
| MCP 访问失败/解析失败 | 解析失败 -32603 兜底已处理；健康探测 1.5s | 矩阵行 10-11 |
| 文件访问拒绝/写入失败 | Result 链+中文文案齐；但**退出时保存失败无 UI（丢稿）**、「错误到眼」未逐点核实 | markdown_service.cpp:283-306、md_module_tree.cpp:634-654 |
| 文件名过长/特殊字符 | 无前置预检，OS 级失败才 toast；Windows 保留名未知 | md_module_menu_poll.cpp:633-651 |
| 编码不识别 | Git 大文件/二进制/非 UTF-8 有占位文案；**编辑器开 GBK/二进制伪装 .md 未知** | manual 02 异常表 |
| 数据库文件丢失/损坏 | 错误映射完整（CORRUPT/BUSY/PERM/FULL）；模块级「库打不开→用户提示」未逐模块核 | sqlite_db.cpp:16-31/:275-277 |
| 视频文件不存在/解码失败 | playbackError 上抛已处理；错误是否上 UI 未核 | mpv_controller.cpp:9 |
| keychain 失败 | 错误链区分 NotFound/拒绝/取消；**toast 丢弃底层错误详情** | keychain_macos.cpp:21-138、memory_module.cpp:90 |

盲区大小：矩阵 P0/P1/未知=4/24≈17%；含 P2「提示可达性未核实」=11/24≈46%。

## §7 跨模块一致性主题（修复时合并处理）

T1 弹窗壳双轨残留（定时三弹窗）｜T2 Git Tab 双违｜T3 页头动作控件五页五样（40/36/32/裸文字）｜T4 空态组件覆盖不一（git/schedule 裸灰字）｜T5 果冻按压覆盖不齐（todo/home 死按）｜T6 Secondary 按钮三态同色全站无 hover｜T7 键盘路径不一（待办可全程键盘、定时只能鼠标；git 无键盘导航）｜T8 字重/材质零散（700 家系待裁决：弹窗标题×3、todo 角标、sidebar/splash 品牌字、md 大纲；估宽三套并存；release_reminder 绕 token 硬编码 hex）｜T9 toast 双谱系（todo 挤标题 vs markdown/memory 双槽）｜T10 文案离群（「知道了」全站唯一、半角冒号×2、「移除」vs「删除」）。

## §8 文档-实现漂移（USER_GUIDE/手册，建议批次 G 统一改）

1. Git 模块=只读查看器（无 stage/commit/分支/网络）git_views.cpp:215；2. 待办固定四列，列管理已删 todo_module.cpp:1083；3. 设置模块开关「即时生效」vs 指南「重启生效」settings_ui_cards.cpp:430-433；4. 连通探测=POST /v1/messages 1-token ping 非 GET /v1/models claude_service.cpp:248-252；5. 首页统计卡 5 张恒显+标题「工作台」；6. 侧栏宽 220 非 260；7. **主题控件实测缺失**（D-00-1）；8. 终端默认目录（D3 待核）。

## §9 正面确认（验收时勿误修）

两档字重五模块主体已落地；schedule 行卡/todo 间距 8 合规；drawDialogShell 已覆盖 9+6 文件（markdown/todo/claude/settings/release/memory 六弹窗）；todo 优先级染色/home 统计卡已走 cardSurface；agent 工具弹窗 v2/终端 need241v2/发布 #195 正样本全落地；空态组件 agent/memory/release/settings/mcp 统一；catch 全站 20 处均 LOG 非静默、空错误分支 0；裸字号无脱离 token。

## §10 需人工补测（≈41 项，索引在两份 walkthrough 末章）

高优先：D1 真机重启复核；BUG-2 崩溃复现定级；git 选仓后全链（原生选择器射程外）；定时建规则保存链；右键菜单系；记忆「开始安装」全链+MCP memory_search 端到端；壁纸真实设壁+让路；Claude 接管/测试连通真机；Windows 侧 credential_protection。

## §11 待老板裁决项

1. **记忆「整理成文」手动入口**：要不要、形态（页头按钮/右键）？
2. **主题切换**：设置页补主题控件（深/浅/跟随）还是删手册口径？现主题究竟在哪切（托盘/原生联动？）
3. **700 豁免名单过会**：弹窗标题×3、品牌字/角标/大纲——按 need219 §2.3 悬置项一并裁决。
4. **全屏提醒渠道**：设置页补开关，或渠道置灰+「需在设置开启」。
5. **BUG-2 复现授权**：真机复现属实操，超出本次调研边界。
6. 批次草案（§12）排产顺序与取舍。

## §12 打磨批次建议（草案，每批=一条需求独立走全流程；批准后一条一条排产）

| 批 | 名称 | 内容（条目号） |
|---|---|---|
| A | 止血 | P0 cron；BUG-2 复现修复；流式空闲超时；MCP 总闸；D1 复核 |
| B | 错误处理收口 | [[nodiscard]] 开启+全站 (void) 点显式化；schedule fire-and-forget 收口；退出丢稿/迁移失败弹窗；错误到眼核实（矩阵 P2 行）；BUG-1 乱码+内部字段名+旧错误残留 |
| C | 弹窗与页头归一 | T1 定时三弹窗迁壳；T3 页头归一；T6 Secondary hover；T9 toast 谱系；todo 详情分隔线 |
| D | 样式收口 | 字重漏盘+700 豁免落地；估宽三套归一 ui_kit；release_reminder token 化；T2 Git Tab；平台宏迁移+映射表更新；MCP 双材质 |
| E | 交互补齐 | T7 键盘路径；归档入口；根级新建目录；T4 空态覆盖；T5 果冻；终端断点维度；文件名预检 |
| F | 记忆像素级 | §1.3 的 19 条 + 裁决项 1/2 落地 |
| G | 文档同步 | USER_GUIDE 以手册为底重写；漂移×8 修正；D4 入 cheat sheet |

## §13 产物索引

```
docs/research/2026-11-18-app-polish-audit/
├── REPORT.md                    本报告
├── timeout-blindspot-matrix.md  超时盲区矩阵（24 行）
├── static-scan.md               静态扫描（16 条）
├── uiux-walk-productivity.md    UI/UX 走查·生产力域+全局壳（23 条）
├── uiux-walk-ai-system.md       UI/UX 走查·AI域+记忆专项（30 条）
├── walkthrough-prod.md          实操走查·生产力域六章
├── walkthrough-ai-system.md     实操走查·AI/系统域九章
└── manual/00~14*.md             用户手册 15 章（演练脚本）
evidence/2026-11-18-need284-polish-audit/   截图 166 张+场景 JSON+dump/report（git-ignore）
```

## §14 边界与未核面

静态扫描未做 `.value()` 无守卫调用图级核对；Windows 侧 keychain/crontab 行为未核（本机 macOS）；停代理在途流收尾未核（矩阵行 8）；telemem 需实跑断网观测。本阶段零代码改动，全部产物为分析文档与证据。
