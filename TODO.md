# DevDesk 台账（三池制，2026-10-29 产品经理改版）

> **⚠ 本册已于 2026-10-30 退役：唯一权威台账 = `docs/pool/pool.db`（池 MCP），人读视图 `docs/pool/pool-report.html`。**
> 本文件仅留历史；下方旧编号与池库映射：需-1/2/3=池 1/2/3；需-4（月度预算硬档）已并入池 3；需-5~9（decay/baseVersion/DST/向量模型/触发线）已迁入池 31~35；BUG-1~3=池 14~16；债-1~6=池 17~22（债-6 托盘行=池 22 关联项）。新条目一律走池 MCP 工具，不再改本文件。

> **规矩（2026-10-29 老板定，全员执行）**：
> 1. **三池**：需求池（用户/产品要的东西）｜bug 池（已发生缺陷）｜待办池（体系债/基建/条件未熟项）。所有未关事项**必须入池**，不再散记。
> 2. **分级**：P0 即时打断处理（尽量卡阶段边界）｜P1-P3 按计划排产，**不插队**。每条目必须带 P 级。
> 3. **卡点标签**：`等授权`（等老板 commit/push/装机等）｜`等环境`（等机器/外网/key）｜`条件未触发`（触发线到了才做）｜`登记不派工`（记账，暂无计划）。
> 4. **入池**：任何人口述的需求/发现的 bug 当次就写进对应池，否则必丢。**捞取**：排产从池里按 P 级捞，谁派活谁标注来源条目 ID。**关账**：完成就地标 `[x]` + 日期 + 一句话证据，满批移入「已销账」压缩区。
> 5. 池条目用 `池-序号`（需-N / BUG-N / 债-N）编号，跨文档引用用编号。
> 历史版本：`git log TODO.md`；改版前全文 `git show <改版前一提交>:TODO.md`。

---

## 📥 需求池

| 编号 | 事项 | 级 | 卡点 | 说明 |
|---|---|---|---|---|
| 需-1 | **打包 + 真机 UAT（0.5.0 收官）** | **P0** | 等授权 | `bash scripts/package-windows.sh` 出包 → 用户安装 → 首跑向导 → 真机点验：自动采集（30s 轮询）、脱敏拦截（动态流可见）、提案卡审核+撤销、hook 注入（≤5 条 ≤500 字）、消耗页、触发线仪表（冻结 15 查询评测）。与 C 线安装程序 UAT 是同一件事（需-2），一处收口。 |
| 需-2 | **安装程序 + 真机 UAT（release 线）** | **P0** | 等授权 | 卡点：老板 commit/push 与装机授权未放，git 与覆盖安装由老板操作；代码侧测试闸已过（2026-10-29 全量 1954/0 + probe 22/22 + 守卫全绿，QA 复核）。0.5.0 出包（Inno Setup / DMG）。 |
| 需-3 | **infer=true 云端提炼真调** | P1 | 等环境 | 当前全部直存（infer=False，零 LLM 调用）。智能提炼需真调智谱 anthropic 端点 glm-5.3-flash（key 运行时读 `~/.claude/settings.json`，TELEMEM_CONFIG 已配 anthropic provider）。注意：套餐 key 用于后台常驻提炼偏离智谱使用条款，用户 2026-10-27 已知悉自担；控耗五条（节流/月度预算软硬档）已实现，实测验证待做。 |
| 需-4 | **月度预算硬档实测** | P1 | 等环境 | 预算耗尽 → 暂停提炼只留检索的面板行为真机验证。 |
| 需-5 | **decay 遗忘清单真实现** | P2 | 条件未触发 | 待 telemem 引擎暴露记忆列举缝（MemoryListFn）；当前 fail-closed 零假卡（扫描触发在，清单为空）。 |
| 需-6 | **baseVersion live 空转** | P2 | 条件未触发 | telemem 无版本查询缝，接受时以快照放行（契约位已留）；引擎升级后接真值。 |
| 需-7 | **Windows DST 真覆盖** | P2 | 等环境 | ucrt 不识别 IANA 时区名 → 恒 SKIP；需 Windows tzdata 分发或引擎时间源注入（需 Windows 机）。 |
| 需-8 | **向量模型升级档** | P3 | 条件未触发 | 记忆量增大（数百条+）后可升 Qwen3-Embedding-0.6B（610MB/约 1.4GB RSS，难集满分）——**必须显式 `-c 8192`**（默认槽位会吃 4GB）；改 `kEmbedModel` 常量+重启引擎即可，见 `docs/superpowers/plans/2026-10-27-memcpp-embed-bakeoff.md`。 |
| 需-9 | **触发线升级 C** | P3 | 条件未触发 | 冻结集转测 Recall@5<85% 或 2 字词命中低于 5/3/11 → 按备用图纸（`docs/superpowers/specs/2026-10-27-memcpp-p0-core-spec.md`）补自研全文通道；当前实测 100%（15/15），余量充足。 |

## 🐛 bug 池

| 编号 | 事项 | 级 | 卡点 | 说明 |
|---|---|---|---|---|
| BUG-1 | **agent_e2e 真网探针首跑挂/复跑绿** | P2 | 登记不派工 | flake，低。明细见 `docs/plans/full-buglist-2026-10-29.md`。 |
| BUG-2 | **归档动画重触发无观察缝** | P2 | 登记不派工 | 测试缺口，低。明细见 `docs/plans/full-buglist-2026-10-29.md`。 |
| BUG-3 | **壁纸线·退出 terminate 尾巴** | P2 | 等环境 | 2026-10-29 老板拍板不专项追——随那台 Windows 机下次使用顺手上行提交。根因 = `src/platform/single_instance_win.cpp` 单实例监听线程 joinable 活到静态析构（std::terminate），非壁纸问题、属既有债。修复代码已写在另一台 Windows 机 `wt-dev-gate` 工作树（`shutdownListener()` + `std::atexit`，增量构建绿）——未上行未验证，本机没有这份代码。收尾口径：验证托盘退出路径（退出码 0 + stdout 无 terminate 行，双次确保确定性）→ 原子提交 `fix(exit): 单实例监听线程 atexit join 收尾——消 terminate 尾巴` → 推 dev。（2026-10-28 核：origin/dev 无此修复） |

