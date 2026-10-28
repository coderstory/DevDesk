# DevDesk 壁纸模块 · 技术方案（as-built）

> 日期：2026-10-28 · **as-built（按实现反推，原始总文档已灭失）**
> 面向后续 AI 会话的权威底稿：架构分层、关键机制契约（含 file:line 锚点）、决策记录。行号锚点基于分支 `feat/wallpaper-3b-final` @ `a161a206`，后续改动若使锚点漂移，以注释原文检索为准。
> 姊妹篇（面向用户的需求口径）：`docs/superpowers/specs/2026-10-28-wallpaper-requirements.md`。原始总文档 `WALLPAPER-MODULE-DEV.md` 已灭失；P1 spec（`2026-10-26-wallpaper-module-design.md`）对 P1 契约仍有效。

## 1. 架构分层

```
┌────────────────────────────────────────────────────────────┐
│ app 装配层  src/main.cpp（:251 模块实例 / :265 moduleForPage │
│             / :305 托盘桥注入）+ src/wallpaper_tray_bridge   │
├────────────────────────────────────────────────────────────┤
│ 模块层      src/module_wallpaper/wallpaper_module.{h,cpp}   │
│  (UI)       wallpaper_ui.{h,cpp}（播放/片库两页，id 契约）   │
├────────────────────────────────────────────────────────────┤
│ 服务装配    wallpaper_service.{h,cpp}（线程模型/锁顺序/      │
│             watcher 接线/扫描/轮换驱动/持久化）              │
├──────────────────────────────┬─────────────────────────────┤
│ 纯逻辑 core/                 │ 平台 platform/               │
│  model / rotation / arbiter  │  wallpaper_host(+_win/_stub) │
│  availability / folder_policy│  mpv_controller(+_stub)      │
│  image_library / video_library  mpv_player / player_backend│
│  settings / presentation     │  yield_watchers              │
│  （时间由调用方传入，不持锁    │  image_probe(+_win)          │
│   不启线程）                  │  （Win 实现 + stub 兜底）     │
├──────────────────────────────┴─────────────────────────────┤
│ 探针/测试   tests/probes/wallpaper_*.cpp（真机五探针）       │
│             tests/test_modules/test_wallpaper_*.cpp（单测）  │
└────────────────────────────────────────────────────────────┘
```

各层职责与红线：

- **core/ 纯逻辑**：零平台 include、零兄弟模块依赖（不 include 任何其它模块）、不持锁不启线程；时间一律 `RotationClock::time_point` 由调用方注入（`rotation.h:17,26`），可虚拟时钟测轮换。文案单点在 `presentation.cpp`，UI 不拼字符串。
- **platform/ 平台层**：一切 `#ifdef _WIN32` / Win32 API 只许出现在这里；业务代码只调 `platform::wallpaper::supported()`。每个平台实现都有同名 stub（非 Windows 由 CMake 选 stub），保证全平台构建通过。
- **service 装配层**：唯一拥有线程与锁的层。锁顺序固定 `mu_ → playerMu_`，`playerMu_` 内不得回调任何加 `mu_` 的方法（`wallpaper_service.h:11`）。测试缝全部要求在 `start()` 之前注入（`wallpaper_service.h:111-128`）。
- **模块/UI 层**：门控（`create_wallpaper_module()` 工厂、`PageId::Wallpaper` 注册表行序 = 侧栏序）+ 节点 id 契约（`wallpaper_ui.cpp:5-36`，probe/单测按 id 定位，不得改名）。
- **探针**：单测不碰真桌面（注入播放器 = 不建宿主不 spawn mpv 不启 watcher，`wallpaper_service.h:16`）；真桌面行为全部由五探针实证。

## 2. 关键机制

### 2.1 WorkerW 宿主挂载（`platform/wallpaper_host_win.cpp`）

- 唯一宿主线程拥有全部窗口（Win32 线程亲和）；show/hide/rebuild 从任意线程投递命令同步等待（有界超时 3s，启动 5s，`:45-46`）。
- WorkerW 查找优先级（`:136-148`）：向 Progman 发 `0x052C` 消息（`:44`，令 Explorer 生成图标层后的 WorkerW）→ ① Progman 直属 WorkerW（Win11 路径）→ ② EnumWindows 找「SHELLDLL_DefView 所在顶层窗的同级 WorkerW」（经典 Win10 路径）→ ③ 兜底直接父到 Progman（仍在图标层后）。
- 窗口栈：隐藏顶层消息窗（非 message-only，为收 TaskbarCreated 等广播）→ 宿主窗（自绘黑底防闪烁）→ 视频/图片子窗。宿主 `WS_EX_NOACTIVATE | WS_EX_TRANSPARENT` + `HWND_BOTTOM`（`:11,307,328`）：不抢焦点、鼠标可点穿到图标。
- 尺寸 = 主屏 `rcMonitor` 物理像素；不自行设 DPI 感知（进程已 PerMonitorV2）。

