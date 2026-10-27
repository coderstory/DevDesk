# 壁纸模块 P5（播放/片库 UI）独立审计

- 日期：2026-10-27
- 审计对象：worktree `D:/project/gui/wt-wp-verify`，分支 `verify/wallpaper-module @ eec88dbb`
- 审计范围：`src/module_wallpaper/wallpaper_ui.{h,cpp}`（1191 行）、`wallpaper_module.cpp` P5 改动、`tests/test_modules/test_wallpaper_ui.cpp`（919 行）、`tests/probes/wallpaper_probe.cpp`（703 行）
- 独立性声明：审计者未参与本轮实现、非实现者延续会话；实现方为另一主会话（分支即其交付物）。只读代码，唯一写动作为本文件。
- 交叉证据：EUI-NEO `components/dropdown.h`、`components/scrollview.h`、`core/dsl.h`、`core/runtime/{runtime_render,runtime_input,runtime_hit_test}.h`、app 侧 `src/ui_kit/*`、`core/{image_library,presentation,rotation,availability}`、`src/platform/{folder_picker,wallpaper_capability}_*`。

## 结论

有条件通过。布局几何、回归面、平台对称性、组件真实能力核对全部成立；但存在 1 处硬要求违反（图片档位过滤宣称已生效）、1 处失效遮罩被 z 序压制的正确性缺陷、1 处与之配套的假绿断言。建议修 F1/F2/F3 后再合入。

## 六维度

### 1. 正确性 —— 主体成立，2 处缺陷
已核成立：
- viewport 公式与钉值一致：页区 792×600 → scroll 高 456、1392×960 → 816（wallpaper_ui.cpp:1142-1144；test_wallpaper_ui.cpp:197-200）。
- 首屏无滚动条：折叠态内容高 412（播放/片库视频）/436（片库图片）< 456，测试 6 场景 + probe §3c 5 场景双钉（test_wallpaper_ui.cpp:275-343；wallpaper_probe.cpp:603-605）。
- 折叠展开 506 > 456 允许滚动，符合"展开后可滚动"口径。
- interval 弹层几何：内容系弹层底 ≈420 < 456，scroll 根 clip（scrollview.h:93）不裁弹层；scrollOffset 经 onChange 回写、超界 clamp（scrollview.h:85-86）。
- 页 padding 序 (left,top,right,bottom) 与 dsl.h:443-451 一致；kHeaderH=40（metrics.h:12）、kButtonHeight=40（button.h:33）实值核对通过。
- **wrapper 审查结论（派单点名）：干净**。dropdown root 子元素默认落 (0,0)（dsl.h:110-113），字段盒与 wrapper ddW×40 逐像素重合；stack 默认不裁剪（dsl.h:124 clip=false），弹层溢出 wrapper 无裁切；命中测试按元素自身 frame + clip 链（runtime_input.h:124-139），不受 wrapper 40 高限制。弹层根高=字段+gap+弹层（dropdown.h:96-97）与 wallpaper_ui.cpp:594-596 注释吻合。
缺陷：
- **F2**（wallpaper_ui.cpp:622-633）失效遮罩 zIndex 0，被 dropdown wrapper 子树（根 stack 恒 zIndex 20，dropdown.h:105-107/244；关闭态也是 20）按 subtreeMaxZIndex 兄弟排序压在下面：后绘（runtime_render.h:250 orderedChildren）+ 先命中（runtime_input.h:111-121 逆序）。遮罩被不透明字段完全盖住、点击落到字段。详见 findings。
- **F6**（wallpaper_ui.cpp:31-32）"首屏任何来源/页签都不出现纵向滚动条"未限定 100% 字号档；卡片高 roundFs 随字号档放大，130% 档内容 ≈521 > 456 必出滚动条，测试只有默认档。详见 findings。

