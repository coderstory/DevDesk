# ADR 0006：EUI-NEO 消费政策——同级源码目录引入、不锁版本、每次用最新源码

- **日期**：2026-10-26
- **状态**：Accepted（**2026-10-29 部分修订**：承载形态由同级目录改为仓内 submodule，见文末注记；「内容级守卫为唯一权威判据」的政策本体不变）
- **决策方**：用户拍板（2026-10-25 晚；决策全文见 `docs/plans/开发计划-2026-10-25.md` §五 D1）
- **相关**：`scripts/check-eui-neo-version.sh`（内容级守卫，唯一权威判据）；`.github/workflows/ci.yml`（取框架步骤 `--depth 1` 克隆）；`.scratch/remediation-2026-10/issues/01-eui-neo-policy-cleanup.md`（本 ADR 的落地票）

## 背景

DevDesk 消费 GUI 框架 EUI-NEO。历史上曾用 git rev 做版本对齐：顶层 CMakeLists 设「最低依赖 rev」软校验（rev-parse + merge-base 祖先检查 + 读不到 rev 的 WARNING），CLAUDE.md 同步记录「当前依赖 rev」，CI 侧亦尝试过 pin 固定 SHA。这套口径在 2026-10 被事实击穿：

- EUI-NEO 历史被重写，旧最低依赖门槛 rev 与旧 pin 尝试的 SHA 在现历史中**均不存在**（`git cat-file -t` fatal）——rev 基准蒸发后，CMake 软校验**每次 configure 都误告警**，成为活 bug；
- 文档里指向具体 rev 的记录随之失去可验证性，误导排查方向。

## 决策

1. **引入方式**：EUI-NEO 以**同级源码目录**（`../EUI-NEO`，`EUI_NEO_DIR` cache 变量 + `add_subdirectory`）集成，不转 submodule。
2. **版本策略**：**不锁版本、每次用最新源码**。本地开发取本地 main 最新；CI 取框架步骤 `git clone --depth 1` 拉 fork HEAD——浅克隆拉 HEAD = 取最新源码，这是**政策本身**，不是待修复的缺陷。configure 期**不做**任何基于 git rev 的 EUI-NEO 版本校验（旧软校验块已随本票政策性删除；EUI-NEO 检出 rev 仅作关于页信息展示）。
3. **版本匹配的唯一权威判据**：`scripts/check-eui-neo-version.sh` 的**内容级 API 守卫**——自动扫出 `src/` 下全部 EUI-NEO 消费编译单元（需求侧）、从顶层 CMakeLists 的 `devdesk_add_eui_includes()` 解析 include 路径（供给侧），对每个消费点跑 `-fsyntax-only`。「DevDesk 现在写的每一行代码，在取到的这份 EUI-NEO 上都编得过」即版本匹配的定义；任一侧推导失准 → exit 2 守卫失准，绝不静默绿。

## 为什么不用 rev pin / 硬版本检查

- **rev 基准会蒸发（既往事实）**：pin 与软校验指向的具体 rev 已随 EUI-NEO 历史重写全部失效——在上游历史不稳定的前提下，任何写死 rev 的机制都没有稳定锚点；
- **CI 现行为即政策**：`--depth 1` 拉 fork HEAD 就是要每次用最新源码，加 pin 与政策直接矛盾（浅克隆也够不到历史，祖先关系检查先天不可用）；
- **判据已收敛且更贴本质**：「API 够不够用」才是版本匹配的本体，「rev 新不新」只是代理指标——同一 rev 在不同机器上还可能因未提交改动而不同。内容级守卫直接验证本体，并在构建前红出（exit 1）且指名是框架版本问题、不是消费代码写错。

## 为什么不转 submodule

- 本仓库日常以多 worktree 并行开发，submodule 的更新 / 切换 / 各 worktree 间同步成本高；同级源码目录对现行工作流零额外成本；
- submodule 天然倾向「钉住一个 commit」，与「不锁版本、每次最新」的政策方向相反。

## 影响

- CI（`.github/workflows/ci.yml` 取框架步骤）、本地开发、文档三处口径一致：取最新源码；版本不匹配由内容级守卫在构建前拦下。
- 「全新机器可复现构建」的口径 = **克隆两仓库 HEAD + `check-eui-neo-version.sh` 绿**，不以 pin-sha 为手段。
- 全库不再维护「当前依赖 rev」类记录（CLAUDE.md、策略清单等已改为引用本 ADR 与守卫脚本）；关于页 / 设置页关于文案只展示 EUI-NEO 检出 rev，无「最低依赖 rev」。
- **重审触发**：若 EUI-NEO 转为稳定 tag 发版且历史不再重写，可重估是否声明版本区间——判据仍应以内容级守卫为主，rev/tag 仅作人类可读的辅助信息。

## 2026-10-29 更新注记（用户拍板）

承载形态由「同级源码目录」改为**仓内 git submodule**（`EUI-NEO/`，url 指向 coderstory fork，见 `.gitmodules`），上文「为什么不转 submodule」一节的前提（多 worktree 同步成本）随仓库单根化不再成立，该节论断作废。政策本体保持：configure 期不做 rev 校验、版本匹配以 `scripts/check-eui-neo-version.sh` 内容级守卫为唯一权威判据；gitlink 记录推进基线，升 submodule 指针 = 消费新源码。CI 取框架随之改为 checkout `submodules: recursive`。
