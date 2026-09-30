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
| `claude_ui_json_fullscreen_enter_cover_and_esc` | ① 进入全屏：`claude.jsonfs.bg` 几何 == (0,0,1600,1000)（覆盖整个窗口/页面内容区）；表单 `claude.dialog.form.panel` 面板整体落在 bg 范围内（「盖住表单 dialog」的数值表达，z=1050 > dialog 1000）。② 全屏框输入 → Esc（输入框 onCancel 消费，返回 true）→ 全屏退、表单还在（opacity 1.0）、未应用文本留在缓冲且回表单首行可见。③ 模块级 Esc（closeTopmostDialog）第一次退全屏、第二次才关表单（序链次序） |
| `claude_ui_json_fullscreen_buffer_roundtrip_and_error_gate` | ① 表单框输 compact → 全屏内「格式化」→ 完成 → 保存 → DB 为 pretty（若全屏是第二份缓冲，格式化空串 no-op、DB 仍是 compact → 红；F-7 同款变异钉口径）。② 全屏框直输 compact → 完成 → 保存 → DB 含该键（全屏写的就是表单保存的那份缓冲）。③ 编辑态全屏改坏 JSON → 「应用」→ `claude.jsonfs.error` 非空、全屏不退、表单不关；「完成」回表单后 `claude.dialog.form.error` 文本与全屏 error **相等**（同源 formError_）；坏值不落库（全表扫描无 `{broken`） |

## 3. 变异自检（覆盖层删除 → 红）

`src/module_claude_proxy/claude_module.cpp` 注释掉 `drawJsonFullscreenOverlay(...)` 调用后重建：

```
FAIL [../tests/test_modules/test_claude_ui.cpp:3191] bg != nullptr
FAIL [../tests/test_modules/test_claude_ui.cpp:3326] click(ui, "claude.jsonfs.format.bg")
[FAIL] claude_module: JSON 全屏编辑进入覆盖窗口几何 + Esc 退回表单文本保留（headless）
[FAIL] claude_module: JSON 全屏编辑同一缓冲往返（双向落库）+ 应用校验红字同源（headless）
Total: 2 / Passed: 0 / Failed: 2
```

恢复调用后重建：两用例恢复 PASS（Total 2 / Passed 2），`--filter claude` 全量 141/141。
结论：新增断言非恒真——覆盖层一旦不画（或画错层），用例立刻变红。

## 4. probe --dump（全屏态落图）

```
build/tests/claude_probes --size 1200x1000 --dump /tmp/jsonfs-probe/jsonfs
  → 全部几何断言通过（exit 0），三档尺寸矩阵无 FAIL
  → dump: jsonfs_json_fs.ppm（2400x2000 = 1200x1000@2x）
```

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
