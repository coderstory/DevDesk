# DevDesk / EUI-NEO 轻量化审计报告

日期：2026-10-03　范围：`DevDesk/src`、`DevDesk/tests`、`EUI-NEO`
方法：静态扫描（重复块哈希 / 符号定义交叉比对 / include 图 / CMakeCache 实证）+ 抽样精读
**本次为只读审计，未修改任何源码。**

---

## 0. 结论速览

| 项目 | 现状 | 可裁 | 裁剪后 | 说明 |
|---|---:|---:|---:|---|
| DevDesk `tests/` | 81,707 | **−10,400 ~ −12,600** | ~70,000 | 低风险，纯删减与合并 |
| DevDesk `src/` | 63,540 | **−1,500 ~ −2,200** | ~61,500 | 需重构，逐项验证 |
| EUI-NEO | 77,425 | **−41,063** | ~38,000 | **几乎全是"已不参与构建"的仓库重量** |
| **合计** | **222,672** | **−53,000 ~ −56,000**（约 **24%**） | ~170,000 | |

**一句话**：最大的包袱不是源码，是**测试（1.29:1 倒挂）**和**EUI-NEO 里 4.1 万行从不参与编译的 demo/测试/未用组件**。
源码本身写得比想象中干净——颜色全走 token（全仓仅 2 处硬编码 `0x`）、CMake 无孤儿源文件（0 个）、ui_kit 抽象没被绕过。

---

## 1. 最大包袱：测试代码 81,707 行（源码的 1.29 倍）

### 1.1 `tests/probes/` 9,592 行 —— CI 从不执行（**首选清理目标**）

| 事实 | 证据 |
|---|---|
| 被 `devdesk_tests` **显式排除**，独立可执行（`LABELS "probe"`） | `tests/CMakeLists.txt:130-135, 265-266` |
| CI **硬关闭** `DEVDESK_BUILD_PROBES: "0"` | `.github/workflows/ci.yml:28` |
| 无 GUI 会话 → 退出码 77 → ctest 记 Skipped | `tests/CMakeLists.txt:272` |
| 产出 PNG/PPM 供**人工判读**，`--manual` 停屏 90s | `probes/about_probe.cpp:26-32`、`probe_harness.h:279,329` |

**其中 2 个被刻意设计成"当前必红"，把红灯当 TODO 清单用**：
- `probes/pet_probes.cpp:11-12,338-350`：「当前菜单树内无该元素 → **本组红 = 证伪门禁**」
- `probes/settings_cards_probes.cpp:24,339`：同上

这正是文档里「14 个 UI probe 全红」的根因之一——红灯混入门禁会淹没真信号。

**与无头单测逐条重复**（probes 自己的注释承认）：
- `markdown_probes.cpp:323-344` ≡ `test_theme_hierarchy.cpp:163`
- `todo_probes.cpp:1-40` ≡ `test_todo_ui.cpp:735-1704`
- `claude_probes.cpp:33-37` ≡ `test_claude_ui.cpp:1816`
- `home_probes.cpp:1601` 自注与 `home_quota_budget_*` 重叠

> **建议**：保留 3 个有独占价值的（`screenshot_probe.cpp` 守判据自身、`settings_dropdown_z_probe.cpp` 只有真窗口能测 z 序、`module_toggle_sidebar_probe.cpp` 唯一真点击派发栈），**删除其余 9 个 → −7,345 行**。

### 1.2 恒真/空洞用例

| 位置 | 内容 |
|---|---|
| `test_platform/test_file_open.cpp:155` | `CHECK(r.empty() \|\| !r.empty()); // 恒真` ← 注释自己承认 |
| `test_modules/test_claude.cpp:4-11` | 文件头自注「恒真、虚增用例数」 |
| `test_markdown.cpp:54` / `test_http_client.cpp:33` | `enum A != enum B` 恒真 |
| `test_result.cpp:63` / `test_home.cpp:32` | 断言 enum 整数值 |
| 全仓 | **38 个用例断言数 ≤1**（8 个为 0，纯调用无断言） |

### 1.3 可合并 / 可参数化

