# 壁纸模块开发交接（2026-10-27 重启前）

> 单一功能分支：`feat/wallpaper-module`（当前 HEAD `4f1ae2c5`）
> 基线：`dev` 的 `25e31b1d` 起（已含本会话 P1/P2/P4 的 merge 与并行会话的其它提交）
> 本文件是重启后的第一入口；详细任务书与审计报告见文末路径。

## 0. 一句话状态

P1 骨架/默认关闭开关/设置卡/文档口径已完成并合入 dev；Batch1 纯逻辑+服务已构建验证（20/20）并合入 dev；**P3a（WorkerW+mpv 真桌面）与 P5（播放/片库 UI）只完成了代码与提交，尚未编译验证**。重启后第一件事是在隔离 worktree 里编译这一分支，修编译错，再跑两个探针与截图。

## 1. 分支与提交

- `feat/wallpaper-module`（HEAD `4f1ae2c5`）：
  - `dac4359c` merge P3a 分支（WIP `f5f3bd3e`，16 文件 +2043/−11）
  - `4f1ae2c5` merge P5 UI 分支（WIP `a752dd42`，8 文件 +2459/−33；`tests/CMakeLists.txt` 冲突已按“两边都保留”解决）
- 之前已合入 `dev` 的壁纸提交：P1 骨架 `3a8f2df5`+`7c7f1360`、设置卡 `dbad69b8`、文档 `fbc3fd93`+`e76e8646`、probe `7e0304a4`+DPI 修复 `d92741fe`、Batch1 `92fad292`+`fb271de4`。
- 并行会话仍在动 `dev`（重启前 HEAD 已到 `25e31b1d`）；**重启后先 `git log --oneline -3 dev` 刷新，不要假设分支点**。

## 2. 已验证（有实测证据）

1. **P1 门控/设置卡/文档**：定向测试 wallpaper 10/10、settings 72/72、gating 10/10；独立审计 PASS。
2. **P1 探针截图 DPI 修复**：实跑 `contentScale=1.35417`、framebuffer `1625×1065`、逻辑 `1200×786`、退出 0；两张图右缘有 45–51px 背景留白（D1 闭环）。截图在 `D:/Data/desktop/wallpaper-p1-empty.png`、`wallpaper-p1-settings.png`（重启后可能仍在）。
3. **Batch1 纯逻辑+服务**：构建成功；`--filter wallpaper` 20/20；模块隔离/交叉编译/守卫自检通过；独立审计 FAIL→修复→复审计 PASS（剩余 R-1～R-5 Minor 记录在 `batch-1-audit.md`）。
4. **HTML 设计稿对照图**：`D:/Data/desktop/wallpaper-ui/reference-1600x1000.png`、`reference-1000x640.png`（Edge headless 渲染官方 mockup）。

## 3. 未验证（必须先做）

- **P3a WorkerW+mpv**：代码未编译、未跑 `wallpaper_desktop_probe`。新增 `src/module_wallpaper/platform/{wallpaper_host*,mpv_controller*,mpv_player*}`、`tests/probes/wallpaper_desktop_probe.cpp`、`tests/test_modules/test_wallpaper_platform.cpp`；`wallpaper_service.*` 有装配改动。测试视频：`D:/Data/Downloads/dynamic-wallpaper-master/fixtures/clip-a.mp4` 与 `clip-b.mp4`。
- **P5 UI**：代码未编译。新增 `wallpaper_ui.h/.cpp`、`tests/test_modules/test_wallpaper_ui.cpp`，改了 `wallpaper_module.cpp` 与 `tests/probes/wallpaper_probe.cpp`。用户硬要求：UI 一点都不能差（§7.1 间距/字号/圆角逐值、§7.3 空态三变体、§7.5 折叠整组、1000×640 无滚动条）。
- 尚未做：Batch 3b（图片层 WIC/四铺法 + 锁屏/熄屏/睡眠/电池/全屏/换屏）、托盘菜单、mpv 打包/GPL、macOS 第二阶段。

## 4. 重启后执行顺序（严格按批，一次编译）

1. 读本文件 + `WALLPAPER-MODULE-DEV.md` §7/§9 + `DevDesk/CLAUDE.md`。
2. 刷新基线与并行会话改动：`git fetch origin`、`git log --oneline -5 dev`、`git status`。
3. 在**隔离 worktree** 里从 `feat/wallpaper-module` 编译（主工作树禁跑构建）：
   `git worktree add D:/wt-wp-verify -b verify/wallpaper-module feat/wallpaper-module`
   `bash scripts/build-test-run.sh --filter wallpaper --no-run`
   解释并修所有编译错误（预计在 P3a/UI 装配与 CMake）。
4. 跑三个东西并把输出存档：
   - `devdesk_tests.exe wallpaper`（必须 0 Failed）
   - `wallpaper_probe --screenshot ... --screenshot-seeded ...`（UI 空页/设置页，验证无裁切）
   - `cmake --build build --target wallpaper_desktop_probe` + 运行（真桌面 crème 视频、position 增长、pause 冻结、stop 无 mpv 残留）
5. P3a/UI 各派独立审计（实现者不得审自己）；按 findings 修→限定复审计→再合 `dev`。
6. 之后再做 Batch 3b（`batch-3b-brief.md`）；最后跑一次全量构建+全量测试（用户要求）。

## 5. 任务书 / 审计 / 证据路径

- 交接总文档：`D:/project/gui/WALLPAPER-MODULE-DEV.md`
- 最小 spec：`DevDesk/docs/superpowers/specs/2026-10-26-wallpaper-module-design.md`
- 任务书（`.superpowers/sdd/WALLPAPER-MODULE-DEV/`，git-ignored）：
  `batch-1-brief.md`、`batch-2-brief.md`、`batch-3a-brief.md`、`batch-3b-brief.md`
- 审计：同目录 `task-0-audit.md`、`task-1-audit.md`、`task-2-audit.md`、`task-3-audit.md`、`task-4-audit.md`、`batch-1-audit.md`
- 台账：同目录 `progress.md`（含 rulings、deferred minors）
- 设计对照：`D:/project/dynamic-wallpaper-win/docs/mockups/devdesk-wallpaper-ui.html`

## 6. 关键决策与禁区

- 壁纸默认关闭；非 Windows 工厂返回 nullptr、能力 false；macOS 本阶段不实现平台层。
- 不恢复其它会话删除的 claude 功能（CC 导入/调用日志/额度/玻璃卡/旧主题等）；壁纸模块不得依赖它们。
- 最小改动/扩展优先：新能力走新文件/新接口，既有 service 只在装配点追加调用。
- 图片像素档过滤尚未实现（`imagePassing==imageTotal`、`imageFiltered==0`），UI 文案不得宣称已过滤。
- mpv.exe 不进 git；开发兜底 `D:/software/mpv/mpv.exe`，正式包走 assets/mpv。
- 机器慢：一批一次编译、只跑定向测试；全量只在最终收口。

## 7. 建议技能（重启后的新会话调用）

- `superpowers:subagent-driven-development`（按批派实现/审计）
- `superpowers:verification-before-completion`（编译/探针证据后再宣布完成）
- `x-coding-tools:cs-retro`（收口后沉淀经验）