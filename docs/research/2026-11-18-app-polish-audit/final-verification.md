# need#329 打磨专项终验报告（final-verification）

- 日期：2026-11-18｜角色：qa-tester（终验波次=本专项最后一次全量验证）
- 验证对象：HEAD `8ffa1821`（工作树含 `674c1875` 明示留存的 wallpaper_ui.cpp/test_wallpaper_ui.cpp need310/311 归位前 hunks，提交信息有记录，非未知脏改；与上轮 2483 全量口径一致）
- 预算：45 次工具调用 / 75 分钟（实际见文末）

## ① 完整档（唯一全量执行档）

命令：`bash scripts/build-test-run.sh --no-run`（完整档，未带 --dev）→ **EXIT=0**，日志 evidence/2026-11-18-need329/fullrun.log。

| 关卡 | 结果 |
|---|---|
| keychain 编译硬禁令前置门（need#327） | ✅ src/ tests/ 零引用 |
| 构建 | ✅（bundle 已同步 build/DevDesk.app） |
| doctest 全量 | ✅ Total **2486** / Passed 2479 / **Failed 0** / Skipped 7（DOCTEST_SKIP 可见跳过族，need#304 口径） |
| UI probe | 52/57 跑绿；5 跳=设计内（b1_material/home_probes 屏装不下 1000 逻辑高采样窗、memory_install_probe 显式子命令驱动 argless 即退 77，均复跑验证退出码 77），跳过≠通过已登记 |
| 探针跳过登记守卫 | ✅（77 登记齐全） |
| 模块隔离守卫 | ✅ |
| Windows 交叉编译守卫 | ✅ 486 源文件全过（上轮 482 → 本轮 +4 为新增文件） |
| std include 守卫 | ✅ 915 文件 |
| 玻璃体系 / ToolGuard 守卫 | ✅ |
| 守卫自检 | ✅ 5 道守卫好树绿/坏树红，非恒真 |

## ② 批审四结论（合并审计，git show 窄读）

1. **#309 摘牌（7da441d7）— 过**。`unregisterAppDatabase()` atomic exchange 置空、幂等、只在真摘过时留痕；atexit 钩子注册点=registerAppDatabase 之后即刻（main.cpp），按 C++ atexit/静态析构逆序规则：晚于后注册的模块 stopAll join 钩子执行、早于 db 静态析构——LIFO 时序断言成立。顺序探针 database_unregister_before_background_join 四阶段复刻生产关闭序（注册态读→摘牌→连接析构→迟到消费按「未注册」降级），删摘牌必红（UAF/ASan）。观察项（非条件）：`std::_Exit(1)` 路径不走 atexit，属崩溃族本就无序可保，范围外。
2. **#318 wecom 判据（983d8f61+b3fd954a）— 过**。wecom deliver 三态分流：NotFound/空值→未配置；InvalidApiKey（值形态坏）→请重录；其余（Internal）→瞬时读取失败不固化终态；status 恒 missing_secret 引擎分类不变。闭环验证：posix/win 两平台 realUnprotect 各仅一处错误码分支且均为 InvalidApiKey（DPAPI blob 解不开），无其它 Internal「解不开」漏判分支；secrets_store 原样上抛 unprotect 失败。测试锁两态文案+坏形态夹具不外泄断言。
3. **#319 空断言（c480847c）— 过**。防恒真双护栏齐：①非 equals 的空串拒（contains 空文本恒真）②空串判据必须带定点 id（全页空文本扫描恒真拒）。schema 层区分「缺 text 字段」（拦）与「显式 text=""」（放行），checks 层行为同构，5 态用例锁（清场绿/非空红/contains 拒/全页拒/缺元素拒=元素消失≠字段为空）。input placeholder 写入 .text 的恒红 caveat 已落注释（EUI-NEO input.h:495）。
4. **754c4531 三层透传（need#328）— 过**。Swift 侧 `workingDirectory: String?` 缺省 nil=上游默认 cwd；C++ 侧 resolveSpawnWorkDir（非空原值/空→$HOME）为唯一解析口，`dir.empty() ? none() : some(String(dir))` 构造正确——none 仅当连 $HOME 都缺失时兜底上游默认。terminal_backend_mac workDir 从 `(void)忽略` 改实接。单测锁解析语义 + spawn e2e 真会话 pwd 落盘比对（/tmp canonical 处理正确）。编译与 e2e 由本轮全量再证。

