# CI 构建版（v0.1.1）问题批修复——A 缩放派生审查 / B 高级配置 UI / C 复制语义与配置导出（设计）

日期：2026-09-30 ｜ 状态：**设计（未实施）** ｜ 来源：CI 构建版（v0.1.1，master `a4430da`）用户上报问题批中的三项 ｜ 关联：`2026-09-27-shared-config-and-failover-design.md`（备用 Profile）、`2026-09-27-profile-auth-field-design.md`（认证字段）、`2026-09-27-profile-reverse-sync-and-copy-design.md`（「复制=建副本」既有拍板）、`2026-09-28-profile-run-design.md`（buildDirectEnv / settings JSON 形态先例）、在飞分支 `fix/detail-env-pane`

> 本文 项 A 的前提已经过一次修正（2026-09-30，调查 agent 像素级实证回填）：**「130% 默认在 macOS 生效」不成立**，macOS 用户日志所有会话均 100%，默认值本就是平台一致的 100（见 §2.1 证据）。项 A 由「字号缩放默认值平台化」改写为「缩放派生审查」，**零默认值改动**。

## 0. 总表

| 项 | 内容 | 优先级 | 类型 | 依赖与边界 |
|----|------|--------|------|-----------|
| **A** | #14 缩放派生审查：紧布局面「文本字号已派生 / 行几何仍是字面」点位盘点，输出本期修 / tech-debt 两张清单；**零默认值改动** | P1（先成文，改动力最小） | 审查 + 小改 | 环境变量 pane 的行几何派生化**归在飞 `fix/detail-env-pane`**（本文只做边界声明，见 §2.4，不重复设计） |
| **B** | #8 高级配置 UI 恢复：编辑弹窗加第 4 个 tab「高级配置」（dialectOverride 下拉 + 备用 Profile 下拉整体挪入）+ 详情页加「高级配置」只读 pane | P1 | 功能补全（数据层 09-27 就绪、UI 从未接线） | 依赖 upstream_dialect.h 既有值域与归一函数（CPX-13）；服务层 `createProfile/updateProfile` 参数位已存在，零服务层签名改动 |
| **C** | #9 复制语义消歧 + 配置导出剪贴板：「复制」改名「建副本」（详情 + 右键菜单），新增「复制配置」（profile → settings.json 形态 JSON → 系统剪贴板，**默认占位不导明文 key**） | P2 | 语义修正 + 新功能（纯 UI 层） | 服务层零改动；序列化复用 `buildDirectEnv` 纯函数；剪贴板走 EUI-NEO `core::window::setClipboardText` |
| **审计** | 方案审计：2026-09-30 完成，9 findings 已处置，见 plans/2026-09-30-batch-fix-audit.json | — | 文档收口 | `docs/superpowers/plans/2026-09-30-batch-fix-audit.json`（medium 3 / low 5 / nano 1，逐条落入对应方案段；6 项已裁决见 §5） |

> **备注（#5 字体发虚，已结案免实施）**：调查结论为磨砂档（windowEffect=frosted）0.15 alpha 材质合成的**设计行为**，与构建（CI / 本地）无关，非缺陷。后续审计**勿再把它当问题查**；本文所有章节均不涉及发虚。

---

## 1. 需求与决策（用户上报 → 本文范围）

| # | 用户上报（v0.1.1 实测） | 本文处置 |
|---|------------------------|---------|
| #14 | 字号缩放调到 130% 后环境变量 pane 文字与行盒重叠 | 项 A：审查 + 两张清单（env pane 本体归在飞修复） |
| #8 | profile 有「高级配置」数据（方言覆盖 / 备用 Profile），UI 上看不到、改不了 | 项 B：表单第 4 tab + 详情只读 pane |
| #9 | 「复制」按钮实际是建副本，用户预期是「把配置复制到剪贴板」 | 项 C：改名 + 新增「复制配置」 |
| #5 | 字体发虚 | **已结案**（磨砂合成设计行为），见总表备注 |

