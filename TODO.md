# DevDesk 未完成任务清单

## 🔴 活跃待办（2026-10-28 memcpp 线收线）

> memcpp 已交付部分见 `docs/superpowers/specs/2026-10-28-memcpp-overview.md`（需求→实现→验证映射）；本段只记**未完成项**，按优先级排。历史快照（含仍开着的 §13 profile 高级 tab）完整保留在下方分隔线之后。

### P0 — 用户已明确要求，下一步先做

- [ ] **MCP 集成（用户 2026-10-28 需求，纯增量接入）**：记忆功能作为一个 MCP 模块注册进 DevDesk 统一 MCP server（`src/module_mcp`，另一线已落 B1-B3）——Claude Code 经统一入口查记忆（recall/search 工具走既有 `module_memory/gateway/telemem_client`），MCP 页面展示记忆模块行+可开关（关=工具不注册/下线）。**红线：既有功能（hook 注入/面板/采集/卡片）一行不动**。前置：读 module_mcp 的模块注册模式与开关持久化；其线 B4 真连 E2E 是他们自己的欠账，不阻塞注册接线。
- [ ] **打包+真机 UAT（S5 收官）**：`bash scripts/package-windows.sh` 出安装包 → 用户安装 → 首跑向导（Python 3.10~3.12 检测/venv/pip 约 97s/引擎拉起）→ 真机点验：自动采集（30s 轮询）、脱敏拦截（动态流可见）、提案卡审核+撤销、Claude Code 提问时 hook 注入（≤5 条 ≤500 字）、消耗页、触发线仪表（冻结 15 查询评测）。

### P1 — 已知缺口（不影响 Windows 日常使用）

- [ ] **infer=true 云端提炼真调**：当前全部直存（infer=False，零 LLM 调用）。智能提炼需真调智谱 anthropic 端点 glm-5.3-flash（key 运行时读 `~/.claude/settings.json`，TELEMEM_CONFIG 已配 anthropic provider）。注意：套餐 key 用于后台常驻提炼偏离智谱使用条款，用户 2026-10-27 已知悉自担；控耗五条（节流/月度预算软硬档）已实现，实测验证待做。
- [ ] **月度预算硬档实测**：预算耗尽 → 暂停提炼只留检索的面板行为真机验证。
- [ ] **decay 遗忘清单真实现**：待 telemem 引擎暴露记忆列举缝（MemoryListFn）；当前 fail-closed 零假卡（扫描触发在，清单为空）。
- [ ] **baseVersion live 空转**：telemem 无版本查询缝，接受时以快照放行（契约位已留）；引擎升级后接真值。
- [ ] **memory_e2e_probe 门禁注入通道**：探针需外部注入 DEVDESK_DATA_ROOT+外部编排 llama-server/telemem，当前在门禁属环境性失败；给 build-test-run.sh 加注入开关。

### P2 — 平台与打磨

- [ ] **macOS 平台面**：`process_host_posix`（含 F2 修复：per-child 环境不污染父进程）仅编译验证，待 mac 实机；引擎托管 posix 侧同。
- [ ] **批量模式标签窄窗重叠**：1200 逻辑宽下批量开关标签与来源 chips 重叠（宽窗正常）。
- [ ] **向量模型升级档**：记忆量增大（数百条+）后可升 Qwen3-Embedding-0.6B（610MB/约 1.4GB RSS，难集满分）——**必须显式 `-c 8192`**（默认槽位会吃 4GB）；改 `kEmbedModel` 常量+重启引擎即可，见 `docs/superpowers/plans/2026-10-27-memcpp-embed-bakeoff.md`。
- [ ] **触发线升级 C**：冻结集转测 Recall@5<85% 或 2 字词命中低于 5/3/11 → 按备用图纸（`docs/superpowers/specs/2026-10-27-memcpp-p0-core-spec.md`）补自研全文通道；当前实测 100%（15/15），余量充足。

### 环境事实（接线时会用到）

- 模型文件收线时已删（曾下载于 spike/.bakeoff/）：bge-small-zh Q8_0 25MB（HF: CompendiumLabs/bge-small-zh-v1.5-gguf）+ Qwen3/bge-m3 各 640MB；`tools/memcpp-integration/spike/run_bakeoff.py` 内有下载 URL，重跑即取。
- spike 全套脚本在 `tools/memcpp-integration/spike/`（run_spike.py 五项实测可复跑）。
- 端口约定：llama-server 回环 11435、telemem 回环 8421；本机 http_proxy 会截胡云端调用（脚本已做 NO_PROXY 幂等旁路）。
- hook 注入通道：`devdesk.exe memory hook` 子命令（先于单实例锁分派）；hooks 配置键级装拆走 DevDesk settings 管理通道。

