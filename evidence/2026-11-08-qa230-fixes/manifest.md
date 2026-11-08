# qa230 收口修复批——逐 BUG 红绿对照（2026-11-08，developer）

红面基准：clue 182（devdesk:qa230-fullrun-red-attribution）+ evidence/2026-11-08-qa230/（修复前全量，HEAD=aea3945d）。
绿面证据：本目录各日志 + qa230-fix-fullrun.log（修复后全量）。

## BUG-1 test_result 取值口扫描器被探针 DSL 撞车
- 红：evidence/2026-11-08-qa230/qa230-fullrun.log「test_result.cpp:224 report.empty()（35 处）」，35 处全为 tests/probes/layout_guard_probe.cpp 的 G.expect(（need221 kit 断言助手）。
- 修：tests/test_core/test_result.cpp scanForTokens 豁免 tests/probes/ 目录（选目录豁免不选词边界的理由见注释：obj.expect( 与 DSL 助手形态一致，词边界无法区分，会把真实误用一起豁免）。
- 绿：fix-redgreen-result.log 11/11；qa230-fix-fullrun.log doctest Total 2268 / Passed 2260 / Failed 0。

## BUG-2/3 github_sync + wallpaper 契约测试同源化（实现=池219/池151 正确形态，裁决已定）
- 红：qa230-fullrun.log 3 条 github_sync（test_github_sync_card.cpp:368/:493 开关右缘、test_github_sync_card_spacing.cpp:119 行距）+ wallpaper 末格（test_wallpaper_module.cpp:447 格阵）。
- 修（参照 3aef6051 断言同源化做法，钉派生值不写死魔法数）：
  - test_github_sync_card.cpp：启用/自动推送改「同行两半列」契约（池153）——启用开关钉左半列右缘 28+halfW、自动推送钉卡内容右缘 kW−28、label 宽=半列宽−44−8、headBottom 单行派生；
  - test_github_sync_card_spacing.cpp:115-119：旧「启用→自动推送纵向 ≥8」随重排废除，改同行钉 y 相等 + 两开关右缘各自半列（列距 16=池152 口径）；
  - test_wallpaper_module.cpp:439-：列数与生产同源 modulesCardCols(9)=3（池151），格宽 1/3 列等分+gap16 直算、2 列走 columnX，列 x/track 右缘同式。
- 绿：fix-redgreen-githubsync.log 47/47；fix-redgreen-wallpaper.log 85/85；全量 doctest 0 失败。

## BUG-4 缺失标准头（std include 守卫红 3 处全在 tests/probes/）
- 红：qa230-rest.log——layout_guard_kit.cpp:138(std::pair)/:266(std::move) 缺 <utility>；bug150_164_probe.cpp:118 缺 <initializer_list>。
- 修：两文件各补一行标准头 include。
- 绿：fix-std-include-guard.log 812 文件全显式；qa230-fix-guard-chain.log 守卫链全绿含守卫自检好树绿/坏树红。

## BUG-5 uat62 frameHasContent 亮主题白帧误报（13/16 页）
- 红：evidence/2026-11-08-qa230/probe-ctest-LastTest.log:718-748——亮页实测非白占比仅 1–3%（旧阈 <5% 即红）、颜色桶 30–51、黑点 0；直拍证页面完整渲染（产品面无恙）。
- 修：tests/probes/uat62_page_shots_probe.cpp frameHasContent 白帧判据主题自适应——暗页口径保留（非白<5%）；亮页改联合判据「非白<5% 且 颜色桶<10」（真渲染亮页桶 30–51 远超阈，未渲染白窗只有窗口环细线凑不满 10 桶）。
- 绿：qa230-fix-fullrun.log probe 段 uat62_page_shots_probe 全 PASS（不在失败清单，无新 -debug 帧——现存 3 个 debug 帧均为修复前 00:44 旧红遗留）；直拍 uat62-shots/01-home-1000.png、05-claude_proxy-1000.png、14-settings-tab0-1000.png（新拍 01:05-01:06，tab0 即修复前误报页）。

## BUG-6 memory_install_probe ctest 恒 skip
- 现象：裸调用退 77 → ctest 恒 skip。
- 结论：**skip 为设计内，不修注册**——tests/probes/memory_install_probe.cpp:326-335 注释显式声明：本探针归 QA 显式子命令驱动（install 需网络 pip / mcpserve 起真 HTTP 服务，不能进守卫链自动跑），argless 退 2 会令守卫链恒红（池199）；tests/CMakeLists.txt:576 无参注册 + :583 SKIP_RETURN_CODE 77 即该设计的落地面。已入 clue 183。

## 全量终态（qa230-fix-fullrun.log，修复后完整跑）
- doctest：Total 2268 / Passed 2260 / Failed 0（红基准 2255+5 → 0 失败）。
- probe：40/41 PASS，唯一失败=bug142_click_chain_probe（已知红，bug 台账在案）；5 skip 恰为设计内清单（memory_install_probe + 4 wallpaper Windows-only）。
- 守卫链六道全绿（qa230-fix-guard-chain.log）：探针跳过登记 / 模块隔离 / Windows 交叉编译 427 / std include 812 / 玻璃体系 / ToolGuard + 守卫自检（好树绿、坏树红）。脚本因 bug142 已知红在 probe 段 EXIT=1（预期终态），守卫链段经 --no-test --no-run（守卫链仍跑）补跑取证。