| 决策 | 内容 | 理由 |
|------|------|------|
| **D-A1** | 项 A **不做任何默认值改动**——`fontScalePercent` 默认 100（`src/module_settings/settings_config.h:121`）、钳制上限 `kMaxFontScalePercent=130`（`settings_config.h:47`）维持原样；「平台条件化默认值」取消（本来就已是平台一致现状） | 调查实证：macOS 默认即 100%，130% 只是用户在设置页显式调出的档位；w7-shell 时代的 130 默认早已被 V3 换键重置回 100（`settings_config.h:117-120` 注释） |
| **D-A2** | 项 A 的产出 = 派生现状审查表 + 两张点位清单（本期修 / tech-debt），本期修仅限 settings 卡片行盒派生化（机械小改，见 §2.3） | env pane 归在飞；其余面在 130% 下要么无重叠风险（全字面不缩放）要么余量充足，无用户实证受害 |
| **D-B1** | 表单弹窗 tabs 由三页变四页：`基础 / 模型映射 / 高级 env / 高级配置`（尾插，不改前三页序） | 前三页序是 §6.1 既有拍板；高级配置 = 高级项之尾，最不打扰 |
| **D-B2** | 备用 Profile（failover）下拉从**基础页**整体挪进新 tab；认证字段（authField）下拉**不动** | 见 §3.2：authField 已有完整渲染控件（`formAuthField_` + 下拉），任务书要求「若有，不动」 |
| **D-B3** | 详情页 tabs 由四变五：`基础信息 / 模型映射 / 环境变量 / 高级配置 / 最近调用`（插在环境变量后、最近调用前，`detailTab_: 3=高级配置, 4=最近调用`） | 高级配置属属性展示，最近调用是时序记录，二者语义不同层；既有用例/探针经 `claude.detail.tabs.hit.3` 导航锚按索引点击处 +1（`detailTab_` 在测试代码零引用，见 §3.5 风险 3） |
| **D-C1** | 「复制」改名「**建副本**」（详情 `claude.act.dup` 按钮 + 列表右键菜单项 + 相关 toast 文案），功能（`duplicateProfile`）零改动 | 09-27 spec 拍板过「复制=建副本」且功能正常；用户预期冲突纯粹是文案歧义 |
| **D-C2** | 新增「**复制配置**」：把当前选中 profile 序列化成 settings.json 形态 JSON（`{"env": {...}}`）写系统剪贴板；**默认占位** `sk-ant-***<尾4位>`，不导出明文 key | 安全决策，理由见 §4.2；「含明文 key」的导出形态**不做**（已裁决 §5①） |
| **D-C3** | 「复制配置」入口：详情操作按钮组第 7 键 + 列表右键菜单新增一项；服务层零改动 | 见 §4.3 布局与 §5⑥ |

---

## 2. 项 A：缩放派生审查（#14）

### 2.1 背景与根因（含证据）

**缩放的唯一生效通道**：`core::theme::setFontScale` → `applyFontScale`（`src/core/theme.cpp:113-121`）——**只乘 6 个 token 字号**（`fontSizeSmall/Body/Large/Title/Page/Display`），注释明说「只缩放字号，间距/圆角不动（字号是可读性需求，布局度量保持稳定）」。grep 全 EUI-NEO 树零 `setFontScale` 入口 → **字面 `.fontSize(N)` 与一切布局几何常量（position/size/lineHeight/行距 pitch/盒高）永不随缩放变化**。

根因一句话：**缩放只作用于 token 字号，而大量紧布局面的行几何按 100% 假设写成字面常量**——凡是「token 字号文本装在字面盒」的组合，130% 下文字 advance 超出盒即重叠/贴边；凡是「全字面」的面，不缩放即无重叠（但缩放对该面失效）。

- 用户实证：130% 档环境变量 pane 重叠（调查 agent 像素级实证）；同档首页无重叠且渲染锐利（截图 `/tmp/probe-local-130.png`）。
- 佐证：首页卡片内几何**有派生**（`src/module_home/home_module.cpp:87` `const float s = core::theme::fontScale()`，`:344` `statH = 144.0f * s`）——派生处无重叠，即派生化有效的正面参照。

### 2.2 审查口径与现状盘点

判据按 2×2：**文本是否随缩放派生 × 几何是否随缩放派生**。

| 面 | 文本字号 | 行/盒几何 | 130% 下的表现 | 证据 |
|----|---------|-----------|---------------|------|
| 首页卡片 | token + 局部 `s` 派生 | 卡内高 `144*s` 派生 | ✅ 无重叠（实测参照系） | `home_module.cpp:87,344` |
| **settings 卡片** | **已派生**（`fs()` helper） | **字面**（16px 标签行盒 / 24px 标题盒 / 40px 下拉） | ⚠️ **高风险**：`fs(12)=15.6px` 文本装 16px 行盒，余量 0.4px，贴边/裁切实证区 | 文本 `settings_ui_logic.cpp:25`、`settings_ui_cards.cpp:101,111,149,179,212,220,293,346`；几何 `settings_ui_cards.cpp:99,109,121,147,155,177,185` |
| todo 卡片 | 字面（全模块零 `fontScale()` 引用） | 字面 | ✅ 无重叠；❌ 缩放对 todo 完全失效（可读性诉求未达成） | `grep -rn fontScale src/module_todo/` 零命中 |
| claude 表单弹窗 | 字面为主 | 字面 | ✅ 无重叠；缩放失效 | `claude_ui_form.cpp:378-538` 等 |
| claude 详情动作按钮 | token（`fontSizeSmall`） | 36px 字面按钮盒 | ✅ 余量足（13×1.3≈16.9 < 36），低风险 | `claude_ui_detail.cpp:707,725,743,761,779,799` |
| claude 详情基础 pane 值 | token | 28/32px 字面盒 | ✅ 余量足，低风险 | `claude_ui_detail.cpp:112,127` |
| claude 详情标题 | token（`fontSizeLarge` 18×1.3≈23.4） | **24px 字面盒** | ⚠️ 贴边（余量 0.6px，未实证重叠） | `claude_ui_detail.cpp:869-877` |
| **claude 详情环境变量 pane** | 字面 | 字面（`kEnvRowPitch=54` / 行盒 48） | **归在飞分支修复（已提交 1ca87b0 待合并，见 §2.4）** | `claude_ui_detail.cpp:297-298,343,350,373,381` |

