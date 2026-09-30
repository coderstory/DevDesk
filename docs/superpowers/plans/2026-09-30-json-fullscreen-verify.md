# 高级设置 JSON 全屏查看/编辑 — 验证留证（2026-09-30）

分支 `feat/json-fullscreen-editor`（base `a1451e9`），worktree `/Users/coderstory/dev/gui/wt-json-fs`。
本文件记录验收命令与实测输出、变异自检、probe 落图；按「功能开发生命周期」阶段 4 口径。

## 1. 定向测试（scripts/build-test-run.sh，macOS arm64 Debug）

```
bash scripts/build-test-run.sh --filter claude  --no-run
  → Total: 141 / Passed: 141 / Failed: 0   （含新增 2 条全屏用例）
  → 模块隔离守卫 ✅

bash scripts/build-test-run.sh --filter settings --no-run
  → Total: 101 / Passed: 101 / Failed: 0
  → 模块隔离守卫 ✅
```

## 2. 新增用例（tests/test_modules/test_claude_ui.cpp）

| 用例 | 断言点 |
|------|--------|
| `claude_ui_json_fullscreen_enter_cover_and_esc` | ① 进入全屏：`claude.jsonfs.bg` 几何 == (0,0,1600,1000)（覆盖整个窗口/页面内容区）；表单 `claude.dialog.form.panel` 面板整体落在 bg 范围内（「盖住表单 dialog」的几何包含）。② **z 档位钉（审计 F-1 补）**：`claude.jsonfs.root` zIndex == 1050 且 > 表单弹窗根 zIndex（dialog 默认 1000）、< `claude.toast` zIndex（1100，每帧无条件构建）——覆盖层绘制/命中序高于表单弹窗、让位 toast。③ 全屏框输入 → Esc（输入框 onCancel 消费，返回 true）→ 全屏退、表单还在（opacity 1.0）、未应用文本留在缓冲且回表单首行可见。④ 模块级 Esc（closeTopmostDialog）第一次退全屏、第二次才关表单（序链次序） |
| `claude_ui_json_fullscreen_buffer_roundtrip_and_error_gate` | ① 表单框输 compact → 全屏内「格式化」→ 完成 → 保存 → DB 为 pretty（若全屏是第二份缓冲，格式化空串 no-op、DB 仍是 compact → 红；F-7 同款变异钉口径）。② 全屏框直输 compact → 完成 → 保存 → DB 含该键（全屏写的就是表单保存的那份缓冲）。③ 「红字同源可见」：编辑态全屏改坏 JSON → 「应用」→ `claude.jsonfs.error` 非空、全屏不退、表单不关；「完成」回表单后 `claude.dialog.form.error` 文本与全屏 error **相等**；坏值不落库（全表扫描无 `{broken`）。④ **判别式（审计 F-6 补）**：先「格式化」坏 JSON（仅行内源 formAdvancedJsonError_ 非空）→ 全屏 error = 「解析失败…」；再「应用」触发基础项闸门（formError_ = 名称/URL 文案，行内源仍在）→ 全屏 error 切换为该文案且不含「解析失败」——两源文本不同，读错源/优先级颠倒即红；仍不退全屏 |

## 3. 变异自检（覆盖层删除 → 红）

`src/module_claude_proxy/claude_module.cpp` 注释掉 `drawJsonFullscreenOverlay(...)` 调用后重建：

```
FAIL [../tests/test_modules/test_claude_ui.cpp:3191] bg != nullptr
FAIL [../tests/test_modules/test_claude_ui.cpp:3326] click(ui, "claude.jsonfs.format.bg")
[FAIL] claude_module: JSON 全屏编辑进入覆盖窗口几何 + Esc 退回表单文本保留（headless）
[FAIL] claude_module: JSON 全屏编辑同一缓冲往返（双向落库）+ 应用校验红字同源（headless）
Total: 2 / Passed: 0 / Failed: 2
```
（输出节选：用例 B 在 :3330 click done、fs_d6 REQUIRE error 在场等多处 CHECK/REQUIRE 同红，doctest 不中断用例，此处只引首两行。）