---


> ⚠ **本文整篇为历史快照，不再维护**——里面的"状态"与"分支"描述多数已过期。
> **当前状态以 `CLAUDE.md` 的「🚦 当前状态」段为准**（那段带日期与出处）。权威待办见 `docs/superpowers/plans/2026-10-21-UAT62-整改清单.md` 与 `2026-10-23-UAT62-收敛执行计划.md`。
> 仍在生效的内容：**§13 profile「高级配置」tab（需求 + 已定决策，待写 spec，未开始）**——这条是唯一还开着的需求条目。
> 2026-10-03 订正：§4 probes / §9 EUI-NEO / §「macOS 构建环境」/ §「分支-合并状态」四处已按当时事实改写（原描述均已失效）。

> 原始日期：2026-09-28 会话截止
> 已交付 20+ commit（详见 `git log --oneline`）

## 未完成项

### 1. 桌宠最大高度设置
- **需求**：设置页加"最大高度"控件（默认 288px, 范围 [48,512]），皮肤超过等比缩放
- **实时生效**：改设置后当前宠物立即 resize
- **窗最小尺寸**：宠物窗最小 48×48
- **涉及文件**：`settings_config.h/cpp`、`settings_ui_cards.cpp`、`pet_window.cpp`、`pet_config.h/cpp`
- **状态**：代理派发后取消（a6e3434efb40b413f）；**2026-09-30 已由「显示大小三档」方案取代并交付**（0.75/1.0/1.5 预设、等比缩放 + 512 上界单因子钳制、即时生效，见 `docs/superpowers/specs/2026-09-30-batch-fix-design.md` §6——本条旧「单值最大高度」形态作废，不再实施）

### 2. 桌宠气泡样式
- ✅ **已完成**（cbe7245）：底色半透 alpha 0.55 + 文本自适应宽 max 280px + 居中
- **待定**：是否需要换行支持（当前单行）

### 3. Todo 卡片拖拽跟手动画
- **需求**：拖动卡片时，卡片本体跟随光标移动（此前只变半透明 0.45，不跟手）
- ✅ **已完成**（598a970）：跟手副本 `todo.dragghost` 画在**页面级顶层 root**（`todo_ui_board.cpp` 的 `drawDragGhost`）——anchor = 按下时卡片 bounds + 按下点起累计位移（框架 `DragEvent::totalX/totalY`），经新纯函数 `ui::dragGhostAnchor` 换算到页面 compose 空间；zIndex 100（压过看板子树、低于弹窗栈底 1000）；opacity 0.85（让目标列插入位占位条从副本底下透出）。源列原卡照旧 0.45 半透明留原位（占位骨架），源列 0.6 调暗门控未动
- **原「关键点」两处失实，已纠正**：① `BoardPlate.dragOffset` 字段不存在（全仓零命中）；② 方案不是 `drawCard` 加 `.translate()` —— 列内容在 `components::scrollView` 根 stack 的 `.clip()` 内（`components/scrollview.h:88`，clip 在渲染侧对整棵子树做 scissor 求交：`core/runtime/runtime_render.h:223-230`），列内 translate 会被裁到列边界，跨列拖（主用例）不成立，故改页面级副本 + 绝对锚点定位
- **未验证**：真人手拖的体感与屏幕像素（本机 `screencapture` 被 TCC 挡，无截屏，也无人肉眼看过）。替代证据（均实测绿）：无头用例 `todo_ui_drag_follow_ghost`（**2026-10-03 轻量化后仅剩这一条**——原并列的 `todo_probes` 类 12 探针与本用例逐条重复、CI 从不执行，已随 8 个重复 probe 一并删除）；本用例做过变异验证（去掉跟手副本 / 去掉位移项即红）