### 2.3 方案：两张清单

**本期修（本批实施，机械小改）**：

| 点位 | 改法 |
|------|------|
| settings 卡片文本行盒（`settings_ui_cards.cpp` 的 16px 标签行 / 18px 描述行） | 行盒高与 lineHeight 从字面改为 `fs()` 派生（如 `fs(16.0f)`），与已派生的字号同源同通道；标题盒 24px 同理。只改数字来源，不动布局结构 |

**登记 tech-debt（不在本批做，记入 backlog）**：

> **已登记（2026-09-30，Wave B 项 A 实施时）**：原登记目标 `docs/KNOWN_LIMITATIONS.md`
> 已整档删除（`5bc5a0a`），下表四条核实后改记
> `docs/superpowers/plans/2026-09-30-scale-derive-tech-debt.md`（T1~T4）——其中
> env pane 一条（下表末行）因 `fix/detail-env-pane` 已并入 master（merge
> `0edd3cf`）**改记「已结案」**；另加实施中新观测两条：settings 页头（T5，
> 130% 档截图实证标题压副标题行）与 settings 22px 卡片标题盒（T6，贴边未实证
> 重叠）。验证留痕见同目录 `2026-09-30-scale-derive-verify.md`。

| 点位 | 现状 | 为什么不修 |
|------|------|-----------|
| todo 卡片全量（文本 + 几何全字面） | 130% 下无重叠，仅「缩放失效」 | 需要整套派生化设计（含卡片高度自适应），不是小改；无用户实证受害 |
| claude 表单弹窗（全字面） | 同上 | 同上 |
| claude 详情标题 24px 盒 / 按钮 36px 盒 / 值盒 | token 文本 + 大字面盒，余量 0.6px~20px | 未实证重叠；贴边点（标题）随 env pane 在飞修复收敛后一并复查 |
| claude 详情 env pane | 字面 | **在飞 `fix/detail-env-pane` 所有（已提交 1ca87b0 待合并）**，见 §2.4 |

### 2.4 与在飞环境变量 pane 修复（#3）的边界（防两处同改）

- `fix/detail-env-pane` worktree（分支 `fix/detail-env-pane`）**修复已提交**：tip = `1ca87b0`「fix(claude-ui): 详情页环境变量 pane 滤模型映射键 + 行几何随字号缩放派生」，领先 master 1 个提交、工作树干净（审计 2026-09-30 实查）——`drawDetailEnv`（`claude_ui_detail.cpp:243-402`，含 `kEnvRowPitch` / `kEnvOverflowH` 常量与行内文本）已按**行几何派生化**方案改完，状态为待合并，非「在飞出活」。
- **本批项 A 不碰 `claude_ui_detail.cpp` 的 env pane 任何一行**（本文 §2.2 表中该行仅作盘点记录，不构成实施依据——重叠机理以在飞分支的实证为准）；项 B 的详情「高级配置」pane 是**新增函数**（`drawDetailAdvanced`），不触碰 `drawDetailEnv`，无文件级冲突但**同文件，串行施工**（项 B 排在 env pane 修复合并后 rebase——等待对象是合并已完成的提交 `1ca87b0`，非等在飞分支出活）。
- settings 卡片派生化在 `module_settings`，与在飞分支零交叠。
- 后续若统一推进「全模块派生化」（todo / 表单），以 env pane 修复提交 `1ca87b0` 确立的派生口径（`fontScale()` 读取点、`fs()` helper 形态）为准绳，不另起炉灶。

### 2.5 改动文件清单（预估）

| 文件 | 改动 |
|------|------|
| `src/module_settings/settings_ui_cards.cpp` | 行盒/标题盒高度字面 → `fs()` 派生（约 8-10 处数字） |
| `docs/superpowers/plans/`（backlog 登记文件，随实施时定名） | tech-debt 四条登记 |

### 2.6 验收判据

- **定向测试**：`bash scripts/build-test-run.sh --filter settings` 全绿；既有 settings 布局用例（含 `settings_effect_dropdown_fits_min_window` 类最小窗断言）不红。
- **几何断言**：新增/改造无头用例断言 130% 档下 settings 卡片标签行 `size().height == fs(16.0f)`（非恒真：把派生改回字面 16 即红——变异自检）。
- **变异自检**：回退 `fs()` 派生 → 上述用例红；`core::theme::setFontScale(1.0)` 档位下几何与改前逐值相等（100% 零视觉变化回归断言）。
- **截屏**：独立 `DEVDESK_DATA_ROOT` 启动、设置页切 130% 档、`screencapture -x` 对比 100%/130% 两档——标签行不贴边不裁切（对照 §2.6 段的 OpenGL 旧帧坑：动鼠标后拍第二张）。

