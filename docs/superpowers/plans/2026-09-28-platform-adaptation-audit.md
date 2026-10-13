# Windows / macOS 双平台适配覆盖度审计汇总

> 🔴 **本文件的"缺口"结论已全部结案（2026-10-03 批注），不要照着它行动**：
> ① 它说「**仓库无 CI**」「无 `.github/`」「macOS 侧从未构建/测试过」——**早已不成立**：`.github/workflows/ci.yml` 现在按 `push dev` / `PR → dev·master` 跑 **macOS(`xcode-27`) + Windows(`windows-2025`/MSYS2) 双平台**构建 + 测试（含 `ctest -L probe`）。
> ② 它说「macOS 无构建入口（脚本全链 Windows 硬编码）」——**已修**（`577e0d0` 给脚本加 macOS/Linux 分支），且 macOS 从 2026-09-28 起就是日常开发平台。
> ③ 它引用 `docs/KNOWN_LIMITATIONS.md`——该文件已于 `5bc5a0a` 删除。
> ④ 它写「本机**无 Mac**」——当时确实如此，2026-09-28 起项目已搬回 macOS/arm64。
> 现行状态见 `DevDesk/CLAUDE.md` 的「🚦 当前状态」。本文件保留作**历史审计记录**（它记录的符号层 / stub 降级 / 反向污染三类发现当时的处置仍可追溯）。

日期：2026-09-28 ｜ 性质：**只读审计汇总 + 修复回执**（不改码结论，不代替修复单）｜ 范围：`DevDesk/`（+ `../EUI-NEO` 只读对照）

**方法**：三片只读分片（A 平台基础设施与构建 / B 功能层逐模块 / C 脚本·发布·文档），各自独立取证后由主会话合并去重。所有结论均有 file:line 或命令证据；本机**无 Mac**，macOS 侧结论一律为**静态判定**。

## 0. 结论摘要

- **符号层无缺口**：`platform` 层八族（keychain / file_open / file_watcher / system_theme / http_client_ca / paths / time / http）在 macOS 上都能解析到实现，业务代码**零 `#ifdef _WIN32/__APPLE__` 泄漏**（核心约束 5 在预处理层成立）；EUI-NEO 的 `-static`/`-mwindows` 只在 MinGW 分支，不污染 macOS。
- **真正的缺口在两处**：① **macOS 侧自 2026-09-21 迁移后从未构建/测试过**（仓库无 CI，`KNOWN_LIMITATIONS` 自认）；② **macOS 无构建入口**（唯一脚本 `build-test-run.sh` 全链 Windows 硬编码）。
- **能力级降级集中在 EUI-NEO 的四个 stub**（窗口效果 / 窗口样式 / 原生右键菜单 / 电源事件）→ 磨砂、托盘 styled 菜单、桌宠三项在 macOS 静默降级。
- **一处"反向污染"**：为迁就 MinGW 语义写的 `remove+rename` 落在所有平台，破坏了 POSIX 的原子替换（已修，见 §3）。
- 未发现范围夹带；本轮已修 5 条可本机验证项（§3），其余按 §5/§6 分类处置。

## 1. 分类总表

| 分类 | 条目 |
|------|------|
| **完全没适配** | macOS 构建入口（脚本从未入库）；托盘 styled 自绘菜单（EUI 只有 Win32 触发点）；文件监听（macOS 走 stub）；系统深浅主题（macOS 恒浅色，波 1 在补）；macOS 侧 `.icns` 素材（全仓零） |
| **适配不完全** | macOS 侧零验证（无 CI）；窗口效果/标题栏联动（stub 降级）；桌宠「不进任务栏 / 原生右键降级 / 睡眠零渲染」三项 Windows-only；`openDirectory` 走 `std::system`；macOS CA 策略 no-op；`Info.plist` 版本无单一源；`build-app.sh` 不拷 `assets/` 且皮肤根按 cwd |
| **适配代码可能有问题** | `atomic_write` 破坏 POSIX 原子性（已修）；`test_settings` 隔离在 macOS 失效、会写真实日志（已修）；`test_keychain` 会写真实 login keychain（已修）；`keychain_macos` 混用 deprecated `SecKeychain*` + `SecItemCopyMatching`（未修，需实机）；`CompilerWarnings` 判据漏 `AppleClang`（已修）；窗口外框经验值取自 Windows 实测（未修，需 Mac 定标） |
| **无法判定** | macOS 能否 configure/build/测试通过；`GLFW_TRANSPARENT_FRAMEBUFFER` 在 macOS 是否生效；`SecKeychain*` 在目标 macOS 的授权行为；`.app` 与文件关联链路；`background@2x.png` 是否被 Finder 采用；DMG 全部命令 |

## 2. 逐条 findings（合并去重，按严重度）

**阻断**

