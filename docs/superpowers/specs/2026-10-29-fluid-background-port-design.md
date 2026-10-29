# 流体颜料背景移植规格卡（deepseek-style-theme → DevDesk）

- 日期：2026-10-29
- 作者：UI/UX 设计师（性感母蟑螂）
- 状态：待幕僚长审 → 码农对缝
- 来源仓库：`Lichtspur/deepseek-style-theme`（MIT；算法原样移植，保留其 THIRD-PARTY-NOTICES 声明义务——我方 redistribution 需附同等声明）
- 效果参照：源仓页面本身就是 HTML/JS 可直接跑（老板定：不另做 demo，看效果开源仓原页面+换色参数）。

## 1. 目标

全窗口底层流动颜料背景：WebGL2 两遍流体求解器（ping-pong 速度场 advect/decay + 鼠标 splat），显示遍做 domain-warp 噪声 + 多色交融。色系换 Claude Clay 暖墨体系。信息优先：内容面板压在背景之上（半透明暖墨底 + blur），对比度不降。

## 2. 算法拆解（移植什么，从哪拿）

子系统在源仓 `lib/client.js` 内自包含，无 React 依赖，我方已抽取验证可独立加载：`docs/mockups/deepseek-fluid-subsystem.js`（= SITE_FLUID_PARAMS + VERTEX/FLOW/DISPLAY 三段 GLSL ES 300 + attachFluidShader + hexToRgb，算法零改动）。

- **FLOW pass**：1/4 分辨率 ping-pong 速度场，`decay=0.96` 衰减；鼠标刷子 `exp(-d²/r²)` 注入，速度 bonus 封顶 0.7。
- **DISPLAY pass**（全屏）：两层相位相反 value noise 域扭曲（distortion=20 / swirl=12 / swirlIterations=8）+ flowmap 采样，`blend_multi` 三色 smoothstep 分带混色。
- **交互馈送（可选 P1）**：attachFluidInteractions——指针掠过按钮轻搅、点击出涟漪。桌面 app 可映射到 ui 事件；默认关。

### 取舍范围（2026-10-29 老板放宽「不必 100% 复刻」，按难度/性能/一致性三把尺裁）

- **必取**：流体颜料背景本体（FLOW+DISPLAY 两 pass + 三色带混色 + 静态降级链）。达标线=贴近 DeepSeek 官网流动颜料观感，不逐像素复刻。
- **不取**：玻璃色散折射（startGlassDispersion，SVG backdrop-filter 桌面 GL 无对应缝，性能与一致性双差）、镜面视差（startSpecularParallax，纯装饰）、液态玻璃侧栏整套（EUI-NEO 现有卡片体系已够，换套破坏全 app 一致性）。
- **缓存备选（P2，不进首版）**：attachFluidInteractions 桌面版。鼠标搅动是彩蛋级体验，无风扇机多点交互成本实测后再定。

## 3. 参数与色系映射

基线 = SITE_FLUID_PARAMS 原值；仅改三色与手感：

| 参数 | 值 | 说明 |
|---|---|---|
| color1 | `#D97757` | Claude Clay 主橙（原 `#8AA3D6`） |
| color2 | `#E8B49A` | 暖橘淡（码农配色案，色带过渡更柔） |
| color3 | `#F5EDE4` | 暖纸白 |
| decay | 0.96 | 落码回原值（老板娘批注 2），手感微调等真机反馈 |
| speed | 14 | 落码回原值（老板娘批注 2） |
| 其余 | 原值 | mouseRadius 0.22 / strength 1.1 / distortBoost 1.35 / swirlBoost 0.45 / scale 0.5 / rotation -5 / softness 100 等 |

底色：暖墨 `#241C15`（canvas clearColor），内容面板 `rgba(20,14,10,.55)` + blur。

## 4. 性能口径（无风扇机红线，源仓已做全保留 + 合并码农口径）

0. **30fps 硬顶**（源仓同款）+ **FLOW 流场 1/4 分辨率**（已在 §2）+ `powerPreference: 'low-power'` + `prefers-reduced-motion` 只画一帧静态 + canvas 离开 DOM 自动停。

