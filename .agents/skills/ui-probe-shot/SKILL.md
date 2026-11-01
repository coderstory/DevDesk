---
name: ui-probe-shot
description: 离屏渲染拿特定 UI 的截图 PNG 与布局 frame 数据——两条现成路径（整页批拍 / 自写探针）+ 坑位清单
---

# ui-probe-shot：特定 UI 截图 + 布局取证（离屏渲染）

能力底座**已实现**，本 skill 只教怎么用，不含框架改动：

- `tests/probes/probe_harness.h`（namespace `probe`）——窗口生命周期、帧驱动、帧捕获（进程内 glReadPixels → PNG）、输入注入、布局断言
- `tests/probes/uat62_page_shots_probe.cpp`——14 个注册页整页批拍（真 `app::compose` 装配链）

用途：UI 改动自证 / QA 视觉验收 / 看图会话 / 布局数值断言。不适用：CI 无 GUI 会话（probe 自动跳过，见坑位 ⑦）；需要人工手感的走查走真机 UAT。

## 路径 A：整页快照（零代码，先试这条）

```sh
cd <repo>/build/tests
./uat62_page_shots_probe --size 1200x1000 --out /abs/path/evidence/<批次>/pages
```

- 产物：`NN-<slug>-<窗口高>.png`，按 `kModuleRegistry` 表序（= 侧边栏序）；分组父项行与平台不支持页（非 Windows 的 wallpaper）跳过不落图，编号可能空号
- 数据态 = 探针空状态（不造假数据），适合页面级视觉验收与设计走查
- 一次跑两档尺寸更佳：`--size 1000x640` 与 `--size 1600x1000`（窄窗自适应审查）
- 局限：只有空态整页。特定交互状态（弹层开、列表有数据、hover 态）→ 路径 B

## 路径 B：特定 UI 状态（自写探针，~60 行起）

1. 新建 `tests/probes/<name>_probe.cpp`——目录被 CMake GLOB 自动收编，**无需注册**；运行名 = 文件名（去 `.cpp`）
2. 骨架（逐行抄自 screenshot_probe / uat62_page_shots_probe，已验证）：

```cpp
#include "probes/probe_harness.h"
#include "test_support.h"   // 需要数据隔离设施时（MdHeadlessEnv / TempDir / ScopedAppRouter）

int main(int argc, char* argv[]) {
    auto args = probe::parseProbeArgs(argc, argv);   // --frames N --size WxH --manual --screenshot <路径>
    if (!probe::initializeWindowBackend()) return 1; // 无显示环境内部 exit 77
    auto window = probe::makeWindow("<name>_probe", args.windowW, args.windowH);
    if (!window) { probe::terminateWindowBackend(); return 2; }
    core::dsl::Runtime runtime;
    runtime.initialize(window);

    // 帧循环：每帧 pollOnce + compose（compose 每帧重建元素树）
    for (int f = 0; f < 24; ++f) {   // 24 帧 = 400ms@60fps，稳帧必须（坑位①）
        probe::composeModuleFrame(runtime, window, "<pageId>",
                                  (float)args.windowW, (float)args.windowH,
                                  [&](eui::Ui& ui, const eui::Screen& screen) {
                                      /* 在这里画你的 UI，或调模块 compose */
                                  });
    }
    // 拍图：推荐 --screenshot 标准接线（未传 = 零开销，传了失败会置 passed=false）
    bool passed = true;
    probe::captureIfRequested(args, window, runtime, &passed);

    runtime.shutdown(false);
    core::releaseInputQueue(window);
    core::window::destroyWindow(window);
    probe::terminateWindowBackend();
    return passed ? 0 : 1;
}
```

3. 交互状态注入（compose 循环之间调用）：`probe::click(window, runtime, x, y)`、`probe::drag(…, x0, y0, x1, y1, steps)`、`probe::queueKey / queueScroll / queueText`（排队后需再 `pollOnce`/`composeModuleFrame` 送达）
4. 构建与运行（**一律走脚本，门禁禁手敲 cmake**）：

```sh
bash scripts/build-test-run.sh --no-run --no-test        # 增量构建
cd build/tests && ./<name>_probe --screenshot /abs/path/out.png --size 1200x1000
```

