# DevDesk 三视角审计 —— 主会话独立取证底稿（2026-11-21）

> 性质：主会话亲自复核的**硬事实清单**（每条都有命令级证据），作为三份子代理报告的交叉校验基准。
> 独立成立，不依赖子代理结论。审计对象：`/Users/coderstory/dev/DevDesk`，commit `4fe330c4`（dev 分支）。

---

## 0. 量化基线（实测）

| 指标 | 实测值 | 取证方式 |
|---|---|---|
| 源码文件数（.cpp/.h/.mm） | 663 | `find src -name "*.cpp" -o -name "*.h" -o -name "*.mm" \| wc -l` |
| 代码总行数 | 144,891 | 同上 + `xargs wc -l` |
| 模块目录数 | 17（15 个业务模块 + 2 个桥文件） | `ls -d src/module_*` |
| 模块代码量 | 113,743 行（最大 module_agent 14,750 行 / 48 文件） | 逐模块统计 |
| 侧边栏 registry 条目 | 16 行（14 顶层项 + release 两子页） | `src/app_registry.h:49-68` |
| 可拨模块 id | 11 个 | `src/module_settings/settings_config.h:39-60` |
| 测试用例总数 | 2,591 例 / 204 文件 | `grep -c REGISTER_TEST` 汇总 |
| 最大单文件 | `release_detail_draw.cpp` 2,742 行 | `wc -l` top10 |
| 超 1,500 行文件 | 11 个 | 同上 |
| `isFail()` 检查点 | 2,152 处 | `grep -rn "\.isFail()" \| wc -l` |
| `throw` 关键字残留 | 0 处 | 全仓 grep |

---

## 1. P1：文档与实现硬漂移（4 条，行号级证据）

### D-01 侧边栏宽度三处数字互相矛盾
- **UI 规范** `docs/UI-UX-DESIGN.md:15`、`:67`、`:686`：260 px，"固定左，**不可拖拽调整**"
- **用户指南** `docs/USER_GUIDE.md:42`：展开 220px / 折叠 72px
- **代码实测** `src/app_sidebar.h:18-19`：`kSidebarWidthExpanded = 200.0f`、`kSidebarWidthCollapsed = 72.0f`
- **影响**：三份文档无一对得上实现，差 60px（30%）。设计评审/走查/验收全按错尺子量；设计师按 260 出图则实现永远"差一点"。
- **修法**：以代码为准反向修文档（200/72）；把该值提升到 `ui_kit/metrics.h` 作单一来源，文档引用 token 名。

### D-02 最小窗口尺寸规范落后两轮裁决
- **UI 规范** `docs/UI-UX-DESIGN.md:58`：最小窗口 1200 × 720
- **代码实测** `src/main.cpp:913-914`：1000 × 640；`:905-912` 注释明写「UAT3 D-R3-2 从 1280×800 收窄…1000 宽逐页适配核验」
- **影响**：规范落后 1280→1000 两次裁决。按 1200 校验布局会漏掉 1000 档真实溢出风险。
- **修法**：规范回写 1000×640 + 收窄理由链。

### D-03 README 模块数与测试数双过期
- `README.md:201`：「十三页全功能运行（可插拔模块 **9 个**：…）」——实测 **11 个**可拨模块，漏计 **Git** 与 **终端**
- `README.md:202`：「Total 2048 / Failed 0（macOS，2026-10-31）」——实测当前 **2,591** 例，落后 543 例 / 20 天
- **影响**：README 是外部准入判断依据，数字错会误导功能完备度与测试健康度评估。
- **修法**：状态段改动态指针（指向 USER_GUIDE + CI 徽章），不写死数字。

### D-04 文档截图全部过期 23 天，内容与现状不符
- **实测** `docs/screenshots/app-*.png` 全部 mtime = **2026-10-29**
- **截图实际内容**（主会话亲自看图确认）：侧边栏仅 **6 项**；Claude 页操作按钮 **6 个挤一行**（启用/测试连通/编辑/删除/复制/运行）
- **当前代码**：侧边栏 **14 个顶层项**；Claude 详情操作按钮已收敛为 **3 键**（`claude_ui_detail.cpp:534-620`：测试连通/编辑/运行）
- **影响**：截图是 UI 审计、对外展示、回归对照的基准物料。用 23 天前截图做验收 = 验的不是现在的东西（历史上已多次发生"基于过时截图报修"错位）。
- **修法**：截图纳入证据链自动生成（`ui_driver` 已有截图能力），或每图标注对应 commit 并设失效期。

---

## 2. P1：守卫覆盖盲区（2 条）

