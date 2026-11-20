# UI/UX 设计专家审计报告（2026-11-21）

> 审计对象：`/Users/coderstory/dev/DevDesk`。性质：**只读审计**——未修改任何界面代码、未构建、未运行、未启动 GUI。
> 本报告覆盖并校正同日主会话的占位版本（该版基于 2026-10-29 旧截图，未发现侧栏裁切问题）。

---

## 0. 审计范围与方法

**判据尺子**：`docs/UI-UX-DESIGN.md`（1,478 行：IA / 布局 / 组件 / 交互 / 动效 / 快捷键 / A11Y）、`docs/ACCESSIBILITY.md`（现状声明）、`docs/USER_GUIDE.md`（310 行）、`src/ui_kit/` 组件契约（`dialog_shell.h` / `metrics.h` / `empty_state.h` / `toast.h`）、老板裁决的两档字重制与卡片统一半透。

**看的截图（13 张，含 2026-11-18 现状与 2026-10-29 旧版对照）**

| 截图 | 版本 | 用途 |
|---|---|---|
| `docs/screenshots/app-home.png` | 10-29 旧 | 首页旧构图（含日期副题） |
| `docs/screenshots/app-todo.png` | 10-29 旧 | Todo 旧版（有副提示行、四色卡） |
| `docs/screenshots/app-markdown.png` | 10-29 旧 | 笔记旧版（侧栏 6 项） |
| `docs/screenshots/app-claude.png` | 10-29 旧 | Claude 旧版（六按钮一行） |
| `docs/screenshots/app-about.png` | 10-29 旧 | 关于页（橙色光斑） |
| `evidence/2026-11-18-need294-closure/light/01-home-1000.png` | **现状** | 首页 / 侧栏裁切取证 |
| `.../light/02-markdown-1000.png` | **现状** | 笔记 / 树空态文案 |
| `.../light/04-todo-1000.png` | **现状** | Todo 空看板 |
| `.../light/13-release_tasks-1000.png` | **现状** | 上线分组展开态（子项被切） |
| `.../light/15-settings-1000.png` | **现状** | 设置页 |
| `.../dark/06-claude_proxy-1000.png` | **现状** | Claude 深色错误态（红字取证） |
| `evidence/unify-batch4/after-claude-list-light.png` | **现状** | Profile 列表命名形态 |
| `evidence/unify-batch4/after-schedule-list-dark.png`、`evidence/20261120-bug354/after-light.png` | **现状** | 定时 / 终端深色 |

**读的代码**：`src/app_registry.h`、`src/app_sidebar.cpp`(+`.h`)、`src/core/router.cpp`、`src/core/theme.h`、`src/core/tray_menu_spec.h`、`src/main.cpp`（键盘 / 窗口下限）、`src/ui_kit/metrics.h`、`dialog_shell.h`、`dialog_form.h`、`toast.h`、`empty_state.h`、`button.cpp`、`src/module_todo/*`、`src/module_markdown/*`、`src/module_claude_proxy/*`、`src/module_home/home_module.cpp`、`src/module_settings/settings_ui_cards.cpp`、`src/module_about/about_module.cpp`、`EUI-NEO/core/input/input_types.h`、`EUI-NEO/core/dsl.h`。

**关键校订**：`docs/screenshots/app-*.png` 全部 mtime = 2026-10-29，与现状差 23 天（侧栏 6 项 → 15 行、Claude 六钮 → 三钮、Todo 副提示行已删）。**凡「当前长什么样」的判断一律以 `evidence/2026-11-18-*` 为准**，旧图只用于对照演化。

---

## 1. 结论摘要

| 档 | 条数 |
|---|---|
| **P0** | **1** |
| P1 | 8 |
| P2 | 10 |
| P3 | 5 |

**总体判断**：底层设计系统（token 分层 / 弹窗标准壳 / 空态组件 / toast 四态 / 危险确认变体）已达到中小团队产品的水准，明显超出个人项目；问题几乎全部集中在**上层装配与规范落地**——最典型的是侧边栏：老板 11-19 把宽度从 220 收到 200（`app_sidebar.h:17`）压了横向，但垂直方向 15 行塞进一个「滚动条视觉全无」的内滚区，导致「设置」以下在 1438×801 这种最常见笔记本窗口下被裁出视口，`关于` 直接不可达。这是本次唯一 P0。

一句话：**组件层是好的；侧栏这一块装配把「能滚」当成了「用户知道能滚」。**

---

## 2. P0 清单

### U-01 侧栏末尾条目被裁出视口，且无任何可滚提示——「关于」在常见窗口高度下不可达

