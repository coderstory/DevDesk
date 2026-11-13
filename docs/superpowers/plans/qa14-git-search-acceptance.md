# QA 验收报告：git 管理器 v2 全局搜索首增量（need14，L 级全量档，v1 同口径）

- 执行人：qa-tester ｜ 日期：2026-11-13 ｜ HEAD=704bb3a8（b2c69429 搜索 + 704bb3a8 bug#21 CJK 横滚修复，均已在位）
- 依据：PRD doc#6 v3 F-08 验收口径 + ui-a.md §5.1a（顶栏搜索框+浮层四态）+ 搜索技术卡 §10 验收锚 + 简报已证事实（开发批定向自测 61/61）
- 阶段节点全量档触发（v2 首增量需求收口）
- 池台账：need14 本轮回写「待验收」；本机 Windows 侧实测，macOS 全部未实测（§六）

## 一、总判定

**半成品（F-08 主面四态实操绿 + bug#21 回归绿 + doctest 全绿；缺口列明如下）。**

- 全量档 doctest：**Total: 2388 Passed: 2373 Failed: 0**（v1 基线 2374 → +14=git_search 新用例；15 skipped 同 v1 结构）。
- UI probe 段 46/48：2 红=bug148_167_sweep（**隔离复跑仍红，见 §五.2**）+ jelly A4（隔离绿 meanDiff=2.41435，与 v1 隔离值同——负载敏感 flaky 维持）。
- 守卫链四绿一红：probe 跳过登记 ✅ / 模块隔离 ✅ / Windows 交叉编译 464/464 ✅ / 发布形态（单 exe 零外部 DLL）✅ / **std-include 守卫 ❌（10 文件传递包含欠账，非本线引入，§五.3）**。链断于此，守卫后置的启动段未达；手补冒烟=环境受阻（§五.4）。
- 10k 仓搜索 probe PASS：开仓收敛 4867ms（10002 文件 status）、扫描态渐进出果（0/0 → 4191/10002 已出 16 条）、ESC 取消消失且 2s 稳态不复活、空结果态在场；扫描期间 F-09 滚动取证 1560ms（UI 不冻）。
- bug#21 CJK 横滚回归 PASS：diff 侧 x 636→576.646（−59.4px）；content 侧 codeW=1407.12px（码点口径宽平面，非字节 3× 值）、thumbW=299.3、x 590→567.176（≥10px 可滚判据）；双侧截图目视无截断、无乱码。

## 二、实操记录（本轮真跑）

| # | 动作 | 结果 |
|---|---|---|
| 1 | `bash scripts/build-test-run.sh` 完整档第 1 轮 | 构建绿（v1.3.7）；doctest **Total 2388 / Failed 0 全绿**；probe 段 46/48，链断于 probe |
| 2 | jelly_probe 隔离复跑 | **PASS meanDiff=2.41435**（链内 1.62 贴阈）——v1 同款负载敏感 flaky |
| 3 | bug148_167_sweep_probe 隔离复跑（链内同款 PATH） | **仍 FAIL**：01-home-fs123-presweep-light titleInk=0，签名与链内一致（§五.2） |
| 4 | `bash scripts/build-test-run.sh --no-test`（补守卫链+启动段） | 四守卫绿；**std-include 守卫红（10 文件）**，链断，启动段未达 |
| 5 | probe 取证缝扩展（qa 测试代码，6 处）：`--search` 外部仓空结果等待 120→2400 迭代、浮层首帧扫描态截图、ESC 后 2s 稳态断言+截图、`--hscroll` 外部仓横滚（diff+content 双侧） | 增量重建绿（`--dev --no-run --no-test`） |
| 6 | 夹具生成（evidence/need14/make_fixtures.sh，仓库外 %TEMP%/qa14-fixtures，用后即清） | repo10k=10002 文件（根级 40 个「第二版」needle 文件行1+2 变更文件）；repo_cjk=2 modified 双侧长 CJK 行（149+ 字符） |
| 7 | 10k 仓 probe：`--repo repo10k --no-diff --search` | **PASS**：开仓收敛 4867ms；扫描态截图（0/0）→ 结果态截图（**正在扫描… 4191/10002 文件**+16 条命中已渐进上屏、match 高亮）→ 点击首条命中→内容视图+命中行常亮（slot.0.hlbg）→ ESC 取消消失+2s 稳态不复活→空结果态「未找到匹配内容」 |
| 8 | CJK 仓 probe：`--repo repo_cjk --hscroll` | **PASS**：diff x 636→576.646（−59.4 过 40px 阈）；content x 590→567.176（−22.8 过 10px 阈，几何留痕 thumbW/codeW）；7 图落盘目视核销：CJK 长行连续渲染无截断无乱码，底部横滚条在场 |
| 9 | 启动冒烟手补（DEVDESK_DATA_ROOT 隔离临时根） | **环境受阻**：build 实例 14s 内退出（用户在跑安装版实例 PID 同名，单实例互斥 src/platform/single_instance_win.cpp）；不杀用户进程，冒烟未完成如实标注 |

