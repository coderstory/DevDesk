# ADR 0007：EUI-NEO 上游策略——fork-and-freeze，不跟随 upstream

- **日期**：2026-10-26
- **状态**：Accepted
- **决策方**：用户拍板（2026-10-25；计划复核 C3 修正 + T8 落地，见 `docs/plans/开发计划-2026-10-25.md` §一 C3 / §二 T8）
- **相关**：`docs/adr/0006-eui-neo-consume-policy-latest-source.md`（消费政策——0006 讲「怎么消费」，本条讲「跟不跟上游」，互为表里）；**策略正文** `EUI-NEO/docs/UPSTREAM-POLICY.md`（框架仓侧，事实表与重估条款以其为准）；`.github/workflows/ci.yml`（取框架步骤）；`scripts/check-eui-neo-version.sh`（内容级守卫）

## 背景

DevDesk 消费的 GUI 框架 EUI-NEO 是 coderstory fork（`origin`，已与本地 main 同步）。fork 点即上游 `sudoevolve/EUI-NEO` main 的停驻处：`782c569`（2026-09-22，「Merge branch 'dev'」）。截至 2026-10-26 实测：

- 上游 main 停在 fork 点已 **34 天（约 5 周），fork 后上游零活动**；
- fork 主线领先 `upstream/main` **125 个提交**（随开发持续增长）；
- EUI-NEO 历史曾整体重写（ADR 0006 记录过 rev 蒸发事实），与上游的提交号对应关系不可依赖。

「不跟随上游」此前只是协作中的事实认知，本 ADR 将其升为成文决策。

## 决策

**fork-and-freeze：不再跟随 upstream。** fork 的 main 是唯一主线，按 DevDesk 需求独立演进；不维护与上游的例行同步通道（不做周期性 merge / rebase `upstream/main`）。框架仓侧的策略正文（含 fork 点事实表与重估条款全文）见 `EUI-NEO/docs/UPSTREAM-POLICY.md`，两文保持互引。

## 理由

1. **上游休眠，跟随没有对象**：上游自 2026-09-22 起零活动；分叉面已 125+ 提交，跟随（或保留将来合并的可能）只会让冲突面随时间单调增长——这正是计划 C3 对原 F-2/P1-4「半年后 rebase 成本指数上升」担忧的修正结论：与其被动积累，不如显式 freeze。
2. **演进源在下游**：DevDesk 需要的框架能力（`setScrollOffset`、`TextRun`、chromeless 等，上游均无）一直由 fork 侧按需求增补；上游并无可「跟进」的能力源。
3. **与消费政策同向**：ADR 0006 已确立不锁版本、每次取最新源码、判据为内容级 API 守卫——这套机制的「最新」本来就定义为 fork 主线最新，upstream 不在链路上。freeze 不改变任何现有机制。

## 与消费政策（ADR 0006）的关系

- **0006 讲「怎么消费」**：同级源码目录引入、不锁版本、每次最新源码、内容级守卫为权威判据；
- **本条讲「跟不跟上游」**：上游已休眠 → fork-and-freeze，不再跟随；
- 两者口径互洽：0006 的「最新源码」= **fork 主线（`origin/main`）最新**，非 upstream 最新；CI `git clone --depth 1` 拉 fork HEAD 与本策略直接一致，取框架步骤**无需任何改动**。
- 重估触发相互独立、不冲突：0006 的重审触发是「EUI-NEO 转稳定 tag 发版且历史不再重写」（消费侧是否引入版本区间）；本条的重估信号是「`upstream/main` 出现新提交」（上游复活，是否恢复跟随）。

## 影响

- **CI 零改动**：`.github/workflows/ci.yml` 取框架步骤维持 `git clone --depth 1 "$EUI_NEO_REPO"`（fork HEAD 即「最新源码」）；其注释「比上游多百余个提交，计数随开发持续增长、不在此写死」与本策略一致——分叉计数只在策略文档里带日期快照记录，不作机制锚点。
- **版本守卫零改动**：`scripts/check-eui-neo-version.sh` 内容级判据不依赖 rev、更不依赖 upstream，freeze 前后语义不变。
- **DevDesk 侧无上游同步例行动作**：不设「同步 upstream」的定期任务；EUI-NEO 的维护口径是本地 main 保持最新并与 `origin` 同步（CLAUDE.md「跨仓库耦合」段既有约定）。
- **与「上游贡献政策」（CLAUDE.md「EUI-NEO 上游贡献政策」段）正交不冲突**：freeze 约束的是 **pull 方向**（不拉取、不合并 upstream 的新提交）；贡献政策约束的是 **push 方向**（框架通用能力修在 `../EUI-NEO`，可选向上游发 PR 反哺社区）。上游休眠不影响后者作为可选动作成立；fork 主线如何演进也只由 DevDesk 需求决定，不取决于上游是否接受 PR。
- **文档口径**：涉及 EUI-NEO 上游**跟随关系**的表述以本 ADR 与 `EUI-NEO/docs/UPSTREAM-POLICY.md` 为准，不再出现「跟随上游」「等上游发版」类预期（`docs/strategy/02-技术能力清单.md`「上游 sudoevolve/EUI-NEO 仅作参考」与本条口径一致）。

## 重估条款

**重估信号：`upstream/main` 出现新提交（即上游复活）。**

信号出现后不自动恢复跟随，重开一次决策：评估上游新内容的价值、cherry-pick 与整体 merge/rebase 的成本（125+ 提交分叉面上的 rebase 成本需实测），再定恢复跟随、按需取用还是继续 freeze。信号出现之前，本策略零维护动作。