- **位置**：
  - `src/app_sidebar.cpp:683-743`（工作项与 Settings/About 排进**同一张**滚动规划表 `navPlan`）
  - `src/app_sidebar.cpp:917-927`（`scrollbarOverlay(true).scrollbarWidth(0.0f)`，注释 `bug188` 明确「滚动条**任何视觉全无**」）
  - 规范原意：`docs/UI-UX-DESIGN.md:24-25`「设置放在侧边栏**底部（独立视觉块）**，遵循 macOS 系统偏好设置习惯」、`:686`「侧边栏宽度固定，不可拖拽调整」
- **问题**：15 行导航（13 平铺 + 分组父项 + 展开后 2 子项）按 `itemH 40 + gap 4` 步进，内容高约 656px；在 1438×801 窗口下视口高约 599px（`availH 771 − toggleZone 56 − navStart 116`）。超出 57px，且**无滚动条、无渐隐遮罩、无自动滚动到当前项**（全文件检索 `ensureVisible` / `scrollTo` / `reveal` 零命中）。规范要求的「系统项独立锚底」在实现里被并进了滚动流。
- **取证**：
  - `evidence/2026-11-18-need294-closure/light/01-home-1000.png`（2876×1602 @2x = 1438×801 逻辑）：侧栏止于「设置」，**「关于」不存在于画面**
  - 同批 `13-release_tasks-1000.png`：「基础配置」被底部折叠钮切断半个字形
  - `15-settings-1000.png`：同样看不到「关于」
- **影响**：**卡死一条完整操作路径**。侧栏是「设置」「关于」的唯一入口；用户既看不到条目、也没有任何提示可以滚（滚轮能滚但界面零线索），等于这两个页面在大量常见窗口尺寸下不可达。这同时使 `USER_GUIDE.md:42`「列出全部已启用模块入口」失效。折叠态（72px，只显图标）下更严重——图标无文字、hover 无 tooltip（全文件 `tooltip` 命中 0，与 `USER_GUIDE.md:43`「悬停出名称 tooltip」**不符**），用户连被裁的是哪一项都无从知晓。
- **建议**（三选一，按侵入度排序）：
  1. **复原规范原意**：把 Settings/About 从 `navPlan` 剥离，锚在折叠钮上方独立块；业务项滚动区下界由该系统块上缘推出（改动集中在 `app_sidebar.cpp:683-750`，几何公式不变）；
  2. 保留同一滚动流，但给滚动区加可视化：顶部/底部 16px 渐隐遮罩（`app_sidebar.cpp:915` 注释称「EUI 无渐变 API，偏差已记池」——可用三段不同 alpha 的半透明矩形近似）；
  3. 至少给 `scrollbarWidth` 一个 2px 细条 + 折叠态补 hover tooltip（`components::tooltip`，同文件 `todo_ui_board.cpp:612-617` 已有现成用法）。
  - 附带必做：当前页不在视口内时自动滚入（`g_navScrollOffset` 有写口，`compose` 里加一次 clamp 后按 currentPage 行 y 回写即可）。

---

## 3. P1 清单

### U-02 同一模块三套名字并存，用户无法把侧栏 / 设置卡 / 指南对上

- **位置**：侧栏取 `displayName()`（`src/app_sidebar.cpp:899`）→ 实拍 `01-home-1000.png` 显示「Markdown / Todo 看板 / 定时调度 / Claude 配置」；设置卡另一套（`src/module_settings/settings_ui_cards.cpp:438-455`）：「Markdown / Todo 看板 / **定时** / **Claude 配置切换器**」；登记表 `shortName` 第三套（`src/app_registry.h:51-55`）：「**笔记** / **待办** / **定时** / **Claude**」；用户指南第四套（`docs/USER_GUIDE.md:64,107,139`）：「**笔记（Markdown）** / **待办（Todo 看板）** / **Claude 配置切换器**」。
- **问题**：Claude 域一个东西出现**四种**称法：「Claude」（侧栏）／「Claude 配置」（页头 `claude_module.cpp:43`、`claude_ui_list.cpp:403`）／「Claude 配置切换器」（设置卡、指南）／「切换 Claude Profile」（首页按钮 `home_module.cpp:324`）；实体层面还有「Profile / 配置 / 环境」混用（`unify-batch4/after-claude-list-light.png` 示例名「生产环境」「测试环境·备用节点」）。Markdown 侧则「Markdown」与「笔记」并存。
- **影响**：老板按指南找「笔记」，在侧栏找不到名为「笔记」的条目；口头沟通、日志、截图三处说法互不映射；新用户无法建立「侧栏 Markdown = 指南的笔记」这一等价关系。
- **建议**：确立**一模块一显示名**单一来源——`kModuleRegistry` 的 `shortName` 升为唯一权威列，侧栏 / 页头 / 设置卡 / 指南 / 首页按钮全部引用它；Claude 域固定「Claude 配置」，内部实体固定「Profile」，示例数据不用「环境」一词。指南在标题处只写一个名字，不要「笔记（Markdown）」这种双写。

