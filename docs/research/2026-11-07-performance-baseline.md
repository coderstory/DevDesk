# DevDesk 性能与内存使用基线分析（pool need #193）

日期：2026-11-07 ｜ 测量者：developer（ZCode 子代理）｜ 只测不改，全部结论以本次实测为准。

## 1. 测量环境与口径

- 机器：macOS（arm64，darwin 27.0.0），本机 Debug 产物 `build/DevDesk`（2026-11-07 16:38 构建，commit `0e634533`，测量期间无源码改动，产物未失效）。
- 数据根：`~/DevDesk`（devdesk.db 352KB + WAL 4KB）。
- 启动方式：`DEVDESK_FLUID_FPS_LOG=1 ./build/DevDesk`（fps 仪表 = bug#194 加的，日志行锚点 `src/app_sidebar.cpp:1255`）。
- 时间对齐口径：进程拉起时刻 t0 由 `python3 datetime.now()` 记录（毫秒），里程碑时间取 `~/DevDesk/logs/devdesk.log` 行首毫秒时间戳，两者同为本地钟。
- **锁屏影响声明**：16:49 起显示器睡眠/锁屏（displaysleep=5min，测量者无法解锁）。因此：
  - 「页面切换交互帧率」维度**未验证**（无人工交互手段）；以 fluid-fps 自动仪表 + sample 抓栈替代观测。
  - 运行时段分「锁屏前可见态」（16:48:01-16:49:30 左右）与「锁屏态」（16:49:30 之后）。可见态 CPU 参照上次会话口径（clue 128/133：18.4fps、CPU 0.2-0.7%），本次补测锁屏态并发现显著退化（见 §5.2）。
- CI ASan/UBSan 慢化 2-10× 为 CI 特性，不混入本基线；全部数据为 macOS 本机 Debug 形态。

## 2. 启动性能分段

两次启动实测（日志里程碑 `main.cpp:213`、`splash.cpp:81/87`、`app_sidebar.cpp:1314/529`）：

| 里程碑（锚点） | run1 冷启动 16:45（背景 OFF） | run2 温启动 16:48（背景 ON） |
|---|---|---|
| t0 → DB 就绪（main.cpp:213） | 0.858s | 0.096s |
| 15 个模块 onStart 完成（router.cpp:111 系列） | t0+0.87~0.96s | t0+0.10~0.17s |
| splash 开始（splash.cpp:81） | t0+1.010s | t0+0.202s |
| splash 结束（splash.cpp:87） | t0+2.954s（持续 1.944s） | t0+2.143s（持续 1.941s） |
| **首帧**（app_sidebar.cpp:1314） | **t0+2.955s** | **t0+2.146s** |
| 侧边栏挂载（app_sidebar.cpp:529） | t0+2.956s | t0+2.147s |

关键结论：
1. **splash 固定动画占首帧 66%（冷）/ 90%（温）**。所有模块 onStart 在 splash 开始显示**之前**已完成（日志序：模块启动 → splash 开始）——splash 期间无任何加载工作，纯常量等待：`kDurationSec=1.68f + kFadeOutSec=0.252f`（`src/ui_kit/splash.cpp:43-44`）。
2. run1 与 run2 的差值（DB 就绪 0.858 vs 0.096s）= 动态链接器/页缓存冷热差。真冷启动（开机后首启）应取 run1 口径 ~2.96s；日常重启取 run2 口径 ~2.15s。
3. 附带发现：splash 日志文案「2.184s 墨染扩散」（splash.cpp:81）与实际生效时长不符——墨染素材时长 kInkDurationSec=2.184s 被 kDurationSec=1.68s 截断，墨染动画从未播完。

## 3. 运行时 CPU 与常驻内存

### 3.1 背景 OFF（run1，无动画基线）
- CPU：**0.0%**（8 点 × 10s 采样，ps pcpu）。
- RSS：**122.2MB 恒定**（8 点波动 <0.05MB）。
- `sample <pid> 5`（5s 全线程抓栈）：**所有线程停在等待态**（wait/sleep 深度同一采样数 2820/2820），无 sqlite、无轮询自旋热栈。

