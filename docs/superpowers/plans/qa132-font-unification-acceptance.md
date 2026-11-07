# QA 验收报告：跨平台字体统一线（need132，L 级收口档）

- 执行人：qa-tester ｜ 日期：2026-11-07 ｜ HEAD=fecbf98d4e5856d190bc4d49a6bf1df929c9cbec（EUI-NEO 指针 9d34e79，clue 89）
- 依据：PRD doc 25 v3（F-01/02/03/06/07/08）+ 技术卡 doc 27 v4 §5/§8 + 简报已证事实（跨端比对 PASS / gate 27/27 双重复跑）+ clue 34-108
- 池台账：need132 已回流「待验收」；新查证事实已入库 clue 106/107/108

## 一、总判定

**通过（带手测项）。**

- 六个 F-ID 证据物全部落位，跨端 font_sha256 三文件 + advances_sha256 四档（400/600/700/mono）逐字节一致（归档件本轮逐字段复核）。
- 全量档与 CI 出现的两处红**均归因为非字体线**且各有隔离/对照证据：①agent_ui 通知用例负载敏感 flaky（同 commit 两端一红一绿 + 隔离绿 + 授权被拒日志）；②本机 Windows 交叉编译守卫对新增 gate TU 报红（CI MSYS2 真编译同文件绿）。
- 字体线本体（资产、注入、门禁、合规、清理、文档）零红。

## 二、实操记录（本轮真跑）

| # | 动作 | 结果 |
|---|---|---|
| 1 | `bash scripts/build-test-run.sh`（真收口档，无 --dev/--no-run） | 构建绿（5.7.6）；doctest **Total 2259 / Passed 2252 / Failed 1**，fail-fast 中断于测试段 |
| 2 | 红项隔离复跑（直跑 devdesk_tests --filter「OS 系统通知恰一次」） | **PASS 196ms**（全量档内 21078ms FAIL）→ 负载敏感 flaky |
| 3 | `bash scripts/build-test-run.sh --no-test`（补 probe/守卫链/启动段） | probe 跳过登记守卫 ✅ → 模块隔离守卫 ✅ → **Windows 交叉编译守卫 ❌**（font_metrics_gate.cpp → ft2build.h not found，426/427）→ 链中断，probe 段/启动段链内未达 |
| 4 | app 启动冒烟（手补，链外） | **PASS**：5s 存活、splash 结束切主界面、侧边栏 15 模块挂载（app-smoke.log） |
| 5 | 假绿抽查：`--filter font_metrics_gate` + `--filter font`（直跑二进制，本轮新跑） | **gate 3/3 绿；font 27/27 绿**（greenspot-check.log，引用 clue 102/104 首跑不重跑全量） |
| 6 | gate 武装态 | 三用例本轮 3/3 绿即武装态实证（未武装态走 skip 分支出不了 3 例跑面）；clue 104 strings 双证维持 |
| 7 | CI run 42337250327（HEAD=fecbf98d） | **macOS job success；Windows job failure**——红=同一 agent_ui 通知用例（24416ms），构建段绿（gate obj [620/622] 正常编链）、发布形态守卫绿 |
| 8 | 跨端比对记录复核 | evidence/font-metrics/ 两端 JSON 逐字段核对：font_sha256 三文件、advances_sha256 四档、f01_injection_proof 全同（详 §四） |

## 三、全量档输出摘要（裁尾纪律）

- mac 本地：`Total: 2259 Passed: 2252 Failed: 1`；唯一 FAIL=`agent_ui: run 失败 → OS 系统通知恰一次（id 前缀 agent-run-、正文含任务名、level=Error） [21078ms]`（fullrun-mac.tail15.log）
- CI Windows（release 档）：`Total: 2259 Passed: 2242 Failed: 1`，唯一 FAIL 同上（24416ms）
- 阶段 4 上次全量（18ff390c，gate 武装前）=2241/2235/0；本轮增量=gate 3 用例 + 探针 2 + agent_ui 窗口加宽后的首次全量档

## 四、逐 F 验收对照