### 2.2 mpv 进程模型与 IPC 死锁防线（`platform/mpv_controller.*`）

进程模型（`mpv_controller.h:4-12`）：`CreateProcess` 子进程 + 命名管道 JSON IPC（管道名 `\\.\pipe\devdesk-wallpaper-mpv-<pid>-<counter>`）+ Job Object kill-on-close（父进程死则 mpv 必死）。唯一控制线程拥有进程/管道/Job 句柄；命令队列投递不阻塞（stop 除外）；`time-pos` 由读线程写原子值，任意线程可读。路径解析三优先级（`mpv_controller.h:40-69`）：`DEVDESK_MPV_PATH` → `<exeDir>/assets/mpv/mpv.exe` → 开发兜底（仅非 Release）。argv 契约（`mpv_controller.h:72-96`）：`--no-config --idle=yes --wid=<n> --input-ipc-server=<pipe> --loop-file=inf --image-display-duration=inf --panscan=1.0 --pause=yes …`。

**IPC 死锁机理与防线契约（改动前必读，`mpv_controller.cpp:4-12`）**：

- 机理：mpv 对每条 IPC 命令回一行 JSON 回执、事件持续推送。若我方只写不读，mpv 出站缓冲积压 → 其 IPC 线程阻塞 → 反压通道 → 我方写命令永久阻塞。另：管道若按同步句柄打开，一个悬挂读会在同一文件对象上把并发写一起串行卡死（读写互卡）。
- 防线（三件套，缺一即死锁回归网必红）：
  1. **OVERLAPPED 是根**：管道按 `FILE_FLAG_OVERLAPPED` 打开，读写两端都走 OVERLAPPED + 事件等待（`mpv_controller.cpp:383-385`）。
  2. **写有界**：OVERLAPPED 写 + ≤1s 有界事件等待（`:171-174`）；写超时 = 判通道死亡 → 就地 `TerminateProcess` + 撤销并排空挂起 IRP，控制线程有界返回（`:197-199`），之后对断管道快速失败。
  3. **读常驻排空**：读线程常驻消费回执与事件（含 time-pos → 原子毫秒，`:218-220`）；收尾先 Terminate 再 `CancelIoEx` + join，保证有界（`:220`）。
- stop 梯子总时延 < 3s（`:263-265`）：quit 有界写（≤300ms）→ 800ms 宽限（`:46`）→ `TerminateProcess` → 500ms 等待（`:47`）→ 撤 IRP + join。
- 测试注入缝（`:573`，`mpv_controller.h:256-259`）：`MpvControllerTestAccess::attachIpcForTest` 注入测试自建管道客户端 + 伪进程句柄，模拟「spawn 成功但对端拒绝读」，驱动 stop 梯子走满有界路径——生产代码禁调。

史实：首版曾用同步句柄读写 → 真机死锁，commit `eec88dbb` 改 OVERLAPPED 有界写 + 常驻读线程修复，`test_wallpaper_platform.cpp` 的死锁回归网由此而来。

### 2.3 轮换 tickLoop 与虚拟时钟

- 轮换内核 `core/rotation.h`：纯逻辑，「下一条播哪条、多久换一条」的唯一真相源；时间由调用方传 `now`（`:26,41-56`）；洗牌袋保证随机模式一轮内不重复；暂停冻结剩余时间、按冻结值续排；`refreshItems` 不打断正在播放的条目。
- 驱动：`WallpaperService::tickLoop` 250ms 一拍（`wallpaper_service.cpp:569-572`），只在 `mu_` 内推进轮换并发出装载命令；懒唤醒（condition_variable wait_for）。
- 虚拟时钟：`setClockForTest`（`wallpaper_service.h:122`）注入时钟函数，单测可瞬间推进数小时验证轮换；扫描/轮换线程的代次（`scanGeneration_`，`wallpaper_service.cpp:499-516`）防旧扫描覆盖新目录。

### 2.4 让路 arbiter 与四路 watcher 注入缝