| 项 | 位置 | 减行 |
|---|---|---:|
| quota 三家（minimax/volcano/opencode）同一断言复制 3 份 ×5 组 | `test_claude_quota*.cpp` | −600 |
| HTTP 注入缝（FakeHttp/httpReturning/httpFailing）三份各自重写，`test_support.h` 里根本没有 | 同上 | −120 |
| `ScopedConfigPath` 样板全仓最高（单文件 63 次） | `test_settings.cpp` | −200 |
| 三家 HTTP 状态码→错误分类真值表 | `test_claude_quota*.cpp` | −180 |
| failover 三用例相似度 0.82–0.84，只有 3 个输入不同 | `test_proxy_router.cpp:859-1051` | −160 |
| 192 个 ≤12 行微用例（getter/常量/枚举单断言） | 全仓 | −900 |
| 相邻高相似块 23 对 | 多处 | −540 |

### 1.4 脆弱的 UI 断言（不减行，但决定维护成本）

8 个 UI 测试文件共 **875 处数字字面量断言**，其中 `frame.x/y/w/h` **532 处**、颜色字面量 56 处。
典型：`test_claude_ui.cpp:6873 fabs(frame.height - 204.0f) <= 0.5f`、`test_todo_ui.cpp:3178 frame.width >= 24.0f`。
这些值**抄自实现**，改一次设计 token 就成片红，且红了分不清是 bug 还是设计变更。
→ 建议改为**关系断言**（A 在 B 之内 / A 与 B 不相交）+ 命名常量，可把 875 处降到 ~200 处。

**合理体量**：源码的 0.6~0.8 倍 = **38,000 ~ 51,000 行**。先砍到 ~70,000（零风险），再收敛像素断言落到 ~50,000。

---

## 2. 源码冗余：`DevDesk/src` 63,540 行

### 2.1 【真重复】cc-switch SQLite 读取脚手架被抄了两份 —— **~110 行**

`quota/ccswitch_source.cpp:131,139,151,222,229` 与 `ccswitch_import.cpp:81,87,99,103,119` 逐字重复 6 个函数：
`columnText` / `providerColumns` / `hasColumn` / `sqliteError` / `unavailable` / `readAll`

⚠ **注释与实现矛盾**：`ccswitch_import.cpp:6` 写着「本文件直接复用，**不重写一份**」，实际整段抄了。
→ 抽出 `ccswitch_sqlite_read.h` 供两侧共用，或让 import 直接调 `listCcSwitchProviders()` 再做映射。

### 2.2 【真重复】跨模块复制的工具函数

架构约束「`module_*` 不可互相依赖」导致每个模块各抄一份。**正解是下沉到 `core/`**，不是让它们互相调。

| 函数 | 份数 | 位置 |
|---|---:|---|
| `toolOk` / `toolError` | 5 / 4 | `agent/app_stats_tool.cpp`、`claude/claude_tools.cpp`、`markdown/markdown_tools.cpp`、`settings/app_tools.cpp`、`todo/todo_tools.cpp` |
| `parseArgs` | 3 | `claude_tools.cpp`、`markdown_tools.cpp`、`todo_tools.cpp` |
| `strField` | 4 | `agent_loop.cpp`、`message_blocks.cpp`、`markdown_tools.cpp`、`todo_tools.cpp` |
| `escapeLikePattern` | 3 | `call_log_store.cpp`、`markdown_service.cpp`、`todo_service.cpp` |
| `nowUnix` | 4 | `app_config.cpp`、`call_log_store.cpp`、`claude_service.cpp`、`profile_repository.cpp` |
| `toLowerAscii` | 4 | `provider_config.cpp`、`upstream_dialect.cpp`、`md_keyword_highlight.cpp`、`http_headers.h` |
| `rowInt`/`rowStr`/`bindAll`（SQLite 行读取样板） | 2 | `agent_store.cpp`、`quota/quota_store.cpp` |
| `sha256Hex` | 2 | `profile_repository.cpp`、`quota_volcano.cpp` |
| `stripTrailingSlash` / `hostOf` | 3 / 3 | claude_proxy 内部多文件 |
| `isSpace` | 4 | markdown 模块内 4 个文件各自定义 |
| `daysFromCivil` | 2 | **`core/time.h` 已有**，`module_home/home_quota_card.cpp` 又抄一份 |
| `trimAscii` | 2 | `core/paths.cpp`、`markdown_service.cpp` |