### 3.2 背景 ON（run2）
- 可见态（16:48:01-16:49:30，参照上会话 clue 128 口径）：18.4fps、CPU 0.2-0.7%。
- 锁屏态：CPU **4.2-16.3%**（趋势 55 点，均值约 10%），详见 §5.2——比可见态口径高一个数量级。

### 3.3 内存趋势（run2，30s 采样两段拼接：段1 16:49:34-16:59:04 共 20 点 + 段2 补采，段1 采样进程曾中断）

| 时段 | RSS (MB) | 备注 |
|---|---|---|
| 16:49:34 | 158.8 | 起点（锁屏窗口内） |
| 16:50:34 | 127.1 | -31.7MB |
| 16:52:34 | 120.1 | 持续缓降 |
| 16:53:04 | 113.6 | 降速收敛 |
| 16:53:04 之后 | **112.9-114.1 稳态** | 段1 尾部 |

结论（口径：段1 连续 9.5 分钟 20 点 + 补段见 §9.3；**30 分钟连续趋势未达成**——app 进程在 16:59:24 后消失，无崩溃栈、无退出日志、RSS 113MB 排除 OOM，原因未明，已作为测量事件记录）：**段内无泄漏/持续上涨迹象**。锁屏窗口内 RSS -45MB（158.8→113.6，推测为窗口 occlusion 后纹理/字体缓存释放与 malloc 归还，方向性判断未逐页验证），此后稳态 ~113MB。完整 30 分钟连续曲线留待可见态复测补齐。

### 3.4 内存关注点核查
- **纹理 atlas**：`kGrayAtlasSize=2048`（单通道）+ `kColorAtlasSize=1024`（RGBA），`EUI-NEO/core/render/text.cpp:32-33` → gray 2048²×1B=4MB + color 1024²×4B=4MB ≈ **8MB**，固定一次性，可忽略。
- **字体栈缓存**：16 槽按（字号×weight）变体缓存 FontStackCacheEntry，`EUI-NEO/core/render/text_shaping.cpp:918`（2026-11-06 修复过「16 槽打满→整屏空白」回退路径）。字号拖动是峰值驱动场景，常驻占比小（RSS 稳态 113MB 内含）。
- sharedTextAtlas 引用计数管理（text.cpp:54-82），无重复建页。

## 4. I/O：SQLite 查询与文件轮询频率

### 4.1 SQLite
- **schedule 引擎 1Hz 查库**：`intervalSec=1` 订阅（`src/module_schedule/schedule_engine.cpp:114`），tick 每秒 `store_.listRules()` 全表读 + 逐条 evaluate（schedule_engine.cpp:167-177）。空闲无到期时也查——1 query/s 是常驻下限。
- **ticker 泵与帧循环绑定**：ticker 由主循环每帧 pump（`src/core/ticker.h:53`）。全仓订阅者仅 2 个：schedule_engine(1Hz)、release_module（低频提醒，release_module.cpp:95）。**主循环不出帧时（动态背景 OFF 且无交互）ticker 完全停摆**——run1 sample 全线程等待佐证。含义：背景 OFF 时 schedule 的 1Hz tick 不推进，到期任务延迟到下一帧（正确性边界，非性能问题，本次只记录）。
- 库体量 352KB / WAL 4KB：单次 listRules 成本微，1qps 不构成 I/O 热点。
- agent Scheduler：常驻 tick 轮询线程 + worker 线程（`src/module_agent/scheduler.cpp:161/186/212` sleep_for+条件等待混合），空闲等待态（sample 佐证），无任务时不产生周期 I/O。