- 仲裁 `core/arbiter.h`：系统原因集合（`Fullscreen/ScreenLocked/DisplayAsleep/SystemSleeping/Battery` 五位，`core/model.h:35-37`）+ 手动暂停 → 单一 `shouldPlay`。契约（`arbiter.h:4-11`）：快照原因只含系统位；锚点只在「未暂停→暂停」那刻写；解禁产生一次 resume seek（断点续播）；set 幂等；电池门控关闭时移除已存在电池位。线程约束：不持锁不启线程，只在 service `mu_` 内调用。
- watcher 接口 `platform/yield_watchers.h`：Windows 实现 = 隐藏消息窗收 `WM_WTSSESSION_CHANGE`（锁屏/解锁）+ `WM_DISPLAYCHANGE`（换屏）+ 轮询 `GetSystemPowerStatus`（电池）与 `EnumWindows`（全屏）；其它平台 stub（start 失败给中文原因、永不回调）。
- 接线（`wallpaper_service.h:13-16,137-143`）：事件回调任意线程可入，喂 arbiter、状态真变才落决策；换屏广播 → `handleDisplayChanged` 重建 WorkerW 宿主并重装当前项；tickLoop 内轮询兜底——全屏每拍（250ms）、电池每 4 拍 ≈1s（`wallpaper_service.cpp:421`，分频计数 `yieldPollCounter_`）。
- 注入缝：`setYieldWatchersForTest`（`wallpaper_service.h:126`）注入假 watcher 驱动裁决矩阵（`22a99daa` 的四路矩阵单测）；显式注入假 watcher 时测试装配下也要接线启动（`testWatcherInjected_`，`:174`）。生产装配用 `createYieldWatchers()`。
- 真机实证：`wallpaper_yield_probe`——全屏测试窗出现 → 快照 paused 且 systemHolds==Fullscreen，且经 IPC 读 mpv `pause` 属性实测为 true（暂停下达到播放器，非仅服务层状态）。

### 2.5 图片 2K 档过滤

- 探针 `platform/image_probe_win.cpp:34-70`：WIC `CreateDecoderFromFilename` + 首帧 `GetSize`——只读元数据头不解码像素；**任何失败一律 known=false（尺寸未知不过滤，宁可放行）**；COM 套间按调用进出配对，遇 STA 冲突按失败处理不硬闯（`:39-41`）。
- 过滤语义 `core/image_library.h:9-10,46-53`：`total` = 扩展名命中数（过滤前）；`passing` = ≥ 档位线 + 尺寸未知者；`filtered = total - passing`；probe 为空 = 不过滤（passing==total，与 P3b 前行为一致）。
- 档位线 `core/settings.h:30`：`imageMinPixels` 默认 3686400（2K=2560×1440；同式 1080P=2073600 / 4K=8294400）。
- UI 契约：片库页两格 `.stat.passing`（参与轮播，accent）/ `.stat.filtered`（低于档位）为**真实计数**（`wallpaper_ui.cpp:24-26`）；早先「过滤未实现、文案不得宣称」口径已随 `283fed9f` 反转。测试：假探针过滤矩阵单测（`22a99daa`）+ 真机 `wallpaper_rotation_probe`。

### 2.6 托盘桥缝（`src/wallpaper_tray_bridge.h`）

- 形态同 `module_toggle_bridge`：main.cpp 不可见 `WallpaperService` 类型，经函数指针双槽缝中立化（`:22-29`）。app 装配段注入真实现（main.cpp:305，捕获-less 直引模块静态实例）；测试注入 recording 桩。
- 命令语义（`:9-13`）：TogglePause 只切手动位（系统让路不因此解除）；Next → service.next；Stop 幂等（恢复 = 设置页重拨模块开关，不新增反降级链路）。
- absent 语义（`:15-20,28`）：`present` = 模块实例已装配（平台能力通过）；实例在但服务已 stop 仍为 true——组保留、命令由服务幂等守卫吞掉。未装配 = 查询 `{false,false}`、命令 no-op + WARN。三枚 `static_assert` 锁签名漂移。

### 2.7 打包链路（mpv 随包 + GPL 合规）

- 链路：打包脚本 `scripts/package-windows.sh:135-154` 把 `MPV_BIN`（默认 `D:/software/mpv/mpv.exe`）拷到包目录 `assets/mpv/mpv.exe`；**源缺失即失败不出包**（静默产出无 mpv 的包 = 安装版壁纸必坏，比打不了包严重）；落点复核断言在 `:207-213`。
- 运行时解析与 2.2 三优先级一致；安装版只能走 `assets/mpv` 第二条（开发兜底仅非 Release）。
- 安装器 `installer/windows/devdesk.iss`：`[Files]`（`:59` 起）整目录装 assets（含 mpv；`recursesubdirs`），并分发 `THIRD-PARTY-NOTICES.md`（`:61-64`）——mpv 为 GPLv2+，GPL 分发须随附许可告示（commit `a161a206`）。
- 红线：mpv.exe 本体不进 git（handoff §6）。

## 3. 决策记录表

