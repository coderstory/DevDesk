# DevDesk 打磨专项收尾报告（FINAL-REPORT）

- 日期：2026-11-18｜条目：need#306（打磨专项收尾文档批）｜角色：product-manager
- 数据来源：池台账（needs / bugs / clues 表）+ `git log 63900b3e..HEAD` 打磨提交族 + 同目录 REPORT.md / closure-qa.md / closure-qa-c.md；素材以台账与提交记录为准，零发明。
- 收口说明（2026-11-18 终验波 need#329）：need#304 / #305 已随 keychain 全链退役（`98d1cccc`）+ 编译硬禁令（`6be415d7`）闭合；原「在飞段【待填】」标记已回填，终验证据见同目录 final-verification.md 与 evidence/2026-11-18-need329/。

## §1 交付矩阵（批次 × 池条目 × 提交号 × 一句话）

审计 need#284（REPORT.md §12 批次草案）批准后按「一条需求走全流程」逐批排产，全部待验收：

| 批次 | 池条目 | 主提交 | 一句话交付 |
|---|---|---|---|
| A 止血 | need#290（P0） | `5022ae9f`（前置 wip `92ca67ae` 流式看门狗 / `a0ed6d99` MCP 总闸） | cron 编辑回填 P0 修复（不再保存即损坏规则）+ BUG-2 SIGABRT 悬垂值捕获 + 流式空闲看门狗（默认 300s，env 可调）+ MCP 工具调用外层总闸（默认 600s）+ D1 复核（驱动假象结案） |
| B 错误处理收口 | need#291 | `f026000e`（前置 wip `66b7d12e`） | schedule 域 25 处 store 写 WARN 留痕 + pending 推进早退回队（3 用例真失败注入）+ 退出丢稿确认弹窗 + 迁移失败 msgbox |
| C 弹窗与页头归一 | need#294 | `c714de01`（C1）+ `130e5136`（C2）+ `ca6b69db`（白卡收口） | 定时三弹窗迁 drawDialogShell 标准壳（Danger 删除钮）+ 四页页头 40 档 + Secondary/Danger 三态 + 白卡 opt-in 描边（memory×2 / wallpaper×1）+ todo 错误 toast 双槽 |
| B 尾项独立 | need#295 | `6d18c802`（前置 wip `1fae39a8`） | result.h `[[nodiscard]]` 全站开启 + 丢弃点两口径收口（接住留痕 / DB 写路径透传）+ `-Werror` 零告警实证 |
| D 样式收口 | need#296 | `111f4445`（前置 wip `6fbc846c`） | 字重两档制漏盘清零 + 估宽归一 cachedTextWidth + Git Tab 块面化 + 平台宏上收 window_layout + MCP 记忆行玻璃 + reminder token 化 |
| E 交互补齐 | need#297 | `ec8cf584`（前置 wip `d23284bc` / `15694ef7`） | schedule 弹窗 Enter/Esc 键盘路径 + todo 归档 chip / 根组右键新建 / 任务卡果冻 + git/schedule 空态 kit 化 + 笔记文件名预检（保留名 + 255 字节） |
| F 记忆像素级 | need#298 | `506cba6d` | 走查 §1.3 全 19 条处置（M1–M20 编号系：窄窗几何 / hover / chevron 动效 / thirdAction 左槽 / 全文弹窗只读化） |
| G 文档同步 | need#299 | `c493605e` | USER_GUIDE 全量重写 310 行 + 漂移×8 全修 + UI 走查方法学 cheat sheet + 附录交付索引 |
| 收尾 P2 修复批 | need#300 | `6c9620a4`（前置 wip `58c36bfc` 三修） | schedule 删除竞态延一帧错开 + 隐藏窗 Exit guard 先唤起主窗 + 启动失败族接 msgbox + 调试残留清零 |
| 杂项批 | need#301 | `a0b27ab8`（审计清零并入 `0e9199c0`） | 500 字重双清零（git_views / agent_dialogs）+ MCP 弃置 worker 计数观测 + home 统计卡果冻 + terminal-settings 遗留清理 |
| driver 能力增强 | need#302 | `0e9199c0`（前置 wip `c660712a`） | driver rightClick 原语 + 定时一次性规则链场景 + disabled 断言（闭合态判据=留树带标志）+ abandonedToolWorkers nodiscard（审计 P2/P3 清零） |
| 壁纸让路 + stall | need#303 | `cb778a0d`（前置 wip `6f0f9fd8`） | 壁纸让路三分语义实态词（真让路才显「让路中」）+ stall 用例语义断言 / 墙钟绑死 stallMs |

**收官批（need#304–#328，2026-11-18 终验 need#329 全量 EXIT=0 通过）**

