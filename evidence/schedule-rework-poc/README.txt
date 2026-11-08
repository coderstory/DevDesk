定时任务重做（池 need #173）POC 证据目录
========================================

POC-1 桌面顶层置顶窗不抢焦点 —— 结论：可行（macOS 实测，Windows 源码级）
  实测（本目录 spike_run.log / frontmost_poll.log，2026-11-08 两轮一致）：
  - spike 独立进程开置顶闹钟窗（alwaysOnTop+focusOnShow(false)+hideFromTaskbar(true)）
  - 前台进程轮询 52 采样 ×2 轮：全程 ZCode（运行 spike 的宿主终端），spike 从未成为前台
  - 窗口 onWindowFocusChanged 回调两轮全程零触发（未点击窗口零焦点事件）
  - 窗口存在 20s（293.974→313.950），期间正常 compose 渲染
  Windows 侧源码级依据（捆绑 GLFW fork）：_glfwShowWindowWin32 默认 SW_SHOWNA
  （win32_window.c:1802-1804）；可见创建必抢焦点（win32_window.c:1544-1550）→
  产品闹钟/置顶小窗必须走隐藏创建路径（hideFromTaskbar=true），且不可 modal
  （modal 子窗 glfwFocusWindow 抢焦点，glfw_app_main.cpp:511-513）。

POC-2 循环晃动动画（±8°/0.8s 全周期 InOutSine 档）—— 结论：可行（机制级，无 fork）
  实测：onTimer 相位翻转精确驱动（compose frame 1→31 对应 30 次翻转 / 11.86s，
  与 0.4s 半周期吻合）；机制源码链：
  - 相位翻转重触发：core/dsl.h:807 onTimer + core/animation.h:274 AnimatedValue::setTarget
    （start_=current_：重触发永远从当前值起插值 → 相位边界 C0 连续无跳变）
  - 帧泵维持：core/runtime/runtime_update.h:843 isRectAnimating → animating_ → 帧
    循环不熄火（clue 128/133 路径）
  - 适配项：Ease 枚举无 InOutSine（animation.h:11-19）→ 用 InOutQuad/InOutCubic
    等价档，不为一条缓动曲线 fork EUI
  - rotate 单位=弧度（core/render/primitive.cpp:145），±8°=±0.1396
  遗留验证（F-03 实现阶段）：逐帧平滑度需带显示器会话复核——本会话显示器休眠且
  无法唤醒（caffeinate -u 无效），screencapture 全黑（poc_burst_*.png 已清理，仅留
  此说明）。复核=跑 tests/spike/schedule-rework/run_poc_macos.sh 连拍段。

Spike 落点（独立、不接产品装配）：tests/spike/schedule-rework/
  - spike_alarm_window.cpp   双 POC 合一 spike（EUI dsl app，全局 namespace app）
  - run_poc_macos.sh         运行脚本：前台轮询 + 连拍 + 日志归档
  - 构建命令见 build.sh；产物 /tmp/spike_alarm_window（临时）

文件清单：
  spike_run.log        spike stderr 全量日志（第 2 轮）
  frontmost_poll.log   前台进程 0.5s 轮询（第 2 轮，52 行全 ZCode）