### U-03 设计规范承诺的全局快捷键 ⌘1–⌘4 / ⌘, 完全未实现

- **位置**：规范 `docs/UI-UX-DESIGN.md:433-440`（切 Home/Markdown/Todo/Claude 与设置表）+ `:471-473`（「所有按钮 / 菜单项右侧标注快捷键」「⌘/ 打开快捷键帮助 modal」）；实现 `src/main.cpp:1194-1206`（全局键只拦 `Ctrl/⌘+B`，其余交 `router->dispatchKeyEvent`）、`src/core/router.cpp:84-89`（只派发给**当前页**模块，架构上不具备跨页跳转能力）。
- **问题**：全仓检索 `InputKey` 在 `main.cpp` 只出现 `::B` 一处；无数字键、无逗号、无 ⌘/。路由层设计（`dispatchKeyEvent` 只找 `pageId() == current_`）决定了「全局切页」无处安放。
- **影响**：规范与指南承诺的全部「专业用户高频操作」落空（且指南 §2 只提 Ctrl+B，等于文档自己也承认只此一个）。文档不可信 = 后续所有「按规范验收」失去锚点。
- **建议**：在 `main.cpp` 的 `onKeyEvent` 全局分支补：按 `kModuleRegistry` 前四项映射 `Ctrl/⌘+1..4`、`Ctrl/⌘+,` → Settings、`Ctrl/⌘+/` → 快捷键说明浮层（复用 `components::tooltip` 或 dialog shell）。**未实现前先把规范 §8.1 标注「目标形态·未实现」**，与 `ACCESSIBILITY.md` 的处理方式一致，不要两套事实并存。

### U-04 快捷键提示文案两平台都写死「Ctrl」，macOS 上按提示操作无效

- **位置**：`src/module_todo/todo_module.cpp:1036`（「先点击卡片选中任务，再用 **Ctrl**+↑/↓ 移动」）、`src/module_markdown/markdown_module.cpp:297`（tooltip「快捷键：**Ctrl**+Shift+P 切换视图、**Ctrl**+N 新建笔记、**Ctrl**+S 保存」）；判定侧走 `modifiers.shortcut()` = macOS ⌘ / Windows Ctrl（`EUI-NEO/core/input/input_types.h:54-60`）。旧版截图 `docs/screenshots/app-todo.png` 页头第二行同样是「Ctrl+↑/↓」。
- **问题**：文案硬写 Ctrl、判定随平台变——**文案与判定不同源**。全仓无 `shortcutLabel()` 之类的单一呈现工具（检索 `"⌘"` / `"Ctrl"` 均无集中常量）。
- **影响**：macOS 用户（本机就是 macOS）按提示按 Ctrl 无效，会判为 bug；平台惯例破功。
- **建议**：加 `core::ui_text::shortcutLabel()`（macOS→⌘、Windows→Ctrl），所有提示 / tooltip / 菜单右标注 / 指南全部走它；CI 加一条「模块内字符串含 `"Ctrl+"` 直接出现在 UI 文案」的轻量守卫。

### U-05 内部实现术语泄漏进用户可见的错误提示

- **位置**：`src/module_claude_proxy/claude_ui_list.cpp:664`（红字 `"Profile 列表加载失败：" + profilesError`）；同类 `src/module_todo/todo_module.cpp:231`（`"服务未就绪（TodoService 未注册）"`）。
- **取证**：`evidence/2026-11-18-need294-closure/dark/06-claude_proxy-1000.png` 左栏整块红字：**「Profile 列表加载失败：服务未就绪（ClaudeService 未注册）」**；右侧详情同时空白到只有字段标签，页右下 toast 才补「Claude 服务未装配 / 本会话内开启的模块需重启后才能提供代理服务」。
- **问题**：把 C++ 类名 `ClaudeService` / `TodoService` 直接抛给用户；且该错误态**没有就地出路**（无「重试」按钮，唯一提示在另一个角落的 toast 里）。违反自家规范 `docs/UI-UX-DESIGN.md:265-270`「用中文 + 具体原因」「可恢复：提供重试」。
- **影响**：老板是非 C++ 用户，看到类名会直接卡住并放弃排查；「一整块红字 + 一片空白」是最难看的错误态形态。
- **建议**：错误层加一次「用户语」映射表：`ClaudeService 未注册 → 「Claude 配置服务未启动，请重启 DevDesk 后重试」+ 就地【重试】`；原始 `profilesError` 只进日志与悬停详情。同时给错误态补加载失败骨架，不要让右栏全空。