### 2.7 风险与回滚

- 风险：`fs()` 派生使行盒变高，卡片总高随之变化，可能撞卡片内既有绝对 y 定位 → 实施时逐卡片核对行 y 累加链（与 detail pane「返回底沿」同口径，优先把链改派生而非钳制）。
- 回滚：改动集中在单文件的数值来源替换，`git revert` 单提交即回滚；零数据/配置迁移。

---

## 3. 项 B：高级配置 UI 恢复（#8）

### 3.1 背景与根因（含证据）

数据层 09-27 已全就绪，UI 从未接线：

- 值域与归一（审计 CPX-13）：`Profile::dialectOverride`（`src/module_claude_proxy/profile_repository.h:45`），值域 `'anthropic_default' / 'opencode_zen' / 空串=未覆盖`，权威定义 `src/module_claude_proxy/upstream_dialect.h:68-76`（`dialectKindFromText` 读侧兜底 / `dialectKindToText` 写侧规范）；写侧再归一 `normalizeDialectOverride`（`profile_repository.cpp:55`）。
- 服务层参数位已存在：`createProfile(..., const std::string& dialectOverride = std::string(), int64_t fallbackProfileId = 0)`（`claude_service.h:75-89`）。
- **接线缺口**：表单提交链把 dialectOverride **硬编码空串**（`claude_ui_form.cpp:308` `/*dialectOverride=*/std::string()`），编辑路径也不赋值（`claude_ui_form.cpp:349-357` 无 `p.dialectOverride`）；表单无该下拉。
- 任务书核对结论：**authField（认证字段三态）已有渲染控件**——下拉在基础页 `claude_ui_form.cpp:431-449`（`kAuthFieldLabels` items、状态 `formAuthField_`/`formAuthFieldOpen_`，`claude_ui_state.h:139-140`），提交已接线（`:307` `authFieldToText(f.formAuthField_)`）→ 按「若有，不动」处置，本项不迁不改。
- 备用 Profile（failover）下拉现状：渲染在**基础页**（认证字段之下，`claude_ui_form.cpp:451-509`：label/hint 455-463、候选 id 归算 464-485、dropdown 486-508；状态 `formFallbackProfileId_`/`formFallbackDdOpen_`，`claude_ui_state.h:164-165`；预填 `:91-92`）。任务书原文称其渲染于「高级 env 页」，实为**基础页**（`drawFormBasicTab` 内，grep 全文件无第二处）——按实际位置「从基础页挪入新 tab」设计，意图不变。

### 3.2 方案

**① 表单弹窗：第 4 个 tab「高级配置」**（D-B1/D-B2）

- tabs items（`claude_ui_form.cpp:960`）`{"基础","模型映射","高级 env"}` → 尾插 `"高级配置"`；分发分支（`:969-975`）加 `formTab_==3 → drawFormAdvancedTab(...)`。
- 新函数 `drawFormAdvancedTab`（`claude_ui_form.cpp`，静态，与其它三个 drawFormXxxTab 同签名风格），内容自上而下：

| 控件 | 规格 |
|------|------|
| 方言下拉 | label「上游方言」+ hint「网关行为差异（鉴权头/会话头/模型名标记）不一致时切换；默认按站点推断」；items `{"自动（按站点推断）", "anthropic", "opencode zen"}`（全角括号——审计指出原稿半角闭括号落在逐字拷贝路径上，已统一）；显示名**不手抄字符串**：`upstream_dialect` 新增纯函数 `dialectKindDisplayName(Kind)`（与 `dialectKindToText` 并排、单一来源，产出 "anthropic" / "opencode zen"），本 items 与 §3.2④ 详情 pane 文案共用；`dialectKindToText` 产内部值（`'anthropic_default'` 等）不上 UI；idx↔值映射 `0↔""`、`1↔"anthropic_default"`、`2↔"opencode_zen"`，**onChange 直接写规范文本**（映射表以 `dialectKindToText(Kind::AnthropicDefault/OpencodeZen)` 为源，不手抄字符串）；选中态逐帧归算：`formDialectOverride_` 经 `dialectKindFromText` 反查 idx（空/未知 → 0，读侧兜底同口径） |
| 备用 Profile 下拉 | **`claude_ui_form.cpp:451-509` 整段搬迁**（label/hint/候选归算/dropdown 逐字保留，含 `profiles` 快照入参与悬空归一逻辑），新 tab 内 y 坐标重排 |

- 新表单状态（`claude_ui_state.h`，紧邻 `formFallbackProfileId_`）：`std::string formDialectOverride_;`（默认空串）+ `bool formDialectDdOpen_ = false;`；`openCreateForm` 置空（`claude_ui_form.cpp:51-60` 区）、`openEditForm` 预填 `f.formDialectOverride_ = p.dialectOverride;`（`:71-92` 区）。