| 条目 | 主提交 | 一句话交付 |
|---|---|---|
| need#304 keychain 禁用·测试门控 | `7d4e6a2b` + `98d1cccc` | KEYCHAIN_TEST_GATE 门控收敛真触达 + DOCTEST_SKIP 可见跳过；二期删 test_keychain（11 用例） |
| need#305 keychain 退役·运行时迁移 app_secrets | `ab4b0fdd` + `c2c75e20`（审计 P0 依赖倒置：保护缝下沉 core）+ `98d1cccc`（二期删五平台文件） | 运行时三处切 app_secrets 专表，keychain 全链退役 |
| need#306 收尾文档批 | `7625763a` | 本报告骨架 + USER_GUIDE keychain/批次F 同步七处 |
| need#307 cron 翻倍归因 | `4e6f76bd` + `417ee8f0` | 翻倍根因=预填默认值追加非事件重复投递；建存→编辑回显 E2E 链 3/3 绿 |
| need#308 wecom 分文案 | `b3aecccc` | NotFound→未配置 / unprotect 等→数据异常需重录，两态用例锁 |
| need#309 关闭序摘牌 | `7da441d7` | appDatabase 退出置空先于连接析构（atexit LIFO）+ 顺序探针 |
| need#310 观感素材 | `14e05bbf` + `9b2fafc7` | FolderMissing 摘要专词 + 非播放态 chips 压暗 + git 过滤空态独立基 id + guide 缺席双向断言 |
| need#311 壁纸视频内存缓存 | `674c1875` | 判定纯函数/设置项/服务开关/mpv per-file 参数/探针 §9（wallpaper_ui.cpp+test 工作树 hunks 留待归位，见 tech 卡 §6） |
| need#313 真机复核 | `83c44cae` | D1 装配 9 启零未装配（结案）+ 终端 cwd 新 P1 发现（立 need#328） |
| need#314 人工项三分拣 | `6faf8d1f` | 需人工 66 项：可自动化 18 / 真机 35 / 裁决或消解 13，已消解 9 对号 |
| need#315 记忆 E2E | `76ca5698` | 可行性简报：42/46 PASS，缺口 G1–G3 冻结期不修（老板令） |
| need#316 手册对账 | `48c6c263` | 15 章 40 条行为 delta + 64 卡壳点三态对账（消解 8/仍在 55/裁决 1） |
| need#317 审计 P1 清零 | `e8519f1d` | key-e2e 子命令自开临时库注册 appDatabase（TestDb+ScopedAppDatabase） |
| need#318 wecom 判据收窄 | `983d8f61` + `b3fd954a` | InvalidApiKey(值形态坏)→重录 / Internal(瞬时)→另报；平台解不开错误码统一 InvalidApiKey |
| need#319 driver 空串判据 | `c480847c` | 空串 equals=定点「字段为空」判据，恒真双护栏（非 equals 拦 + 无 id 全页扫描拦） |
| need#320 走查自动化批1 | `02cdb201` | 侦察场景+四页 id dump 取证（生产场景未落地，交接报告） |
| need#321 主题三档 | `ac7e7dfe` + `8ffa1821` | 外观卡主题三档行（浅色/深色/跟随系统）+ 卡高 184→226 + settings 137/137 绿 |
| need#322 700 字重豁免 | `1c86293e` | 5 处归 600 + 3 处豁免标记（裁决项 4 收口） |
| need#324 并行度 | `a6676a1b` | DEVDESK_JOBS 默认 3→6（老板令，脚本+AGENTS+CLAUDE 三处口径同步） |
| need#325 走查批2 | `019e0f56` | schedule-resident-enable-chain + todo-keyboard-move 入 CMake（A-4/A-5/A-6/C-13） |
| need#326 EUI 双壳互冻 RCA | `8a303dba` | AnimatedValue setTarget 早退假说链 + 方案A五行自愈（待 POC） |
| need#327 keychain 编译硬禁令 | `6be415d7` | check-no-keychain.sh 前置门（src/tests 零引用，命中即构建中止） |
| need#328 终端 workDir | `754c4531` | macOS 桥接三层透传（Swift workingDirectory some/none + resolveSpawnWorkDir）+ spawn e2e |

（need#312 / #323 在提交族 `63900b3e..HEAD` 无对应提交，行不加防发明，终态以池台账为准。）

## §2 质量底盘演进表