| # | 结论 | 证据 | macOS 上会怎样 |
|---|------|------|----------------|
| B1 | macOS 侧自迁移后从未构建/测试 | `docs/KNOWN_LIMITATIONS.md`（自认 T2–T11 只在 Windows 编译测试）、仓库无 `.github/` | 可用性纯声明；任何编译级问题都不会暴露 |
| B2 | macOS 无构建入口 | `scripts/` 仅 5 个 Windows 侧脚本；`build-test-run.sh` 含 `taskkill`/`C:/msys64`/`.exe`；`CLAUDE.md` 提到的 `dev.sh`/`test.sh`/`screenshot.sh`/`reset-db.sh` **在磁盘与 git 全历史都不存在**（文档幻觉，无法从 git 恢复） | 只能照文档手敲 cmake；无测试/截屏入口 |
| B3 | 托盘 styled 自绘菜单在 macOS 永不触发 | EUI-NEO `tray.h` 只在 Win32 消息分支调 `eui_tray_menu_requested`；`tray_bridge.h` 自注"当前仅 Windows" | 右键只剩原生两项菜单；**左键直唤主窗失效** |

**重要**

| # | 结论 | 证据 | macOS 上会怎样 |
|---|------|------|----------------|
| I1 | 系统深浅主题恒浅色 | `system_theme_posix.cpp` 恒 `return false`（CMake 把 `APPLE` 也映到它） | 「跟随系统」永远解析成浅色（**另一会话波 1 在补，勿重复做**） |
| I2 | 文件监听无 macOS 实现 | `file_watcher_stub.cpp` `active()==false`；消费方退回 5s mtime 轮询 | 同秒改回等长内容漏检 |
| I3 | 窗口效果全链无 macOS 实现 | EUI `window_effect_stub.cpp` 恒 false → 降级 Transparent | 磨砂档视觉降级为半透；设置页文案仍写"需 Windows 11 22H2+"（对 Mac 用户措辞不准） |
| I4 | 标题栏深浅联动无效 | 非 Windows 平台 nativeWindowHandle 为 nullptr（代码已注明） | 标题栏跟随系统外观而非应用主题（已声明差异） |
| I5 | 桌宠三项 Windows-only | 不进任务栏（`window_style_win.cpp`）、原生右键降级不可达（`pet_window.cpp` 早退）、电源事件 stub | 窗口正常显示，仅上述三项无效（均有注释声明） |
| I6 | `atomic_write` 破坏 POSIX 原子性 | 为 MinGW `rename` 不覆盖，所有平台先 remove 再 rename | 崩溃窗口；**已修**（§3-2） |
| I7 | `test_settings` 隔离在 macOS 失效 | 只重定向 `APPDATA`，而 macOS 路径读 `HOME` | 跑测试会截断**真实** `~/Library/.../logs/devdesk.log`；**已修**（§3-3） |
| I8 | `test_keychain` 写真实钥匙串 | 无平台分隔 | 可能弹授权框/污染用户钥匙串；**已修**（§3-4） |
| I9 | `CompilerWarnings` 判据漏 `AppleClang` | 只匹配 `Clang`/`GNU`/`MSVC` | macOS 上 `-Wall -Wextra -Wpedantic -Werror` **全部静默失效**；**已修**（§3-1） |
| I10 | `keychain_macos` 混用两套 API + 先删后加非原子 | 同文件内 `SecKeychain*`（deprecated）与 `SecItemCopyMatching` 并存 | 未修：需实机判断授权框与失败语义 |
| I11 | 窗口外框经验值取自 Windows | `core/window_layout.*` 的 14.4/37.6 来自 Win11 125% | 可能把本可放下的窗口误判最大化；需 Mac 定标 |
| I12 | macOS 需手工 OpenSSL 参数 | `find_package(OpenSSL REQUIRED)` 无 APPLE 分支；spec 里 `brew install` 名单缺 openssl | 手敲 configure 易卡在找不到 OpenSSL |

**提示**

| # | 结论 | 证据 |
|---|------|------|
| N1 | `file_open.h` 契约被破坏 | 平台无关头里声明 win-only `wideToUtf8ForTest`；**已修**（§3-5） |
| N2 | `openDirectory` 在 macOS 走 `std::system("/usr/bin/open …")` | 阻塞调用线程、语义只有退出码 |
| N3 | `check-consistency.sh` 纯 macOS 却留在 Windows 侧 | 当前开发平台上 100% 不可用（**归波 1 的 L10**） |
| N4 | `probe_winfx.ps1` 输出路径指向已删 worktree | 硬编码 `D:\project\gui\wt-winfx\build\...` |
| N5 | `prep-eui-neo-pr.sh` 锁定已完成的旧 PR | 会 `git add` 另一个仓库 |
| N6 | 托盘图标回落通用图标 | macOS 侧未设 `trayIconPath` → 用 `NSImageNameApplicationIcon` |
| N7 | 文档腐烂（多处） | `CLAUDE.md` 的 macOS 脚本行、根 `CLAUDE.md` 的 `wt-*/` 约定（实测无该目录）、README 的 EUI-NEO rev 与 `CMakeLists.txt` 不一致、`check-consistency` 结论段过期 |

