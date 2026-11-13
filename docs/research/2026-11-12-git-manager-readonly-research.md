# git 管理器（只读版）调研报告

> 日期：2026-11-12｜需求：DevDesk 新模块「git 管理器（只读）」——目录树（全部文件/仅修改两模式）、内容查看区（语法高亮）、git 变更列表、变更 diff，可选全局搜索；对标 IDEA，不做编辑/staging/commit。
> 范围注记：**license 维度按用户令（2026-11-12）全项忽略**——本报告不含 license 选型维度与「license 传染」风险项，候选不因 license 影响推荐结论。
> 结论分级：已确认 = 本轮一手来源实读；已确认（二手）= 官方/权威转述；待验证 = 有来源但未实测；推测 = 判断。

## 0. 结论先行（≤5 条）

1. **git 数据面主方案 = libgit2 静态链**（已确认）：纯 C 实现、C API 直链 C++20、`git2.h` 59 个模块头逐项覆盖只读需求全部数据面（status/diff/revwalk/tree/blame/ignore），稳定线 v1.9.7（2026-08-13）+ v2.0.0-rc（2026-10-14）活跃双线维护；本仓 Windows 侧已有静态链 sqlite3/curl/openssl 同款工序先例。git CLI spawn 为备选（IDEA/VS Code/Sublime Merge 同路线），只读版不引入外部进程依赖。
2. **diff 计算零自研**（已确认）：libgit2 内置 xdiff（与 git 同源移植），算法默认 myers，可选 patience/minimal；**无 histogram**（main 分支 diff.h 实读无该 flag，histogram 仅 git CLI 有）。IDEA 默认同为 myers，只读查看器不受影响。
3. **语法高亮两路线**（已确认）：a) tree-sitter——纯 C runtime（v0.27.2，2026-11-07 发版，nightly 持续），grammar 按语言独立仓编译链入，GitHub/Neovim 生产背书；b) 扩展仓内 `md_highlight.cpp` styled-runs 行级词法先例（零新依赖）。建议 b 起步、a 按语言渐进。
4. **竞品格局印证轻量原生方向**（已确认）：重 web 栈（GitKraken=Electron、GitButler=Tauri+Svelte、IDEA 本体 Java）正是用户痛点；轻量原生栈（Fork/Sublime Merge=C++、Gittyup=C++/Qt、Tower/SourceTree=双平台各原生）为对位。**最重要参考实现 = Gittyup**（MIT 开源、libgit2+自绘渲染架构与 DevDesk 最同构）；只读动线参考 QGit（起家即只读历史/patch/变更文件查看器）。
5. **「90% 复用」假设：能力维度成立、工作量维度不成立**（已确认）：数据面与计算面（status/diff/log/tree/blame、diff 算法、高亮引擎）≈100% 可复用标品库；但 UI 集成层（文件树控件、变更列表、diff 渲染视图、搜索面板、libgit2 RAII 包装）必须自研——EUI-NEO 无 tree/code-view 组件，估 70%+ 代码量落在这层（推测，按组件缺口盘点）。

---

## 1. 需求映射（功能点 → 数据面 → UI 面）