**② 基础页重排（failover 挪走后）**：原 failover 块占 y 380..438，其后元素整体上移 66px——提示行 `446→380`、错误行 `470→404`、接管警告 `492→426`（`claude_ui_form.cpp:511-538`）；认证字段下拉底（332+40=372）与新提示行 380 间距 8px，与既有行距一致。`:517` 文案「模型映射和高级 env 在另外两个标签页配置」→「模型映射、高级 env 和高级配置在另外几个标签页配置」。

**③ 提交链接线**（`submitForm`）：

- 新建分支 `claude_ui_form.cpp:308`：`/*dialectOverride=*/std::string()` → `f.formDialectOverride_`。
- 编辑分支 `:355-357` 区补一行 `p.dialectOverride = f.formDialectOverride_;`——**编辑路径透传既有字段**：现路径经 `svc->getProfile`（`claude_ui_form.cpp:343`）读回该字段并读侧归一（`profile_repository.cpp:171,191-192`），`updateProfile` 直写传入 struct 的该字段（`:484`，服务层纯透传）——现状天然保留覆盖，**不存在「编辑即清覆盖」的隐性 bug**（审计 2026-09-30 实证）；此行的作用是把新下拉的值真正写进编辑路径（不接线则下拉改动落不了盘），对「不动该下拉」场景行为与现状等价。
- 写侧归一：下拉只产规范值 + DB 层 `normalizeDialectOverride` 再兜底（`profile_repository.cpp:296,484` 已在写路径上）——双层保险，符合 CPX-13「写侧归一」口径，无需新增归一代码。

**④ 详情页「高级配置」只读 pane**（D-B3）

- tabs items（`claude_ui_detail.cpp:893-894`）插 `"高级配置"`（idx 3），`"最近调用"` 3→4；分发分支（`:931-944`）`detailTab_==3 → paneBottom = drawDetailAdvanced(...)`，原 `else`（最近调用）改 `==4`。**pane 函数沿用「返回内容底沿」契约**（`claude_ui_detail.h:13-20`），并走 P1 既有锚定/兜底链（`actY` 三档，`claude_ui_detail.cpp:945-959`）零改动——高级配置是固定行数内容，同基础 pane 的 ② 档路径。
- 只读两行（行样式复用基础 pane 的 label+值盒写法，`claude_ui_detail.cpp:71-114` 同款）：
  - **方言**：`DetailView` 新增 `std::string dialectText;`——compose 装配处（`claude_module.cpp:135` `DetailView dv;`）用纯函数 `UpstreamDialect::forBaseUrl(p.baseUrl, p.dialectOverride)` 算生效方言：显式覆盖 → `"anthropic"` / `"opencode zen"（显式）`；空 → `"自动（按站点推断）→ " + 生效方言名`（推断结果可审计，正是本 pane 的价值）。显示名统一走 `dialectKindDisplayName`（§3.2① 单一来源，不直接用 `dialectKindToText` 内部值）。
  - **备用 Profile**：`DetailView` 新增 `std::string fallbackName;`——compose 从既有 profiles 快照按 `fallbackProfileId` 解析名字（悬空/0 → `"无"`/`"（已删除）"`），与列表快照同源、pane 内不做服务调用（避免每帧 DB 查询）。

### 3.3 改动文件清单（预估）

| 文件 | 改动 |
|------|------|
| `src/module_claude_proxy/claude_ui_state.h` | `formDialectOverride_` / `formDialectDdOpen_` 两状态；tab 相关注释同步（:123「… / 3 最近调用」→ 4、:132 页签注释加「3 高级配置」，随 tab 数 4→5 更新） |
| `src/module_claude_proxy/claude_ui_form.cpp` | tabs 尾插 + `drawFormAdvancedTab` 新函数 + 基础页重排 + submitForm 两处接线 + openCreate/openEdit 预填 |
| `src/module_claude_proxy/claude_ui_detail.h` | `DetailView` +2 字段（dialectText / fallbackName）；:2-4 / :102-104「三 pane / 四个 pane / 四页」计数注释同步 |
| `src/module_claude_proxy/claude_ui_detail.cpp` | tabs 插入 + `drawDetailAdvanced` 新函数 + 分发分支；分发区「四个页签的 actY 必须相等」注释与 LOG_DEBUG tab 号口径同步 |
| `src/module_claude_proxy/claude_module.cpp` | `DetailView` 装配两字段赋值（:135 区） |
| `tests/test_modules/test_claude_ui.cpp` | 新用例 + `claude.detail.tabs.hit.3` 导航锚 10 处 +1（:3646 tab 循环上界 3→4）+ 原 `:2480` failover 用例先 click `form.tabs.hit.3`（D-B3 使最近调用 hit.3→hit.4；`detailTab_` 在测试零引用，见 §3.5 风险 3） |
| `tests/probes/claude_probes.cpp` | `claude.detail.tabs.hit.3` 2 处同步 +1（:248 tab 循环上界 3→4、:290；审计实测耦合点） |

