# Profile 头像圆圈三处改 + 首字 UTF-8 修复 — 实施报告（2026-10-01，审计修订版）

## 用户需求（原话）

「profile 列表里的 item 有个白色的圆圈，里面有个问号，且没有垂直居中。现在帮我改：这个圆圈改成随机颜色的圆圈，且有外层包边。」

## 改了什么 / 为什么

落点：`src/module_claude_proxy/claude_ui_list.cpp`（drawProfileCard + 匿名命名空间常量区）

1. **白底 → 随机色（按 profile 稳定派生）**
   - 为什么不是真随机：compose 每帧都跑，每帧真随机会闪烁。控制器裁定「随机 = 稳定派生」。
   - 实现：FNV-1a 64 对 `p.id`（int64，持久化主键，改名不变）逐字节哈希 → 对 9 色板取下标（`avatarHash` / `kAvatarPalette`）。同一 profile 永远同色，不同 profile 大概率不同色。
   - 色板来源集中一处：`kAvatarPalette[9]` —— 9 个低饱和暖色系基色（clay/陶土/赭石/沙棕/灰绿/桉叶/雾蓝/藕紫/豆沙），与主题 Claude Clay #D97757 同族协调。深浅主题经 `eui::mixColor`（header-only 值类型桥接）自由基色派生：浅色主题 = 基色调白成粉彩底 + 加深一档描边/字形；深色主题 = 基色调暗成深底 + 调亮一档描边/字形。
   - 字形**颜色**从 `textPrimary` 改为派生 ring 色（圆内对比度保证）；**内容口径**见下条。

2. **外层包边**
   - 实现：`.border(1.5f, avatar.ring)`，ring 色 = 基色加深（浅主题）/ 提亮（深主题）一档（控制器许可的「同色系加深一档」方案，选它而非中性 borderDefault 的理由：与随机底色成套、视觉是「同色环」而非孤立灰线）。
   - 旧代码本有 1px borderDefault 但用户看不见（视为「没有包边」）→ 改 1.5px 同色环。

3. **垂直居中**
   - 根因：旧 `y + 16` 使圆心落在 y+36，而 96 高卡片中心是 y+48（偏上 12px）。
   - 修：`avatarY = y + (h - avatarSize) * 0.5f`（从 h 推导，行高变了也居中）；字形框同 y。

4. **[审计 Important] 头像首字按 UTF-8 码点取（首轮报告归因失实的更正）**
   - **更正**：首轮报告把「?」归因为「空 keychain 显示占位字形」——**错误**。真根因是既有 bug：`std::string(1, p.name[0])` 只取名字首**字节**，CJK 首字符是 3 字节 UTF-8，截出单字节非法序列渲染成 ?（用户原话「里面有个问号」正是此观感）；英文名 profile 显示正常，故一直未暴露。截图中「智谱 GLM 网关」行的 ? 就是这个 bug，与 keychain 无关。
   - 修：`firstUtf8CharOr()` —— 首字节判序列长度（0xxxxxxx=1 / 110xxxxx=2 / 1110xxxx=3 / 11110xxx=4），取完整码点；续字节须 10xxxxxx；非法 / 截断序列回退 ?（空名占位同字形）。「智谱网关」→ 显示「智」。

## TDD 证据

### 首轮（圆圈三处改）
- **RED**：`claude_profile_avatar_color_ring_centered` 对未改代码跑：`!isPlaceholder — 头像圆圈仍是占位底色 surfaceElevated`。
- **GREEN**：实现后定向全绿。
- **变异验证**（框架 fail-fast，RED 时 (b)(c)(d) 未跑到，逐条补做）：
  - 变异① border 宽 0 → (c) 捕获 `av->border.width >= 1.0f`
  - 变异② 取消居中（回 y+16）→ (d) 捕获 `圆心 246 vs 行中心 258`（恰 12px）
  - 变异③ 色板下标恒 0 → (b) 捕获 `anyDistinct`