| 需求点 | 数据面（复用标品） | UI 面（自研/EUI-NEO 集成层） |
|---|---|---|
| 目录树·全部文件 | std::filesystem 递归 walk + libgit2 ignore 判定（`git2/ignore.h`）或 status untracked 分类 | EUI-NEO 无 tree 组件（能力盘点 §0 硬限制）：`virtualList` + ▶/▼ 字符图标 + 展开状态机，懒加载子目录 |
| 目录树·仅修改文件 | `git_status`（`git2/status.h`：worktree vs index vs HEAD 分类） | 同上，行模型=变更条目过滤 |
| 变更列表 | `git_status` 状态枚举 + `git_diff` 文件级条目 | `virtualList` 或 `dataTable`（斑马纹/表头现成） |
| 查看 diff 内容 | `git_diff_tree_to_workdir/index`（`git2/diff.h`，xdiff 算法内置），patch/hunk/line 三级 API | 自研行盒渲染：styled-runs 增删行色 + 行号 + unified/side-by-side + 块折叠（md_highlight.cpp 先例扩展） |
| 文件内容查看 + 语法高亮 | 文件读取（fs::path 宽字符 API，clue#27 红线）+ tree-sitter 或词法扫描器 | styled-runs 行盒自绘；无语言 grammar 时回退纯文本 |
| 全局搜索（可选） | 目录 walk + 内容匹配；或 `git grep` 等价（只读版可后置） | 搜索框（components::input 现成）+ 结果列表（virtualList） |
| 仓库发现/打开 | `git_repository_open_ext`（`git2/repository.h`） | 仓库路径选择入口 |

---

## 2. 竞品矩阵（用户点名 9 项全覆盖 + 补充）

| 产品 | 形态/动线 | 技术栈（用户痛点标注） | git 后端 | 活跃度/开源 | 参考价值 |
|---|---|---|---|---|---|
| **IDEA 内置 VCS**（对标基准） | Commit 工具窗=变更列表+右侧 diff；项目树变更文件着色；本机 git 自动探测 | Java IDE 本体；**git 集成走原生 git 进程**（git4idea shell 原生可执行文件）+ JetBrains 自维护 JGit fork 辅助 | git CLI（+JGit 辅助） | 商业闭源 | 动线与信息架构基准：变更列表→点开→diff 右侧呈现 |
| GitKraken | 独立 GUI 客户端 | **Electron + Node.js**（用户痛点实锤） | NodeGit = libgit2 绑定 | 商业闭源 | 反面教材：重栈；libgit2 链库生产先例 |
| SourceTree | 独立 GUI | **双平台各原生**：Windows C#/WPF、Mac Cocoa/ObjC（Atlassian 官方确认，非 Qt 非 Java） | git CLI | 商业免费闭源 | 双原生维护成本参照 |
| Fork | 独立 GUI，轻量口碑 | C++/Qt 闭源；bundle git binary | git CLI | 商业闭源 | 轻量交互；大仓 freeze issue #122 为性能反面案例 |
| Tower | 独立 GUI | **双平台各原生**：Mac ObjC、Windows C#/.NET WPF（官方博客） | git CLI | 商业付费闭源 | 双原生+自研 diff 交互口碑 |
| Sublime Merge | 独立 GUI，性能标杆 | **C++ 自研跨平台 GUI toolkit**（无 Qt/Electron，官方站原话"zippy cross-platform GUI toolkit"） | git 可执行（git_binary 可配） | 商业闭源 | 与 DevDesk 形态最接近的商业对照：自绘 toolkit + git 进程 |
| GitButler | 独立 GUI | **Tauri + Rust 后端 + Svelte 前端**（repo DEVELOPMENT.md 原文），web UI 栈 | gitoxide（Rust） | 开源（repo 可读） | 开源新势力，但栈不适配 |
| **Gittyup** | 独立 GUI（GitAhead 停维护后活跃续作） | **C++11 / Qt≥6.4 / Scintilla / cmark，libgit2 以 bundled submodule 链入** | **libgit2（进程内）** | 开源活跃：Release 2.0.0（11-30）+ dev build 持续，2.3k stars | **第一参考实现**：libgit2+自绘 diff 架构同构；交互（diff/树/history）可摘 |
| QGit | 只读历史查看器起家 | C++/Qt | git CLI | 开源小众：tibirna/qgit 216 stars/950 commits；GitHub Releases 页空（v2.9 2025-10 为二手 LinuxLinks 数据） | 只读动线原型：browse history / view patch / changed files / blame——恰是本需求范围 |
| 补：VS Code 源控面板 | IDE 内置面板 | TypeScript/Electron 壳 | **要求本机安装 git**（官方 docs："VS Code uses this installation for Git operations"） | 开源 | 「仅修改文件」模式（资源管理器着色+源控视图）动线参照 |
| 补：lazygit / gitui | 终端 TUI | Go / Rust（均非 C++，不直接复用） | git CLI | 开源活跃 | 动线参考（变更列表→diff 键位流）；大仓口碑 gitui 优于 lazygit（HN 讨论） |