### 4. Probes 扩展
- **已交付**：`tests/probes/` 现有 **5 个探针源文件**（2026-10-03 轻量化：从 13 个收到 5 个）（home / module_toggle_sidebar / screenshot / settings_dropdown_z + `probe_harness.h`），全部编译、接入 `ctest -L probe`，并在 `scripts/build-test-run.sh` 测试段之后自动跑（失败即整体 `exit 1`）。
- ~~**版本控制缺口（2026-09-28 实测）**：`tests/probes/` 下 3 个 probe 源文件从未被 git 跟踪——`claude_probes.cpp` / `home_probes.cpp` / `markdown_probes.cpp`，共 942 行。**待办：把这 3 个文件 `git add` 进版本控制**~~ → ✅ **已解决**：`git ls-files tests/probes/` 现在 13 个文件全在版本控制里（2026-10-03 核）。
- **2026-10-03 新增能力**：探针进程内帧捕获 `--screenshot <路径>`（`glReadPixels` → PNG，不需要系统「屏幕录制」授权；`screenshot_probe` 是它的自测）。详见 `CLAUDE.md`「功能开发生命周期」阶段 4。
- **待补**：
  - 多窗 stub（pet 行为档 P3）
  - `confirmDialogProbe` 共享骨架（spec 规划未实现）
  - Cl-13/Cl-14 探针断言组未全量实现
  - `--size` 尺寸矩阵多跑不支持（只支持单尺寸）

### 5. Rules 商标统一
- 缺圆角 item 已补（6f2b5b0：6 模块 9 处）
- 深色/浅色主题下各模块 hover 色验证——建议 probes P2 批覆盖

### 6. 安装程序
- ✅ 63MB setup.exe 已放 `D:\data\desktop\DevDesk-0.1.4-setup.exe`
- 含 9 皮肤 + about 漂浮圆环 + 全部修复
- package-windows.sh 已加 ccache launcher

### 7. About 页面效果
- ✅ 漂浮圆环（7addc2b：7 环 Spring 漂移）
- ✅ 2 卡左右并排（de20482）
- ✅ 删数据卡

### 8. 测试速度
- ✅ soak 60s 加 `DEVDESK_SOAK=1` 门控（b5d8525：3min→1m46s）
- ✅ 批量输出替代 per-case endl flush（b3bb24d）
- ⚠️ Top-3 慢测试：soak 60s (75s)、streaming 15s 超时（15s）、markdown 5000 行（5s）

### 9. EUI-NEO 上游
- ✅ `5b0cd34` styledmenu dismiss rect + requestDismiss 按值参数防 UAF
- **永不 push 到 `origin`（2026-09-28 用户决策）**：原文记「本地 main 领先 origin 94 个提交（2026-09-30 实测）」——**2026-10-03 实测为 97 个**（`git -C ../EUI-NEO rev-list --count origin/main..HEAD`），且 `fork/main` 已与本地齐平（`cbd0b48`，即 CI 用的那个远端）。origin/main 停在 `v0.6.0-4-g782c569`，本地领先是长期状态、提交数只会继续增长，不再按「待 push」跟踪。

### 10. About 页布局调整（2026-09-28 真机反馈）
- ✅ **已完成**（`a97355b`）
  - 内容区 720 → **1040**（`about_module.cpp` `kAboutContentW`），卡片宽 356 → **516**；最小窗（页面宽 = 1330 − 侧边栏 208 = 1122）下左右各余 41px，居中不溢出。**最小窗高度口径已变（2026-09-29 组 A）**：原写死的 845 是设计下限，现经 `core::window_layout::minWindowSizeForWorkArea` 换算后还要与工作区取 min —— macOS 落地 1330×805 点、Windows 落地 1663×973 物理（= 778 逻辑），845 只在工作区放得下时才成立
  - 顶部留白 24 → **64**（= `spacingXXLarge` 40 + `spacingXLarge` 24，两档 token 叠加推导——单档 40 偏小、双档 80 越界草图 60-70 区间；非裸魔法数）
  - hero 图标 48 → **56**（= `spacingXXLarge` + `spacingLarge`）；**页面标题字号仍是规范值 32px 未动**（CLAUDE.md「视觉规范」：页面标题 32 / 正文 15），「放大」观感由加宽 + 留白承担
  - footer 不再跟卡底流式排，改**贴页面底部**（底边 = `screen.height` − 40），卡片与 footer 之间即呼吸区
- **回归断言**：`tests/test_modules/test_ui_modern.cpp` `uiAboutLayout`（改前实测红：`hero.frame.y` = 24；改后绿）

