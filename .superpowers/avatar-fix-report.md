# Profile 头像圆圈三处改 — 实施报告（2026-10-01）

## 用户需求（原话）

「profile 列表里的 item 有个白色的圆圈，里面有个问号，且没有垂直居中。现在帮我改：这个圆圈改成随机颜色的圆圈，且有外层包边。」

## 改了什么 / 为什么

落点：`src/module_claude_proxy/claude_ui_list.cpp`（drawProfileCard + 匿名命名空间常量区）

1. **白底 → 随机色（按 profile 稳定派生）**
   - 为什么不是真随机：compose 每帧都跑，每帧真随机会闪烁。控制器裁定「随机 = 稳定派生」。
   - 实现：FNV-1a 64 对 `p.id`（int64，持久化主键，改名不变）逐字节哈希 → 对 9 色板取下标（`avatarHash` / `kAvatarPalette`）。同一 profile 永远同色，不同 profile 大概率不同色。
   - 色板来源集中一处：`kAvatarPalette[9]`（claude_ui_list.cpp 匿名命名空间）——9 个低饱和暖色系基色（clay/陶土/赭石/沙棕/灰绿/桉叶/雾蓝/藕紫/豆沙），与主题 Claude Clay #D97757 同族协调。深浅主题经 `eui::mixColor`（header-only 值类型桥接）自由基色派生：浅色主题 = 基色调白成粉彩底 + 加深一档描边/字形；深色主题 = 基色调暗成深底 + 调亮一档描边/字形。
   - 字形**颜色**从 `textPrimary` 改为派生 ring 色（圆内对比度保证）；**内容口径不变**（名字首字符 / 空名占位 `?`，未动）。

2. **外层包边**
   - 实现：`.border(1.5f, avatar.ring)`，ring 色 = 基色加深（浅主题）/ 提亮（深主题）一档（控制器许可的「同色系加深一档」方案，选它而非中性 borderDefault 的理由：与随机底色成套、视觉是「同色环」而非孤立灰线）。
   - 旧代码本有 1px borderDefault 但用户看不见（视为「没有包边」）→ 改 1.5px 同色环。

3. **垂直居中**
   - 根因：旧 `y + 16` 使圆心落在 y+36，而 96 高卡片中心是 y+48（偏上 12px）。
   - 修：`avatarY = y + (h - avatarSize) * 0.5f`（从 h 推导，行高变了也居中）；字形框同 y。

## TDD 证据（先红后绿 + 变异验证）

- **RED**：先写测试 `claude_profile_avatar_color_ring_centered`（tests/test_modules/test_claude_ui.cpp，接在 claude_profile_active_badge_centered 后），对未改代码跑：`FAIL [test_claude_ui.cpp] !isPlaceholder — 头像圆圈仍是占位底色 surfaceElevated`。
- **GREEN**：实现后 `Total: 1 Passed: 1 Failed: 0`。
- **变异验证**（证明断言非恒真；框架 fail-fast，RED 时 (b)(c)(d) 未跑到，逐条补做）：
  - 变异① border 宽 0 → (c) 捕获：`av->border.width >= 1.0f — 头像圆圈缺外层包边`
  - 变异② 取消居中（回 y+16）→ (d) 捕获：`圆心 246.000000 vs 行中心 258.000000`（恰 12px 偏移）
  - 变异③ 色板下标恒 0 → (b) 捕获：`anyDistinct — 三个不同 profile 的头像色全相同`
  - （变异③第一版让 avatarHash 失去引用 → -Werror unused-function 构建失败、陈旧二进制跑出假失败；已改保持引用的变异形态重跑，见下方「过程教训」）
- 断言清单：(a) 同 profile 两次 compose 色一致 + 非占位色非纯白；(b) 三 profile 至少两色；(c) border 宽 ≥1；(d) 圆心 y == 行 y + 行高/2（容差 0.5）+ 字形框与圆同心。

## 真机验证（隔离数据根）

- `DEVDESK_DATA_ROOT=/tmp/avcheck_dataroot` 启动 worktree 构建产物（未碰 /Applications/DevDesk.app；只杀自己启动的 PID）。
- 钥匙串安全：空数据根启动历史上清过用户真实 key——本分支 base 已含 keychain 护栏（2c2afb3：空 keep 跳过 + claude: 白名单 + service 按数据根派生，69e634b），启动前核实了 `keychainServiceForDataRoot` 派生逻辑，测试根不触真实钥匙串。
- sqlite3 CLI 直插 3 个 profile（Anthropic 官方 / Kimi 直连 / 智谱 GLM 网关），cliclick 点侧边栏进 Claude 页。
- 截图：`.superpowers/avatar-fix-shot2.png`（整页）、`.superpowers/avatar-fix-zoom.png`（列表特写）。三行头像分别为桉叶绿 / 赭石 / 雾蓝的粉彩底 + 同色环，行内垂直居中；?（智谱行，空 keychain 显示占位字形——已知态，非 bug）。
- OpenGL 旧帧坑：动了鼠标（重绘触发）后拍的第二张，取后一张为准。

## 测试数字

- 定向：头像圆圈 1/1 绿；变异 ×3 各红。
- 全量：见提交前数字（`scripts/build-test-run.sh --no-run`）；probe：`ctest -L probe` 9/9（提交前实跑，以运行输出为准）。

## 过程教训（留档）

- 本测试框架的 filter 匹配的是**描述**（第二个参数），不是用例名——ASCII 词 "avatar" 匹配不到中文描述，要拿描述里的子串（如「头像圆圈」）过滤。
- 匿名命名空间函数在变异时若失去全部引用，-Werror 直接构建失败，跑的还是旧二进制——变异必须保持符号被引用，否则会拿陈旧产物误判。