自认的重复：`quota/ccswitch_source.cpp:85`「与 `opencode_go_provider.cpp:126` 的 splitUrl 同款（两处各自持有，将来要共用时…）」

### 2.3 【可简化】UI 配置样板：`settings_ui_cards.cpp`（1,156 行）

**8 张卡片各自复制一整条几何派生链**（重复块扫描：同一 6 行块在该文件内出现 **8 次**）：

```cpp
// settings_ui_cards.cpp:90-97（第 1 张卡）… 在 :263 / :337 / :442 / :947 … 原样复现
const float titleH = roundFs(24.0f);
const float descY  = 18.0f + titleH + roundFs(2.0f);
const float descH  = roundFs(18.0f);
const float labelY = descY + descH + roundFs(10.0f);
const float labelH = roundFs(16.0f);
const float ddY    = labelY + labelH + roundFs(6.0f);
const float hintY  = ddY + 40.0f + roundFs(6.0f);
```

以及每个文本块固定 8 行链条（`:102-110`）：
```cpp
ui.text(id + ".title").position(28.0f, 18.0f).size(w - 56.0f, titleH)
  .text("外观").fontSize(t.sizes.fontSizeLarge).lineHeight(titleH)
  .fontWeight(600).color(t.colors.textPrimary).build();
```

→ **抽 `CardRowGeom`（一次算完 8 个 y）+ `labelText()/descText()` helper**，8 份合成 1 份，每个文本块 8 行 → 2 行。**预计 −400 ~ −600 行**，且字号缩放逻辑从 8 处收敛到 1 处（改一次生效）。

同类：`claude_ui_list.cpp:547-567,685`（3 次）、`claude_ui_detail.cpp:1020-1062`（4 次）、`claude_ui_ccswitch.cpp:296` ≡ `claude_ui_list.cpp:707`。

### 2.4 尺寸字面量散落：**2,843 处 `x.0f`**

| 文件 | 处数 |
|---|---:|
| `claude_ui_form.cpp` | 472 |
| `markdown_module.cpp` | 333 |
| `settings_ui_cards.cpp` | 266 |
| `claude_ui_detail.cpp` | 239 |
| `agent_module.cpp` | 187 |
| 其余 | ~1,346 |

好消息：颜色**没有**这个问题——全仓 `0xRRGGBB` 仅 **2 处**、零 `Color(0x...)`，token 化做得彻底。
→ 建议把间距/圆角/控件高（28/56/40/24/18/12/10/6 等高频值）进 `ui_kit/metrics.h`（该文件已存在、仅 69 行，是当前唯一尺寸常量来源，扩展它即可）。

### 2.5 已核查为**误报**，不要动

| 项 | 结论 |
|---|---|
| `ui_kit/{card,dialog_form,toast}.h` | 看起来像孤儿头文件，实际用 `#include <ui_kit/...>` 角括号形式被引用（card.h←about_module、dialog_form.h←6 处、toast.h←5 处）。**在用** |
| `drawToast` 出现 5 处 | 各模块只是薄封装调用 `ui_kit::drawToast`（`todo_module.cpp:576`），**已统一**，非重复实现 |
| `paths_macos/posix/win.cpp` 等平台三连 | 项目硬约束「平台差异拆文件」，**合理，不是冗余** |
| 未被 CMake 收录的源文件 | **0 个**，构建清单干净 |

---

## 3. EUI-NEO：41,063 行"从不参与编译"的重量

实证来源：`DevDesk/build/CMakeCache.txt` + `build/_eui_neo_build` 下 128 个 `.o`。

```
EUI_BUILD_APPS=OFF   EUI_BUILD_USER_APPS=OFF   EUI_BUILD_TEST_FIXTURES=OFF
EUI_ENABLE_MODULES=OFF   EUI_RENDER_BACKEND=opengl   EUI_WINDOW_BACKEND=glfw
```

### 3.1 档 A —— 构建上已裁掉、源码仍在仓库（**25,472 行 / 109 文件**）

