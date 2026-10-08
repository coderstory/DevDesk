# 缩放派生审查（Wave B 项 A）— 验证留痕（2026-09-30）

实现分支 `fix/settings-card-fs-derive`（自 master 系 `73af50b` 切出，含 `0edd3cf`
env-pane 合并）；工作区 `DevDesk/.claude/worktrees/settings-card-fs-derive`；
全部构建/测试走 `scripts/build-test-run.sh`（worktree 自动定位 EUI-NEO）。

改法一句话：settings 卡片行盒/行距/卡高从生效字号派生（`roundFs(base) =
round(base × fontScale)`，env-pane `1ca87b0` 同口径，100% 档逐值等于旧字面 =
零回归锚），行 y 沿派生盒底累加（spec §2.7「优先把链改派生」），代理卡行几何
`proxyCardHeightFor` 单一来源（draw 与 compose 卡高公式同源）。

## 判据 1：定向测试全绿

`bash scripts/build-test-run.sh --filter settings --no-run`

- 基线（改前，同 worktree）：`Total: 90 Passed: 90 Failed: 0`
- 实施后（含 2 条新用例）：**`Total: 92 Passed: 92 Failed: 0`** + 模块隔离守卫通过
- 探针套件：`ctest -L probe` → **8/8 passed**（含被改的 `settings_cards_probes`）

## 判据 2：几何断言（100% 与 130% 双档，fontScale RAII 守卫）

新用例 `settings_card_rows_scale_derived`（tests/test_modules/test_settings.cpp）：
每 tab compose 后对行盒矩形池两两断言——

1. **行盒两两不重叠**（跨卡同池，x 相离的同行「标签|控件|说明」列不算重叠）；
2. **卡 frame 两两不重叠** + **行盒 ⊆ 所属卡 frame**（卡高没跟派生即红）；
3. **派生钉值**（字面期望，非恒真）：130% 档 `appearance.theme.label 高 == 21`、
   `desc 高 == 23`、`title 高 == 31`、`modules 行距 == 73`、`行内偏移 == 26`、
   `proxy 卡高 == 142`；100% 档同点位 == 16/18/24/56/20/132（旧字面）。
   日志卡 20px 盒两档恒 20（未派生行对照钉）。

守卫形态与 `claude_env_tab_no_overlap_and_model_keys_hidden`（test_claude_ui.cpp）
同款 `FontScaleGuard` RAII（进程级全局状态，REQUIRE 提前 return 靠析构还原）。

## 判据 3：变异自检（红 → 绿）

变异：`roundFs` 改回写死（`return base;`，即行盒恒等 100% 字面）→

```
FAIL [test_settings.cpp:1559] ms::roundFs(16.0f) != 21.0f
FAIL [test_settings.cpp:1798] problem.empty() — appearance.title 高 = 24.000000（期望 31.000000）
Total: 92 Passed: 90 Failed: 2        ← --filter settings；两条新用例红
```

（90 条既有用例在变异下仍全绿——即 100% 档零回归锚的直接证明：写死 ≡ 改前行为。）

还原派生 → `Total: 92 Passed: 92 Failed: 0`。变异仅存在于 `roundFs` 单点
（settings_ui_logic.h），全部派生消费点同时回退，证明断言非恒真。

## 判据 4：dump probe 截图（真 GLFW glReadPixels，todo/about 批先例）

`settings_cards_probes --dump <prefix>`（§D，本次新增）：通用 tab 在 fontScale
100% / 130% 各真渲染一帧，`glReadPixels` 读 GL 后备缓冲写 P6 PPM（present 前
取帧），`sips` 转 PNG：

- `docs/screenshots/2026-09-30-settings-fs-derive-100.png`
- `docs/screenshots/2026-09-30-settings-fs-derive-130.png`

观察记录：两档外观卡/编辑器字号卡行盒均无贴边无裁切，130% 档降级提示行
（派生盒）完整、三列下拉与标签无叠印。
**附带发现**：130% 档页头「设置」标题墨迹压副标题行——页头是 `ui_kit::kHeaderH`
跨页共享 chrome、非卡片行盒，不在本期修范围，已登记
`2026-09-30-scale-derive-tech-debt.md` T5（截图即证据）。

## 边界核对

- 零默认值改动：`settings_config.h` 零触碰（`fontScalePercent` 默认 100 /
  `kMaxFontScalePercent=130` 原样）。
- 零 `claude_ui_*` 触碰（env pane 归 `fix/detail-env-pane`，已并入 master）。
- 改动文件：`settings_ui_logic.h`（roundFs + 代理行 helper + 卡高常量注释）、
  `settings_ui_cards.cpp`（5 卡行盒/链派生）、`settings_module.cpp`（cardHeight
  Fixed 分支 + proxyH 公式）、`test_settings.cpp`（+2 用例）、
  `settings_cards_probes.cpp`（+§D dump）、spec §2.3 状态行 + 本文件 + tech-debt
  清单 + 2 截图。