### 4.2 文件轮询（watcher 家族核对，结论以实测为准）
- **macOS FileWatcher = FSEvents 事件驱动，不轮询**（`src/platform/file_watcher_macos.cpp`；启动日志 `FileWatcher: 已启动监听（FSEvents）notes`）。轮询兜底 stub 仅在无原生 watcher 平台生效（file_watcher_stub.cpp）——macOS 本机不触发。
- wallpaper 服务线程：100ms/250ms **条件变量等待**（谓词驱动+兜底，`src/module_wallpaper/wallpaper_service.cpp:580/663`），非忙轮询；播放后端 Null 时无额外工作。
- Settings 日志 tail：compose 收割模式（`src/module_settings/settings_module.cpp:1807-1810`），事件驱动。
- 结论：**watcher/轮询家族在 macOS 本机空闲态无忙轮询热点**（run1 CPU 0.0% + sample 全等待 + FSEvents 事件驱动三重佐证）。历史 known issue 与本机现状不矛盾——问题域在无原生 watcher 的平台。

## 5. 渲染帧率

### 5.1 泵机制与实测分布
- 帧泵：守护线程 50ms 节拍 `requestFrame()` = 20fps 老板定值（`src/app_sidebar.cpp:80-87`，kFluidPumpPeriodMs=50）。
- 本次 run2 全程 fluid-fps 逐 5s 点（n≈82）分钟均值：

| 分钟 | 16:48 | 16:49 | 16:50 | 16:51 | 16:52 | 16:53 | 16:54 | 16:55 |
|---|---|---|---|---|---|---|---|---|
| fps 均值 | 11.3(启动期) | 6.3 | 16.7 | 5.0 | 16.7 | 18.6 | 18.6 | 18.7 |

- 帧构建耗时 avg_build **215-277us**（远小于 50ms 节拍）——构建不是瓶颈。
- 实测稳态 18.6fps vs 泵定值 20fps：**缺口 -7%**（50ms sleep 相位损耗 + 事件等待漂移；可见态口径 17.2-18.6，上会话 18.4）。
- **两态抖动现象**：存在 ~18.6fps 稳态与 ~5fps 低速态的分钟级交替（16:49、16:51 为低速态），切换与显示状态（睡眠/锁屏）时间相关但无逐秒显示状态记录，对应关系未严格确立。

### 5.2 锁屏/遮挡态每帧 CPU 成本暴涨（本次最重要发现）
对照：

| 态 | fps | CPU | 每帧 CPU 成本（CPU/fps） |
|---|---|---|---|
| 可见态（clue 128 上会话口径） | 18.4 | 0.2-0.7% | ~0.38ms/帧 |
| 锁屏态（本次实测） | 18.6 | 10-16% | **~8ms/帧（≈20×）** |

- 现象：锁屏后 fps 不降（仍按泵节拍出帧）但每帧 CPU 成本涨约 20 倍。
- 根因链（本次闭合）：泵门控「开关开 && 窗可见」已存在（`src/app_sidebar.cpp:285`，drawFluidBackground 每帧写入），但「窗可见」判定基于 isMiniaturized + GLFW_VISIBLE/Cocoa isVisible（`src/platform/window_visibility_macos.cpp:3`）——**锁屏时窗口未最小化，Cocoa isVisible 仍为 true，门控判定失真**，泵不停 → 18.6fps 持续出帧 → 每帧 present 在遮挡态成本暴涨。EUI-NEO glfw_app_main.cpp/Metal present 无 occlusion 感知（grep 无该关键字）。
- 对用户的影响：挂后台/锁屏场景下动态背景以 18.6fps 持续烧 ~1.5 核当量 CPU——后台功耗热点。

### 5.3 页面切换/交互帧率
**未验证**（锁屏无交互手段）。替代证据：帧构建耗时 250us 量级说明 compose 层成本低；交互期 fps 稳定性留待可见态复测（复测命令见 §8）。

## 6. Top 3 热点（按收益/成本排序）