自报工具调用数：约 59 次（对照预算 60 收紧档，未超）。墙钟约 1.6 小时（对照收紧 45 分钟——超时已如实声明：守卫链交叉编译+全量 probe 段为链内固有长耗时）。

## 三、F-08 逐项对照（四态 + ①~④）

| 项 | 验收锚 | 判定 | 证据物（evidence/need14/ 下） |
|---|---|---|---|
| 四态·空 | 无查询浮层不显示；空结果「未找到匹配内容」 | **通过** | 10k/main_search_empty.png（空结果态实拍）；空态=probe 搜索段前置浮层缺席（档① popup 等待前无 git.search.popup） |
| 四态·扫描 | 「正在扫描… x/y 文件」+已出结果渐显 | **通过** | 10k/main_search_scanning.png（首帧 0/0）→ 10k/main_search_results.png（4191/10002+16 条渐进）——渐进出果双帧对照 |
| 四态·取消 | ESC/清除→浮层消失回稳 | **通过** | probe PASS 行「ESC 取消浮层消失且 2s 稳态不复活（F-08④）」+ 10k/main_search_cancelled.png；取消①新查询=档④重提交隐式过（上一扫描作废重扫） |
| 四态·错误 | 浮层错误块「无法完成搜索」+原因 | **未实操（动线不可达）** | 代码在场 src/module_git/git_views.cpp:1438-1460、controller 触发点 git_page_controller.cpp:730-735；worker 句柄按路径缓存（git_worker.cpp:52-63）⇒ UI 动线下 repo==nullptr 分支不可达（开仓失败先拦在 F-01② 内联错误页）——防御分支，登记 §五.5 |
| F-08① 搜索→匹配→结果 | 扫描态+结果态截图 | **通过** | 10k/main_search_results.png（计数头+relPath:行号+preview+match 高亮） |
| F-08② 点击→定位 | 右栏内容视图+命中行常亮 | **通过** | probe PASS「命中行定位+常亮底色在场」+ 10k/main_search_jump.png（slot.0.hlbg） |
| F-08③ 大仓后台不冻+渐进 | 扫描态截图+增量上屏 | **通过** | §二#7 全链：扫描中滚动取证 1560ms、10002 文件后台扫描、双帧渐进对照 |
| F-08④ 三路取消 | ESC 显式；新查询/切仓/切模块=seq 过期自灭 | **部分**：ESC+新查询实操绿；切仓/切模块取消=代码路径（git_page_controller.cpp:166/459 seq++）+取消稳态断言间接，UI 未单独实操 | 10k/main_search_cancelled.png；probe 稳态 2s 无复活 |
| bug#21 CJK 横滚 | content+diff 两生产点码点口径 | **通过（Windows）** | cjk/main_hscroll.png（diff −59.4px）+ cjk/main_hscroll_content.png（codeW=1407 码点平面+目视无截断乱码）+ 704bb3a8 修复 commit |