### 2. 回归面 —— 未发现问题（已核）
- wallpaper_module.cpp P5 改动仅 compose 接线：snapshot 取一次 + 13 个 UiActions 回 this→service_ + uiState_ 传引用（wallpaper_module.cpp:32-69）。onStart/onStop 结构、pageId/displayName/iconGlyph 未动。
- P1 门控回归由 probe 全链覆盖：默认关不注册（§1）、开→注册+侧栏+空态（§2）、设置卡 44×24 开关几何（§3b）、关→注销+回 Home+条目消失+toast（§4），且走真装配/真注册表/真命中分发（probe:319-416/675-691）。
- 服务侧 P5 触点核对：setImageMinPixels 触发重扫（wallpaper_service.cpp:257-264）、setInterval 吸附（203-210）、setMode 不重扫只换轮换语义（195-201）——与 UI 期望一致。

### 3. 平台对称性 —— 未发现问题（已核）
- 模块 CMakeLists 源集按 WIN32 分支完备（CMakeLists.txt:40-54：win 用 wallpaper_host_win/mpv_controller，非 win 用 stub），模块全平台可编译。
- capability 三平台判据：win=true、macos=false、posix=false（wallpaper_capability_*.cpp）；工厂 nullptr → 页面不注册（wallpaper_module.cpp:74-77），非 Windows 上 UI 不可达。
- probe 在不支持平台诚实跳过 exit 77（wallpaper_probe.cpp:168-174）。
- 业务侧零平台宏（model.h 零依赖声明成立）；folder picker posix/mac 兜底返回 fail 不崩（folder_picker_posix.cpp:7-12），且 UI 不可达时不会走到。

### 4. 静默失败面 —— 1 处构造性脆弱，其余已核
已核成立：
- onChange index 越界：clamp + 显式边界检查（wallpaper_ui.cpp:609-615）；selected 越界由组件 clamp（dropdown.h:88）。
- 空 items dropdown：kRotationChoiceSeconds 固定 6 项不可能为空；即便为空组件走 placeholder 路径不崩（dropdown.h:87-89）。
- UiActions 全部判空后注入（如 wallpaper_ui.cpp:433-436、529-531、897-906）；扫描中按钮双保险（去回调 + 遮罩 blockPointer + 测试钉 onClick 为空，test_wallpaper_ui.cpp:840-842）。
- pickerSlot 用户取消 isFail 静默返回（wallpaper_module.cpp:58）。
- 片库卡扫描遮罩不受 z 序问题影响：ui_kit button/wtab/wswitch 零 zIndex（已核源码），遮罩后绘即覆盖。
缺陷/脆弱：
- **F4**（wallpaper_probe.cpp:492-496 + 673；wallpaper_ui.cpp:616/651/685/739/1101/1124/1173）probe 的 WallpaperUiStubModule.state 与生产 WallpaperModule.uiState_ 均被 UI 闭包以引用捕获；uiStub 在 §3c 作用域结束析构后，runtime 上一帧元素树仍持有 [&state] 闭包。当前代码在下一帧 compose 前无输入分发、生产侧与 settings 等模块同为 app 级约定，故均不可复现；但任何人在 §3c 与 §4 之间插入一次排队点击即打在已析构 state 上（UAF）。构造性脆弱，见 findings。
- F2 的另一面：失效控件点击"无声无息"（状态门闩挡弹层、控件外观照常可悬停），属失效表达缺失而非崩溃，归入 F2。

### 5. 测试有效性 —— 断言总体变异有效，1 处假绿 + 系统性口径缺口
变异有效（回退即红）已核：几何逐值 32/20/16/12/104/148/56/150/192/816（test:166-272）、折叠整组开合+默认收起+组独立性（559-637）、空态三变体×2 逐字对 presentation（520-555）、四态快照哨兵（412-517）、最小窗 6 场景越界扫描（275-343）、整树禁入口/文件名扫描（861-919）、档位映射与接线（640-702、705-778）。
- **F3 假绿**：test_wallpaper_ui.cpp:650 遮罩"存在性"断言在 F2 场景恒真——节点在、但被字段盖住；不修 F2 该断言照样绿，是假绿单例。
- **F3 系统性口径**：fireClick（test:101-106）直调 element->onClick()，绕过 runtime_input.h:49-52 的 interactive/disabled/命中链——所有 fireClick 用例（tabs.1.hit、mode.2、resolution.2、fit.1、item.3、folder.select.bg）只证明接线、不证明命中区域。probe 有真指针队列命中分发（probe:366-373）但 §3c 未对 interval 下拉使用。
- **F3 断言缺失清单**（硬要求无覆盖）：
  1. 图片文案不得宣称已过滤 —— 无任何断言（F1 因此漏网）；
  2. interval dropdown 真实开合流（field 点击 → onOpenChange → state.intervalOpen → 下一帧 open）—— 无断言，item.3 直点绕过 open/disabled 门；
  3. 失效遮罩完整覆盖与点击阻断 —— 仅存在性断言（即 F3 假绿本体）；
  4. 130% 字号档最小窗首屏行为 —— 无用例（见 F6）。

