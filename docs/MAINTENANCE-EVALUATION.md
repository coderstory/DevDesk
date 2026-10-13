# DevDesk 长期维护评估（P16）

> 对应 spec §16 P16：「评估 EUI-NEO Metal 后端或 fork 需求；评估 Markdown 编辑器自实现」。
> 基线：2026-09-22，Windows 11，124/124 测试全绿。基调：**YAGNI——每项给"现在不动 / 建议何时动"的结论**，不产出实施计划。

## 结论速览

| 评估项 | 结论 | 触发再评估的信号 |
|--------|------|------------------|
| EUI-NEO Metal 后端 vs 继续 OpenGL | **现在不动，继续 OpenGL** | 回到 macOS 开发且 OpenGL 在目标 macOS 版本上实际不可用 / 性能不达标 |
| 是否 fork EUI-NEO | **现在不 fork，继续上游协同** | 上游停滞（数月无响应 / 拒收 PR）且 DevDesk 被框架缺陷阻塞 |
| Markdown 编辑器自实现 | **现在不自实现，继续 EUI-NEO input 组件** | 单篇笔记大到输入卡顿，或语法高亮成为硬需求 |

## 评估一：EUI-NEO Metal 后端 vs 继续 OpenGL

**现状（已审计事实）**：

- EUI-NEO 0.6.0 渲染后端两个：`opengl`（默认）与 `vulkan`（`EUI_RENDER_BACKEND` 可选），**无 Metal 后端**；窗口后端 glfw（默认）/ sdl2。
- DevDesk 当前开发平台是 Windows 11（OpenGL 路径实测稳定：124/124 测试、10MB 流式 240–320 MB/s、soak 0% 丢失）。
- macOS 27+ 是目标平台，且**已是当前开发平台**（2026-09-28 起本项目就在 macOS/arm64 上开发，详见 `DevDesk/CLAUDE.md`）。~~macOS 分支代码保留（接口隔离模式），自 2026-09-21 迁移后未重新构建验证~~ ← **这句已过期**：macOS 侧的构建 / 测试在 CI 里逐日真跑（`.github/workflows/ci.yml` 的 macOS job），早年的 `.mm` 编译失败也已修完。Apple 侧 OpenGL 长期处于弃用状态，Liquid Glass / vibrancy 亦未支持（EUI-NEO-PR-PLAN 评估为 XL 工作量，未选定）。

**判断**：Metal 后端是典型的"为未来平台分支预付的大额成本"——1–2 周量级、只在 macOS 上有意义、且当前没有任何 macOS 侧的实际故障或性能数据支撑。在 macOS 分支连"能否编译通过"都未验证的当下写 Metal 后端，是在给一个未激活的平台做优化。OpenGL 在弃用状态下仍可用这一事实没有改变，DevDesk 的 UI 负载（卡片、文本、列表）远未到需要 GPU 架构升级的程度。

**结论：现在不动。** 建议何时动：

1. 决定把 macOS 27+ 从"目标"变回"在开发"时，先做 macOS 分支全量构建 + 测试的恢复验证（这是前置，与后端无关）；
2. 恢复验证后若 OpenGL 在目标 macOS 版本**实际不可用**（链接失败 / 崩溃）或帧率不达标，再评估：优先推动 EUI-NEO 上游加 Metal 后端（符合 DevDesk 的上游贡献政策），DevDesk 自建或 fork 是最后手段。

## 评估二：Markdown 编辑器是否自实现

**现状（已审计事实）**：

- 编辑器用的是 EUI-NEO `input` 组件：多行 + IME + 选区 + 撤销重做均为框架自带能力（EUI-NEO-CAPABILITIES 已核对）。
- 预览用 EUI-NEO 自带 md4c 渲染，编辑→预览 debounced 联动；文件层（子目录、外部修改重扫、FTS5 索引）由 DevDesk 自己的 MarkdownService 管理。

**判断**：自实现编辑器（语法高亮、大文件虚拟化、自绘光标 / IME 组合窗口、撤销栈）是 GUI 框架级别的工作量，而 DevDesk 的差异化价值在文件管理 / 搜索 / 与 Claude 工具链的集成，不在编辑器内核。当前没有任何一条用户反馈或测试数据指向 input 组件的能力缺口。

**结论：现在不自实现。** 建议何时动（满足任一再评估）：

1. 单篇笔记大到输入出现可感知卡顿（先量：多大文件、多少行时掉帧）；
2. 语法高亮成为明确需求（届时优先评估 md4c 侧的 token 高亮 + EUI-NEO 富文本渲染，而不是从零写编辑器）；
3. EUI-NEO input 组件出现无法绕过的缺陷且上游不修（此时按上游贡献政策先提 PR，仍轮不到自实现）。

## 附：fork EUI-NEO 的判断标准

维持 CLAUDE.md 的上游贡献政策：EUI-NEO 的问题走上游 PR（当前候选见 [EUI-NEO-PR-PLAN.md](EUI-NEO-PR-PLAN.md)：P0 fontWeight 真切换、P1 全局 hotkey；另有 GCC `-Os` C4 mangle、platform.cpp `__APPLE__` 守卫等小项，原见 `docs/KNOWN_LIMITATIONS.md` 上游一节——**该文件已于 `5bc5a0a` 删除**，候选项的现行记录以 `EUI-NEO-PR-PLAN.md` 与 `docs/superpowers/plans/2026-09-27-eui-upstream-candidates.md` 为准）。fork 只在上游停滞且缺陷阻塞 DevDesk 时才值得——fork 的成本是永久性的（永久脱离上游演进、所有框架修复自己背），目前两个已选 PR 候选都是小改动，远未到那一步。

> ⚠ **2026-10-03 批注（评估结论已与现实脱节）**：本节写于 2026-09 中旬。此后事实变了——EUI-NEO 本地 main 已领先上游 `origin`（sudoevolve/EUI-NEO）**97 个提交**且**已决策永不推送回上游**（2026-09-28 用户决策），同时代码被推到了自建远端 `fork`（coderstory/EUI-NEO），**CI 已把该 fork 当作构建源**（`.github/workflows/ci.yml` 的 `EUI_NEO_REPO`）。也就是说「是否 fork」这条**实质上已经发生**，不再是「未选定」；上表「现在不 fork，继续上游协同」与本节"远未到那一步"的结论**均已过期**。现行口径见 `DevDesk/CLAUDE.md` 的「EUI-NEO 上游贡献政策」与「⚠ 跨仓库耦合」两段。