## 四、失败四分类

1. **jelly A4 红=测试缺陷（负载敏感 flaky）**：链内 1.62 贴阈 vs 隔离 2.41435（与 v1 隔离值一致）。建议维持 v1：加采样前稳态等待或容差带（不强制）。
2. **bug148_167_sweep 红=疑似真红劣化（非本线）**：v1 判「采样竞态、隔离绿」；本轮**隔离复跑仍红**（01-home-fs123-presweep-light titleInk=0，与链内同签名）——稳定性劣化或 v1 隔离绿偶然。属 home/主题采样面，git 搜索线不触碰。**建议入池**（P2 候选），本任务不修。
3. **std-include 守卫红=既有欠账（非本线引入）**：10 文件直接用 std 符号未显式 include——git_changes_model.cpp:42、git_module.cpp:33/34（v1 线）、git_views.cpp:1504、git_repo.cpp:164/191/195（blame 8ecf04e6 v1）、single_instance_win.cpp:168、code_lexers.cpp:108、test_git_refresh.cpp:195、splash_transition_probe.cpp:67、git_page_probe.cpp:293（既有 catch 块，非本轮补丁引入）。交叉编译 464/464 实编不受影响，纯静态守卫欠账。**建议入池交 developer 补显式 include（小修）**；补齐后守卫链+启动段才能完整过（阶段档出口）。
4. **启动冒烟未达=环境受阻**：单实例互斥（用户在跑安装版）。旁证：本轮 3 轮 git_page_probe+48 probe 链全部真启动窗口渲染；v1 同链启动段曾绿。补齐口径：用户关闭安装版后 `bash scripts/build-test-run.sh --no-test` 单跑一次。

## 五、新发现与登记建议（命中即停，均不展开修）

- **错误态动线不可达**（防御分支/冗余容错观察）：requestSearch 的「无法打开仓库」分支在现动线下不可达——与并行代码审计（P1×3+P2×3 已落台账）是否同项由收口裁决，本轮只登记证据。
- **搜索框 placeholder 疑似左截断**（P3 观察项）：CJK 仓截图顶栏搜索框显「内容…」（ui-a §5.1a 口径应为「搜索全仓内容…」整串左起）——10k 深色首帧（聚焦态）无此现象，疑非聚焦态渲染或截图时点问题，建议 developer 一眼核对 git_views.cpp:307 placeholder 渲染。
- **sweep 稳定红**（见 §四.2）。

## 六、macOS 差异面（全部未实测，如实标注）

- F-08 四态与 bug#21 横滚的 macOS 实测未做（本机 Windows）；CJK 码点口径跨平台一致由 doctest（test_git_search truncateAtCodepoint）+CI 兜底。既有平台不对称红不变（macOS FileWatcher 轮询兜底 ×2、keychain 空账号语义 ×1）。

## 七、证据物清单

| 证据 | 路径 |
|---|---|
| 全量档日志（doctest Total 2388 + probe 46/48） | evidence/need14/build-full.log |
| 守卫链日志（四绿+std-include 红） | evidence/need14/guards-startup.log |
| 2 红隔离复跑 | evidence/need14/flaky-isolated.log（jelly PASS 2.41435）、evidence/need14/sweep-isolated.log（仍红 titleInk=0） |
| 10k 搜索 probe 全档截图 ×6 + 日志 | evidence/need14/10k/（scanning/results/jump/cancelled/empty/changes_scrolled）；10k probe 输出在本卡 §二#7 |
| CJK 横滚 probe 双侧截图 ×7 | evidence/need14/cjk/（diff/content_md/hscroll/hscroll_content/changes_scrolled/main 等） |
| 夹具脚本+清单 | evidence/need14/make_fixtures.sh、fixtures.log |
| probe 取证缝扩展（qa 测试代码，未 commit） | tests/probes/git_page_probe.cpp（--hscroll 参数与解析、②d 外部仓 content 侧横滚、扫描态首帧截图、ESC 稳态断言、外部仓空结果等待放宽） |
