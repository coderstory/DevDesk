# 10: EUI-NEO 上游策略文档（fork-and-freeze）

**What to build:** 把上游策略从「事实认知」升为「成文决策」，两份产出：EUI-NEO 侧 `UPSTREAM-POLICY.md`（放 EUI-NEO 仓库 docs/），DevDesk 侧补一条 ADR（`docs/adr/`，若 01 号票的消费政策 ADR 已立，本票为其增补上游关系章节或独立成篇，保持互引）。内容要点：fork 点 782c569、上游 main 停在 2026-09-22（fork 后上游零活动）的休眠事实、**fork-and-freeze** 决策与理由、消费口径（源码同级引入、不锁版本、每次最新源码，即 D1 政策）、「若 upstream 复活则重估」条款。硬性要求：策略文字与 ci.yml 现行为（拉 fork HEAD）、版本守卫判据（内容级）**一致不自相矛盾**。计划依据：`docs/plans/开发计划-2026-10-25.md` T8 + §五。

**Blocked by:** 01（政策清理完成、ADR 口径定型后才有可「一致」的基准）

**Status:** done

- [x] EUI-NEO 侧 UPSTREAM-POLICY.md 合入（fork 点/休眠事实/决策理由/重估条款齐全）
- [x] DevDesk 侧 ADR 落 `docs/adr/`，与 01 号票的消费政策 ADR 互引不冲突
- [x] 策略与 ci.yml 现行为、守卫判据三处对照过，无自相矛盾
- [x] 合入门槛：9 个 `check-*.sh` 守卫全过（文档票，守卫照跑）

## 证据摘要（2026-10-26）

**上游事实（EUI-NEO 仓实测，非照抄票面）**：fork 点 = upstream/main 停驻处 `782c569`（2026-09-22「Merge branch 'dev'」）；fork 侧首个提交 `bb5c965`（同日 2026-09-22）；截至 2026-10-26 上游零活动 **34 天（约 5 周）**；fork main（= origin/main，`3c8c7ca`）领先 upstream/main **125 提交**。

**产出**：

- EUI-NEO 侧 `docs/UPSTREAM-POLICY.md`（该仓 main 提交 `a2e2064`，未 push）：事实表 + fork-and-freeze 决策与三条理由（上游休眠 / 演进由 DevDesk 需求驱动 / rev 对应关系因历史重写不可依赖）+ 与 DevDesk 消费政策（ADR 0006）的关系（「最新源码」= **fork 主线最新**，非 upstream）+ 重估条款（信号 = `upstream/main` 出现新提交，重开决策不自动跟随）。
- DevDesk 侧 `docs/adr/0007-eui-neo-upstream-fork-and-freeze.md`（分支 `wt-10-upstream-policy`，基于 dev `cb787cab`，未合并未 push）：与 0006 互引（0006 讲「怎么消费」、本条讲「跟不跟上游」，重估触发相互独立不冲突）；写明与 CLAUDE.md「EUI-NEO 上游贡献政策」段的正交关系（freeze 管的是 pull 方向，贡献 PR 是 push 方向的可选反哺，互不矛盾）。

**一致性三对照（四处互洽）**：① `ci.yml` 取框架步骤 `--depth 1` 拉 fork HEAD =「每次用最新源码」政策本身，与本策略直接一致、零改动；其「比上游多百余个提交，不在此写死」注释与策略文档「带日期快照记录计数、不作机制锚点」的写法一致。② `scripts/check-eui-neo-version.sh` 内容级判据不依赖 rev / upstream，freeze 前后语义不变（本次实测对准含策略文档的 EUI-NEO `a2e2064`，62 个消费编译单元全绿）。③ ADR 0006 消费政策（不锁版本、每次最新）与 0007 无冲突。④ 两份新文档口径互引一致。

**守卫与测试**：门槛守卫全过——7 个无参 `check-*.sh`（consistency / module-isolation / no-glass / probe-skip-gate / std-includes / toolguard-braces / windows-cross-compile）+ `check-eui-neo-version.sh --root . --eui-dir ../EUI-NEO`（exit 0）。第 9 个 `check-cumulative-markers.sh` 在 dev tip（`cb787cab`）对 CRLF 行尾敏感为既有红，修复在 09 号票分支（`50f15727`，改守卫脚本本身）在飞，非本票范围；主树该分支上 60/60 绿可证。`--no-run` 一条龙：configure / 构建 / 发布形态守卫全绿。ctest：Total **1586** / Failed **7**——7 条全部落在禁区文件对应套件（splash_contract ×5、window_geometry 生产接线 ×1、markdown 主体面 ×1），为 dev HEAD 既有问题、03/04/05 号票并行在飞修复中（主树 dirty diff 可证：`tests/doctest.h` 新增 `test_temp::repoRoot()` 修 6 条 `__FILE__` 相对路径假红；`test_markdown.cpp` 批M 拍板 `wc.background→wc.hover` 期望更新修 1 条），与本纯文档票无关；基线 Total 1586 / Failed 0 即主树含在飞修复态。probe 门禁段 3 失败（agent_e2e / home_probes / home_probes_min_window，真实开窗几何断言）为环境性差异，按惯例忽略。