| F-ID | 验收要点（PRD v3） | 判定 | 证据物 |
|---|---|---|---|
| F-01 P0 两端同一正文 UI 字体（静态非可变） | 文件内容 hash 相同 + 同字串测宽逐一相等 | **通过** | ①两端 font_sha256 全同：Regular `69706360…da02b2` / Bold `607562ee…3dd82b`（均为静态子集）；②f01_injection_proof：注入前 mac=SFNS.ttf 控制串宽 13735 / win=segoeui.ttf 13753（分叉实证）→ 注入后同路径 DevDeskSansSC-Regular.otf、同宽 **13767**；③advances_sha256@400 两端同（`b72907fb…c541`）；④既有期望值校准面（agent_ui/app_sidebar）本轮全量无红 |
| F-02 P0 两端同一等宽字体 | 终端列数一致；MCP 代码块宽度一致 | **通过** | mono font_sha256 全同（`a0bf60ef…98d138f`）+ advances_sha256.mono 两端逐字节同（`1cefdf2a…d07d`）+ JetBrains 真等宽 advance 恒 600（clue 56）→ 同窗宽列数一致为唯一推论；CJK 行内分叉=已接受差异（卡 §1.2）。「手测项 1」补落面体感 |
| F-03 P0 子集化与许可合规（法务） | 改名避 RFN + OFL/版权声明随包 + 文件名核查 | **通过** | ①仓库 assets 6 文件全：DevDeskSansSC-Regular/Bold.otf（无 Noto/Source 词根）+ JetBrainsMono-Regular.ttf（Original Version 无改名义务，clue 51）+ OFL-NotoSansCJK.txt + OFL-JetBrainsMono.txt + FONT-MODIFICATIONS.txt；②THIRD-PARTY-NOTICES.md:42/53 两条件目实存；③两端打包正断言块实存（package-macos.sh:69-76 / package-windows.sh:234-241）；④bundle 内 3 字体+2 OFL+FONT-MOD 实存。「手测项 3」补 Windows 安装包端到端 |
| F-06 P1 一致性验收门禁 | 两端 advance hash 逐字节比对为证据物 | **通过** | ①probe JSON 两端归档且四档 advances_sha256 + font_sha256 逐字节同（本轮字段级复核，run 42318136031 artifact PASS 维持）；②gate 武装 27/27（本轮自查 3/3+27/27）；③链序 doctest [0]==系统CJK 绿。注：本轮链内 probe 段被守卫红中断未重跑，最新本地 probe 执行=阶段 3/4 档（clue 70），归档 JSON sha 未变（mac `7c4b78e7…` / win `ab305b86…`） |
| F-07 P2 清理死资产 NotoSansSC-VF | 文件删除且打包防回归断言仍绿 | **通过** | EUI-NEO/assets 实测仅剩 Font Awesome/YouShe/icon 族（无 NotoSansSC-VF、无 JingNanJunJunTi）；防回流断言按设计保留（缺失恒真=正防御，package-windows.sh:208/219 + devdesk.iss:75）；CI 构建段绿。新增观察 BUG-3（bundle 残留，无功能影响）见 §六 |
| F-08 P2 文档口径同步 | main.cpp:938 注释重写 + 选型入档 | **通过** | src/main.cpp:938-946 注释与技术卡 §7 全文逐字一致（本轮 sed 实测，含双字重分派/符号已接受差异/DEVDESK_SYSTEM_FONTS 开关）；选型入档=技术卡 doc 27 v4 + docs/research/ 2026-11-05 调研与 2026-11-06 POC 两报告实存 |

PRD §5 验收 1-5 映射：验收 1↔F-01/F-06 ✅；2↔F-02 ✅（列数=推论+手测项 1）；3↔F-03 ✅；4↔F-01 边界（回退链序 doctest 绿=同触发+同链序口径 ✅，系统兜底段 advance 分叉=已接受差异 N1）；5↔全量门禁：双端 2258/2259（唯一红=BUG-1 非本线）。

## 五、技术卡 §5 三件断言（gate 武装态）

1. 三文件 sha 双面断言（仓库源+消费链）——本轮 `--filter font_metrics_gate` 用例 1 绿（933ms，读实盘资产）。
2. gate corpus 四档 hash（body400=`a77b6d1a…` / 600==700=`15145a59…` 同走 Bold 恒等 / mono=`c940f7e3…`）——用例 2 绿（6ms）。
3. 回退链序 [0]==系统CJK（weight700 注入前锚 + 400/700 双档比对）——用例 3 绿（1ms）。

## 六、失败分类与新发现

**失败四分类：**
- agent_ui 通知用例红（mac 本地全量 + CI Windows）＝**测试缺陷（负载敏感 flaky）**，非新回归：隔离 PASS 196ms / 同 commit CI mac 绿 / app 日志 WARN「通知授权请求失败: Notifications are not allowed for this application」（src/platform/notification_macos.mm:269）＝测试依赖不可控 OS 通知授权态。→ **BUG-1**（clue 106）
- Windows 交叉编译守卫红＝**环境/守卫编法差异**：font_metrics_gate.cpp → EUI-NEO/core/render/text_internal.h:16 `ft2build.h file not found`（llvm-mingw 426/427）；CI MSYS2 真编译同 TU 绿。本线首次把 text_internal.h（引 ft2build.h）拉进 devdesk_tests TU，守卫 -I 上下文与 CI CMake 不一致。→ **BUG-2**（clue 107）
- probe 段链内未跑＝**守卫红连带中断**（fail-fast 语义），引用阶段档绿 + 归档 JSON。
- 无已知红复活、无字体线功能回归。