### U-06 设计规范几何值三方漂移，规范已失去「尺子」资格

- **位置与实测**：

  | 项 | 规范 `docs/UI-UX-DESIGN.md` | 用户指南 `USER_GUIDE.md` | 实现 |
  |---|---|---|---|
  | 侧栏宽 | **260 px**（`:15,67,686`「固定，不可拖拽」） | **220 / 72**（`:42`） | **200 / 72**（`app_sidebar.h:17-18`，need372 老板 11-19 裁决） |
  | 最小窗口 | **1200×720**（`:58`） | — | **1000×640**（`main.cpp:912-913`） |
  | 默认窗口 | 1600×1000（`:57`） | — | 1600×1000（一致 ✅） |

  另：`src/app_sidebar.cpp:543` 注释仍写「最小窗 1200×720 → H=680 落此档」，与 `main.cpp:912-913` 矛盾；`app_sidebar.cpp:533` 日志串写「宽 220/72 可折叠」，实际 200。
- **问题**：同一参数四份文件四个值，连代码内部注释都未同步。规范 §1.1 的 IA 图仍只列 5 个模块（`:14-21`），与现状 15 行完全脱节。
- **影响**：QA 拿哪份对？任何「间距/宽度不符」的判词都无法裁决；历史已多次因此拉锯。
- **建议**：规范只保留 token 名与引用（`core/theme.h` / `ui_kit/metrics.h` / `app_sidebar.h`），删除所有字面量复制；在文档头加一行「本表的数值以代码常量为准，改动须同步本表」。同时重写 §1.1 为当前 15 行结构（这一项需老板确认分组方案，见 U-10）。

### U-07 Toast 时长与规范脱节，且单实例无队列——连续操作结果互相覆盖

- **位置**：`src/ui_kit/toast.h:29`（`kToastDurationSec = 3.5f` 全站统一）、`src/ui_kit/toast.h:38-40`（注释明示「EUI-NEO toast 是**单实例（无队列）**，单实例节流语义由这些位承载」）；规范 `docs/UI-UX-DESIGN.md:276-278`（成功 5s / **错误 8s**、**最多 3 个垂直堆叠**、hover 暂停倒计时）。
- **问题**：错误提示只活 3.5s（规范要 8s，理由是「更严重」）；第二条 toast 直接顶掉第一条，批量保存 / 批量删除时用户只看到最后一条结果。
- **影响**：用户错过错误提示 = 不知道操作失败；规范里「错误可记录、可恢复」在反馈层被削弱。
- **建议**：`ToastLevel::Error` 独立时长 8s；单实例下改为「合并计数」文案（「已保存 3 项（1 项失败）」）或引入最小队列（≥3 条）。两条都实现前，先把规范改成 3.5s 单实例以免文档撒谎。

### U-08 首页图表卡空态吃掉首屏 60% 面积——构图级无效空白

- **位置**：`src/module_home/home_module.cpp:718-776`（图表卡固定大高度 + 整卡空态「今日暂无 API 调用 / 启动 Claude 代理后将展示用量统计」）。
- **取证**：`evidence/2026-11-18-need294-closure/light/01-home-1000.png` 与 `docs/screenshots/app-home.png` 两版一致——卡片高约 600px（占内容区 60%+），正中仅两行灰字；五张统计卡被挤在上沿约 150px 内。
- **问题**：新用户默认态下，「视觉权重最大的一块」给了零数据。开工三问逐一不过：
  - ② 大段无效空白 → 是，且是整卡级；
  - ③ 信息密度与视觉权重平衡 → 权重倒挂（零信息占最大权重，高频统计挤在顶部）。
- **影响**：首屏「一屏看清今日开发状态」的定位被空图吃掉；新用户第一印象是「应用没数据 / 坏了」。
- **建议**：空态降为一行摘要条（≤96px）：「今日暂无 API 调用 · 启动 Claude 代理后显示分布」+ 就地【去启动】；有数据时再展开成图表（`contentHeightProvider` 式的高度自适应，同 `dialog_shell.h:59-63` 思路）。折叠态记忆保留（现有 collapse 机制已在）。

### U-09 空看板 / 空列无行动入口——违反自家空态四件套