### 热点 1：锁屏/遮挡态动态背景帧循环 CPU 暴涨（收益最大）
- 现象：锁屏态 18.6fps 稳定出帧，CPU 10-16%（≈1.5 核当量）；可见态口径 0.2-0.7%。
- 数据：§5.2 表 + 趋势 §3.3（CPU 4.2-16.3% 全程）。
- 根因锚点：可见性判定失真 `src/platform/window_visibility_macos.cpp:3`（锁屏时 Cocoa isVisible 仍 true）+ 泵门控写入点 `src/app_sidebar.cpp:285` + 泵本体 `src/app_sidebar.cpp:80-87`。
- 收益估计：后台/锁屏场景 CPU -90%+（笔记本挂后台高频）；可见态不受影响。
- 修复成本估计：中低——window_visibility 的 macOS 实现接入 `NSWindowOcclusionState.visible`（锁屏/全遮挡时为 false，Cocoa 原生 KVO，一处判定改动，Windows 侧对称核对 IsIconic/occlusion 语义），门控管线无需动。

### 热点 2：splash 常量 1.93s 占温启动首帧 90%（成本最低）
- 现象：温启动 t0→首帧 2.15s，其中 splash 固定动画 1.941s；模块加载在 splash 前已完成，splash 无任何等待意义。
- 数据：§2 表。
- 根因锚点：`src/ui_kit/splash.cpp:43-44`（kDurationSec=1.68 + kFadeOutSec=0.252 纯常量）。
- 收益估计：首帧时间 -1.9s（-89% 温口径 / -65% 冷口径）。
- 修复成本估计：低——缩短常量或加「再次启动跳过」逻辑；属产品决策（WeChat-minimal 克制风是设计拍板）。附带项：日志文案 2.184s 墨染与实际 1.68s 不符（墨染被截断），修正文案或对齐常量。

### 热点 3：fluid fps 缺口与两态抖动（收益小，暂缓）
- 现象：稳态 18.6fps vs 泵定 20fps（-7%）；偶发 ~5fps 低速态分钟级交替。
- 数据：§5.1 分钟均值表。
- 根因锚点：`src/app_sidebar.cpp:80-87`（50ms sleep 相位损耗：requestFrame→等待→出帧的相位不对齐）+ 低速态疑似显示状态相关（未严格确立）。
- 收益估计：视觉平滑度 +7%，低速态消除；CPU 侧收益为负（fps 升会涨 CPU，见热点 1——先修热点 1 再谈提频）。
- 修复成本估计：中（相位对齐改造泵/主循环握手）。

## 7. 优化建议（收益/成本排序）

1. **【做】遮挡态停泵/降泵**（热点 1）：改 `src/platform/window_visibility_macos.cpp` 接入 NSWindowOcclusionState（锁屏/全遮挡判不可见），泵门控管线（app_sidebar.cpp:285）自动生效；Windows 侧对称核对。预期后台功耗数量级下降。
2. **【做，产品拍板】splash 缩短/跳过**（热点 2）：数据已证 splash 期间无加载工作，1.93s 纯等待；建议「同日内二次启动跳过墨染」或常量减半。
3. **【缓】泵相位对齐提 fps**（热点 3）：先做 1 再评估（帧率与功耗此消彼长）。
4. **【不做】内存优化**：稳态 113MB、无上涨、atlas 8MB 可忽略——本轮无内存优化项。字体栈 16 槽峰值仅在字号拖动场景，已有回退修复。
5. **【记录】背景 OFF 时调度停摆**：主循环无帧 → ticker（含 schedule 1Hz）不推进，到期任务延迟到下一帧；非性能项，建议后续条目核实「背景 OFF 长期挂机时提醒是否延迟」。

## 8. 复测命令