恢复调用后重建：两用例恢复 PASS（Total 2 / Passed 2），`--filter claude` 全量 141/141。
结论：新增断言非恒真——覆盖层一旦不画（或画错层），用例立刻变红。

### 3.1 z 档位变异（审计 F-1 收口，2026-09-30）

zIndex(1050)→zIndex(900) 变异（form.cpp `claude.jsonfs.root`），重建后：

```
FAIL [../tests/test_modules/test_claude_ui.cpp:3212] fsRoot->zIndex != 1050
[FAIL] claude_module: JSON 全屏编辑进入覆盖窗口几何 + Esc 退回表单文本保留（headless）
Total:  2 / Passed: 1 / Failed: 1
```

红。900 < dialog 1000 时 EUI-NEO 兄弟 stable_sort 反序命中会让表单弹窗的全屏
backdrop（onClick=关表单）先被命中——功能真坏而旧用例全绿，现被 z 值钉直接拦下。
恢复 1050 后 `--filter claude` 141/141 全绿。

## 4. probe --dump（全屏态落图）

```
build/tests/claude_probes --size 1200x1000 --dump /tmp/jsonfs-probe/jsonfs
  → probe 既有几何断言全通过（三档尺寸矩阵无 FAIL，exit 0）
  → dump: jsonfs_json_fs.ppm（2400x2000 = 1200x1000@2x）
```

（json_fs 在 probe 里是 onClick 驱动 + dumpState 的落图步骤；覆盖层自身的几何与
z 由单测断言钉住，见 §2/§3.1。）

落图内容核对（人工看图）：整区覆盖层（标题「高级设置 JSON（全屏）」+ hint + 大号多行
JSON 框，预填 profile 原文）+ 底部「格式化 / 应用 / 完成」三按钮；表单 dialog 被完全盖住。
PPM→PNG 转换图：`/tmp/jsonfs-probe/jsonfs_json_fs.png`（临时产物，不入库）。

## 5. 层级选型（写进 commit message 的结论）

取「ui_kit 顶层 overlay（画在全部弹窗之后 + 显式 z=1050）」而非 splash 式
app_sidebar compose 顶部 return 拦截：全屏态是 claude 模块私有状态，app 层拦截需要
把模块状态透到 app/loader 层（跨层耦合；splash 是 app 独占状态才走该形态）。z=1050
介于 dialog 默认 1000 与 toast 1100 之间——盖住表单弹窗、让位「应用」成功的 toast。

## 6. 未做 / 边界

- 未启动真实 app 人工点验（阶段 6 由用户真机点验）；无头几何 + probe 落图覆盖至此。
- 仅 macOS 验证（本机开发平台）；改动无平台分支。
- 「应用」成功随 submitForm 关表单 → 全屏同帧消失（可见性派生 formOpen_ &&
  formJsonFullscreen_）；未采用「应用成功留在全屏」——见 commit message 理由。

## 7. 审计收口（2026-09-30，审计文件 2026-09-30-json-fullscreen-audit.json）

| finding | 处置 |
|---------|------|
| F-1 medium（z 档位零断言，1050→900 变异仍绿） | **已修**：用例 A 补三条 z 断言（root==1050 / >dialog 根 / <toast 根）；§3.1 变异红→恢复绿留证 |
| F-2 low（基础项错误在全屏态不可达，红字与修复动作断开） | **已修**：hint 行补「基础项（名称/URL/Key）缺失需回表单基础页补填」 |
| F-3 low（formJsonFullscreen_ 复位仅靠两个入口，不变式无结构性保障） | **已修**：drawJsonFullscreenOverlay 早退分支顺手 `formJsonFullscreen_=false`——表单不在场 ⇒ 旗必复位，任何关表单路径下一帧即清 |
| F-4 low（verify 文档两处措辞失真） | **已修**：§3 注明输出节选（用例 B 另有多处同红）；§4 改「probe 既有断言通过 + 落图，覆盖层几何由单测断言」 |
| F-5 info（偏离任务书「留在全屏」的记录） | 不动（偏离论证已双处留痕，审计确认成立） |
| F-6 info（红字同源断言非判别） | **已修**：用例 B ④ 构造两源文本不同状态，路径①（行内源可达）/路径②（提交闸门优先）分别判死 |