### 6. 文档口径 —— 2 处违规，其余已核一致
- **F1 违规**：UI 固定文案宣称像素档过滤已生效（wallpaper_ui.cpp:312/314/316"低于档位不参与轮播"、:989"按总像素量档位筛选"、:979/983"参与轮播/低于档位"统计格），而 image_library.cpp:59-60/133-134 明示 minPixels 显式忽略、passing==total、filtered==0（wallpaper_service.h:48-49 同口径），同页 presentation.cpp:152-156 空态却如实写"分辨率筛选待后续版本接入"——同页自相矛盾且违反硬要求。
- **F5 违规**：页脚硬编码"mpv 0.41.0"（wallpaper_ui.cpp:1076）为全仓唯一版本出处，无 pin/校验（handoff:63 mpv.exe 不进 git、开发兜底外部路径、P6 打包未做）；wallpaper_ui.h:10 与 test_wallpaper_ui.cpp:8 宣称"卡片实色、无玻璃/半透"，但 ui_kit/card.h:15-17 cardFrame 恒挂 kCardGlassAlpha=0.92 半透，测试 sameColor（test:117-120）只比 RGB 恰好掩盖——口径与实现相反（实现本身无错）。
- 已核一致：空态三变体与 presentation.cpp:134-166 逐字一致（测试钉死）；倒计时/间隔/速度/音量/让路原因唯一文案源成立；"不显示文件名、无删除/维护/转码/队列"由整树扫描钉住；UI 不拼状态文案（快照哨兵机制）成立。

## Findings（6 条：major 3 / minor 3）

### F1 · major · 文档口径/正确性：UI 宣称未实现的图片档位过滤
- 位置：wallpaper_ui.cpp:312、314、316（resolutionHint）；wallpaper_ui.cpp:989（folder hint）；wallpaper_ui.cpp:979、983（统计格标签）
- 问题：固定文案"按总像素量档位筛选，低于档位不参与轮播""参与轮播/低于档位"，与 image_library.cpp:59-60、133-134（minPixels 显式忽略、passing=total、filtered=0）及同页 presentation.cpp:152-156（"分辨率筛选待后续版本接入"）直接矛盾。
- 影响：用户切换 1080P/2K/4K 档会真实触发重扫（wallpaper_service.cpp:262）但结果恒不变——"调了没反应"被 UI 背书为"已过滤"，误导用户；后续 P3 接入过滤时必须全页排查宣称点，双口径文案抬高一倍维护成本。
- 建议：过滤未接入期间 resolution 卡 hint 改"分辨率筛选将在后续版本生效"；folder hint 删去档位筛选句；片库图片统计只保留"目录内图片"一格，隐藏参与/低于两格；并按 F3 补"不得宣称过滤"断言（可仿 test:519 的文案哨兵写法钉 wallpaper_ui 侧固定串）。

