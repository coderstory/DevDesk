# DevDesk 未完成任务清单

> 2026-09-28 会话截止  
> 已交付 20+ commit（详见 `git log --oneline`）

## 未完成项

### 1. 桌宠最大高度设置
- **需求**：设置页加"最大高度"控件（默认 288px, 范围 [48,512]），皮肤超过等比缩放
- **实时生效**：改设置后当前宠物立即 resize
- **窗最小尺寸**：宠物窗最小 48×48
- **涉及文件**：`settings_config.h/cpp`、`settings_ui_cards.cpp`、`pet_window.cpp`、`pet_config.h/cpp`
- **状态**：代理派发后取消（a6e3434efb40b413f）

### 2. 桌宠气泡样式
- ✅ **已完成**（cbe7245）：底色半透 alpha 0.55 + 文本自适应宽 max 280px + 居中
- **待定**：是否需要换行支持（当前单行）

### 3. Todo 卡片拖拽跟手动画
- **需求**：拖动卡片时，卡片本体跟随光标移动（此前只变半透明 0.45，不跟手）
- ✅ **已完成**（598a970）：跟手副本 `todo.dragghost` 画在**页面级顶层 root**（`todo_ui_board.cpp` 的 `drawDragGhost`）——anchor = 按下时卡片 bounds + 按下点起累计位移（框架 `DragEvent::totalX/totalY`），经新纯函数 `ui::dragGhostAnchor` 换算到页面 compose 空间；zIndex 100（压过看板子树、低于弹窗栈底 1000）；opacity 0.85（让目标列插入位占位条从副本底下透出）。源列原卡照旧 0.45 半透明留原位（占位骨架），源列 0.6 调暗门控未动
- **原「关键点」两处失实，已纠正**：① `BoardPlate.dragOffset` 字段不存在（全仓零命中）；② 方案不是 `drawCard` 加 `.translate()` —— 列内容在 `components::scrollView` 根 stack 的 `.clip()` 内（`components/scrollview.h:88`，clip 在渲染侧对整棵子树做 scissor 求交：`core/runtime/runtime_render.h:223-230`），列内 translate 会被裁到列边界，跨列拖（主用例）不成立，故改页面级副本 + 绝对锚点定位
- **未验证**：真人手拖的体感与屏幕像素（本机 `screencapture` 被 TCC 挡，无截屏，也无人肉眼看过）。替代证据（均实测绿）：无头用例 `todo_ui_drag_follow_ghost` + `todo_probes` 类 12（`queuePointerButton/Motion` 真实注入指针走真实命中测试与拖拽派发，断言副本帧位 = 卡位 + 累计位移；fc1b4f8）——两处都做过变异验证（去掉跟手副本 / 去掉位移项即红）

### 4. Probes 扩展
- **已交付**：7 probe exe 全编译 + 全 PASS + harness 补键盘/右键/滚轮/拖拽/文本
- **版本控制缺口（2026-09-28 实测）**：`tests/probes/` 下 3 个 probe 源文件从未被 git 跟踪——`claude_probes.cpp`（172 行）、`home_probes.cpp`（386 行）、`markdown_probes.cpp`（384 行），共 942 行。它们是真交付物：文件头分别引用 spec `docs/superpowers/specs/2026-09-28-devdesk-probes-design.md`（§3-probe-full）与具体 commit（`046beaa` home sparkline 顶带修复 / `770beb3` markdown 双层卡修复）。这 3 个正是上面「7 probe exe」里缺失的 3 个（7 = claude / home / markdown / pet / settings_cards / settings_dropdown_z / todo），同目录另 4 个 probe 源文件与 `probe_harness.h` 已被 git 跟踪。**待办：把这 3 个文件 `git add` 进版本控制**（本清单只记录文档事实，未代为提交）。
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
- **永不 push 到 origin（2026-09-28 用户决策）**：EUI-NEO 本地 main 领先 origin 82 个提交（2026-09-28 实测），已决策永不推送——origin/main 停在 v0.6.0，本地领先是长期状态、提交数只会继续增长，不再按「待 push」跟踪。

---

## macOS 构建环境（2026-09-28 本机实测）

- 本机 macOS / arm64 已装 Homebrew 工具链（`/opt/homebrew/bin`）：ninja 1.13.2、openssl@3 3.6.4（同路径另有 cmake 4.4.3）
- configure 命令（**macOS 上须手敲**；`build/` 首建或换平台时先删重建）：
  ```
  export PATH="/opt/homebrew/bin:$PATH"
  cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DOPENSSL_ROOT_DIR=/opt/homebrew/opt/openssl@3
  ```
- `scripts/build-test-run.sh` **已移植为跨平台**（提交 `577e0d0` 给三个脚本加 macOS/Linux 分支：Darwin 的 `/opt/homebrew/bin` PATH 前置、`-DOPENSSL_ROOT_DIR=/opt/homebrew/opt/openssl@3`、`pkill -f` 杀进程、`otool -L` 依赖清单、macOS 的 ccache 配置路径；原先写死的 Windows 工具链 PATH 与 `C:/msys64/mingw64` openssl 路径改为按 `DEVDESK_OS` 分支取，Windows 分支行为不变）。**但端到端未验证**——移植后只做过语法检查与定点验证，从未在 macOS 上完整跑过一次「构建 + 测试 + 启动」，`check-consistency.sh` 的 `screencapture` 分支还依赖「屏幕录制」TCC 权限（无权限时的行为未知，见 `docs/KNOWN_LIMITATIONS.md`）。在端到端跑通之前，手敲上面两条命令仍是当前最可靠的路径。
- macOS 首次构建失败：EUI-NEO `core/platform/window_style_macos.mm` 与 `core/platform/context_menu_macos.mm` 编译不过（详见 backlog `docs/superpowers/plans/2026-09-22-backlog.md` §三）。归因：EUI-NEO 2026-09-28 提交 `6f771c3` 从未在 macOS 上编译过。

---

## 已知 Bug

| 问题 | 位置 |
|---|---|
| todo 卡片拖拽无跟手动画已修（598a970） | `todo_ui_board.cpp:drawDragGhost`（不再是 `drawCard`） |
| md tabs 切换过渡已删（1cdd36f） | 原为整页 fade 闪烁 |
| pet 右键菜单缺 dismiss rect 已修（5b0cd34） | EUI-NEO styledmenu |
| 主窗最小尺寸已调 1330×845（65c1543） | main.cpp |

---

## 分支/合并状态

- 全在 `master`（ddf3425 起 22+ commit）
- 无残留 worktree
- 未提交改动：`tests/probes/` 下 3 个 probe 源文件未跟踪（`claude_probes.cpp` / `home_probes.cpp` / `markdown_probes.cpp`，共 942 行，见 §4）——此前记的「0 未提交改动」不成立；除此之外工作区干净
- EUI-NEO：**永不 push 到 origin（2026-09-28 用户决策）**，origin/main 停在 v0.6.0；本地 main 领先 origin 82 个提交（2026-09-28 实测）为长期状态，不再有「待 push」事项（见 §9）