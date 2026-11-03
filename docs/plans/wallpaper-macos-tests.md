# 壁纸 macOS 平台层 · 测试与证据清单

日期：2026-11-03 · 需求：macOS 壁纸功能完整实现（视频 + 图片）

## 交付内容

| 层 | 文件 | 说明 |
|---|---|---|
| 宿主 | `src/module_wallpaper/platform/wallpaper_host_macos.mm` | CGWindowLevel 桌面层窗口（`kCGDesktopWindowLevelKey`）+ AVPlayerLayer + 自绘图片层；主队列有界同步投递（3s） |
| 宿主类型 | `src/module_wallpaper/platform/wallpaper_host_macos_view.h` | ObjC 类型（仅 .mm 可 include）；图片层四铺法自绘 |
| 播放 | `src/module_wallpaper/platform/mpv_controller_macos.mm` | 同名接口的 AVFoundation 实现：AVQueuePlayer + AVPlayerLooper（视频）/ CALayer（图片）；无 mpv 进程 |
| 让路 | `src/module_wallpaper/platform/yield_watchers_macos.mm` | 锁屏（分布式通知 + 会话字典重读）/ 熄屏（CGDisplayIsAsleep）/ 睡眠（NSWorkspace）/ 电池（IOKit）/ 全屏（CGWindowList 覆盖率）/ 换屏（CGDisplayReconfiguration） |
| 图片探针 | `src/module_wallpaper/platform/image_probe_macos.cpp` | CGImageSource 读 PixelWidth/Height，不建解码器 |
| 纯几何 | `platform/image_fit_geometry.h` / `platform/fullscreen_geometry.h` | 零平台依赖，跨平台单测 |
| 能力 | `src/platform/wallpaper_capability_macos.cpp` | `supported()` → true |

## 证据物

### F1 — 纯逻辑单测（17 条，`tests/test_modules/test_wallpaper_macos_platform.cpp`）
四铺法几何矩阵（Fill/Fit/Center/Tile 各自落位 + 退化输入 + 四模式确不相同）、全屏覆盖率聚合（单窗口/半屏/同 pid 拼接/异 pid 取最大/溢出封顶/空与零尺寸/内缩补偿/坐标翻转）、图片探针 unknown 语义（不存在/假 PNG/空路径）。

实测：`--filter wallpaper` → **Total 67 / Failed 0**（含原有 50 条）。

### F2 — 平台层真机探针（`tests/probes/wallpaper_macos_probe.cpp`）
§1 宿主建窗 2940×1912（1470×956@2x）· §2 视频真实播放（4s 内位置前进）· §3 暂停冻结 + 恢复续播 · §4 seek 落位 · §5 图片 position 恒 0 · §6 四铺法下发无错 · §7 stop 幂等无残留 · §8 让路 watcher 启动 + 双轮询通道。
实测：**PASS**（`--video pic/fixtures/clip-a.mp4`）。

### F3 — 服务端到端真机探针（`tests/probes/wallpaper_service_macos_probe.cpp`）
真 WallpaperService 装配（不注入 player/host/watcher）：真文件夹扫描 → 图片档位过滤 → 混合池（video+image）→ 换片 → 图片来源 → 四铺法切换 → 手动暂停/恢复 → 停止 → 二次启动。
实测：**23/23 PASS**。

### F4 — 进程内渲染实证（像素级）
把壁纸窗口 layer 渲进 bitmap 后统计像素：
- 图片：`nonBlack=20000/20000`、`contentsScale=2.0`、`bounds=1470×956` → 图片真的画满整屏。
- 四铺法像素分布各异：Fill 4050 / Fit 2730 / Center 8364 / Tile 12500（青绿块计数）→ 铺法真的走不同绘制路径，不是只改枚举。
- 视频：`videoLayer.isReadyForDisplay=1` + `pos=0.94`（`renderInContext` 取不到视频帧是 AVFoundation 既定行为，改用 readiness 判据）。

### F4b — 睡眠/唤醒路径实测
真服务装配 + 真宿主 + 真播放器，注入无让路假 watcher 隔离终端全屏误报，走真 watcher 接口回调驱动：

| 阶段 | paused | systemHolds | 倒计时 |
|---|---|---|---|
| 基线 | 0 | 0 | 299 |
| 睡眠中 | **1** | 含 SystemSleeping | 冻结 299 |
| 醒来 | **0** | **0** | 298（继续走） |
| 醒来 2s 后 | 0 | 0 | 296（正常递减） |

即：睡眠→暂停、醒来→恢复、轮换时钟随之继续，三者同真。

### F5 — 图片尺寸探针真机
`/tmp/wall-test-4k.png` → `known=1 2560x1440`；`assets/icon.png` → `known=1 256x256`。

## 期间发现并修复的真实缺陷（均由真机探针暴露，非纸面推理）