- **位置**：`src/module_todo/todo_ui_board.cpp:628-640`（空列只给 `opts.message`，注释记载 H20「空列『+ 添加任务』入口删除——创建入口唯一」）；对照规范 `docs/UI-UX-DESIGN.md:249-261`（图标 48px + 标题 + 副标题 + 主操作按钮）与 `src/ui_kit/empty_state.h:10-14`（「防 §4.12 禁止项：空状态无操作入口」）。
- **取证**：`evidence/2026-11-18-need294-closure/light/04-todo-1000.png`——三列各只有居中一行「暂无任务」，无图标、无引导句、无按钮；与同批 `13-release_tasks-1000.png` 的上线任务空态（图标 + 主句 + 引导句 + 新建按钮）形成直接对比。
- **问题**：同一产品内两种空态密度；Todo 空列连规范要求的图标都省了。且「创建入口唯一在页头」在空看板时是有问题的——用户的注意力在列里。
- **影响**：清空看板后没有「下一步在哪」的暗示；直接违反 `empty_state.h` 自己写下的禁止项。
- **建议**：空列补图标 + 引导句（「点右上角『新建任务』创建第一个」）；若坚持单一入口，则空看板整体（非单列）给一次带按钮的空态，列内保留单行。至少不能三列都只留四个字。

---

## 4. P2 清单

### U-10 侧栏 13 个顶级入口平铺无分组——也是 P0 的结构性成因
- **位置**：`src/app_registry.h:49-67`（除「上线」外全部 `parent = nullopt`）；分组渲染机制 `src/app_sidebar.cpp:767-839` **已实现但只用了一处**。
- **问题**：AI 类（Claude / 智能体 / 记忆 / MCP）、开发类（Git / 终端 / Markdown）、生活类（Todo / 定时 / 壁纸 / 上线）混在一条长龙里，靠滚动找入口。
- **影响**：可发现性差，且 15 行是 P0 裁切的直接来源。模块只会继续增长。
- **建议**：复用既有分组树，收敛为「工作 / 开发 / AI」三组 + 底部系统项（Settings/About），一级入口降到 5–6 个。**此项需老板裁决**（是接受平铺，还是接受分组带来的多一次点击）。

### U-11 两个齿轮图标重复，「基础配置」与「设置」无法区分
- **位置**：`src/app_registry.h:65`（ReleaseConfig `U+F013 fa-gear`）与 `:66`（Settings 同 `U+F013 fa-gear`）。
- **取证**：`evidence/.../light/13-release_tasks-1000.png` 侧栏里两者上下相邻。
- **影响**：折叠态（只显图标）时完全不可分辨；展开态也削弱层级。
- **建议**：基础配置换 `fa-sliders`（0xF1DE）或 `fa-cube`。

### U-12 弹窗宽度有一条路径绕过 `metrics.h` 单源
- **位置**：`src/module_agent/agent_module_dialogs.cpp:471`（`std::max(640.0f, screen.width * 0.5f)`）vs `src/ui_kit/metrics.h:17-40`（五档枚举 + `dialogWidthPx`，其余 39 处调用全部走枚举）。
- **影响**：出现第六种宽度语义（半屏）+ 硬编码 640；字号放大后弹窗重排会漏掉此路径。
- **建议**：改 `DialogWidth::Wide`；若确需半屏，新增枚举档并写入规范表。

### U-13 硬编码字号绕过 `scaled()`——字号缩放档对记事本失效
- **位置**：`src/module_notepad/notepad_window.cpp:77,109,654,690,725,778,788,827,1253,1325,1346,1448`（裸 `fontSize(12.0f/13.0f/14.0f)`）；契约见 `src/core/theme.h:118-122`（「硬编码字面量须经 `scaled()` 包裹才随档位缩放」）。
- **影响**：设置页字号调到 130% 时，记事本窗文字不放大——老板明确要求过全站字号缩放覆盖。
- **建议**：统一改 `t.sizes.fontSizeSmall/Body` 或 `core::theme::scaled()`；守卫加一条「模块目录下裸 `.fontSize(<数字>f)` 告警」。

### U-14 主按钮高度 44 / 40 两档并存
- **位置**：首页 `src/module_home/home_module.cpp:1011`（44）vs 其他页 40（`todo_ui_board.cpp:124` 新建任务、`markdown_module.cpp:485` 新建笔记、`claude_ui_detail.cpp:560` 三动作均 40）；规范 `docs/UI-UX-DESIGN.md:88-90` 定「主按钮 44 / 次按钮 40」。
- **影响**：同语义控件跨页高度不一致，切页时有轻微跳动；规范与实现各自只对一半。
- **建议**：二选一收敛（要么全 44，要么规范改 40 并写清理由），禁止「首页 44、别处 40」。