## 🔧 待办池（体系债 / 基建）

| 编号 | 事项 | 级 | 卡点 | 说明 |
|---|---|---|---|---|
| ~~债-1~~ | **探针残留守卫 + agent_e2e 挪 live** | P2 | ✅ 2026-10-29 销账 | 已交付合并 dev（merge b11d3666，实现 1666a2cb + G-1 兜底 3a0d2dea）；QA 史密斯专员反证红/绿+复验关单；guard_probe_residue 落 build-test-run.sh 测试段尾部（TERM→-9 兜底），agent_e2e_probe LABELS live（probe 门禁 22→21）。判据与终验：`docs/plans/probe-residue-incident.md`。 |
| 债-2 | **UI 交互链路自动化验证缺位** | P2 | 登记不派工 | qa/研发可自主实证启动/截屏/日志/数据/HTTP，但点击/输入类交互无法自主执行（当前按 AGENTS.md §八如实标「交互链路未验证」）。长期解法：给 DevDesk 建测试自动化驱动入口（CLI/IPC 触发页面与操作）。 |
| 债-3 | **`build/DevDesk.app` 陈旧 bundle 陷阱** | P2 | 登记不派工 | 构建脚本常规路径不重组 .app bundle，产物是 `build/DevDesk` 裸二进制——直接启动旧 bundle 会得出「新功能不存在」假象（2026-10-29 MCP 集成验收实际踩中）。修法候选：构建时清理陈旧 bundle，或验收/打包口径钉死 `--install`/裸二进制（待用户选）。 |
| 债-4 | **process_runner 三平台共同待办** | P2 | 登记不派工 | 2026-10-29 交叉审 3d59d076 登记，不单独派工，下次触碰时修：① 收尸后 exited=true 超时不再生效，管道被孙进程持有则 EOF 永不来——三平台统一补「收尸后排空加有界宽限（timeoutMs/2）」（process_runner_macos.cpp:268、posix:189）；② poll revents 只认 POLLIN\|POLLHUP，POLLERR/POLLNVAL 不排水不关 fd（process_runner_macos.cpp:247-262）；③ posix waitpid 失败 break 静默吞 ECHILD，与 macos 显式 fail+留痕口径不齐（process_runner_posix.cpp:204）。 |

---

## 🧰 memcpp 环境事实（接线时会用到，不随池条目删除）

- 模型文件收线时已删（曾下载于 spike/.bakeoff/）：bge-small-zh Q8_0 25MB（HF: CompendiumLabs/bge-small-zh-v1.5-gguf）+ Qwen3/bge-m3 各 640MB；`tools/memcpp-integration/spike/run_bakeoff.py` 内有下载 URL，重跑即取。
- spike 全套脚本在 `tools/memcpp-integration/spike/`（run_spike.py 五项实测可复跑）。
- 端口约定：llama-server 回环 11435、telemem 回环 8421；本机 http_proxy 会截胡云端调用（脚本已做 NO_PROXY 幂等旁路）。
- hook 注入通道：`devdesk.exe memory hook` 子命令（先于单实例锁分派）；hooks 配置键级装拆走 DevDesk settings 管理通道。

## 🗄️ 已销账（压缩区，只留一行指针；细节查 git 历史）

- 2026-10-29：memcpp MCP 集成交付（PRD `2026-10-29-memcpp-mcp-integration.md`，全量 1927/0）；调度线 R2/R3/R4 销账（commits 29a61928 / 1b97ab4a / 2a2f2e5f，调度域 88/88）；release 线置顶泄漏修复（09d8aa62）+ S8 只读源接线（6c36ce49）；A-02 口径冲突核实为无冲突；壁纸线 agent_e2e_probe 环境红销账（22/22）；macOS 平台面 mac 实测绿；批量模式标签窄窗重叠修复（e088aaef）；AGENTS.md 平台文件统计更正（3a94749b）。
- 2026-10-28：TODO.md 全文清理重写，基准 `40bd1d7e`（本机此前落后 origin/dev 179 提交；EUI-NEO 同步至 fork/main @ 3ee91da）。
- 2026-09-30：§13「profile 高级配置」闭环交付后又于 UAT5 整体删除（b232398c 等）——现行口径：接管不写非 env 顶层键（`settings_sync.cpp:309`）。若要恢复 = 新需求重新入池，勿引用旧决策。
- 壁纸线 deferred（非欠账）：图片壁纸四铺法后续增强；macOS 平台层规格 P8（代码未写，勿宣称可用）。永久不做：多显示器/转码降帧/维护删除当前壁纸/玻璃半透/独立应用。
- memcpp 线缓置（有意不做）：F-3 父行存在性预检（导入/脚本入口出现时再补）；提醒窗句柄耦合块不抽纯函数；cron 编辑回显为 v1 有意取舍（`schedule_ui_edit.cpp:371`）。
- 权威背景：整改台账 `docs/superpowers/plans/2026-10-21-UAT62-整改清单.md`；当前状态以 `CLAUDE.md`「🚦 当前状态」段为准。
