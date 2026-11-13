# QA 验收报告：git 管理器 v1（need12，L 级全量验收档）

- 执行人：qa-tester ｜ 日期：2026-11-13 ｜ HEAD=1f507ef6（批次四 wip 链顶，EUI-NEO 本地 main=cad42b1 未推）
- 依据：PRD doc 6 v3（slug 2026-11-12-git-manager-readonly，§8 六条+§3 F-ID 卡+§6 UI 契约）+ UI 权威方案 A ui-a.md（重命名行 28px 压缩双行口径）+ 简报已证事实（批次1~4、47/47、probe 深浅 PASS）
- 阶段节点全量档触发（需求全开发完毕）
- 池台账：BUG-18 已入池（P2，untracked diff 空态缺口）；need12 本轮回写「待验收」

## 一、总判定

**半成品（主面通过；缺口列明如下，其余全绿）。**

- 全量档 doctest：**Total 2374 / Passed 2359 / Failed 0**（首轮 7 红=Git 登记后跨模块契约用例期望滞后，S 级直修后复跑全绿）。
- UI probe 段 46/48 绿 + 2 例隔离复跑绿（负载敏感 flaky：bug148_167_sweep titleInk 采样竞态 / jelly A4 压扁 meanDiff 1.59 贴阈抖动，隔离 2.41 PASS）。
- 守卫链四项全绿：probe 跳过登记 / 模块隔离 / Windows 交叉编译（补 libgit2 include 后 462/462）/ 发布形态（单 exe 零外部 DLL）。
- F-09 双夹具 probe PASS：10k 变更仓（9990 条）收敛 3030ms、滚动帧泵 303ms、峰值内存 92.5MB；1GB untracked 仓收敛 2716ms、峰值 95.1MB（大文件未读入内存）。
- 判半成品的缺口（§五）：**BUG-18（P2）untracked 条目 diff 空态语义误导未修**（已入池跟踪，非终态）+ 六类高亮逐类截图被其阻断（md/java 已截图，js/ts/sql/json doctest 间接）+ macOS 全部未实测 + 手测清单未做。

## 二、实操记录（本轮真跑）

