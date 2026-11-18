# DevDesk 打磨专项收尾报告（FINAL-REPORT）

- 日期：2026-11-18｜条目：need#306（打磨专项收尾文档批）｜角色：product-manager
- 数据来源：池台账（needs / bugs / clues 表）+ `git log 63900b3e..HEAD` 打磨提交族 + 同目录 REPORT.md / closure-qa.md / closure-qa-c.md；素材以台账与提交记录为准，零发明。
- 在飞说明：need#304 / need#305 闭合前本文保持「在飞段【待填：闭合后补】」标记，闭合后由收口批回填。

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

**在飞段【待填：闭合后补】**

| 条目 | 现状 | 已见 wip | 闭合后补 |
|---|---|---|---|
| need#304 keychain 禁用·测试门控 | 进行中 | `757ce749`（用例级 KEYCHAIN_TEST_GATE + cleanupProfileKey no-op，默认零钥匙串触达） | 【待填：闭合后补】 |
| need#305 keychain 退役·运行时三处迁移 app_secrets（一期） | 进行中 | `ab4b0fdd` | 【待填：闭合后补】 |

## §2 质量底盘演进表

| 时点 | doctest Total / Failed | 事件与证据 |
|---|---|---|
| 2026-10-29 | 1954 / 0 | 全量基线 |
| 批次 A/B 收口 | 2462 / 0 | closure-qa（HEAD=66b7d12e 工作树）；probe 52/52（5 设计内跳过）；当轮 Windows 交叉编译红 1（test_mcp_protocol.cpp `setenv`/`unsetenv` POSIX 依赖）→ 可移植封装修复后消除 |
| 批次 C 收口 | 2462 / 1→0 | closure-qa-c：唯一红=emptyState 旧断言钉 hover 未随 C2 surfaceElevated 契约同步（一行修复）；白卡 Secondary 消失态专项确认→opt-in 描边处置 |
| 批次 E 收口 | 2468 / 0 | E 批增量入账 |
| 批次 F / 杂项后 | 2481 / 0 | 终态 |

终态质量底盘：**唯一已知红 = need#256 钥匙串空账号**（平台层语义，二期消）；UI probe 52/52；守卫链全绿（含 Windows 交叉编译 482/482）；`-Werror` + `[[nodiscard]]` 零告警。

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
| bug#39（#297） | git 过滤空态与干净空态共用基 id「git.list.empty」 | 未处置 | P2 观测：后续探针编写注意；need 时拆独立基 id |
| bug#40（#303） | FolderMissing 折叠摘要落「未播放」泛词；非播放态 chips 亮显并存 | 未处置 | P2：FolderMissing 单列「文件夹不可用」或口径注释；非播放态压暗 chips 可选 |
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