服务层（`claude_service.*` / `profile_repository.*`）**零改动**；唯一例外：`upstream_dialect.*` 新增 `dialectKindDisplayName` 纯函数（§3.2①，只增不改、行为零变化）。

### 3.4 验收判据

- **定向测试**（`--filter claude`，headless 手法照 `claude_ui_fallback_dropdown_edit_persists`，`test_claude_ui.cpp:2480`）：
  1. 表单切到 tab 3，方言下拉三档可选，选「opencode zen」+ 保存 → `svc->getProfile(id).dialectOverride == "opencode_zen"`（新建与编辑各一）；
  2. 选「自动（按站点推断）」保存 → `dialectOverride == ""`；编辑已设覆盖的 profile 不动该下拉保存 → 覆盖值保留（**接线回归钉**：钉住透传保留语义、防未来表单重构丢字段——现路径本就保留，见 §3.2③）；
  3. failover 下拉在新 tab 内选择/保存/悬空归一行为与搬迁前逐断言等价（原 `:2480` 用例迁移到新 tab 仍绿）；
  4. 详情页切 tab 3 显示方言生效文本与备用名；`detailTab_==4` 仍是最近调用。
- **变异自检**：把 submitForm 的 `f.formDialectOverride_` 改回硬编码空串 → 用例 1/2 红；把 tabs 尾插改插中间 → 用例 4 红（非恒真）。
- **几何断言**：新 tab 内两下拉底沿 ≤ 弹窗底部按钮区上沿（dialog h=600 内容区约束）；详情 tab 3 的 paneBottom 不越 `contentBottom`（锚定链既有断言口径）。
- **截屏**：表单 tab 3 与详情 tab 3 各一张（独立数据根 + `screencapture -x`），确认布局不挤不叠、文案正确。

### 3.5 风险与回滚

- 风险 1：编辑路径补 `p.dialectOverride` 对「不动该下拉」场景**行为不变**（现路径经 `getProfile` 已隐式保留既有覆盖，实证 `profile_repository.cpp:171,191-192` / `:484`，审计 2026-09-30）——不存在「先红后绿」前提，用例 2 第三分支按**接线回归钉**定位（钉住保留语义，防未来表单重构丢字段），不做「修复隐性 bug」口径的先红后绿验证。
- 风险 2：与在飞 `fix/detail-env-pane` 同文件（`claude_ui_detail.cpp`）→ 串行施工（§2.4）。
- 风险 3：详情 tabs 插入使「最近调用」从 hit.3 位移到 hit.4——**真正耦合点是 `claude.detail.tabs.hit.3` 导航锚**（`detailTab_` 在 tests/ 零引用，只 grep 它会得出「测试零耦合」的错误结论）：
  - `tests/test_modules/test_claude_ui.cpp` **10 处** hit.3 点击需 +1 或改导航目标：`:1009, :1162, :1273, :1770, :3646`（tab 循环上界 3→4）`, :3691, :3742, :3818, :3963, :4024`；
  - `tests/probes/claude_probes.cpp` **2 处**：`:248`（tab 循环上界 3→4）、`:290`；
  - 另：原 `:2480` failover 用例需先 click `form.tabs.hit.3`（§3.4 判据 3 的迁移，已列入 §3.3 改动清单）。
- 回滚：单分支原子提交（显式 pathspec），revert 即回；无数据迁移（dialectOverride 列本就存在）。

---

## 4. 项 C：复制语义消歧 + 配置导出剪贴板（#9）

### 4.1 背景与根因（含证据）

- 现状「复制」= `duplicateProfile` 建副本（2026-09-27 spec §4 拍板，功能正常）：详情第 5 键 `claude.act.dup`（`claude_ui_detail.cpp:775-792`，文案 `"复制"`，warn `"复制失败"`）+ 列表右键菜单第 2 项（`claude_ui_list.cpp:786` `{"编辑"},{"复制"},{"删除"},{"设为当前"}`，`:808` `case 1: onDup(p.id)`）。用户预期是「把配置写进剪贴板」——纯文案歧义 + 剪贴板能力缺失。
- 序列化先例齐备：`buildDirectEnv(baseUrl, key, authField, models, legacyModel, extraEnv, sharedExtraEnv)` 纯函数（`provider_config.h:255`、`provider_config.cpp:406`）——BASE_URL 去尾斜杠 / 密钥键三态（authField → `ANTHROPIC_AUTH_TOKEN`/`ANTHROPIC_API_KEY`）/ 槽位 + 片段 + extra_env 合并；`runProfile` 已用它产 `{"env": {...}}` JSON（`claude_service.cpp:488-497`）。
- 剪贴板 API：`core::window::setClipboardText(const std::string&)`（`../EUI-NEO/core/window/window_backend.h:25`，无窗口句柄参数；EUI-NEO `components/input_model.h:557` 有直接调用先例）——DevDesk 业务代码尚未用过，本批为首个消费点。