### U-15 加载态覆盖不均：四个模块零加载反馈
- **位置**：`src/module_schedule/`、`src/module_mcp/`、`src/module_terminal/`、`src/module_memory/` 检索「加载中 / spinner / skeleton」**均 0 命中**；而 `module_git/git_views.cpp:155`（`skeletonPulseOpacity`）、`module_release/release_list_ui.cpp:389`（`drawSkeletonBody`）、`module_agent` 均有骨架屏。规范 `docs/UI-UX-DESIGN.md:216-245`。
- **影响**：定时 / MCP / 记忆面板首帧与刷新时直接空白到出数据，慢盘时用户会误判「无数据」或「坏了」。
- **建议**：四模块首载加最小骨架（3 行占位色块即可，复用 `empty_state` 同款视觉语言），刷新用行内 spinner；不要复用一个空态文案冒充加载。

### U-16 空态文案指向已不存在的控件
- **位置**：笔记树空态「还没有笔记 / 点**右上角 +** 开始」；取证 `evidence/.../light/02-markdown-1000.png`。当前页头右上角是文字按钮「新建笔记」，「+」是树工具条上的方形按钮（`docs/screenshots/app-markdown.png` 底部还有「+ 笔记 / + 文件夹」）。
- **影响**：用户按「+」去找会找错位置；属于文案与控件不同源。
- **建议**：直接引用控件 label（「点右上角『新建笔记』」），或去掉方位词只留动作。

### U-17 「关于」页面向开发者而非用户，且巨型渐变是孤例
- **位置**：`src/module_about/about_module.cpp:226-231`（`Git commit` / `编译器` / `平台` / `C++ 标准`）；取证 `docs/screenshots/app-about.png`。
- **问题**：① 整页两张卡，最大的信息量是「拥抱版本号 / AppleClang 21.0.0 / C++20 / EUI-NEO rev d84e1be」——老板不写 C++，此页对用户零价值；② 整页橙色光斑铺底，其余 13 页均为纯色 + 半透卡片，翻到此页像进了另一个应用（与「卡片统一半透、克制装饰」口径冲突）。
- **建议**：首屏放版本 + 数据目录（可点开）+ 文档/反馈入口；构建信息折叠进「诊断信息（复制）」次级区。光斑要么删，要么升级为全站统一处理并写进规范。

### U-18 页头窄档弹性规则只存在于代码注释，规范缺位
- **位置**：`src/module_todo/todo_ui_board.cpp:99-105`（注释记载 bug311「内容宽 536 时标题只剩 80px，『Todo 看板』被截成『Todo 看』」）、收窄公式 `:118-128`；笔记侧 `src/module_markdown/markdown_module.cpp:379-386`（`headerLayout` 紧凑档）。规范 `docs/UI-UX-DESIGN.md:55-77` 无对应断点规则。
- **影响**：每加一个页头控件就要再写一轮抢宽度补丁（bug311 / bug312 已是第二次）。
- **建议**：规范补一节「页头窄档弹性规则」（优先级：标题 ≥148 → 搜索框收 140 → 主按钮保 120），各页共用同一 helper。

### U-19 弹窗 z 序六档裸数字，无具名层级
- **位置**：990（`schedule_ui_history.cpp:446`）、1000（组件默认 + `release_dialogs.cpp:338`）、1100（`agent_module_dialogs.cpp:2133,2281`）、1200（`release_dialogs.cpp:503`、`wallpaper_delete_bridge.cpp:116`、`quit_save_bridge.cpp:80`）、1300（`release_detail_draw.cpp:2426,2532,2691`）；唯一具名常量 `dialog_form.h:24` 的 `kDialogZIndexUnset = -1` 是哨兵不是层级。
- **影响**：跨模块叠弹窗谁盖谁靠约定（bug352 即此族）；990 这种「压到默认之下」的手段意图无注释。
- **建议**：`ui_kit/metrics.h` 建层级枚举（`kZBase < kZDialog < kZDialogStacked < kZOverlay`），全调用点引用；异常值补意图注释。

---

## 5. P3 清单

### U-20 笔记四模式命名语义难分
- **位置**：`src/module_markdown/markdown_module.cpp:420-421`（`{"编辑", "实时", "双栏", "预览"}`）。
- **问题**：「实时」与「双栏」都不说明差异（推测为单栏实时渲染 / 编辑+预览并排）。
- **建议**：改「仅编辑 / 编辑即预览 / 左右分栏 / 仅预览」，或加 tooltip 一句说明。