### F2 · major · 正确性：失效遮罩被 dropdown 子树 z 序压制，视觉与命中双失效
- 位置：wallpaper_ui.cpp:624-632（遮罩，zIndex 0）vs components/dropdown.h:105-107、244（根 stack 恒 zIndex 20，关闭态也是）；core/dsl.h:1774-1788（subtreeMaxZIndex 兄弟稳定排序）；core/runtime/runtime_render.h:250（按 orderedChildren 绘制）；core/runtime/runtime_input.h:49-52、111-121（命中按绘制逆序）
- 问题：遮罩与 dropdown wrapper 是同父兄弟，wrapper 子树上界 20 > 遮罩 0 → wrapper 整子树（含不透明字段、label、chevron）排在遮罩之后绘制，遮罩完全不可见；命中同样先打到字段。所谓"遮罩既断开点击也表达失效"两点都不成立：弹层没开只是因为 onOpenChange 里 disabled→false 的状态门闩（wallpaper_ui.cpp:616-618），字段 hover 高亮照常——失效态零视觉表达、点击无声无息。
- 影响：用户无法分辨间隔下拉在单循环/单张下不可用（静默失效）；后续任何人调 z 序或组件 zIndex 档位都可能悄悄改变此行为而无测试拦截（见 F3）。
- 建议：遮罩 .zIndex(25)（覆盖 20 档字段、低于弹层 900 即可）；或 disabled 分支干脆不建 dropdown、以暗色 style 直绘假字段；同时在测试里钉"遮罩后绘于字段 + blockPointer"。

### F3 · major · 测试有效性：遮罩断言假绿 + fireClick 绕过命中链 + 4 项硬要求零断言
- 位置：test_wallpaper_ui.cpp:650（存在性断言=假绿）；test_wallpaper_ui.cpp:101-106（fireClick 直调 onClick）；test_wallpaper_ui.cpp:681（item.3 绕过 open/disabled 门）
- 问题：见六维度第 5 条。缺失断言清单：①图片文案不宣称过滤；②dropdown 真实开合流；③遮罩覆盖与阻断；④130% 字号档首屏行为。
- 影响：F1/F2 这类缺陷正是在这些缺口里存活的——存在性断言给出假绿信心，fireClick 给出"交互已测"的错觉；等价于把"改动回退会红"的保证收缩到"接线被删才会红"。
- 建议：遮罩加绘制序断言（遍历父 children 的 orderedChildren/子树 z 比较或 frame+blockPointer 联合断言）；补 field→onOpenChange→下帧 open 的状态机用例（两帧 compose）；补 F1 文案哨兵；130% 档复用 kPage1000x640 + setFontScale（如主题测试既有缝）补首屏断言。

### F4 · minor · 静默失败面：probe 中被引用捕获的 UiState 生命周期是构造性脆弱
- 位置：wallpaper_probe.cpp:79-94、492-496、673；捕获点 wallpaper_ui.cpp:616、651、685、739、1101、1124、1173
- 问题：uiStub（含 state）在 §3c 块末析构，runtime 内上一帧元素树仍持 [&state] 闭包；当前 §3c→§4 之间无输入分发故安全，生产侧同构窗口（模块析构→下帧 recompose）与 settings 等模块同为 app 级约定。
- 影响：今天不可复现；任何人在该窗口插入排队点击即 UAF，且 crash 点在闭包里、难定位。
- 建议：uiStub 提升到 main 作用域（与 wallpaper 同生命周期），或 §3c 收尾先空 compose 一帧再析构；长期由 runtime 提供"树重建即弃旧回调"语义并写入口径文档。

### F5 · minor · 文档口径：mpv 版本号无出处 + "卡片实色"宣称与实现相反
- 位置：wallpaper_ui.cpp:1076；wallpaper_ui.h:10；test_wallpaper_ui.cpp:8、117-120；对照 ui_kit/card.h:15-17、docs/handoff/2026-10-27-wallpaper-module-handoff.md:63
- 问题："mpv 0.41.0"全仓唯一出处即此字符串，无 pin/校验（mpv.exe 不进 git、正式打包在 P6）；"卡片实色（ui_kit::cardFrame），无玻璃/半透"与 cardFrame 恒 0.92 alpha 相反，测试 sameColor 只比 RGB 掩盖之。
- 影响：版本号是对外部二进制的未经证实宣称（用户机器 mpv 版本可变）；"实色"注释会误导后续做卡片视觉决策的人。
- 建议：页脚去版本号或运行期取 mpv 版本；两处注释改"cardFrame 0.92 半透表面"；sameColor 可加 alpha 通道比较开关以免继续掩盖。