```sh
# 冷/温启动分段（t0 记录 → 日志里程碑对齐）
python3 -c 'import datetime;print(datetime.datetime.now())'
DEVDESK_FLUID_FPS_LOG=1 /Users/coderstory/dev/DevDesk/build/DevDesk &
tail -100 ~/DevDesk/logs/devdesk.log   # 取 splash.cpp:81/87、app_sidebar.cpp:1314 行时间戳

# 空闲 CPU/RSS 多点
PID=$(pgrep -x DevDesk); for i in $(seq 1 8); do sleep 10; ps -p $PID -o rss=,pcpu=; done

# 线程活动抓栈（等待态/热栈判定）
sample $PID 5 -file /tmp/sample.txt

# 内存趋势
for i in $(seq 1 55); do sleep 30; ps -p $PID -o rss=,pcpu= | awk -v ts="$(date +%H:%M:%S)" '{print ts,$1/1024,$2}' >> /tmp/dd-trend.csv; done

# fluid fps 分布
grep "fluid-fps" ~/DevDesk/logs/devdesk.log | grep -o "rate=[0-9.]*fps"
```

## 9. 原始数据

### 9.1 run1（16:45，背景 OFF）空闲采样
```
sample1-8（10s 间隔）：rss 122.25→122.219MB，cpu 0.0%，etime 00:59→02:09
sample 5s 抓栈：全部线程 2820/2820 采样停在等待态；无 sqlite3/轮询热栈
线程清单要点：主线程、NSEventThread、coremedia×3、agent Scheduler×3、wallpaper player×1、DispatchQueue 若干
```

### 9.2 run2 fluid fps（本次全部 82 点分钟聚合）
见 §5.1 表；逐点样例（稳态段）：16:54:02→18.62、16:54:07→18.58、16:54:12→18.50、16:54:57→18.53（连续 18.5±0.1）。

### 9.3 内存趋势完整 CSV（30s 采样；段1 /tmp/dd-trend.csv，补段 /tmp/dd-trend3.csv）
```
段1（run2，16:49:34-16:59:04，20 点；app 进程 16:59:24 后消失，原因未明，此后点缺失）：
ts,rss_mb,cpu_pct
16:49:34,158.797,4.5
16:50:04,144.578,15.3
16:50:34,127.125,16.3
16:51:04,127.453,7.4
16:51:34,128.047,4.2
16:52:04,128.047,7.3
16:52:34,120.062,14.5
16:53:04,113.594,7.4
16:53:34,113.594,7.0
16:54:04,114.141,8.7
16:54:34,113.656,9.3
16:55:04,113.562,11.3
16:55:34,113.047,15.1
16:56:04,113.266,15.2
16:56:34,112.922,9.1
16:57:04,113.188,13.2
16:57:34,113.344,12.3
16:58:04,113.344,15.6
16:58:34,113.344,17.8
16:59:04,113.656,13.6
```
补段尝试（run3，17:19 重启新进程）：仅采到 1 点（17:19:21 RSS 210.6MB 启动期）后，app 于 17:19:42 被外部 SIGTERM 优雅退出（console 有完整「停止模块」流程日志 router.cpp:121）——run2 于 16:59:24 的消失同样疑似外部回收（agent 会话启动的 nohup GUI 进程生命周期不可控）。**补段不可用，连续 30 分钟曲线未达成**；「无持续上涨」判据在段1 9.5 分钟窗口内成立（158.8→113.6 收敛后 6 分钟恒稳 112.9-114.1），30 分钟连续曲线留待正常桌面会话复测（复测命令 §8）。

## 10. 未验证项
- 页面切换/交互帧率稳定性（锁屏，无交互手段）——可见态复测留给下轮。
- 锁屏前可见态 CPU 本次实测（沿用 clue 128 口径）；遮挡态 CPU 暴涨的 present 路径帧级根因（门控失真链已闭合，present 层成本机制待框架侧取证）。
- 低速态（~5fps）与显示状态切换的严格对应关系。
- 背景 OFF 长期挂机时 schedule 提醒延迟的实际影响面。
- **连续 30 分钟内存曲线**（app 进程两次被外部回收：run2 16:59:24 无日志消失、run3 17:19:42 SIGTERM 优雅退出；正常桌面会话复测补齐）。
- run2 进程消失的确切原因（无崩溃栈无退出日志；排除 OOM：RSS 113MB）。
