# 现成美观流体效果 C++/GLSL 轮子调研（2026-10-29）

任务来源：开发群老板娘派单——「现成、美观、可直接落进 OpenGL/C++ 桌面应用的流体背景轮子」，接入目标 = EUI-NEO shadertoy 缝。

## 结论（一句话）

**没有「拿来即用」的 C++ 成品库；最优路线是直接搬 PavelDoGreat/WebGL-Fluid-Simulation 的 GLSL shader 文件进 shadertoyGraph**——其算法本体就是十余个 GLSL fragment shader + FBO ping-pong，与 shadertoy 缝的 Buffer pass 机制一一对应，C++ 侧零新增依赖。

## 候选清单（全部本轮实查，2026-10-29）

| 仓库 | 星数 | 许可 | 观感 | 接入难度 | 截图/效果 |
|---|---|---|---|---|---|
| [PavelDoGreat/WebGL-Fluid-Simulation](https://github.com/PavelDoGreat/WebGL-Fluid-Simulation) | 16,769 | MIT | 彩色流动颜料（点拖出墨，缓慢交融） | 低——shader 文件逐个搬进 shadertoyGraph 的 Buffer pass，宿主逻辑由缝承担 | README 顶部演示动图 |
| [bassicali/inkbox](https://github.com/bassicali/inkbox) | 59 | MIT | 墨水/染料，GPU Gems「Fast Fluid Dynamics Simulation on the GPU」的现代 OpenGL 实现（2D/3D/WebGL 三版） | 中——C++ 宿主与 demo UI 深耦合，只拆 shader；观感偏仪表化（速度场/压力场可视化），美化需换配色输出 | README `images/screen1.png`、`2dvid.gif` |
| [kamindustries/touchFluid](https://github.com/kamindustries/touchFluid) | 238 | MIT | TouchDesigner + GLSL 流体 | 仅参考（宿主为 TouchDesigner，不可移植） | README 动图 |
| tunabrain/gpu-fluid（上轮已核，MAC 网格+FLIP 烟雾风） | 503 | zlib | 烟雾 | 已封存，不重复调研 | — |

## 排除项

- FluidX3D（5.3k★）/ fluid-engine-dev（2.1k★）/ SPlisHSPlasH / SPHinXsys：CFD 重型引擎（LBM/SPH/3D），与「桌面背景观感」不匹配，依赖量大。
- Shadertoy 站单文件高赞流体 shader：本轮未能两源核实具体条目（站外检索通道结果不可用），**不列**；不凭记忆报 shadertoy 链接。

## 与母蟑螂 HTML 美化档的关系

母蟑螂的 `fluid-polish-compare-demo.html`（CSS gooey 滤镜、Clay 暖色带、24-60s 呼吸漂移）是 **CPU 合成零 GPU** 路线，与 shader 路线是两档成本。若老板看完 HTML demo 嫌观感不够，shader 档（搬 PavelDoGreat）是下一投入档位——本报告即其可行性凭证。

## 证据等级

已确认：上述星数/许可/算法族（GitHub API + 源码/README 本轮直读）。
待验证：PavelDoGreat shader 与 shadertoyGraph 330 版本行的逐文件兼容（属码农 demo 原型步骤的范畴，非本调研）。

## 追加（2026-10-29 晚，老板指路：dsh 自家插件）

老板娘让改查 dsh checkout 的流体实装。已定位（本机源码直查）：

- dsh web GUI 的背景实装 = 插件 **`dsh-plugin-wallpaper-engine` v1.4.0**（MIT，本机 `~/.dsh/profiles/tauri/node_modules/dsh-plugin-wallpaper-engine`），描述原文：Wallpaper Engine backgrounds for the DSH web GUI，经捆绑的 **WebWallGL** 引擎实时渲染 Scene/Web 壁纸于聊天层之后，带 liquid-glass、blur/scrim/border/opacity 可调。
- 渲染核心 = **[webwallgl](https://github.com/oneincase/webwallgl) v3.0.0**（MIT，WebGL2 播放 Wallpaper Engine 壁纸：scene.pkg/网页/视频三态，含粒子/SceneScript/音频可视化）。
- 许可：全链 MIT（自家生态），可抄。
- 对比一句话：**dsh 版（webwallgl + WE 流体壁纸生态）观感上限最高、素材现成最多，但它是 WebGL2/JS 管线**——接进 DevDesk 的 C++/EUI-NEO 窗口得嵌 webview，与 shadertoy 缝不是一条路；适用于「背景走 web 壳」的方案。若背景层留在 EUI-NEO 自绘缝内，仍以 PavelDoGreat shader 搬运为准；WE scene.pkg 内含的 shader 素材可拆出来反哺。
- 附注：dsh checkout（`.../dependencies/dsh`）只是打包运行时（client.cjs bundle，无 UI 源码），真源在本机 profile `~/.dsh/profiles/tauri/node_modules/`。