| 项 | 行数 | 文件 | 参与编译 |
|---|---:|---:|---|
| `apps/`（gallery/serial_tool/calculator/promo/vcd_viewer/clock…） | 9,702 | 26 | **0 个 .o** |
| `tests/` | 11,320 | 67 | **0 个 .o** |
| `examples/` | 2,665 | 13 | **0 个 .o** |
| `modules/`（keyboard.h 932 + serial 853） | 1,785 | 3 | DevDesk include 数 = 0 |

### 3.2 档 B —— 需改代码才能删（**15,591 行**）

| 项 | 行数 | 说明 |
|---|---:|---|
| `core/render/vulkan/` | 9,234 / 27 | opengl 后端下从不编译；DevDesk 对 `vulkan\|SDL` 引用 = 0 |
| `components/` 未用组件 22 个 | 5,267 / 22 | header-only 且 DevDesk 不 include 聚合头，**从未进入任何 TU**。含 `colorpicker/carousel/timepicker/stepper/navbar/sidebar/checkbox/radio/slider/barchart/datatable/piechart/text/image/panel/workshop×4` |
| `core/app/sdl2_app_main.cpp` | 873 | glfw 二选一，永不编译 |
| `core/audio/` | 217 | DevDesk 零引用（`grep -i audio` 无命中）但**确实被编译**，唯一有编译收益项（−1 TU） |

必留（已验证在用）：`tray_bridge.c` 1279、`render/text.cpp` 2333、`image_source.cpp` 1233、`components/markdown.h` 1722、`input_model.h` 1031、`core/render/opengl/` 3781。

⚠ **注意**：B 档除 `core/audio/` 外，**删了只瘦仓库、不瘦构建**——它们当前一行都不参与编译。
实际编译进 `eui_neo` 的只有 **15,898 行 / 32 文件**。

---

## 4. 建议执行顺序（按 收益/风险 比）

| 序 | 动作 | 减行 | 风险 | 前置 |
|---|---|---:|---|---|
| 1 | 删 `tests/probes/` 9 个 | −7,345 | **零**（CI 本就不跑） | 无 |
| 2 | 删恒真/空洞用例 ~10 处 | −55 | 零 | 无 |
| 3 | 删 EUI-NEO `apps/` + `examples/` + `tests/` + `modules/` | −25,472 | 低（已 OFF） | 确认无人再引用 |
| 4 | 删 EUI-NEO 22 个未用组件 + vulkan + sdl2 | −15,374 | **中**（vulkan 要动 CMakeLists 7 处分支；若日后要 Vulkan 后端则只删组件） | 决策是否保留双后端 |
| 5 | 测 试合并/参数化（quota 三家、settings 样板） | −1,600 ~ −2,400 | 低（需逐条跑绿） | 1 之后 |
| 6 | 抽 `settings_ui_cards.cpp` 几何链 + 文本 helper | −400 ~ −600 | **中**（UI 几何，须 `--screenshot` 目视比对） | 有截图基线 |
| 7 | 工具函数下沉 `core/`（toolOk/parseArgs/strField/…） | −600 ~ −900 | 中（跨 5 个模块，须过模块隔离守卫） | `check-module-isolation.sh` |
| 8 | 合并 `ccswitch_*` SQLite 脚手架 | −110 | 低 | 无 |
| 9 | UI 像素断言改关系断言 | 不减行 | 中（875 处） | 6 之后 |

**第 1、2、3 步合计 −32,872 行，几乎零风险，可立即执行。**

---

## 5. 风险提示

- **别动 `cc-switch/`**（只读参考，永不 commit）。
- 步骤 4 删 vulkan 前先决策：DevDesk 是否保留未来切 Vulkan 的能力。若保留，只删 22 个组件（−5,267）。
- 步骤 6/9 涉及 UI 几何，**必须**用 `bash scripts/build-test-run.sh` + probe 截图目视比对，否则字号缩放回归无从发现。
- `DevDesk/CMakeLists.txt:76` 注释声称依赖 `EUI-NEO/tests/probes/settings_dropdown_z_probe.cpp`，但该路径在 EUI-NEO 下**不存在**——删 `tests/` 前先确认这处注释是否已过期。
- 测试总数以运行输出 `Total:` 为准，删减后基线变化需同步更新 `CLAUDE.md`/`AGENTS.md`（当前记 1259/1253）。

---

## 6. 执行记录（2026-10-04，分支 `refactor/lightweight`，基于 `dev`）

