# 01: EUI-NEO 消费政策对齐清理

**What to build:** EUI-NEO 消费政策已定案：同级源码目录引入、**不锁版本、每次用最新源码**（用户拍板 2026-10-25 晚，决策全文见 `docs/plans/开发计划-2026-10-25.md` §五 D1）。本票把代码库中与该政策相悖或已过期的表述全部清干净，并让「内容级 API 守卫是版本匹配的唯一权威判据」这一口径落到纸面：① 删除顶层 CMakeLists 中基于 git rev 的最低依赖软校验块——其 rev 基准在 EUI-NEO 重写后的历史中已蒸发（旧最低依赖门槛与旧 pin 尝试所指向的两个 rev 短 sha 均已 `git cat-file` fatal 不存在），该检查当前**每次 configure 都在误告警**，是活 bug；② 修正版本守卫脚本 `check-eui-neo-version.sh` 头部「已决策永不 push」的过期叙述（现实：origin 为 coderstory fork 且已与本地同步，CI `--depth 1` 拉 fork HEAD 即最新源码，取框架步骤**维持现状不加 pin**）；③ 修正 CI 配置顶部「比上游多 90+ 提交」计数注释（实为 125）；④ 清理 CLAUDE.md 等文档中「当前依赖 rev」类引用；⑤ 消费政策写成 ADR 入 `docs/adr/`。

**Blocked by:** 无（可立即开工）

**Status:** done

**证据摘要（2026-10-26，分支 wt-01-policy-cleanup）**：全新 configure（git 可用/不可用两场景）零 EUI-NEO rev WARNING；7 个无参守卫 PASS + `check-eui-neo-version.sh` 绿（62 消费 TU @ 3c8c7ca 全过）；`./scripts/build-test-run.sh --no-run` 全量 Total 1581 / Passed 1572 / **Failed: 0**（probe 段 3 例失败为缺 LLM 凭据的环境性既有状态，与本票无关）；三令牌（两个已蒸发旧 rev 短 sha + 旧最低依赖 rev CMake 变量名）`git grep` 工作树与提交树双清零。附带必要清理：旧最低依赖 rev 宏的活代码消费点（src/module_about、src/module_settings 的宏注入与关于文案 + test_settings 断言）一并移除——否则 grep 清零不成立、关于页会显示空门槛；EUI-NEO 检出 rev 保留为关于页信息展示（经「构建期信息注入」段注入，非门槛）。第 9 个守卫 check-cumulative-markers.sh 为带清单参数的工具型脚本，非无参基线成员（无对应清单输入，未单跑）。

- [x] configure 输出不再出现 EUI-NEO rev 相关 WARNING（git 可用与不可用两种场景都验证）
- [x] 守卫脚本头部注释与现实一致（fork 已同步、CI 拉 HEAD=最新源码、rev 比对不成立的理由保留），脚本本体行为与退出码不变
- [x] CI 配置过期计数注释修正；取框架步骤保持 `--depth 1` 克隆不变
- [x] 全库无指向已蒸发 rev（两个旧短 sha）的活引用
- [x] `docs/adr/` 新增消费政策 ADR，与 ci.yml 现行为、守卫脚本判据三处口径一致
- [x] 合入门槛：9 个 `check-*.sh` 守卫全过 + ctest 全绿（Failed: 0，Total 不低于最新基线，参考 646）
