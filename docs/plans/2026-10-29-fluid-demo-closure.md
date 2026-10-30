# 流体背景 demo 原型收口报告（2026-10-29）

结论：达标收口（老板娘审批：Clay 暖橙光斑柔和游动、纹理回归、白核退化可接受、暖墨底不死黑）。

## 交付物

- demo 源码：EUI-NEO `examples/fluid_background.cpp`（按裁定留本地，不提交 fork，等 app 集成方案）
- 效果帧：`docs/mockups/fluid-bg-v6-{early-200,mid-900,late-1800}.png`
- 截帧命令：`FLUID_DEMO_SCREENSHOT=<帧号> FLUID_DEMO_SCREENSHOT_PATH=<ppm> ./fluid_background`（构建目录 /tmp/eui-fluid-demo，离屏无窗）

## 算法与关键参数（v6 定稿）

- 12+6 pass 全套搬运自 PavelDoGreat/WebGL-Fluid-Simulation（上游对照清单：docs/plans/2026-10-29-fluid-glue-upstream-audit.md，算法与参数层逐值一致）
- 染料注入：`exp(-dot(p,p)/radius) * color * 0.3`（无方向项，上游 dye splat 同构）
- 峰值控制：Reinhard 软压缩 `(1+L/1.44)/(1+L)` 只放 display 展示层，不进模拟累积场（放累积场=等效加耗散，颜料存不住，v5 平棕/近黑两版失败的共同根因）
- 底色：暖墨 #241C15 在 display 加注入；集成时按裁定回调一档（当前偏中性灰观感）
- 演示轨迹：mouse_state auto 轨迹为多频 Lissajous（覆盖疏密不均，避免固定椭圆匀速注入导致整片饱和平台）

## 过程教训（入档）

1. 手写胶水一行，出错概率一行——dye splat 混入方向项即翻车；胶水写完必出上游对照清单。
2. 后处理曲线只能放展示层；放进模拟场会改变物理守恒。
3. 演示用自动指针路径的形状直接决定画面形态（匀速闭合轨道=饱和平台）。

## 遗留

- app 集成方案未立项（待拍板后排产）：流体层挂 EUI-NEO 渲染缝的具体方式、性能预算（笔记本无风扇机）、开关入口。
- EUI-NEO 侧 examples 改动未提交，集成时随正式代码走。