- 也可 `cd <repo>/build && ctest -L probe -R <name>_probe --output-on-failure`（全部 probe 门禁 = `ctest -L probe`）
- 截图布局类探针别只跑不看：PNG 落 `evidence/<日期>-<任务>-<角色>/`（git-ignore），路径写回池条目/清单卡

## 布局取证（frame 数据，与截图同进程同状态）

- 元素几何在 `element.frame{ x, y, width, height }`，**逻辑 px**（探针窗刻意 dpiScale=1，逻辑=物理坐标）；PNG 像素尺寸 = 逻辑 × `capture.scale()`（Retina 2x），两者别混
- 按 id 取单个：`ui.find("id")->frame`；整棵树 dump（探针里加一段即可）：

```cpp
std::function<void(const core::dsl::Element&, int)> dump =
    [&](const core::dsl::Element& e, int depth) {
        std::cout << std::string(depth * 2, ' ') << (e.id.empty() ? "<unnamed>" : e.id.c_str())
                  << "  frame=(" << e.frame.x << "," << e.frame.y
                  << " " << e.frame.width << "x" << e.frame.height << ")"
                  << " z=" << e.zIndex << "\n";
        for (const auto& c : e.children) dump(*c, depth + 1);
    };
for (const auto& root : ui.roots()) dump(*root, 0);
```

- z 序断言现成：`probe::assertDrawnAbove(ui, topId, bottomId)`（LCA 绘制序）、`probe::assertNoEqualZOverlap(ui, popupId)`（弹层 z 碰撞扫描）；像素级颜色/内容断言抄 `screenshot_probe` ⑤ 与 `uat62_page_shots_probe` 的 `frameHasContent`

## 坑位清单（全部真实踩过）

1. **24 帧稳帧**：弹层透明度等走 `animate().transition()` 逐帧插值，只 poll 不 compose 树不更新；刚 open 就拍 = 半透明中间态，像产品缺陷其实早拍。24 帧（400ms@60fps）盖过仓库最长过渡
2. **Element* 跨 compose 失效**：compose 每帧重建树，上一帧拿的指针/`SwapScrollCallback` guard 在下次 compose 后是悬垂——guard 必须在对应 compose 前销毁
3. **逻辑 vs 像素**：`frame` 是逻辑 px，PNG 是 framebuffer 像素（×scale）。断言几何用 frame，断言视觉用像素
4. **渲染只许 FrameCapture 干**：常规帧循环只 update/compose **不 render**；`readFrame` 内部走 beginFrame → render → glReadPixels（present 前）→ present。自己绕开写渲染循环 = 空窗黑图 / 未定义内容
5. **窗口不可缩放是刻意的**：`makeWindow` resizable=false + pointerScale=dpiScale=1，坐标断言才不漂移
6. **建窗失败 ≠ 环境问题**：金丝雀判据（`classifyDisplayCapability`）把「平台不可用」（跳过 77）与「代码 bug」（必须红）分开，别把后者当环境放走
7. **无显示环境 = exit 77**：ctest 记 Skipped（需配 `SKIP_RETURN_CODE 77`，新 probe 抄现有 CMake 段）；本地有 GUI 会话必须真跑，77 不是通过
8. **图标空白 = 旧构建**：c18cef9d 后 POST_BUILD 已把图标字体复制到 `<exeDir>/assets`，截图与 cwd 无关；若仍见图标空白，先重新构建再查
9. **不触真实用户数据**：模块页构图用 `test_support::MdHeadlessEnv`（空笔记目录+空库）+ `ScopedAppRouter`（抄 uat62_page_shots_probe 132-150 行）；临时文件用 `test_support::TempDir`（析构整棵删）
10. **--screenshot-seeded / --screenshot-publish**：中途状态点/发布态落图，与 `--screenshot` 相互独立可同给（语义见 `ProbeArgs` 注释）

## 现成参考探针（按需抄结构）

| 探针 | 抄什么 |
|---|---|
| `uat62_page_shots_probe` | 真 app::compose 全页批拍、Router 导航、空态数据隔离 |
| `screenshot_probe` | 帧捕获判据语义、纯色构图+像素断言 |
| `schedule_ui_probe` / `agent_dialog_probe` | 模块页 + 交互状态（点击/弹层） |
| `todo_drag_probe` | 拖拽序列 |
| `settings_dropdown_z_probe` | 弹层 z 序断言 |
