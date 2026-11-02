# ui-probe-harness 小抄（DevDesk UI 探针 / 截图取证）

> **从真实数据生成，生成日期 2026-11-02**。来源：`tests/probes/` 现行代码、原 `.agents/skills/ui-probe-shot` 副本（git 历史 `94d496ab^`）、CLAUDE.md 阶段 4 探针段。通用方法论归 devflow 插件 `ui-probe-shot` skill，本页只记 **DevDesk 特有事实**。

## 一页速览

| 事项 | 事实 |
|---|---|
| 能力底座 | `tests/probes/probe_harness.h`（namespace `probe`）：窗口生命周期 / 帧驱动 / 进程内 glReadPixels 落 PNG / 指针键盘滚轮拖拽文本注入 / 布局与 z 序断言 |
| 整页批拍（零代码，先试这条） | `cd build/tests && ./uat62_page_shots_probe --size 1200x1000 --out /abs/path/evidence/<批次>/pages`：14 行注册表真 `app::compose` 批拍，实拍 12 页（分组父项行与 macOS/Linux wallpaper 跳过空号；Windows 实拍 13 页）；产物名 `NN-<slug>-<窗口高>[-fs<字号%>][-sc].png` |
| 批拍可选档位 | `--font-scale 0.85~1.30`（字号缩放对照）、`--sidebar-collapsed`（折叠侧栏）；建议一次跑 `--size 1000x640` + `--size 1600x1000` 两档；数据态 = 探针空状态（不造假数据） |
| 新探针零注册 | `tests/probes/` 被 CMake GLOB 自动收编：自动 `add_test` + `LABELS probe` + `SKIP_RETURN_CODE 77`；**探针文件放目录即收编，运行名 = 文件名去 `.cpp`，零 CMake 改动** |
| 构建门禁 | 一律 `bash scripts/build-test-run.sh --no-run --no-test`（禁手敲 cmake）；**探针不进 CI**：CI 侧 `DEVDESK_BUILD_PROBES=OFF` 根本不构建 probe 目标 |
| 运行 | `cd build/tests && ./<name>_probe --screenshot /abs/path/out.png --size 1200x1000`；全量探针门禁 `ctest -L probe`，单个 `ctest -L probe -R <name>_probe --output-on-failure` |
| 截图参数族 | `--screenshot <路径>`（标准接线：未传零开销，传了落图失败置 passed=false + 非零退出）；`--screenshot-seeded` / `--screenshot-publish`（中途状态点 / 发布态落图，与 `--screenshot` 相互独立可同给）；`--frames N --size WxH --manual` 均由 `probe::parseProbeArgs` 解析 |
| 逻辑 vs 像素 | `element.frame{ x, y, width, height }` 是**逻辑 px**（探针窗刻意 dpiScale=1，逻辑=物理）；PNG 像素 = 逻辑 × `capture.scale()`（macOS Retina 2x，Windows 通常 1x）。**断言几何用 frame，断言视觉用像素，别混** |
| 稳帧 | 弹层透明度等走 `animate().transition()` 逐帧插值——**24 帧（400ms@60fps）后再拍**；刚 open 就拍 = 半透明中间态 |
| 帧循环纪律 | 每帧 `pollOnce` + `composeModuleFrame`（compose 每帧重建元素树）；`Element*` 跨 compose 失效（悬垂）；渲染只许 `FrameCapture` 干（`readFrame` 内部 beginFrame → render → glReadPixels → present），自己绕开写渲染循环 = 空窗黑图 |
| 数据隔离 | 探针默认空状态不触真实用户数据：`test_support::MdHeadlessEnv`（空笔记目录+空库）+ `ScopedAppRouter` + `test_support::TempDir`（析构整棵删） |
| 布局取证 | `ui.find("id")->frame` 单点；整棵树递归 dump `e.frame` + `e.zIndex`；现成断言 `probe::assertDrawnAbove(ui, topId, bottomId)`（LCA 绘制序）、`probe::assertNoEqualZOverlap(ui, popupId)`（弹层 z 碰撞扫描）；像素颜色断言抄 `screenshot_probe` 与 `uat62_page_shots_probe` 的 `frameHasContent` |
| exit 77 ≠ 通过 | 无显示环境内部 exit 77 跳过；本地有 GUI 会话必须真跑。与 CI 不构建（`DEVDESK_BUILD_PROBES=OFF`）是**两回事**；建窗失败另有金丝雀判据（`classifyDisplayCapability`）分「平台不可用」与「代码 bug」 |
| GLFW 头 | 探针**勿单独 `#include <GLFW/glfw3.h>`**——harness 已统一给 GLFW + glad，混用撞 `OpenGL header already included` #error |
| 图标空白 | = 旧构建：POST_BUILD 已把图标字体复制到 `<exeDir>/assets`（截图与 cwd 无关），先重新构建再查 |
| 证据物去向 | PNG 落 `evidence/<日期>-<任务>-<角色>/`（git-ignore），路径写回池条目 / 测试清单卡 |

## 交互注入（compose 循环之间调用）

| API | 用途 |
|---|---|
| `probe::click(window, runtime, x, y)` | 点击 |
| `probe::drag(…, x0, y0, x1, y1, steps)` | 拖拽序列 |
| `probe::queueKey / queueScroll / queueText` | 排队后需再 `pollOnce` / `composeModuleFrame` 送达 |

## 参考探针（按需抄结构）

| 探针 | 抄什么 |
|---|---|
| `uat62_page_shots_probe` | 真 app::compose 全页批拍、Router 导航、空态数据隔离 |
| `screenshot_probe` | 帧捕获判据语义、纯色构图 + 像素断言 |
| `schedule_ui_probe` / `agent_dialog_probe` | 模块页 + 交互状态（点击 / 弹层） |
| `todo_drag_probe` | 拖拽序列 |
| `settings_dropdown_z_probe` | 弹层 z 序断言 |

## Windows 侧差异（⚠ 待验：Mac 机编写，以下为代码接线推断，Windows 机首跑批拍后把实测记入 evidence 并删除本标注）

- exe 同落 `build/tests/`（MinGW 单配置生成器）；MSYS2 bash 下 `./<name>` 自动补 `.exe`
- 显示缩放通常 =1 → PNG 尺寸 ≈ 逻辑尺寸（无 macOS 的 2x）
- wallpaper 工厂在 Windows 返回真实例 → 批拍实拍 13 页（多一页 wallpaper）
