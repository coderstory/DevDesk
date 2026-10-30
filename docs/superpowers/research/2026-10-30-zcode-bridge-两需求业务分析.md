# zcode bridge 两条需求 · 业务分析与排产建议

日期：2026-10-30 ｜ 主笔：惨品（业务）｜ 技术协作：老学究 ｜ 状态：调研稿，报老板娘定排产，不动码

## 需求一：ZCode CLI 安装配置接入 bridge（标准化接入）

**一句话**：把「装 CLI → 登录 → 两层 patch 配置 → 验证连通」的人工流程沉淀成标准化接入文档 + skill，新机/重装零踩坑。

**量级**：S——纯文档+skill 包装，零代码。素材现成：小萝莉的 zcode-dsh-setup skill 已带全套踩坑速查（信任前置/默认模型/-p 模式行为），查重后是「整理+补 bridge 差异项」不是重写。
**依赖**：bridge 本体可用（见需求二的 bug 不阻塞接入，阻塞的是体验稳定性）。
**风险**：低。唯一注意 = 两层 patch 配置是宿主升级易碎点，文档必须写「升级后重验清单」。
**排产建议**：先做——半天级，且是需求二的前置（fork 修复后的验证要靠标准化接入环境）。

## 需求二：fork bridge 源码自修

**一句话**：bridge 三类实锤问题（stop 懒检查、reasoning 跨 delta 过滤漏、drain 路径不一致，复现命令在 BUG 清单）fork 自修 vs 等 upstream 的取舍。

**量级**：M——三处都是流式管线行为 bug，单点修复不重架构；但缺上游测试护栏，每修一条都要自建「先红后绿」复现用例，成本主要在这里。
**依赖**：需求一的标准化环境；本地仓已在 `/Users/coderstory/dev/dsh-zcode-plan-bridge`（含 cordis.patch.yml 补丁层）。
**风险**：中——① fork 后背负 upstream 同步税（上游更新要手工 rebase 补丁层）；② 补丁层（cordis.patch.yml）与源码修的边界要先定：能 patch 配置解决的绝不改源码，改源码的要在 fork README 记账；③ 修错流式管线会把「偶发丢字」变成「稳定丢字」，每条修复必须带真实会话回归。
**成本对比（业务判据）**：stop 懒检查直接影响无人值守可靠性（僵尸会话占额度）——这是业务痛点，等 upstream 无期；reasoning/drain 是体验毛刺。建议**分级 fork**：stop 懒检查必修自扛，其余两条先量化出现频率（跑一周 zcode 单统计），高频才修，低频挂池等 upstream。
**排产建议**：需求一落地后开 fork 修复单，M 级，走 zcode 通道+QA 变异；upstream 同步税单独立债条目挂池。

## 非目标
不动 DevDesk 主仓代码；bridge 仓改动全部走独立 fork 分支；不给 upstream 直接提 PR（先自用验证稳定）。