**趋势小结**（扫描级）：新势力全面转向 web 技术栈（Electron→Tauri 演进），传统阵营守住原生（Qt/各平台原生/自研 toolkit）；git 后端两流派并行不悖——**进程内 libgit2**（GitKraken/Gittyup）与 **spawn git CLI**（IDEA/VS Code/Sublime Merge/Fork/Tower/SourceTree）均有一线生产验证。轻量只读查看器细分（QGit、gitui、Gittyup 的 history 视图）证明只读版是成立的产品形态。

---

## 3. 底层库选型论证（git 数据面）

### 3.1 候选对比

| 候选 | 引入方式 | 只读数据面覆盖 | 活跃度 | 依赖重量 | 平台适配 |
|---|---|---|---|---|---|
| **libgit2**（推荐） | C 静态链，CMake `add_subdirectory` / FetchContent（Gittyup 同款 bundled 先例） | **全覆盖**：`git2.h` 59 模块头含 blame/status/diff/revwalk/tree/checkout/ignore（main 分支 raw 实读） | v1.9.7 2026-08-13；v2.0.0-rc.1 2026-10-14；双线安全维护；10.6k stars | 零外部强依赖（HTTPS/SSH 可选，只读本地仓不需要） | Win/mac 一等公民（GitKraken/Gittyup 双平台生产验证） |
| git CLI（spawn） | 无库，进程调用 `status --porcelain` / `diff` / `log` / `cat-file` | 全覆盖且含 histogram、全保真用户 git 配置 | 随用户 git | 零库依赖但**进程管理+PATH git 依赖**（DevDesk settings 已有 git.exe 探测先例 `local_git_probe_win.cpp:21`） | 双平台可用；Windows 进程启动成本与输出解析脆弱面 |
| JGit | Java，不适配 C++ | — | — | — | 淘汰：技术栈红线 |
| gitoxide | Rust + FFI | 覆盖中（扫描级印象，未逐项核） | 活跃 | FFI 桥层成本 > libgit2 直链 | 淘汰：跨语言桥自造 |

### 3.2 论证

- **只读场景正好避开 libgit2 相对 CLI 的已知短板面**（网络协议、新特性滞后、hooks——只读版全不需要），CLI 的全保真优势在只读版无意义。
- **禁异常约束天然契合**（已确认）：libgit2 全错误码返回，映射 `Result<T>`（`src/core/result.h`）是薄包装；CLI spawn 则需进程生命周期+stderr+exit code 三层错误缝合。
- **大仓性能**：`git_status` 现代实现已高效；配合懒加载+虚拟化（§5）足够。
- **histogram 缺失影响小**（已确认 diff.h 无 flag）：myers 是 git/IDEA 默认；如后续确需，可加 CLI 备选桥（settings 探测 git.exe 已在位）。
- 三问门：libgit2 **能落地**（C90+CMake、双平台成熟构建、仓内静态链工序先例）、**覆盖 100%**（模块头逐一对上需求映射表）、**有生产级参考**（Gittyup bundled submodule 生产使用、GitKraken NodeGit 同库生产）。过。CLI 路线同样过三问但败于「外部进程+PATH 依赖」收益比，列备选不推荐。

### 3.3 libgit2 落地条件清单（验收：含前置/工序/验证点）