| 时点 | doctest Total / Failed | 事件与证据 |
|---|---|---|
| 2026-10-29 | 1954 / 0 | 全量基线 |
| 批次 A/B 收口 | 2462 / 0 | closure-qa（HEAD=66b7d12e 工作树）；probe 52/52（5 设计内跳过）；当轮 Windows 交叉编译红 1（test_mcp_protocol.cpp `setenv`/`unsetenv` POSIX 依赖）→ 可移植封装修复后消除 |
| 批次 C 收口 | 2462 / 1→0 | closure-qa-c：唯一红=emptyState 旧断言钉 hover 未随 C2 surfaceElevated 契约同步（一行修复）；白卡 Secondary 消失态专项确认→opt-in 描边处置 |
| 批次 E 收口 | 2468 / 0 | E 批增量入账 |
| 批次 F / 杂项后 | 2481 / 0 | 终态 |
| 终验（need#329，HEAD=8ffa1821 工作树） | 2486 / 0 | 完整档 EXIT=0：doctest 2479 过 / 7 跳（DOCTEST_SKIP 可见跳过族）；probe 52/57（5 设计内跳过，均有复跑 77 佐证）；守卫链全绿（keychain 硬禁令前置门 / Win 交叉编译 486 文件 / std include 915 文件 / 模块隔离 / 玻璃 / ToolGuard，自检好树绿坏树红非恒真） |

终验质量底盘（2026-11-18 need#329）：原唯一已知红 need#256 钥匙串空账号**已随 keychain 全链退役消号**（`98d1cccc` 删 + `6be415d7` 编译硬禁令防复发）；probe 52/57（5 设计内跳过）；守卫链全绿（含 Windows 交叉编译 486/486）；`-Werror` + `[[nodiscard]]` 零告警。

## §3 挂池 P2 观测项清单（锚点 + 处置建议）

未 resolved 的 bug 行 + 相关 clue，逐条现状与建议（「已收口」= 有对应交付提交，建议台账闭行）：

| 来源 | 锚点 | 现状 | 处置建议 |
|---|---|---|---|
| bug#28（#290） | mcp_protocol.cpp:317 总闸弃置 worker 泄漏无上限 | 观测已做：need#301 弃置计数 + LOG_WARN 含在途弃置数 | 保留 P2 观测；是否加并发上限 / 同 tool+session 单飞拒绝，待真实泄漏数据 |
| bug#29（#290） | test_streaming_proxy.cpp:826 wallclock 敏感 | 已收口：need#303 语义断言 + stallMs 绑死（`cb778a0d`） | 台账闭行 |
| bug#30（#291） | quit_save_bridge.cpp:46 隐藏窗 Exit 静默失灵 | 已收口：need#300 guard 阻止即唤起主窗（`58c36bfc`） | 台账闭行 |
| bug#31（#291） | main.cpp:175 启动失败静默 abort | 已收口：need#300 接 msgbox（`58c36bfc`） | 台账闭行 |
| bug#32（#291） | schedule 域 fire-and-forget 30 处级 | 已交付：25 锚点 WARN 留痕 + 回队（`f026000e`，登记备查） | 台账闭行 |
| bug#33（#294） | 白卡 Secondary 消失态（theme.cpp:83 surfaceElevated=#FFFFFF） | 已收口：`ca6b69db` ButtonOptions opt-in 描边（memory×2 / wallpaper×1） | 台账闭行 |
| bug#34（#294） | schedule_ui_edit 删除钮→同帧双弹窗竞态 | 已收口（应用侧）：need#300 延一帧错开；根因沉框架线 clue#307 | 应用侧闭行；框架侧随 clue#307 |
| bug#35（#294） | schedule_ui_edit.cpp:920 删除钮 danger 手抄副本第二真源 | 未处置 | P2：ui_kit 增 footer 左槽或危险单钮 helper，与记忆引擎详情弹窗 +33 hack 同族统一处置 |
| bug#36（#295） | DB 写路径「接住=透传」口径备案；proposal_store.cpp:445 等 4 处 bind 静默跳过 | 口径已备案 | 后续同类收口按透传口径执行；bind 留痕列 P3 顺手清 |
| bug#37（#296） | git_views 500 字重两处 + agent_dialogs 估宽 500 | 已收口：need#301 双清零（`a0b27ab8`）；框架域残留沉 clue#306 | 台账闭行；框架侧随 clue#306 |
| bug#38（#296） | release_reminder.cpp:90 最低档黄→绿非观感等价 | 未裁决 | 见 §4 裁决项；裁决后收口目检亮暗双档留痕 |
| bug#39（#297） | git 过滤空态与干净空态共用基 id「git.list.empty」 | 已收口：独立基 id 拆分（`14e05bbf`）+ filter.empty.guide 缺席双向断言（`9b2fafc7`）；终验实拍 evidence/2026-11-18-need329/git-states_05_filter_empty.png | 台账闭行 |
| bug#40（#303） | FolderMissing 折叠摘要落「未播放」泛词；非播放态 chips 亮显并存 | 已收口：摘要专词「目录不见了」+ 非播放态 chips 压暗（`14e05bbf`）；终验实拍专词条 evidence/2026-11-18-need329/wallpaper/library-empty-1000x640-light.png（chips 展开态无现役探针镜头，压暗口径以 wallpaper_ui.cpp:813 注释与断言为准） | 台账闭行 |
| bug#41（#302） | driver queueText 注入 cron.expr 字段文本翻倍待归因 | 挂起：待 #300 落地后重跑 dev302-recon 归因 | 重跑归因：真 app 缺陷立 bug，驱动伪影闭案；cron 建存→回显 E2E 链随此解锁 |
| bug#42（#298） | M 编号双标不一致；batchBtnRowY 固定 7.0f 未 scaled | 主会话顺手清零中（M 编号）；缩放口径未动 | 编号清零后闭行；缩放口径下次过带统一 |
| bug#43（#301） | home 果冻手写展开省略 card::build gradient/opacity 两行 | 行为等价，属演进敏感点 | 观测：框架 CardStyle 默认档演进时人工同步 |
| clue#306 | EUI-NEO/components/workshop/neumorphic_button.h:215 框架域 500 字重 | open | 下次 EUI-NEO fork 提交线顺手归 400/600（need219 两档制延伸到框架） |
| clue#307 | EUI 框架双壳同帧「1→0 + 0→1」过渡互冻（need#300 探针实锤） | open（应用侧已延帧规避） | 框架线待查 dialog 过渡状态机对同帧对偶开合的处理 |