### 11. 设置页删除最外层卡片（`settings.body.bg`）
- ✅ **已完成**（`a97355b`）：删 `settings_module.cpp` 里 M3-4 加的 `settings.body.gradient` stack 与其内 `settings.body.bg` 渐变底板（surface → surfaceElevated 垂直 + radiusLarge）——当日第一轮已删 9 卡 `cardFrame`，这块页面级底板就是用户眼中剩下的「最外层卡片」
- ⚠️ **背景（此需求反复出现的根因，务必记住）**：用户**此前多次**提出该需求，但**从未进入本文件**（历史查证：仓库任何待办文档都查不到此条）→ 没落文档 = 没人执行 = 每次都丢失，用户只能反复提（原话「让你删除好多次了」）。**规矩：用户口述的需求当次就写进本文件**，否则同一需求会被反复提出
- 删除后底色由主窗 `clearColor` 承担（off alpha=1 实色 / 半透 0.85 / 磨砂 0.55 + Acrylic，`settings_config.cpp:187-199`），符合 frost spec `docs/superpowers/specs/2026-09-22-eui-window-frost-design.md:53`「各模块页面要么靠 clearColor 当底、要么自铺不透明 surface 色」
- **回归断言**：`uiModernSettingsBodyGradientRemoved`（反断言：`settings.body.bg` / `settings.body.gradient` 只要存在即红，并反证设置页仍在渲染，防「整页 compose 早退 → find 恒 nullptr」的假绿）

### 12. 待定：同一套 M3-4 渐变底 pattern 的残留（本次未动）
- `md.preview.bg`（`src/module_markdown/markdown_module.cpp:1438`）是同 pattern **最后一处**；用户本次只要求设置页 → 是否一并删待用户拍板
- spec 漂移：`docs/superpowers/specs/2026-09-28-about-page-design.md:118`「主内容区 720px 宽」与 `:163`「`kAboutContentW = 720.0f`」仍写旧值（实现已 1040）；本次按范围纪律未改 spec
- 陈旧注释：`src/ui_kit/card.cpp:21` 仍引用已删的 `settings.body.bg`（ui_kit 不在本次改动范围）

### 13. profile「高级配置」tab（settings.json 非 env 顶层键接管）
- **需求**（用户 2026-09-28 提出，原话要点）：
  - profile 新增「高级配置」tab：一个文本框，内容是 JSON，语义 = **完整 settings.json 除去 `env` 属性**（permissions / hooks / statusLine / model 这类顶层键）
  - 应用时把**最终计算出的 env** 与这个 JSON **合并写入** `~/.claude/settings.json`
  - 共享配置片段（shared config）**只保留两个 tab**：`高级配置` 与 `环境`
  - 该 JSON 文本框**支持自动格式化**
  - 每个 profile 支持**导出为 JSON 写入剪贴板**
  - 新建 profile 与新建片段**都支持从完整配置 JSON 导入**（粘贴 → 自动拆进 环境 tab 与 高级配置 tab）
- **已定决策**（用户逐条拍板，勿改）：
  1. **高级配置为权威（整块接管）**：写入时文件里的非 env 部分以它为准 —— 它写什么就是什么
  2. **写入路径拆三档**：起代理**只动** `env.ANTHROPIC_BASE_URL`（+ 占位 token）；**切/应用 profile** 才写 env 全量受管键 + 非 env 键；停代理只还原 baseUrl（+ 撤占位 token），**非 env 键不动**
     - ⚠️ 这是**对现有行为的有意变更**：今天起代理会写整个受管 env（`settings_sync.cpp:294` 的 `buildProxyEnv`：BASE_URL + 占位 token + 模型槽位 + profile extra_env + 片段 env）
  3. **非 env 键的卸载**：写入前快照旧的非 env 全量顶层键；切到另一 profile 时全量替换；切到无 profile / 删 profile 时**还原快照**
  4. **合并深度**：profile 的 advanced 与所绑片段的 advanced = **顶层浅合并，片段赢**（与现有 env「片段赢」口径一致，`provider_config.h:205`）
  5. **env 键**：高级配置文本里若出现 `env` 属性 → **拒绝并报错**，提示到环境 tab 改
  6. **导出形状**：完整 settings.json（env + 高级配置合并）+ 一个 `devdesk` 扩展块（name / modelNames / fallbackProfileId / sharedConfigIds）；导出**写真实 token**（不带占位符）
  7. **片段导入**：剥掉托管键（BASE_URL / AUTH_TOKEN / 模型槽位键，它们由 profile 层托管），并在导入预览里**列明剥掉了哪些**
  8. `devdesk` 块**非法时不整体 fail**（env 是主体、devdesk 是增强）
