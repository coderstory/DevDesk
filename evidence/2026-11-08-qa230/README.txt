qa230 阶段档全量（HEAD=aea3945d，2026-11-08 00:35-00:50）
doctest: 2268/2255/5（红归属见 clue:qa230-fullrun-red-attribution）
守卫链: 探针跳过登记/模块隔离/Windows交叉427/427/玻璃/ToolGuard 绿；std include 红=3处 tests/probes 缺头
probe: 45 注册 / 38 绿 / 5 skip（4 wallpaper=Windows-only, memory_install=裸调用77）/ 2 红（bug142=已知clue92；uat62=clue174 亮页误报族，直拍 proof-settings-tab0 全渲染）
守卫自检: 4/5 OK；std-include 自检合成树A-F全对，好树步因真实3违规退出（守卫未坏）
启动: PID 活 + splash 实测 1.203s（#196）+ 流质泵挂起线（#218）