1. 引入：源码级引入（FetchContent 或 vendored），Windows MSYS2 静态链工序参照 openssl/curl 现例；macOS 源码编译（Homebrew 包存在但静态链需求同 openssl 先例走自编）。具体 CMake 工序待开发期 POC（待验证）。
2. 线程模型：后台 worker 线程做 status/diff 计算；libgit2 对象跨线程约束——per-thread `git_repository` handle 或互斥（待验证：具体线程安全边界以 v1.9 文档为准）。
3. 全局初始化：`git_libgit2_init` 一次性（模块 init 期）。
4. RAII 包装层：C API → C++ 值语义 + `Result<T>`（自研薄层，预计数百行）。
5. UTF-8 路径边界：DevDesk 内部 UTF-8 ↔ Windows 宽字符；libgit2 Windows 侧路径编码行为待 POC（见 §7 风险 2）。

---

## 4. diff 与语法高亮

### 4.1 diff

- **计算**（已确认）：libgit2 xdiff 与 git 同源；flag = 默认 myers / `GIT_DIFF_PATIENCE` / `GIT_DIFF_MINIMAL` / indent heuristic；无 histogram（main 分支 `include/git2/diff.h` 实读）。git CLI 四算法（myers/minimal/patience/histogram，git 官方 diff-options 文档；经验研究多推荐 histogram 于代码变更——arXiv 2507.22071，扫描级）。
- **渲染**（自研，主要工作量）：无 C++ 标品可直接摘入 EUI-NEO（Qt Scintilla/QPlainTextEdit 均不可引）。方案：virtualList 行盒 + styled-runs（增/删/上下文行色 + 行内词级高亮可选）+ hunk 头折叠；md_highlight.cpp 的 runs 模型直接扩展。交互基准取 IDEA/Gittyup（unified 起步，side-by-side 二期）。**Scintilla/Lexilla 评估结论：不引入**——为只读查看引入整个编辑组件栈过重，且需对接 EUI-NEO 渲染管线（推测，未做适配性验证）。

### 4.2 语法高亮

| 路线 | 内容 | 优势 | 代价 |
|---|---|---|---|
| **B（推荐起步）：扩展 md_highlight styled-runs** | 行级词法扫描器（关键词/字符串/注释/数字 run 分类），按扩展名分派，先覆盖 C/C++/CMake/JSON/MD | 零新依赖；仓内先例（`md_highlight.cpp:1-3`，含"漏标回默认色优于错标"边界哲学）；theme token 直通 | 无语法树（跨行结构不精确）；每语言手写扫描器 |
| **A（渐进增强）：tree-sitter** | runtime 纯 C（v0.27.2 2026-11-07 发版，nightly 持续，27.5k stars）；`tree_sitter/api.h` 唯一入口，grammar 仓编译出 parser.c 链入；官方 binding 无 C++ 版但纯 C 可直链 C++ | 完整语法树、容错解析、GitHub/Neovim 生产背书；运行时按需解析 | 每语言一 grammar 仓引入与编译；语言清单数量未逐一核实（待验证）；0.27 线有 breaking changes 跟版成本 |

只读查看器视角：B 先交付（C++/CMake/JSON 高优先），A 作为增量按语言补，两者可在同一 styled-runs 输出模型下共存（A 的 query 捕获映射为 runs）。

---

## 5. 目录树大仓性能方案

- **全部文件模式**：懒加载——首屏只 walk 根层，目录展开时再 walk（与 virtualList 行模型天然契合：展开=增量 append 行）；walk 时逐路径过 libgit2 ignore 判定（`git2/ignore.h`）。
- **仅修改模式**：直接吃 `git_status` 结果，不 walk 文件系统。
- **风险先例**（已确认，issue 级）：Fork #122（10k+ 变更文件渲染 freeze）、GitAhead #161（未 ignore 的多 GB untracked 文件致 hang+内存爆）、lazygit monorepo 文件句柄/慢（#1888，HN 对比 gitui）。应对：变更列表/树一律 virtualList 虚拟化；单文件超阈值只显占位不读内容；status 计算移后台线程 + 1s 节流刷新（DevDesk compose 节流先例在位）。
- EUI-NEO tree 缺口的官方应对口径已固化在能力盘点 §10（virtualList + ▶/▼ + 状态机，约 150 行起步）。

