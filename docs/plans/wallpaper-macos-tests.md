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

## 已知非回归

- `sidebar_fold_reserves_toggle_zone`（`test_app_sidebar.cpp:337`）在**未改动的干净树**上同样失败，属既有问题（该用例按固定 8 模块/固定行号断言，与本需求无关）。已单独确认：stash 全部改动后重建、该用例仍红。

## 已知限制

- 三种让路信号（锁屏/熄屏/睡眠）无法在无人值守自动化里真触发，靠事件源注册成功 + 启动即重算覆盖；真机 UAT 需人工锁屏/合盖各验一次。
- 全屏让路在终端跑探针时会因终端窗口铺满而恒为「全屏中」——这是正确行为，探针已按 `systemHolds` 派生断言而非硬编码。