## ③ 视觉补拍（evidence/2026-11-18-need329/）

| # | 文件 | 内容 | 判定 |
|---|---|---|---|
| 1 | theme-light/15-settings-1000.png | 设置页亮档：外观卡主题三档行（主题=跟随系统 dropdown 可见）+ 字号缩放 + 窗口效果 | ✅ 目检通过 |
| 2 | theme-sysdark/15-settings-1000.png | 设置页暗渲染（跟随系统×系统暗色解析；系统暗色下探针 EXIT=0 帧判据过） | ✅ 目检通过 |
| 3 | wallpaper/library-empty-1000x640-light.png | FolderMissing 片库空态：摘要专词「**目录不见了**」（bug#40 前半实拍） | ✅ 目检通过 |
| 4 | git-states_05_filter_empty.png | git 过滤无匹配空态：独立基 id 空态 +「试试更短关键词，或点 × 清空」guide（bug#39 实拍） | ✅ 目检通过 |
| 补 | wallpaper/play-video-1600x1000-light.png | 播放态主界面（正在播放条/进度/12 个视频） | ✅ 补充 |
| 补 | git-states_01..09（除 05 外 8 张） | git 探针空态/两栏/平铺/过滤激活/干净仓/下拉/diff/组头折叠等 | ✅ 补充 |
| 补 | theme-light/09-wallpaper-1000.png | 壁纸页亮档整页（已暂停让路横幅） | ✅ 补充 |

**如实记欠账**：
- **纯「深色」选项选中态未拍**：批拍探针仅亮/暗两档（UAT62_DARK env），且 need#321 落地后设置模块 compose 时按配置（缺省跟随系统）重解析主题，env 强制 setDark 被覆盖（theme-dark/ 首拍实证：暗 env 出亮渲染，已删废片）；暗渲染证据改由系统暗色（osascript 切换、拍后即还原浅色）获得。选中态切换需预置配置库，现役探针不支持。
- **chips 压暗展开态未拍**：让路条件 chips 在「这些情况会让路」行展开后渲染，现役探针无行展开镜头；压暗口径以 wallpaper_ui.cpp:813 注释与断言为准（bug#40 回写已注明）。
- **wallpaper_probe --shots-dir 模式新发现 1 条断言红**：§3c library-empty-1000x640 档「页面宽应为 eff−208」实测 1780（eff=2000，差 12px，疑窄窗滚动条 gutter 与常数口径不一致）。默认 CTest 档（全量 52/57 内）无此路径、全绿——shots-dir 专属断言，**记录不修**，交 developer 裁决断言常数或窄窗自适应归属（测试缺陷/布局口径二选一）。

## ④ 收口与推送

- FINAL-REPORT.md 回填四处：收口说明、§1 收官批矩阵（need#304–#328 共 23 行，#312/#323 提交族零命中不列防发明）、§2 质量底盘终验行+终态段（need#256 消号）、§3 bug#39/#40 已收口闭行。
- 推送前清单与结果见文末附录（由终验波次执行记录回填）。

## 结论

**完成**（终验四轴：完整档 EXIT=0 + 批审四件全过 + 视觉 4 主证 7 补证 + FINAL-REPORT 回填+推送）。半成品项（不阻塞专项关闭）：chips 压暗展开态与纯深色选中态两处视觉欠账（探针能力缺口，已有代码/测试断言兜底）、wallpaper_probe shots-dir 断言红一条待 developer 归属。

## 附录：实际执行记录

- 全量数：2486/0（跳 7）；probe 52/57；守卫链全绿；EXIT=0。
- 视觉计数：主证 4 + 补证 7 = 11 张（≥4 达标）。
- 工具调用：33 次（预算 45 内）；墙钟 ≈45 分钟（预算 75 内）。
- 推送：`git push origin feat/notepad-batch1:dev`（带代理 https_proxy=http://127.0.0.1:7897），推送前 origin/dev..HEAD 清单见下。