### F6 · minor · 正确性/文档口径："首屏无滚动条"承诺未限定字号档
- 位置：wallpaper_ui.cpp:31-32；test_wallpaper_ui.cpp:275-343；wallpaper_ui.cpp:1164（contentKey 已含 fontScale）
- 问题：130% 字号档下卡片高 roundFs 放大（内容 ≈521 > 456），1000×640 最小窗首屏必出滚动条，与"任何来源/页签都不出现"的字面承诺冲突；测试仅默认档。
- 影响：字号的用户可调（80-130%），承诺口径与可复现行为不符；后续若有人把"无滚动条"当全档不变式去优化会白费工。
- 建议：注释与测试描述补"默认字号档"限定（或按需求原文钉档位范围），并加 130% 档用例把真实行为钉成显式口径。

## UI 硬要求核对总表

| 硬要求 | 结论 |
|---|---|
| 间距/字号/圆角逐值 | ✓ test:166-272 逐值钉死（32/20/16/24/15/14/12/12 圆角/40 按钮/104/148/56·150·192/816·456） |
| 空态三变体 | ✓ test:520-555 ×2 来源逐字对 presentation.cpp:134-166 |
| 折叠整组 | ✓ test:559-637（任一标题整组开合、让路独立、默认收起） |
| 1000×640 无滚动条 | ✓ 默认字号档 6 场景钉死；✗ 130% 档失守（F6） |
| 图片过滤不宣称 | ✗ F1 违规（resolution hint / folder hint / 统计格三处） |
| 失效态半透遮罩覆盖+阻断 | ✗ F2 z 序压制，遮罩不可见、命中穿透到字段；folder 卡扫描遮罩 ✓ 不受影响 |
| EUI-NEO 真实能力 | ✓ DropdownBuilder 无 .position()、wrapper 方案与组件 build() 根高语义吻合、无假设 API（逐个签名核对） |

## 审计方法备注

- 逐行读 wallpaper_ui.cpp（1191 行）、test_wallpaper_ui.cpp（919 行）、wallpaper_probe.cpp（703 行）、wallpaper_module.cpp、wallpaper_service.{h,cpp}、presentation.{h,cpp}、image_library.{h,cpp}、rotation.h、availability.h、model.h、ui_kit 五组件、EUI-NEO dropdown/scrollview/dsl/runtime_render/runtime_input/runtime_hit_test、平台 capability/picker 三实现、模块 CMakeLists。
- 分支/commit 实证：worktrees/wt-wp-verify HEAD → refs/heads/verify/wallpaper-module → eec88dbbd3ba5b4822400bd47864d699af71a63a。
- z 序结论三重实证：dsl.h rebuildOrderedChildren（1767-1788）+ runtime_render orderedChildren 遍历（250）+ runtime_input 逆序命中（111-121）；渲染/命中均消费 z 排序列表。
- 未执行任何构建/测试（只读审计）；探针绿灯与截图目检结论采信派单书，本审计对其结论的修正仅 F2（disabled 态不在五张截图场景内，目检覆盖不到属正常）。

## 限定复审计（2026-10-27，针对 ef5bdf79 + dce4a25a 两笔修复提交）

- 审计者：独立子代理，未参与两笔修复（实现者为 verify/wallpaper-module 分支主会话），独立性自检通过；范围仅 `git diff eec88dbb..HEAD`，不做全面复审。P3a 侧对账与新引入面 R-2/R-3/R-4 见 p3a-audit 文末同名小节。

### 处置对账（6/6 已修，异常 0）

| 原 finding | 处置 | 核对 |
|---|---|---|
| F1 [major] 宣称未实现的过滤 | 修 | hint 三档改"后续版本生效"、folder hint 删筛选句、passing/filtered 两格不建（wallpaper_ui.cpp:315-324、985-990、1005-1012）；新哨兵 wallpaper_ui_no_filter_claims（test:962-1001，全树禁"不参与轮播/低于档位" + 两格缺席 + 口径子串）。残留口径漂移见 R-1 |
| F2 [major] 遮罩 z 序压制 | 修 | .zIndex(25)（wallpaper_ui.cpp:644）；与弹层 900 冲突核查见下① |
| F3 [major] 假绿+四缺口 | 修 | ①哨兵（见 F1）；②真实开合流 field→onOpenChange→下帧 open→item→onChange+收弹层（test:703-721）；③遮罩 z 比较+帧覆盖+interactive+门闩（test:655-674）；④130% 档显式口径用例（test:1007-1028）。四项全落地，无缺项 |
| F4 [minor] probe UAF 窗口 | 修 | uiStub 提升至 main 作用域（wallpaper_probe.cpp:195-200），reset 早于 runtime.shutdown 且其后无输入分发，与建议一致 |
| F5 [minor] 版本号+实色宣称 | 修 | 页脚去版本号（wallpaper_ui.cpp:1082-1084）；两处注释改 0.92 半透口径（wallpaper_ui.h:10-11、test:8-10）；sameColor alpha 开关建议为"可加"可选项，以注释显式声明不钉（test:118-119），处置成立 |
| F6 [minor] 无滚动承诺未限定档 | 修 | 注释限定默认档（wallpaper_ui.cpp:32-34）+ 130% 档数值断言（test:1007-1028，135/192/73 = roundFs 换算核对通过） |