### U-21 Todo 列头高与圆角自成一档
- **位置**：`src/module_todo/todo_ui_logic.h:30`（`kColumnHeaderHeight = 56.0f`，而全站页头 `ui_kit::kHeaderH = 40`）、`todo_ui_board.cpp:480`（`radius(6.0f)`）、`:1046`（`radius(8.0f)`）、`:111`（列容器 `12.0f`）——同页 6/8/12 三种半径，token 只有 6/10/14（`core/theme.h:86-90`）。
- **建议**：列头圆角统一 `radiusSmall`、列容器 `radiusLarge`；列头高提为语义常量并进 `metrics.h`。

### U-22 无障碍：Tab 焦点遍历与屏幕阅读器未实现（已诚实声明，但用户侧无告知）
- **位置**：`docs/ACCESSIBILITY.md:12-20` 明写「当前不支持屏幕阅读器」「全键盘导航**未达成**：EUI-NEO 的 Tab 焦点循环未实现」；实现佐证：`src/app_sidebar.cpp:852-860` 导航项只挂 `onClick`（`EUI-NEO/core/dsl.h:705-712` 置 `interactive` 而非 `focusable`），全库检索 `InputKey::Tab` 无焦点调度。
- **说明**：这是**已立项并如实声明的 P2 backlog**，不算隐瞒（加分项）。真正的缺口是 `USER_GUIDE.md` 面向用户的部分未告知——依赖无障碍的用户只能用到才发现。
- **建议**：指南 §5 常见问题加一条「DevDesk 目前不支持屏幕阅读器与全键盘 Tab 导航（现状见 docs/ACCESSIBILITY.md）」；侧栏导航项补 `focusable + Enter/Space` 至少覆盖最关键的转页路径。

### U-23 折叠钮 hover 粒子成本高于收益
- **位置**：`src/app_sidebar.cpp:1068-1085`（hover 迸发 12 颗粒子、生命 1600ms、每帧 ≤12 个矩形元素）。
- **影响**：低频动作（折叠侧栏）配最长动效，与规范动效档上限 300ms（`UI-UX-DESIGN.md:288-299`）偏离。
- **建议**：降为 3–4 颗 / 400ms，或直接去掉，保留已有的 1.09 scale + 阴影反馈。

### U-24 截图证据链腐烂 23 天
- **实测**：`docs/screenshots/app-*.png` 全部 mtime = **2026-10-29**；与现状差异：侧栏 6 项 → 15 行、Claude 六钮 → 三钮、Todo 副提示行已删、侧栏文案全改。
- **影响**：验收 / 回归 / 对外展示用 23 天前的物料 = 验的不是现在的东西；本次审计也因此必须先找 `evidence/2026-11-18-*` 才能判 P0。
- **建议**：截图纳入自动化重生（仓内已有截图断言能力），每批 UI 收口重拍；文件名或伴随 md 标注 commit hash。

### U-25 空态图标仍用裸 FontAwesome 码点
- **位置**：`src/ui_kit/empty_state.h:34`（`std::optional<unsigned int> icon`）；调用侧如 `todo_ui_board.cpp:281`（`0xF00B`）、`release_list_ui.cpp` 等。
- **建议**：抽 `ui_kit/icons.h` 具名常量（`kIconTableColumns` / `kIconRocket`…），消灭散落十六进制。

---

## 6. 分维度小结

- **信息架构（IA）**：13 个顶级入口平铺进 200px 侧栏、分组机制只用了一处、「系统项独立锚底」未实现——这是 P0 的结构性根源。规范 §1.1 仍停留 5 模块时代（U-06/U-10）。
- **导航与可达性**：首页作为导航枢纽（统计卡 / 最近编辑 / 快捷按钮三类跳转）、Ctrl+B 折叠、分组父项语义清晰，是强项；真实缺口是**入口本身可能看不到**（P0）与全局快捷键承诺落空（U-03）。
- **视觉层级**：标题 24/600 与正文 16/400 两档制落实，卡片半透与页面底分区分明；失衡集中在首页空图表卡（U-08）与 Todo 空列（U-09）——是构图级不是参数级问题。
- **状态与反馈**：空态组件化（`empty_state.h` 记废四种历史形态）与危险确认体系（`dangerConfirm` 四处同款）做得扎实；加载态覆盖不均（U-15）、toast 时长与队列脱节（U-07）、错误态无出路（U-05）是主要缺口。
- **文案**：内部类名泄漏（U-05）与跨平台 Ctrl 硬写（U-04）最伤；命名四套并存（U-02）让文档与界面无法互相映射。`USER_GUIDE` 的「出错了看哪」三栏表格是罕见的好实践，但被底层错误文案拖累。
- **一致性**：token 体系扎实（弹窗宽度 / 间距 / 半径 / 语义色都有单源，`ui_kit` 层硬编码弹窗宽 0 处）；漂移全部出在**模块自绘层**——agent 半屏弹窗（U-12）、记事本裸字号（U-13）、首页 44px 主按钮（U-14）、Todo 三档半径（U-21）。
- **无障碍/可用性**：对比度体系（`primaryText` / `errorText` / `successText` / `onAccent` 分槽并附实测比值）超出常规水准；缺口在 Tab 焦点遍历与折叠态无 tooltip，但均已如实声明（U-22）。
- **平台惯例**：macOS 自绘壳按 Windows 构图排布（`app_sidebar.cpp:402-407` 注释明示「视觉从红绿灯彩点改为 Windows 同款」）、Dock/托盘左键唤起右键菜单、Cmd 快捷键判定正确——这些是既有裁决，不列为问题；只有文案层的 Ctrl/⌘ 不分（U-04）是真的破功。