| 日期 | 决策 | 依据 |
|---|---|---|
| 2026-10-26 | 壁纸 = DevDesk 页面模块，**不做独立应用**；默认关闭；Windows 先行 | P1 spec §1.2（用户拍板的需求基线） |
| 2026-10-26 | EUI-NEO 上游 fork-and-freeze：fork main 是唯一主线，不跟随 upstream；推送政策 fork-only（框架通用能力修在 fork，可选上游 PR 反哺；worktree 隔离线不得对 EUI-NEO 执行 git 操作，需改上游时产 patch 回单由非隔离线应用） | ADR 0006（消费政策）/ ADR 0007（上游策略）；CLAUDE.md 上游贡献政策段 |
| 2026-10（时点快照） | DevDesk 侧与 EUI-NEO fork 同步至 **v0.6.1**（fork 发布记录 `EUI-NEO/docs/releases/v0.6.1.md`）。当前 fork 顶层 CMake `project(VERSION 0.7.0)` 已前移（待核：决策时点确为 v0.6.1，0.7.0 为后继演进）；消费侧不锁版本，唯一权威判据 = `scripts/check-eui-neo-version.sh` 内容级 API 守卫 | EUI-NEO 仓库发布文档与 CMakeLists（只读核对于 2026-10-28） |
| 2026-10-27 | 设置入口「独立全宽卡」方案**作废**，收进模块插拔卡网格第 5 格；卡高按平台二值 254/198；格 id 保留 `settings.card.wallpaper.*` | 用户真机拍板；P1 spec §2.6 决策变更注记；commit `dfb642ad` |
| 2026-10-27 | mpv IPC 同步句柄读写互卡 → **OVERLAPPED 有界写 + 读线程常驻排空**；stop 梯子压进 3s | commit `eec88dbb`；`mpv_controller.cpp:4-12` 契约注释 |
| 2026-10-28 | 图片播放**走 mpv 直播路径**（`--wid` 嵌入 + `--image-display-duration=inf` 常驻），自绘四铺法（Fit/Center/Tile）**deferred**（非永久不做；Fill 经 `--panscan=1.0` 生效，枚举与持久化留位） | commit `62c74e1a`；`mpv_controller.h:72-96` |
| 2026-10-27 起（全程） | mpv.exe **不进 git**；打包期 `MPV_BIN` 源缺失即失败；GPL 告示随包 | handoff §6；commit `6914c3d6` + `a161a206` |

## 4. 测试与探针底稿（后续会话的验证入口）

- 定向跑法：`bash scripts/build-test-run.sh --filter wallpaper`（构建纪律见仓库 CLAUDE.md：worktree 隔离、工具链 PATH 前置、禁主工作树构建）。
- 单测：`test_wallpaper_core.cpp`（纯逻辑全矩阵） / `test_wallpaper_ui.cpp` / `test_wallpaper_module.cpp`（门控注册） / `test_wallpaper_platform.cpp`（IPC 死锁回归网） / `test_wallpaper_tray.cpp`。
- 真机探针（`tests/probes/`）：`wallpaper_desktop_probe`（WorkerW+mpv 生命周期 5 场景） / `wallpaper_rotation_probe`（扫描/轮换/过滤端到端） / `wallpaper_image_rotation_probe`（视频↔图片自动切换） / `wallpaper_yield_probe`（让路四路真机，读 mpv IPC 实测暂停） / `wallpaper_probe`（真窗口 UI + 截图）。
- 收口记录：2026-10-28 全量构建+全量测试收绿（Release 增量，`build-test-run.sh --release --no-run`），判据 Total 全过 0 Failed + 守卫绿；结果见当日回单。

## 5. 灭失文档对照

- 原始总文档 `WALLPAPER-MODULE-DEV.md`（需求全景、§1.3 永久不做、§6.2 mpv 路径解析、§7 UI 值表、§10 里程碑、§11 UAT 场景）**已灭失**。本文与需求书 as-built 为其替代权威底稿。
- 可追溯残片：P1 spec（含 §5 转录的永久不做清单与 §2.6 决策变更注记）、`docs/handoff/2026-10-27-wallpaper-module-handoff.md`（重启前状态）、git 历史全部 wallpaper 提交（`git log --grep=wallpaper -i`，自 `3a8f2df5` P1 骨架至 `a161a206` GPL 合规）。
- 已灭失且无替代的细节：原始 UAT 场景全表（§11.3）、UI HTML 原型稿的验收细则（§7 逐值）。UI 逐值现以 `wallpaper_ui.cpp:5-36` 的 id/几何契约注释与 `test_wallpaper_ui.cpp` 断言为权威（待核：与已灭失原型稿是否逐值一致无法追溯，以现实现为准）。