- **路径**：用户已确认这是**架构级**改动 → **先写 spec（`docs/superpowers/specs/`）再实施**；本条目只记需求与决策，**待写 spec**
- **🔴 已知高风险点（写 spec / 实施前必看）**：`settings_sync.cpp:294 takeOver` 的 sidecar 快照当前**只快照 `env`**（结构 `{"managedKeys":{...},"envSnapshot":{...}}`，见 `settings_sync.cpp:131-132`）。决策 1 + 3 要求把**非 env 键也纳入快照**，否则「切 profile / 卸 profile」时撤不干净，会持续污染用户真实配置
- **现状锚点**（供 spec 对齐，非决策）：profile 表单当前 3 个 tab（`claude_ui_form.cpp:960` `.items({"基础", "模型映射", "高级 env"})`）；片段编辑弹窗 `drawSharedConfigDialog` 当前**无 tab**（只有名称输入 + 环境变量编辑器）→「片段只保留两个 tab」按新增这两个 tab 理解
- **状态**：未开始（需求 + 决策已记录；**待写 spec**）

---

## macOS 构建环境（原文为 2026-09-28 本机实测；2026-10-03 订正）

> ⚠ **下面这段的"未验证"结论已全部失效**——macOS 从 2026-09-28 起就是日常开发平台，`build-test-run.sh` 逐日真跑，CI 的 macOS job 也在跑它。留在下面只为沿革，**别再按它行事**（尤其别照抄"在端到端跑通之前，手敲两条命令仍是当前最可靠的路径"——现在反之：**一律走脚本，手敲是违规**）。

- 本机 macOS / arm64 已装 Homebrew 工具链（`/opt/homebrew/bin`）：ninja 1.13.2、openssl@3 3.6.4（同路径另有 cmake 4.4.3）
- configure 命令（**原文写"macOS 上须手敲"——已作废**；日常构建与测试一律 `bash scripts/build-test-run.sh`，它会自动定位 EUI-NEO 并补 `-DOPENSSL_ROOT_DIR=/opt/homebrew/opt/openssl@3`）：
  ```
  export PATH="/opt/homebrew/bin:$PATH"
  cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DOPENSSL_ROOT_DIR=/opt/homebrew/opt/openssl@3
  ```
- `scripts/build-test-run.sh` **已移植为跨平台**并已在 macOS 上长期真跑（提交 `577e0d0` 给三个脚本加 macOS/Linux 分支：Darwin 的 `/opt/homebrew/bin` PATH 前置、`-DOPENSSL_ROOT_DIR=/opt/homebrew/opt/openssl@3`、`pkill -f` 杀进程、`otool -L` 依赖清单、macOS 的 ccache 配置路径；原先写死的 Windows 工具链 PATH 与 `C:/msys64/mingw64` openssl 路径改为按 `DEVDESK_OS` 分支取，Windows 分支行为不变）。
- ~~macOS 首次构建失败：EUI-NEO `core/platform/window_style_macos.mm` 与 `core/platform/context_menu_macos.mm` 编译不过~~ → ✅ **已解决**，macOS 构建早已全绿（见 `CLAUDE.md`「🚦 当前状态」）。

---

## 已知 Bug

| 问题 | 位置 |
|---|---|
| todo 卡片拖拽无跟手动画已修（598a970） | `todo_ui_board.cpp:drawDragGhost`（不再是 `drawCard`） |
| md tabs 切换过渡已删（1cdd36f） | 原为整页 fade 闪烁 |
| pet 右键菜单缺 dismiss rect 已修（5b0cd34） | EUI-NEO styledmenu |
| 主窗最小尺寸已调 1330×845（65c1543）；2026-09-29 组 A 起改为经 `core::window_layout::minWindowSizeForWorkArea` 换算 + 与工作区取 min 后落地（macOS 1330×805 点 / Windows 1663×973 物理），845 退为"工作区放得下时"的设计下限 | main.cpp |

---

## 分支/合并状态（2026-10-03 订正）

- **集成分支是 `dev`，不是 `master`**（原文写"全在 `master`"已过期）：近期合并全是 `Merge branch 'worktree-agent-*' into dev`；`master` 只是发布分支（`.github/workflows/release.yml` 只在 `push master` 时出制品），本机没有本地 `master`。
- 工作区干净（`git status` 只剩 `?? Testing/` 之类非仓库产物）。
- ~~未提交改动：`tests/probes/` 下 3 个 probe 源文件未跟踪~~ → ✅ **已解决**（13 个探针文件全在版本控制里，见 §4）。
- EUI-NEO：**永不 push 到 `origin`（2026-09-28 用户决策）**，origin/main 停在 `v0.6.0-4-g782c569`；本地 main 领先 origin **97** 个提交（2026-10-03 实测）为长期状态，**`fork` 已齐平（`cbd0b48`）**，不再有「待 push」事项（见 §9）。