## §4 待老板裁决项全集

| # | 裁决项 | 背景（一句） | 选项 |
|---|---|---|---|
| 1 | 常驻规则保存守卫 | 审计 BUG-1：定时新建「常驻」规则保存被拦（乱码面已由批次 A 文案安全化修复），被拦守卫的存废与口径未定 | ①保留守卫并补引导文案 ②移除守卫 ③调整守卫口径（放宽/收紧） |
| 2 | 设置页主题控件 | 实测设置页无主题切换控件（D-00-1），现深浅色随系统自动，USER_GUIDE 已按「入口规划中」口径落档 | ①设置页补深/浅/跟随控件 ②维持随系统自动并固化手册口径 |
| 3 | 记忆「整理成文」手动入口 | 全链目前必须靠对话注入驱动，代码中无手动入口（手册 08 卡壳点#1） | ①补页头按钮 ②补右键菜单入口 ③不补（维持对话驱动） |
| 4 | 700 字重豁免名单 | 两档制（400/600）外残留 700 家系：弹窗标题×3、todo 角标、sidebar/splash 品牌字、md 大纲（need219 §2.3 悬置项一并过会） | ①按名单豁免（品牌/标题类保留 700）②全部归 600 两档制 |
| 5 | release_reminder 黄→绿观感 | reminder 最低档 Yellow #EAB308→success #10B981 非观感等价（error/warning 两档精确等值）；「票据黄」语义 vs token 阶梯冲突 | ①目检亮暗双档留痕后接受 ②保「票据黄」专项例外 ③统一 token 阶梯 |
| 6 | keychain 存量迁移 a/b | need#305 一期运行时已切 app_secrets 专表（`ab4b0fdd`），存量钥匙串数据的迁移方案 a/b 待裁决；二期迁移器随裁决解锁 | ①方案 a ②方案 b（方案细节以 need#305 条目为准） |

## §5 二期遗留清单

1. **keychain 退役收尾**（阻塞于裁决项 6）：存量迁移器 + `platform/keychain*` 五文件删除 + `test_keychain.cpp` 删除（随 #304 SKIP 语义落地后）。
2. **D1 真机重启复核**：驱动假象已结案，剩真机重启路径复核 + 场景回归。
3. **终端 cwd 真机双击复核**（审计 D3，驱动 cwd 假象待真机证据）。
4. bug#41 queueText 翻倍归因与 cron 建存→回显 E2E 链解锁（见 §3）。
5. 框架线两项：clue#306 框架 500 字重、clue#307 双壳同帧互冻（EUI-NEO fork 线处置）。

## 附：产物索引（本目录）

```
docs/research/2026-11-18-app-polish-audit/
├── REPORT.md            多维分析综合报告（need#284，审计底稿）
├── FINAL-REPORT.md      本报告（收尾：交付矩阵/质量底盘/挂池观测/裁决项/二期遗留）
├── closure-qa.md        批次 A+B 收口全量验证
├── closure-qa-c.md      批次 C 收口 QA（白卡专项 + 暗色三态）
├── timeout-blindspot-matrix.md / static-scan.md / uiux-walk-*.md / walkthrough-*.md
└── manual/              用户手册 15 章（演练脚本）
```
