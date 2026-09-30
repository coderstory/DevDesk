# 共享配置片段列表限高 2 条 — 验证留痕（2026-09-30）

实现分支 `fix/shared-list-cap-2`（自 master `a1451e9` 切出）；工作区
`/Users/coderstory/dev/gui/wt-shared-list-cap`；全部构建 / 测试走
`scripts/build-test-run.sh`（worktree 自动定位 EUI-NEO + ccache）。

**用户原话**：「共享配置片段这个列表高度需要限制，最多显示 2 条，超过使用滚动条。」

改法一句话：区块自然高从「随条目数涨」（`76 + N×64`）改为**恒定**
（`kSharedFixedH + kSharedCardsH` = 76 + 128 = 204，条目区恒 2 整行），
条目区继续用既有 `components::scrollView` 块内滚——超过 2 条出滚动条；
`sharedH` 的 4 处消费点全部落在 `claude_ui_list.cpp` 内（三路径调用 + 列表视口
`listH` 推导），签名从 `sharedBlockH(bodyH, count, hasError)` 收成
`sharedBlockH(bodyH)`，块高不再随片段数变化。

## 空态 / 1 条策略（恒 2 条框架，留白不收缩）

0 条 / 错误态 / 1 条也按 2 行框架画（提示行画在框架顶部，其余留白）。理由：
左列 Profile 列表视口 `listH = sharedBlockTop − 间距 − 列表起点`，而
`sharedBlockTop` 由区块高推出——区块恒高则增删片段 / 出错恢复**整列不跳**
（旧形态 0 条 94 / 1 条 140 / 4 条 332，每加一条全列重排一次）。
恰 2 条时内容高 128 == 视口 → `scrollMaxOffset == 0`，**不出滚动条**，对齐
用户口径「超过才滚」（条目区取「2 个整行 128」而非「2 卡 + 1 间距 120」正是
为此；120 会让恰 2 条也出一条 8px 的空滚动条）。极矮窗退化路径保留
`min(自然高, sharedBlockMaxH(bodyH))` 上界（条目区 < 2 行仍可滚，LOG_ERROR
守门原样）。

## 判据 1：定向测试全绿

`bash scripts/build-test-run.sh --filter claude --no-run`

- 基线（改前，同 worktree）：`Total: 139 Passed: 139 Failed: 0`
- 实施后（含 1 条新用例）：**`Total: 140 Passed: 140 Failed: 0`** + 模块隔离守卫通过

## 判据 2：几何断言

新用例 `claude_shared_list_cap_two_cards`（`tests/test_modules/test_claude_ui.cpp`，
4 个独立 TestEnv：0/1/2/3 条片段，compose 1600×1000 后断言真实元素 frame）：

1. **区块高恒 204**（76 固定 + 128 条目框架），0/1/2/3 条全同 —— 含 `blockH0`
   跨档相等断言；主变异判别点。
2. **不溢出**：区块底 ≤ 底部按钮行上沿（钉底锚点）。
3. 0 条：空态提示行存在、`claude.shared.scroll` **不存在**（无条目可滚）。
4. ≥1 条：`claude.shared.scroll` 存在，可视区恒 **128**（2 整行）。
5. 1/2 条：`scrollMaxOffset == 0`（不出滚动条）。
6. 3 条：前 2 条完整落在视口内、第 3 条顶 ≥ 视口底（offset 0 下不可见）、
   `scrollMaxOffset > 0`。
7. 3 条：滚一行（64）后偏移跨帧保持（`scrollOffset == 64`）且余量 ≥ 一整行。
   注：无头布局 frame 是内容坐标、不随 offset 平移（偏移在渲染期应用），故第 7
   条断状态 + 余量，「第 3 条进入视口」的渲染侧核对由 probe --dump 落图。

probe 侧（`tests/probes/claude_probes.cpp` 新增 ⑤，8 片段种子 × 3 档尺寸
1200×720 / 1200×1000 / 1262×801）：`claude.shared.scroll` 存在、可视区 == 128、
第 3 条卡顶 ≥ 视口底、`scrollMaxOffset > 0`；`claude_probes` 三档全过 exit 0。

## 判据 3：变异自检（红 → 绿）

变异：`sharedBlockNaturalH`/`sharedBlockH` 改回随条目数涨（旧三参形态，
`-Werror,-Wunused-const-variable` 同步注释掉 `kSharedCardsH`）→

```
FAIL [../tests/test_modules/test_claude_ui.cpp:5331] std::fabs(block->frame.height - 204.0f) <= 0.5f
Total: 1 Passed: 0 Failed: 1        ← --filter 限高

claude_probes[1200x720]: shared scroll viewport 210 != 128 (2 行；bug shape: 区块自然高随条目数涨)
claude_probes[1200x720]: card 2 top 476 在可视区内（底 558）—— offset 0 下可视区超过 2 条
claude_probes[1200x1000]: shared scroll viewport 490 != 128 ...
claude_probes[1262x801]: shared scroll viewport 291 != 128 ...
```

还原恒高 → `Total: 140 Passed: 140 Failed: 0`、`claude_probes` exit 0。
即断言在「改回随条目数」时红、修复态绿，非恒真。

## 判据 4：dump probe 截图（真 GLFW glReadPixels，about / settings 批先例）

`build/tests/claude_probes --dump /tmp/slc-cap` → `glReadPixels` 写 P6 PPM
（present 前取帧，不依赖系统截屏）→ `sips` 转 PNG，落库：

- `docs/screenshots/2026-09-30-shared-list-cap/actions7-2cards-scrollbar.png`
  （8 片段种子，详情 tab 0 帧）

观察记录：左列「共享配置片段」区块可视区恰 2 张卡（片段 65 / 片段 66）+
区块内滚动条（8 条种子），「+ 新建片段」钉在区块底 / 面板底、未被挤压；
Profile 列表与其视口不受影响。

## 既有断言回归面（2 处随视口高度的机械更新）

- `uiModernVirtualProfileRows`（`tests/test_modules/test_ui_modern.cpp:717`）：
  区块高 94→204（0 片段档）→ 列表视口矮 110px → 滚动偏移 600→1500 让行 17
  重新落进「偏移 ± 1 视口 overscan」渲染窗；`claude.profile.0.bg` 断言改
  `claude.profile.17.bg`（offset 1500 下第 0 行已被 slot 窗口裁掉——窗口化语义
  未变，反而更明显）。
- `claude_shared_block_outside_viewport` / `claude_shared_block_bottom_anchored`
  只锁锚点不变量（区块底 == 底部按钮行上沿 − 12 等），恒高后依然成立，零改动。

## 边界核对

- `sharedH` 消费点：`grep -n sharedH src/` 全部命中 `claude_ui_list.cpp`
  （:433 计算 + :447/:466/:585 三路径调用 + `listH` 推导消费 `sharedBlockTop`）；
  模块外零消费。
- 几何常量（`kSharedCardsH` 等）保持 cpp 匿名命名空间私有；测试侧以字面量
  204/128/64 断言 + 注释对应关系（双向漂移哨兵）。
- 平台零差异：纯布局几何，无平台分支 / 无 `#ifdef`。
- 改动文件：`src/module_claude_proxy/claude_ui_list.cpp`、
  `tests/test_modules/test_claude_ui.cpp`（+1 用例）、
  `tests/test_modules/test_ui_modern.cpp`（2 处机械更新）、
  `tests/probes/claude_probes.cpp`（+⑤ 断言）、本文件、1 截图。