| # | 动作 | 结果 |
|---|---|---|
| 1 | `bash scripts/build-test-run.sh` 全量档第 1 轮 | 构建绿（v1.1.5）；doctest **Total 2374 / Failed 7**，链中断于测试段 |
| 2 | 7 红定性+直修（tests/test_core/test_router.cpp ×4 处、tests/test_modules/test_claude.cpp、test_wallpaper_module.cpp ×3 处、test_settings.cpp ×1 处——期望数组/枚举值/默认清单数同步 Git 登记新现实，非凑绿：PageId 序与默认清单含 git 是 PRD 决策④正确产物） | 复跑定向属全量档第 2 轮验证 |
| 3 | 全量档第 2 轮（同口径完整跑） | doctest **Total 2374 / Passed 2359 / Failed 0 全绿**；probe 段 46/48，2 红中断链 |
| 4 | 两红 probe 隔离复跑（直跑 build/tests/*.exe） | **均 PASS**：bug148_167_sweep EXIT 0；jelly A4 meanDiff 2.41435（全量档内 1.58565 贴阈 FAIL）→ 负载敏感 flaky 非真回归 |
| 5 | `bash scripts/build-test-run.sh --no-test` 补守卫链 | probe 跳过登记守卫 ✅ → 模块隔离守卫 ✅ → 交叉编译守卫 ❌（test_platform git TU 缺 git2.h -I） |
| 6 | 交叉编译守卫修复（scripts/check-windows-cross-compile.sh INCS 补 vendored libgit2 include，对齐 libvterm 先例）+ 单跑复验（msys64 工具链） | **462/462 编过全绿**（含 git 测试 TU） |
| 7 | 启动冒烟（build/DevDesk.exe 真启动） | **PASS**：8s 存活、splash 1.2s 切主界面、侧边栏 **16 模块入口**挂载、日志含「启动模块： Git」（router.cpp:111）——F-14 运行时实证 |
| 8 | git_page_probe 深色/浅色各一轮（标准 fixture，链内 probe 段亦各跑过） | **双 PASS**；深色 5 图+浅色 5 图落盘；横滚拖动 x 636→-1632（F-07⑥ 平移实证）；过期条出场+点击重载消失（F-13③） |
| 9 | markdown_probe --screenshot（EUI cad42b1 横滚扩展既有模块回归面） | **PASS**（1200x1000），md_reader.png 落盘目视正常 |
| 10 | F-09 夹具生成（evidence/qa12-fixtures/make_fixtures.sh，系统临时目录） | 夹具1=9990 条变更（9910 untracked+60 M+20 D，`git status --porcelain -uall` 清点）；夹具2=1GB+300MB untracked 单文件（fsutil 秒建） |
| 11 | git_page_probe --repo 夹具1（新增 --repo 注入缝+收敛计时+滚动取证，测试代码） | **PASS**：收敛 3030ms；10 步滚轮+100 帧泵 303ms（30s 冻结线内）；peak WS 92,495,872 B |
| 12 | git_page_probe --repo 夹具2 --no-diff | **PASS**：收敛 2716ms；peak WS 95,145,984 B（1.3GB untracked 在仓，进程内存未涨=不读大文件内容实证）；首轮无 --no-diff 时 ②b FAIL 即 BUG-18 发现现场 |
| 13 | 六类样例仓 probe（make_sample_repo.sh：基线 commit+修改 js/java/ts 三类取证） | **PASS**：diff 截图（git_samples_diff.png）+内容高亮截图（git_samples_content_md.png=b_sample.java）；样例仓 untracked 版首跑 FAIL=BUG-18 根因定位入口 |
| 14 | 干净仓空态 probe（clean_repo，--no-diff） | 截图落盘：中栏「工作区无变更」+「保存文件后列表会自动更新（1s 节流）」——F-03③ 通过（probe 元素断言不适配空态占位，非缺陷，目视核对） |

自报工具调用数：约 60 次（对照预算 90，未超）。墙钟约 1.5 小时（对照 300 分钟，未超）。

## 三、全量档输出摘要（裁尾纪律）

- 第 1 轮：`Total: 2374 Passed: 2352 Failed: 7`（evidence/qa12-fullrun.log）——7 红全为 Git 登记期望滞后：core PageId ordering ×2、claude PageId 序号、wallpaper 表序/默认清单 9→10/往返豁免、app_tools read_settings 豁免。
- 第 2 轮：`Total: 2374 Passed: 2359 Failed: 0`（evidence/qa12-fullrun2.log）；probe `96% tests passed, 2 failed out of 48`（bug148_167_sweep / jelly，均隔离绿）。
- 第 3 轮（--no-test）：守卫链 2 绿+交叉编译红→修复后单跑绿（evidence/qa12-fullrun3-notest.log）。
- 启动段链内未达（两轮分别断于 doctest/probe/守卫红），手补冒烟（§二#7）。

## 四、逐 F 验收对照（PRD v3 §3 矩阵 / §8 六条）

| F-ID | 验收要点 | 判定 | 证据物（evidence/qa12-git-ui/ 下除非另注） |
|---|---|---|---|
| F-01 打开与校验 | ①有效仓→主界面；空态页 | **通过（①+空态）**；②非 git 内联错误/③取消/④加载指示=doctest 数据面间接，UI 截图未取；⑤记忆仓失效未直接取证 | git_main_dark.png（顶栏仓库名+完整路径）；git_empty_state.png（空态页）；probe ①→② pickerSlot 注入开仓动线真跑 |
| F-02 树·全部文件 | ignored 不出现；懒加载；超长名截断 | **通过（截图面）**；首屏<1s 无独立打点（收敛 3030ms 含 status 全量）；展开交互/无权限降级未取证 | git_main_dark.png：树无 build/（.gitignore 命中）、说明文档▶折叠态、超长中文名截断；懒加载=doctest 绿 |
| F-03 树·仅修改 | 空态文案；两模式切换 | **通过** | git_clean_nochange.png：「工作区无变更」+1s 节流提示；「仅修改」Tab 在场（git_main_dark.png） |
| F-04 内容查看 | 行号+内容；占位族 | **部分**：文本内容+行号✔截图；空/失败/超大/二进制占位=doctest 数据面间接，UI 截图未取（树点击 probe 阶段不存在，BUG-18 修复后随样例仓补） | git_main_dark_content_md.png；git_big10k_diff 同源 content 截图 |
| F-05 语法高亮 | 六类着色；回退纯文本；先文本后上色 | **部分**：md✔（标题/粗体/行内码/java 围栏）、java✔ 截图；js/ts/sql/json=doctest 间接；.py 回退未截图 | git_main_dark_content_md.png（md）；git_samples_content_md.png（java）；高亮扫描器 doctest 绿（批次3 已证面） |
| F-06 变更列表 | 四类标签；分组；单击→diff；10k 虚拟化 | **主面通过**；untracked 条目单击 diff 缺口=**BUG-18（P2 已入池）**；D/重命名标签截图未直接目视（doctest 覆盖） | git_main_dark.png（变更(2)/未跟踪(2) 分组+M/U 标签）；git_big10k_changes_scrolled.png（10k 组+条目流）；BUG-18 复现=任意 untracked 条目单击 |
| F-07 diff 查看 | unified 三色+行号；hunk 头；横滚；巨 diff | **主面通过**；hunk 折叠真实点击=手测项；删除文件全删行/新文件全增行截图未取（后者=BUG-18 本体）；巨 diff 分块=doctest 间接 | git_main_dark_diff.png（hunk 头 @@ -1 +1,2 @@、红/绿行号列、增删上下文）；git_main_dark_hscroll.png+拖动 x 636→-1632 实测 |
| F-08 全局搜索 | 决策①：不进 v1 | **范围外核销**（首个增量批次生效） | — |
| F-09 大仓性能 | 10k 变更列表流畅；GB 级 untracked 不 hang 不爆；status 期 UI 可交互；1s 节流 | **通过（①②④）**；③=收敛泵期间帧持续推进（渲染不阻塞）间接证，真实手感手测 | probe_big10k.log（收敛 3030ms/滚动 303ms）；ws_big10k.log（92.5MB）；probe_biguntracked.log（2716ms）；ws_biguntracked.log（95.1MB）；夹具清单 evidence/qa12-fixtures/ |
| F-10 中文路径 | 中文仓/条目/内容/diff 全链 | **Windows 通过**；macOS 未实测 | git_main_dark.png（「示例仓库」仓+「说明文档/接口规范.md」条目+中文长名文件）；git_samples.png（中文目录/说明文件.md untracked 条目在场） |
| F-11 二进制/超大占位 | 双判据占位；占位不读内容 | **数据面通过（doctest）；UI 截图未取**；「占位不读内容」由夹具2 peak WS 95MB 旁证（1.3GB 在仓不涨内存） | ws_biguntracked.log；doctest 47 用例（批次面） |
| F-12 submodule 降级 | 不展开/不误报 | **doctest 通过**（批次四提交 1f507ef6 含降级 doctest）；②probe fixture 内置子仓行=简报已知未验证面 | doctest；probe fixture 未含 submodule 行 |
| F-13 刷新与一致性 | watcher 节流；过期条；失败保旧 | **通过（①③）**；④切走切回保留=手测；②刷新失败保留旧数据未直接取证 | git_main_dark_stale.png（过期条「内容已过期，点击重载」）；probe ②c 点击重载消失输出；节流=doctest+空态截图文案旁证 |
| F-14 模块登记 | 笔记之后「Git」；隔离守卫；入口无断链 | **通过** | src/app_registry.h:51-52（Git 紧随笔记）；启动日志「启动模块： Git」+16 入口；隔离守卫绿 |

PRD §8 六条映射：1 主路径✔（Windows 截图对照 §6 全项；macOS 未实测）；2 次路径部分（md/java 截图✔+其余 doctest）；3 失败路径部分（空态/过期条截图✔，非 git 仓/记忆仓失效=doctest 间接）；4 边界部分（中文✔、二进制/超大 doctest、submodule doctest）；5 性能✔（双夹具 probe）；6 证据物✔（22 截图+3 全量日志+probe/夹具/内存采样日志+doctest 全绿）。

## 五、失败分类与新发现

**失败四分类：**
1. **7 红 doctest（已修复）**＝Git 登记后期望滞后，非产品缺陷——期望同步属验收正确产物（S 级直修+全量复跑绿）。
2. **2 红 probe＝测试缺陷（负载敏感 flaky）**：jelly A4 meanDiff 贴阈抖动（1.59 vs 阈值间 2.41 隔离值）；sweep titleInk=0 采样竞态。非 git 线，隔离双绿。建议（不强制）：两用例加采样前稳态等待或容差带。
3. **交叉编译守卫红＝环境/守卫编法差异**（qa132 BUG-2 同构第二例）：git TU 引 git2.h 守卫上下文缺 -I。**已修**（守卫脚本补 vendored libgit2 include）+复验 462/462。CI 真 CMake 构建不受影响（doctest 全量绿旁证）。
4. **BUG-18＝新回归（产品缺口，M 级处理入池）**：untracked 条目单击→右栏 diff 显「无内容变更」空态，应为全增行（IDEA 对标语义）。根因锚点 src/platform/git/git_repo.cpp:90（INCLUDE_UNTRACKED 无 CONTENT 旗标）+ src/module_git/git_page_controller.cpp:524。**修复约束**：不可无脑加 GIT_DIFF_INCLUDE_UNTRACKED_CONTENT——1.1GB untracked 会被全文读入生成 patch，正是 F-09② 防 GitAhead #161 内存爆场景（夹具2 peak 95MB 依赖现行为）。需 developer 权衡：小文本全增行/大文件二进制走 F-11 占位/目录保持不递归。**已入池 bug#18（P2）**。

**易用性同轨观察（不另开单，随 BUG-18 一并裁决）**：变更列表空态文案「保存文件后列表会自动更新（1s 节流）」把实现细节（节流）暴露给用户，P3 级文案问题，触碰时顺手改。

## 六、遗留待用户项（手测清单，agent 无法替代）

1. 真实鼠标横滚手感（F-07⑥：probe 拖拽已证平移，手感/惯量未测）。
2. tooltip 逐格（超长文件名悬浮全名——截图已证截断，悬浮未测）。
3. hunk 折叠/重试按钮真实点击链（F-07②/F-04③）。
4. F-13④ 切走模块再切回状态保留。
5. status 计算期间真实操作手感（F-09③ 大仓加载中点击）。
6. 1GB untracked 仓真实打开体感（probe 已证不 hang 不爆，体感流畅度）。

## 七、macOS 差异面清单（全部未实测，如实标注）

- 全部 14 个 F-ID 的 macOS 实测：未做（本机 Windows）。含：F-10 macOS 中文路径同仓同结果、macOS 原生材质三档下 git 页渲染、macOS 打包链。
- 既有平台不对称红（非本线）：macOS FileWatcher 轮询兜底 ×2、keychain 空账号语义 ×1——F-13① 外部编辑触发刷新在 macOS 的行为受前者影响，实测时注意。
- doctest/probe 数据面双平台一致由 CI 兜底（本轮未查 CI run——push 未发生，无新 CI）。

## 八、证据物清单

| 证据 | 路径 |
|---|---|
| 全量档 3 轮日志 | evidence/qa12-fullrun.log / qa12-fullrun2.log / qa12-fullrun3-notest.log |
| git 页截图 ×22（深浅主题/空态/干净仓/样例仓/两大仓） | evidence/qa12-git-ui/（清单见目录） |
| markdown 回归截图 | evidence/qa12-md-regression/md_reader.png |
| 夹具脚本 ×3 + probe/内存采样日志 | evidence/qa12-fixtures/（make_fixtures.sh / make_sample_repo.sh / run_fixture_probe.sh / probe_*.log / ws_*.log / stage_*.log） |
| BUG-18 | 池 bug#18（P2，owner=developer） |
| 本轮改码（未 commit，收口归 need#13 终审计） | tests/test_core/test_router.cpp、tests/test_modules/{test_claude,test_settings,test_wallpaper_module}.cpp、tests/probes/git_page_probe.cpp（--repo/--no-diff/收敛计时/滚动取证）、scripts/check-windows-cross-compile.sh（libgit2 include） |

## 九、晨间动作项（跨仓推远端顺序纪律，本任务未推任何远端）

1. 先推 EUI-NEO 本地 main（cad42b1 横滚扩展+90f5c24 等）→ `fork`；
2. 后推 DevDesk → `origin`（gitlink 已指 cad42b1）——顺序反了 CI 会报指向错误文件的编译错。
3. BUG-18 修复后回归：样例仓 probe 全六类逐类截图补 §8.2 完整证据（make_sample_repo.sh 已备好六类基线）。