1. 渲染分辨率封顶：`dpr = min(devicePixelRatio, 1.5)`（源仓同款）；再乘 0.75 缩放系数可选，流体是模糊层不需要全分辨率。
2. 页面失焦/最小化即 `setRunning(false)`；模块开关关闭时 dispose（源仓教训：泄漏一帧一个全分辨率求解器）。
3. brush 鼠标馈送默认关（源仓 ships OFF，最贵路径）。
4. **降级链**：无 WebGL2 / shader 编译失败 → 静态双色竖渐变（EUI-NEO 原生）→ 再不行纯色暖墨底。源仓 attachFluidShader 本身 no-op 降级不抛错，照抄语义。
5. 帧循环带 rAF 取消与 dispose，禁全局单例常驻。

## 5. EUI-NEO 落地缝（码农对缝项）

- GLSL ES 300 → OpenGL 3.3 core：`#version 300 es` 换 `#version 330 core`，`in/out` 同构，RGBA16F 纹理 + ping-pong FBO 需要 `GL_EXT_color_buffer_float` 等价物（桌面 GL 原生支持 half-float renderable）。**待验证**：EUI-NEO 是否暴露"全窗口底层自绘 quad"缝（先 UI 后绘背景层）；没有则按上游贡献政策在 fork 加，不搞本地 hack。
- 精度两处偏差必须带上（源仓已修过的坑，别再踩一遍）：① `precision highp`（Intel 核显 mediump=16bit 会塌成色块大方块）；② DISPLAY_SHADER 的 hash 用 Dave Hoskins sin-free 版（`sin(dot)*43758` 在大坐标下退化成平台硬边）。

## 6. 验收标准

1. 色系手感：老板开源仓原页面 + 换色参数过目拍板（不再自做 demo）。
2. app 内：真窗口 probe 实拍 + 几何断言（背景层 z 序在最底、内容面板可读性 = 文字与背景对比度数值断言）。
3. 性能：无风扇目标机上 GPU 占用有数字证据（活动监控器截图），失焦后帧循环确停（代码断言）。
4. 降级链逐级验证：禁用 WebGL2 模拟 → 静态渐变兜底可见。
5. MIT 声明落进我方分发物（关于页/第三方声明文件）。

## 7. 烘焙管线规格（【已封存备档】2026-10-29 短暂启用后撤回：老板改令「继续完善当前动态效果」，shader 线解冻为主线路线=参数全套回原值 + 模糊烘进 DISPLAY pass（5-9 tap 高斯）；本节留档，日后若再转搬运路线按此执行）

> 依据：C++ 重写 shader 两版对照均不及格；浏览器子系统渲染是已被源仓验证的正确结果——「把浏览器画好的搬进 app」。

- **内容形态**：含暖墨底色的完整帧（不带 alpha 通道，体积小、播放层最简）；色系=§3 终版三色 + 底色 #241C15。
- **分辨率**：烘焙 800×500（16:10 对齐主窗口比例），播放层双线性拉伸到窗口尺寸——流体是模糊层，半分辨率无观感损失。
- **帧率/时长**：15fps × 30s = 450 帧。流体低速运动 15fps 足够平滑。
- **无缝循环**：流体 sim 无天然循环，采用 2s 交叉淡化接缝——烘 32s，末 2s 与首 2s 渐变混合后截取 30s。判据：接缝处逐帧无跳变（数值断言：相邻帧平均像素差 ≤ 3/255）。
- **驱动**：Playwright 无头 Chrome 加载源仓子系统（/tmp/dst 内临时 host page，管线工件不进仓）。参数全套原值（§3）。
- **体积红线**：动画 WebP ≤ 10MB。超标降档顺序：15fps→12fps→10fps，仍超则 800→640 宽。
- **播放层对缝（码农唯一对缝项）**：EUI-NEO 是否已有全窗口底层循环贴图/视频播放能力；有则填帧，没有再评估加缝（按上游贡献政策走 fork）。
- **性能口径**：播放期仅解码+贴图；无风扇机验收=解码 CPU 占用数字证据，目标 <10% 单核。
- **验收**：① 并排贴图与源仓页面（同参数）目测无明显差别 ② 循环接缝不可见（数值断言）③ 体积 ≤10MB ④ 解码 CPU <10% ⑤ MIT 声明随帧资产分发。
