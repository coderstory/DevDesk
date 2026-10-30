# 流体 demo 粘合层上游对照清单（2026-10-29）

对照基准：PavelDoGreat/WebGL-Fluid-Simulation script.js（/tmp/webgl-fluid，depth-1 克隆）。
范围：EUI-NEO examples/fluid_background.cpp 中全部「上游没有、demo 自写」的逻辑。

## 逐项核对

| # | 自写项 | 上游对应 | 结论 |
|---|--------|----------|------|
| 1 | splat_velocity 的 `delta*6000`、`aspect<1 时 delta.x*=aspect`、`radius=0.0025`、`aspect>1 时 radius*=aspect` | SPLAT_FORCE=6000、correctDeltaX、SPLAT_RADIUS/100=0.0025、correctRadius | 一致，非自由发挥 |
| 2 | splat_dye 仅用 color（已删 delta 残留项，本次 BUG 根因） | 上游 dye splat 本就只传 color | 已对齐上游 |
| 3 | 压暗参数：splat ×0.35 / 半径 ×2.4 / display ×0.55 | 上游无（demo 背景化裁决项） | 老板娘拍板的刻意偏离，保留 |
| 4 | color_state 定值 Clay 色 ×0.15 | 上游 generateColor() 动态 HSV ×0.15 | 刻意替换（Clay 色带拍板），衰减系数 0.15 与上游一致 |
| 5 | mouse_state pass（auto 轨道 0.32 幅度 + iMouse 合并 + cur/prev 存 rgba） | 上游 JS 侧 pointermove 算 delta，无此 pass | 自创，但只是指针数据源替换（无真实鼠标时自动演示），不触算法 |
| 6 | pressure_clear=0.8 / 速度耗散 0.2 / 压力 20 次迭代 / curl=30 | PRESSURE=0.8、VELOCITY_DISSIPATION=0.2、PRESSURE_ITERATIONS=20、CURL=30 | 一致 |
| 7 | bloom 链（8 级、threshold 0.6、soft knee 0.7、intensity）与 sunrays（Density 0.3/Decay 0.95/Exposure 0.7/weight 1.0） | BLOOM_*、SUNRAYS_* 默认值 | 一致 |

## 结论

- 算法与参数层：与上游逐值一致，无第二处自由发挥。
- 自创仅两处：mouse_state（数据源）、color_state 定值（拍板项）+ 压暗三参数（拍板项）。
- 规矩落档：后续胶水代码写完必出本表同类「上游对照清单」。