---

## 7. 做对了的部分

1. **弹窗几何单一来源，且是真正的设计系统**：`src/ui_kit/metrics.h:8-62` 把宽五档（420/460/640/720/760）、头 69 / 底 73 / 内容区 h−158、按钮 40 最小 96、内缩 24 全部具名常量化；`dialog_shell.h:23-69` 统一壳（头 + 唯一内滚区 + 右锚按钮对 + 最小尺寸 + `dangerConfirm`/`dangerThird` 变体），并支持 `contentHeightProvider` 按内容自然高供给。**39 处调用全部走枚举，硬编码弹窗宽 0 处**——全仓 token 化最彻底的一层，任抽一个弹窗都能查到可直接编码的数值。
2. **语义色槽位分层，把对比度问题挡在 token 层**：`src/core/theme.h:20-52` 区分 `primary`（图形/边框）与 `primaryText`（小字）、`onPrimary` 与 `onAccent`（绿底深墨 8.4:1）、`error` 与 `errorText`、`success/warningText`，每条注释带实测比值与 WCAG 判定。自研 GUI 项目里极少见。
3. **空态收敛为一个组件，并把「为什么废弃旧形态」写进代码**：`src/ui_kit/empty_state.h:10-47` 记录四种被废弃形态（大白卡包裹 / 裸文字两行 / 顶部对齐 / 单行文字），统一为图标+主句+引导句+行动按钮且整体垂直居中——可直接对照验收（U-09 正是据此判 Todo 违规）。
4. **危险操作统一二次确认，并写明不可恢复后果**：`dialog_shell.h:64` + `md_module_menu_poll.cpp:517-530`（「将删除笔记「路径」及其磁盘文件，不可恢复。」）+ `claude_ui_form.cpp:607` + `todo_ui_dialogs.cpp:462`，四处同款，无一处「点一下就删」。托盘删除壁纸也走同一壳（`app_sidebar.cpp:1465`）。
5. **toast 四态一态一固定图标，显式修掉历史不一致**：`src/ui_kit/toast.h:21-27` 注释明确记载这是整改「同一个『成功』在不同页面长得不一样」的旧问题，并将时长统一为单值。
6. **无障碍现状诚实声明且规定优先级**：`docs/ACCESSIBILITY.md:12-24` 不粉饰，明写「不支持屏幕阅读器」「Tab 循环未实现」，并规定「与设计规范 §10 冲突时以本文件（实现现状）为准」——这是罕见且正确的工程诚实。

---

## 8. 审计盲区声明

1. **未启动应用**：hover / press / focus 三态手感、拖拽跟手、jelly 按压、动效曲线体感、真实拖窗后的重排、字号 130% 下的逐页破版，全部未实测，仅按代码与静态截图判断。
2. **2026-10-29 旧截图与现状有多处不符**：凡引用旧图处均已在正文标注并与代码交叉核对；但旧图不能用于验证现状视觉（如 Todo 四色卡片配色是否仍在）。
3. **Windows 侧完全未覆盖**：无 Windows 截图，标题栏 / Alt 菜单 / 原生文件对话框 / DWM 材质 / 125% 缩放下的视口裁切仅按代码与注释推断，未列条目（P0 的裁切问题在 Windows 1240×640 下限下**只会更严重**，但未实测）。
4. **未逐屏看内部布局**：agent / memory / mcp / wallpaper / git / 终端只看代码片段与 1 张深色截图，页级构图与内部信息密度未评。
5. **无真实 a11y 实测**：VoiceOver / NVDA、真实 Tab 顺序、200% 字号、纯键盘完成一条完整任务，均未验证。
6. **未评估性能与数据正确性**：粒子开销、骨架屏、大列表滚动、数据库错误路径均不在本次 UI/UX 审计范围。
7. **未审计 `module_release` 详情页与 `module_agent` 会话页的深层交互**：两模块合计 2.7 万行，仅抽读了弹窗与返回逻辑。