**净 −5,096 行**（+609 / −5,705，含本次新增的两个 core 头）。

### 6.1 已落地

| # | 动作 | 净变化 | 验证 |
|---|---|---:|---|
| 1 | 删 8 个与无头单测逐条重复的 probe | **−5,346** | 构建绿；剩余 probe `ctest -L probe` **6/6 绿** |
| 2 | 删 4 条恒真用例（enum 互不等 ×2、排中律、与 `todo_pageid` 重复的顺序不变量） | −24 | `Total` 1286→1282（−4 = 恰好删的 4 条） |
| 3 | cc-switch 读取脚手架合并 → 新 `quota/ccswitch_sqlite.h` | −65 | 构建绿；`test_claude_quota_ccswitch` / `test_ccswitch_import` 全绿 |
| 4 | 工具函数下沉 `core`（`escapeLikePattern`×3、`trimAscii`×2、`nowUnix`×4） | −36 | 构建绿；全量测试绿 |
| 5 | 设置页 8 处标题行 + 4 处描述行的 8~9 行 builder 链 → `cardTitle`/`cardDesc` | −49 | 构建绿（**逐字等价，零视觉变化**——属性顺序、position-x、字重、颜色、字号均已逐处核对） |

保留的 4 个 probe 各持独占判据：`home_probes`（quota 真数据落图，是 `quota_live_probe --dump` 的**唯一**下游）/ `screenshot_probe`（帧捕获能力自测）/ `settings_dropdown_z_probe`（只有真窗口能测 z 序）/ `module_toggle_sidebar_probe`（唯一真点击派发栈）。

### 6.2 未落地 + 理由（是判断，不是遗漏）

| 报告建议 | 结论 | 理由 |
|---|---|---|
| §3 EUI-NEO 裁剪 −41,063 | **不做** | 报告 §3.2 自己认定「除 `core/audio/` 外删了**只瘦仓库不瘦构建**」；A 档同样已被 4 个 `OFF` 开关关在构建外。唯一有编译收益的 `core/audio/`（−1 TU）是 `include/eui_neo.h` 公共 API（该总头第 5 行就 `#include "eui/audio.h"`），删它=破坏框架 API 契约。且 EUI-NEO 是永不 push 的 fork + 仓库有 `pr-bodies/`（上游 PR 意图），删 `tests/`/`apps/` 会削弱这条路径。**收益≈0、风险>0。** |
| §1.3 测试合并/参数化 −1,600~2,400 | **不做** | §1.2 的「38 个用例断言 ≤1」经复核**不成立**：`test_todo_service.cpp` 的 20 条 `todo_svc_*`、`test_home.cpp` 的 4 条都是**宏展开的 2 行者**，断言在宏体内，静态扫描看不见。按此清单批量删 = 删掉有效测试。**判据不可靠。** |
| §9 UI 像素断言改关系断言（875 处） | **不做** | 需逐条目视比对；而唯一覆盖设置页的 `settings_cards_probes` 恰在本次被删 → **没有验证手段**。要先有截图基线才能动。 |
| §4 删 vulkan / sdl2 −10,107 | **不做** | 同 EUI-NEO：只瘦仓库；且删除即失去双后端能力。 |

### 6.3 验证

- 构建：`bash scripts/build-test-run.sh --no-run` 全绿；6 道静态守卫自检全过（好树绿 / 坏树红）。
- 单测：`Total: 1282 / Passed: 1276 / Failed: 0 / Skipped: 6`（基线 1286/1280/0/6）。
- probe：`ctest -L probe` **6/6 Passed**（真实窗口，非 SKIP）。

### 6.4 顺带修的

- `Testing/`（ctest 在源码根跑时生成的临时目录）此前未被忽略，已加入 `.gitignore` 并从暂存区剔除。
- `scripts/check-probe-skip-gate.sh` 自检夹具里引用的 probe 名（`about_probe`）随删除同步改为 `home_probes`。
- `CLAUDE.md` / `TODO.md` / `scripts/build-test-run.sh` / `tests/CMakeLists.txt` / `probe_harness.h` 里写死的「12 个 probe 可执行 / 14 个 probe 测试 / 13 个探针源文件」全部同步为 4 / 6 / 5。