### 4.2 方案

**① 改名「建副本」**（D-C1）：`claude_ui_detail.cpp:778` 文案、`:787` warn 标题；`claude_ui_list.cpp:786` 菜单项。动作 id / 回调 / `duplicateProfile` 行为零改动。

**② 新增「复制配置」**（D-C2）：

- **序列化（纯函数，UI 层）**：`module::claude` 新增自由函数（落 `claude_ui_logic.h/.cpp`，无头可测）：
  `std::string buildProfileExportJson(const Profile& p, const std::string& sharedExtraEnvJson, const std::string& keyPlaceholder)` —— 内部调 `buildDirectEnv(p.baseUrl, keyPlaceholder, p.authField, p.modelsConfigJson, p.model, p.extraEnvJson, sharedExtraEnvJson)` → `{"env": {...}}`（platform::json，手法同 `claude_service.cpp:490-497`）。**占位值**：`"sk-ant-***" + 尾 4 位`（尾 4 位与详情页 API Key 脱敏提示同源，已裁决 §5②；Keychain 读不到/为空 → 传空串，`buildDirectEnv` 空值不写凭据键，导出即无 token 键）。**坏 extraEnvJson 防静默丢键**：`buildDirectEnv` 对非法 extraEnvJson 读路径整体忽略（`provider_config.cpp:414-421`，与运行功能同口径）——compose 接线序列化前对 `p.extraEnvJson` 试解析，失败 → `LOG_WARN` 一行（写明导出将缺失该部分 env 键）并**继续导出**，不静默。
  - **不导明文 key 的理由**：剪贴板是跨应用共享面（粘贴进聊天 / issue / 截图即泄露，且离开 Keychain 后流向不可审计）；对比「运行」功能的临时 settings 明文落盘（profile-run D-8 接受的**功能性**代价，文件留在本机 `run-settings/` 且 24h 清扫），复制配置无此必要性——结构 + BASE_URL + 模型槽位 + extra env 已满足「把配置发给别人看/存档」的主诉求，token 由使用者自填。含明文变体**不做**（已裁决 §5①）。
- **动作接线**：`DetailActions` 新增 `std::function<void(const Profile& p)> copyConfig;`；compose（`claude_module.cpp`）实现：`buildProfileExportJson(...)` → `core::window::setClipboardText(json)` → toast「已复制配置」（成功）/「复制配置失败」（无选中，沿用 `warn` 通道）。`setClipboardText` 返回 void、模块层无窗口句柄无法回读校验（写失败为低概率已知限制，toast 仍发「已复制配置」）——接线处固定 `LOG_WARN` 一行（profile id + 导出 env 键数）作为 void API 无回读下的最低留痕。片段 env 取 `sharedExtraEnvJsonFor(p.sharedConfigId)`（claude_service 既有只读 getter，同 `drawDetailEnv` 用法 `claude_ui_detail.cpp:271-274`）。
- **入口与布局**（D-C3，§4.3）。

**③ 入口位置与按钮组排布**：详情操作组 6 键 → **7 键**（启用/测试连通/编辑/删除/建副本/复制配置/运行），`actW = (w - actGap * 6.0f) / 7.0f`（`claude_ui_detail.cpp:688-693` 公式与注释同步改，扣除 **6** 个 gap）；行高 36 / 副提示 / 分隔线不变 → `kActionBlockH=61` 不动，锚定链零改动。新键样式与中性键一致（surfaceElevated + 描边）。列表右键菜单 items → `{"编辑"},{"建副本"},{"复制配置"},{"删除"},{"设为当前"}`，`onSelect` index 映射同步（复制配置 `case 2` 派发新回调，删除 2→3、设为当前 3→4；`ProfileListActions` 新增 `requestCopyConfig`）。

**④ 服务层零改动**：`ClaudeService` / 仓库 / 代理 / `upstream_dialect` 全部不动；`buildDirectEnv` 是既有纯函数，剪贴板写入是 platform 调用——纯 UI 层序列化 + clipboard，符合任务书边界。

### 4.3 改动文件清单（预估）

| 文件 | 改动 |
|------|------|
| `src/module_claude_proxy/claude_ui_logic.h/.cpp` | `buildProfileExportJson` 纯函数 |
| `src/module_claude_proxy/claude_ui_detail.h` | `DetailActions` +`copyConfig` |
| `src/module_claude_proxy/claude_ui_detail.cpp` | 改名（2 处文案）+ 第 7 键 + `actW` 公式 |
| `src/module_claude_proxy/claude_ui_list.h/.cpp` | 菜单项改名 + 新项 + index 映射 + `ProfileListActions` +1 回调 |
| `src/module_claude_proxy/claude_module.cpp` | 两处回调接线（详情 copyConfig / 列表 requestCopyConfig）+ include `core/window/window_backend.h` |
| `tests/test_modules/test_claude_ui.cpp` | 序列化形状 / 改名 / index 映射 / 7 键几何用例 |