### 新引入面（派单四点中 ①④）

- **① zIndex(25) 与弹层 900 档冲突面**：**无冲突，已核**。门闩不止 onOpenChange 一处——绘制期 `.open(!disabled && state.intervalOpen)`（wallpaper_ui.cpp:610）使 disabled 时弹层 zIndex 归 0 且 visible=0、items disabled（dropdown.h:162、98、197/253）；遮罩只在 disabled 分支构建。两子树在任何状态组合下不共存于 900/25 同时存活的局面；连"弹层开着时外部切单曲"的窗口也被绘制期门闩收掉（下帧弹层即塌回 0 档）。注释宣称与实现一致。
- **④ F1 文案与 presentation.cpp 权威口径**：**非逐字一致**。presentation.cpp:155 权威口径为"分辨率筛选**待后续版本接入**"，新 UI hint 为"分辨率筛选**将在后续版本生效**"（wallpaper_ui.cpp:316-324）；且 wallpaper_ui.cpp:312-313 注释宣称"口径必须与 presentation::emptyStateCopy 一致——'筛选将在后续版本生效'"属错误归源——presentation 用的不是这句话（原审计 F1 建议文本本身即"将在后续版本生效"，实现照抄建议，背离不存在，但两套表述并存+注释误导归源是新缺口）→ R-1 [Low]。哨兵只钉"后续版本"子串，拦不住两处表述继续漂移。
- 其余 diff 面（几何外溢、probe、旧断言残留）已核无问题：删除两统计格只缩短内容高不破首屏钉值；全 tests/probes grep 无"参与轮播/低于档位/0.41.0/436"残留断言；无 stat.passing/filtered 残留引用。
- 测试健壮性小瑕疵 → R-5 [Low]：fontscale_130 用例 setFontScale(1.3f) 后、reset(1.0f) 前有三个 REQUIRE（test:1013/1017/1024），失败路径跳过恢复会以 1.3 档污染同进程后续用例（级联红，非假绿）。

### 抽查：F2 遮罩 z 序断言变异敏感（静态推演）

回退 wallpaper_ui.cpp:644 的 zIndex(25)：遮罩节点默认 zIndex=0（dsl.h:123 默认值），dropdown 根 stack 恒 20（dropdown.h:107）→ `mask->zIndex > dropdownRoot->zIndex` 即 0>20 **红**（test:664）。两个操作数来自不同节点的独立事实源（组件默认档 20 是组件侧常量，不随被测改动走），断言非恒真；z 序回退只有这条能拦，其余断言（存在性/帧覆盖/interactive）在回退下仍绿——恰好补上原 F3 指认的"存在性断言假绿"缺口。配套：blockPointer→interactive 字段（dsl.h:179）、field id（dropdown.h:109）、popup 关闭态 zIndex 0/打开态 900（dropdown.h:162、253）均与组件实证吻合，无假红。**结论：变异敏感。**

### 复审计结论

**通过**。原 6 条全部已修且与审计建议实质一致（异常 0）；新引入面无 major（R-1 [Low] 文案归源漂移 + R-5 [Low] 测试失败路径档位污染）；F2 z 序抽查变异敏感。R-1 建议随下次触点把 hint 口径改成与 presentation.cpp:155 逐字同串或反向统一，并修正 wallpaper_ui.cpp:313 归源注释。
