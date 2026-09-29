# Bug B 修复验证留证：桌宠「显示桌宠」无可见反馈（2026-09-30）

分支 `fix/pet-enable-feedback`（自 master d201cb5 切出，worktree 隔离）。

## 问题与修法

- **现象**：运行中开桌宠模块后点「显示桌宠」，只有页头副标题小字（setStatus）+ 日志，
  用户视线在卡片上感知不到——真机反馈「点了没效果」（用户日志 00:32:32「本会话未装配，
  重启后生效」）。
- **根因**：桥 `module::pet::showPetWindow()` 返回 bool，false 三义（未装配 / 窗口已开 /
  启动失败但已置装配）全部折成同一句误导文案，且唯一反馈通道是页头小字。
- **修法**：
  1. 桥收成四态枚举 `PetShowOutcome{Shown, AlreadyOpen, NotAssembled, StartFailed}`
     （`src/module_pet_bridge.h`，签名哨兵同步）；`startPetWindow` 改返 bool
     （皮肤不可用 = false，未入队开窗）。
  2. settings 侧（`src/module_settings/settings_module.cpp`）：`showPetWindow()` 按四态
     分流——NotAssembled → toast「已启用，重启 DevDesk 后生效」（中性非错误），
     StartFailed → toast「桌宠启动失败」（错误样式），AlreadyOpen → toast「桌宠已在显示中」
     （修掉原「已开着被误报未启用」），Shown → 维持状态行；模块开关
     `applyPetModuleVisibility(true)` 同口径（Shown/AlreadyOpen 窗口本体即反馈，只
     setStatus；两失败分支给 toast）。
  3. settings 首次接入 toast（ui_kit::ToastState + drawToast，claude 同款
     3.5s / fa-circle-check，节点 id `settings.toast`）。

## 测试数（`bash scripts/build-test-run.sh --no-run --filter X`，worktree 内）

| 跑法 | 结果 |
|---|---|
| master 基线（对照，主树 d201cb5） | `--filter settings` → Total: 92 / Passed: 92 / Failed: 0 |
| 修复后 `--filter settings` | **Total: 94 / Passed: 94 / Failed: 0**（+2 = 新增两条） |
| 修复后 `--filter pet` | **Total: 45 / Passed: 45 / Failed: 0** |
| 具名 `./devdesk_tests toast` | 9/9，含两条新用例 PASS |
| 具名 `./devdesk_tests showPetWindow` | 2/2，含新桥用例 PASS |
| 模块隔离守卫（脚本自动串跑） | 通过（settings 零 pet 依赖 / 桥调用齐全） |

新增用例：

- `settings_pet_show_button_toast_when_not_assembled`——未装配态点「显示桌宠」→
  `settings.toast.title` 文案断言 =「已启用，重启 DevDesk 后生效」+ 空正文（中性）+
  零开窗请求入队（headless 真点击 `settings.card.pet.show.bg`）。
- `settings_pet_show_button_toast_when_start_failed`——「可发现不可加载」皮肤夹具下
  点按钮 → toast「桌宠启动失败」+ 正文含「皮肤」。
- `pet_show_window_outcomes`——桥四态各归各位（NotAssembled / StartFailed / Shown /
  AlreadyOpen，后两者分别经夹具与 onWindowCreated 回填假句柄构造）。
- `pet_show_window_reenqueues`（既有）断言更新为 `== PetShowOutcome::Shown`。

## 变异自检（红 → 绿）

**变异**：`src/module_settings/settings_module.cpp` 两处
`showToast("已启用，重启 DevDesk 后生效")` → `setStatus("桌宠本次会话未启用，重启后生效")`
（即退回修复前的「只 setStatus、无 toast」形态；桥层不动，编译通过）。

红（变异态，`--filter settings`）：

```
[FAIL] settings: 未装配态点「显示桌宠」出「重启生效」toast（Bug B） [6ms]
Total:  94 Passed: 93 Failed: 1
❌ 测试有失败（上方为 FAIL 摘要；要看完整输出跑: …/build/tests/devdesk_tests）
```

绿（还原变异后复跑，`--filter settings`）：

```
Total:  94 Passed: 94 Failed: 0
✅ 测试全绿
```

结论：新增 toast 断言非恒真——反馈接线被回退即红，恢复即绿。

## 边界与已知限制

- toast 的红/绿**样式**（isError → 图标/配色切换）由 ui_kit::drawToast + EUI-NEO
  components 承载，本批断言钉的是**文案与分支分类**（与 claude 既有 toast 测试同口径）。
- AlreadyOpen 无头不可自然到达（handle 只在真主循环回填），桥用例经
  `onWindowCreated` 回填假句柄构造，settings 层该分支无单测。
- 运行中装配（免重启即时开窗）按任务口径**另立后续**，本批只做诚实反馈。