### G-01 模块隔离守卫名为「模块隔离」，实为两组点对点断言（覆盖 <15%）
- **脚本结构实测**（`sed -n '216,260p' scripts/check-module-isolation.sh`）只有两段断言体：
  - **B 段**：`src/module_settings/CMakeLists.txt` 非注释行不得 link `devdesk_module_claude_proxy`（**一条边**）
  - **C 段**：`src/module_agent/*.{h,cpp}` 不得 include `module_home|markdown|todo|claude_proxy|settings`；反向查 12 个指定工具文件不得引用 `module_agent/`（**agent 一个中心点的双向对**）
  - A 段（settings 不碰 pet）/ D 段（home）已随桌宠摘除标注"已移除"
- **无任何全模块遍历**：`grep -n "for .* in .*module_\|src/module_\*"` 零结果；模块名全为硬编码字面量
- **脚本残留死引用**：`module_pet`（已退役模块）/ `module_pet_bridge` 仍在注释与列表里（`src/module_pet` 不存在，注释自己写"2026-10-13 已摘除"）
- **完全无断言的模块（11 个）**：wallpaper(9,552) / memory(12,676) / release(12,635) / schedule(7,542) / mcp(5,106) / git(4,856) / notepad(3,838) / terminal(3,538) / home / about / todo
- **影响**：AGENTS.md §六「module_* 之间禁止互相依赖」是全仓硬约束，守卫只覆盖 agent 一个中心 + settings→claude 一条边。11 个模块（>60,000 行 = 全模块 53%）**可任意互相 include 而守卫恒绿**。这是"守卫绿 ≠ 合规"的系统性盲区，也直接削弱 §五 守卫链的公信力。
- **补充实测**：主会话正则扫全仓 `#include`（排除同模块）→ 当前**未发现**违规，代码是干净的；但**无机制防未来违规**。
- **修法**：改为**动态枚举 `src/module_*`**，对每模块断言"不得 include 任何 `module_<其他>/`"（N×(N−1) 全覆盖），保留 B/C 作额外语义断言；清掉 module_pet（已退役模块）死引用。

> 对照：其余守卫覆盖是**真实全仓**的（主会话逐个实跑）——
> `check-std-includes` 扫 936 文件全绿、`check-no-keychain` 零引用、`check-no-glass` 无调用、`check-toolguard-braces` tests 全扫。问题只在模块隔离这一个。

### G-02 弹窗 z 序散落六档魔数，无具名常量层
- **实测分布**（`grep -rn "zIndex = "`）：
  - `990` — `schedule_ui_history.cpp:446`
  - `1000` — `release_dialogs.cpp:338`（组件默认值同为 1000）
  - `1100` — `agent_module_dialogs.cpp:2133`、`:2281`
  - `1200` — `schedule_ui_list.cpp:1292`、`wallpaper_delete_bridge.cpp:116`、`quit_save_bridge.cpp:80`
  - `1300` — `release_dialogs.cpp:522`
  - todo 另走自有 `d.popups.zOf(Popup::X)` 机制（第 7 套）
- **唯一具名常量**：`ui_kit/dialog_form.h:24` 的 `kDialogZIndexUnset = -1`（哨兵，非层级定义）
- **影响**：跨模块叠弹窗层级靠人肉约定，六档魔数散落 7 文件，改一处不知影响谁。历史 bug352（windowRing 被内容层遮挡）即 z 序失控族。
- **修法**：`ui_kit/metrics.h` 增具名层级（kZBase/kZDialog/kZDialogStacked/kZOverlay/kZSplash），全调用点改引用；`schedule_ui_history` 的 990 需复核意图（疑似临时压制手段）。

---

## 3. P1：异常纪律与错误文案（2 条）

### E-01 `http_client.cpp` 用 try/catch 解析状态码，违背「不抛异常」铁律
- **位置** `src/platform/http_client.cpp:323-327`：`try { code = std::stoi(codeStr); } catch (...) { code = 0; }`
- **影响**：AGENTS.md §六 + `core/result.h:1` 明写「不抛 C++ 异常」。这是全仓**唯一** `std::stoi`（`module_about/about_module.cpp:57` 注释明写"不用 std::stoi：异常路径与本仓库『不抛异常』约定相悖"，说明团队有共识但此处漏网）。更严重的是它在 **libcurl 的 C 回调**里，异常越过 C 边界是 UB。
- **修法**：换手写十进制解析（仓内已有范式 `module_todo/todo_view_state.cpp:20`「手写十进制解析，项目铁律不碰异常」）。

### E-02 内部字段名泄漏进用户可见错误文案
- **位置** `src/core/schedule_rule.cpp:249/256/289/317/324/344`：`"once.at 非法："`、`"once.at_local 非法："`、`"interval 规则"`、`"calendar.unit "`、`"calendar.workdays_only "`
- **传播路径**：经 `errorMessage()` 被 `pageError_` / toast 直接展示（全仓 1,443 处 `errorMessage()` 直出，范例 `agent_module_schedule.cpp:422/721`、`claude_module.cpp:284/408/425/437/449`）
- **影响**：用户看到「calendar.workdays_only 非法」无法理解也不知道改什么，直接违反"错误提示须说清发生了什么 + 现在该做什么"。
- **修法**：分两层——底层保留技术消息（入日志），用户面过 `humanize(ErrorCode, context)` 映射到「哪个输入框填错 + 该怎么填」。