---

## 6. 映射结论：验证「90% 标品可复用」假设

**按能力面计：成立。**
| 层 | 复用内容 | 复用方式 |
|---|---|---|
| git 数据面 | libgit2 全部所需模块 | 链库（静态） |
| diff 计算 | libgit2 内置 xdiff | 同上 |
| ignore 判定 | libgit2 `git2/ignore.h` | 同上 |
| 高亮引擎（可选） | tree-sitter runtime + grammar 仓 | 链库+编译期引入 |
| 交互参考 | Gittyup（libgit2+自绘，最同构）、QGit（只读动线）、IDEA（信息架构基准） | 参考实现摘交互，不引代码（Qt 红线） |

**按代码量计：不成立。** 必须自研（收敛到 EUI-NEO 集成层 + module_git 薄层）：
1. libgit2 RAII 包装层 + `Result<T>` 映射（薄，自研）；
2. 文件树控件（virtualList+状态机，EUI-NEO 无 tree 组件）；
3. 变更列表视图；
4. diff 渲染视图（行盒/styled-runs/折叠，最大单件）;
5. 语法高亮扫描器（路线 B）或 tree-sitter 接线（路线 A）；
6. 全局搜索面板（walk+匹配+结果列表，无现成）。

估 UI 集成层占 70%+ 代码量（推测，按组件缺口盘点）。**给 PM 的一句话：标品负责「算得对」，自研负责「画得出」；砍自研面=砍需求面，没有更多第三方可引。**

---

## 7. 风险清单（license 项已按 2026-11-12 用户令移除）

1. **大仓性能**（已确认先例）：未 ignore 大文件、巨 diff 渲染——懒加载+虚拟化+阈值占位+后台线程节流（§5）。
2. **中文/非 ASCII 路径**（仓内已证风险面，clue#27）：Windows 窄字符 ANSI 打开必败；全链 fs::path/宽 API。libgit2 Windows 侧对 UTF-8 路径的内部宽字符转换行为**待 POC 实证**（README 主张 portable，未实测）。仓发现、status 条目、diff 打开文件三处都是边界。
3. **二进制文件 diff**：libgit2 diff 返回二进制标记；只读版显「二进制文件」占位（Gittyup 有同场景交互可参考——具体形态待验证）。
4. **submodule 仓**：外层仓 status 含 submodule 条目类型；只读版建议不展开 submodule 内部（显目录降级），枚举细节待验证（`git2/submodule.h` 在位）。
5. **libgit2 特性滞后**（已确认一例）：无 histogram；只读范围不受影响，CLI 备选桥可后补。
6. **线程安全边界**：libgit2 对象跨线程约束待按 v1.9 文档落实（per-thread handle 倾向）。
7. **EUI-NEO 自绘控件陷阱**（仓内已证，clue#22）：compose measure pass 每帧两跑，树/diff 行盒的命中矩形与高亮叠加必须按次幂等；`setScrollOffset` 时序约束（clue#14）适用于 diff 联动滚动。
8. **tree-sitter 跟版**：0.27 线 breaking changes（已确认 release notes）；锁 minor 跟 patch。

---

## 8. 遗留与待验证

| 项 | 缺口 | 补法 |
|---|---|---|
| libgit2 Windows UTF-8/中文路径行为 | 未实测 | 开发期 POC（最小 repo open + status） |
| libgit2 MSYS2 静态链工序 | 未验证（mingw64 有包，静态链需自编） | POC：openssl 工序复刻 |
| tree-sitter 官方 grammar 语言清单数量 | 未逐一核实 | 引入前按目标语言逐仓核对 |
| QGit 最新版本锚点 | GitHub Releases 页空，v2.9 为二手 | 如需精确锚点查 tibirna 上游 git 仓 tag |
| GitButler gitoxide 数据面细节 | 仅确认后端语言 | 不影响本选型（栈已淘汰），不补 |
| 二进制/submodule 状态枚举细节 | 未展开 | 开发期对照 v1.9 API 头文件 |

