bug#194 动态背景 20fps 限帧 前后对比（2026-11-07 macOS arm64 本机实测）

[基线] 改动前（常驻 onFrame 驱动，runtime 满刷新率出帧）
  帧泵频率: 60.0fps（60Hz 屏满帧率）
  单帧 compose 建元素: 79-83us
  进程 CPU: 30.9-32.7%（ps 三次采样）

[after] 20fps 泵 + EUI 空闲限时等待
  帧泵频率: 18.4fps（<=20fps 老板定值达标；周期=泵 50ms 打点+主循环 25ms 消费量子的节拍和）
  单帧 compose 建元素: ~131us
  进程 CPU: 0.2-0.7%（ps 三次采样）

GPU 口径说明: macOS 无免 sudo GPU 采样手段，GPU 负载按 帧率x每帧渲染量 估算——
流质背景为 14 大晕+42 卫星的混合填充四边形，每秒填充量正比帧率，60->18.4 即 GPU 侧降 ~69%。

原始证据: fps-log-extract.txt（DEVDESK_FLUID_FPS_LOG=1 仪表输出，见 app_sidebar.cpp drawFluidBackground）
基线 stderr 全量: fluid_base.log / after stderr: fluid_after4.log