---

## 4. P2：一致性欠账（2 条）

### C-01 设计 token 在模块自绘层被大面积绕过
- **实测**：`src/module_*/` 内 `40.0f / 24.0f / 20.0f` 字面量 **673** 次；spacing token 引用仅 **66** 次
- **token 侧已完备**：`src/core/theme.cpp:55-64` 定义了完整 spacing/radius 体系（8/4/16/24/40、10/6/14/16/999）
- **影响**：主题与字号缩放（`setWindowHeight` 驱动的 H19 封顶）无法统一施加到硬编码值，缩放档出现局部不缩放。
- **反差说明**：`ui_kit` 组件层 token 化做得好（39 处 `DialogWidth::` 引用、硬编码弹窗宽 0 处），**欠账集中在模块自绘层**。

### C-02 空态组件缺 3 个模块，同场景两套找法
- **已覆盖 10 个**（agent/git/markdown/mcp/memory/release/schedule/todo/wallpaper/home 部分）
- **未覆盖**：`claude_proxy`（自绘单行 `claude_ui_list.cpp:674`「暂无 Profile，点右上角 + 新建」）、`terminal`（`terminal_page.cpp:978` 自绘）、`notepad`（无空态）
- **已覆盖的做得很好**：`agent_module.cpp:697-705` 与 `mcp_page_module.cpp:396-403` 均为"图标+主句+引导句+行动钮"四件套
- **影响**：同样"空列表"，agent 给引导与行动钮，Claude 只给一行灰字且「右上角 +」指向另一区域——同样空态要用户学两套找法。

---

## 5. 做对了的部分（避免单向失真）

1. **Result 纪律执行度高**：2,152 处 `isFail()` 检查、`throw` 全仓 0 处（唯一漏网是 E-01）。
2. **平台拆分是真拆**：`src/platform/` 三平台体质完整（`_macos`/`_win`/`_posix`），且有 `.zcode/cheatsheets/platform-file-map.md` 逐能力对照表，显式标注「⚠ 无 posix 兜底」族——改代码前能查到"同族另一头"。
3. **危险操作二次确认已体系化**：`dialog_shell.h:56/64` 的 `dangerConfirm`/`dangerThird` 被 schedule/todo/markdown/agent/memory 全部采用（17 处），不是各模块自创。
4. **空态组件设计纪律写进注释**：`ui_kit/empty_state.h:1-14` 明写"防 §4.12 禁止项：插画式空态 / 情绪化文案 / 无操作入口"，`EmptyStateOptions` 四字段结构强制引导下一步。
5. **对比度有专门 token 分槽**：`core/theme.h` 把 `primaryText`/`accentText`/`errorText`/`successText`/`warningText`/`onPrimary`/`onAccent` 全分离，注释带实测比值（「#D97757 当小字仅 2.96:1，低于 WCAG AA 4.5:1」）——罕见的对比度自觉。
6. **测试面广且分域**：2,591 例 / 204 文件，agent 240 / mcp 188 / markdown 157 / todo 136 / memory 130。
7. **线程与 COM 纪律**：`file_trash_win.cpp`（在飞代码）std::thread 创建捕获 `system_error`、明确 join（`:194`）、COM 套间容忍 `RPC_E_CHANGED_MODE`——照抄 `folder_picker_win.cpp` 已验证范式而非自创。
8. **并发写有内建保护**：`core/sqlite_db.cpp:275` 打开即设 `sqlite3_busy_timeout(5000)`，跨线程写库不会静默失败。

---

## 6. 审计盲区声明

- **未运行 app 做真机交互**（本轮是静态审计；主工作区当时有并发开发线占用）。
- **未做动态内存/线程检测**（ASan/TSan 需独立构建档）。
- UI 判断基于**过期截图 + 代码 DSL**，非当前运行时渲染——截图过期本身已列 D-04。
- 性能与启动耗时未测。
- need#356 壁纸删除线（现已被另一会话 stash 保存）只做代码阅读，未纳入结论。

---

## 7. 与子代理报告的关系

- `2026-11-21-audit-cpp.md`（code-reviewer 面）
- `2026-11-21-audit-uiux.md`（ui-ux-designer 面）
- `2026-11-21-audit-enduser.md`（qa-tester 面）

三份落盘后主会话逐条复核：与本文冲突的**以命令实证为准**；子代理独有发现若能复现则采信，不能复现则标"待复核"。