### 4.4 验收判据

- **定向测试**（`--filter claude`）：
  1. `buildProfileExportJson`：authField=bearer → env 含 `ANTHROPIC_BASE_URL`（尾斜杠已去）+ `ANTHROPIC_AUTH_TOKEN == "sk-ant-***1234"` 形态（占位，非明文）+ 槽位键 + extra env；api_key/both 三态键名各断言；绑定片段时冻结键以片段值出现；`{"env":{...}}` 顶层形状断言。
  2. 改名：`ui.find("claude.act.dup")` 按钮文本 == "建副本"；右键菜单 items[1] == "建副本"、items[2] == "复制配置"，`case 2` 派发 `requestCopyConfig(p)`、`case 3` 删除、`case 4` 设为当前。
  3. 7 键：点击 `claude.act.copyconfig.bg` 触发回调且参数 == shownCopy（headless `click` 手法同 `:2044`；headless 下 `setClipboardText` 行为实施首日实测确认安全 no-op，probe 真窗口后端会**真写系统剪贴板**——见 §4.5 风险 3）。
- **几何断言**：7 键 `size().width == (w - 72)/7` 且行右缘与容器右缘对齐（等分无溢出）；窄右栏（最小窗）下无键宽 < 文案宽（probe 实测，已裁决 §5⑥）。
- **变异自检**：占位改明文 → 用例 1 红；`/7` 改回 `/6` → 几何断言红；菜单 index 不改 → 用例 2 红。
- **截屏**：详情操作组 7 键 + 右键菜单展开各一张（独立数据根），确认布局与文案；真机粘贴剪贴板内容人工核一眼（占位非明文）。

### 4.5 风险与回滚

- 风险 1：7 键在最小窗右栏过窄（四字按钮「测试连通」「复制配置」）→ **已裁决 §5⑥**：probe 实测，不达标改**两行按钮组**（`kActionBlockH` 单常量传播）；「复制配置仅入右键菜单」不采用（主诉求不降级为隐藏功能）。
- 风险 2：占位值含尾 4 位——泄露面极低（与详情页既有脱敏同口径），**已裁决 §5② 保留尾 4 位**；固定 `"sk-ant-***"`（一处常量）留作退路。
- 风险 3：headless 测试环境无窗口后端时 `setClipboardText` 行为未知（backend stub vs 真写）→ 实施第一步实测 headless 路径（GLFW 未 init 按宏语义应早退为安全 no-op，未实测）；单测**经既有注入缝旁路**——headless 触发 `copyConfig` 回调断言接线，不真写剪贴板；probe（真窗口后端）点击 `claude.act.copyconfig.bg` 会**真写运行者系统剪贴板**（覆盖当前剪贴板内容，多人并行跑 probe 互相覆盖）——probe 说明注明该副作用。断言只钉 `buildProfileExportJson` 返回值与回调接线，剪贴板真写以真机验收为准（不写假绿断言）。
- 回滚：纯 UI 层，单提交 revert 即回；零数据/服务层波及。

---

## 5. 已裁决（主会话 2026-09-30 拍板，采纳审计建议）

| # | 问题 | 裁决 |
|---|------|------|
| ① | **「含明文 key」的复制配置要不要做** | **不做**（维持 D-C2）。剪贴板是跨应用共享面，明文 key 一经粘贴（聊天/issue/截图）即离开 Keychain 托管、流向不可审计；若未来确有需求另立小 spec（二次确认 + 粘贴后自动清剪贴板），不在本批搭便车 |
| ② | 占位值含尾 4 位还是固定 `sk-ant-***` | **含尾 4 位**：`sk-ant-***<尾4>`，与详情页 apiKeyHint 同一脱敏口径（同面展示、不扩大泄露面）；固定常量形态保留为一处退路（§4.5 风险 2） |
| ③ | settings 卡片派生化随本批还是整批 tech-debt | **随本批**：单文件数值来源替换、与 B/C 零文件交叠、变异自检现成（`fs(16)` 断言回退即红），本批唯一无风险增量 |
| ④ | 表单第 4 tab 名称与顺序 | **尾插 + 与详情 pane 同名「高级配置」**：尾插保住前三页序（§6.1 既有拍板），既有 `form.tabs.hit.0/1/2` 测试导航零破坏；插「高级 env」前需 ~10 处测试返工，不值 |
| ⑤ | 复制配置是否进列表右键菜单 | **进**：与既有四动作全在菜单对称；菜单当前零测试耦合，重映射成本即 case 表；兼作 ⑥ 不达标时的兜底入口 |
| ⑥ | 详情操作组 7 键在最小窗右栏的键宽 | **probe 实测裁决；不达标改两行按钮组**（备选方案：`kActionBlockH` 单常量传播 61→约 104，锚定链 `kActReserve` 自动跟随，代价内容框底上移 ~43px）。不采用「仅入右键菜单」——复制到剪贴板是用户主诉求入口，不降级为隐藏功能 |