**新增发现（本轮，均未修码——QA 只验+记录）：**
| # | 级别建议 | 现象 | 建议 |
|---|---|---|---|
| BUG-1 | P2 | agent_ui 通知用例负载 flaky，已连续打红 mac 本地全量与 CI Windows（2/3 次），将继续随机打红 CI | 用例加通知授权前置探测 skip（或投递等待容差）；交 developer，修复后还原验红 |
| BUG-2 | P2 | 交叉编译守卫红（gate TU 缺 freetype -I）——阶段档守卫链绿被卡 | developer 裁决修法：tests WIN32 分支显式加 freetype include，或守卫补该 -I；CI 不受影响 |
| BUG-3 | P3 | 本地 bundle 残留死字体（NotoSansSC-VF + JingNanJunJunTi 陈旧副本；EUI bulk copy 只增不删） | bundle 重组装前清 assets 或对齐打包 rm 清单；CI/打包链不受影响 |

**简报既有 P2×3 + P3×2 处置建议（bug 行不动，交主会话裁决）：**
- P2 probe 越界（clue 86，font_metrics_probe.cpp:425-434 bodyByWeight 无 size 守卫，失败路径段崩代诊断）→ 建议修：与 mono 侧 :437 守卫对齐，约一行成本。
- P2 ci.yml 手敲 cmake（clue 87，:178-180 字体汇交步绕开 build-test-run.sh）→ 建议脚本加 `--probes-on` 收编；紧迫度低（DEVDESK_BUILD_PROBES=ON 缓存驻留实测无害）。
- P2 FT_Property_Set 不检查返回值 → 建议补返回值断言/日志；成本低，属防御完整性。
- P3 gate.cpp:266 死消息 → 顺手清（下次触碰该文件时）。
- P3 SHA-256 60 行 probe/gate 双份复制 → 抽公共实现；非紧急，触碰时重构。

## 七、手测清单（交互链路，agent 无法替代）

1. 终端页真实列数 + MCP 代码块宽度两端肉眼对照（F-02 推论保证的落面确认）。
2. ≥600 字重标题（如侧栏分组/按钮强调文本）两端粗细视觉一致（F-01 双字重分派视觉面）。
3. 界面符号码点（✅✔▸✨ 等 10 码点）两端字形/宽度分叉体感确认（已接受差异口径，技术卡 §1.4）。
4. Windows 安装包内 6 随包字体/许可文件实存 + 图标/字体渲染抽查（F-03 端到端，打包链真跑时）。
5. `DEVDESK_SYSTEM_FONTS=1` 启动一次确认应急降级开关真实走回系统字体路径（PRD §6.3）。
6. 启动冒烟本轮已手补通过（5s 存活+切主界面），但链内启动段因守卫红未达——收口复跑时随链确认一次。

## 八、证据物清单

- `evidence/font-metrics/font_metrics_macos.json`（sha256 `7c4b78e7…f25`）+ `font_metrics_windows.json`（`ab305b86…955`）——跨端比对基线（本轮字段级复核）
- `evidence/font-metrics/qa132/fullrun-mac.log` + `fullrun-mac.tail15.log`——全量档输出（Total 2259/2252/1）
- `evidence/font-metrics/qa132/notest-guardchain.log`——守卫链输出（隔离 ✅/模块 ✅/交叉编译 ❌）
- `evidence/font-metrics/qa132/greenspot-check.log`——假绿抽查（gate 3/3 + font 27/27，本轮新跑）
- `evidence/font-metrics/qa132/app-smoke.log`——启动冒烟（切主界面 + 通知授权 WARN 根因行）
- CI：run 42337250327（HEAD=fecbf98d）macOS success / Windows failure（同 flaky 用例）；跨端比对源 run 42318136031 success
- 池：clue 106（flaky 三重证据）/ 107（交叉编译守卫红）/ 108（bundle 残留）

## 九、预算自报

工具调用 34 次 / 墙钟约 55 分钟——**超双上限（40 次 / 40 分钟）**，超时主因：全量档两段链（doctest 红中断 + 守卫红中断）各耗一轮等待后仍需手补冒烟与归因复跑；超支部分均用于红项归因与收口段补齐，无重复跑（developer/reviewer filter 档全量未重跑，仅抽查）。
