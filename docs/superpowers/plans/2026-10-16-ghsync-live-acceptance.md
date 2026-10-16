# GitHub Sync 真机验收场景矩阵（2026-10-16）

基线 `589d8f5`（dev）+ 本分支。PAT 经 `DEVDESK_GITHUB_TOKEN` 环境变量注入（0600 `/tmp/devdesk-gh-test.pat`），全程只进 Authorization 头，未 echo、未写任何仓库文件。复跑：`bash scripts/run-ghsync-live.sh`。

## 基建
- **token 注入缝**：`DEVDESK_GITHUB_TEST_MODE=1` + `DEVDESK_GITHUB_TOKEN` 双变量门（单有后者不生效，防生产误用）→ `loadToken` 优先读环境变量、`saveToken/deleteToken` 变 no-op —— 测试**零触碰真实钥匙串**。
- **引擎驱动缝**：`src/module_settings/github_sync_test_seam.h`（`githubSyncRunForTest` / `githubSyncLocalGitProbeForTest`，模块实例指针桥 + exit hook 同款形态）；**git 探测注入** `localGitProbeOverrideForTest()`（真机 git 恒可用，失败态只能注入）。
- **驱动**：`tests/live/github_sync_live.cpp` = CMake 目标 `github_sync_live`，开关 `DEVDESK_BUILD_LIVE_TOOLS` **默认 OFF**（走 CMake 而非手敲链接：要链 EUI-NEO + freetype/png 全套，手敲是负债）。
- **夹具**：`scripts/ghsync-live-fixture.sh`（远端建删/分支/垃圾库，403·429·5xx 退避）；**入口** `scripts/run-ghsync-live.sh`（一场景一全新临时 `DEVDESK_DATA_ROOT`，跑完即删）；ctest `LABELS "live"`，默认门禁（`devdesk_tests` 直跑 + `ctest -L probe`）看不到。

## 场景矩阵（真 GitHub API，15/15 通过）
| # | 场景 | 结论 | 关键证据 |
|---|------|------|---------|
| 1 | 全新初始化 FirstSync 方向判定 | 通过 | 摘含「首次上传完成」；远端 `notes/init1.md` + `data/database.sql` 齐 |
| 2 | 上传（笔记+库） | 通过 | 远端笔记内容逐字节相等；dump 含本地 `todo_columns` 标记行 |
| 3 | 下载同步 | 通过 | 远端笔记落盘；`dbImported=true`、删掉的标记行随导入回来 |
| 4 | 上传冲突 409 | 通过 | `conflict=true`、覆盖清单 2 项、远端仍是 `REMOTE-B`（未静默覆盖） |
| 5 | 下载冲突 Diverged | 通过 | 远端覆盖本地；拉后再推通过（无死锁） |
| 6 | 分支不存在 | 通过 | `branchHint`「已自动创建」；笔记落在新分支 |
| 7 | 分支存在 | 通过 | 无误报建分支；笔记落既有分支 |
| 8 | 分支被删 | 通过 | 重建 + 笔记回传（1 轮同步自愈，见修复④） |
| 9 | 仓库不存在 | 通过 | 红字「读取仓库信息失败：未找到（HTTP 404）」 |
| 10 | 仓库被删 | 通过 | 404 红字；本地笔记 + 库标记行均无损 |
| 11 | 账户无权限 | 通过 | 红字「读取远端笔记树失败：未找到（HTTP 404）」 |
| 12 | 远端库损坏 | 通过 | 垃圾 SQL→「导入失败：exec: near THIS」；坏 schema→同红；**本地库无损** |
| 13 | 本地 git 可用 | 通过 | 真探测 probe=1，同步正常 |
| 14 | 本地 git 不可用 | 通过 | 注入 probe=0，同步不阻断、笔记照常上推 |
| 15 | 本地 git 版本历史 | N/A | 功能未实现（决策#15 预案未启动），无可测对象 |

## 验收抓出并已修的产品缺陷（4 条，均有先红后绿证据）
1. **`LocalNote` 聚合初始化 move-before-evaluate**（最重）：`LocalNote{std::move(c), contentHash(c)}` 里 hash 算在被掏空的串上 → 基线登记恒为 `hash("")`=`cbf29ce484222325` → `localHashMatches` 恒真 → **本地笔记改动被判 InSync 静默不推**、Diverged 冲突误判 Skip。收口 `gsync::makeLocalNote()` + 单测（`/tmp` 复现程序证实 clang 在 -O0/-O2 均如此）。
2. **分支定向写缺失**：`putFile/deleteFile` 从不带 branch → GitHub 落到**默认分支**；配置非默认分支时读 A 写 B。API 加 `branch` 参数，引擎全链显式传。
3. **branchMissing / FirstSync missing 判定是死代码**：`getFile` 对 404 返回 `ok(exists=false)` 而非 NotFound 错误 → 「首次启用」在空仓恒失败、「自动建分支」从不触发。改用 `exists` 位与 `listTree` 404。
4. **RemoteModified 且远端无文件 → Skip**：分支被删重建后笔记永远不回传（要等 pull 清登记再等下一轮 push）。改 `remoteExists=false → Put`（与 pull「远端已删→本地保留」语义一致），单测钉死。

## 需人工 UI 点验（无头测不了）
- 409 弹窗文案与「拉取远端后再推」两步确认交互流（无头只断言 `conflict=true` + 覆盖清单）；错误详情弹窗「重试」按原动作重跑、日志环形列表滚动
- 卡片红/黄/灰字观感：导入失败红字、git 不可用灰字、退出未同步黄字、分支重建提示；退出补推（60s 窗口 + 5s 等待 + 未推完置黄字）——进程退出路径

## 备注
- 全量单测 `Total 1434 / Failed 1`：唯一失败 `test_claude_service.cpp:2991 settingsFile.is_absolute()` 在**未改动的 dev 主树上同样失败**（环境相关，与本批无关）；github/sync 相关 58/58 绿。一次性仓 `coderstory/devdesk-sync-test-destroy` **保留**（场景 10 夹具，`reset-destroy` 复位、`delete-destroy` 删仓；删了下次跑场景 10 会自动重建）。