### 审计批（首字 UTF-8 + 环色补强 + 深色分支）
- **RED**：新增 `claude_profile_avatar_initial_utf8`（智谱网关 / Alpha 双样本）：`txCjk->text == "智" — 渲染 text 长度 1（应为 3）`。（首跑还暴露测试自身缺陷：列表按 name ASC 排，「Alpha」排在「智谱网关」前 → 写死下标取错行；改经 `svc->listProfiles()` 同序映射名字→行号。）
- **GREEN**：`firstUtf8CharOr` 实装后定向全绿。
- **变异④**（首字取法回退旧实现）→ UTF-8 用例红：`渲染 text 长度 1`。
- **环色断言补强**（审计 Minor）：(c) 段补 `环色 != theme.borderDefault`；**变异⑤** ring 色回退 borderDefault → 该断言红。
- **深色主题派生**（审计 Minor）：(e) 段 setLight/setDark 各 compose 一帧：深浅 fill/ring 必须互异（isDark 分支生效）、各自非占位色、环各自非中性 borderDefault；结束按 `currentThemeName()` 还原全局主题。注意点：`g_current` 是单例被原地改写，token 快照必须**值拷贝**（引用会跨主题别名）。

## 真机验证（隔离数据根）

- `DEVDESK_DATA_ROOT=/tmp/avcheck_dataroot` 启动 worktree 构建产物（未碰 /Applications/DevDesk.app；只杀自己启动的 PID）。
- 钥匙串安全：空数据根启动曾清过用户真实 key——本分支 base 已含护栏（2c2afb3：空 keep 跳过 + claude: 白名单 + service 按数据根派生 69e634b），启动前核实了 `keychainServiceForDataRoot` 派生逻辑，测试根不触真实钥匙串。
- sqlite3 CLI 直插 3 个 profile，cliclick 进 Claude 页。**浅色主题截图**：`.superpowers/avatar-fix-zoom.png`（列表特写：桉叶绿/赭石/雾蓝粉彩底 + 同色环 + 行内垂直居中）、`avatar-fix-shot2.png`（整页）。注：该批截图摄于首字修复前，「智谱」行字形仍是 ?（既有 bug，本批已修）；其余三处改动的观感不受影响。
- **深色主题截图：未完成**——拍摄中途机器自动锁屏（需用户触控 ID/密码，agent 无法代解），screencapture 只能拍到黑帧。替代证据：(e) 段无头断言把深浅两主题的派生值全部数值钉住（isDark 分支 + 非占位 + 非中性环）；几何值与浅色截图一致（同一 compose 代码路径，仅颜色不同）。解锁后可按同流程（DEVDESK_DATA_ROOT + cliclick + screencapture）补拍。
- OpenGL 旧帧坑：动了鼠标（重绘触发）后拍第二张取后一张。

## 测试数字

- 定向：头像两用例（color_ring_centered + initial_utf8）2/2 绿；变异 ①-⑤ 各被对应断言捕获。
- 全量（审计批后）：**Total: 898 / Passed: 892 / Failed: 0**（+1 = 新增 utf8 用例）；probe：`ctest -L probe` **8/8**。
- base 落后 dev tip 81 提交（worktree 切出后 dev 前进），协调者基线 948/942 + probe 9/9 是 dev tip 数字；本树零失败即无回归。

## 过程教训（留档）

- 本测试框架的 filter 匹配的是**描述**（REGISTER_TEST 第二参数），不是用例名——ASCII 词匹配不到中文描述。
- 匿名命名空间函数在变异时若失去全部引用，-Werror 构建失败 → 跑的是陈旧二进制（假红/假绿），变异必须保持符号被引用。
- DOCTEST_* 断言宏不能进非 bool lambda（宏展开含 `return false`）——helper 里只取值，断言留给用例体。
- 主题 token 全局单例被 setDark/setLight 原地改写，跨主题断言要值拷贝快照，不能存引用。
