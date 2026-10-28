# DevDesk 未完成任务清单

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

---

## 上线模块收尾计划（2026-10-28 用户指令，生效中）

**阶段目标**：先不执行测试；依次完成 ①剩余全部功能开发 ②测试代码编写 ③文档完善，然后**全部 commit、合并到 `dev` 分支**。测试执行放在合并之后统一做。

- **代码位置**：全部实现在 `D:/wt-rel-core`（分支 `wt-release-core`），主仓 `feature/20261027-release-module` 只有文档；合并时以 worktree 分支为源
- **待开发（进入批次 4）**：审计 B 剩余 3 major——~~提醒窗开窗失败即死（`release_reminder`）~~、steps 每秒 N+1 扫库（需 store 加按任务一次拉全的查询）、列表状态不按时间推导（需 `release_logic` 加纯函数）；Z 线留下的三件——~~详情页迁移到 `dueItems` 新签名~~、删除 `appendSyntheticConfigs` 兜底、N+1 同前；审计 A/B 共 28 minor 待分诊（修/不修/仅记录）
- **测试编写**：批次 2/3 新增功能中无断言的——配置编辑层、跳过勾选、服务/环境移除、详情页返回；测试写好**不跑**，与代码一起 commit
- **文档完善**：spec §10/§11 与实现同步（侧边栏重排、默认折叠、ticker、编辑层、原子化建单）；`.scratch/release-module/checklist.json` 状态更新
- **完成后动作**：全部 commit（显式 pathspec）→ 合并到 `dev` → 用 `C:/Tools/Git/mingw64/libexec/git-core/git-push.exe` 推送 → 在 `dev` 上跑完整门禁（构建 + 全量单测 + 探针）
- **审计口径**：批次 2/3 的 diff 尚未被审计，合并前需补一次审计 C（范围小）

## 2026-10-28 · 增补批（H27-H31）执行指令（用户原话）
- 先不执行测试，优先完成全部功能的开发、测试编写、文档完善，然后全部 commit，合并到 dev 分支
- 落实口径：实现子任务只写码 + 写测试，不构建不跑测试；合并 dev 由主会话串行做；全部合入后由主会话统一跑一次构建 + 测试收口
- 本批范围：docs/superpowers/plans/2026-10-21-UAT62-整改清单.md「增补需求 · 2026-10-28 用户登记（H27-H31）」节

## 上线模块收口状态（2026-10-28 18:15，用户指令"收尾了，没干完的写 TODO.md"）

已进 dev（本地 dev 26a2deaf = 调度线全部 + 上线模块全部 + 修复批）：实现 6ca2174a → 审计 C1/C2 全处置（52f68391/749aeec7/bd11fc52/aa0adf6b/8126eaa9/d1b34740，台账见 docs/superpowers/plans/2026-10-28-release-audit-C1C2-disposition.md）→ 与 origin/dev 合并 76aebdde（MCP 在上线上面，17 冲突按 2026-10-28-merge-dev-prep.md 解）→ dev 合并 26a2deaf。

**未干完清单（按优先级）：**
0. **dev 推送未完成（收口时被卡）**——本地 dev `26a2deaf` 推送被拒：memcpp 会话已把「Memory 页」推上 origin/dev（其融合序 …Agent/Memory(5)/Wallpaper/Mcp/Settings/About，与本地 12 页序分叉），同步合并需重钉 13 页枚举（Memory 排智能体后，MCP 仍在上线上面）+ test_router/test_app_assembly/test_wallpaper 等全部钉值；且 dev 集成席位 wt-merge 被调度会话占用解冲突中。**待席位空闲：merge origin/dev → 重钉枚举序 → git-push.exe origin dev**。
1. **整批无编译/测试实测证据**——修复批 7 个提交 + 两个合并均未构建未跑测试（本机免编译指令；CI 验证环节按用户指示取消）。首个实测点 = 下次任何一次构建；红了按失败项修。macOS 侧全程仅静态审计，无实机编译。
2. **feature/20261027-release-module 文档分支未合入 dev**——上面还有交付闸门 hook（.claude/hooks/deliverable-gate.sh）、spec 同步版、TODO 收口段、调度交接文档原版。后续合并时：handoff 文档 dev 侧已是"路径仓库相对"修订版，冲突取 dev 侧。
3. **安装程序未做**——上线模块随 0.5.0 出包（Inno Setup / DMG）+ 用户真机 UAT 未做。
4. **A-02 待拍板**——列表卡进度分母与详情页 5 槽位口径不一致，需要用户定夺（分母按服务×环境还是按配置项）。
5. **缓置项**——F-3 父行存在性预检（导入/脚本入口出现时再补）；提醒窗句柄耦合块未抽纯函数（openReminder/closeForEmpty/collectReminderView/buildDemoView/绘制，硬抽会改行为）。
6. **已知无关红灯**——agent_e2e_probe 在本机因 DPAPI 凭据失败（先前已判与需求无关，未做基线复核）。