## 3. 本轮已修（修复批 P1，5 个原子 commit，已合并 master）

| commit | 内容 | 验证 |
|--------|------|------|
| `a4d55b9` | `cmake/CompilerWarnings.cmake` 判据补 `AppleClang`（同 Clang 分支） | 项目内探针：AppleClang/Clang/GNU 选项集一致；`build.ninja` 仍带 `-Werror`（114 处） |
| `34e4087` | 原子写按平台拆文件：`atomic_write_win.cpp`（逐字保留 remove+rename）/ `atomic_write_posix.cpp`（单次 rename，真原子） | `--filter atomic` 1/1、`--filter THS-03` 7/7 |
| `3819a11` | `test_settings.cpp` 的 `AppDataEnv` 非 Windows 侧一并隔离 `HOME` | `--filter settings` 67/67（在 `build/tests` 内跑） |
| `8a20054` | `test_keychain.cpp` 对写/枚举真实钥匙串的两例在 macOS 整例跳过 | `--filter keychain` 4/4 |
| `e1c702d` | `wideToUtf8ForTest` 声明移出平台无关头（新增 `file_open_win.h`） | `--filter file_open` 10/10 |

**附加实测（有价值）**：MSYS2 MinGW64 g++ 16.2 的 `std::filesystem::rename` **已能覆盖**已存在目标 —— 旧注释"MinGW rename 不覆盖"在当前工具链上已过时；因"Windows 行为一字不变"仍保留原写法，实测已记入代码注释待后续单独收敛。

**本轮已登记但未修（超出本批范围）**：`markdown_service.cpp` 另有一份同款 `remove+rename` 私有拷贝，POSIX 上有同样非原子窗口。

**已知预存在失败（非本轮回归）**：测试用例「模块隔离守卫」在 `CWD=仓库根` 时失败（`findRepoRoot()` 相对候选返回空），换 `build/tests` 目录跑即过；引入 commit 早于本批基线。

## 4. 避让清单（另一会话"波 1 修复"在飞，本批一律未碰）

`src/platform/system_theme_posix.cpp`（波 1 的 L2 在补 `system_theme_macos.cpp`）、`scripts/check-consistency.sh`（L10 Windows 化）、`docs/KNOWN_LIMITATIONS.md` 与 `CLAUDE.md`（L11 文档同步）、`docs/superpowers/plans/2026-09-27-wave1-fix-design.md`。**对账时机**：波 1 全部合入后，用本报告 §2 逐条比对是否已被覆盖，避免重复修。

## 5. 待拍板（产品决策，不擅自做）

1. **是否恢复 macOS 开发链路**：重写 `scripts/dev-macos.sh`（构建/测试/截屏/清库）并进 CI（或至少给出可复现的手敲流程）；代价：无 Mac 机可验证。
2. **托盘能力**：macOS 是否要补 styled 自绘菜单 + 左键直唤（需改 EUI-NEO 的 APPKIT 分支，属上游工作）。
3. **桌宠三项 Windows-only**（不进任务栏/原生右键降级/电源事件）是否补 macOS 实现。
4. **文件监听**：是否给 macOS 实现（FSEvents/kqueue）以消除轮询漏检窗口。
5. **磨砂**：macOS 是否接受"半透"为终态，或改文案口径（把"需 Windows 11 22H2+"改为按平台分述）。
6. **`keychain_macos` 现代化**（改用 `SecItem*` 全套 + 原子写 + 备份策略）——需实机验证授权行为。
7. **窗口外框经验值**：是否在 Mac 上实测重标（消除最大化误判）。
8. **文档订正批**：`CLAUDE.md` 的 macOS 脚本行、根 `CLAUDE.md` 的 `wt-*/` 约定、README 的 EUI-NEO rev、`check-consistency` 结论段、spec 的 submodule/vcpkg 时代上手步骤（多份，建议一次集中订正）。

## 6. 不可验证项（本机无 Mac + 无 CI）

macOS 能否 configure/build/测试通过（需实机跑一次并观察 OpenSSL/FTS5/AppleClang 告警面/测试全绿）；`GLFW_TRANSPARENT_FRAMEBUFFER` 实际生效性；`SecKeychain*` 与未签名 App 的授权行为；`.app`/文件关联/`lsregister` 链路；DMG 全部命令与 `background@2x.png` 采用情况；窗口外框偏差量。
