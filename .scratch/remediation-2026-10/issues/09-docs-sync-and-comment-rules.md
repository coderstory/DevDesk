# 09: 文档同步 + 注释规范

**What to build:** 三件事。① workspace 根 CLAUDE.md 重写为**纯指针文档**（只指向 `DevDesk/CLAUDE.md`，不复制状态）——复核确认其仍写「六模块含 pet」，而现仓库为 7 模块（about/agent/claude_proxy/home/markdown/settings/todo，无 pet）；注意该文件在 workspace 根（DevDesk 仓库之外），验收看内容不看提交。② DevDesk `docs/` 过时状态快照移入 `docs/archive/`，「当前状态」类段落统一加日期戳约定。③ 注释规范成文：存量不回退，新增注释「契约与不变量优先，叙事历史进 git/docs」，置于 agent 可读位置（CLAUDE.md / 贡献文档）。计划依据：`docs/plans/开发计划-2026-10-25.md` T7+T13。

**Blocked by:** 无（可立即开工）

**Status:** done

- [x] workspace 根 CLAUDE.md 无 pet/六模块表述，纯指针化（指向 DevDesk/CLAUDE.md）
- [x] docs 顶层无超过 30 天未核对的状态类文档；归档进 `docs/archive/`；「当前状态」段带日期戳
- [x] 注释规范成文且 agent 可读（写入 CLAUDE.md 或贡献文档）
- [x] 合入门槛：9 个 `check-*.sh` 守卫全过 + ctest 全绿（文档票不改代码，守卫照跑防误伤）

## 证据摘要（2026-10-26）

- **① 根指针化**：`D:\project\gui\CLAUDE.md`（仓库外）重写为纯指针文档——只留「这是什么项目 / 入口在哪 / 状态看哪」，指向 `DevDesk/CLAUDE.md` 为唯一状态源；全文无 pet、无「六模块」表述。该文件不在 git 仓内，按票面以内容验收。
- **② 归档**：新增 `docs/archive/`，移入 4 个时点状态快照——`PR-DESCRIPTION.md`（09-21 PR 描述草稿）、`CONSISTENCY-CHECK.md`（09-21 UI 一致性检查报告，内容还停在「compose 全 stub」时代）、`MAINTENANCE-EVALUATION.md`（09-22 基线评估，124/124 时代）、`lightweight-audit-2026-10-03.md`（10-03 一次性审计报告）。架构 / 指南 / 规格 / 能力盘点类（ARCHITECTURE、USER_GUIDE、UI-UX-DESIGN、MACOS-APP-BUNDLE、ccache-setup-guide、EUI-NEO-CAPABILITIES、EUI-NEO-PR-PLAN）留原地。悬空引用同步：`EUI-NEO-PR-PLAN.md`、`2026-10-19-ui-test-gap-map.md`、`check-consistency.sh` 的 `DOCS_FILE` 常量。日期戳约定立入 `DevDesk/CLAUDE.md`「🚦 当前状态」段（状态段必须带日期戳；时点快照超 30 天未核对归档进 docs/archive/）。
- **③ 注释规范**：`DevDesk/CLAUDE.md`「注释纪律」段原有口径（必留契约/不变量、必删演变叙事）已存在，按 T13 补总纲一条：「契约与不变量优先，叙事历史进 git/docs；存量注释不回退」。
- **④ 累积标记守卫转绿**：基线红根因 = 清单文件工作区为 **CRLF 行尾**——空行变 `"\r"` 被误报「缺 :: 分隔」（exit 2）、所有标记尾部带 `\r` 致 grep 全 miss。修 `scripts/check-cumulative-markers.sh`：校验前剥行尾 CR + 空行/纯空白/注释行忽略（语义不变，非空行才校验格式）。**真实缺失标记 0 条 → 删除断言 0 条**（60/60 全命中，断言集一条未动，无需删项）。负路径复核：真格式错仍 exit 2、真缺失仍 exit 1。
- **⑤ 验证**：全量 `./scripts/build-test-run.sh --no-run` → 主套件 `Total: 1586 / Passed: 1577 / Failed: 0`（与基线一致，未动代码）。probe 段 3 失败（agent_e2e_probe / home_probes / home_probes_min_window）为本机环境性既有（缺 LLM 凭据 / 真窗口环境），与基线相同，本票改动不含任何 C++ / 探针输入。9 守卫退出码：consistency / module-isolation / no-glass / probe-skip-gate / std-includes / toolguard-braces / windows-cross-compile / eui-neo-version / cumulative-markers = **全 0**。