| # | 缺陷 | 后果 | 修法 |
|---|---|---|---|
| 1 | watcher `start()` 同步回调重算 | 服务持 `mu_` 调 watcher → 回调取同一把锁 → **自死锁**（栈实证停在 `psynch_mutexwait`） | 重算改为投递到自身串行队列 |
| 2 | `NSTimer` 挂在串行队列上 | 队列线程无 run loop，位置镜像永不刷新 | 去掉镜像，`positionSec()` 直接读 `AVPlayer.currentTime` |
| 3 | async block 捕获 `const std::string&` | 引用悬垂 → 路径变空串 →「无法识别的文件路径」 | `make_shared` 拷贝后进 block |
| 4 | `stringWithUTF8String:` / `fileURLWithPath:` 二次解释 | 含中文的临时目录解析失败 | 统一走 `CFURLCreateFromFileSystemRepresentation` |
| 5 | 宿主 `width/height` 用逻辑尺寸 | 契约要求物理像素 | `convertRectToBacking` |
| 6 | `show()`/`hide()` 同步等主线程 | 服务持 `mu_` 调 show，主线程可能正等 `mu_`（snapshot/setMode）→ **互等 3s 超时「显示壁纸宿主超时」**（醒来路径实测复现） | 两者改 `dispatch_async` 投递即返回；可见性与调用方无需同步 |

## 已知非回归

- `sidebar_fold_reserves_toggle_zone`（`test_app_sidebar.cpp:337`）在**未改动的干净树**上同样失败，属既有问题（该用例按固定 8 模块/固定行号断言，与本需求无关）。已单独确认：stash 全部改动后重建、该用例仍红。

## 与参考实现（pic）的逐项对照

| pic 的做法 | 本实现 | 结论 |
|---|---|---|
| `CGWindowLevelForKey(.desktopWindow)` | `CGWindowLevelForKey(kCGDesktopWindowLevelKey)` | 一致 |
| `collectionBehavior` 四项（跨 Space/静止/不入循环/全屏辅助） | 同上四项 | 一致 |
| `isOpaque/hasShadow/ignoresMouseEvents/backgroundColor` | 同上 | 一致 |
| `canBecomeKey/Main = false` | 同上 | 一致 |
| `videoGravity = resizeAspectFill` | 一致 | 一致 |
| 图片层 `contentsScale = backingScaleFactor` + layout 里跟屏 | 一致 | 一致 |
| 图片层 `needsDisplayOnBoundsChange` | 一致 | 一致 |
| 图片解码 EXIF orientation 重摆正 + `ShouldCacheImmediately` | 一致 | 一致 |
| `audioTimePitchAlgorithm = .spectral` | 一致 | 一致 |
| `preferredForwardBufferDuration = 3.0` | **原先漏掉，已补** | 已对齐 |
| looper 先插后扫（队列全程非空防闪屏） | 一致 | 一致 |
| 速度挂 player 不挂 item | 一致 | 一致 |
| 全屏：layer==0 / alpha>0 / 排除本进程 / 按 pid 聚合 / 除以 visibleFrame / 封顶 1.0 | 一致 | 一致 |
| 全屏：桌面层内缩实测 14/9 补偿 + 坐标翻转 | 一致 | 一致 |
| 锁屏：分布式通知只当触发器 + 重读会话字典 `CGSSessionScreenIsLocked` | 一致 | 一致 |
| 熄屏：每次现读 `CGDisplayIsAsleep` | 一致 | 一致 |
| 睡眠：只由 willSleep/didWake 位驱动 | 一致 | 一致 |
| 电池：IOKit 事件源 + 读失败不静默折值 | 一致 | 一致 |
| 换屏：`CGDisplayRegisterReconfigurationCallback` | 一致 | 一致 |
| 全屏**事件**源：Space 变更 + 应用激活/失活三通知 | **已接**（2026-11-03 补齐） | 一致 |

**差异 3 已消除**：全屏检测现为「3 个 workspace 事件 + 服务 250ms 轮询」双驱动，与 pic 同形状（轮询更密）。事件只负责即时重判，判定函数与轮询共用同一条路径，结论必然一致；重复推送由 `HoldArbiter::setSystemReason` 的幂等守卫吸收，不会重复触发暂停/恢复。

## 平台能力差异（按实际，不强行统一）

**视频容器**：实测 AVFoundation 对 mkv / webm / avi 一律 `Cannot Open`（`AVURLAsset.isPlayable == NO`），
与 Windows mpv 路线的扩展名白名单（mp4/mov/m4v）实际可播范围相同——因此 `core/video_library.cpp` 的
共用白名单不改，UI 的「MKV/AVI/WEBM 不会被扫描」文案两平台都成立（已在 `wallpaper_ui.cpp` 注明实测依据）。

**全屏判定口径（有意保留差异，不统一）**：
- Windows：窗口矩形**精确等于**显示器矩形才算全屏（`yield_watchers_win.cpp:121-125`），最大化不算。
- macOS：**覆盖率 ≥ 1.0** 即算（`fullscreen_geometry.h:117`），最大化也算。
两者是各自平台生态的既有取舍（macOS 侧沿用 pic 已验证口径），业务影响一致（盖满屏就让路），不做互相迁就。

## 已知限制

- 三种让路信号（锁屏/熄屏/睡眠）无法在无人值守自动化里真触发，靠事件源注册成功 + 启动即重算覆盖；真机 UAT 需人工锁屏/合盖各验一次。
- 全屏让路在终端跑探针时会因终端窗口铺满而恒为「全屏中」——这是正确行为，探针已按 `systemHolds` 派生断言而非硬编码。