---

## 9. 来源清单

**精读（一手实读）**：
- https://raw.githubusercontent.com/libgit2/libgit2/main/include/git2.h （59 模块头清单）
- https://raw.githubusercontent.com/libgit2/libgit2/main/include/git2/diff.h （算法 flag 实读）
- https://github.com/libgit2/libgit2 + /releases （版本/活跃度）
- https://github.com/tree-sitter/tree-sitter + /releases + https://tree-sitter.github.io/tree-sitter/using-parsers （C API/集成流程）
- https://github.com/Murmele/Gittyup + /releases （依赖栈/版本）
- https://github.com/tibirna/qgit （功能面/规模）
- https://github.com/git/git （语言/许可事实）
- 仓内：`docs/EUI-NEO-CAPABILITIES.md`（§0/§10）、`src/module_markdown/md_highlight.cpp:1-60`、AGENTS.md、DOCS-CONVENTIONS.md、池 clue#1~#34

**扫描（搜索摘要级，官方/权威为主）**：
- VS Code 官方 docs（sourcecontrol/overview：本机 git 要求）
- JetBrains 官方 docs（git4idea 原生 git）+ JetBrains/intellij-deps-jgit repo
- Sublime Merge 官方站/docs（command_line）+ 官方 forum（git_binary/自研 toolkit）
- Tower 官方博客（Mac ObjC / Win C#/.NET WPF）
- Atlassian 社区+官方博客（SourceTree C#/WPF + Cocoa）
- GitKraken 官方博客（Electron+NodeGit/libgit2）
- GitButler repo DEVELOPMENT.md（Tauri+Rust+Svelte）
- git-fork.com（Fork 产品面）；fork-dev/TrackerWin#122（大仓 freeze）
- gitahead/gitahead#161（untracked 大文件 hang）；jesseduffield/lazygit#1888；HN gitui vs lazygit
- git 官方 diff-options 文档 + arXiv 2507.22071 + StackOverflow（diff 四算法）
- LinuxLinks（QGit v2.9 2025-10，二手）

---

## 10. 硬清单自检（2026-11-07 老板令清单逐条）

1. **落地性**：libgit2 主方案含落地条件清单（§3.3：引入工序/线程模型/RAII/路径边界五项，其中两项标待验证并给补法）；tree-sitter 落地路径明确（api.h+parser.c 链入）；无「纯可行」无依据结论。✔
2. **覆盖率**：§1 需求映射表逐功能点对到数据面+UI 面，100% 覆盖（全局搜索标可选可后置，写明谁补怎么补）。✔
3. **生产级锚点**：libgit2（repo+版本号+文件路径 `include/git2/*.h` raw 级）、tree-sitter（repo+tag v0.27.2+commit 3fb7be4）、Gittyup（repo+Release 2.0.0+依赖清单）、参考先例均在 repo/release/文件路径级可核对；竞品矩阵每行含 repo/官网锚点。唯一降级：QGit 最新版本为二手数据（已标注，§8 有补法）。✔
4. **结论分级**：全文已确认/已确认（二手）/待验证/推测逐处标注；关键结论（§0 五条）均标级。✔
5. **三问门**：libgit2（三问全过，推荐）；git CLI（三问过，败于外部进程/PATH 依赖收益比，列备选）；tree-sitter（过，定位可选增强）；淘汰项败因已写：JGit（技术栈红线）、gitoxide（FFI 桥自造成本）、Qt/Scintilla 系代码引入（技术栈红线+过重）、Electron/Tauri 系（用户痛点本身）。✔

**自报工具调用数**：33 次（≤60 预算内；含 2 线索取、5 线索入库、13 网页精读/检索、4 仓内读、9 池/文档操作）。
