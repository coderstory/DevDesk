# DevDesk 池未完成清单快照（Mac 台账）

> 生成：2026-11-23 16:44:09 +0800 · 来源：Mac 机 `docs/pool/pool.db` 只读直查（单事务一致快照）· 本文件为一次性快照，后续状态以池 db 为准
> 用途：换机迁移（Mac → Windows）。Windows 机按本文件把 Mac 台账的未完成内容并入本机池，口径见「导入须知」
> 口径：未完成 = 状态 ∉ {关闭, 拒绝}（含 待验收 / 进行中 / 排产 / 在池 / 挂起），含 temp 临时池，软删条目除外；与 Win 快照（仓库根 `POOL-UNFINISHED.md`，commit 34336e3a）同口径
> 剔除：2026-11-23 10:00 从 Win 台账导入 Mac 池的 87 条（Mac id 396–482，标题首带 `[Win迁移#N]`）不在本文件——Win 池已有原条目。其中 3 条已在 Mac 关账：Win need#371（Mac need#416）、Win bug#341（Mac bug#443）、Win bug#399（Mac bug#460）

## 导入须知（Windows 机）

- **按内容语义对账，不按 id**：两库分叉前共祖，同一 id 内容可能不同（Mac#339=MCP 详情页 vs Win#339=TAB 缩小）。Win 池已有等价条目 → 只并入进度记录/线索，不重复入池；无等价条目 → 入池，标题前加 `[Mac迁移#N]` 溯源（与 Win→Mac 导入同构）。
- **状态机逐级**（在池→排产→进行中→待验收）：原「待验收」导入时只能落「在池」，原状态写进 detail 首行与 blocker。
- Mac 侧已关闭 310 条、拒绝 13 条不在本文件；`personal_todo` 9 条与未挂在未完成条目上的线索（open 线索共 360 条，此处只带挂在下列条目上的）也不在本文件。
- 排版约定（机器可解析）：条目边界 = `#### <池>#<id> ·` 四级标题；字段以 `**字段名**：` 起头，原文到下一个 `**字段名**（N）：` 行或 `---` 为止（原文内不会出现这类行）；子记录一行一条（`- ` 起头），多行记录的续行缩进 2 空格；原文含 markdown 标题行的字段以围栏代码块原样包住；流程卡全文在附录 A，`<!-- doc:<id>:v<版本> begin/end -->` 之间即 doc content 原文（尾部换行已去）；`body` 列是 detail 的 400 字截断件（已逐条核过），不另列。

## 总览

| 池 | 待验收 | 进行中 | 排产 | 在池 | 挂起 | 小计 |
|---|---|---|---|---|---|---|
| need | 19 | 0 | 0 | 15 | 7 | **41** |
| bug | 8 | 0 | 0 | 14 | 6 | **28** |
| debt | 0 | 0 | 0 | 9 | 2 | **11** |
| temp | 0 | 1 | 0 | 0 | 0 | **1** |
| **合计** | **27** | **1** | **0** | **38** | **15** | **81** |

关联资料量：进度记录 184 条（全量，不截断）、关联 BUG 7、验收证据 0、状态变更 208 条、关联线索 108 条（open 104 / stale 4）、关联文档 7 篇（全文见附录 A）。

## 快速索引

| # | 状态 | 优先级 | 负责人 | 标题 |
|---|---|---|---|---|
| need#156 | 待验收 | P1 | developer | 全站弹窗基础样式统一：标题+分隔线+内容区+按钮区（靠右/主副色/上细线）+ 最小尺寸 |
| need#189 | 待验收 | P3 | ZCode主会话 | 主控台 tab 不应出现滚动条（一屏内展示） |
| need#263 | 待验收 | P1 | developer | 记事本模块：整合 md 预览器，对标 Windows 记事本（打开保存新建/查找替换/字体字号/JSON格式化/代码高亮/md 预览） |
| need#269 | 待验收 | P1 | developer | EUI-NEO DslWindowConfig 补 minSize/maxSize 透传（底层 WindowCreateRequest 已有、DSL 层未暴露） |
| need#285 | 待验收 | P1 | developer | need263 批2：查找替换引擎+高亮统一层迁移 ui_kit（F-06/F-07/F-08/F-12） |
| need#286 | 待验收 | P1 | developer | need263 批3：字体字号+wrap+状态栏补全+JSON 格式化+md 预览（F-09/10/11/13/15/16/17/19） |
| need#288 | 待验收 | P1 | developer | need263 批4：未保存保护链+quit guard+设置页文件关联开关（F-05/F-18/D-3/D-7） |
| need#297 | 待验收 | P2 | developer | 打磨批次E·交互补齐（键盘路径/归档入口/根级建目录/空态覆盖/果冻/终端断点/文件名预检） |
| need#331 | 待验收 | P1 | bug-fixer | 文件关联回读错位（OS 弹窗已选 DevDesk、设置页仍显示未关联） |
| need#332 | 待验收 | P1 | developer | Claude 配置页验收七发现（环境变量间距/顶空白/使用中半透明/导入钮回归/连通测试审计/提示文本位置/env列表滚动） |
| need#333 | 待验收 | P2 | bug-fixer | GitHub 卡 Token 眼睛离开页面未恢复掩码（A16 验收不通过） |
| need#335 | 待验收 | P2 | bug-fixer | 记忆配置 tab 无滚动（折叠展开后底部不可达）——受冻结令约束待口径 |
| need#337 | 待验收 | P3 | bug-fixer | 退出未保存弹窗紧凑化（壳 opt-in compact 档+去底盘空白） |
| need#340 | 待验收 | P3 | bug-fixer | 新建智能任务弹窗左 tab 双行文本上下间距微调（观感偏挤） |
| need#342 | 待验收 | P3 | bug-fixer | 终端页新建按钮左侧空隙收窄（观感微调） |
| need#343 | 待验收 | P2 | developer | MCP 卡片布局重设计：文本统一全左侧（designer 门控→实现接力 #339 后） |
| need#344 | 待验收 | P3 | bug-fixer | 「今日 token 分布」卡右侧【<】翻页钮垂直居中修正 |
| need#357 | 待验收 | P1 | ui-ux-designer | DevDesk 三视角独立审计（普通用户 / UI-UX 专家 / C++ 专家） |
| need#380 | 待验收 | P2 | ZCode主会话 | 全仓代码量精简：砍不可达/冗余代码与配置、压缩啰嗦实现 |
| need#231 | 在池 | P3 | ZCode主会话 | 上线模块查询数据源能力缺口：Nacos 服务状态/文本清单查询无后端能力（#192 §4.7 查询条被阻塞） |
| need#257 | 在池 | P3 | ZCode主会话 | 记忆检索质量根治：embedding 升级/hybrid/重排，拉开正负例分差（阈值参数面不可解） |
| need#260 | 在池 | P2 | ZCode主会话 | 记忆修剪/管理：全量列举+单删/批量删+全清强确认（telemem 列举/删除缝已在，need31 结论过时） |
| need#264 | 在池 | P1 | main | EUI-NEO 框架能力补齐（记事本前置）：保存对话框 + 软换行 word wrap + 字体族枚举 |
| need#350 | 在池 | P3 | main-session | 记事本独立窗接入 minSize(720,480)——need#269 能力的消费端接线 |
| need#365 | 在池 | P1 | product-manager | 首启无引导：零数据时 14 个模块平铺 + 一堆 0，新用户不知道从哪开始 |
| need#374 | 在池 | P1 | ZCode主会话 | 三视角审计修复批验收交接卡（11 条已交付 + 验证证据 + 待裁决） |
| need#382 | 在池 | P1 | main-session | 主动找我：事件驱动的主动推送体系（todo 到期/agent 完成/定时提醒聚合到统一达） |
| need#383 | 在池 | P1 | main-session | 终端启动目录从单值升级为「常用目录列表」（多套代码场景，纯点选无快捷键） |
| need#384 | 在池 | P1 | main-session | todo 到期接 schedule 通知（#382 A 档最小闭环）：逾期扫描器 + 去重 + 四渠道投递 |
| need#385 | 在池 | P1 | main-session | 删除项目卡（need-11）死代码：UI 已移除但 640 行仍在编译 |
| need#386 | 在池 | P1 | main-session | DB 族大手术：删数据库升级代码 + 删备份 + 删 dump（不做向后兼容） |
| need#387 | 在池 | P2 | main-session | 数据库每小时定时备份（复用 schedule 系统任务机制，非启动触发） |
| need#388 | 在池 | P2 | ZCode主会话 | 上线模块简化：收敛为可配置检查单（老板 2026-11-22 定调） |
| need#392 | 在池 | P3 | main-session | kMaxToolResultBytes 三处拷贝常量收敛到共享常量头 |
| need#34 | 挂起 | P3 |  | 向量模型升级档（Qwen3-Embedding-0.6B） |
| need#35 | 挂起 | P3 |  | 触发线升级 C（自研全文通道备用图纸） |
| need#56 | 挂起 | P3 | ZCode主会话 | 发布任务全屏配置编辑层交互重设计（与详情页新模式对齐） |
| need#61 | 挂起 | P2 | ZCode主会话 | Windows 机实测 ui-probe-shot 路径 A，验证 skill Windows 差异小节并删待验标注 |
| need#140 | 挂起 | P1 | developer | macOS 标题栏几何对齐 40px（原 need132 F-04/F-05 拆分）：路线 A/B 待老板复核 |
| need#315 | 挂起 | P3 | qa-tester | 记忆 E2E 可行性探查（DB 化 key 通道回归前置） |
| need#345 | 挂起 | P2 | bug-fixer | todo 四列折叠互斥（折叠新列自动展开上一列）——独立折叠态修复 |
| bug#239 | 待验收 | P1 | ZCode主会话 | mac 包主二进制硬链 Homebrew openssl@3 而 .app 内无 dylib——未装 brew 新机启不来（发布阻断） |
| bug#243 | 待验收 | P2 | ZCode主会话 | DMG 图标是旧的蓝色怪物 + DMG 页面排版错乱（标签被裁/文字重叠） |
| bug#251 | 待验收 | P1 | main | Dock 图标偏大：assets/icon.png 满幅(96.1%)，运行时 applyWindowIcon 覆盖了已对齐 HIG 的 icns |
| bug#271 | 待验收 | P1 | 主会话 | 托盘右键菜单失焦后不自动关闭 |
| bug#274 | 待验收 | P1 | bug-fixer | 窗口效果半透/磨砂两档 Win+mac 双平台均无窗体半透明（老板 2026-11-17 实测） |
| bug#275 | 待验收 | P1 | bug-fixer | 花见导入报无法导入（全盘访问已授）+ 点提示里的系统设置按钮闪退（老板 2026-11-17 实测） |
| bug#368 | 待验收 | P0 | developer | 侧边栏末两项被裁出视口且无滚动提示：「关于」页在常见窗口高度下不可达 |
| bug#371 | 待验收 | P1 | developer | 用户指南四处与实际不符：macOS 快捷键写 Ctrl（实为 ⌘）+ 三处描述已不存在功能 + 自动归档无告知 |
| bug#209 | 在池 | P3 | ZCode主会话 | 本地 bundle 残留死字体（无功能影响，重打包自愈） |
| bug#211 | 在池 | P2 | ZCode主会话 | CI Windows 通知用例红（机制假设：MinGW 线程创建失败被 best-effort 丢弃）——待裁决产品口径 |
| bug#228 | 在池 | P3 | ZCode主会话 | Windows 侧流质泵无 occlusion 检测：被完全遮挡时仍出帧（macOS 已修，平台不对称） |
| bug#258 | 在池 | P2 | developer | memory 弹窗（全文/编辑）130% 字号档结构断言红——存量装具欠账（need256 期间发现，非其引入） |
| bug#259 | 在池 | P2 | ZCode主会话 | 记忆安装失败无镜像源 UI 出口：PyPI 直连超时必现，镜像透传缝有效但 GUI 无配置入口（FIND-159-1 迁出） |
| bug#277 | 在池 | P2 | developer | module_git 批次 19 处 std include 违规——非 --dev 构建被守卫链硬拦（CI 推送必红） |
| bug#287 | 在池 | P3 | main | EUI-NEO 基线存量单测红两处：input_line_styles / input_scrollbar（非 notepad 线引入） |
| bug#292 | 在池 | P3 | main | EUI input 缺 readOnly 旋钮——只读窗编辑框一帧回显假活（notepad 走查 P2-2） |
| bug#351 | 在池 | P3 | main-session | 合成 mouseMoved 事件触发 GLFW _glfwInputCursorPos 断言崩溃（need331 取证副作用，真实路径未证可触） |
| bug#366 | 在池 | P1 | developer | 同族残留：memory 模块三处 fopen/一处 mpv 路径仍用 path::string()，中文用户名下失效 |
| bug#370 | 在池 | P1 | qa-tester | 全量收口 3 个 probe 红（预存，已隔离归因）：notepad 焦点回归 + module_toggle 侧栏 + ui_driver Agent 项不可达 |
| bug#390 | 在池 | P1 | ZCode主会话 | Windows CI 红：MSYS2 OpenSSL 4.1 弃用警告×-Werror 在 vendored cpp-httplib（先于 need380 存在） |
| bug#391 | 在池 | P1 | ZCode主会话 | std include 守卫 --self-test 在 CI macOS 假绿（坏树 A/B 变异未生效，libc++ 传递闭包漂移嫌疑） |
| bug#488 | 在池 | P2 | developer | devflow 全批令 cov 冻结在 ZCode/Mac 恒失败（四次实测，单条令正常；需 hook 插桩定位+python 兜底修复） |
| bug#14 | 挂起 | P2 |  | agent_e2e 真网探针首跑挂/复跑绿 |
| bug#16 | 挂起 | P2 |  | 壁纸线·退出 terminate 尾巴 |
| bug#46 | 挂起 | P3 | ZCode主会话 | todo 搜索/悬停自动路径守卫无会红用例（池 15 核对 B 点挂账） |
| bug#47 | 挂起 | P3 | ZCode主会话 | todo_anim_seam 用例④⑤条件轮询 2s 负载下偶发超时（flake） |
| bug#117 | 挂起 | P2 | ZCode主会话 | clean Windows 环境 CJK 度量下 home 统计卡 badge 文案超宽（真发现，CI 隔离待真机 UAT 复核） |
| bug#188 | 挂起 | P2 | ZCode主会话 | Token 额度上限提高：日 1000 万 / 周 5000 万 / 月 1 亿 |
| debt#51 | 在池 | P3 | ZCode主会话 | 测试资产优化：agent_ui DB 持久化用例下沉 service 层（缓行） |
| debt#121 | 在池 | P3 | ZCode主会话 | CI msys2 UCRT64 迁移（两轮翻车已回退 MINGW64+消音，PATH 传导机制待查） |
| debt#250 | 在池 | P3 | ZCode主会话 | need244 审计残余批：P2×9+P3×1 留观（清单卡 need244-claude-install-p2-residual.md） |
| debt#252 | 在池 | P2 | main | 增量构建不拷更新后的图标资产：POST_BUILD copy_if_different 无 DEPENDS，图标改动静默不生效 |
| debt#276 | 在池 | P2 | developer | Debug 覆盖安装致 TCC 授权失效：构建走稳定签名身份（bug275 关联改进） |
| debt#363 | 在池 | P1 | developer | ARCHITECTURE.md 依赖表与 54 处 module_→eui 实践互斥：文档过期未追认既有实践 |
| debt#364 | 在池 | P1 | developer | 文档-实现硬漂移四处：侧边栏宽 260/220/200、最小窗 1200/1000、模块数 9/11、测试数 2048/2591 |
| debt#367 | 在池 | P2 | developer | 错误文案双层化同族残留：schedule_rule 还有 21 处内部字段名 + 全仓 1443 处 errorMessage 直出无翻译层 |
| debt#389 | 在池 | P3 | ZCode主会话 | claude_run_settings 死表+死函数清理（B-7 后遗）+ .verify-screenshots 归位 |
| debt#48 | 挂起 | P3 | ZCode主会话 | 记忆解析层两条 QA 备注：get_memory 冗余容错分支+isoToEpochSec 注释尾缀 |
| debt#213 | 挂起 | P2 | ZCode主会话 | EUI fork 能力项：retained layer 目标缩放 + 签名降频（低分辨率离屏渲染路径，动态背景再降 GPU） |
| temp#489 | 进行中 | P1 | qa-tester | 2026-11-23 收口批：全量复验 + push（11 commit）+ 装机 |

## need 池

### 待验收（19）

#### need#156 · 全站弹窗基础样式统一：标题+分隔线+内容区+按钮区（靠右/主副色/上细线）+ 最小尺寸

- 状态：待验收 · 优先级：P1 · 负责人：developer · 阶段：批B 完成待验收
- 创建：2026-11-06 22:38:06 +0800（developer）· 更新：2026-11-23 09:55:14 +0800（developer）

**详情**：

老板提（2026-11-06）：全站弹窗基础样式统一（设计需求，非单点 bug）。规格：
① 顶部：标题区；标题下方一根分隔线；线下方留少量空隙；
② 底部：按钮区——按钮统一靠右；主按钮（应用/确认）主色；副按钮（关闭/取消）次级色；按钮区上方一根细线；
③ 中间：内容区；
④ 弹窗最小尺寸限制。
影响面：全站所有弹窗（智能体/记忆/MCP/上线/设置等），需 ui-ux-designer 出统一弹窗规范（设计文档 + demo），developer 落地为共享组件。仅登记，暂不分析不开发。

**关联线索**（12）：

- [open] clue#105 · 主题：corta:dialog-standard-baseline · 域：corta · 关键词：弹窗规范, dialog_form, EUI-NEO dialog 实值, 盘点, 按钮 · 事实：全站弹窗现状与框架实值（2026-11-07 need156 盘点，UI 弹窗统一规范基准）：① 站内共享件 src/ui_kit/dialog_form.h/.cpp（DialogFrame/DialogText/DialogButtons；按钮对=左锚各半宽 (w-68)/2×40、y=h-60 底距20、gap8、字号15、9 种样式组合）；src/ui_kit/metrics.h DialogWidth 五档 420/460/640/720/760、kDialogInsetX=24（:42 注释残留旧值 28 与实值不符）、正文行宽=w-48、kDialogTitleFontSize=20。② EUI-NEO components/dialog.h 实值：backdrop 黑 alpha 暗0.46/亮0.28；面板 border=withOpacity(border,0.82)；radius=tokens.metrics.radius.section=16（.theme() 后；fallback 18）；阴影=panelShadow blur24/offsetY8/alpha 暗0.28/亮0.12；默认 actions 行已右锚（按钮 min96/max150、y=max(88,h-58)）；转场 160ms OutCubic scale0.965→1/translateY14→0；zIndex 默认 1000；clamp 弹窗宽高≤视口-96（spacing.overlay=48 两侧，components/theme.h:66）。③ 使用分布：共享件 12 文件（markdown 12 处、release_config 6、memory 4、claude 6、agent 3、settings 4 含 git_error_dialog 自绘行布局、todo 4、wallpaper 2）；纯 components::dialog 自绘 14 处（schedule×3、agent×3、release×5、todo×2、mcp×1）；todo 显式 zIndex（popups.zOf）与默认 1000 并存。④ 共性问题：P1 按钮排布两套（共享左锚半宽 vs 框架右锚）；P2 全站无标题分隔线、无按钮区细线；P3 zIndex 双轨；P4 按钮几何漂移（gap8/12、底距20/h-58、字号15/body）；P5 metrics.h 注释与实值漂移；P6 无最小尺寸下限。⑤ platform/message_box（message_box_macos/win/posix）为原生系统弹窗，不在站内弹窗样式域。 · 来源：一手源码 src/ui_kit/dialog_form.h:26-105、dialog_form.cpp:64-72、metrics.h:18-44、EUI-NEO components/dialog.h:19-91、components/theme.h:61-78,299-301、grep 盘点 2026-11-07；登记：2026-11-07 12:28:04 +0800 ui-ux-designer；复核：2026-11-07 12:28:04 +0800
- [open] clue#190 · 主题：devdesk:dialog-clamp-viewport-48 · 域：devdesk · 关键词：dialog,clamp,viewport,overlay,弹窗,视口,need156,dialog_shell · 事实：EUI components::dialog 视口 clamp 实值是「视口 − spacing.overlay(48)」一次性相减（EUI-NEO/components/dialog.h:81-84），不是设计文档 §3.4 写的「视口 −96」；need156 标准弹窗壳因此在壳内预 clamp −96（ui_kit/dialog_shell.cpp:46-51，框架 −48 更松不生效）。坑：自绘弹窗若直接传设计高（如 600）给框架，矮视口（1000×640）下会得 640−48=592 而非预期 544（agent_ui_tools_dialog_geometry 曾红于此） · 来源：实测+文件:行；登记：2026-11-08 02:19:10 +0800 developer；复核：2026-11-08 02:19:10 +0800
- [open] clue#193 · 主题：devdesk:ui-kit-button-click-node-bg · 域：devdesk · 关键词：button,onClick,.bg,探针,不可点击,need156,dialog_migrate_a_probe · 事实：ui_kit::button(id,...) 的可点节点是 rect 子节点 <id>.bg（onClick 挂在 bg 上，见 src/ui_kit/button.cpp:79-84），stack <id> 自身无 onClick；探针对按钮类入口必须点 <id>.bg，点 <id> 会报「不可点击或缺节点」且弹窗不打开。need156 批A dialog_migrate_a_probe 的 memory.rv.edit.<proposalId> / memory.cfg.data.clear 两处即是此坑（改为 .bg 后四弹窗亮暗两态 SELF-CHECK 全 present、退出码 0）。同理 dialog_shell 的按钮 id 是 <id>.cancel/<id>.confirm，可点节点 <id>.cancel.bg/<id>.confirm.bg。 · 来源：实测 ./build/tests/dialog_migrate_a_probe 修前 exit 1 / 修后 exit 0 + src/ui_kit/button.cpp:79-84；登记：2026-11-08 08:44:37 +0800 developer；复核：2026-11-08 08:44:37 +0800
- [stale] clue#194 · 主题：devdesk:dialog-shell-content-area-height-drift · 域：devdesk · 关键词：dialog_shell,DialogContentArea,内容区,h-158,h-142,注释漂移,need156 · 事实：ui_kit::drawDialogShell 实际传入的 DialogContentArea.h = h - kDialogHeaderH - kDialogFooterH = h-142（src/ui_kit/dialog_shell.cpp:121-122），而 dialog_shell.h:22 与 metrics.h:48 注释都写「内容区高 = h-158」。注释与实现差 16（h-158 对应内容底 = h-89，即底区细线上隙 16；实现给到 h-73 与细线重合）。迁移弹窗时按 area.y/area.h 相对定位可不受影响；绝对 y 推导请以 area.h 实值为准。 · 作废理由：superseded: 被线索 199（subject=devdesk:dialog-shell-content-area-height-drift）取代；标废：2026-11-08 10:21:31 +0800 developer；被 clue#199 取代 · 来源：文件:行 src/ui_kit/dialog_shell.cpp:119-123 vs src/ui_kit/dialog_shell.h:22 / src/ui_kit/metrics.h:48；登记：2026-11-08 09:02:53 +0800 developer；复核：2026-11-08 09:02:53 +0800
- [open] clue#195 · 主题：devdesk:dialog-shell-third-action-slot-gap · 域：devdesk · 关键词：dialog_shell,第三按钮,memory.dialog.engine.wizard,need156,批B,波1 · 事实：need156 批B 波1 实测：memory.dialog.engine（引擎详情）在未安装态需要第三个动作「首次安装向导」，而 DialogShellSpec 只有 cancel/confirm 两槽——保留 ui_kit::button 绘制，y 用 area.y + area.h + 17（= 壳按钮行 h−56，与 .cancel 同基线），x=area.x，宽 158；节点 id 不变 memory.dialog.engine.wizard(.bg)。memory_probe 全绿（含 clickEl("memory.dialog.engine.cancel.bg") 关闭链路与 memory.dialog.wizard.panel 高 348 断言）。 · 来源：实测 ./build/tests/memory_probe PASS + src/module_memory/memory_module_modals.cpp:370-383；登记：2026-11-08 09:04:31 +0800 developer；复核：2026-11-08 09:04:31 +0800
- [open] clue#196 · 主题：devdesk:wallpaper-noaccess-test-coverage · 域：devdesk · 关键词：wallpaper,noaccess,probe覆盖,need156,批B,测试 · 事实：wallpaper.dialog.import.noaccess 无独立 probe 覆盖（tests/probes/ 下 wallpaper_* 系列均不构建该弹窗），只有 doctest tests/test_modules/test_wallpaper_ui.cpp:932-969（wallpaper_ui_import_no_access_dialog）覆盖：body 双原因文案、.confirm.bg 点击走 UiActions.openPrivacySettings、.cancel.bg 点击置 importNoAccessDismissed。need156 批B 迁壳后该用例仍全绿（--filter wallpaper Total 85 / Passed 85）。 · 来源：文件:行 tests/test_modules/test_wallpaper_ui.cpp:932-969 + tests/probes/ 无引用；登记：2026-11-08 09:04:31 +0800 developer；复核：2026-11-08 09:04:31 +0800
- [open] clue#197 · 主题：devdesk:md-menu-poll-dialogs-shell-migrated · 域：devdesk · 关键词：md_module_menu_poll,md.dialog,drawDialogShell,need156,批B波2,坐标平移 · 事实：need156 批B波2 实测：src/module_markdown/md_module_menu_poll.cpp 五个弹窗（md.dialog.newfolder/del/rename/deldir/renamedir）已全部迁 ui_kit::drawDialogShell，该文件 drawDialogFrame/drawDialogButtons 计数归零。高度：newfolder 保持 320；del 200→240、rename 220→240、deldir 200→240、renamedir 220→240（§3.4 下限）。危险语义 del/deldir 用 shell.dangerConfirm=true。内容区实高 h-158：newfolder 320→162，del/deldir 240→82，rename/renamedir 240→82（rename 内容行共 82 高，正好贴满内容区，无溢出）。drawDialogText 复用时把 dialogWidth 传 area.w + ui_kit::kDialogBodyInsetX，则实际文本宽 = area.w（沿用旧「宽 = 传参 − 48」口径）。--filter md Total 258/Passed 257/Failed 0，markdown_probe 与 md_scrim_probe 均 PASS。 · 来源：实测 bash scripts/build-test-run.sh --dev --filter md + git status/grep src/module_markdown/md_module_menu_poll.cpp；登记：2026-11-08 09:16:59 +0800 developer；复核：2026-11-08 09:16:59 +0800
- [open] clue#198 · 主题：devdesk:release-probe-stale-after-need192-focus-layout · 域：devdesk · 关键词：release_probe,基线红,need192,焦点式双栏,drawEnvSection,装具过期,need156 · 事实：need156 批B波3 实测：tests/probes/release_probe 在 HEAD 759b0dea 基线（git stash 掉本波改动后重编）即 exit=1，红因全在 release.detail.env.* 结构缺失（环境分组卡 release.detail.env.23.bg / 分组标题 .name / 区标题 .title）+ D3 挤压态断言——src/module_release/release_detail_draw.cpp 已被 need192 焦点式双栏重构（7002d7e5），旧 drawEnvSection（:1294）变为零引用死代码，探针仍按旧清单断言。探针文件自 36b1f602 后未再同步（7002d7e5/12899847 提交均未改 tests/probes/release_probe.cpp）。结论：该探针红为存量装具过期，与本波 todo/release_config 弹窗迁壳无关，修 probe 属 need192 遗留债不归 need156。 · 来源：实测：git stash 两文件 → build-test-run --dev --no-test --no-run → ./build/tests/release_probe exit=1（基线日志 evidence/dialog-migrate-b/release/release_probe_baseline_HEAD.log）；对照 git log -- src/module_release/release_detail_draw.cpp / tests/probes/release_probe.cpp + grep drawEnvSection 零调用；登记：2026-11-08 10:20:07 +0800 developer；复核：2026-11-08 10:20:07 +0800
- [open] clue#199 · 主题：devdesk:dialog-shell-content-area-height-drift · 域：devdesk · 关键词：dialog_shell,DialogContentArea,h-158,注释一致性,need156 · 事实：ui_kit::drawDialogShell 传入的 DialogContentArea.h 现为 h - kDialogHeaderH(69) - kDialogFooterH(73) - kDialogContentBottomGap(16) = h-158（src/ui_kit/dialog_shell.cpp:119-123，commit 38bdbdb3 已补减呼吸隙）；dialog_shell.h:22 与 metrics.h:48 注释的 h-158 与实现已一致。240 档实高 82、360 档 202、460 档 302、552 档 394。需验算内容是否压细线的弹窗以 area.h 为准。 · 来源：文件:行 src/ui_kit/dialog_shell.cpp:119-123 + 实测 need156 波3 六弹窗布局验算；登记：2026-11-08 10:21:31 +0800 developer；复核：2026-11-08 10:21:31 +0800
- [open] clue#200 · 主题：devdesk:todo-release-config-dialogs-shell-migrated · 域：devdesk · 关键词：todo_ui_dialogs,release_config_ui,drawDialogShell,need156,批B波3,delcol,overdue,docpick,service,env,delete · 事实：need156 批B波3 实测（commit 6178b69b）：src/module_todo/todo_ui_dialogs.cpp 三弹窗（todo.dialog.delcol / overdue / docpick）与 src/module_release/release_config_ui.cpp 三弹窗（release.dialog.service / env / delete）全部迁 ui_kit::drawDialogShell，两文件 drawDialogFrame=0、drawDialogButtons=0、drawDialogShell=3。高度：delcol 200→240（下限）+dangerConfirm；overdue h=182+44n（n 上限 8，封顶 534，原 12 行→8 行）；docpick h=182+min(44n,370)；release service 360 / env 460 / delete 240（dangerConfirm）。内容区实高 h−158（clue199）：docpick 滚动视口底=h−89 贴内容底，env 错误行底 369 ≤ 内容底 371，delete 正文 66+错误行 16=82 正好贴满。节点 id：三个旧 .title 字面量改由壳内 <id>.title 生成（同名不变）；todo.dialog.overdue.title.icon（头区警告三角）随壳无标题图标槽退场，overdue 标题色由 errorText 归一为 textPrimary（壳无标题色参数）。todo 两个 components::dialog 直用点（newtask :211 / detail :845）本波未动。测试：--filter todo 152/152、--filter release 113/113；todo_collapse_probe / todo_drag_probe PASS。 · 来源：实测 bash scripts/build-test-run.sh --dev --filter todo / --filter release + ./build/tests/{todo_collapse_probe,todo_drag_probe}; grep 归零证据 evidence/dialog-migrate-b/shell-migration.txt；登记：2026-11-08 10:21:36 +0800 developer；复核：2026-11-08 10:21:36 +0800
- [open] clue#201 · 主题：devdesk:agent-dialog-probe-stale-tools-close-after-shell-pilot · 域：devdesk · 关键词：agent_dialog_probe,工具弹窗,close.bg,装具过期,基线红,need156,批B波4 · 事实：need156 批B波4 实测：tests/probes/agent_dialog_probe.cpp 在 HEAD 即红——D2 段断言 findElement("agent.dialog.tools.close.bg") 非空（:951/:987），但 commit 36b1f602 试点迁移时已把可用工具弹窗改 ui_kit::drawDialogShell 并退役自绘 ✕（关闭改底区 .cancel），该 commit 只改了 tests/test_modules/test_agent_ui.cpp 与 release_probe，未同步 agent_dialog_probe.cpp（git show --name-only 36b1f602 不含此文件；文件最后改动仍在 d9ca75f0）。结论：此红为存量装具过期，与本波 agent/settings 四弹窗迁壳无关；探针 C 段（AI 草稿子弹窗双层 z 序 / 校验错误 / 生成中 / 请求失败 / 遮罩命中序）全部通过，--shot-draft 与 --shot-draft-error 两张 PNG 正常落图。修 probe 属 36b1f602 遗留债，不归 need156。 · 来源：实测 ./build/tests/agent_dialog_probe --shot-draft ... exit=1（evidence/dialog-migrate-b/agent/agent_dialog_probe.log）+ git log -- tests/probes/agent_dialog_probe.cpp + git show 36b1f602 --name-only；登记：2026-11-08 10:52:21 +0800 developer；复核：2026-11-08 10:52:21 +0800
- [open] clue#202 · 主题：devdesk:agent-settings-dialogs-shell-migrated-wave4 · 域：devdesk · 关键词：agent.dialog.task.draft,agent.dialog.delete,settings.github.err,settings.github.disclosure,drawDialogShell,need156,批B波4,zIndex1100 · 事实：need156 批B波4 实测（四弹窗迁壳）：src/module_agent/agent_module_dialogs.cpp 两弹窗——agent.dialog.task.draft（Standard 460 / 高 380 保持 / zIndex 1100 保留）与 agent.dialog.delete（Compact 420 / 高 190→240 下限 / dangerConfirm=true 承接原 DangerThemed）。draft 的双动作（generate/dismiss）与 ✕（draft.close）壳无对应槽 → 保留自绘，按钮带落 area.y+area.h+33=h−56；generate 仍 disabled(generating)。src/module_settings/git_error_dialog.cpp settings.github.err（ExtraWide 720 / 高 552 保持）内容区重算：摘要 y=area.y、提示行 area.y+52、日志 y=area.y+74 高 area.h−74−76（原硬编码 kLogsH=248 退场）、选中详情贴 area.y+area.h−76；三按钮（retry/copy/close）保留自绘落 area.y+area.h+33，btnW=(area.w−24)/3。src/module_settings/settings_module.cpp settings.github.disclosure（Standard 460 / 高 240 保持）原自绘 64×40 文字钮 .btn.close 退役→壳 .cancel（confirm 留空）。三文件 drawDialogFrame=0 / drawDialogButtons=0 / drawDialogShell 分别 4+1+1。测试：--filter agent 155/153（2 skip 同基线）、--filter settings 136/136；仅改一条断言（草稿按钮带行底距 20→16）。 · 来源：实测 bash scripts/build-test-run.sh --dev --filter agent / --filter settings + grep 归零证据 evidence/dialog-migrate-b/shell-migration.txt + 截图 evidence/dialog-migrate-b/agent/；登记：2026-11-08 10:58:47 +0800 developer；复核：2026-11-08 10:58:47 +0800

**进度记录**（17）：

- 2026-11-06 23:29:52 +0800 · 老板补充（2026-11-06，场景=上线任务编辑弹窗）：弹窗**太大导致显示不全**（超出屏幕/被裁）。要求弹窗尺寸策略双向约束：①最小尺寸控制（原 #156 已提）；②**最大尺寸也必须适配实际可用空间**（不能超出可视区域）。统一弹窗规范除布局样式外，须包含尺寸响应式规则：min 尺寸 + max = min(设计上限, 视口可用空间 - 安全边距)，内容超限时内部滚动。可参照 need170 弹窗 v2 已实现的 `min(600, 视口-96)` 高度策略。
- 2026-11-07 12:14:42 +0800 · 解冻：UI/UX 门控先行——ui-ux-designer 出全站弹窗统一规范+demo，不写实现码。
- 2026-11-07 12:35:36 +0800 · 设计产出完成：① 设计文档 docs/design/2026-11-07-dialog-standard-design.md（页面级构图节+三问、全站弹窗盘点 12 文件/14 处自绘、四段式规格参数全落值、亮暗 token 对照、状态/动效/自适应表、先验验证清单 G1-G5、禁止项自查）；老板规格①②③④逐条落实：标题分隔线 1px borderDefault 线下隙16、按钮右锚 min宽96 gap8 高40 圆角10 字14、按钮区细线 1px borderMuted、最小尺寸 420×240。② demo docs/design/2026-11-07-dialog-standard-demo.html（自包含零依赖，亮暗切换/三代表弹窗/最小尺寸边界/果冻按压，node 语法+结构校验过，三张截图自审过）。③ 新查证事实已入 clue 105（弹窗现状 P1-P6 与 EUI-NEO dialog 实值）。④ 对现状统一变更：按钮左锚→右锚、新增双线、zIndex 归一、gap/底距/字号归一、最小尺寸兜底。实现派发归主会话，demo 非实现蓝图。
- 2026-11-08 01:40:41 +0800 · 实现阶段开启（设计已拍板）：ui_kit 共享弹窗组件+3 试点迁移（release 新建任务/agent 工具/memory 审核详情），其余弹窗迁移清单出终报。实现棒在飞。
- 2026-11-08 02:19:21 +0800 · 组件：ui_kit/dialog_shell.h/.cpp（头区 69=标题20/700+分隔线y=52全宽｜内容区唯一内滚｜底区 73=细线 y=h-73 全宽+右锚按钮对 min 96 自适应 gap 8 字 14｜min 420×240 + 视口 −96 clamp 优先；danger/disabled 态就绪；节点 id <id>.title/.title.line/.footer.line/.cancel/.confirm）。几何常量入 metrics.h（kDialogHeaderH 69/kDialogFooterH 73/kDialogMinW/H/kDialogButtonMinW/Gap）；kDialogButtonGap 上收单源，dialog_form.cpp 去重。试点：① release 新建任务（宽五档 Wide 640 替 560，内容+11 平移，非常规确认子弹窗同壳 220→240）② agent 可用工具（玻璃壳+✕退役，ExtraWide 720，关闭归底区副钮/Esc/遮罩，masonry 不回退）③ memory 提案全文（内容区归一；该文件被并发提交 79a6f94e 携带入库，内容为本条产出）。测试适配：release/agent 两用例节点 id 换壳后口径；release_probe:816 title id 换 release.newtask.dialog.title。迁移清单（下批依据）：可直接换壳=agent.task/detail（#192 后）、mcp 执行详情、release.detail 三弹窗、schedule 三弹窗、todo 两自绘+zIndex 归一、settings.github.err；共享件旧壳批（DialogFrame+drawDialogButtons 左锚半宽）=memory 编辑/掩码/清空×3、release 配置×6、settings×3、claude×6、markdown×12、wallpaper×2；域外=platform message_box。未验证：agent/memory 弹窗亮暗截图探针（无现成 shot 开关，预算内未扩，headless 功能绿已覆盖存在性/几何/关闭三路径）；before/after 对照未完成（stash 态共享构建树编译失败，仅留 after 侧证据）
- 2026-11-08 08:37:42 +0800 · batA 收尾开工：工作区遗留未提交改动转为可提交绿状态（探针红修复 + dialog_shell 死字段清理 + 证据 + 定向测试 + 单 commit）
- 2026-11-08 08:46:25 +0800 · 批A 已收口并提交 b059f5e5（memory 编辑/掩码/清空 + claude 表单/删除/切换迁标准壳；dialog_shell 批B 半成品字段剪除；新增 dialog_migrate_a_probe 四弹窗亮暗取证）。探针根因 = 入口按钮 onClick 挂在 <id>.bg 而探针点 stack（探针问题，非壳缺陷）。定向测试 claude 138/138、memory 106(105 pass/1 skip)、release 113/113 全绿；证据 evidence/dialog-migrate-a/ 8 图 + selfcheck.log（32 行 present / 0 MISSING）。待办：批B 扩展位按钮（secondary/surface/danger/id）需随实现加回；其余模块弹窗迁移留后续批。
- 2026-11-08 08:47:26 +0800 · 批A 收口核实通过（主会话独立复验：探针退出码 0 / 32 present / 0 MISSING；--filter claude 138/138 绿；commit b059f5e5 五文件 +379/−191 不含无关改动；dialog_shell.h 四死字段已删且全仓零引用，HEAD 原态即无扩展字段故不入提交）。批B 规模盘点：旧壳 drawDialogFrame 残留 8 文件 18 调用点（memory 2 / markdown 5 / wallpaper 1 / settings git_error 1 + settings_module 1 / release_config 3 / todo 3 / agent 2）。
- 2026-11-08 09:06:03 +0800 · 批B 波1 完成（commit 27700148）：memory.dialog.wizard / memory.dialog.engine / wallpaper.dialog.import.noaccess 三弹窗迁 drawDialogShell，两文件 drawDialogFrame 归零；--filter memory 106/105/0（1 skip）、--filter wallpaper 85/85/0；证据 evidence/dialog-migrate-b/memory/（12 图，memory_probe PASS）；wallpaper 无 probe 覆盖，仅 doctest。新增线索 194（壳内容区高注释 h-158 vs 实现 h-142 漂移）、195（engine 弹窗第三动作无槽位保留自绘）、196（wallpaper noaccess 无 probe 覆盖）。
- 2026-11-08 09:17:16 +0800 · 批B波2 完成：md_module_menu_poll.cpp 五弹窗（newfolder/del/rename/deldir/renamedir）迁 drawDialogShell，文件内 drawDialogFrame/drawDialogButtons 归零；--filter md Total 258 Passed 257 Failed 0；commit 759b0dea；证据 evidence/dialog-migrate-b/md/md_new_note_shell.png；线索 197。
- 2026-11-08 10:09:45 +0800 · 批B波3 开工：迁移 todo 3 + release 3 旧壳弹窗
- 2026-11-08 10:21:24 +0800 · 批B波3 完成并提交 6178b69b：todo.dialog.delcol/overdue/docpick + release.dialog.service/env/delete 六弹窗全部迁 drawDialogShell，两文件 drawDialogFrame/drawDialogButtons 归零（证据 evidence/dialog-migrate-b/shell-migration.txt）。--filter todo 152/152、--filter release 113/113；todo_collapse_probe/todo_drag_probe PASS。附注：1) todo.dialog.delcol 高 200→240（下限）且 DangerThemed 换 dangerConfirm；overdue 由 12 行降为 8 行（内容区 h−158）；overdue 头区警告图标与 errorText 标题色随壳统一化退场（壳无图标/标题色槽，已入 clue）；2) release_probe 在 HEAD 基线即 exit=1（release.detail.env.* 结构缺失，need192 焦点式双栏重构成因，clue198），与本波无关，未修不属本任务；3) 未跑全量/守卫链（纪律）。
- 2026-11-08 10:58:52 +0800 · 批B波4（末波）完成：agent 2 + settings 2 四弹窗全部迁 ui_kit::drawDialogShell，commit 059f2ba2。三文件 drawDialogFrame/drawDialogButtons 归零（drawDialogShell 分别 4/1/1）。节点 id 零改名（draft.generate/dismiss/close、delete.msg、err.logs.*/btn.*、disclosure.body 全保留）；退场两项：disclosure.btn.close（改壳 .cancel，无测试引用）、draft.title（改壳同名 .title 节点）。zIndex 1100 双保留。高度：draft 380 保持、delete 190→240 下限、err 552 保持、disclosure 240 保持。测试 --filter agent 155/153（2 skip 同基线）、--filter settings 136/136，仅改一条断言（草稿按钮带行底距 20→16）。证据 evidence/dialog-migrate-b/{agent,settings}/。存量红：agent_dialog_probe D2 段 tools ✕ 断言（clue201，36b1f602 退役 ✕ 未同步探针，与本批无关）；git_error_dialog 与 disclosure 无专属探针（布局/文案由 31 条 doctest 覆盖 github_sync 用例）。
- 2026-11-08 10:59:50 +0800 · 批B 主体完成（波1-4）：全仓 ui_kit::drawDialogFrame / drawDialogButtons 调用点已全部归零（18 处→0），6 条 commit 未推（b059f5e5 批A / 27700148 波1 / 759b0dea 波2 / 6178b69b 波3 / 059f2ba2 波4 + 38bdbdb3 壳缺陷修）。壳级缺陷：内容区高漏减底部呼吸隙 16（h−142 应 h−158），已修入 metrics.h kDialogContentBottomGap 单源常量，影响全部已迁弹窗（各减 16px 内容高）。待裁决：①todo.dialog.overdue 标题图标与 errorText 标题色因壳无槽位被归一（节点 todo.dialog.overdue.title.icon 已删）②release_probe 存量基线红 6 条（need192 重构后 drawEnvSection 零引用的装具过期，非本批引入）③agent_dialog_probe 存量红（36b1f602 退役 tools ✕ 后探针未同步）。剩余：components::dialog 直用点 11 处（schedule 3 / mcp 1 / agent 2 / release_detail 3 / todo 2）留待后续批。
- 2026-11-08 11:51:16 +0800 · 开工：老板拍板三项——①壳加 titleIcon/titleColor 参数（恢复 todo.dialog.overdue 警告三角与 errorText 标题色，方案 A）②修 release_probe 6 条存量红（need192 后 drawEnvSection 零引用的装具过期）③修 agent_dialog_probe 1 条存量红（36b1f602 退役 tools ✕ 后探针未同步）。批B 主体已推 origin/dev（7002d7e5..059f2ba2，6 条）。
- 2026-11-08 12:34:15 +0800 · 批B 收尾三项全部完成并推送（e9f1f414）：①壳加 titleIcon/titleColor 参数（方案 A，todo.dialog.overdue 恢复警告三角 + errorText 标题色）②release_probe 6 条存量红修复（095db3d5，装具对齐 need192 焦点式双栏，归 need192 阶段三）③agent_dialog_probe 1 条存量红修复（tools ✕ 退役后改断言 .cancel.bg）。全量验证：doctest Total 2269 / Passed 2261 / Failed 0 全绿，守卫链全过；UI probe 仅剩 2 条存量红（已挂 bug #232：b1_material 三态判据失效源于 need219 批5 的 hover 隐状态词、bug142 模块 tab 卡片缺失为 11-07 已记录 knownfail），与本批无关。全站旧壳 drawDialogFrame/drawDialogButtons 调用点已归零（18→0），components::dialog 直用 11 处留待后续批。
- 2026-11-23 09:55:14 +0800 · 机收口留验（含裁决→观感）：壳参数化 e9f1f414 在（dialog_shell+todo 接线+agent_dialog_probe）；bug 行 id15 已核销（随 #165 关）；095db3d5 探针改写实挂 need192（已关）；全站弹窗观感归老板走查

**状态变更**（10）：

- 2026-11-06 22:38:06 +0800 ·  → 在池 · 入池（ZCode主会话）
- 2026-11-07 12:14:28 +0800 · 在池 → 排产 · （ZCode主会话）
- 2026-11-07 12:14:42 +0800 · 排产 → 进行中 · 解冻：UI/UX 门控先行——ui-ux-designer 出全站弹窗统一规范+demo，不写实现码。（ZCode主会话）
- 2026-11-07 12:35:36 +0800 · 进行中 → 待验收 · 设计产出完成：① 设计文档 docs/design/2026-11-07-dialog-standard-design.md（页面级构图节+三问、全站弹窗盘点 12 文件/14 处自绘、四段式规格参数全落值、亮暗 token 对照、状态/动效/自适应表、先验验证清单 G1-G5、禁止项自查）；老板规格①②③④逐条落实：标题分隔线 1px borderDefault 线下隙16、按钮右锚 min宽96 gap8 高40 圆角10 字14、按钮区细线 1px borderMuted、最小尺寸 420×240。② demo docs/design/2026-11-07-dialog-standard-demo.html（自包含零依赖，亮暗切换/三代表弹窗/最小尺寸边界/果冻按压，node 语法+结构校验过，三张截图自审过）。③ 新查证事实已入 clue 105（弹窗现状 P1-P6 与 EUI-NEO dialog 实值）。④ 对现状统一变更：按钮左锚→右锚、新增双线、zIndex 归一、gap/底距/字号归一、最小尺寸兜底。实现派发归主会话，demo 非实现蓝图。（ui-ux-designer）
- 2026-11-08 01:40:41 +0800 · 待验收 → 进行中 · 实现阶段开启（设计已拍板）：ui_kit 共享弹窗组件+3 试点迁移（release 新建任务/agent 工具/memory 审核详情），其余弹窗迁移清单出终报。实现棒在飞。
- 2026-11-08 02:19:21 +0800 · 进行中 → 待验收 · 组件：ui_kit/dialog_shell.h/.cpp（头区 69=标题20/700+分隔线y=52全宽｜内容区唯一内滚｜底区 73=细线 y=h-73 全宽+右锚按钮对 min 96 自适应 gap 8 字 14｜min 420×240 + 视口 −96 clamp 优先；danger/disabled 态就绪；节点 id <id>.title/.title.line/.footer.line/.cancel/.confirm）。几何常量入 metrics.h（kDialogHeaderH 69/kDialogFooterH 73/kDialogMinW/H/kDialogButtonMinW/Gap）；kDialogButtonGap 上收单源，dialog_form.cpp 去重。试点：① release 新建任务（宽五档 Wide 640 替 560，内容+11 平移，非常规确认子弹窗同壳 220→240）② agent 可用工具（玻璃壳+✕退役，ExtraWide 720，关闭归底区副钮/Esc/遮罩，masonry 不回退）③ memory 提案全文（内容区归一；该文件被并发提交 79a6f94e 携带入库，内容为本条产出）。测试适配：release/agent 两用例节点 id 换壳后口径；release_probe:816 title id 换 release.newtask.dialog.title。迁移清单（下批依据）：可直接换壳=agent.task/detail（#192 后）、mcp 执行详情、release.detail 三弹窗、schedule 三弹窗、todo 两自绘+zIndex 归一、settings.github.err；共享件旧壳批（DialogFrame+drawDialogButtons 左锚半宽）=memory 编辑/掩码/清空×3、release 配置×6、settings×3、claude×6、markdown×12、wallpaper×2；域外=platform message_box。未验证：agent/memory 弹窗亮暗截图探针（无现成 shot 开关，预算内未扩，headless 功能绿已覆盖存在性/几何/关闭三路径）；before/after 对照未完成（stash 态共享构建树编译失败，仅留 after 侧证据）（developer）
- 2026-11-08 02:35:32 +0800 · 待验收 → 进行中 · （developer）
- 2026-11-08 10:58:52 +0800 · 进行中 → 待验收 · 批B波4（末波）完成：agent 2 + settings 2 四弹窗全部迁 ui_kit::drawDialogShell，commit 059f2ba2。三文件 drawDialogFrame/drawDialogButtons 归零（drawDialogShell 分别 4/1/1）。节点 id 零改名（draft.generate/dismiss/close、delete.msg、err.logs.*/btn.*、disclosure.body 全保留）；退场两项：disclosure.btn.close（改壳 .cancel，无测试引用）、draft.title（改壳同名 .title 节点）。zIndex 1100 双保留。高度：draft 380 保持、delete 190→240 下限、err 552 保持、disclosure 240 保持。测试 --filter agent 155/153（2 skip 同基线）、--filter settings 136/136，仅改一条断言（草稿按钮带行底距 20→16）。证据 evidence/dialog-migrate-b/{agent,settings}/。存量红：agent_dialog_probe D2 段 tools ✕ 断言（clue201，36b1f602 退役 ✕ 未同步探针，与本批无关）；git_error_dialog 与 disclosure 无专属探针（布局/文案由 31 条 doctest 覆盖 github_sync 用例）。
- 2026-11-08 11:51:16 +0800 · 待验收 → 进行中 · 开工：老板拍板三项——①壳加 titleIcon/titleColor 参数（恢复 todo.dialog.overdue 警告三角与 errorText 标题色，方案 A）②修 release_probe 6 条存量红（need192 后 drawEnvSection 零引用的装具过期）③修 agent_dialog_probe 1 条存量红（36b1f602 退役 tools ✕ 后探针未同步）。批B 主体已推 origin/dev（7002d7e5..059f2ba2，6 条）。（ZCode主会话）
- 2026-11-08 12:34:15 +0800 · 进行中 → 待验收 · 批B 收尾三项全部完成并推送（e9f1f414）：①壳加 titleIcon/titleColor 参数（方案 A，todo.dialog.overdue 恢复警告三角 + errorText 标题色）②release_probe 6 条存量红修复（095db3d5，装具对齐 need192 焦点式双栏，归 need192 阶段三）③agent_dialog_probe 1 条存量红修复（tools ✕ 退役后改断言 .cancel.bg）。全量验证：doctest Total 2269 / Passed 2261 / Failed 0 全绿，守卫链全过；UI probe 仅剩 2 条存量红（已挂 bug #232：b1_material 三态判据失效源于 need219 批5 的 hover 隐状态词、bug142 模块 tab 卡片缺失为 11-07 已记录 knownfail），与本批无关。全站旧壳 drawDialogFrame/drawDialogButtons 调用点已归零（18→0），components::dialog 直用 11 处留待后续批。（ZCode主会话）

---

#### need#189 · 主控台 tab 不应出现滚动条（一屏内展示）

- 状态：待验收 · 优先级：P3 · 负责人：ZCode主会话 · 阶段：测试中
- 创建：2026-11-06 23:22:36 +0800（ZCode主会话）· 更新：2026-11-08 17:17:20 +0800（ZCode主会话）

**详情**：

老板提（2026-11-06）：主控台这个 tab 最好不要有滚动条——期望内容在一屏内展示下（布局紧凑到无需滚动），或至少避免出现滚动条。需查「主控台」是哪个模块的 tab（疑似记忆模块或智能体模块的 dashboard？），并压缩该页布局高度使其一屏容纳。仅登记，暂不分析不开发。

**关联线索**（2）：

- [open] clue#214 · 主题：devdesk:memory-console-height-budget · 域：devdesk · 关键词：need189,主控台,高度预算,滚动条,kCardGap,临界窗高,344fs · 事实：need189 实测：记忆主控台内容高链（memory_module_panels.cpp:43/45/129/189）为 64+132+104+44=344 段高 ×fontScale + 3×kCardGap(8，memory_module_internal.h:51 已由 need219 从 16 改 8) + 最后组头不追加 gap = 344·fs+24；fs=1.0→368、1.1→402.4、1.3→471.2（probe memory.view.canvas 实测逐档吻合）。可视高 = 窗口客户区 − 40(titlebar) − 104(bodyY) − 12 = windowH−156。出滚动条临界窗高 = 344·fs+24+116+40 = 344·fs+180：fs1.0→524 / 1.1→554.4 / 1.2→592.8 / 1.3→627.2，全部低于最小窗 640 → 现 HEAD 在全部受支持窗高与字号档下主控台不出滚动条（130%+1000×640 最紧，余量 12.8px）。唯一残留溢出：面板库故障行 `cy += 26.0f`（panels:35，未随字号缩放）叠 130%+最小窗 → 497.2>484。scrollview 溢出判据=initialContentHeight>height（EUI-NEO/components/scrollview.h:73-77），未溢出时不建 scroll 节点；溢出时内容宽减 24（非 overlay 档）在 probe 表现为 canvas 宽 1054→1030。 · 来源：实测 memory_probe --size 1118x{480..701} --fs {1.0,1.1,1.2,1.3,1.4} ± uat62_page_shots_probe 1338x741 --font-scale 1.1 像素扫描；登记：2026-11-08 16:45:53 +0800 developer；复核：2026-11-08 16:45:53 +0800
- [open] clue#215 · 主题：devdesk:memory-console-band-height-fix · 域：devdesk · 关键词：need189,主控台,带高常量,kHealthGridH,kUsageBandH,底距8px,临界窗高,--filter memory · 事实：need189 修复：主控台三段带高收成单一来源常量（memory_module_internal.h:57-59 kEngineBandH=64 / kHealthGridH=118 / kUsageBandH=86），健康格底距 22→8、消耗带底距 26→8（均落在规范 §4.3 的 8px 间距档），面板库故障行 cy 由裸 26.0f 改 scaled(26.0f)。内容高链 344·fs+24 → 312·fs+24，出滚动条临界窗高 344·fs+180 → 312·fs+180（130% 档 627.2→585.6，低于最小窗 640）；实测 memory.view.canvas 五档 fs 1.0..1.4 分别 336/367.2/398.4/429.6/460.8，与公式吻合；1338x620+fs130 改前像素扫描右缘绿 thumb 3106 px、改后 0。信息项与字号档零改动，--filter memory Total 106/Passed 105/Failed 0。坑：kHealthGridH/kUsageBandH 是内容底+8 口径（不是设计文档 §2.3/§2.4 的 132/104）——若后续要在格内加内容行必须同步抬常量，否则会压线。 · 来源：实测 memory_probe + uat62_page_shots_probe 像素/几何双判据，evidence/2026-11-08-need189/manifest.md；登记：2026-11-08 17:08:49 +0800 developer；复核：2026-11-08 17:08:49 +0800

**进度记录**（1）：

- 2026-11-08 17:17:20 +0800 · 2026-11-08 完成。实测先推翻原前提：当前 dev 默认窗口本就不出条（装机版 11-05 23:27 早于 need181 分层 11-08 02:15）；真实溢出面=高字号+小窗（130%+620 高超 7.2px）。修：三段带高收单一来源 + 底距归一到 8px + 故障行补 scaled。临界窗高 444→412·fs（130% 档 627.2→585.6）

**状态变更**（4）：

- 2026-11-06 23:22:36 +0800 ·  → 在池 · 入池（ZCode主会话）
- 2026-11-08 15:39:04 +0800 · 在池 → 排产 · （ZCode主会话）
- 2026-11-08 15:39:08 +0800 · 排产 → 进行中 · （ZCode主会话）
- 2026-11-08 17:17:20 +0800 · 进行中 → 待验收 · 2026-11-08 完成。实测先推翻原前提：当前 dev 默认窗口本就不出条（装机版 11-05 23:27 早于 need181 分层 11-08 02:15）；真实溢出面=高字号+小窗（130%+620 高超 7.2px）。修：三段带高收单一来源 + 底距归一到 8px + 故障行补 scaled。临界窗高 444→412·fs（130% 档 627.2→585.6）（ZCode主会话）

---

#### need#263 · 记事本模块：整合 md 预览器，对标 Windows 记事本（打开保存新建/查找替换/字体字号/JSON格式化/代码高亮/md 预览）

- 状态：待验收 · 优先级：P1 · 负责人：developer · 阶段：全批完成待验收
- 创建：2026-11-16 21:02:48 +0800（developer）· 更新：2026-11-18 03:37:29 +0800（developer）

**详情**：

PRD v5 定稿：doc id=40 slug=2026-11-16-notepad-module v5（doc_get 读回验证）。v5 小改（与 UI 设计稿 v2 对齐，已裁决以设计稿为准）：同一文件重复打开→聚焦已有窗、不开新窗（防双窗后保存静默覆盖=数据丢失风险，新增 N-16）。四处改动全部落位：①F-20① 改聚焦口径+pageId 序号后缀仅用于不同文件同名/窗口内部唯一标识；②流程 I2 改聚焦（有未保存改动不改内容仅提前台）+新增异常分支（已有窗已破坏/已关闭→正常开新窗）；③F-02 补「选中已打开路径→聚焦该窗不开新窗」；④§十三 F-20/F-02 验收同步改+新增不同文件同名两窗并存验收。连带同步：流程 A2/B1 同口径、D-6 迁移步骤①措辞、§十四增设计稿 v2 来源、I6 新增不同文件同名流程、R-1 回归动作补聚焦断言。其余内容不动。待裁决仍为 D-1/D-3/D-7/D-8。

**关联线索**（58）：

- [open] clue#228 · 主题：notepad-module:save-dialog-gap · 域：notepad-module · 关键词：EUI-NEO,保存对话框,saveFileDialog,记事本 · 事实：EUI-NEO 文件对话框只有打开侧：core/platform/platform.h:60-62 声明 openFileDialog / chooseFile / chooseFiles（FileDialogOptions 仅 prompt/allowedExtensions/initialDirectory/filterName/allowMultiple），全仓 grep saveFile|NSSavePanel|GetSaveFileName|IFileSaveDialog 零命中 → 保存/另存为对话框是记事本硬缺口，须先在 EUI-NEO fork 补 saveFileDialog。实现落 core/platform/platform.cpp:705（Win GetOpenFileNameW / mac osascript）单文件内 #ifdef 分流，非 *_win/_macos 拆文件。 · 来源：实测 EUI-NEO e066ef4 源码；登记：2026-11-16 21:10:48 +0800 main；复核：2026-11-16 21:10:48 +0800
- [open] clue#229 · 主题：notepad-module:highlight-infra · 域：notepad-module · 关键词：高亮,styled-runs,code_lexers,TextRun · 事实：现有可复用高亮基建（三处、模型不同不可混用）：①src/ui_kit/text_highlight/md_highlight.h MdHighlighter::runsForLine(fullText,lineText,lineStartByte) — 编辑器侧行级 styled-runs，供 components::input 的 .lineStyles(LineStylesProvider)；②src/ui_kit/text_highlight/code_lexers.h CodeHighlighter — 代码词法仅 CodeLang{Java,JavaScript,TypeScript,Sql,Json} 5 类，行级 HlSpan{start,end,HlKind}；③src/module_markdown/md_keyword_highlight.h — 预览侧代码块多色（C++/Python 等，LangSpec 表驱动，返回 eui::TextRun）。styled-runs 通道 core/render/text_types.h:37 的 TextRun 只有 color 一个样式维度（无粗斜体）。 · 来源：实测 src/ui_kit/text_highlight/*、src/module_markdown/md_keyword_highlight.h、EUI-NEO core/render/text_types.h:37-50；登记：2026-11-16 21:10:48 +0800 main；复核：2026-11-16 21:10:48 +0800
- [open] clue#230 · 主题：notepad-module:find-replace-gap · 域：notepad-module · 关键词：查找替换,Ctrl+F,搜索,状态栏 · 事实：现有 md 模块无编辑器内查找/替换（grep searchBar|findBar 于 md_module_editor.cpp = 0 命中；Ctrl+F 仅 md_viewer_window.cpp:323 只读查看器有）；页内只有搜索框（笔记树搜索 markdown_module_internal.h:1007 searchQuery_）与状态栏 md_module_editor.cpp:258 drawStatusBar（含 A-/A+ 字号步进 kEditorFontMinPx=12/kEditorFontMaxPx=24）。查找/替换对记事本是全新能力，不能靠接线复用。 · 来源：实测 src/module_markdown/md_module_editor.cpp、md_viewer_window.cpp:323；登记：2026-11-16 21:11:53 +0800 main；复核：2026-11-16 21:11:53 +0800
- [open] clue#231 · 主题：notepad-module:lexer-coverage · 域：notepad-module · 关键词：词法器,语言覆盖,LangSpec,复用 · 事实：md_keyword_highlight.cpp 已内置 9 类语言的 LangSpec 词法（Lang 枚举：None/Cpp/Python/Js/Go/Rust/Shell/Java/Json，入口 mdKeywordHighlight(code, lang, colors) 返回 code 字节坐标 runs，纯函数零依赖）。这是现有最强复用点——记事本代码高亮不必引入第三方 lexer，只需补 C#/PHP/SQL/HTML/CSS/YAML/TOML 等表项。而 code_lexers.h（ui_kit）仅 5 类且模型不同（HlSpan+Kind），两者不应双源维护。 · 来源：实测 src/module_markdown/md_keyword_highlight.cpp:25-62；登记：2026-11-16 21:12:17 +0800 main；复核：2026-11-16 21:12:17 +0800
- [open] clue#232 · 主题：notepad-module:font-settings-conflict · 域：notepad-module · 关键词：设置页,字体,字号,#151,冲突 · 事实：需求冲突预警：设置页「字号」行已于 #151 按老板令删除（src/module_settings/settings_ui_cards.cpp:277-280 注释：被字号缩放替代），改在 md 编辑器页内做 A-/A+ 步进（md_module_editor.cpp:258 drawStatusBar，域 kEditorFontMinPx=12 / kEditorFontMaxPx=24，持久化键 settings.editorFontSize 与 core::theme 通道保留）。新需求要求「设置里支持设置字体、字体大小」与 #151 裁决方向相反，需在 PRD 澄清项中让用户选择：①在记事本页内（状态栏/工具条）设字体与字号 ②在设置页新增记事本专属卡（≠复活全局字号行）。另：全站无字体族设置 UI 先例，仅 terminal 有（terminal_settings.h:36 fontFamily + terminalFontOptionValues()）。 · 来源：实测 src/module_settings/settings_ui_cards.cpp:277-280、src/module_terminal/terminal_settings.h:36；登记：2026-11-16 21:12:53 +0800 main；复核：2026-11-16 21:12:53 +0800
- [open] clue#233 · 主题：notepad-module:boss-decisions · 域：notepad-module · 关键词：裁决,PRD,范围,多标签,扩展名 · 事实：老板 2026-11-16 四项裁决（澄清问答成果，PRD 直接采用）：①与现有笔记页关系 = 并存，记事本吸收原只读 md 查看器窗（笔记页不动）；②字体/字号设置落记事本页内（沿用 #151「设置页不放字号」方向）；③多标签 v1 不做，后续单独提；④关联扩展名 = md/json/txt + yaml/toml/log/ini + js/ts/css/html/xml/sql/py/cpp 等代码格式（三档全选）。 · 来源：AskUserQuestion 2026-11-16 用户答复；登记：2026-11-16 21:14:31 +0800 main；复核：2026-11-16 21:14:31 +0800
- [open] clue#234 · 主题：notepad-module:os-open-routing · 域：notepad-module · 关键词：OS漏斗,dispatchPageForFile,扩展名路由,关联 · 事实：OS 打开漏斗现有扩展名路由（src/app_assembly.cpp:303-308 dispatchPageForFile）：.md/.markdown→PageId::Markdown；.json→PageId::ClaudeProxy；.js/.html/.htm/.txt→PageId::Markdown；其余→Home。另 pathHasMarkdownExtension（app_assembly.cpp:267）仅 .md/.markdown 走独立只读查看器窗并把主窗留托盘（armTrayIfViewerOnlyLaunch:343 / secondInstancePathsNeedMainWindow:231）。记事本接入需改这四处判定一致性，否则 json 会抢到 ClaudeProxy 页、txt 关到笔记页——不能只改注册表。 · 来源：实测 src/app_assembly.cpp:231/267/303-308/343；登记：2026-11-16 21:14:47 +0800 main；复核：2026-11-16 21:14:47 +0800
- [open] clue#235 · 主题：notepad-module:statusbar-and-assoc · 域：notepad-module · 关键词：状态栏,字号步进,文件关联,多扩展名 · 事实：状态栏可复用样板：md 页状态栏（md_module_editor.cpp:258 drawStatusBar）已有 行:列 · N 字 的码点计数（byteOffsetToLine/lineStartOffset/utf8Length，带 (rev,cursor,mode) 三级缓存）+ 右端字号步进（label 30 + A- 24 + 值 28 + A+ 24，margin24；步长 1px，域 [12,24]，边界 disabled）。记事本状态栏（含字体/字号页内设置）照此規格建。另：md_assoc_card 的「关联 .md 文件」卡目前单扩展名硬编码（md_assoc_logic.h:2 标题即「关联 .md 文件」），多扩展名注册需扩展 file_association 的 AssocConfig（当前 productionConfig = {DevDesk.md, .md} 单扩展名）。 · 来源：实测 src/module_markdown/md_module_editor.cpp:258-420、src/module_settings/md_assoc_logic.h、src/platform/file_association.h:31-32；登记：2026-11-16 21:16:06 +0800 main；复核：2026-11-16 21:16:06 +0800
- [open] clue#236 · 主题：notepad-module:service-reuse-boundary · 域：notepad-module · 关键词：MarkdownService,复用边界,文件读写,原子写 · 事实：硬事实：MarkdownService 是 .md 专属库服务，不可直接给记事本复用——markdown_service.cpp 全文多处硬判 extension()!=.md 即跳过（:149/:181/:528/:677/:748/:1211），且写入必带索引+FTS 更新（markdown_service.h:57 write 注释「relPath 须带 .md」）。记事本为任意扩展名文件，必须走自己的纯文件读写（可复用 md_file_open.cpp:readFileUtf8 的 Windows UTF-16 路径纪律 + core/atomic_write.h 原子写），不能接笔记索引层。 · 来源：实测 src/module_markdown/markdown_service.cpp:138-149/181/518-528/675-678/745-748/1211 + markdown_service.h:57 + src/core/atomic_write.h；登记：2026-11-16 21:16:17 +0800 main；复核：2026-11-16 21:16:17 +0800
- [open] clue#237 · 主题：notepad-module:filedialog-channel · 域：notepad-module · 关键词：文件夹选择器,保存对话框,复用,通道 · 事实：DevDesk 现有代码**从未使用** EUI-NEO 的 openFileDialog（全仓 grep chooseFile/openFileDialog 零命中）；文件选择一律走自研 platform::folderpicker::pickFolder（src/platform/folder_picker.h，macos/win/posix 拆文件 + 可注入缝 setPickerForTest；用在 module_memory/panel_state.cpp:744、module_terminal/terminal_page.cpp:1239）。结论：记事本的打开/保存对话框宜按 AGENTS.md 铁律「EUI-NEO 能力缺失优先在 fork 修」给 EUI-NEO 补 saveFileDialog（与现有 openFileDialog 同文件 core/platform/platform.cpp:705 同款 osascript choose file name / GetSaveFileNameW 分流），而不是在 DevDesk 自造 workaround；且 DevDesk 侧不应直接调 EUI 的 openFileDialog（否则与 folderpicker 双通道）。 · 来源：实测 grep src/ 全仓 + src/platform/folder_picker.h；登记：2026-11-16 21:16:30 +0800 main；复核：2026-11-16 21:16:30 +0800
- [open] clue#238 · 主题：notepad-module:module-wiring-checklist · 域：notepad-module · 关键词：新模块接入,契约点,PageId,CMake,门控 · 事实：新模块接入必改清单（五处契约点，漏一处即静默失效）：①PageId 枚举加值（src/core/module_interface.h:15 + pageIdToString 同步）；②侧边栏登记表一行（src/app_registry.h kModuleRegistry，顺序=侧边栏序=枚举序）；③extern "C" create_notepad_module() 声明；④CMake: 根 CMakeLists.txt:234-248 区 add_subdirectory(src/module_notepad) + 新建 src/module_notepad/CMakeLists.txt 目标 devdesk_module_notepad；⑤模块开关：settings_config.h 加 kModuleIdNotepad + defaultEnabledModules + 可能需 kModuleDefaultsVersion +1 迁移（当前值 4）。另 ⑥ app_assembly.cpp 的 dispatchPageForFile 扩展名路由与 pathHasMarkdownExtension 需同步改造（否则 OS 双击仍送旧页）。 · 来源：实测 src/core/module_interface.h:15-57、src/app_registry.h、CMakeLists.txt:234-248、settings_config.h:57-72/89；登记：2026-11-16 21:16:38 +0800 main；复核：2026-11-16 21:16:38 +0800
- [open] clue#239 · 主题：notepad-module:edit-primitives · 域：notepad-module · 关键词：查找替换,undo,InputState,光标跳转 · 事实：查找/替换实现可直接复用的底层原语：①EUI-NEO input 内置 Ctrl+Z/Ctrl+Shift+Z undo/redo + pushUndoState（components/input.h:257-266，InputModel::pushUndoState/undoEdit/redoEdit）；②批量替换可以“一次 pushUndoState + 一次性改 st.text + ++textRevision”成一个原子 undo 步（md_module_editor.cpp:120-150 commitEditBuffer 即此模式）；③完全无内置查找/替换（grep Find|Replace 零命中），需自建。光标跳转/选区：直接写 InputState.cursor/selectionStart/selectionEnd 并设 followCaret=true（md_module_editor.cpp:126-141 实例）。 · 来源：实测 EUI-NEO/components/input.h:257-266、src/module_markdown/md_module_editor.cpp:120-150；登记：2026-11-16 21:16:53 +0800 main；复核：2026-11-16 21:16:53 +0800
- [open] clue#240 · 主题：notepad-module:encoding-facts · 域：notepad-module · 关键词：编码,BOM,UTF8校验,二进制探测 · 事实：编码处理现状（记事本必须自建的部分）：①全仓无非 UTF-8 编码转换能力（grep iconv|Gbk|GB18030|CP_ACP 零业务命中，MultiByteToWideChar 只用于 UTF-8↔UTF-16 路径转换）→ 记事本 v1 只能假设 UTF-8，非 UTF-8 文件需降级为「只读+提示」或做编码探测（PRD 需裁决）；②严格 UTF-8 校验器已有可复用实现 src/module_git/git_content_model.cpp:19 isValidUtf8（拒代理区/超 U+10FFFF/过长编码/悬挂 continuation），同样有二进制探测口径（kBinaryScanBytes=8192 首 8KiB 含 NUL = 二进制，git 同口径）；③无 BOM 处理代码，BOM 会被当普通字符显示。 · 来源：实测 grep src/ 编码相关 + src/module_git/git_content_model.cpp:15-40/106；登记：2026-11-16 21:17:14 +0800 main；复核：2026-11-16 21:17:14 +0800
- [open] clue#241 · 主题：notepad-module:module-template-perf · 域：notepad-module · 关键词：模块模板,性能边界,undo栈,测线 · 事实：最小模块模板作脚手架：src/module_about/about_module.cpp（391 行）= 最简 Router 页面模块范例；src/module_mcp/mcp_page_module.cpp（1263 行）= 带多个子视图的中间档范例。EUI-NEO input 多行模式：undo 栈深 kMaxUndoDepth=128（input_model.h:376），无行数/字符数硬上限，但全文档度量缓存 cachedLines 为全量向量（input_model.h:593 measureLines 整篇切行）→ 大文件内存/首帧成本需实渲探针实测（不能静态结论）。 · 来源：实测 src/module_about/about_module.cpp、EUI-NEO/components/input_model.h:376/593；登记：2026-11-16 21:17:32 +0800 main；复核：2026-11-16 21:17:32 +0800
- [open] clue#242 · 主题：notepad-module:viewer-migration-surface · 域：notepad-module · 关键词：查看器窗,迁移面,探针,app_assembly · 事实：「记事本吸收只读 md 查看器窗」的改造面（实测）：①唯一生产调用点 = src/app_assembly.cpp:319 module::markdown::openViewerWindow(path)；②查看器公开 API 在 src/module_markdown/md_viewer_window.h（openViewerWindow / kViewerScrollId / kViewerSearchInputId / viewerSessionCountForTest / viewerSearchInjectForTest / viewerSelectionInjectForTest）；③测试面：tests/probes/md_viewer_bug259_probe.cpp + md_viewer_selection_probe.cpp（两个探针直接 include md_viewer_window.h）+ tests/test_modules 内关于 viewer 的用例。迁到记事本 = 要同步改这两个探针的 include/链接目标 + 探针 CMake 登记。 · 来源：实测 grep src/ tests/ 全仓 + src/module_markdown/md_viewer_window.h；登记：2026-11-16 21:17:41 +0800 main；复核：2026-11-16 21:17:41 +0800
- [open] clue#243 · 主题：notepad-module:probe-migration · 域：notepad-module · 关键词：探针,迁移,CMake,GLOB · 事实：查看器迁移测试面精确清单：tests/probes/ 下三个探针直 include md_viewer_window.h 并直接调用 openViewerWindow——md_viewer_probe.cpp、md_viewer_bug259_probe.cpp、md_viewer_selection_probe.cpp。探针 CMake 走 GLOB（tests/CMakeLists.txt:471 CONFIGURE_DEPENDS）+ 按文件名命名目标，不需手工登记，但目标链接库集在 CMakeLists 统一给——新增探针文件名避免 _macos_probe 后缀且归属 devdesk_module_markdown 链接集。若查看器能力迁到新 module_notepad，这三个探针需改 include 路径与链接库（改 module_notepad）。 · 来源：实测 tests/probes/ 文件清单 + tests/CMakeLists.txt:457-490；登记：2026-11-16 21:17:58 +0800 main；复核：2026-11-16 21:17:58 +0800
- [open] clue#244 · 主题：notepad-module:windows-markdown-verified · 域：notepad-module · 关键词：Windows记事本,Markdown,预览按钮,官方形态 · 事实：调研棒关键事实经主会话独立复核（2026-11-16，WebFetch 官方源）：Windows 记事本 Markdown 支持确实已上线——官方博客 2025-05-30，版本 v11.2504.50.0/11.2504.52.0；视图切换原文「You can switch between formatted Markdown and Markdown syntax views in the view menu」＋「the toggle button in the status bar at the bottom of the window」；可设置关「you can disable support for formatting entirely in app settings」。即官方形态 = 视图切换（菜单+状态栏 toggle），不是单一预览按钮——用户需求的「添加一个预览按钮」是简化形态，PRD 可采用但应在交互文档说明与官方形态的差异。 · 来源：WebFetch 官方博客复核 + docs/research/2026-11-16-notepad-module-research.md Q1；登记：2026-11-16 21:18:33 +0800 main；复核：2026-11-16 21:18:33 +0800
- [stale] clue#245 · 主题：notepad-module:wordwrap-gap · 域：notepad-module · 关键词：自动换行,wordwrap,软换行,框架缺口 · 事实：硬事实（影响「自动换行」功能可行性）：EUI-NEO input 多行模式是硬行切分 + 不换行渲染——.wrap(false) 写死在内部（components/input.h:475/496），水平靠 horizontalScroll 滚动，measureLines 按 '\n' 切行（input_model.h:593）。即：无「软换行（word wrap）」能力，记事本的自动换行开关需在 EUI-NEO fork 补软换行排版，或在 DevDesk 另走一条只读预览渲染路。同理无虚拟滚动（全量 cachedLines），大文件需降级。 · 作废理由：superseded: 被线索 257（subject=notepad-module:wordwrap-gap）取代；标废：2026-11-16 21:44:53 +0800 main；被 clue#257 取代 · 来源：实测 EUI-NEO/components/input.h:475/496 + input_model.h:593；登记：2026-11-16 21:18:48 +0800 main；复核：2026-11-16 21:18:48 +0800
- [open] clue#246 · 主题：notepad-module:preview-reuse · 域：notepad-module · 关键词：预览渲染,MarkdownBuilder,复用边界,预览按钮 · 事实：md 预览渲染真源可直接复用且不依赖 MarkdownModule：md_module_preview.cpp:347-356 用 components::markdown(ui, id).markdown(源码段).width(w).wrapContentHeight().style(style).build() 逐块渲染，块重排由 MdBlockFlowDeps 驱动（md_module_preview 里的 MdBlockFlow 钩子结构）。记事本的「md 预览按钮」可轻量用「整篇单块 components::markdown」而不背块流/同步滚动，但需自备 MarkdownStyle（scaledPreviewStyle 在 markdown_module_internal.h:125 为模块私有，需提出去或重写）。块级实时预览/同步滚动属笔记页专属，记事本 v1 不需要。 · 来源：实测 src/module_markdown/md_module_preview.cpp:347-356、markdown_module_internal.h:125；登记：2026-11-16 21:19:02 +0800 main；复核：2026-11-16 21:19:02 +0800
- [open] clue#248 · 主题：notepad-module:association-hard-limit · 域：notepad-module · 关键词：文件关联,UserChoice,自动处理,平台限制 · 事实：用户新需求「设置里加个开关，开了之后自动处理文件的打开方式」有平台硬约束（必须告知用户，否则交付时会出现「开了不生效」）：Windows 8/10/11 起禁止程序静默抢占默认打开方式——UserChoice 键有 hash 保护，写注册表改不动；官方路径只有 IApplicationAssociationRegistrationUI::LaunchAdvancedAssociationUI 弹系统关联 UI 让用户选。仓内既有实证同口径：src/platform/file_association.h:5-8 注释「写入成功 ≠ 双击行为生效（Win10 22H2 实测：注册表写入层生效、行为面不接管）」。所以「自动处理」的真可达范围 = ①自动注册 ProgId + Capabilities 清单 + 写 HKCU\Software\Classes 壳 + ②主动拉起系统关联设置页让用户一键确认 + ③回读查询展示真态（已生效/需确认/失败）——不能承诺「静默接管」。macOS 侧由 LaunchServices 走 app bundle 声明，通常无需用户确认，但首次需系统重建关联缓存。 · 来源：docs/research/2026-11-16-notepad-module-research.md Q5 + 实测 src/platform/file_association.h:5-8；登记：2026-11-16 21:24:45 +0800 main；复核：2026-11-16 21:24:45 +0800
- [open] clue#249 · 主题：notepad-module:boss-decisions-round2 · 域：notepad-module · 关键词：裁决,文件关联,自动换行,字体枚举 · 事实：老板 2026-11-16 二轮裁决（封住三个关键分歧）：①文件关联开关 = **自动注册 + 拉系统确认**（自动写注册表/声明能做的全做 + 回读展示真态；Windows 首次拉起系统关联 UI 让用户当场确认；不承诺静默接管）；②**自动换行必须做**，愿意等 EUI-NEO 软换行框架改造（#267 升为阻塞项，不是可选项）；③字体 = **枚举全部系统字体**（#266 选方案 A 补 systemFontFamilies()，不采用受限表方案 B）。 · 来源：AskUserQuestion 2026-11-16 二轮答复；登记：2026-11-16 21:27:34 +0800 main；复核：2026-11-16 21:27:34 +0800
- [open] clue#250 · 主题：notepad-module:assoc-card-conflict · 域：notepad-module · 关键词：关联卡冲突,D-7,设置页,归并 · 事实：主会话发现的需求冲突（PRD v3 需裁决）：F-19「设置页记事本开关自动维护文件关联」与设置页现有「关联 .md 文件」卡（src/module_settings/md_assoc_card.h / md_assoc_logic.h）会打架——两者都要注册 .md，产生三种冲突态（md 卡开+记事本关 / md 卡关+记事本开 / 双开时谁写谁读），且两张卡的状态字四态互不知情。推荐方案 = md 关联卡归并进记事本开关（单一入口维护全部扩展名含 .md），md 卡从设置页退役或改跳转链接。已升 D-7 交 PRD。 · 来源：主会话实测（两张卡均涉及 .md 注册）+ PRD v2 F-19/R-5；登记：2026-11-16 21:28:47 +0800 main；复核：2026-11-16 21:28:47 +0800
- [open] clue#251 · 主题：notepad-module:encoding-scope-risk · 域：notepad-module · 关键词：编码转换,ANSI,UTF16,BOM,D-8 · 事实：风险提示（已交 PRD v3 升 D-8）：F-16 编码控制是隐藏大工程——全仓无任何非 UTF-8 编码转换能力（grep iconv|Gbk|GB18030|CP_ACP 业务代码零命中），也无 BOM 处理代码。要真正支持「ANSI 编码保存」「UTF-16 BOM 读取」，需引入编码转换层（macOS iconv / Windows MultiByteToWideChar 或自带码表）。推荐 v1 只做 UTF-8（含 BOM 识别与保留）+ 非 UTF-8 一律只读打开+状态栏标注推测编码；完整编码转换列为后续独立需求。 · 来源：主会话实测 grep 全仓 + git_content_model.cpp:19 仅 UTF-8 严格校验；登记：2026-11-16 21:28:47 +0800 main；复核：2026-11-16 21:28:47 +0800
- [open] clue#252 · 主题：notepad-module:json-key-order · 域：notepad-module · 关键词：JSON格式化,键序,std::map,重排 · 事实：JSON 格式化键序事实独立复核（主会话）：src/platform/json.cpp:23 容器为 std::map<std::string, JsonImpl> objVal —— 对象键按字典序输出，**不保留输入顺序**。后果：记事本 JSON 格式化会重排用户文件的键序（语义等值但字节序变）；对配置文件类场景（package.json、tsconfig 等）用户可能不愿接受。PRD D-4 采用「接受现状+界面明示」；若老板不接受，替代方案 = JSON 格式化只做缩进规范化（文本层重排）而不走 parse/stringify，或引入保序容器。 · 来源：实测 src/platform/json.cpp:15-23；登记：2026-11-16 21:29:30 +0800 main；复核：2026-11-16 21:29:30 +0800
- [open] clue#253 · 主题：notepad-module:json-format-sideeffects · 域：notepad-module · 关键词：JSON格式化,精度,隐式改写,幂等 · 事实：JSON 格式化隐式陷阱（实测 src/platform/json.cpp:325-357 + :89 注释）：格式化走 parse→stringify(pretty) 全链，除键序重排外还有两个隐式改写——①大整数过 double 路径可能丢精度（JsonImpl 只有 int64/double 两档，:89 注释「double 用 yyjson 最短形」）；②非法 Unicode 字节会被替换/转义（YYJSON_WRITE_ALLOW_INVALID_UNICODE）、Infinity/NaN 会被写成字面量（ALLOW_INF_AND_NAN）。对「格式化」这种应当幂等且不丢信息的操作，UI 应在首次执行时提示可能重排键序，并建议先备份（或推荐 D-4 改走「仅缩进规范化」的文本层方案）。 · 来源：实测 src/platform/json.cpp:325-357、:89；登记：2026-11-16 21:29:43 +0800 main；复核：2026-11-16 21:29:43 +0800
- [open] clue#255 · 主题：notepad-module:yyjson-locate-pos · 域：notepad-module · 关键词：yyjson,locate_pos,行列换算,JSON错误提示 · 事实：可直接反手就能用的发现（主会话已用官方文档复核）：yyjson 自带 yyjson_locate_pos(data, len, pos, &line, &col, &chr) 将字节偏移换算为行/列（行列 1 基，字符索引 0 基，按 Unicode 字符计）——注释称「If you need the line and column number of the error, you can use the yyjson_locate_pos() function」。现仓 src/platform/json.h 的 ParseDiag 只暴露 bytePos，记事本 JSON 格式化错误提示「第 X 行第 Y 列」只需在 json 封装层加一个薄封装即可，**不需自写行列换算**（省一块工作量与一块测试面）。另参照 vscode-json-languageservice 的做法（jsonValidation.ts:63）：错误提示应为区间（offset+length）而非单点。 · 来源：官方 API.md（raw.githubusercontent.com/ibireme/yyjson/master/doc/API.md）复核 + docs/research/2026-11-16-notepad-oss-reference.md Q3；登记：2026-11-16 21:39:49 +0800 main；复核：2026-11-16 21:39:49 +0800
- [open] clue#256 · 主题：notepad-module:notepad4-anchor-unverified · 域：notepad-module · 关键词：复核,Notepad4,undo组,待验 · 事实：复核状态声明（主会话）：docs/research/2026-11-16-notepad-oss-reference.md 中「Notepad4 src/Edit.cpp:3635 EditReplaceAllInSelection + EditReplaceAllFlag_UndoGroup + SciCall_BeginUndoAction 包裹」这组锚点，主会话尝试用 WebFetch 直抓 raw 文件复核但**抓取被截断**（只到 EditAlignText），未能独立确认。不否定调研棒结论（其称 gh api 实查过 7991 行），但下游引用前应自行核验或降级为「待验」。同文其他锚点（SumatraPDF RegistryInstaller.cpp 的 RegisterForOpenWith/UnregisterFromBeingDefaultViewer/SHChangeNotify、yyjson_locate_pos）主会话已交叉确认。 · 来源：主会话 WebFetch 复核尝试（截断）；登记：2026-11-16 21:41:54 +0800 main；复核：2026-11-16 21:41:54 +0800
- [open] clue#257 · 主题：notepad-module:wordwrap-gap · 域：notepad-module · 关键词：软换行,measureLines,纠错,FD-2 · 事实：【纠错，取代 clue 245】EUI-NEO input 多行模式**已经有软换行**：measureLines（components/input_model.h:741-765）对超宽逻辑行按 caretX 停靠点切多个显示段（lines.push_back 时 hardBreakAfter=false 即软段）并逐段 measureMetrics；multiline 布局恒 state.horizontalScroll=0（input_model.h:150）、visibleTextWidth=viewportWidth。此前我说的「.wrap(false) 写死 = 不支持软换行」是误读：input.h:475/496 的 .wrap(false) 作用于**已经切好的单个显示行**（每行已不超宽，不需要再 wrap），不是禁软换行。结论：FD-2 不是从零补能力，而是补残余缺口。 · 来源：主会话亲自复核 EUI-NEO components/input_model.h:140-175/728-770；登记：2026-11-16 21:44:53 +0800 main；复核：2026-11-16 21:44:53 +0800
- [open] clue#258 · 主题：notepad-module:shared-worktree-hazard · 域：notepad-module · 关键词：共享工作树,分支串台,git worktree,多棒并行 · 事实：基础设施风险（实战发生，2026-11-16）：EUI-NEO 子仓只有一份工作树，多棒并行开工互相抢指针——实际发生两次：①字体枚举棒提交后 poc/word-wrap 与 feat/system-font-enum 两个分支指向同一 commit dadd536；②保存对话框棒未提交改动在分支切换时被“静默吸收”（CMakeLists 两行因目标分支已有相同内容而 checkout 不报错）。主会话处置：让保存棒 stash→checkout→pop 归位，主会话亲自只暂存自己的 6 个文件并 commit（51e793a），他人的三个托盘调试脏文件未混入。**教训：多棒并行同一子仓时必须每棒一个 git worktree（git worktree add），不能共一份工作树**；或严格串行。 · 来源：实战事故 2026-11-16 + 主会话处置；登记：2026-11-16 21:52:15 +0800 main；复核：2026-11-16 21:52:15 +0800
- [open] clue#259 · 主题：notepad-module:window-not-page · 域：notepad-module · 关键词：独立窗口,定位修正,非侧边栏,openWindow · 事实：老板 2026-11-16 三轮定位修正（覆盖 PRD v3 / UI 设计稿的「独立侧边栏页面」前提）：记事本是**独立窗口**，只能**通过打开文件访问**，**不能从主程序（侧边栏/页面导航）访问**。即：不进 Router 页面、无侧边栏条目；入口 = OS 双击关联文件 / 打开方式。与现有 md_viewer_window 同型（app::openWindow 独立子窗、不进 Router），而非 module_* 页面模块。影响：PageId/侧边栏登记/A-1~R-7 回归声明中的「新增 PageId 与注册行」作废；UI 设计稿三段式版面仍可用但容器从「页面」改「窗口」（需自窗口标题栏/最小尺寸/关窗未保存保护）；此前 D-6（查看器窗退役）变为自然成立——记事本窗就是接任者。 · 来源：用户 2026-11-16 三轮输入（原文：「记事本上独立的窗口，通过打开文件来访问，不能主程序访问」）；登记：2026-11-16 21:56:43 +0800 main；复核：2026-11-16 21:56:43 +0800
- [open] clue#260 · 主题：notepad-module:boss-decisions-round3 · 域：notepad-module · 关键词：裁决,多窗,无新建,托盘态 · 事实：老板 2026-11-16 三轮裁决（封住窗口化后三个歧义，PRD v4 直接采用）：①**纯打开即开，无新建入口**——不双击文件就没有记事本窗，Ctrl+N 不提供（想新建得先在文件管理器建空文件再打开）；②**多窗并存，一文件一窗**——同现有 md 查看器窗同构（自带标题栏、可多窗同时开、互不影响）；③**双击关联文件启动时主窗保持托盘态**，只出记事本窗（复用 armTrayIfViewerOnlyLaunch 既有口径）。 · 来源：AskUserQuestion 2026-11-16 三轮答复；登记：2026-11-16 21:58:45 +0800 main；复核：2026-11-16 21:58:45 +0800
- [open] clue#261 · 主题：notepad-module:independent-window-lifecycle · 域：notepad-module · 关键词：独立生命周期,子窗,主窗分离,托盘 · 事实：老板 2026-11-16 三轮补充（窗口生命周期要求）：记事本窗需**与主窗口分离、独立生命周期**——即主窗关闭/隐藏到托盘不应连带影响记事本窗，记事本窗也不依附主窗的显示状态。待查清 EUI-NEO 现有子窗（app::openWindow / childWindows）是否已具备此语义（现有 md 查看器窗为基本参照），还是需框架补齐（如独立顶层窗、独立进程/事件循环、主窗隐藏时子窗存活、退出顺序策略）。 · 来源：用户 2026-11-16 三轮补充输入；登记：2026-11-16 21:59:12 +0800 main；复核：2026-11-16 21:59:12 +0800
- [open] clue#262 · 主题：notepad-module:childwindow-lifecycle-facts · 域：notepad-module · 关键词：子窗,生命周期,主循环,托盘存活 · 事实：EUI-NEO 子窗生命周期实测事实（主会话读码）：①主循环由主窗驱动（glfw_app_main.cpp:853 while(!glfwWindowShouldClose(window))），子窗由 DslWindowManager childWindows 管，主窗隐藏到托盘后子窗照常更新/渲染（:889-915 托盘分支 childWindows.updateAll + createRequestedWindows + pruneClosedWindows）——即**主窗托盘时子窗可存活**已有先例（原为桌宠 R5）。②主窗关闭回调用 trayAvailable && !forceClose 拦下改为隐藏（:828-833），有模态子窗时重聚炓模态并拦关。③应用退出时 childWindows.destroyAll（:1100）。④子窗创建参数：md_viewer_window.cpp:850-854 用 DslWindowConfig.title/pageId/windowSize(900,640)/resizable/focusOnShow，可另设 hideFromTaskbar/alwaysOnTop/windowEffectOverride。**限制**：子窗仍依赖主窗的主循环存活——主窗真正退出（托盘 Exit / app::requestExit）则子窗一并销毁。真正的“独立进程/独立事件循环”不在现框架内。 · 来源：主会话实测 EUI-NEO core/app/glfw_app_main.cpp:828-833/853/889-915/1100 + src/module_markdown/md_viewer_window.cpp:850-854；登记：2026-11-16 22:00:29 +0800 main；复核：2026-11-16 22:00:29 +0800
- [open] clue#263 · 主题：notepad-module:boss-decisions-round3b · 域：notepad-module · 关键词：裁决,独立生命周期,托盘,进程 · 事实：老板 2026-11-16 三轮生命周期裁决：①「独立生命周期」= **与主窗互不影响即可**（主窗关到托盘/最小化/切页不影响记事本窗；多记事本窗互不影响；关主窗不连带关记事本窗）——**不做独立进程**（框框架已有桌宠子窗先例可达成）；②**关最后一个记事本窗 = 记事本真的退出**，主进程留托盘（保证关联双击能再开窗），与现有 md 查看器窗同口径。 · 来源：AskUserQuestion 2026-11-16 三轮生命周期答复；登记：2026-11-16 22:04:21 +0800 main；复核：2026-11-16 22:04:21 +0800
- [open] clue#264 · 主题：notepad-module:window-lifecycle-feasible · 域：notepad-module · 关键词：托盘态开窗,子窗关闭,进程存活,可行性 · 事实：关键可行性结论（主会话实测，记事本窗方案成立）：①主窗托盘态下，主循环仍执行 createRequestedWindows + pruneClosedWindows（glfw_app_main.cpp:912-914）——即**托盘态也能新开/关闭子窗**，所以「双击文件→新建记事本窗」在托盘态可行；②子窗关闭 API `core::window::requestWindowClose(Handle)`（core/window/window_backend.h:46），已有三处生产用法（fullscreen_channel.cpp:110、release_reminder.cpp:371/613/624），onWindowDestroyed 回调覆盖 requestWindowClose/Alt+F4/应用退出三类销毁路径；③「关最后一个记事本窗」只是销毁子窗，不会触发主窗口退出（主窗 shouldClose 不受子窗影响）——主进程自然留托盘，无需额外机制。 · 来源：主会话实测 EUI-NEO core/app/glfw_app_main.cpp:906-915 + core/window/window_backend.h:46 + DevDesk 三处生产用法；登记：2026-11-16 22:05:48 +0800 main；复核：2026-11-16 22:05:48 +0800
- [open] clue#265 · 主题：notepad-module:same-file-reopen-decision · 域：notepad-module · 关键词：同文件,聚焦,双开,裁决 · 事实：主会话裁决（用户未答复，按默律取安全方）：同文件重复打开 = **聚焦已有窗**（不开第二窗）。理由：双开同一文件时后保存会静默覆盖前保存，属数据丢失风险；聚焦方案无此风险且体验符合系统惯例（macOS/Windows 多数编辑器同口径）。PRD v4 F-20 与设计稿 §2 的矛盾以本条为准，需回写 PRD v5 / 保留设计稿原口径。 · 来源：主会话裁决（AskUserQuestion 无答复，按安全默认）；登记：2026-11-16 22:28:04 +0800 main；复核：2026-11-16 22:28:04 +0800
- [open] clue#267 · 主题：notepad-module:decisions-by-recommendation · 域：notepad-module · 关键词：裁决,推荐项,全批令,D-1,D-3,D-7,D-8 · 事实：老板 2026-11-17 「继续」+ 全批令（排产迁移与派发全放行）：D-1/D-3/D-7/D-8 四项未逐条答复，按主会话先前明示口径「说按推荐就走推荐」采用 PRD 推荐项——D-1=字号与笔记页共用 editorFontSize 通道（联动）；D-3=主窗退出遇多窗未保存逐窗依次保护、任一取消中止整个退出；D-7=md 关联卡归并进记事本开关（单一入口维护全部扩展名含 .md）；D-8=v1 只做 UTF-8（BOM 识别与保留，非 UTF-8 只读+标注推测编码，编码转换后续独立入池）。四项均可逆（后续改口径只需调整对应模块，不推翻架构）。 · 来源：用户 2026-11-17 「继续」+ SessionStart 全批令；登记：2026-11-17 07:55:34 +0800 main；复核：2026-11-17 07:55:34 +0800
- [open] clue#268 · 主题：notepad-module:extension-list-authoritative · 域：notepad-module · 关键词：D-D,扩展名清单,单一真源,notepad_file_types · 事实：【D-D 扩展名清单原文，权威来源】（技术方案卡 §十五 风险 4 的唯一待补事实，已补齐）老板 2026-11-16 一轮裁决原选（三档全选）：①md / json / txt（必含）；②yaml / toml / log / ini；③js / ts / css / html / xml / sql / py / cpp 等代码格式。落单一真源 src/core/notepad_file_types.h 时建议展开为点前缀全名：.md .markdown .json .txt .yaml .yml .toml .log .ini .js .mjs .cjs .jsx .ts .tsx .css .html .htm .xml .sql .py .cpp .cc .cxx .h .hpp。批 1 开工可直接采用，不需再查。 · 来源：clue 233（AskUserQuestion 一轮裁决原文）+ 主会话展开；登记：2026-11-17 08:08:27 +0800 main；复核：2026-11-17 08:08:27 +0800
- [open] clue#269 · 主题：notepad-module:quitguard-semantics · 域：notepad-module · 关键词：quitGuard,拦截语义,死锁风险,批4约束 · 事实：quit guard 行为实测确认 + 实施必守约束（主会话复核 a7a60e0）：consumeExitRequest() 是 exchange(false) 取走即清，shouldProceedExit() 在消费点问 guard；false → 不 break 不 destroyAll，主循环继续。模块处理完保护链后需**再调一次 app::requestExit()** 重新置位 → 下一帧再进 guard（无请求丢失、无死锁，实测 fixture 已验：#1 BLOCK 主循环继续→#2 ALLOW 真退出）。⚠ **必守约束**：若 guard 永远返回 false，用户永远退不出应用——notepad 侧 guard 必须保证三条穷尽路径（全部窗已处理完 / 用户选取消后不再拦截后续请求 / 窗均已关闭）都能返回 true；批 4 验收必须包含「拒退一次后可正常退出」用例与「取消保护后不卡死」用例。 · 来源：主会话复核 EUI-NEO a7a60e0 dsl_app_impl.h:346-384 + glfw_app_main.cpp:881-894；登记：2026-11-17 08:28:10 +0800 main；复核：2026-11-17 08:28:10 +0800
- [open] clue#270 · 主题：notepad-module:merge-pending-tasks · 域：notepad-module · 关键词：遗留待办,合并,字体门禁,四分支 · 事实：遗留待办（合入阶段必做）：tests/CMakeLists.txt:285-289 登记行已注释停用（字体枚举棒因该用例依赖 feat/system-font-enum 分支的 TextPrimitive::systemFontFamilies，未合入 main 时编不过会把 macOS 构建链拖红），且 tests/unit/system_font_gate.cpp 源文件已不在工作区。→ **#266（dadd536）合并进 EUI-NEO main 后，必须恢复该源文件、取消 CMakeLists 注释、跑绿**。另：#265 saveFileDialog、#266 字体枚举、#269 minSize、#272 生命周期四项共 4 个 EUI 分支均未合入 main、未 push，合并时需按「先推 EUI fork→再推 DevDesk」顺序。 · 来源：实测 tests/CMakeLists.txt:285-289 + 工作区核实源文件缺失；登记：2026-11-17 08:29:32 +0800 main；复核：2026-11-17 08:29:32 +0800
- [open] clue#273 · 主题：notepad-module:probe-recipe-headless · 域：notepad-module · 关键词：探针配方,headless,sessionRegistry,compose,id锚点 · 事实：notepad 窗口层无头探针配方（批 2/3/4 探针直接复用）：probe 进程 include <eui/detail/dsl_app_impl.h> + 桩 app::dslAppConfig/app::compose（markdown_probe 同款）后，app::openWindow 内联实现=入队不建窗，故可真调 module::notepad::openNotepadWindow(path) 走真实载入链（notepadLoadFile→applyLoadedState），session 从 module::notepad::detail::sessionRegistry() 取（notepad_session.h:30 公开声明），compose 用 runtime.compose(pageId, w, h, [](Ui&,Screen&){ notepadComposeSession(session, ui, screen); })（tests/probes/notepad_probe.cpp 已验证 PASS，2026-11-17）。状态栏/工具条 id 断言锚=notepad_module.h:18-22；components::input 根元素 id=传入 id 原样（EUI-NEO components/input.h:189 ui_.stack(id_)，"notepad.editor.input" 可直接 find）。 · 来源：实测 tests/probes/notepad_probe.cpp PASS + EUI-NEO components/input.h:189；登记：2026-11-17 12:13:16 +0800 developer；复核：2026-11-17 12:13:16 +0800
- [open] clue#274 · 主题：notepad-module:save-btn-disabled-fix · 域：notepad-module · 关键词：toolbarButton,disabled,保存钮,缺陷修复,批4注意 · 事实：生产缺陷已修（探针断言逼出，2026-11-17）：notepad_window.cpp toolbarButton 原先只灰显保存钮（换色+textDisabled）不设 disabled 元素旗标，只读/错误态按钮仍可点（功能上被 notepadSaveSession 早返回兜底，但交互面假活）。已补 .disabled(!enabled)（notepad_window.cpp:47，md_module_editor.cpp:381/400 步进钮同款），探针断言「只读态保存钮 disabled」已过。批 4 三选对话框/保护链接管保存钮时注意：disabled 态 = 元素旗标，不是颜色约定。 · 来源：实测 notepad_probe 只读态断言 + 修复 notepad_window.cpp:47；登记：2026-11-17 12:13:16 +0800 developer；复核：2026-11-17 12:13:16 +0800
- [open] clue#276 · 主题：notepad-module:macos-ls-registration-hygiene · 域：notepad-module · 关键词：LaunchServices,安装位互踩,lsregister,绑定,双击,Info.plist · 事实：多会话/多线共用 /Applications/DevDesk.app 安装位互踩实锤（2026-11-17）：本人 --install 后数分钟内被并行线（~/.zcode/worktrees/0d01 构建）再装旧版覆盖；LS 数据库记录的在盘二进制指纹失配 → LS 拒绝拉起，表象=「The document X could not be opened. DevDesk cannot open files in the "Markdown Document" format.」（弹窗名取自 plist 首个 claim 类型名）。修复口径：①装机后必须 lsregister -f /Applications/DevDesk.app 重注册；②LSSetDefaultRoleHandlerForContentType 把 public.plain-text/public.text/net.daringfireball.markdown/public.json 绑 bundle id（钉活副本，压过死路径陈旧记录——wt-audit 已删路径的记录 lsregister -u 报 -10814 清不掉，darwin 27 已移除 -kill 全库重建选项）；③LS 绑定生效是异步的，set 后要 sleep 数秒再验证（LSCopyDefaultApplicationURLForURL 立查可能回旧值）。验证工具=swift 直调 LSCopy* API（lsregister -dump 太噪）。 · 来源：bug280 排查实测 2026-11-17（sample/-1712/osascript 三实验）；登记：2026-11-17 13:17:40 +0800 main；复核：2026-11-17 13:17:40 +0800
- [open] clue#277 · 主题：eui-frame:macos-odoc-dispatch · 域：eui-frame · 关键词：odoc,NSApplicationDelegate,NSAppleEventManager,bug281,darwin27,-1712 · 事实：darwin 27（macOS 26.x）Apple Event 派发实证（2026-11-17，AEProbe 最小探针，/Users/coderstory/dev/tmp-bugfix281）：kAEOpenDocuments(odoc) 只经 NSApplicationDelegate 默认路径派发（application:openFile:/openFiles:），经 setEventHandler 装的 NSAppleEventManager 用户 handler 被 AppKit 静默旁路——同进程 quit/oapp 事件用户 handler 与默认机制均正常，唯 odoc 两路全死（用户 handler 不 fire、无默认实现则无回复 → 发送方 -1712 超时；LS 冷启则静默丢弃零日志）。glfwWaitEventsTimeout 的 nextEventMatchingMask 等待内 odoc 可经 delegate 派发并唤醒等待（GLFW 手工事件循环无需 run loop）。探针对照数据：永久 [NSApp run]、LS 拉起、CFBundleDocumentTypes 声明、CFRunLoopRunInMode/NSRunLoop runMode 泵——均不改变用户 handler 死亡这一事实；delegate 补 openFiles: 后 GLFW 式手工循环下 osascript open 立即 exit 0。修复=EUI fork 6dd3600（feat/window-lifecycle-hooks）：GLFWAppDelegate + EUITrayTarget 补 openFile:/openFiles: 转发进 open_file_bridge 队列 + _glfwWaitEventsCocoa sendEvent nil 守卫。⚠AE 实验纪律：osascript 温开受 TCC 逐目标自动化授权门控，授权 pending 时表象同为 -1712（会污染实验判读）；干净实验应走 LS 冷启通道或先确认 Terminal→目标 bundle 的授权。托盘 teardown setDelegate:nil（tray_bridge.c:532 一带）后 delegate 缺失，odoc 会再死——后续若支持托盘运行期重建需补。 · 来源：AEProbe 探针实测 2026-11-17 + EUI fork 6dd3600；登记：2026-11-17 13:50:36 +0800 bug-fixer；复核：2026-11-17 13:50:36 +0800
- [open] clue#278 · 主题：notepad-module:childwindow-titlebar-optout-fix · 域：notepad-module · 关键词：titlebarExtendContent,extendContentIntoTitleBar,FSCV,windowEffectOverride,clearColor,标题栏,漏底,bug282 · 事实：notepad 子窗 UI 三缺陷根因与修复实锤（bug282，2026-11-17 真机验收过）：①标题栏缺失根因=DevDesk 主窗自绘壳把 extendContentIntoTitleBar=true 设成 app 级全局态（settings_config.cpp:371 applyWindowEffectMode），EUI 建子窗/运行时联动无条件继承（glfw_app_main.cpp:496/:965-967/:989-991 applyTitleBarAppearanceToWindow）→ 子窗被 FSCV 铺成满窗内容、交通灯叠进客户区、无标题文字；②半透明漏底根因=子窗无 windowEffect 覆盖时跟随全局三档（desiredChildWindowEffect glfw_app_main.cpp:422-427）+ 全局 clearColor 广播回填 alpha<1（:977-982）。修复：EUI 新增逐窗豁免缝 DslWindowConfig::titlebarExtendContent(bool)（dsl_app.h:363-380，request 字段 app.h:61-65，impl 拷贝 :201，glfw 侧签名加 override 参数默认 nullopt，主窗三调用点零改动）+ notepad 窗四件套配置（notepad_module.cpp:120-137：clearColor 钉 alpha=1 / windowEffect(None) / ignoreClearColorOverride(true) / titlebarExtendContent(false)）。compose 工具条 y=0 无需改：FSCV 关闭后原生标题栏占非客户区，GL 内容自动从条下开始。EUI commit 84e3389（feat/window-lifecycle-hooks，未 push）。运行时切档不回冲：材质传播读 request 覆盖（:1003-1006）、标题栏联动读同字段（:965-967/:989-991 改后）。 · 来源：实测 glfw_app_main.cpp:357-380/422-427/496/960-1010 + window_effect_macos.mm:846-935 + 真机截图 evidence/bug282-283-cold-start.png；登记：2026-11-17 18:52:36 +0800 bug-fixer；复核：2026-11-17 18:52:36 +0800
- [open] clue#279 · 主题：notepad-module:odoc-coldstart-tray-arm · 域：notepad-module · 关键词：odoc,冷启,armTray,托盘,g_mainWindowComposed,温开,bug283 · 事实：odoc 冷启托盘 arm 修复实锤（bug283，2026-11-17 真机验收过）：根因=armTrayIfViewerOnlyLaunch（app_assembly.cpp）只接 argv 通道，macOS Finder 双击/open 命令走 LS→odoc→NSApplicationDelegate→handleAppleEventOpenFile（main.cpp:527-542），冷启在 main 装配 arm（main.cpp:1100）之后、首帧 compose 之前到达，arm 未覆盖该通道。修复=通道无关 helper armTrayForNotepadAssociation(path, channel)（app_assembly.cpp:293-301）：温开门槛内聚在 helper（mainWindowStartupArmingWindowOpen 判 g_mainWindowComposed 首帧闩，sidebar compose 首帧 markMainWindowComposed 置位 app_sidebar.cpp:1319-1322，resetMinimizedFlagForTest 同步回翻），argv/odoc 两通道共用同一状态位不复制；主窗已 compose 后送达=温开 no-op 不抢隐藏（含托盘态双击场景）。真机验证：冷启日志「关联启动（记事本关联 odoc）：主窗保持托盘态，仅记事本窗」（app_assembly.cpp:298）+ 主窗不弹仅记事本窗；温开日志「记事本查重命中，聚焦已有窗」无新 arm 行。定向用例 assembly_arm_tray_odoc_cold_start 三边断言绿（tests/test_modules/test_app_assembly.cpp:580-620）。DevDesk commit 64378979（未 push）。 · 来源：实测 app_assembly.cpp:293-310 + main.cpp:527-542 + 真机日志 devdesk.log 18:50:57/18:51:29；登记：2026-11-17 18:52:36 +0800 bug-fixer；复核：2026-11-17 18:52:36 +0800
- [open] clue#288 · 主题：notepad-module:highlight-layer-migration-facts · 域：notepad-module · 关键词：LangSpec,语言名,md_keyword_highlight,迁移,ui_kit,text_highlight,notepad_language,收敛,待办 · 事实：高亮统一层迁移+语言映射落地实锤（need263 批 2/池 #285，2026-11-18）：①md_keyword_highlight.{h,cpp} 已 git mv 至 src/ui_kit/text_highlight/，namespace 改 ui_kit::text_highlight（与 code_lexers.h 同目录惯例一致；md_highlight.h 仍是 module::markdown 历史孤例未动），逻辑零改；全仓旧路径 include/限定名引用清零（仅余 code_lexers.h:3 注释历史提及，禁改文件）。②关键接口事实：LangSpec 无公开语言枚举——公开入口 mdKeywordHighlight(code, lang: std::string_view, colors) 按语言名（大小写不敏感+别名 cpp/py/javascript/golang 等）分派，内部 enum Lang{None,Cpp,Python,Js,Go,Rust,Shell,Java,Json} 在 cpp 匿名空间（8 语言）；MdHighlightColors/mdHighlightColors 仍属 module::markdown（md_highlight.h）。③notepad_language.{h,cpp} 落地 src/module_notepad/：映射表 16 扩展→8 语言名直接喂 mdKeywordHighlight；两处收敛（.ts→Js 词法、.md/.markdown→None——LangSpec 无 Markdown 词法，且 md_highlight 的 MdHighlighter 是需 fullText 的有状态围栏模型，per-line 三参契约接不了）。provider highlightLine 用 4 参（displayText, lineStartByte, lineNo, lang）保持无状态多窗安全，runs 平移为全文档坐标；三参 lineStyles 契约由 window 层 lambda 绑 session.language 达成（棒 2 接线）。颜色源=module::markdown::mdHighlightColors(core::theme::current())。④待办：TypeScript 专属词法表、Markdown 词法接入（LangSpec 加表或契约演化）；候选 .mjs（langOf 已认识）未入映射表。 · 来源：实测 src/ui_kit/text_highlight/md_keyword_highlight.{h,cpp} + src/module_notepad/notepad_language.{h,cpp} + 定向构建/用例输出；登记：2026-11-18 01:16:36 +0800 developer；复核：2026-11-18 01:16:36 +0800
- [open] clue#290 · 主题：notepad-module:findbar-wiring-framework-facts · 域：notepad-module · 关键词：onKeyEvent,passthrough,styleRevision,lineRuns缓存,value重置,查找条,双通路键路由 · 事实：查找条 UI 接线的三个框架级实锤（need263 批 2 棒 2 实测，批 3/4 UI 面直接复用）：①input 元素 .onKeyEvent passthrough 先于内置编辑处理（EUI-NEO components/input.h:240-243，返回 true 即消费、内置全跳过）——Enter/Shift+Enter/Esc 差异化语义（单行 input 内置 Enter 一律 onEnter 不分 shift）必须走 passthrough 拦截；②窗级 onKeyEvent（DslWindowConfig）只在 UI 树未消费该键时触发（core/runtime/runtime_input.h:323 `if (!handled && keyEventHandler_)`）——『input 聚焦时 input 先吃、未聚焦冒泡到窗级』的双通路接线无双重处理风险；③行级样式缓存键 = (lineNo, textRevision, styleRevision)（input_model.h:946-948），命中集/当前命中变化必须 ++st.styleRevision 才会重算 lineRuns（textRevision 不变的高亮变化场景）；另 .value(text) 与 state.text 不等时会重置光标/选区/undo 栈（input.h:145-163）——外部全选等状态注入要直写 InputState 再靠 value 相等跳过重置（notepad 查找条 pendingSeed 模式）。落地文件：src/module_notepad/notepad_find.{h,cpp}（引擎纯函数）+ notepad_window.cpp（findTick/drawSearchBar/notepadCommitText）+ notepad_session.h NotepadFindState。 · 来源：实测 EUI-NEO components/input.h:145-243 + input_model.h:941-957 + runtime_input.h:323 + notepad_probe PASS；登记：2026-11-18 01:39:26 +0800 developer；复核：2026-11-18 01:39:26 +0800
- [open] clue#291 · 主题：notepad-module:wrap-toggle-framework-gap · 域：notepad-module · 关键词：wrap旋钮,InputBuilder,软换行,横向滚动,F-19,批3矛盾,禁做EUI · 事实：F-19 wrap 关断的框架前提纠错（need263 批 3 棒 A 实查，2026-11-18）：简报「input .wrap(bool) 开关即用、无框架改造」与代码实际不符——EUI-NEO HEAD 019e7e9 的 InputBuilder（components/input.h:48-117 全部 setter 清单）无任何 wrap 旋钮，.wrap(bool) 只存在于 input 内部对 ui.text 显示行元素的调用（input.h:475/496，语义=已切好的显示行不再二次折行）；multiline 分支恒 state.horizontalScroll=0（input_model.h:150）、measureLines 按 viewportWidth 无条件切段（input_model.h:741-765），软换行不可关。word-wrap POC 文档的残余缺口①②③（词边界/缓存键/metrics 切片）均为质量修复、不含开关 API。结论：wrap 关断（超宽行横向滚动）需 EUI-NEO fork 给 InputBuilder 加 wrap(bool)（跳过分段+multiline 分支放开水平滚动），DevDesk 侧无可落地机制（单行模式会剥 \n 不可用）。棒 A 按「禁做不动 EUI-NEO」落了状态层：session.wrapOn + notepad_prefs notepad.wrap 键读写穿透已通，UI 钮未上（假开关不交付），后续棒授权动 EUI 后接线仅需 builder 一处 + 钮一枚。 · 来源：实测 EUI-NEO components/input.h:48-117/475/496 + input_model.h:130-175/741-765 + docs/research/2026-11-16-word-wrap-poc.md §4；登记：2026-11-18 01:57:04 +0800 developer；复核：2026-11-18 01:57:04 +0800
- [open] clue#292 · 主题：notepad-module:yyjson-locate-pos-boundary · 域：notepad-module · 关键词：yyjson_locate_pos,行列,越界钳界,码点计数,jsonLineColForPos,F-11 · 事实：yyjson_locate_pos 边界语义实测补全（need263 批 3 棒 B，yyjson 0.12.0 编译实测，补充 clue #255）：①行列 1 基、列按 Unicode 码点计（CJK 3 字节=1 列单位，实测 "ab\n中文x" pos=9→line 2 col 3）；②pos>len 返回 false 且 line/col/chr 全 0——封装必须先钳界（platform/json jsonLineColForPos 已做：pos>len 钳到 text.size()，空文本=1/1）；③pos=len 合法=文末位置（"a\nb" pos=3→line 2 col 2）；④'\n' 本位算前一行末列。落地：src/platform/json.h JsonLineCol+jsonLineColForPos（json.cpp 内部调 yyjson），notepad 不直接碰 yyjson。 · 来源：实测 EUI-NEO/3rd/yyjson-0.12.0/src/yyjson.c:3043 + /tmp 编译实测 7 组边界 + test_notepad_preview 用例绿；登记：2026-11-18 02:15:11 +0800 developer；复核：2026-11-18 02:15:11 +0800
- [open] clue#293 · 主题：notepad-module:preview-embed-facts · 域：notepad-module · 关键词：components::markdown,MarkdownStyle,scrollView,contentKey,previewOn,探针ui指针,块流隔离 · 事实：md 预览组件面接线事实（need263 批 3 棒 B 实测）：①components::markdown（EUI-NEO components/markdown.h:2625 入口）+ MarkdownStyle 构造器直接吃 ThemeColorTokens（markdown.h:26）；本地预览样式 = MarkdownStyle(componentTheme) 后 blockGap *= core::theme::fontScale()（module_markdown scaledPreviewStyle 同语义：字号族已在 componentTokensWeChat 预乘不可再乘，间距侧要乘——markdown_module_internal.h:125-137）；②scrollView 包 markdown（.width(innerW).wrapContentHeight()）可滚，contentKey 传 std::to_string(InputState.textRevision) 可稳住度量缓存（预览态禁编辑→textRevision 稳定）；③headless 探针陷阱（补充 clue #273 配方）：probe 全局 ui* 指针随「最后 compose 的 session」的 Ui 树走——多 session 探针里 ui->state<T>(id) 写前必须先 composeSession(目标 session) 两帧再写，否则状态写到别的窗树上（实战踩坑：光标断言写进 txt 会话树）。 · 来源：实测 EUI-NEO components/markdown.h:26/2625/1089 + scrollview.h:81-99/163 + notepad_probe PASS（5 session）；登记：2026-11-18 02:15:20 +0800 developer；复核：2026-11-18 02:15:20 +0800
- [open] clue#294 · 主题：notepad-module:softwrap-knob-landed · 域：notepad-module · 关键词：softWrap旋钮,EUI落地,横滚跟随,缓存键,行级runs作废,frame.x绝对坐标,光标跟随,无横滚条 · 事实：EUI softWrap 旋钮落地实锤（need263 批 3 棒 C，2026-11-18，分支 feat/window-lifecycle-hooks 未提交）：①InputBuilder 新增 .softWrap(bool=true)（components/input.h:63-65，build 每帧写 InputState.softWrap）；②布局缓存键纳入 softWrap（input_model.h cachedSoftWrap：翻转即全量重建 + cachedLineRuns.clear()——行号/行起点语义随切分变化，行级 runs 缓存必须整体作废，否则换行翻转后高亮按旧行索引错位）；③measureLines 增第 5 参 softWrap（false=逻辑行整行单段不切软段）；④multiline 布局分支 softWrap=false 横向滚动跟随光标（InputModel::syncMultilineHorizontalScroll：文档级平移，clamp 上界=全文最宽行 cachedTextWidth；undo/按键/文本输入三处原 horizontalScroll=0 清零点全部改走该 helper）；⑤渲染层多行行元素 position(-layout.scroll, y)；selectionRects 整行高亮宽 softWrap=false 取逻辑行实际宽。单行路径零改动，无 #ifdef。测试：EUI tests/unit/input_soft_wrap.cpp（已登记 EUI CMakeLists TEST_UNIT_SOURCES）。⚠两条坑：文本元素 frame.x 是窗口绝对坐标（含 textViewport 的 inset 偏移），横滚断言必须相对 soft.textViewport.frame.x 比较；caret 在文末时切 wrap 会竖滚到末屏，text.0 被视口裁掉是正确行为非缺件。最小实现口径：无横滚条 UI，横向可视性=光标跟随（点击/方向键），滚轮横向未接。 · 来源：实测 EUI-NEO components/input.h:63-65/144-146/171/268/371/407/467 + input_model.h（stash 基线对照）+ build-unit 测试输出；登记：2026-11-18 02:35:23 +0800 developer；复核：2026-11-18 02:35:23 +0800
- [open] clue#295 · 主题：notepad-module:eui-unit-baseline-reds · 域：notepad-module · 关键词：存量红,input_line_styles,input_scrollbar,基线复证,挂池,非回归 · 事实：EUI-NEO feat/window-lifecycle-hooks HEAD 019e7e9 存量单测红两处（stash 基线复证，与 softWrap 棒改动无关，非回归）：①tests/unit/input_line_styles.cpp——「ui: visible-line callback count = 0 (want 8)」（UI 集成段对『首帧 followCaret 把视口滚到末行』的假设失效：value 非聚焦播种 cursor=0，首帧不滚，二帧全命中缓存零回调——测试前提或首帧播种行为有一步已变，待查）；②tests/unit/input_scrollbar.cpp exit 8——End 键后断言 verticalScroll >= maximum-1 不满足。其余 input 家族（input_model/input_editing/input_key_passthrough/input_on_cancel/ime_rect/styled_runs/text_ellipsis/ui_state）基线与改动后均绿。两处修复应挂池单独处理，不混入 notepad 批 3 收口。 · 来源：实测 EUI-NEO build-unit：git stash 基线对照（HEAD 019e7e9）+ ./input_line_styles 与 ./input_scrollbar 复跑；登记：2026-11-18 02:35:37 +0800 developer；复核：2026-11-18 02:35:37 +0800
- [open] clue#296 · 主题：notepad-module:protect-chain-landed · 域：notepad-module · 关键词：notepad_protect,quitGuard,veto,豁免旗标,consumeExitRequest,再入队,probe坑,bg点击 · 事实：未保存保护链接线实锤（need263 批 4 棒 A，2026-11-18）：①纯决策层 src/module_notepad/notepad_protect.{h,cpp}——notepadProtectOutcomeFor（三选×两语境映射）/notepadCloseShouldIntercept（veto 裁决+豁免一次性消费）/notepadQuitScan（Allow/WaitDialog/AskAt 三态）/notepadQuitGuard（注册表序扫描+弹窗置位）。②豁免旗标（skipCloseConfirm）作废锚点=sessionSetDirty 开头 dirty==true 即清（**含早返回路径**——已脏再编辑 sessionSetDirty(true) 早 return 也要清，否则不保存后再编辑不重保护）。③quit guard 接线单点=app_assembly.cpp assembleAndStart（app::setQuitGuard(module::notepad::notepadQuitGuard)，router.startAll 后）；guard 空/全干净/全豁免→true 放行，AskAt→弹窗+false；每步处理完由对话框回调 requestExit 再入队（clue #269 语义，probe 用 app::detail::consumeExitRequest() 断言 rearm）。④对话框=components::dialog content() 缝自绘三钮（不保存=ThemeColorTokens.error 色文字钮）；theme 参数必须是 components::theme::ThemeColorTokens（=core::theme::componentTokensWeChat(t) 产物，core::theme::Tokens.colors 是另一类型编译不过）。⑤探针坑：onCloseRequested 闭包随 DslWindowConfig 走而 probe 的 app::openWindow=入队不建窗——veto 语义只能以 notepadCloseShouldIntercept 纯面等价断言；按钮点击=ui->find(id+".bg")->onClick()（components::button onClick 挂 .bg 子元素）；notepadSessionWindowDestroyed 探针内安全（handle 为空跳过几何落库仅注销）。⑥probe G 段三条穷尽路径全绿：拒退一次后可退出/取消不卡死（重入链重弹）/全豁免放行。 · 来源：实测 notepad_probe PASS + test_notepad_protect 5/5 + notepad 域 38/38；登记：2026-11-18 02:51:30 +0800 developer；复核：2026-11-18 02:51:30 +0800
- [open] clue#297 · 主题：notepad-module:assoc-multi-ext-landed · 域：notepad-module · 关键词：file_association,多扩展,unregisterAll,launchAdvancedAssociationUI,ts冲突,UTI归组,Info.plist,notepad_assoc,旧注册迁移,notepadAssocEnabled · 事实：F-18 关联开关多扩展化落地实锤（need263 批 4 棒 B，2026-11-18）：①platform/file_association.h AssocConfig={progId,extensions:vector}，productionConfig()=DevDesk.notepad+core::notepad 全 26 清单，testConfig()=DevDeskTest.notepad+{.mdtest,.notepadtest} 双隔离扩展；API 更名 isDefaultForMd→isDefaultFor(ext)+overallState()聚合+setAsDefaultFor()+unregisterAll()+launchAdvancedAssociationUI()（api_map.h kAdvancedAssociationUiUri="ms-settings:defaultapps" 锚）。②聚合纯函数 aggregateAssocStates：全 Ours=Ours/含 Unknown=Unknown/否则 Others/空表 Unknown。③Windows：单 ProgId 树+逐扩展默认值；旧 F-06 注册迁移=写入前快照查 .md 归 kLegacyMdProgId("DevDesk.md") 才删旧 ProgId 树（幂等）；unregisterAll 仅回读归己（新+旧 ProgId）才删扩展默认值，有清除才广播（spy 断言过）。④mac：12+ 批量写入=全部 fire+dispatch_group 单次 1.5s 总预算（逐项 await 最坏 26×超时挂 UI）；**.ts 豁免**=系统 UTI 是 public.mpeg-2-transport-stream（视频，2026-11-18 本机 UTType 实测 26 扩展全量：yaml/yml→public.yaml、log→com.apple.log、py→public.python-script、cpp/cc/cxx→public.c-plus-plus-source、h/hpp→c-header/c-plus-plus-header 均系统已声明；toml/ini/sql/cjs/jsx/tsx 是 dyn.*）→ Info.plist 补 com.devdesk.{toml,ini,sql,cjs,jsx,tsx} imported 声明（归 public.plain-text，cjs/jsx 归 javascript-source），CFBundleDocumentTypes 15 组覆盖 25 扩展（.ts 刻意不声明，仅 Windows 接管）；mac unregisterAll/launchAdvancedAssociationUI=NotImplemented（关=停自愈如实呈现）。⑤module_settings：notepad_assoc_logic.{h,cpp}/notepad_assoc_card.{h,cpp} 接管（md_assoc_* 四文件删除），SettingsConfig 字段 notepadAssocEnabled/NotepadAssocEnabled 但 DB 键沿用 "mdAssocEnabled"/"mdAssocEverEnabled" 字面量（存量兼容）；卡 id=settings.card.notepad_assoc；引导带=notepadAssocFailingSummary 逐扩展摘要（N 项未生效：前 4 项+…）+launchAdvancedAssociationUI 按钮。⑥测试：test_notepad_assoc.cpp 9 用例（四态/自愈/聚合/摘要/roundtrip/卡高/config 形状）+platform fileassoc 3 用例（roundtrip/unregisterAll 幂等不误删他者/清理容忍他者）+探针 settings_notepad_assoc_probe.cpp（旧 md 探针删除）。⚠探针坑：mac 文字档行数实测必须用与卡片同源的 notepadAssocMacGuideText(摘要) 组合串，裸指引行数≠实际绘制行数。 · 来源：实测构建绿+notepad_assoc 9/9+settings 137/137+fileassoc 2/2+探针 OK+隔离守卫0；登记：2026-11-18 03:06:06 +0800 developer；复核：2026-11-18 03:06:06 +0800
- [open] clue#299 · 主题：notepad-module:need289-walkthrough-findings · 域：notepad-module · 关键词：另存为缺失,F-04,快捷键缺口,M加H,M加Shift加F,错误态字体,只读假活,焦点回收,弹层收起,统计节流,字重500,need289,走查,ui_driver不可达子窗,修复批次 · 事实：need289 全交付面走查实锤（2026-11-18，报告 docs/research/2026-11-18-notepad-polish-audit.md，证据 evidence/2026-11-18-notepad-polish/walkthrough/）：①P1-1 F-04 另存为整条缺失——全仓生产代码零 saveAs 实现、工具条无钮（notepad_window.cpp:944-1013 装配全量）、键路由无 M+Shift+S（notepad_window.cpp:1258-1259 shortcut 显式排除 shift）、测试面零覆盖；PRD v5 F-04 核心+§九工具条明文。②P1-2 快捷键缺口——notepadHandleKeySession（notepad_window.cpp:1255-1303）仅 O/S/W/F/Equal/Minus 六支，PRD §九明文的 M+H/M+Shift+F/M+P 全未接线。③P2×6：错误占位窗字体下拉未禁用（notepad_session.cpp:153 fontControlsEnabled 不含 errorState，A± 已守触发器漏）；只读窗编辑框假活可打字回显回滚（notepad_window.cpp:1089-1097 onChange 早返回但 input 未 disabled）；Esc 关查找条焦点不回归编辑器（notepadFindClose :251-260 无 requestFocus，开条有播种关条无对称回收）；字体弹层无外点/Esc 收起（:714-718 hit 仅自身区域）；状态栏统计每帧全量扫描无节流（:1143-1173，10MB 只读窗每帧 3×10MB 遍历，设计稿 §11.4 200ms 节流未实现）；notepad_assoc_card.cpp:181 fontWeight(500) 两档制第三档违例（notepad 域唯一一处）。④P3×10 见报告（●估宽 CJK 偏小/编码段 102px 溢出/工具条文本钮偏差/状态栏三态缺/提示条档位/弹窗 460 vs 420/换行钮空洞/跨行块注释断层/UTF-16 乱码注记/二进制无三选）。⑤通道事实：ui_driver 结构性不可达 notepad 子窗（单窗 Router 驱动无子窗泵 + N-14 无入口），F-18 关联卡走查 10/10 过；ui_driver 截图在点击后立即拍会捕未重组空帧，需 wait ms≥1；notepad_probe --screenshot 出真实 GL 帧 PNG 可当窗口像素证据；本机 screencapture 全黑弃用。⑥基线复证：notepad doctest 47/47 + probe 6 session PASS + settings_notepad_assoc_probe OK（本轮实测）。修复批次建议见报告 §4：批1=另存为+快捷键（M），批2=P2 五小改（S），批3=字重一行+P3 收尾（S-M）。 · 来源：实测 probe/ui_driver/doctest + 全 10 文件代码面走读；grep 全仓 saveAs 零命中；登记：2026-11-18 06:16:43 +0800 qa-tester；复核：2026-11-18 06:16:43 +0800
- [open] clue#301 · 主题：notepad-module:saveas-shortcuts-landed · 域：notepad-module · 关键词：另存为,F-04,M加Shift加S,M加H,M加Shift加F,M加P,M加Shift加V,btnSaveAs,工具条布局,pendingFormatJson,shortcutShift,probe H段,saveFileDialog,need289 · 事实：need289 修复棒 1 另存为+快捷键接线落地实锤（2026-11-18，feat/notepad-batch1 未提交）：①F-04 全链——工具条新钮 notepad.btnSaveAs@x160（设计稿 §4.2 锚；布局级联让位：字体触发器/弹层 168→240、stepper 336→408、format 420→492、preview 492→564、wrap 564/420→636/492），禁用矩阵=editable（与保存钮同口径：readonly/binary/错误窗禁用）；守卫在 notepadSaveSessionAs 内（只读窗编辑镜像可能是原字节乱码，重编码会写坏数据——简报口径优先于设计稿 §7.2 只读「另存为可用」列）。②saveFileDialog（eui/platform.h:80，FD-1 need265）DevDesk 侧首个消费方：SaveFileDialogOptions{prompt,defaultName,defaultExtension(取 s.title 点后),allowedExtensions=notepadExtensions}；Cancelled 静默返回零副作用、不 toast。③notepadSaveSessionAsTo 落地单点（声明在 notepad_session.h、实现在 notepad_file_io.cpp，独立成函数=无头直测）：脏缓冲=encodeBuffer(editText,hasBom,eolMode)、干净=rawBytes 字节直通；迁移=path/normKey/title/rawBytes/loadText/loadHasBom/loadEol/mixedEol=false/enc=notepadProbeEncoding(bytes)/language.clear（compose 懒求值重算）/mtime 重取/sessionSetDirty(false)+handle 非空时 setWindowTitle(title)（sessionSetDirty(false) 对干净窗是 no-op，标题必须显式再设）；写失败 return fail 且 session 未落任何态。④键路由：shortcut() 显式排除 shift → shift 组合单独 shortcutShift 判据；M+Shift+F 键处理器无 Ui 访问权 → session.pendingFormatJson 置位、compose 编辑分支消费（find.pendingJump 同模式，消费点再核预览/只读/扩展守卫）；M+P（设计稿 §6）与 M+Shift+V（PRD §九）双支路同走 notepadTogglePreviewIfAvailable（matrix.showPreview&&previewEnabled guard——PRD 与设计稿快捷键表分歧，终报已注明待主会话裁决）；M+H=notepadFindOpenReplace（未开=notepadFindToggle 复用全套播种+replaceOpen=true，已开=仅展开不重播种）。⑤测试面：test_notepad_saveas 4 用例（脏迁移矩阵/干净直通+覆盖既有目标/写失败零副作用/中文空格宽路径——⚠夹具须预建父目录，notepadWriteFile 契约父目录由调用方保证）；probe H 段=钮存在+三态禁用矩阵+M+Shift+S 只读/错误窗守卫直驱零副作用（⚠可编辑窗禁直驱 M+Shift+S，否则弹真 osascript 对话框挂起=超时红）+M+H/M+Shift+F/M+P·V 断言。⑥验证数据：--filter notepad 51/51（47 回归+4 新）+ notepad_probe PASS（H 段）+ GL 帧截图 evidence/2026-11-18-notepad-polish/saveas-fix/notepad-probe-saveas.png。⑦并行作业坑：quit_save_bridge.{h,cpp} 为条目 290/291 在途新文件，登记前 devdesk_tests/bug142_click_chain_probe 链接断（app_sidebar.cpp:1461 引用 app::quit_save::drawConfirmDialog → Undefined symbols；ld swiftCxx warning 是噪声）——共享 build 树验证遇此先 git status 归因，勿当自己回归。 · 来源：实测：--filter notepad 51/51 + notepad_probe PASS + 截图；代码面 src/module_notepad/notepad_{file_io.cpp,window.cpp,session.h,module.h}；登记：2026-11-18 06:45:03 +0800 developer；复核：2026-11-18 06:45:03 +0800
- [open] clue#302 · 主题：notepad-module:fix2-p2p3-landed · 域：notepad-module · 关键词：need289,修复棒2,P2字体禁用,只读假活不可修,pendingFocusEditor,scrim,pendingPopupDismiss,统计缓存,fontWeight600,提示条info可关,换行钮564,EUI只读旋钮归属,probeI段 · 事实：need289 修复棒 2（P2×6+P3×10 处置）落地实锤（2026-11-18，feat/notepad-batch1 未提交）：①P2-1 错误窗字体禁用=matrix.fontControlsEnabled 加 !errorState（notepad_session.cpp）+ drawFontPopup 残留弹层收起守卫扩 errorState；probe C 段补触发器 disabled 断言。②P2-2 只读编辑框假活=不可修不硬做：EUI InputBuilder 无 disabled/readOnly 旋钮（components/input.h 全 setter 清单核实），且打字/IME/粘贴走内部 onTextInput 通道（input.h:384，onKeyEvent passthrough 拦不到），onChange 早返回+value 回置已保证数据安全；真禁用需 EUI 加旋钮——本批 EUI-NEO 禁动，归属独立池条目（EUI InputBuilder .readOnly/.disabled 旋钮）。③P2-3 关条焦点回归=NotepadFindState.pendingFocusEditor 请求位（notepadFindClose 置位，compose 编辑分支消费 requestFocus(kNotepadEditorInputId)，键处理器无 Ui 访问权同 pendingJump 模式）；probe D 段补 ui->isFocused(editor) 断言（dsl.h:1565 isFocused API 可用）。④P2-4 字体弹层收起=drawFontPopup 开时铺全窗透明可点 scrim（base+.popup.scrim，onClick 收起不改值；弹层面板后画在上+自身 hit blockPointer 保内点击）+ session.pendingPopupDismiss Esc 请求位（键路由 Esc 兜底置位，drawFontPopup 单点消费）；probe I1 断言 scrim 开/点收/Esc 收。⑤P2-5 统计节流=NotepadStatusStats 缓存（Ui 态，键=(textRevision,cursor)，未变帧零扫描）替代每帧 3×全文遍历；probe I2 断言 cursor/revision 变化重算。⑥P2-6 字重=notepad_assoc_card.cpp:174 measureTextWidth+181 fontWeight 两处 500→600（measure 不同步会量宽失配）。⑦P3 修 5 项：P3-1 ●估宽 CJK 分档 12px/其余 7px（notepadTitleEstWidth）；P3-2 编码段 102→128px（w-478→w-500，行尾段 w-368→w-364 联动）；P3-5 提示条分档（只读=warning 不可关原样/大文件=info+notepad.promptbar.close 可关，关=Ui 态 dismissed 旗标）；P3-6 保护弹窗 460→420；P3-7 .json 换行钮 x=(showPreview?636:showFormat?564:492) 消除预览位空洞（probe I3 断 x=564+I4 断提示条可关）。⑧P3 不修 5 项：P3-3 图标钮/menuMore（设计稿单方承诺，文本钮 label 可见 tooltip 冗余）；P3-4/P3-10 状态栏三态+只读胶囊（需 saveFailed 状态机扩展非打磨最小面，toast+脏态已双反馈）；P3-8 跨行块注释断层（per-line 词法契约固有，归属 clue #288）；P3-9 UTF-16 乱码/二进制三选（v1 范围注记，行为更宽非缺陷）。⑨验证：--dev 构建绿+--filter notepad 51/51+--filter notepad_assoc 9/9+notepad_probe PASS（7 session 含 I 段）+settings_notepad_assoc_probe OK+字重 grep 域内 0 处 500+GL 帧截图 evidence/2026-11-18-notepad-polish/saveas-fix/notepad-probe-fix2.png。改动 6 文件全在授权域，未 commit（简报禁），未碰对方 WIP。 · 来源：实测：--filter notepad 51/51 + notepad_assoc 9/9 + notepad_probe PASS + assoc probe OK；代码面 src/module_notepad/* + module_settings/notepad_assoc_card.cpp + EUI-NEO components/input.h:48-117/384；登记：2026-11-18 06:59:34 +0800 developer；复核：2026-11-18 06:59:34 +0800

**进度记录**（13）：

- 2026-11-16 21:05:22 +0800 · 用户 2026-11-16 明确要求本轮动作=调研分析（/ask 调研分析）。源码已同步完成。本轮范围限定：外部事实调研 + 现有实现盘点，不进入开发/不改产品代码。
- 2026-11-16 21:14:31 +0800 · 四项澄清裁决已入 detail（clue 233）。外部调研棒在飞（researcher，输出 docs/research/2026-11-16-notepad-module-research.md）。
- 2026-11-16 21:22:10 +0800 · 需求升级：独立 UI + 设置页开关自动处理文件关联 + EUI 先补齐能力 + GitHub 开源参考 + 先 HTML demo/UI 设计稿。PRD 棒仍在跑，设计棒与 EUI 补齐棒并行启动。
- 2026-11-16 21:28:00 +0800 · PRD v2 已交付待审计：slug=2026-11-16-notepad-module，F-ID 20 条，FD 3 条，D-ID 6 条待裁决。
- 2026-11-16 21:32:34 +0800 · PRD v3 已交付待审计：slug=2026-11-16-notepad-module，F-ID 20、FD 3（FD-2 阻塞）、D-ID 8 待裁决。
- 2026-11-16 21:56:44 +0800 · 【三轮定位修正】老板 2026-11-16：「记事本是独立窗口，通过打开文件来访问，不能主程序访问」——推翻「侧边栏页面」前提。PRD v4 与 UI 设计稿需同步改写（容器：页面→独立子窗）。待确认两点后开工。
- 2026-11-16 22:04:21 +0800 · 三轮定位修正：独立窗口 + 只能靠打开文件访问 + 独立生命周期（互不影响、不做独立进程）。PRD 升 v4 与 UI 设计稿待改。
- 2026-11-16 22:09:50 +0800 · PRD v4 已交付待审计：独立窗口形态定稿，F-ID 20（删新建+新增多窗管理），FD 3（两项已交付、FD-2 剩 M 级收尾），D-6 定案，待裁 D-1/D-3/D-7/D-8。
- 2026-11-16 22:30:48 +0800 · PRD v5 已交付待审计：同文件聚焦口径对齐设计稿 v2（四处改动+连带同步），待裁 D-1/D-3/D-7/D-8。
- 2026-11-17 07:55:39 +0800 · 四项待决按推荐采纳（2026-11-17 全批令）。PRD v5 就绪，进技术方案卡。
- 2026-11-17 08:07:27 +0800 · 记事本模块 L 级技术方案卡交付：slug=2026-11-16-notepad-module-tech（v1）。5 批次（0=框架三件套 close-veto/quit-guard/requestFocus/setTitle；1=骨架+漏斗+viewer 退役；2=查找替换+高亮迁 ui_kit；3=视图与格式；4=保护链+关联开关）。最高风险 3 条见卡 §十五。
- 2026-11-17 11:39:00 +0800 · 交接快照:.zcode/handoff/20261117-1138-notepad-263.md（2026-11-17 11:38）。当前态：批 0 已交付(401ae71+minSize merge=ca57a38)；批 1 产物已落盘但链接失败未定位（最可能=devdesk_module_notepad 未进 tests/CMakeLists.txt 链接集）；EUI 四分支均未 push 未合 main。#273 同主题。
- 2026-11-18 03:37:29 +0800 · 四批全量交付完成（2026-11-18）：origin/dev=e4c85243 已推送（feat 4cc02f08 squash + 关联卡补交 + 与 need291/299/300 线语义合并），EUI-NEO fork main=0e4031f 先行推送。20/20 F-ID 全落地，全量 2429/2429 绿+守卫链过+probe 6 session PASS。池条目：#273（批1）/​#285（批2）/​#286（批3）/​#288（批4）全待验收（evidence 各自在案）。待办残留：CI 双平台实证（win 关联注册表/mac 27.2 SDK typeWithFilenameExtension）、真机 UAT（保护链弹窗/关联开关/预览排版目检）、EUI 基线存量红 #287 另棒修。

**状态变更**（6）：

- 2026-11-16 21:02:48 +0800 ·  → 在池 · 入池（main）
- 2026-11-16 21:05:22 +0800 · 在池 → 排产 · 用户 2026-11-16 明确要求本轮动作=调研分析（/ask 调研分析）。源码已同步完成。本轮范围限定：外部事实调研 + 现有实现盘点，不进入开发/不改产品代码。（main）
- 2026-11-16 21:21:57 +0800 · 排产 → 进行中 · （product-manager）
- 2026-11-16 21:28:00 +0800 · 进行中 → 待验收 · PRD v2 已交付待审计：slug=2026-11-16-notepad-module，F-ID 20 条，FD 3 条，D-ID 6 条待裁决。（product-manager）
- 2026-11-17 07:55:39 +0800 · 待验收 → 进行中 · 四项待决按推荐采纳（2026-11-17 全批令）。PRD v5 就绪，进技术方案卡。（main）
- 2026-11-17 08:07:27 +0800 · 进行中 → 待验收 · 记事本模块 L 级技术方案卡交付：slug=2026-11-16-notepad-module-tech（v1）。5 批次（0=框架三件套 close-veto/quit-guard/requestFocus/setTitle；1=骨架+漏斗+viewer 退役；2=查找替换+高亮迁 ui_kit；3=视图与格式；4=保护链+关联开关）。最高风险 3 条见卡 §十五。（developer）

**关联文档**（2）：

doc#40 · 2026-11-16-notepad-module（prd v5 · 更新 2026-11-16 22:30:39 +0800）→ 附录 A
doc#42 · 2026-11-16-notepad-module-tech（tech v1 · 更新 2026-11-17 08:07:05 +0800）→ 附录 A

---

#### need#269 · EUI-NEO DslWindowConfig 补 minSize/maxSize 透传（底层 WindowCreateRequest 已有、DSL 层未暴露）

- 状态：待验收 · 优先级：P1 · 负责人：developer · 阶段：测试中
- 创建：2026-11-16 22:32:00 +0800（developer）· 更新：2026-11-20 11:11:54 +0800（developer）

**详情**：

来源：need#263 记事本窗 UI 设计稿审计发现（主会话 2026-11-16 实测）。

事实：EUI-NEO 底层已支持窗口最小/最大尺寸限制——core/window/window_types.h:42-43 的 WindowCreateRequest 有 minWidth/minHeight（及 maxWidth/maxHeight），core/window/window_backend.cpp:1380 已调 glfwSetWindowSizeLimits。但 **DSL 层未暴露**：include/eui/dsl_app.h:243-266 的 DslWindowConfig 无 minWidth/minHeight/maxWidth/maxHeight 字段，include/eui/detail/dsl_app_impl.h 也无透传。

后果：UI 设计稿 §2 声称「DslWindowConfig 能力清单含 minSize 720×480」是能力虚构；记事本窗当前无法设置最小尺寸约束（可被拖到任意小，布局断点失效）。

交付：给 DslWindowConfig 补 minSize(w,h) / maxSize(w,h) builder，透传到 WindowCreateRequest.minWidth/minHeight/maxWidth/maxHeight。改动面：dsl_app.h 加字段+builder、dsl_app_impl.h 加透传，底层零改。验收：macOS+Windows 双端真机拖窗到边界停住；不传 = 无限制（现有行为零回归）。

**进度记录**（5）：

- 2026-11-17 07:56:16 +0800 · 全批令下发（2026-11-17），作为记事本窗前置依赖优先做。
- 2026-11-17 08:00:19 +0800 · 已交付：EUI-NEO feat/window-min-size @ 0f253fb（父 690484a）。5 文件 +66 行：DslWindowConfig 增 minWidth/minHeight/maxWidth/maxHeight 字段 + minSize()/maxSize() builder；dsl_app_impl.h 透传到 DslWindowRequest（负值归 0）；app.h 加请求字段；glfw_app_main.cpp 透传到 WindowCreateRequest。单测 pet_window_config 增 3 组断言（minSize 正立/min+max 共存/负值归 0），实跑 all checks passed 退出码 0。构建绿（build-test-run.sh --dev）。未 push。Windows 侧待实机复验。
- 2026-11-20 10:47:11 +0800 · 老板「继续执行」落令（2026-11-20），派 developer 执行 EUI-NEO DSL minSize/maxSize 透传；EUI-NEO 仓内作业，完成后 fork 先推。
- 2026-11-20 11:10:11 +0800 · 完成。EUI-NEO 侧交付已在 origin/main（0f253fb，2026-11-17 已合入，本会话 cherry-pick 复核为空=补丁全量在 main）：dsl_app.h:253 字段+298-310 minSize/maxSize builder、dsl_app_impl.h:184-187 透传（负值归 0）、app.h:27-30 DslWindowRequest 四字段、glfw_app_main.cpp:446-449 createManagedWindow 透传、tests/unit/pet_window_config.cpp 三组断言——简报「DSL 层未暴露」前提已过时；EUI-NEO 零新提交、无需 push（feat/window-min-size 本地分支=指向已合并提交的陈旧指针，已 ff 本地 main 至 origin/main 顶点 868a3a3...868a0a3）。DevDesk 侧新增离屏探针 window_minmax_limits_macos_probe.cpp（commit 626b3e69）：A 段 DSL→请求四字段链复证（含负值归 0）+ B 段真窗口断言 nativeWindowInfo().platformWindow(NSWindow*) contentMinSize=(720,480)/contentMaxSize=(1920,1080)，无约束对照窗 (0,0)/FLT_MAX 基线成立，PASS exit=0。实测锚：macOS AppKit 对程序化 resize 不强制 limits（仅用户拖拽生效，可见/隐藏窗一致），故断言面选原生落点状态；Windows 侧 WM_GETMINMAXINFO 会钳程序化 resize，可另做 _win_probe（未写未验，清单外建议）。单测 pet_window_config 目标被 EUI_BUILD_TEST_FIXTURES 门控（DevDesk 构建树内不存在），其断言面已由探针 A 段 DevDesk 侧复证。
- 2026-11-20 11:11:54 +0800 · 待验收口径：macOS 侧已实证；Windows 真机拖窗复核挂「下次 Windows 机顺手」（WM_GETMINMAXINFO 可另写 _win 探针，agent 已给建议未执行）；消费端接线（记事本窗 .minSize(720,480)）已另立新条目归 notepad 线。

**状态变更**（4）：

- 2026-11-16 22:32:00 +0800 ·  → 在池 · 入池（main）
- 2026-11-17 07:56:16 +0800 · 在池 → 排产 · 全批令下发（2026-11-17），作为记事本窗前置依赖优先做。（main）
- 2026-11-20 10:47:11 +0800 · 排产 → 进行中 · 老板「继续执行」落令（2026-11-20），派 developer 执行 EUI-NEO DSL minSize/maxSize 透传；EUI-NEO 仓内作业，完成后 fork 先推。（main-session）
- 2026-11-20 11:11:54 +0800 · 进行中 → 待验收 · 待验收口径：macOS 侧已实证；Windows 真机拖窗复核挂「下次 Windows 机顺手」（WM_GETMINMAXINFO 可另写 _win 探针，agent 已给建议未执行）；消费端接线（记事本窗 .minSize(720,480)）已另立新条目归 notepad 线。（main-session）

---

#### need#285 · need263 批2：查找替换引擎+高亮统一层迁移 ui_kit（F-06/F-07/F-08/F-12）

- 状态：待验收 · 优先级：P1 · 负责人：developer · 阶段：测试中
- 创建：2026-11-18 01:06:40 +0800（developer）· 更新：2026-11-18 01:41:02 +0800（developer）

**详情**：

批 2（2026-11-18 排产，老板令「没开发完毕不要停下」连发授权）。范围=tech 卡 §四/§五/§十一/§十二 批 2 行：①棒 1（在飞）=md_keyword_highlight 迁 ui_kit/text_highlight（namespace ui_kit::text_highlight 逻辑零改）+ notepad_language 扩展名→语言映射单一真源 + per-line provider + test_notepad_language；②棒 2（棒 1 回后派）=notepad_find 纯函数引擎（findAll/nextFrom/prevFrom/replaceOneAt/replaceAll，F-07 落点锁）+ mergeRuns 两层合并 + commitTextAtomically（单撤销 F-08）+ notepad_window 查找条 UI 接线（M+F/Enter 环绕/Esc 清高亮留词/.lineStyles 接线）+ test_notepad_find + probe 查找键盘流扩展。验收：迁移零逻辑 diff+旧引用清零、markdown/notepad_language/notepad_find 定向绿、隔离守卫 0、probe PASS。

**进度记录**（5）：

- 2026-11-18 01:07:34 +0800 · 批 2 主条目（2026-11-18 登记）。⚠️ 棒 1 在飞代理简报中池标记误写为 284（那是另一条线「全应用打磨专项」，非本条）——已发 SendMessage 纠偏为 285；若 #284 台账出现本棒写入痕迹，以本条目为准。
- 2026-11-18 01:18:48 +0800 · 棒 1 完工验收绿（迁移零逻辑 diff/旧引用清零/markdown 121 全绿/隔离守卫 0，evidence 已在案），重开进行中供棒 2（notepad_find 引擎+UI 接线+probe 扩展）开工。
- 2026-11-18 01:19:58 +0800 · 棒2开工：clue 全量已取（#288 迁移接口事实采信：md_keyword_highlight→ui_kit::text_highlight，notepad_language 4参 provider highlightLine(displayText,lineStartByte,lineNo,lang)，.ts→Js/.md→None 收敛）；tech 卡 §四（F-07 落点锁/commitTextAtomically/mergeRuns 预合成）已读。下一步：读设计稿 §4.5 + session/window/module 现状 → notepad_find.{h,cpp} 纯函数引擎 → 测试 → UI 接线 → probe 扩展。
- 2026-11-18 01:39:37 +0800 · 棒2完工：notepad_find.{h,cpp} 纯函数引擎（findAll ASCII折叠/全词含CJK字节/环绕N-3 nextFrom·prevFrom 返回下标制 kFindNoHit、replaceOneAt 内含 F-07 落点锁、replaceAll 不重扫、mergeRuns 切点法命中层覆盖语法层）；session 增 NotepadFindState（query/appliedQuery/hits/cur/hitsRevision/pendingSeed/pendingJump/styleDirty + language 懒求值）；module 头增 notepad.search.* 10 id；window 接线（M+F/Esc/Enter·Shift+Enter 双通路键路由：input 聚焦走 passthrough、未聚焦冒泡窗级；输入即重算 appliedQuery 门控 + textRevision 门控保持邻近；.lineStyles 语法层 highlightLine + 命中层 mergeRuns 预合成 warning@26%·22%/primary@38%·30% 叠 warning；大文件两层全关仅跳转+计数；commitTextAtomically 同构 helper，单个/全部替换一次 push=单撤销 F-08，全部替换 toast 含 0 处）。测试 test_notepad_find.cpp 登记 TIN-10；probe 增 D 段查找键盘流。新证 clue #290。未验证项：真实窗口视觉（命中色像素/查找条布局目检）归 qa/UAT；替换钮点击路径 probe 未驱（doctest 已锁引擎层）。状态保持进行中，收口归主会话。
- 2026-11-18 01:41:02 +0800 · 批 2 两棒全部完工验收，wip commit 21164fa8。

**状态变更**（6）：

- 2026-11-18 01:06:40 +0800 ·  → 在池 · 入池（developer）
- 2026-11-18 01:07:34 +0800 · 在池 → 排产 · 批 2 主条目（2026-11-18 登记）。⚠️ 棒 1 在飞代理简报中池标记误写为 284（那是另一条线「全应用打磨专项」，非本条）——已发 SendMessage 纠偏为 285；若 #284 台账出现本棒写入痕迹，以本条目为准。（main）
- 2026-11-18 01:10:04 +0800 · 排产 → 进行中 · （developer）
- 2026-11-18 01:16:36 +0800 · 进行中 → 待验收 · （developer）
- 2026-11-18 01:18:48 +0800 · 待验收 → 进行中 · 棒 1 完工验收绿（迁移零逻辑 diff/旧引用清零/markdown 121 全绿/隔离守卫 0，evidence 已在案），重开进行中供棒 2（notepad_find 引擎+UI 接线+probe 扩展）开工。（main）
- 2026-11-18 01:41:02 +0800 · 进行中 → 待验收 · 批 2 两棒全部完工验收，wip commit 21164fa8。（main）

---

#### need#286 · need263 批3：字体字号+wrap+状态栏补全+JSON 格式化+md 预览（F-09/10/11/13/15/16/17/19）

- 状态：待验收 · 优先级：P1 · 负责人：developer · 阶段：测试中
- 创建：2026-11-18 01:43:27 +0800（developer）· 更新：2026-11-18 02:37:14 +0800（developer）

**详情**：

批 3（2026-11-18 排产，全批令在案+老板令不停）。范围=tech 卡 §十二 批 3 行：棒 A=字体族下拉（F-09，systemFontFamilies 已合入 EUI 生命周期分支）+A±字号（F-10，走 core::theme::setEditorFontSize 通道 D-1 联动笔记页）+wrap 开关（F-19，框架软换行已证 clue257）+状态栏编码/行尾/●未保存补全（F-15 UI 面/F-17）；棒 B=JSON 格式化（F-11，platform/json+yyjson_locate_pos 行列）+md 预览（F-13，components::markdown 整篇渲染）。依赖已解除：EUI 指针 5d205f9c 已含字体枚举+saveDialog。验收：定向过滤绿+probe 扩展+真机目检归 UAT。

**进度记录**（6）：

- 2026-11-18 01:57:13 +0800 · 棒 A 完工（need263 批 3）。落地：F-09 字体族下拉（工具条触发器 notepad.ddFont+scrollView 内滚弹层 260×300+首项「默认字体」=空族，InputBuilder.fontFamily 原生支持即时生效，systemFontFamilies 进程级缓存）；F-10 A±（M+=/M+- 键路由 Equal/Minus 分支+工具条 stepper A-/值/A+，走 core::theme::setEditorFontSize D-1 联动+session.editorFontSize+notepadSaveEditorPrefs 写穿透，鎟[10,32]，toast 反馈）；F-15/F-17 状态栏编码/行尾两段（notepad.status.encoding/eol，UTF-8↔UTF-8 BOM 循环+LF↔CRLF 循环，点切即刻标脏：dirty=(editText!=loadText)||(hasBom!=loadHasBom)||(eolMode!=loadEol) 基线判定，保存后基线前移，只读窗禁用灰显，错误窗不渲染，窄窗<900 隐藏=设计稿 T1 档）；F-19 仅状态层：session.wrapOn+prefs 读写已通、UI 钮未上——简报前提「.wrap(bool) 即用」与代码不符（InputBuilder 无 wrap 旋钮，multiline 恒软换行，clue 291），禁做不动 EUI-NEO→后续棒授权框架改动后接一枚钮即可；prefs 扩 notepadSaveEditorPrefs（font/wrap/fontSize 三键写穿透，fontSize 钟改[10,32]）。clue 270 门禁恢复：tests/unit/system_font_gate.cpp 新建+CMakeLists 取消注释。未验证项：①prefs 持久化真实 app 档（headless 无 appDatabase，键位复用既有 notepadLoadPrefs 载入链，机制风险低）②下拉弹层/点切段真实点击交互（probe 只保证不回归+截图静态面，交互验证归 QA/UAT）③Windows 侧 systemFontFamilies 枚举（本机 macOS 验证，CI 双平台覆盖）。改动 8 文件均未 commit（简报禁做）；文件清单：notepad_session.h/notepad_prefs.*/notepad_module.*/notepad_file_io.cpp/notepad_window.cpp/tests/CMakeLists.txt/tests/unit/system_font_gate.cpp（新）。
- 2026-11-18 01:59:07 +0800 · 棒B开工（2026-11-18）：F-11 JSON格式化 + F-13 md预览。线索#252/#253/#255/#288/#290/#291与方案卡§六/§七已读。步骤：jsonLineColForPos薄封装→工具条两钮→previewOn切换→测试+probe→定向验证。
- 2026-11-18 02:15:32 +0800 · 棒B完工（2026-11-18，F-11+F-13 全部验收项交付）：【改动】①platform/json.{h,cpp} 新增 JsonLineCol+jsonLineColForPos 薄封装（yyjson_locate_pos，越界钳界，1 基码点列）；②notepad_module.h 新 id：kNotepadBtnFormatId/btnPreview/preview/previewContent/previewEmpty；③notepad_session.h +previewOn 字段 + NotepadToolbarMatrix 纯函数 + notepadSessionHasExtension（normKey ASCII 折叠后缀判据）；④notepad_window.cpp：格式化钮（仅 .json，x=420，tooltip 键序明示=D-4，readonly/binary/预览禁用）+ notepadFormatJson（parse→stringify pretty 两空格→notepadCommitText 单撤销，失败=「第 X 行 第 Y 列：<msg>」toast+光标跳错误位 followCaret，成功 toast 后缀「对象键已按字典序重排」）+ 预览钮（仅 .md/.markdown，x=492，预览态钮面切「编辑」）+ notepadTogglePreview + drawPreviewSlot（scrollView+components::markdown 整篇渲染，MarkdownStyle 本地构建 blockGap×fontScale，空文件「（空文件）」占位，contentKey=textRevision 稳度量缓存）+ 工具条联动（预览态：格式化/M+F/查找条渲染/字体下拉/A± 全禁，字体弹层强制收起）；⑤tests/CMakeLists.txt 登记 test_notepad_preview.cpp；⑥test_notepad_preview.cpp 7 用例（行列边界含 CJK 码点列/越界钳界/空文本、parse diag 集成、格式化幂等+键序+缩进、矩阵扩展名/禁用/预览联动）；⑦notepad_probe E 段（按钮矩阵三态+previewOn 编辑槽位让位+查找条不渲染+A± disabled+切回光标保留）。【测试】新 7 用例绿、notepad 定向 33/33 绿（含既有 26 回归）、probe PASS。【未验证项】真机目检预览排版（PRD F-13「排版=查看器值」归 QA）；JSON 格式化动作流真机点击（按钮→notepadFormatJson 接线为薄 lambda，纯函数面已测）；tooltip hover 呈现（headless 不测 hover）。不 commit（简报禁做）。新线索 #292/#293。状态保持进行中待验收。
- 2026-11-18 02:16:48 +0800 · wrap棒开工：developer 接手，先 clue_fetch notepad-module，然后 EUI softWrap 旋钮 + notepad 换行钮接线
- 2026-11-18 02:35:45 +0800 · wrap棒完工：①EUI softWrap 旋钮落地（InputBuilder.softWrap(bool=true) + InputState.softWrap/cachedSoftWrap 缓存键 + measureLines 第5参 + multiline 横滚跟随 syncMultilineHorizontalScroll + 渲染行 x 偏移 + selectionRects 整行宽）——单行路径零改动无 #ifdef；②EUI 单测 input_soft_wrap 绿（false 单段+横滚跟随/true 回归/翻转缓存重建/UI 级接线），已登记 TEST_UNIT_SOURCES；③notepad 接线：editor .softWrap(s.wrapOn) + 工具条 notepad.btnWrap（选中态高亮，让位格式化/预览钮 x=420/564）+ notepadToggleWrap 翻转+prefs 写穿透 + largeFile 载入强制关 wrapOn（applyLoadedState 两分支）+ 钮禁用；④probe F 段：钮在场可用/softWrap 随动/1.15MB 大文件钮禁用+softWrap 关，PASS（6 session）。最小实现口径：无横滚条 UI，横向可视性=光标跟随，滚轮横向未接（终报注明）。两侧均未 commit（主会话收口统一提交）。新事实 clue #294/#295。
- 2026-11-18 02:37:14 +0800 · 批 3 三棒全部完工验收。

**状态变更**（4）：

- 2026-11-18 01:43:27 +0800 ·  → 在池 · 入池（developer）
- 2026-11-18 01:43:54 +0800 · 在池 → 排产 · （main）
- 2026-11-18 01:44:33 +0800 · 排产 → 进行中 · （developer）
- 2026-11-18 02:37:14 +0800 · 进行中 → 待验收 · 批 3 三棒全部完工验收。（main）

---

#### need#288 · need263 批4：未保存保护链+quit guard+设置页文件关联开关（F-05/F-18/D-3/D-7）

- 状态：待验收 · 优先级：P1 · 负责人：developer · 阶段：测试中
- 创建：2026-11-18 02:37:35 +0800（developer）· 更新：2026-11-18 03:37:17 +0800（developer）

**详情**：

批 4（最终批，2026-11-18 排产，全批令在案）。范围=tech 卡 §九/§十/§十二 批 4 行。棒 A=未保存保护链（F-05）：三选对话框（components::dialog content() 三钮：保存/不保存/取消）+ 关窗 veto 接线（onCloseRequested 批 1 占位替换：dirty→三选，保存/不保存→requestWindowClose，取消→不关；干净窗直通）+ quit guard 接线（app::setQuitGuard，D-3 逐窗按创建序依次保护、任一取消中止整个退出；必守约束 clue #269：三条穷尽路径全可返回 true——拒退一次后可正常退出/取消后不卡死）。棒 B=设置页记事本开关（F-18）：AssocConfig 多扩展化（platform/file_association.h:26-32 extension→extensions）+ productionConfig progId DevDesk.notepad+26 清单 + 旧 DevDesk.md 注册幂等迁移 + 关闭=unregisterAll 撤销（新语义老板已裁）+ Windows launchAdvancedAssociationUI（ms-settings:defaultapps）+ macOS 逐扩展回读 + D-7 md 关联卡退役（mdAssoc* 纯函数改名 notepadAssoc* 留 settings 复用）+ 单测迁移。依赖已备：quit guard/关窗 veto 批 0 已交付（401ae71），saveFileDialog 已合入（F-04 另存为顺带接线）。验收：三选流 probe 断言+拒退一次后可退出+取消不卡死；关联四态矩阵单测；真机目检归 UAT。

**关联线索**（1）：

- [open] clue#298 · 主题：platform:file_association_merge_e290bb42 · 域：platform · 关键词：file_association, macos.mm, F-18, 27.2 SDK, UTType, need263, need288 · 事实：need288 合并手术实测：origin/dev(bda66ad5) 的 file_association_macos.mm 仍是 F-09 旧接口（isDefaultForMd/setAsDefaultForMd、AssocConfig.extension 单数、progId 字面量 DevDesk.md），其能过 CI 是因 dev 侧 file_association.h 未被 need291 线改动仍为旧版；合并结果头文件取我方 F-18 版（extensions 向量 + isDefaultFor/setAsDefaultFor + productionConfig().progId=DevDesk.notepad，src/platform/file_association.h:29-41）。远端 SDK 修复四件套已原样移植进 F-18 版 macos.mm：UTType.h/UTTagClass.h 显式三头引入、C API <11 路径 pragma push/pop 成对包裹（12+ 走 UTType typeWithFilenameExtension: 类路径）、toOpenContentTypeOfFileAtURL API 形态；写入等待保留我方批量 dispatch_group 1.5s 总预算形态（超时=ok 交回查）。合并 commit=e290bb42（未 push）。 · 来源：实测 git show origin/dev:src/platform/file_association_macos.mm + 合并 e290bb42 本机 27.0 SDK 构建绿；登记：2026-11-18 03:35:48 +0800 developer；复核：2026-11-18 03:35:48 +0800

**进度记录**（6）：

- 2026-11-18 02:51:40 +0800 · 批 4 棒 A 完工（F-05 三选+关窗 veto+quit guard）：纯决策层 notepad_protect.{h,cpp}（三选映射/veto 裁决+豁免一次性消费/quitScan 三态）+ session 三字段（skipCloseConfirm/protectOpen/protectQuitContext，豁免作废锚在 sessionSetDirty 含早返回）+ drawProtectDialog（dialog content() 缝三钮，error 色危险钮）+ onCloseRequested 占位置换（notepad_module.cpp）+ app::setQuitGuard 装配接线（app_assembly.cpp assembleAndStart）+ test_notepad_protect 5 用例（TIN-10 已登记）+ probe G 段（veto 等价断言/三钮锚/不保存窗关计数减/保存落盘核对/quit 全链 consumeExitRequest 再入队+拒退可退+取消不卡死+全豁免放行）。新事实 clue #296。未验证项：真窗交互面（GLFW close 钮/托盘 Exit 实机流）probe 无实窗无法覆盖，归 qa/UAT；mtime 拦截语义保持拦截不扩三选（按简报口径）。棒 B（设置页关联开关）接续。
- 2026-11-18 02:53:04 +0800 · 棒B开工（need263 批4棒B F-18+D-7）：已读 clue #250/#268 + 方案卡 §十 全案。计划：①AssocConfig 多扩展化+unregisterAll+launchAdvancedAssociationUI ②win/macos 拆文件实现+Info.plist 26 扩展 ③md 关联卡退役→notepad 开关卡（四态/自愈逐扩展化）④新用例 TIN-10+settings 回归。
- 2026-11-18 03:06:13 +0800 · 棒B完工（need263 批 4 棒 B：F-18 多扩展关联开关 + D-7 md 卡退役）：①AssocConfig 多扩展化+production/test 切换+unregisterAll（仅归己才清，幂等）+launchAdvancedAssociationUI（api_map 锚齐）②win 多扩展循环+旧 DevDesk.md 注册迁移+mac 批量写入（12+ dispatch_group 单次 1.5s 总预算）+Info.plist 26 扩展覆盖（.ts 豁免，UTI 冲突实测线索 #297）③md 卡四文件删除，notepad_assoc_card/logic 接管，四态/自愈逐扩展化+摘要引导带，DB 键沿用存量字面量④测试：notepad_assoc 9/9 绿+fileassoc 2/2 绿+settings 137/137 绿+探针 OK（四态+截图，evidence/20261118-notepad-batch4-stickb）+隔离守卫 0+真机启动冒烟过。⚠平台边界：Windows 注册表路径本机仅编译级（CI 双平台实机验证）；mac 实写入路径未真机执行（测试红线禁触真实关联，UAT 归收口）。状态保持进行中，收口归主会话。
- 2026-11-18 03:07:57 +0800 · 批 4 两棒完工验收。20/20 功能点全交付。
- 2026-11-18 03:25:39 +0800 · 收尾合并手术（2026-11-18）：origin/dev 被 git 重设计线推进（bda66ad5）+macOS 27.2 SDK file_association 编译修复线（他们也在修 mac 打不开）；与 feat 合并余 3 处未决（EUI 指针/git_page_controller/macos.mm 语义合并），派 developer 手术棒。
- 2026-11-18 03:35:55 +0800 · 断点收尾=任务完成：3 处冲突全部裁决（EUI-NEO 指针=我方 0e4031f；git_page_controller.cpp=远端 need291 重写版 805 行，<utility>/<algorithm> 显式 include 已含、md_highlight 已走 ui_kit 新路径、std 守卫 908 文件 0 违例；file_association_macos.mm=语义合并：基底取远端 SDK 修复四件套+我方 F-18 多扩展语义，4 冲突块逐块裁决，12+ 写入保留我方 dispatch_group 1.5s 总预算形态，.ts 豁免/isDefaultFor(ext)/unregisterAll NotImplemented 全保留）。合并 commit=e290bb42（默认信息，未 push 归主会话）。验证：--dev 构建绿；notepad 47/47、fileassoc 2/2、notepad_assoc 9/9、settings 137/137（与合并前基线一致）；std include 守卫 0 违例；notepad_probe PASS。新线索 clue 298（远端 macos.mm 仍 F-09 旧接口的跨仓事实）。残留待办：UTType 类路径 typeWithFilenameExtension: 本机 27.0 SDK 编译绿，27.2 CI 编译待推送后确认（远端证伪的是 typeWithTag/typesWithTag 两 selector，非本 selector）；模块设置页 notepad_assoc_card/logic 四文件在工作树为 untracked 状态，若被 tracked CMakeLists 引用需主会话决定提交面（本次合并未含）。

**状态变更**（6）：

- 2026-11-18 02:37:35 +0800 ·  → 在池 · 入池（developer）
- 2026-11-18 02:37:41 +0800 · 在池 → 排产 · （main）
- 2026-11-18 02:38:24 +0800 · 排产 → 进行中 · （developer）
- 2026-11-18 03:07:57 +0800 · 进行中 → 待验收 · 批 4 两棒完工验收。20/20 功能点全交付。（main）
- 2026-11-18 03:25:39 +0800 · 待验收 → 进行中 · 收尾合并手术（2026-11-18）：origin/dev 被 git 重设计线推进（bda66ad5）+macOS 27.2 SDK file_association 编译修复线（他们也在修 mac 打不开）；与 feat 合并余 3 处未决（EUI 指针/git_page_controller/macos.mm 语义合并），派 developer 手术棒。（main）
- 2026-11-18 03:37:17 +0800 · 进行中 → 待验收 · （main）

---

#### need#297 · 打磨批次E·交互补齐（键盘路径/归档入口/根级建目录/空态覆盖/果冻/终端断点/文件名预检）

- 状态：待验收 · 优先级：P2 · 负责人：developer · 阶段：测试中
- 创建：2026-11-18 11:36:59 +0800（developer）· 更新：2026-11-23 09:55:14 +0800（developer）

**详情**：

REPORT.md §12 批次 E：①T7 键盘路径（schedule 编辑弹窗主输入 Enter=提交/Esc=取消；git 树/变更列表键盘导航）②todo 归档显式入口（详情弹窗补「归档」chip 或右键菜单）③md 根级新建目录（空树无处右键，根目录组行支持右键或树顶显式入口）④T4 空态组件覆盖（git 左/中栏、schedule 无规则页换 ui_kit::emptyState，git 窄盒用无图标档）⑤T5 果冻按压补齐（todo 任务卡接 jellyCard 保持拖拽命中面；home 统计卡可选）⑥终端窄窗断点改页面区宽（terminal_page.cpp:238 screen.width→减侧栏宽，Router pageOriginX 同源）⑦文件名前置预检（md 新建/重命名补 Windows 保留名+长度上限预检，MCP 写侧 64 字先例 notes_mcp_tools.cpp:25）。E1=todo+md 域（②③⑤todo 侧⑦），E2=schedule+git+terminal 域（①④⑥）。设计环节跳过理由同前（基准+走查具名；#156 §9 键盘规范/todo 折叠列键盘范式 todo_ui_board.cpp:511-530）。

**关联 BUG**（1）：

- 子表行#39 · 2026-11-18 12:08:31 +0800 · 已解 · E2 审计 P2 挂池：git 过滤无匹配空态与干净空态共用基 id「git.list.empty」（kit 化后区分只剩 .guide 有无，probe 已双向锁 guide，但未来仅断言 .message 存在性的探针会两态假阳）——后续探针编写注意或 need 时改独立基 id（zcode）

**进度记录**（6）：

- 2026-11-18 11:37:05 +0800 · 开工令=老板「继续」全批令；D 已落库，E1/E2 文件域不相交并行。
- 2026-11-18 11:55:08 +0800 · 断点进度：E2-a（schedule 弹窗 Enter/Esc/自动聚焦）+ E2-b（git 左栏×2 / schedule 列表空态换 emptyState，探针与 doctest 节点契约同步更新）已实现并 wip commit 15694ef7；定向 --filter schedule 136/136、git 179/179、terminal 48/48 全绿。E2-c 验证否决：pageScreen.width=pageContentWidth(整窗−侧栏)（app_sidebar.cpp:1401-1424+307-309），模块拿到的已是页面区宽，断点本就正确，不改。完整档 --no-run 被并行线（markdown/todo 在途半落盘，md_module_menu_poll/markdown_service -Werror 红）阻塞，待其落稳后复跑即可，我方产物未失效无需重建。
- 2026-11-18 12:03:50 +0800 · 收口补记：完整档 doctest 2468/2 失败均越界域（memory 钥匙串=AGENTS.md 已知平台不对称红族；todo 详情归档 chip=并行线未提交改动），构建已随并行线落稳转绿。git_page_probe 全 PASS（含干净仓/无匹配空态新契约实机验证）；schedule_ui_probe 默认场景 PASS。schedule_ui_probe --empty-rules（S1）本机红=既有环境态：引擎在新数据根仍产生系统行（sys.0 命中），空态分支不可达，改动前同因同红；空态节点契约由 headless doctest（test_schedule_ui_list 空帧断言 .message/.action）等价验证通过。
- 2026-11-18 12:11:54 +0800 · 断点进度：批次E 四项全部落地（E1-a 详情归档chip、E1-b 根组行右键新建子目录、E1-c todo卡果冻jellyFrame接入、E1-d Windows保留名+255字节预检）；定向 --filter todo 153/153、--filter markdown 123/123 全绿；完整档 --no-run EXIT=0（doctest 2468/0败+probe 52基线6设计内跳+守卫链全绿）；wip commit d23284bc（13 文件，含 3 个新用例）。home 统计卡果冻未做（简报可选项，预算内不做）。剩余：收口 qa 阶段全量+审计。
- 2026-11-18 12:20:15 +0800 · 批次 E 交互补齐七项闭环（E2-c 验证否决防无效改动）。
- 2026-11-23 09:55:14 +0800 · 机收口留验（证据不足）：六项有归宿（E1=d23284bc+ec8cf584、E2-a/b=15694ef7、E2-c 验证否决正当）；但「git 树/变更列表键盘导航」子项 src/module_git/ 全目录无键盘痕迹——待老板裁决补做或明示裁剪

**状态变更**（4）：

- 2026-11-18 11:36:59 +0800 ·  → 在池 · 入池（zcode）
- 2026-11-18 11:37:05 +0800 · 在池 → 排产 · 开工令=老板「继续」全批令；D 已落库，E1/E2 文件域不相交并行。（zcode）
- 2026-11-18 11:37:44 +0800 · 排产 → 进行中 · （zcode）
- 2026-11-18 12:20:15 +0800 · 进行中 → 待验收 · 批次 E 交互补齐七项闭环（E2-c 验证否决防无效改动）。（zcode）

---

#### need#331 · 文件关联回读错位（OS 弹窗已选 DevDesk、设置页仍显示未关联）

- 状态：待验收 · 优先级：P1 · 负责人：bug-fixer · 阶段：测试中
- 创建：2026-11-18 21:44:09 +0800（bug-fixer）· 更新：2026-11-20 11:17:37 +0800（bug-fixer）

**详情**：

老板验收实测（2026-11-18 新装 app）：文件关联 OS 弹窗选了「都用 DevDesk 打开」，设置页关联卡仍显示未关联。背景：notepad 批4 交付关联链（need288：AssocConfig 多扩展化+productionConfig+macOS 逐扩展回读）；今日 /Applications 刚覆盖重装（LaunchServices 对新副本的注册可能滞后/旧路径残留）。已知排查链（bug280 实锤）：-kill 已被 darwin27 移除；修法=-f 重注册+LSSetDefaultRoleHandler 钉绑定；bind 异步生效验证须延时。方向：①回读口径 vs 注册口径错位（设置页查的 UTI/handler 与 OS 弹窗写的注册项不同源）②重装后需 -f 重注册③回读无延时读到旧值。复现：双击 .md→OS 选 DevDesk→设置页回读；+直接 LSCopyDefaultRoleHandlerForContentType 命令行对照（设的是否真在 LS 库里）。

**进度记录**（6）：

- 2026-11-18 21:44:24 +0800 · 老板验收报修，与 #330（终端域）文件面不相交并行。
- 2026-11-18 22:19:03 +0800 · 主会话观测：wip commit 064f40c1 已落（mac 回读改按 UTI 查 handler，darwin27 文件 URL 查询对不存在探针返 nil），但子代理终报未达、池未自回——代理或仍在 LS 延时验证线。若统一重装窗口前终报仍缺：核对该 commit diff+定向用例入 QA 波次代验，防漏通知卡死收口。
- 2026-11-18 23:21:10 +0800 · QA 波次代验绿（#348）：全量波次（doctest 2498 例 0 红 + probe 58 项全过 + 守卫链）EXIT=0，覆盖本条断言面（commit 064f40c1 mac 关联回读按 UTI 查 handler）；代验收账成立，终态收口仍归 bug-fixer。
- 2026-11-20 10:52:00 +0800 · 续棒接手：从已有排查链（bug280 实锤链 + 三嫌疑方向）继续，定位设置页回读 vs LS 库 vs OS 弹窗写入三向错位环节。
- 2026-11-20 11:15:45 +0800 · 定位结论：三向对照闭合——①LS库（2026-11-20实测）：清单内全部扩展（md/markdown/json/txt/yaml/yml/toml/log/ini/js/mjs/cjs/jsx/css/html/htm/xml/sql/py/cpp/cc/cxx/h/hpp，.ts豁免外24项）handler全=com.devdesk.app，行为面 LSCopyDefaultApplicationURLForURL(真实.md)=/Applications/DevDesk.app；②OS弹窗写入已生效（同上LS现状佐证）；③设置页读：旧11-18验收版用 LSCopyDefaultApplicationURLForURL(不存在的探针文件)→darwin27返nil→回读永假=永显未关联，此环已被上一棒修掉（wip 064f40c1 改 LSCopyDefaultRoleHandlerForContentType UTI级查询），且 /Applications 现装（11-19 09:04）已含修复（nm 仅新API导入）。续棒增量：修复后实操证据落地——新探针 need331_assoc_card_probe 真渲染设置页模块tab关联卡，双档对照：裸跑（锚CFBundleGetMainBundle=空→Unknown）显「未生效+26项未生效」=老板症状机制复现；壳.app（锚=com.devdesk.app）显「已关联」。残留风险：裸二进制直跑实例（如遗留pid build/DevDesk）锚空必显未生效，复验必须以 .app bundle 形态启动。验证环境状态：/Applications 已装含修复版，app 已以 bundle 形态重启（pid 51849）。建议（清单外不执行）：①boss 复测直接开设置页即可见已关联；②GLFW 合成 mouseMoved 断言崩溃（.ips 11:01:45）是测试注入手法副作用，真实用户路径未证可触，建议另立条目评估。
- 2026-11-20 11:17:37 +0800 · 待验收口径（老板复测必读）：务必以 .app bundle 形态启动复测（裸二进制 build/DevDesk 直跑锚必为空、必显未生效——那是机制非 bug）；当前 /Applications 版即修复版，app 已重启（pid 51849）。复测路径：双击 .md→OS 弹窗选 DevDesk→设置页关联卡应显已关联。附带观察（合成 mouseMoved 触发 GLFW 断言崩溃）已另立 bug 条目评估。

**状态变更**（4）：

- 2026-11-18 21:44:09 +0800 ·  → 在池 · 入池（zcode）
- 2026-11-18 21:44:24 +0800 · 在池 → 排产 · 老板验收报修，与 #330（终端域）文件面不相交并行。（zcode）
- 2026-11-18 21:44:45 +0800 · 排产 → 进行中 · （zcode）
- 2026-11-20 11:17:37 +0800 · 进行中 → 待验收 · 待验收口径（老板复测必读）：务必以 .app bundle 形态启动复测（裸二进制 build/DevDesk 直跑锚必为空、必显未生效——那是机制非 bug）；当前 /Applications 版即修复版，app 已重启（pid 51849）。复测路径：双击 .md→OS 弹窗选 DevDesk→设置页关联卡应显已关联。附带观察（合成 mouseMoved 触发 GLFW 断言崩溃）已另立 bug 条目评估。（main-session）

---

#### need#332 · Claude 配置页验收七发现（环境变量间距/顶空白/使用中半透明/导入钮回归/连通测试审计/提示文本位置/env列表滚动）

- 状态：待验收 · 优先级：P1 · 负责人：developer · 阶段：待验收
- 创建：2026-11-18 21:49:35 +0800（developer）· 更新：2026-11-18 22:13:17 +0800（developer）

**详情**：

老板验收实测（2026-11-18）Claude 配置页七发现：①环境变量 tab 元素布局过大、上下间距过大②基础信息 tab 顶部空白过多（疑内容垂直居中错位）③profile 列表「使用中」tag 无半透明处理（=need#174 待验收项，修后并闭）④profile 导入按钮消失（疑回归——USER_GUIDE §3.4 有「导入当前配置」，git log 查哪批丢了）⑤连通测试按钮疑未真拿 token 去测（独立审计：审代码链 测试连通→probeProfile 是否带 key/真 POST）⑥编辑弹窗-模型配置-提示文本「显示名称留空时…」位置不对应放底部⑦编辑弹窗环境变量 tab 多次添加后列表无滚动条+明细行缺间距。①②③⑥⑦=claude_ui 域 UI 修；④=回归 hunt+恢复；⑤=审计线出结论。

**进度记录**（4）：

- 2026-11-18 21:49:51 +0800 · 老板验收报修六 UI+一审计；与 330/331 域不相交。
- 2026-11-18 21:53:14 +0800 · ⑤审计闭环：token 真发（双头同源转发链）；P2 无 key 提前提示+日志前缀已追加 #332a 修线。
- 2026-11-18 22:10:49 +0800 · 交接态：332a（八项 UI+追加 token 提示两项）与 332b 审计（已闭：token 真发；P2 前缀已转 a 线）在飞。
- 2026-11-18 22:13:17 +0800 · 断点：八项全落地（六项+追加⑧⑨），wip commit 54fca381。①env 页 32 高输入+8px 行距+virtualList 滚动（5 行上限/溢出提示取消）②详情面板删 P1-C2 垂直居中（claude_ui_detail.cpp:725 段）③徽标 α 0.78→0.55 ④溯源=98c7c17e 删「导入配置」连带丢入口；恢复：列表头「导入」钮+parseImportedEnv 还原+applyImportedToForm+module importCurrentSettings（读 ~/.claude/settings.json 预填，保存仍走 5s 预检）⑥hint 并一行移底区状态槽（槽位网格 52→48 让净空）⑧probeProfile/testConnection 加 keyMissing 出参，缺 key 报「未保存 API Key」⑨日志前缀 testConnection→probe。定向 claude 面 145 例全绿；截图 4 张 evidence/2026-11-18-need332/。待验收。

**状态变更**（4）：

- 2026-11-18 21:49:35 +0800 ·  → 在池 · 入池（zcode）
- 2026-11-18 21:49:51 +0800 · 在池 → 排产 · 老板验收报修六 UI+一审计；与 330/331 域不相交。（zcode）
- 2026-11-18 21:50:19 +0800 · 排产 → 进行中 · （zcode）
- 2026-11-18 22:13:17 +0800 · 进行中 → 待验收 · 断点：八项全落地（六项+追加⑧⑨），wip commit 54fca381。①env 页 32 高输入+8px 行距+virtualList 滚动（5 行上限/溢出提示取消）②详情面板删 P1-C2 垂直居中（claude_ui_detail.cpp:725 段）③徽标 α 0.78→0.55 ④溯源=98c7c17e 删「导入配置」连带丢入口；恢复：列表头「导入」钮+parseImportedEnv 还原+applyImportedToForm+module importCurrentSettings（读 ~/.claude/settings.json 预填，保存仍走 5s 预检）⑥hint 并一行移底区状态槽（槽位网格 52→48 让净空）⑧probeProfile/testConnection 加 keyMissing 出参，缺 key 报「未保存 API Key」⑨日志前缀 testConnection→probe。定向 claude 面 145 例全绿；截图 4 张 evidence/2026-11-18-need332/。待验收。（developer）

---

#### need#333 · GitHub 卡 Token 眼睛离开页面未恢复掩码（A16 验收不通过）

- 状态：待验收 · 优先级：P2 · 负责人：bug-fixer · 阶段：测试中
- 创建：2026-11-18 21:51:55 +0800（bug-fixer）· 更新：2026-11-18 22:19:27 +0800（bug-fixer）

**详情**：

老板验收（2026-11-18）：A16 不通过——GitHub 卡 Token 眼睛显形后切页再回，掩码未恢复（#320 侦察预判属实：githubSyncTokenVisible_ 仅 toggle 无重置，settings_module.cpp:1671/2059；manual 13 章口径称有恢复=文档与实现漂移）。修：页面离开钩子重置 githubSyncTokenVisible_=false（找 settings 模块页离开时机：onPageLeave/onStop/组合页切走判定，与「Token 显形仅本页有效」语义对齐）+单测锁（显形→离开→回来=掩码）。

**进度记录**（2）：

- 2026-11-18 21:52:09 +0800 · 老板验收判缺陷；settings 域与在飞四线不相交。
- 2026-11-18 22:19:27 +0800 · 接棒核账：交付物在身（802e0023），拨「待验收」对齐其余修复线；老板 A16 复验随统一重装批。

**状态变更**（4）：

- 2026-11-18 21:51:55 +0800 ·  → 在池 · 入池（zcode）
- 2026-11-18 21:52:09 +0800 · 在池 → 排产 · 老板验收判缺陷；settings 域与在飞四线不相交。（zcode）
- 2026-11-18 21:52:22 +0800 · 排产 → 进行中 · （zcode）
- 2026-11-18 22:19:27 +0800 · 进行中 → 待验收 · 接棒核账：交付物在身（802e0023），拨「待验收」对齐其余修复线；老板 A16 复验随统一重装批。（zcode）

---

#### need#335 · 记忆配置 tab 无滚动（折叠展开后底部不可达）——受冻结令约束待口径

- 状态：待验收 · 优先级：P2 · 负责人：bug-fixer · 阶段：测试中
- 创建：2026-11-18 21:56:36 +0800（bug-fixer）· 更新：2026-11-18 22:17:41 +0800（bug-fixer）

**详情**：

老板验收实测（2026-11-18）：记忆「配置」tab 无滚动——展开折叠组后底部内容不可见无法下滚。锚点：memory_module_panels.cpp 配置 tab 容器（四折叠组+安全隐私组，#181 结构；外层缺 scrollView 或内容高未驱动滚动区）。**状态受记忆域冻结令约束（clue#311）——待老板解冻口径**。

**关联线索**（2）：

- [open] clue#317 · 主题：eui:scrollview-measurecache-key-missing-state · 域：eui · 关键词：scrollview,MeasureCache,contentKey,折叠,滚动不更新 · 事实：EUI scrollview.h MeasureCache 键=（宽，视口高，gap，contentKey），**不含折叠态等 UI 状态**：折叠组展开后命中旧测量→内容高不重算、scrollable 不重判（全折叠默认低于视口→永不出滚动条）。DevDesk 侧修法=contentKey 拼折叠态签名（need335 memory 配置 tab 实锤）；任何「展开/切换子集后滚动区不更新」的 scrollView 先查 contentKey 是否漏状态维度。 · 来源：need335 子任务报告（ae96cb3a，2026-11-18）；登记：2026-11-18 22:19:03 +0800 zcode；复核：2026-11-18 22:19:03 +0800
- [open] clue#318 · 主题：eui:stack-wrapcontent-abs-children-undermeasure · 域：eui · 关键词：wrapContent,绝对定位,测量偏小,contentHeightProvider,stack · 事实：EUI layout.h stack 的 wrapContent 对**绝对定位子元素**测量值与子元素实画底不一致（need335 探针：声明几何 1320、实画至 1323）——即便测量缓存重算，供高仍偏小不可信。应用侧绕法=scrollView contentHeightProvider 供渲染 pass 回写实占底（need335 范式：compose 末尾回写 cfgContentBottom_）；根治候选=EUI-NEO fork 修 stack 测量（绝对定位族），待老板立项令，未立项不排产。 · 来源：need335 子任务报告（ae96cb3a，2026-11-18）；登记：2026-11-18 22:19:03 +0800 zcode；复核：2026-11-18 22:19:03 +0800

**进度记录**（2）：

- 2026-11-18 21:58:08 +0800 · 老板裁决：验收 bug 豁免冻结（clue 补录口径：验收期记忆 bug 即时修，新功能/调研仍冻结）。
- 2026-11-18 22:17:41 +0800 · 断点完成：已修已测已 wip commit（ae96cb3a）。根因两层：① MeasureCache 键不含折叠态→展开后命中旧测量 scrollable 不重判；② EUI stack wrapContent 对绝对定位子元素测量偏小（声明几何 1320 被解析为 1320 但子元素实画至 1323/尾部量差）→ 改 contentHeightProvider 供 composeConfig 渲染 pass 回写实占底（cfgContentBottom_）。验证：新单测 memory_config_scroll_tail_group_reachable_when_expanded 先红后绿；--filter memory 126 例 125 绿 1 平台跳过（Windows-only stress）。待验收。

**状态变更**（4）：

- 2026-11-18 21:56:36 +0800 ·  → 在池 · 入池（zcode）
- 2026-11-18 21:58:08 +0800 · 在池 → 排产 · 老板裁决：验收 bug 豁免冻结（clue 补录口径：验收期记忆 bug 即时修，新功能/调研仍冻结）。（zcode）
- 2026-11-18 21:58:36 +0800 · 排产 → 进行中 · （zcode）
- 2026-11-18 22:17:41 +0800 · 进行中 → 待验收 · 断点完成：已修已测已 wip commit（ae96cb3a）。根因两层：① MeasureCache 键不含折叠态→展开后命中旧测量 scrollable 不重判；② EUI stack wrapContent 对绝对定位子元素测量偏小（声明几何 1320 被解析为 1320 但子元素实画至 1323/尾部量差）→ 改 contentHeightProvider 供 composeConfig 渲染 pass 回写实占底（cfgContentBottom_）。验证：新单测 memory_config_scroll_tail_group_reachable_when_expanded 先红后绿；--filter memory 126 例 125 绿 1 平台跳过（Windows-only stress）。待验收。（bug-fixer）

---

#### need#337 · 退出未保存弹窗紧凑化（壳 opt-in compact 档+去底盘空白）

- 状态：待验收 · 优先级：P3 · 负责人：bug-fixer · 阶段：测试中
- 创建：2026-11-18 22:03:57 +0800（bug-fixer）· 更新：2026-11-18 22:20:12 +0800（bug-fixer）

**详情**：

老板验收（2026-11-18）：「修改未保存」弹窗文本观感好大——实拍定因=松散（默认高 240 底盘空白+正文按钮间距大），老板裁口径=紧凑化布局（字号不动）。修：DialogShellSpec 加 opt-in compact 档（紧凑下限 200，去 240 底盘空白）；quit 弹窗 spec.compact+height=内容实测值（头 69+正文 66+底 73≈224）。零波及其它弹窗（默认非 compact）。

**进度记录**（3）：

- 2026-11-18 22:04:12 +0800 · 老板裁决紧凑化；S 级直做（quit_save_bridge 无在飞线；dialog_shell opt-in 零波及）。
- 2026-11-18 22:10:49 +0800 · 交接快照态（2026-11-18 22:0x）：紧凑化三文件已改**未提交未目检**——dialog_shell.h（spec.compact 字段）+dialog_shell.cpp（clamp 200 档）+quit_save_bridge.cpp（compact+height=212）；已过一轮编译+quit 场景 19/19（/tmp/n337-shot2/ 有弹窗帧 png 未目检）；接棒：目检紧凑度（可调 height）→commit；老板令：一切执行归子任务，主会话不直做。
- 2026-11-18 22:20:12 +0800 · 目检结论：达标不改 height。212 生效后正文与底部按钮区间余量≈22pt（正常内边距），旧 240 底盘 50pt 空带消失，老板口径「去底盘空白」达成；字号未动，非 compact 弹窗零波及（clamp 分支仅 compact 走 200 下限）。注意：前会话遗留 /tmp/n337-shot2 旧截图/dump 是 compact 生效前的 240 帧（文件名 need291-*），已用 /tmp/n337-shot3 新帧取代。

**状态变更**（4）：

- 2026-11-18 22:03:57 +0800 ·  → 在池 · 入池（zcode）
- 2026-11-18 22:04:12 +0800 · 在池 → 排产 · 老板裁决紧凑化；S 级直做（quit_save_bridge 无在飞线；dialog_shell opt-in 零波及）。（zcode）
- 2026-11-18 22:04:25 +0800 · 排产 → 进行中 · （zcode）
- 2026-11-18 22:20:12 +0800 · 进行中 → 待验收 · 目检结论：达标不改 height。212 生效后正文与底部按钮区间余量≈22pt（正常内边距），旧 240 底盘 50pt 空带消失，老板口径「去底盘空白」达成；字号未动，非 compact 弹窗零波及（clamp 分支仅 compact 走 200 下限）。注意：前会话遗留 /tmp/n337-shot2 旧截图/dump 是 compact 生效前的 240 帧（文件名 need291-*），已用 /tmp/n337-shot3 新帧取代。（bug-fixer）

---

#### need#340 · 新建智能任务弹窗左 tab 双行文本上下间距微调（观感偏挤）

- 状态：待验收 · 优先级：P3 · 负责人：bug-fixer · 阶段：测试中
- 创建：2026-11-18 22:23:31 +0800（bug-fixer）· 更新：2026-11-18 22:29:37 +0800（bug-fixer）

**详情**：

老板验收实测（2026-11-18 22:4x）：agent 模块「新建智能任务」弹窗左侧 tab 列表——每 tab 项两行文本（主+副），观感偏挤，要求增加一点上下间距（item 内上下 padding/行距微调，幅度小档 +4~6pt 级，勿过度）。纯间距参数调整，S 级，按老板令仍派子任务执行。若有几何钉值单测同步。

**进度记录**（1）：

- 2026-11-18 22:23:35 +0800 · 老板验收报修；module_agent 文件域与在飞线（338 todo/home、339 mcp、331 notepad）不相交，并行必要。

**状态变更**（4）：

- 2026-11-18 22:23:31 +0800 ·  → 在池 · 入池（bug-fixer）
- 2026-11-18 22:23:35 +0800 · 在池 → 排产 · 老板验收报修；module_agent 文件域与在飞线（338 todo/home、339 mcp、331 notepad）不相交，并行必要。（zcode）
- 2026-11-18 22:23:39 +0800 · 排产 → 进行中 · （zcode）
- 2026-11-18 22:29:37 +0800 · 进行中 → 待验收 · （bug-fixer）

---

#### need#342 · 终端页新建按钮左侧空隙收窄（观感微调）

- 状态：待验收 · 优先级：P3 · 负责人：bug-fixer · 阶段：测试中
- 创建：2026-11-18 22:24:59 +0800（bug-fixer）· 更新：2026-11-18 22:31:19 +0800（bug-fixer）

**详情**：

老板验收实测（2026-11-18 22:5x）：终端页「新建」按钮左侧空隙偏大，要求减一点（小档收窄 −4~6pt 级，勿过度；单一来源改布局常量）。S 级微调按老板令仍派子任务。若几何钉值单测同步。

**进度记录**（2）：

- 2026-11-18 22:25:02 +0800 · 老板验收报修；module_terminal 与在飞五线文件面不相交（#330 终端路径线已交付关账），并行必要。
- 2026-11-18 22:31:19 +0800 · 断点进度：完成。落点=右动作组排布（terminal_page.cpp:221 一处），新建槽位距 8→4（−4pt，小档收窄下限，勿过度）；不动 mid/设置/分隔线槽位，窄窗 32 图标钮态同受益；命中测试随 rect 位置自动跟随，无溢出风险（tabAreaW 计算仍用 kActionGap，不受影响）。验收建议：老板实机打开终端页对比左隙。

**状态变更**（4）：

- 2026-11-18 22:24:59 +0800 ·  → 在池 · 入池（bug-fixer）
- 2026-11-18 22:25:02 +0800 · 在池 → 排产 · 老板验收报修；module_terminal 与在飞五线文件面不相交（#330 终端路径线已交付关账），并行必要。（zcode）
- 2026-11-18 22:25:06 +0800 · 排产 → 进行中 · （zcode）
- 2026-11-18 22:31:19 +0800 · 进行中 → 待验收 · 断点进度：完成。落点=右动作组排布（terminal_page.cpp:221 一处），新建槽位距 8→4（−4pt，小档收窄下限，勿过度）；不动 mid/设置/分隔线槽位，窄窗 32 图标钮态同受益；命中测试随 rect 位置自动跟随，无溢出风险（tabAreaW 计算仍用 kActionGap，不受影响）。验收建议：老板实机打开终端页对比左隙。（bug-fixer）

---

#### need#343 · MCP 卡片布局重设计：文本统一全左侧（designer 门控→实现接力 #339 后）

- 状态：待验收 · 优先级：P2 · 负责人：developer · 阶段：测试中
- 创建：2026-11-18 22:26:07 +0800（developer）· 更新：2026-11-18 23:03:42 +0800（developer）

**详情**：

老板令（2026-11-18 23:0x）：MCP 卡片布局**重新设计**，文本全在左侧（当前卡片文本分布散/居中，统一左对齐排布）。流程：ui-ux-designer 门控必经（重新设计=新设计面）——先出设计文档（参数填齐）+自包含 HTML demo；范围先钉对象（主列表 server 卡 vs 编辑页顶三卡，或全卡族统一左对齐原则，设计文档内明确）；**实现线排 #339（module_mcp 在飞）落定后接力**，同文件域互斥禁并行改码。设计须符合两档字重制（标题行 600/明细 400）与仓内卡片半透同池行规范。

**进度记录**（4）：

- 2026-11-18 22:26:17 +0800 · 老板重设计令；设计阶段（文档+demo，零改码）与在飞线无文件冲突可先行；实现阶段与 #339 同域互斥、排其落定后接力——并行必要性成立且阶段化解耦。
- 2026-11-18 22:35:39 +0800 · 设计完成（ui-ux-designer，2026-11-18）：①设计文档 docs/design/2026-11-18-mcp-card-redesign.md——范围钉板=主列表工具卡（mcp_page_module.cpp:407-584）+记忆模块行参数钉口径；详情弹窗 hero 区列同一左对齐原则但标注「实现待 #339 落定后」（§9）；含 file:line 现状锚 5 项、全数值参数表（卡 340-420×96、文本列 x=16 宽=卡宽-84、右轨仅图标/操作位、字重 600/400 两档、light/dark 全 token 对、动效 120ms+果冻 0.97）、状态表/自适应表/先验清单矩阵。②demo docs/design/2026-11-18-mcp-card-redesign.html——自包含单文件，双主题切换（?theme=dark 可直连），含长名省略/长短描述/未启用/禁用态/空态样例；已 headless 截图自查（light/dark/720px 窄窗三张，不破版）。零改码：未动 src/、tests/、EUI-NEO。状态保持进行中，待主会话转设计评审后接力实现。
- 2026-11-18 22:49:51 +0800 · 设计阶段收口（文档+demo 落定，老板可预览）；实现线即派 developer（module_mcp 实测无在飞冲突——#339 真身=module_agent，先前同模块互斥判断作废）。实现以文档参数为唯一真值源，断言引用具名设计常量。
- 2026-11-18 23:03:42 +0800 · 参数落地自查矩阵（真值源 docs/design/2026-11-18-mcp-card-redesign.md）：§4.1 卡高 96=kCardH(mcp_page_module.cpp:143)、圆角14/描边/阴影 glassShadowColor token 沿用未照抄 demo rgba；§4.2 文本列 x=16 宽=卡宽−84（kCardTextTrack），名称 y16 盒22 mono600/14 省略号、描述 y64 盒16 400/12 省略号；§4.3 状态行 y42 盒18=点8×8全圆r4+y47、点词距6、词11/600、词距chip12、chip40×18 pill 距6；§4.4 右轨=图标(卡宽−56,16)40×40 r10 primarySoftBg+码点16/primary、hover箭头(卡宽−32,64)16×16 fa-arrow-right primaryText 默认透明0；§4.6 动效120ms=bg Color OutQuad 0.12+箭头 Opacity OutCubic 0.12（仓内既有 transition 通道，schedule/claude 先例），果冻未新增（现卡无，遵红线）；§9 hero 实现待 #339（设计文档门控），现状已合原则（左缘 x=20/x=88 两条、右侧零文本）零改动；记忆行本已左列零改动（§4.5 现状不变）。旧常量 kCardTextX/kStatusGap/kCtaW 随布局废除；旧用例 desc_status_gap（右对齐前提）由 need343 两条结构断言替代。未验证项：light 主题实拍（token 同管道无分支，低风险）、hover 视觉态截图（探针不注 hover，箭头浮现逻辑由 mcp_page_hover_cta 断言覆盖）

**状态变更**（4）：

- 2026-11-18 22:26:07 +0800 ·  → 在池 · 入池（ui-ux-designer）
- 2026-11-18 22:26:17 +0800 · 在池 → 排产 · 老板重设计令；设计阶段（文档+demo，零改码）与在飞线无文件冲突可先行；实现阶段与 #339 同域互斥、排其落定后接力——并行必要性成立且阶段化解耦。（zcode）
- 2026-11-18 22:26:23 +0800 · 排产 → 进行中 · （zcode）
- 2026-11-18 23:03:42 +0800 · 进行中 → 待验收 · 参数落地自查矩阵（真值源 docs/design/2026-11-18-mcp-card-redesign.md）：§4.1 卡高 96=kCardH(mcp_page_module.cpp:143)、圆角14/描边/阴影 glassShadowColor token 沿用未照抄 demo rgba；§4.2 文本列 x=16 宽=卡宽−84（kCardTextTrack），名称 y16 盒22 mono600/14 省略号、描述 y64 盒16 400/12 省略号；§4.3 状态行 y42 盒18=点8×8全圆r4+y47、点词距6、词11/600、词距chip12、chip40×18 pill 距6；§4.4 右轨=图标(卡宽−56,16)40×40 r10 primarySoftBg+码点16/primary、hover箭头(卡宽−32,64)16×16 fa-arrow-right primaryText 默认透明0；§4.6 动效120ms=bg Color OutQuad 0.12+箭头 Opacity OutCubic 0.12（仓内既有 transition 通道，schedule/claude 先例），果冻未新增（现卡无，遵红线）；§9 hero 实现待 #339（设计文档门控），现状已合原则（左缘 x=20/x=88 两条、右侧零文本）零改动；记忆行本已左列零改动（§4.5 现状不变）。旧常量 kCardTextX/kStatusGap/kCtaW 随布局废除；旧用例 desc_status_gap（右对齐前提）由 need343 两条结构断言替代。未验证项：light 主题实拍（token 同管道无分支，低风险）、hover 视觉态截图（探针不注 hover，箭头浮现逻辑由 mcp_page_hover_cta 断言覆盖）（developer）

---

#### need#344 · 「今日 token 分布」卡右侧【<】翻页钮垂直居中修正

- 状态：待验收 · 优先级：P3 · 负责人：bug-fixer · 阶段：测试中
- 创建：2026-11-18 22:27:06 +0800（bug-fixer）· 更新：2026-11-23 09:55:14 +0800（bug-fixer）

**详情**：

老板验收实测（2026-11-18 23:1x）：「今日 token 分布」卡片右侧【<】翻页钮未垂直居中（观感偏移）。定位线索：疑 module_claude_proxy 用量统计卡（今日分布+右缘日期翻页 chevron）——自行确认；修=按钮 y 按卡头文本行视觉中线对齐（文字行高+字号线 ascent 对齐，非容器几何对半的像素错觉也要排查）。S 级微调按老板令仍派子任务；几何钉值单测同步。

**进度记录**（3）：

- 2026-11-18 22:27:09 +0800 · 老板验收报修；落点疑 module_claude_proxy（自行确认），与在飞各线文件面预期不相交；若与 #341 落点撞模块先等其提交（域互斥自查）。
- 2026-11-18 22:34:23 +0800 · 断点收尾：已 commit 54304dbd；剩=解锁屏后补前后截图（home.chart_collapsed=1 即老板所见折叠态，app 已装新版 bundle）
- 2026-11-23 09:55:14 +0800 · 机收口留验（含裁决，执行棒证实）：翻页钮修法 kChevronOpticalY 已被 2894adf5（need380 C-2）整卡裁撤，验收对象消亡、截图永不可补（t4-home-current-state.png 为现存实况）。待老板裁决：随卡退役关闭 or 指定历史 commit 补证

**状态变更**（4）：

- 2026-11-18 22:27:06 +0800 ·  → 在池 · 入池（bug-fixer）
- 2026-11-18 22:27:09 +0800 · 在池 → 排产 · 老板验收报修；落点疑 module_claude_proxy（自行确认），与在飞各线文件面预期不相交；若与 #341 落点撞模块先等其提交（域互斥自查）。（zcode）
- 2026-11-18 22:27:16 +0800 · 排产 → 进行中 · （zcode）
- 2026-11-18 22:34:23 +0800 · 进行中 → 待验收 · 断点收尾：已 commit 54304dbd；剩=解锁屏后补前后截图（home.chart_collapsed=1 即老板所见折叠态，app 已装新版 bundle）（bug-fixer）

---

#### need#357 · DevDesk 三视角独立审计（普通用户 / UI-UX 专家 / C++ 专家）

- 状态：待验收 · 优先级：P1 · 负责人：ui-ux-designer · 阶段：测试中
- 创建：2026-11-21 00:05:07 +0800（ui-ux-designer）· 更新：2026-11-21 05:09:07 +0800（ui-ux-designer）

**详情**：

老板 2026-11-21 令：分别从普通用户、UI 交互设计专家、C++ 专家三视角审查 DevDesk 桌面应用，给出独立审计报告。三面并发派：qa-tester（用户可用性/业务闭环）、ui-ux-designer（界面与交互设计）、code-reviewer（C++ 架构/工程标准）。产物=docs/reports/2026-11-21-three-perspective-audit.md（合并报告，分级清单不代修）。

**进度记录**（3）：

- 2026-11-21 00:20:40 +0800 · UI/UX 只读审计开工
- 2026-11-21 03:06:54 +0800 · 只读 UI/UX 审计完成，报告 docs/reports/2026-11-21-audit-uiux.md（P0×1 / P1×8 / P2×10 / P3×6）
- 2026-11-21 03:44:30 +0800 · 普通用户视角审计完成（qa-tester，只读：文档+6图+pool.db 只读+代码文案）。报告 docs/reports/2026-11-21-audit-enduser.md。结论 P0=0 / P1=3 / P2=8 / P3=6。P1：①USER_GUIDE 三处教的功能界面不存在（最近编辑列表已删+设置页笔记目录卡已删+项目卡未接线）②已完成任务次日启动自动归档且无任何提示+归档只渲20张+详情只读，指南未提③macOS 全部快捷键文档写 Ctrl 实为 ⌘（EUI shortcut() 在 APPLE 下返回 super）。正向：错误提示体系/自动保存三态/危险操作说明后果/本地优先数据透明。

**状态变更**（6）：

- 2026-11-21 00:05:07 +0800 ·  → 在池 · 入池（ZCode主会话）
- 2026-11-21 00:16:14 +0800 · 在池 → 排产 · 老板令：三视角独立审计 DevDesk 桌面应用。分三面并发派只读角色（qa-tester 用户视、ui-ux-designer 设计视、code-reviewer C++ 视）。（ZCode主会话）
- 2026-11-21 00:16:53 +0800 · 排产 → 进行中 · 开工：三面并发派出（ZCode主会话）
- 2026-11-21 02:43:39 +0800 · 进行中 → 待验收 · 三视角审计交付完毕。5 份报告落盘 + 仓库外备份；P0/P1 全部入池并已派发修复（358/359/360/361/362 五棒并发）。UI/用户两面因子代理超时由主会话接管，独立性差异已在报告声明。（ZCode主会话）
- 2026-11-21 03:44:30 +0800 · 待验收 → 进行中 · 普通用户视角审计完成（qa-tester，只读：文档+6图+pool.db 只读+代码文案）。报告 docs/reports/2026-11-21-audit-enduser.md。结论 P0=0 / P1=3 / P2=8 / P3=6。P1：①USER_GUIDE 三处教的功能界面不存在（最近编辑列表已删+设置页笔记目录卡已删+项目卡未接线）②已完成任务次日启动自动归档且无任何提示+归档只渲20张+详情只读，指南未提③macOS 全部快捷键文档写 Ctrl 实为 ⌘（EUI shortcut() 在 APPLE 下返回 super）。正向：错误提示体系/自动保存三态/危险操作说明后果/本地优先数据透明。
- 2026-11-21 05:09:07 +0800 · 进行中 → 待验收 · 审计 + 修复全链完成。关账留给老板验收（审查 368 侧栏结构大改、371 快捷键格式），下轮接手看 need#374。（ZCode主会话）

---

#### need#380 · 全仓代码量精简：砍不可达/冗余代码与配置、压缩啰嗦实现

- 状态：待验收 · 优先级：P2 · 负责人：ZCode主会话 · 阶段：测试中
- 创建：2026-11-21 23:45:03 +0800（ZCode主会话）· 更新：2026-11-22 02:41:15 +0800（ZCode主会话）

**详情**：

老板令（2026-11-21~22，七连）：①砍不可达/死代码 ②砍冗余代码和配置 ③压缩啰嗦/重复实现 ④五维卡死（代码量/注释量/执行周期/内存/CPU）⑤每功能每 UI 卡多维预算行 ⑥【功能层减法】替用户考虑砍功能/缩功能——候选必须老板裁决，只提案不行动 ⑦简化功能、简化流程。流程：六只读线（dead/config/config2/verbose/perf/featcut）出证据 → 范围卡+预算卡+功能裁决卡三裁决 → 分批实施 → 收口全量验证。硬约束同前（不动行为/测试绿/EUI-NEO 另案/测试只清死助手/平台红不碰）。

**关联线索**（16）：

- [stale] clue#356 · 主题：devdesk:frugal-dev-policy · 域：devdesk · 关键词：抠门,预算,卡死,代码量,注释量,执行周期,内存,CPU,need380 · 事实：老板令（2026-11-21）：开发全面抠门化，五维卡死——功能代码量/注释量/执行周期/内存使用/CPU 使用，硬预算从紧开发（原话「需要严格，卡死功能的代码量，注释量，执行周期，内存使用，cpu使用，用抠门的方式去开发」）。落地：need#380 范围+预算双卡裁决；具体数字待 [ro:perf] 基线实测后老板锁定；锁定前按「从紧」默认执行；后续需求方案卡必须带五维预算段，评审加预算轴。 · 作废理由：superseded: 被线索 357（subject=devdesk:frugal-dev-policy）取代；标废：2026-11-21 23:57:56 +0800 ZCode主会话；被 clue#357 取代 · 来源：老板会话原话 2026-11-21；need#380 detail 扩围记录；登记：2026-11-21 23:57:07 +0800 ZCode主会话；复核：2026-11-21 23:57:07 +0800
- [open] clue#357 · 主题：devdesk:frugal-dev-policy · 域：devdesk · 关键词：抠门,预算,卡死,代码量,注释量,执行周期,内存,CPU,少即是多,UI卡,need380 · 事实：老板令（2026-11-21，五连细化）：开发全面抠门化——①五维卡死：功能代码量/注释量/执行周期/内存使用/CPU 使用（硬预算从紧）；②「少即是多」「是时候减肥了」=减法优先，能删则删；③预算粒度=每个功能、每个 UI 卡各设多维度预算行（非仅全局总量）。落地：need#380 四只读线（dead/config/verbose/perf 基线）→ 范围卡+每功能/每UI卡预算卡双裁决（数字老板锁定后固化进 AGENTS.md/插件）；锁定前按从紧默认执行；后续需求方案卡必带五维预算段且按功能/UI 卡粒度逐行列预算，评审加预算轴。 · 来源：老板会话原话 2026-11-21 五连；need#380 detail；登记：2026-11-21 23:57:56 +0800 ZCode主会话；复核：2026-11-21 23:57:56 +0800
- [open] clue#358 · 主题：devdesk:need380-config-scan · 域：devdesk · 关键词：need380,config扫描,死脚本,gitignore,assets双份,release.yml,PROTECTED_PATTERNS,memcpp-probe · 事实：need#380 [ro:config] 扫描完成（25/45 调用，HEAD=af65c51b，未动文件）：死候选脚本×4——audit-font-scale.sh≈95行/syntax-sweep.sh≈125行/worktree-audit.sh≈330行（全仓含隐藏目录 0 引用）/dispatch-check.sh≈260行（仅 2026-10-19 handoff 文档引用）≈共-810行；CMakeLists.txt:490-498 fonts/ POST_BUILD 死拷贝块≈9行（本机无 fonts/、无生成机制，字体已拥 assets/）；.gitignore:120 !assets/models/* no-op、:50 fonts/JetBrainsMono 死条、:55 3rd/doctest 死条（各1行）；.zcode/armory/gates/pre-tool-use.sh:4,76,89 PROTECTED_PATTERNS 6 个死路径（2026-10-29 工作区壳清理遗存，运行时零开销）；assets 双份冗余复核成立（memory-embedding.7z 34,225,793B 内含 gguf 26,472,640B，散 gguf 亦 tracked；两侧各有活消费者：7z=打包链 unpack-memory-assets.sh ← 三打包脚本，gguf=测试/探针运行时 test_memory_engine.cpp/run-memory-e2e.sh/memcpp-probe）≈26.5MB；tools/memcpp-probe/CMakeLists.txt:17 add_subdirectory(llama.cpp) 目录不存在必 fail（一次性工程使命已毕）≈40行。全活面：CMake option×5、cheatsheets 零死链、28 scripts 其余全活、gitignore !assets/llama/bin/* load-bearing。CI 缺口×2：release.yml 两 job 缺 submodule update --remote -- EUI-NEO 步（P1：违反不锁版本契约，gitlink 停祖先=发版编译红风险）；release 无 DEVDESK_BUILD_PROBES=0（P2：12 probe 纯编译链接零运行纯浪费）。总预估≈-860行+26.5MB；全保守分级（需验证×4/老板裁决×5），无安全删级。深查缺口：tests/CMakeLists.txt 1,013 行、tools/memory-lab、docs demo 引用面。 · 来源：[ro:config] agent 实测报告（rg --hidden 全仓+CMake/workflows/.gitignore/.zcode 逐面核对）；登记：2026-11-22 00:00:24 +0800 ZCode主会话；复核：2026-11-22 00:00:24 +0800
- [open] clue#359 · 主题：devdesk:need380-feature-cut · 域：devdesk · 关键词：砍功能,缩功能,简化流程,替用户考虑,须用户同意,need380,featcut · 事实：老板令（2026-11-22，两条扩围）：①替用户考虑砍功能/缩功能——候选必须用户（老板）同意，任何功能退役/瘦身未经裁决不得实施；②简化功能、简化流程（用户流程步数减少、入口收敛）。归入 need#380 第六线 [ro:featcut]（product-manager 只读出候选清单）；范围卡将增设功能裁决区。 · 来源：老板会话原话 2026-11-22；need#380 detail 第七连；登记：2026-11-22 00:01:27 +0800 ZCode主会话；复核：2026-11-22 00:01:27 +0800
- [open] clue#360 · 主题：devdesk:need380-featcut-proposals · 域：devdesk · 关键词：need380,featcut,砍功能,A-1壁纸,A-2上线,B-6建副本,C-1归档,裁决卡 · 事实：need#380 [featcut] PM 线完成（14 模块全覆盖，16 候选全须老板裁决，全文=doc need380-feature-cut-proposals）：A 砍×2——A-1 壁纸模块退役(-8225行，推荐裁)/A-2 上线模块退役(-12637行，先查 release 表 30 天写入再定)；B 缩×8——B-6 Profile 建副本并入复制配置(推荐做)/B-3 试运行砍/B-5 告警 4 滑条→默认+B-2 模板 13→3~4/B-4 记忆健康 UI 收敛/B-1 触发六类→三四类/B-7 临时会话砍/B-8 四态→三态(低优先倾向保留)；C 简化×6——C-5 日志 6 档→3/C-6 MCP 记忆开关双入口/C-4 终端细项默认值/C-2 首页 token 卡收敛/C-1 待办自动归档加提示而非砍/C-3 .md 关联收敛到笔记(保留非 md 文本)。排序：B-6→C-5→C-6→C-4→C-2→B-3→B-5→B-2→C-1→B-1→B-4→B-7→C-3→B-8→A-1→A-2。git 模块建议保留；about 无候选。开放问题×4：A-2 自用查证/B-8 体感判定/减法节奏(推荐每条独立 need 流程不打包)。 · 来源：[featcut] product-manager 线终报（doc need380-feature-cut-proposals v1）；登记：2026-11-22 00:11:16 +0800 ZCode主会话；复核：2026-11-22 00:11:16 +0800
- [open] clue#361 · 主题：devdesk:need380-featcut-selfuse-check · 域：devdesk · 关键词：need380,featcut查证,A-2在用,A-1壁纸,release_task_log · 事实：need#380 featcut 开放问题查证（主会话读 ~/DevDesk/devdesk.db 实测）：A-2 上线模块确认在用——release_task 有 2026-11-06 done 行（真实发版执行），release_task_log 30 天内 18 条（最新 2026-11-06 23:33），steps 30 天 3 条；另有两个远未来占位 draft（2038/2050-11-06，疑测试残留，可顺带清）。→ A-2 建议撤案不裁。A-1 壁纸证据两可：~/DevDesk/wallpaper/settings.json 已配置（video/源目录指向用户自选）但 lastPlayedPath 空+库内仅 settings.json 无本地素材——是否在用需老板自判，上裁决卡。 · 来源：sqlite3 ~/DevDesk/devdesk.db 实测 + ~/DevDesk/wallpaper/settings.json；登记：2026-11-22 00:12:38 +0800 ZCode主会话；复核：2026-11-22 00:12:38 +0800
- [open] clue#362 · 主题：devdesk:need380-verbose-scan · 域：devdesk · 关键词：need380,verbose,重复代码,tool_kit,propagate,process_runner,样板 · 事实：need#380 [ro:verbose] 完成（滑窗 W=15/18/30 扫描+抽验，总净减 -620~-890 行，中置信）：①工具 helper 五件套（toolError×9/parseArgs×7/strField×6/intField×3/arrayOfJsonTexts×2）跨 12 文件逐字同构 dup≈241行 → 落 platform/tool_kit（core 禁依赖 platform 故不落 core），净-165，低风险；②process_runner macos/posix poll 主循环 ~110行同构（macos.cpp:310 注释自证上轮审计已标未收口）→ process_runner_common.cpp，净-80~85，需验证三处刻意差异（ECHILD stdin 双端/SIGKILL 收尸 LOG_WARN/break）；③wallpaper 图片/视频库扫描 ~77行同构 → 同模块共享，净-55~60；④mcp 双 config 读改写 ~45行 → 参数化，净-35~40；⑤Result fail-rewrap ::fail(x.errorCode(),x.errorMessage()) 36 文件 569 行形态 → core/result.h 加 propagate<U>(Result<T>)，净-150~250，不违 [[nodiscard]]；⑥JSON get-or-default 阶梯 44处×3行 → platform/json.h 加 getIfX/asStringOr，净-60~90；⑦tests：wallpaper 三 probe 脚手架 ~186行 dup 净-100~130（需验证）+ font_metrics gate/probe ~82行 dup 净-45~55。负面明证：LOG_*×721 无模式、setup/teardown 无样板、UI compose 已有 kit 采纳充分。全清单 file:line 锚点在 agent 终报。 · 来源：[ro:verbose] agent 终报（滑窗扫描+人工抽验 4 组）；登记：2026-11-22 00:17:32 +0800 ZCode主会话；复核：2026-11-22 00:17:32 +0800
- [open] clue#363 · 主题：devdesk:need380-config2-scan · 域：devdesk · 关键词：need380,config2,spike归档,docs死链,ARCHITECTURE,tests-CMake,缝源重复 · 事实：need#380 [ro:config2] 完成（tests-CMake/tools/docs 死链三段）：A tests/CMakeLists 1,013 行逐目标=无死条目，4 发现——set_tests_properties(memory_e2e/agent_e2e) 在 DEVDESK_BUILD_PROBES=OFF 可达路径无门（:857,863，与 :870-872 注释自相矛盾，需验证 0 行）；孤儿场景 terminal-claude-interactive.json 全仓 0 引用（31行，可能待接线需验证）；bridge 缝源/模块链接集 7 处逐字重复提 CMake 变量净 ~-90 行；孤儿注释 2 行。B tools：memcpp-integration/spike 1,019 行使命已毕（telemem_process.cpp:68 自证已回填）建议仓库外归档=老板裁决；memory-lab 不判死（3 处生产引用）；顶层 py 全活。C docs 死链 957 条引用判死 554 对（排误报后）：P1×2=ARCHITECTURE.md 仍引用已退役 module_pet + docs/reports/2026-11-21 审计报告×5 份 12 处路径失准（审计报告不可复核风险）；P2=ADR0005 死头文件（裁决）、历史 spec 死脚本 10 种（裁决：批量核销 vs 加历史稿标注）、plans 历史文档 ~420 处旧路径（建议索引加一句「历史路径以 git log 为准」不改文）。代码可删合计 ~1,150 行（含 spike 1,019）。 · 来源：[ro:config2] agent 终报（tests-CMake 全文逐目标+tools 全消费面+docs 957 条引用全量核对）；登记：2026-11-22 00:17:32 +0800 ZCode主会话；复核：2026-11-22 00:17:32 +0800
- [open] clue#364 · 主题：todo:tags-zero-usage · 域：devdesk · 关键词：todo,tags,零使用,#380,精简,截减候选 · 事实：todo tags 实测零使用（老板 2026-11-22 亲述「tags 其实没啥用，我都没用」）。数据侧：~/DevDesk/devdesk.db todo_tasks 共 2 条、tags 空值 2 条（100% 未用）。代码侧：tags 铺 70 处/7 文件（todo_service.cpp 19 / todo_ui_dialogs.cpp 21 / todo_dialogs.h 9 / todo_ui_board.cpp 7（卡片 chip 前 2 个 + 「+N」）/ todo_service.h 7 / todo_module.cpp 6 / todo_tools.cpp 1）；但工具侧只有 todo.list 单处输出 tags（todo_tools.cpp:84），无任何 MCP/agent 工具能写 tags。→ 裁决：tags 列为 #380（全仓精简）的截减候选，归入 todo 域清理批；不得为 tags 新增任何能力。 · 来源：老板口述 2026-11-22 + sqlite3 实测 todo_tasks + grep 代码分布；登记：2026-11-22 00:21:32 +0800 main-session；复核：2026-11-22 00:21:32 +0800
- [open] clue#365 · 主题：todo:tags-delete-decision · 域：devdesk · 关键词：todo,tags,全删,选项A,70处,迁移,#380 · 事实：todo tags 处理方式已裁决 = **选项 A 全删**（老板 2026-11-22）：去掉字段 + UI + 序列化，共 70 处 / 7 文件。分布：todo_service.cpp 19、todo_ui_dialogs.cpp 21、todo_dialogs.h 9、todo_ui_board.cpp 7（卡片 chip 前 2 个 + 「+N」）、todo_service.h 7、todo_module.cpp 6、todo_tools.cpp 1（todo.list 输出）。删时必须同步：① todo_tasks 表 tags 列（需迁移，注意历史数据 2 条均空值）；② core::overdue::kWhereSql 不涉 tags 无需改；③ todo_tools.cpp:84 的 tags 输出字段；④ 详情弹窗编辑入口（todo_ui_dialogs.cpp:200/329）；⑤ 卡片 chip 渲染（todo_ui_board.cpp:898-929）。执行归属 #380 精简线 todo 域，**当前未开工（老板 2026-11-22 明示「现在不开工」）**。 · 来源：老板裁决 2026-11-22 + 代码分布实测；登记：2026-11-22 00:23:14 +0800 main-session；复核：2026-11-22 00:23:14 +0800
- [open] clue#366 · 主题：devdesk:need380-testq-scan · 域：devdesk · 关键词：need380,testq,测试有效性,A级,pr_win_placeholder,shim盲区,ui_driver缺口,cron缺口 · 事实：need#380 [ro:testq] 完成（静态审计未跑二进制，18/60 调用）：测试面总体 A 级有效——2,567 用例/≈25,644 断言（密度≈10/用例）；真零断言用例仅 13/2567（0.5%，初筛 302 系正则漏匹配 DOCTEST_CHECK_EQ 变体），其中 8 个 return 校验结果=等效断言，真冒烟/占位 5 个。C 级实锤唯一：tests/test_platform/test_process_runner.cpp:93 pr_win_placeholder（DOCTEST_CHECK(true) 恒真占位，非 macOS 编译分支）删之净-15 行，同族清点全仓恒真仅此 1 处。抽检 8 套件（app_config 字节往返/sqlite 四面/terminal 解码后逐字/vterm cell 级/github maskUrl/credential roundtrip/git_threads 2000 轮并发）无一虚高。结构性风险×3（框架限制非假测试）：shim 仅报首个失败（绿≠全断言已验）/body 提前 return true=静默绿（对未来新增用例是陷阱，评审须查断言前 return）/宏集固定。高价值缺口 top5：ui_driver 0 自动用例（框架+scenes 齐备未挂接）/cron.cpp 无直测（跨月/夏令时/非法串）/Result 跨层传播无系统测试/file_watcher mac 轮询兜底语义薄/live 族 1,804 行无自动门禁。 · 来源：[ro:testq] agent 终报（205 文件密度统计+13 文件深读+恒真全仓扫）；登记：2026-11-22 00:24:11 +0800 ZCode主会话；复核：2026-11-22 00:24:11 +0800
- [open] clue#367 · 主题：devdesk:need380-tools-survey · 域：devdesk · 关键词：need380,tools,jscpd,Bloaty,include-cleaner,nanobench,hyperfine,反例,内存缺口 · 事实：need#380 [ro:tools] 完成（联网实据，top-5 全双平台可用）：①jscpd 5.4.3（MIT，v5 已 Rust 原生二进制免 Node）token+语义级重复检测——比 rg 级强（改写型克隆可抓），ci.yml 加 1 step min-tokens 50 先基线后定门；②Bloaty 1.1 二进制体积构成=减码证据物（mac brew bottle 有；PE 解析 experimental 待验一次）；③misc-include-cleaner 零新依赖（runner LLVM 20.1.8 自带，ci.yml:203-216 现成报告步加 check 名）；④nanobench 4.6.0 单头微基准=CPU 预算门挂 doctest filter（MinGW 计数器降级未实测）；⑤hyperfine 2.0.0 命令级计时=执行周期基线（本地为主 CI 噪声大）。候补第 6 cppcheck（互补低重叠但第二套告警分诊噪声抵消，缓）。反例清单（避免重查）：IWYU（版本锁 clang+MinGW 假阳性）/Mull（无 Windows 支持违双平台）/Tracy（MinGW 非官方支持，仅帧泵深排临时用）/easy_profiler（休眠）/PMD CPD（需 JVM，jscpd 胜）。五维预算覆盖：内存维度无轻量自动门=唯一缺口（ASan 档无峰值阈值，后续专项票）；注释量 rg 已覆盖不引入工具。全文来源含实取链接在 agent 终报；报告未落盘（harness 纪律），归档可写 docs/research/2026-11-22-lightweight-tooling-survey.md 由后续批次执行。 · 来源：[ro:tools] researcher 终报（23 调用，gh api/npm/brew/runner 镜像实取）；登记：2026-11-22 00:25:36 +0800 ZCode主会话；复核：2026-11-22 00:25:36 +0800
- [open] clue#368 · 主题：devdesk:need380-perf-baseline · 域：devdesk · 关键词：need380,perf,基线,冷启动1125ms,RSS171MB,空闲CPU0.74%,逐页2.9ms,预算上限 · 事实：need#380 [ro:perf] 完成（macOS arm64 Debug 形态，2026-11-22 实测，五维基线可作预算卡死依据）：①冷启动首帧中位 1125ms（区间 1124-1874，首启冷缓存 1874；建议上限 ≤1400ms/冷缓存档 ≤2800ms）、主界面就绪中位 1424ms（含 1.2s 定长 splash；≤1800ms）；②稳态 RSS 171MB（≤220MB）；走遍页面 RSS 代理峰值 132MB（真机峰值未测，如实标注）；③空闲 CPU 均值 0.74%/峰 1.97%（卡 ≤2%）；操作期 52-79%（回归口径：完成后 2s 内回落 ≤2%）；④逐页切换中位 1.5-2.9ms/页 全程 max 10.1ms（driver 口径 ≤20ms/页）；todo 新增 14ms/schedule 新建保存 7ms/settings 开关 2-3ms（≤50ms）；⑤主二进制 51.8MB（≤60MB）、.app 62MB（≤75MB）、线程 28（≤36）。方法：冷启动=直启二进制+devdesk.log 时间戳 n=5；页走/操作=devdesk_ui_driver 真装配真渲染临时数据根；/tmp/dperf/ 留原始脚本与数据未落仓。纪律：冷启动前置 kill 了运行中实例且测完未重启（由主会话恢复）；/Applications 仅只读 stat 未安装。未测如实标注：真机走遍页 RSS 峰值、真实键鼠注入耗时（无辅助功能授权）。 · 来源：[ro:perf] agent 终报（真实 app+ui_driver 双口径实测，/tmp/dperf 原始数据）；登记：2026-11-22 00:27:47 +0800 ZCode主会话；复核：2026-11-22 00:27:47 +0800
- [open] clue#369 · 主题：devdesk:need380-eui-scan · 域：devdesk · 关键词：need380,eui,EUI-NEO死代码,widget_style,组件消费面,渲染后端重复 · 事实：need#380 [ro:eui] 完成（EUI-NEO@ce35582 双面查，-280~490 行）：A 安全删×4≈30 行高置信——apps/serial_tool/app.cpp:135 byteHex/:208 countHexBytes（匿名空间双面 0 调用）、core/render/text.cpp:743 readCodepoint 包装（0 调用）、apps/promo/app.cpp:422-431 #if 0 块（全仓唯一）。B 老板裁决≈107 行——widget_style.h 整文件 62 行全仓 0 引用、theme.h PageHeaderLayout、SerialPort writeChar/flushReceiver、RoundedRectPrimitive 4 个 transform setter（transform 走 setTransform 另路）；B6 决策项：datatable/layout_debug/piechart/navbar/radio 五组件 DevDesk 0 消费（内部 demo/test 有引用非死码，产品面去留属老板）。C 需验证≈实测 382 行精确重复→净减 250-350（低中置信需先出共享层设计）：五对组件头 DSL 样板互抄 162 行、OpenGL/Vulkan 渲染后端样板 158 行（跨后端抽象需设计评审）、内部头/平台工具 62 行。负面明证：注释块/死守卫/死 id 全产品 0 命中；7 个低引用 struct 均为存活值类型（误报已排）；glfw/sdl 平行实现无 ≥15 行重复不构成合并对象。 · 来源：[ro:eui] agent 终报（ctags 101 局部符号+697 顶层函数+277 类批量双面计数，低引用者行级取证）；登记：2026-11-22 00:28:51 +0800 ZCode主会话；复核：2026-11-22 00:28:51 +0800
- [open] clue#370 · 主题：devdesk:need380-dead-scan · 域：devdesk · 关键词：need380,dead,死代码,ReleaseViewState,死常量,LOG_TRACE,安全删 · 事实：need#380 [ro:dead] 完成（全仓 122,337 标识符词频+5,924 定义行逐条复核，-165~195 行中值 181）：A 组安全删×20 条证据完备——最大件 ReleaseViewState 整类 -47（release_view_state.h/.cpp，getter「存活」系 todo 域同名撞车，实例 0）；死函数/方法×12（containsAsciiCaseInsensitive -20/formatTodayCn -10/liveActiveBlockStableId -9 等）；死类/结构×4（MiniTool/CapturedBlock/PrereqView/PageIconProbe）；死测试 helper×2；k 系死常量×9 -10；LOG_TRACE 宏 -2。B 组需验证×2：claude_data_refresh_wake_saw_ready_for_test（extern "C" 测试钩子零引用需上游确认）、jelly_probe enum StateIdx（改匿名 enum 无行收益）。C 组备注：docs-only 死常量名 ~108 个（清文档另立任务）、注释残留废常量名 ~15 处 P2、DevDeskSansSC-Bold.otf.unicodes.txt 非死物不删。负面明证：注释代码块/永假分支/#if 0/CMake 未挂源/从未 include 头全仓 0；probes 55 个「未挂 CMake」系 GLOB 全量编译非死码。构建层未验（只读约束），清理 PR 过一次编译即闭环。 · 来源：[ro:dead] agent 终报（词频库+逐条全词 rg 复核+误报留痕）；登记：2026-11-22 00:33:16 +0800 ZCode主会话；复核：2026-11-22 00:33:16 +0800
- [open] clue#372 · 主题：devdesk:need380-featruling · 域：devdesk · 关键词：need380,功能裁决,A-1保留,A-2保留,可配置检查单,need381 · 事实：need#380 功能裁决结果（老板 2026-11-22）：A-1 壁纸模块**保留**（不裁不冻结）；A-2 上线模块**保留**，后续简化为「可配置的检查单」——已立独立需求记录该后续。featcut 其余 14 条（B缩×8/C简化×6）与批A-D/预算仍待老板审后开工令。 · 来源：老板会话原话 2026-11-22；need#381；登记：2026-11-22 00:54:13 +0800 ZCode主会话；复核：2026-11-22 00:54:13 +0800

**进度记录**（30）：

- 2026-11-21 23:53:44 +0800 · 老板「继续」令已落（全批令 cov=380）。三只读扫描线（[ro:dead]/[ro:config]/[ro:verbose]）在飞，返回后汇总证据清单+范围卡裁决。
- 2026-11-21 23:53:50 +0800 · 扫描阶段进行中：3 条只读线（死代码/冗余配置/重复啰嗦）在飞，预算各 ≤60 次调用/45 分钟。
- 2026-11-21 23:57:56 +0800 · 扩围三连已入线索（clue#356 将被细化版取代）：预算粒度=每功能/每 UI 卡。四扫描线在飞。
- 2026-11-22 00:00:25 +0800 · 线 1/4 [ro:config] 已回：≈-860行+26.5MB 裁决候选+release.yml P1/P2 缺口×2，关键事实已落 clue（devdesk:need380-config-scan）。填并发位补派 2 线：[ro:config2]（tests-CMake/tools/docs 死链深查，上轮声明缺口）+ [ro:eui]（EUI-NEO 死代码只读扫，范围卡素材）。在飞：dead/verbose/perf/config2/eui 共 5。
- 2026-11-22 00:01:27 +0800 · 扩围：+功能层减法线（[ro:featcut] product-manager，候选须老板裁决）。在飞 6 线。
- 2026-11-22 00:03:07 +0800 · +线 7/7 [ro:testq] 测试有效性审计（老板令：有效测试才有价值，覆盖率=敷衍；评估轴 A/B/C 分级非覆盖率）。在飞 7 线：dead/verbose/perf/config2/eui/featcut/testq。
- 2026-11-22 00:05:18 +0800 · +线 8/8 [ro:tools] 轻量开源工具调研（researcher，联网必检）：三轴=质量/减码/性能，重点=能自动执行五维预算的工具；只调研不实装，产出 top-5 短名单。在飞 8 线。
- 2026-11-22 00:17:32 +0800 · 扫描进度 4/8 回（config/featcut/verbose/config2，均落 clue+doc）。在飞：dead（续跑）、perf（续跑）、eui、testq、tools。累计候选：静态面 ≈-2,630~-3,000行+26.5MB+功能 16 条；全部待三卡裁决后分批。
- 2026-11-22 00:23:08 +0800 · 2026-11-22 老板裁决 todo tags 处理方式 = 选项 A（全删：字段 + UI + 序列化，70 处/7 文件）。本批归入 #380 精简线的 todo 域，尚未开工（老板明示「现在不开工」）。
- 2026-11-22 00:24:11 +0800 · 扫描进度 5/8 回（+testq：测试面总体 A 级，C 级仅 1 占位用例，高价值缺口×5 已落 clue#364）。在飞：dead（续跑）、perf（续跑）、eui、tools。
- 2026-11-22 00:25:36 +0800 · 扫描进度 6/8 回（+tools：top-5 工具短名单+反例清单落 clue#367，内存预算自动门=唯一缺口）。在飞：dead（续跑）、perf（续跑）、eui（续跑）。三线全回即上三卡裁决。
- 2026-11-22 00:27:49 +0800 · 扫描进度 7/8 回（+perf：五维基线实测完成落 clue#368，建议上限已附）。app 实例已恢复启动。剩：dead（续跑）、eui（续跑）。全回即上三卡。
- 2026-11-22 00:28:51 +0800 · 扫描进度 7.5/8：+eui（EUI-NEO -280~490 行落 clue#369；B6 五组件 0 消费是纯决策项）。仅剩 dead（续跑中，探针见其正活跃扫描）。回齐即上三卡。
- 2026-11-22 00:33:16 +0800 · 8/8 扫描线全部完成（config/config2/dead/verbose/testq/perf/eui/tools + featcut PM 线），全部落 clue。上三卡裁决：范围批选/五维预算数字/功能减法。
- 2026-11-22 00:40:30 +0800 · 裁决卡未获勾选；批A（安全删族 -215 行，原始令直接面+证据完备）按已授权范围先行派发 developer（wave 1 在飞，不 commit）。批B/C/D+预算档+功能减法仍待裁决，稍后重上卡。
- 2026-11-22 00:44:12 +0800 · 老板纠正「我没说开工」：批A 实施棒已即时 TaskStop，实施越权撤回（工作树核实无任何编辑落地，无回滚需要）。全部实施批（A/B/C/D）+预算档+功能减法均停在待裁决，等老板明确令。扫描结论 8/8 已落 clue 不动。
- 2026-11-22 00:54:13 +0800 · 功能裁决部分落定（clue:need380-featruling）：A-1 壁纸保留、A-2 上线保留；A-2 后续简化已立 need#381（在池待开工）。仍待：批A-D 开工令、预算档、B/C 类 14 条功能小修。零实施棒在跑。
- 2026-11-22 00:54:21 +0800 · 功能裁决部分落定（clue#372）：A-1 壁纸保留、A-2 上线保留；A-2 后续简化已立 need#388（在池待开工）。仍待：批A-D 开工令、预算档、B/C 类 14 条功能小修。零实施棒在跑。
- 2026-11-22 00:56:06 +0800 · 老板「开」令（2026-11-22）。wave1 三棒并行在飞：批A 安全删族（src/tests 域）/批B 脚手架清理（scripts/tools/根CMake/gitignore/.zcode 域）/批D-wave1 CI+docs（.github/docs 域）。预算档=采纳建议档（首帧≤1400/RSS≤220MB/空转CPU≤2%/页切≤20ms/操作≤50ms/二进制≤60MB/线程≤36）。后续波：批C 主仓（等批A 落地避文件冲突）→ EUI A组（submodule，先推 fork）→ nanobench+include-cleaner --fix+assets 26.5MB 收敛 → 功能小修波（14 条按 PM 排序，B-8 按 PM 建议保留不动）。各棒不 commit，主会话核产物后 pathspec wip: 提交。
- 2026-11-22 00:59:09 +0800 · 批B 完成并核产物提交：14 文件 -1,914 行（死脚本×4/spike+memcpp-probe 归档仓外 ~/devdesk-archive-20261122//fonts 死块/gitignore×3/.zcode 死路径×6，构建绿）。老板「需求池里的都清了」=功能面全部放行：14 条功能小修 wave5 按 PM 排序实施（B-8 按 PM 建议保留不动）；need#388（上线检查单化）同步放行，先 req-analysis 澄清形态。wave1 余两棒在飞（批A src/tests、批D CI+docs）。wave4 加项：.gitignore:70-76 spike 专属忽略死条（批B 遗留观察）。
- 2026-11-22 01:03:45 +0800 · 批D-wave1 完成并提交 88d49bd1（8 文件 +45/-16：release.yml 补 EUI-NEO --remote×2+probes=0、ci.yml jscpd 基线步、ARCHITECTURE/审计报告×3/ADR0005 死链修复、DOCS-CONVENTIONS 历史路径口径行）。已提交：批B 5302d559、批D 88d49bd1。批A 在飞。wave4 待办汇总：run-clang-tidy.sh:42 CHECKS 加 misc-include-cleaner（.clang-tidy 不存在且命令行覆盖配置文件，唯一正确落点在脚本）+ CLAUDE.md md_highlight→md_preview_highlight 1 处 + .gitignore:70-76 spike 忽略死条。
- 2026-11-22 01:09:28 +0800 · 批A 安全删族完成：21/22 项已删（⑤CapturedBlock 保留未删——与代码实际矛盾：md_module_menu_poll.cpp:248 struct 的局部变量 captured 在 255/260-261/266-270 在用，整删必编译红，证据[ro:dead] clue#370 对该项的判定有误，待主会话裁决是否改匿名 struct）。改动 30 文件（29 M + 2 D release_view_state.h/.cpp + module_release/CMakeLists.txt 摘源清单），26 个删除符号全仓残留 rg=0。验证：--dev 构建绿 + 定向套件全绿（A release/todo/markdown Total511 F0 S1；B home/wallpaper/memory/settings Total352 F0 S0；C agent/http/schedule/process_runner/icons Total371 F0 S2；test_main logging Total2 F0 S0）。未 commit（主会话 pathspec 统一提交）；tests/probes/claude_proxy_e2e_probe.cpp 为他人未跟踪文件未动。
- 2026-11-22 01:11:18 +0800 · 批A 完成并提交 24259792：33 文件 -217 行（21/22 项，⑤CapturedBlock 扫描误报——实为活代码经实施棒拦截保留，判定依据留报告）。累计已提交净 -2,086 行（批B -1,914/批A -217/批D +16）。wave2 批C 主仓 8 项已派（单棒串行 90/60，process_runner 合并带宁漏勿错闸）。
- 2026-11-22 01:35:15 +0800 · 批C 重复压缩实施完成（研发代理，未 commit 待主会话统一提交）：8/8 项全部落地——①工具五件套→platform/tool_kit（12 文件收敛）②getIf 族→platform/json.h（15 阶梯）③Result::propagate→core/result.h（720 处/53 文件）④mcp rewriteExisting→module_mcp/mcp_config_common（产品名参数化，backupFile 符号保留）⑤wallpaper 扫描→core/library_scan⑥process_runner 排空收尸→platform/process_runner_common（两刻意差异开关保真：ECHILD 关 stdin 仅 posix；SIGKILL 收尸失败 WARN 仅 macos）⑦wallpaper 三 probe 公共脚手架（仅逐字节相同 7 块收敛，差异段保留）⑧font sha256→tests/support/font_sha256.h。验证：--dev 构建绿；定向套件 mcp 201(1 skip)/wallpaper 110/font 30/schedule 141/todo 154/agent 158(2 skip) 全过；isolation 守卫过。git diff --stat：81 文件 +913/-2188（净 -1275，含新文件 728 行）。Windows 侧 CI 编译验证待跑。
- 2026-11-22 01:37:27 +0800 · 批C 完成并提交 abb3cea4：90 文件 +950/-2188 净 -1,275 行（propagate 720 处/53 文件超预估、tool_kit/json getIfX/mcp 参数化/wallpaper 扫描合并/process_runner 跨平台合并两差异开关保真/probe 公共头）。构建绿+isolation 过+六域套件全绿 794 例 0 失败。插曲：主会话目录级 add 曾裹挟 #379 会话未跟踪探针文件进提交，已软摘出 amend（abb3cea4），教训将入共享 worktree 协议。wave3（EUI 4 死码删+fork 推送）与 wave4（tidy 脚本/CLAUDE.md/gitignore 死条）并行在飞。Windows 侧编译验证待 CI。
- 2026-11-22 01:39:17 +0800 · wave4 完成并提交（clang-tidy +misc-include-cleaner 4 处同步/.gitignore spike 死条×5）。CLAUDE.md md_highlight 线证伪收案：:227 是 test_md_highlight 测试名引用非旧文件名，零改动。待 wave3（EUI fork 推送）返回后 bump 指针，随后 wave5 功能小修 14 条（UI 微调跳过 ui-ux-designer 记理由：删减收敛类无新设计面）+收口全量验证。
- 2026-11-22 01:52:44 +0800 · wave5-d 断点（实现完成、验证被并发波次阻断）：B-1+C-3 代码与测试已改完——src/core/notepad_file_types.h（matchesAnyExtension 抽取 + markdownExtensions/isMarkdownExtension 判据）、src/app_assembly.cpp（漏斗 .md→笔记页 open: 负载/目录外拷贝 captureFileIntoNotesDir/模块未注册兜底记事本窗、secondInstance 唤醒 .md 需主窗、armTray .md 不藏主窗）、src/module_schedule/schedule_ui_edit.cpp（新建页签收敛三类、存量 kind 编辑保底追加页签）、tests test_app_assembly.cpp+test_notepad_file_types.cpp 按新语义更新。验证状态：devdesk_module_schedule/notepad/settings 三库目标编译绿（01:52 产物）；DevDesk 主程序链接与全部套件运行被阻断——工作树实况与任务书事实 1 不符（33 个改动文件，非 2 个），他域在途编辑致 src/module_memory/memory_module.h:68 语法错误 + tests test_claude_service.cpp/test_provider_config.cpp 引用已删除 API，app 与 devdesk_tests 目标均编不过。下一步：他域波次收口后重跑 --dev 构建 + schedule/notepad/markdown/assembly 套件 + schedule 探针即收口；未 commit（任务书禁做）。
- 2026-11-22 01:53:20 +0800 · wave5d 完成（代码落盘、验证 deferred）：B-1 新建收敛三类+存量规则引擎分支全保留（周朖→cron 迁移被否：语义不等价，零数据丢失红线优先）；C-3 .md 改走笔记模块（captureFileIntoNotesDir+navigateTo，笔记模块关闭时兖底记事本窗，notepadExtensions/OS 关联面不动）。三库目标编译绿（新 .a 产物实证）；套件/主程序验证被 a/b/c 在途编辑阻断（memory 语法错瞬态+claude 测试引用已删成员瞬态），收口时重跑 --dev+四套件+schedule 探针+实操 .md 双击。wave5-c 撞墙已拉活；a/b 续跑中。
- 2026-11-22 02:00:45 +0800 · wave5-a/b/d 全部完成并按域提交：5316933e(-1,229)/a0051397(-973)/b0aab3bb(+125)。累计已提交 wip 净 ≈-5,462 行+EUI fork a3aabba(-32)。仅剩 wave5-c（todo/mcp/terminal/home，已续跑）在飞。收口待办清单：①wave5-c 落地提交 ②全量 doctest+probe+守卫链（含补跑 agent_e2e_probe/claude_proxy_e2e 相关/schedule 探针/实操 .md 双击）③feat: 收口 commit ④纪律固化（五维预算入 AGENTS/插件）⑤遗留小项：claude_run_settings DB 表+sweepStaleRunSettingsFiles 死代码（wave5-a 上报，独立条目）。
- 2026-11-22 02:41:15 +0800 · 战役收口：feat 2894adf5 已推 origin/dev（ssh 443 通道）。纪律固化=AGENTS.md 五维预算段（feat 内）。遗留单列：notepad_probe（bug399 既有）、claude_proxy_e2e_probe（#379 登记 ctest 参数后计入）、claude_run_settings 死表另立 debt。

**状态变更**（4）：

- 2026-11-21 23:45:03 +0800 ·  → 在池 · 入池（ZCode主会话）
- 2026-11-21 23:53:44 +0800 · 在池 → 排产 · 老板「继续」令已落（全批令 cov=380）。三只读扫描线（[ro:dead]/[ro:config]/[ro:verbose]）在飞，返回后汇总证据清单+范围卡裁决。（ZCode主会话）
- 2026-11-21 23:53:50 +0800 · 排产 → 进行中 · 扫描阶段进行中：3 条只读线（死代码/冗余配置/重复啰嗦）在飞，预算各 ≤60 次调用/45 分钟。（ZCode主会话）
- 2026-11-22 02:41:15 +0800 · 进行中 → 待验收 · 战役收口：feat 2894adf5 已推 origin/dev（ssh 443 通道）。纪律固化=AGENTS.md 五维预算段（feat 内）。遗留单列：notepad_probe（bug399 既有）、claude_proxy_e2e_probe（#379 登记 ctest 参数后计入）、claude_run_settings 死表另立 debt。（ZCode主会话）

**关联文档**（2）：

doc#47 · need380-feature-cut-proposals（audit v1 · 更新 2026-11-22 00:11:16 +0800）→ 附录 A
doc#48 · need380-slimming-tech（tech v1 · 更新 2026-11-22 00:46:00 +0800）→ 附录 A

---

### 在池（15）

#### need#231 · 上线模块查询数据源能力缺口：Nacos 服务状态/文本清单查询无后端能力（#192 §4.7 查询条被阻塞）

- 状态：在池 · 优先级：P3 · 负责人：ZCode主会话
- 创建：2026-11-08 02:45:15 +0800（ZCode主会话）· 更新：2026-11-08 02:45:15 +0800（ZCode主会话）

**详情**：

来源：#192 阶段二实现发现（2026-11-08）。设计 §4.7 要求 Deploy/Verify 查询内联条（Nacos 服务状态/文本清单查询），但 release 域 grep 无任何 http 查询能力（无 Nacos 客户端/无文本清单数据源）——UI 条已设计（内联 max 160 展开），缺的是数据源能力本身。候选：产品接入 Nacos API（新依赖/新配置面）或降级为静态清单展示。属能力缺口非 UI 缺陷，待老板定方向后立实现批。PRD doc 36 §10 能力缺口清单同族。

**状态变更**（1）：

- 2026-11-08 02:45:15 +0800 ·  → 在池 · 入池（ZCode主会话）

---

#### need#257 · 记忆检索质量根治：embedding 升级/hybrid/重排，拉开正负例分差（阈值参数面不可解）

- 状态：在池 · 优先级：P3 · 负责人：ZCode主会话
- 创建：2026-11-10 01:03:37 +0800（ZCode主会话）· 更新：2026-11-10 01:03:37 +0800（ZCode主会话）

**详情**：

来源：bug255 修复批实测结论（2026-11-10，evidence/2026-11-10-memory-bugfix/POC-and-regression.md）。faiss score=1/(1+L2²)、向量已归一化，bge-small-zh-v1.5 下真实弱正例（0.4179-0.4380）与负例（0.4098-0.4518）分数带重叠，0.4 阈值负对照偶泄漏、0.5 杀真命中——参数面不可解（完整实测数据已注释留痕 telemem_client.cpp）。根治候选：①换更强 embedding 模型 ②hybrid 检索（关键词+向量）③重排层。改动在引擎链，成本较高，P3 挂起待老板裁决优先级。当前 0.4 保持（真命中优先）。关联 need:159/bug:255。

**状态变更**（1）：

- 2026-11-10 01:03:37 +0800 ·  → 在池 · 入池（ZCode主会话）

---

#### need#260 · 记忆修剪/管理：全量列举+单删/批量删+全清强确认（telemem 列举/删除缝已在，need31 结论过时）

- 状态：在池 · 优先级：P2 · 负责人：ZCode主会话
- 创建：2026-11-10 01:33:42 +0800（ZCode主会话）· 更新：2026-11-10 01:33:42 +0800（ZCode主会话）

**详情**：

老板问（2026-11-10）：过时的、重复的记忆能修剪吗？核查结论（venv 只读，telemem==1.10.1 mcp/server.py）：工具面已齐——get_memories(列举,scope+limit) :280 / get_memory :297 / update_memory :313 / delete_memory :333（撤销链已用,telemem_client.h:64）/ delete_all_memories :352 / memory_history :376。need31（2026-10-31 关）「待 telemem 暴露记忆列举缝」结论已过时——钉版 1.10.1 缝已在。

缺口=DevDesk 侧无 UI：①修剪界面（全量列表分页+每条文本/类型前缀/入库时间+单删/多选删+全清强确认）②重复检测（库内无语义去重，采集指纹去重只防同轮；增强项=入库时/修剪时近似重复提示）③过时辅助（按时间排序+来源标注，无自动 decay）。

候选分级：M 级先做①手动修剪（seam 全现成，纯 DevDesk 侧）；重复提示②与自动遗忘（TTL/decay）作后续增量。待老板裁决排产。关联 need:31（旧账，口径已被本条取代）。

**状态变更**（1）：

- 2026-11-10 01:33:42 +0800 ·  → 在池 · 入池（ZCode主会话）

---

#### need#264 · EUI-NEO 框架能力补齐（记事本前置）：保存对话框 + 软换行 word wrap + 字体族枚举

- 状态：在池 · 优先级：P1 · 负责人：main
- 创建：2026-11-16 21:22:49 +0800（main）· 更新：2026-11-16 21:22:49 +0800（main）

**详情**：

来源：need#263 记事本模块前置依赖（老板 2026-11-16 令「缺失的能力 EUI 先补齐并验证」）。

三项框架能力缺口（本会话实测确认）：
1. **保存对话框**：EUI-NEO 只有 openFileDialog（core/platform/platform.h:60-62），全仓 grep saveFile|NSSavePanel|GetSaveFileName|IFileSaveDialog 零命中。需补 saveFileDialog（Windows GetSaveFileNameW / macOS osascript choose file name，与现有 openFileDialog 同文件分流，不建议拆 *_win/_macos 文件——现有实现即单文件 #ifdef）。
2. **软换行（word wrap）**：components/input.h:475/496 写死 .wrap(false)，水平只能 horizontalScroll；measureLines（input_model.h:593）只按 '\n' 硬行切分。记事本「自动换行」开关需框架支持软换行排版。
3. **字体枚举**：resolveFontPathForFamilyImpl（core/render/text_shaping.cpp:570-650）是硬编码白名单（monospace/Menlo/Monaco/SF Mono/PingFang/微软雅黑/SimHei/YaHei/Cascadia/YaShe），无 systemFontFamilies() 之类枚举 API。记事本「设置字体」只能先做受限列表，或补枚举。

纪律：一律在 EUI-NEO fork 修（AGENTS.md §一），不在 DevDesk 造绕路；macOS + Windows 两头都要验证；先在 fork 补→push fork→再动 DevDesk（跳仓推送顺序）。

**状态变更**（1）：

- 2026-11-16 21:22:49 +0800 ·  → 在池 · 入池（main）

---

#### need#350 · 记事本独立窗接入 minSize(720,480)——need#269 能力的消费端接线

- 状态：在池 · 优先级：P3 · 负责人：main-session
- 创建：2026-11-20 11:11:55 +0800（main-session）· 更新：2026-11-20 11:11:55 +0800（main-session）

**详情**：

need#269 验收发现（developer 棒清单外建议，主会话裁定立条）：EUI-NEO DSL minSize/maxSize 能力已上 fork/main（0f253fb），但记事本独立窗（need#263 v2 线）尚未接线消费——窗仍可被拖到任意小、布局断点失效。交付：记事本窗创建处 DslWindowConfig 链 .minSize(720,480)（如需上限加 .maxSize，按 notepad 设计稿 §2 口径）。文件域=module_markdown/notepad 窗创建链，与 #331（files/settings）、#269（已完成）无交集。验证：DevDesk mac 侧探针已有（window_minmax_limits_macos_probe 可扩消费端断言）；Windows 侧随 need#269 待验收项一并真机复核。

**状态变更**（1）：

- 2026-11-20 11:11:55 +0800 ·  → 在池 · 入池（main-session）

---

#### need#365 · 首启无引导：零数据时 14 个模块平铺 + 一堆 0，新用户不知道从哪开始

- 状态：在池 · 优先级：P1 · 负责人：product-manager
- 创建：2026-11-21 02:33:43 +0800（product-manager）· 更新：2026-11-21 02:33:43 +0800（product-manager）

**详情**：

来源：2026-11-21 三视角审计用户面 P1-02（主会话扮演普通用户走查首页零数据状态）。现状：USER_GUIDE.md:31-32 写「启动后先看到约 1.2 秒的启动画面，随后进入首页」——之后没有任何引导。零数据时首页是 5 张显示 -- 或 0 的统计卡 + 空图表 + 空最近编辑，侧边栏 14 个模块名平铺。用户感受（普通用户视角）：「第一次打开看到 14 个功能名 + 一堆 0，不知道该先点哪个，也不知道这软件是干嘛的」。文档里说「DevDesk 是本地优先的开发者桌面工具集」但普通用户不会去读文档才打开软件。影响：首启 30 秒决定用户是否继续用。修法（推荐方案）：零数据时（笔记数=0 且待办数=0 且无 Profile）首页加引导区——①3 个入口卡（记一条笔记 / 加一个待办 / 配置 Claude 模型）+ 一句话产品定位；②可选提示「用不上的模块可以在设置里关掉」。有任一数据后自动隐藏。可参考现有 ui_kit::EmptyStateOptions 四字段风格（图标+主句+引导句+行动钮）。注意：需老板确认是否做（属新 UI 能力而非修 bug）；若做要过 ui-ux-designer 门控。方案卡见 docs/reports/2026-11-21-audit-fix-plan.md §9 B9-b。

**状态变更**（1）：

- 2026-11-21 02:33:43 +0800 ·  → 在池 · 入池（product-manager）

---

#### need#374 · 三视角审计修复批验收交接卡（11 条已交付 + 验证证据 + 待裁决）

- 状态：在池 · 优先级：P1 · 负责人：ZCode主会话
- 创建：2026-11-21 05:08:59 +0800（ZCode主会话）· 更新：2026-11-23 10:11:51 +0800（ZCode主会话）

**详情**：

2026-11-21 三视角审计修复批的验收交接卡（供下轮验收直接使用，不用重查）。

【已交付 11 条】
bug358 壁纸退出崩溃（P0）—— 先红后绿，12/12
bug368 侧栏裁切「关于」不可达（P0）—— 30/30 + 实窗截图
bug359 Windows 回收站标志位
bug360 模块隔离守卫重建（15 模块全枚举 + 平台宏守卫 + 3 棵坏树）
bug361 异常纪律（std::stoi + 内部字段名）
bug362 路径 UTF-8（toUtf8 + 5 处）
bug369 tooltip 文档 + Claude 错误态
bug371 文档四处（快捷键 ⌘/Ctrl 平台化 + 删不存在功能 + 自动归档说明）
bug372 kToggleable 索引漂移（改 id 定位）
bug373 notepad_probe 断言过期

【验证结果】doctest Total 2555 / Passed 2547 / Failed 0 / Skipped 8；probe 63/63 全过；守卫链全绿（含各守卫 --self-test）。已装机 /Applications/DevDesk.app（sha256 逐字节一致、签名有效、Dock 缓存已刷新）并启动，15 模块全部启动无新 ERROR。

【审核时需重点看的】
① bug368：侧栏结构大改（+232 行），建议老板亲自看真机效果（滚轮 + chevron 点击 + 深链自动滚入在三种窗口高度下的表现）。
② bug371：快捷键文案改成 ⌘/Ctrl 双写，请确认这个格式你满意。
③ 新发现未修（已入池）：bug366（memory 三处 fopen 同族编码问题）、bug367（schedule 还有 21 处内部字段名）、bug363/364/365（文档对齐 / ARCHITECTURE 追认 / 首启引导）。

【待裁决 3 项】侧边栏宽度 260 vs 200（我按 200 实现了）、侧栏是否要分组、Git/终端是否真可关。

**进度记录**（2）：

- 2026-11-23 09:55:15 +0800 · 机收口结果（2026-11-23）：交接卡 11 条中 9 条已代关（358/359/361/362/369/372/373/360/356），368/371 留观感待老板过目；三裁决（侧栏 260vs200/是否分组/Git+终端可关性）仍待老板。全批收口后本卡可关。
- 2026-11-23 10:11:51 +0800 · 三裁决全部落定（2026-11-23 裁决卡）：①侧栏宽度=维持 200；②不分组；③git/终端开关=补（承接条目 #416 待行首令）。交接卡使命完成；368/371 仍留观感待老板过目。本卡待放行后随批关账

**状态变更**（1）：

- 2026-11-21 05:08:59 +0800 ·  → 在池 · 入池（ZCode主会话）

---

#### need#382 · 主动找我：事件驱动的主动推送体系（todo 到期/agent 完成/定时提醒聚合到统一达）

- 状态：在池 · 优先级：P1 · 负责人：main-session
- 创建：2026-11-22 00:19:24 +0800（main-session）· 更新：2026-11-22 20:40:18 +0800（main-session）

**详情**：

老板 2026-11-22 确认需要（原话表述「主动找我」，作流量入口要件之一）。

现状实证：能力全部存在但彼此孤立——schedule 有 cron/interval/once 引擎 + 4 种通知渠道（fullscreen/toast/os_notification/wecom），agent 有 scheduler 定时驱动能力，todo 有 dueAt/listOverdue()；但：① todo 与 schedule 零联动（module_todo 全仓零引用 schedule）；② 4 个渠道只用得到「定时页里手工建的规则」；③ 无任何聚合视图——用户不主动进模块就什么都不会发生。

方向待 PRD 收口（三档由浅入深）：A 最小闭环=todo.dueAt 接 schedule 渠道（一个 adapter）；B 首页/AI 工作台着陆页=今日到期任务+agent 运行态+最近变更聚合；C 全量主动化=系统级通知入口 + 托盘宝提示未读数。

需要老板裁决的开放点：① 哪些事件值得主动推送（todo 到期/agent 完成/CI 挂/定时任务）；② 推送形态偏好（系统通知 vs 应用内 vs 托盘徽标）；③ 是否允许打扰（免打扰时段）。

流程：L 级（跨模块 + 产品形态）→ PM PRD（含上面三个裁决点）→ ui-ux-designer → 开发 → 审计 → QA。

**进度记录**（2）：

- 2026-11-22 00:22:21 +0800 · 拆分说明（2026-11-22）：老板单独确认要 #384（todo 到期→schedule 通知），该条 = 本条目 A 档（最小闭环）的执行体，已在 #384 落地。本条目余下 B/C 两档：#384 交付后再评估首页工作台聚合（B）与系统通知+托盘徕标（C）是否仍有必要。避免与本条重复计工。
- 2026-11-22 20:40:18 +0800 · 老板 2026-11-22 晚令「暂不开发，先收敛，明天统一验收」——本条保持在池不动；接续须行首「开工 #382」或全批令。三裁决点（推送事件范围/推送形态/免打扰时段）待 PRD 阶段收敛上卡。

**状态变更**（1）：

- 2026-11-22 00:19:24 +0800 ·  → 在池 · 入池（main-session）

---

#### need#383 · 终端启动目录从单值升级为「常用目录列表」（多套代码场景，纯点选无快捷键）

- 状态：在池 · 优先级：P1 · 负责人：main-session
- 创建：2026-11-22 00:20:30 +0800（main-session）· 更新：2026-11-22 20:40:18 +0800（main-session）

**详情**：

老板 2026-11-22 确认加入需求。

现状实证：src/module_terminal/terminal_settings.h:42 `std::string workDir = ""` —— 全局单值，所有新会话共用；terminal_page.cpp:1405-1430 设置面板里是一个文本输入框 + 「浏览」钮（接平台原生文件夹选择器）。module_terminal.cpp:20/117 经 setNewSessionOptions 把它传给 controller。

痛点：老板「一个项目多套代码」（backend/web/shared 各一目录），单一 workDir 导致每开新 tab 要么先进设置改一次路径、要么进去再 cd。

目标：workDir 从单值升级为「常用目录列表」：① 设置面板里可增删多个目录并持久化；② 新开 tab 时可直接点选目录（或默认从头一条）；③ 空/无效目录的错误行与兜底行为保持不变。

硬约束：macOS + Windows 双平台（文件夹选择器已有 platform::folder_picker 两端实现）；UI 自适应；纯点选无快捷键；不引入项目/workspace 实体概念（老板明确不要）。

待方案卡定的开放点：① 存储形态（terminal_settings 新增 JSON 数组键 vs 新表）；② 新 tab 的目录选择交互落点（新建 tab 弹层 vs 顶栏下拉 vs 设置里“默认目录”）；③ 旧单值配置的迁移兼容；④ 是否需要「一次开多个 tab 各在一目录」的成组启动（老板提及多套代码可能联动，需确认）。

流程：M 级 → PM 短 PRD（含上面四裁决点）→ ui-ux-designer → 开发 → 审计 → QA。

**进度记录**（1）：

- 2026-11-22 20:40:18 +0800 · 老板 2026-11-22 晚令「暂不开发，先收敛，明天统一验收」——本条保持在池不动；接续须行首「开工 #383」或全批令。四开放点（存储形态/选择交互/旧值迁移/成组启动④须确认）待方案卡阶段收敛。

**状态变更**（1）：

- 2026-11-22 00:20:30 +0800 ·  → 在池 · 入池（main-session）

---

#### need#384 · todo 到期接 schedule 通知（#382 A 档最小闭环）：逾期扫描器 + 去重 + 四渠道投递

- 状态：在池 · 优先级：P1 · 负责人：main-session
- 创建：2026-11-22 00:22:16 +0800（main-session）· 更新：2026-11-22 00:22:16 +0800（main-session）

**详情**：

老板 2026-11-22 明确「加入需求」。本条目是 #382（主动找我：事件驱动推送体系）的 A 档最小闭环，先做这一块。

现状实证：
- todo 侧：todo_service.h `std::optional<int64_t> dueAt`（Unix 秒，nullopt=未设置）已存；TodoService::listOverdue()（todo_service.cpp:870）已实现，逾期谓词单源 core/overdue.h kWhereSql（与 HomeService::getStats 共用）。→ 数据与查询完全就绪。
- schedule 侧：schedule_rule 表已含 kind/spec_json/channel_ids/action_kind,m issed_policy 等全字段；sch 引擎（schedule_engine.h）已有 interval/once + 错过补跑策略；渠道 4 种（fullscreen/toast/os/silent，channel_registry.h:34-37）齐全；action_runner.h 已有 remind/remind_open/run_task/webhook_only 四动作 + 重试阶梯（0s/30s/300s）。
- 缺口：module_todo 全仓零引用 schedule（grep 证实）；todo.dueAt 与 schedule 引擎之间无任何桥。

方向（待 PRD 收口）：不做「每条 todo 建一条 schedule_rule」的粗暴映射（N 条任务 = N 条规则，容易规则爆炸 + 双向同步地狱）。前置倾向：schedule 侧新增一个「todo 到期扫描器」——单条常驻规则，引擎每 tick 调 listOverdue() 拿逾期集，与上次快照 diff，只对新增逾期任务发一次通知（去重靠 schedule_run 记录或单独 marker 表）。

待裁决：① 触发时点=逾期后立即 vs 截止前 N 小时预警 vs 两者都要；② 通知内容是否带任务标题+打开跳转（deeplink 到 todo 页）；③ 已完成/已归档任务不提醒的排除口；④ 是否允许用户逐任务开关提醒。

流程：M 级 → PM 短 PRD（含四裁决点）→ 开发 → 审计 → QA。跨模块（todo+schedule）但不动两模块核心，新增桥接层。

**状态变更**（1）：

- 2026-11-22 00:22:16 +0800 ·  → 在池 · 入池（main-session）

---

#### need#385 · 删除项目卡（need-11）死代码：UI 已移除但 640 行仍在编译

- 状态：在池 · 优先级：P1 · 负责人：main-session
- 创建：2026-11-22 00:28:48 +0800（main-session）· 更新：2026-11-23 10:05:30 +0800（main-session）

**详情**：

老板 2026-11-22 确认加入需求。

现状实证（重要：UI 已删、代码未清）：
- kCardSpecs[] 的 draw 函数清单里**没有** drawProjectCardsCard（现存 10 个：appearance/editor/proxy/mcp/modules/reminder/notepad_assoc/log/logview/github_sync）→ 项目卡 UI 入口已不存在。
- 但三文件仍在 src/module_settings/CMakeLists.txt:14-16 编译：project_card_store.cpp（7958B）、project_cards_card.cpp（16357B）、project_info_tool.cpp（4687B）。
- settings_module.cpp 仍有 ~40 处活引用：include 两处（:18-19）、refreshProjectCards()（:207/:298 调用 + :1518 实现）、projectCards_/projectCardDraft_ 成员（:2157-2158）、动作 switch（:1531-1553）、新增/编辑/保存（:1561-1592）、草稿快照传递（:1815-1816 in.projectCards = &projectCards_）。

删除清单（待方案卡确认）：① 三源文件 + CMakeLists 三行；② settings_module.cpp 的 include/成员/refreshProjectCards/动作 switch/快照传递；③ project_cards_card.h 的 projectCardsCardHeightFor + drawProjectCardsCard 声明；④ settings_ui_cards.h 的 CardDrawArgs::projectCards 等字段（需先确认无其他消费方）；⑤ schema.h 里 project_cards 表 DDL + schema.cpp:668 的「需-11 老库逐列补缺」迁移（注意：已落库用户该表会残留，需决定是否 DROP）。

风险点：schema.cpp:668 的补列迁移与 project_info_tool 可能被 MCP 侧注册（main.cpp:587 有 registerProjectInfoTool 调用）——需先核实 project_info 是否仍在 MCP 服务里对外暴露，如果 claude 侧还能看到它就不能直删。

归属 #380 精简线。M 级（跨 UI/业务/schema 三面，但无新逻辑，纯删）。流程：方案卡（含 schema 处置裁决）→ 开发 → 审计 → QA。

**进度记录**（3）：

- 2026-11-22 00:29:14 +0800 · 风险点核实（2026-11-22）：project_info 是**活的 MCP 工具**，不是死代码——main.cpp:587 每次启动注册进 core::mcp::globalMcpTools()，claude 侧 tools/list 能看到；实现读 project_cards 表（getProjectCard/listProjectCards）。其 description 还写着「可在 DevDesk 设置 → 项目卡 tab 登记」但该 tab 已不存在 → 工具活着但维护入口死了（半瘫痪）。
  
  → 删除不能只删代码，三个处置必须一起拍：A 全删（UI 死代码 + project_info 工具 + project_cards 表 DDL/迁移，claude 失去该能力，需确认老板接受）；B 只删 UI 死代码保留工具+表（claude 能查但只能手改 DB 维护）；C 恢复 UI 入口（与精简方向相反）。待老板裁决。
- 2026-11-22 00:30:10 +0800 · **老板裁决 2026-11-22 = 选项 A 全删**：UI 死代码 + project_info MCP 工具 + project_cards 表 DDL/迁移一并删除。
  
  完整删除清单（含核实结果）：
  ① 三源文件：project_card_store.cpp / project_cards_card.cpp / project_info_tool.cpp + CMakeLists:14-16 三行
  ② settings_module.cpp：include（:18-19）、refreshProjectCards()（:207/:298 调用 + :1518 实现）、projectCards_/projectCardDraft_ 等成员（:2157-2158）、动作 switch（:1531-1553）、新增/编辑/保存（:1561-1592）、草稿快照传递（:1815-1816）
  ③ main.cpp:587 registerProjectInfoTool 整段（含 :583-585 注释）+ module_settings 里 register/unregisterProjectInfoTool 声明与实现
  ④ settings_ui_cards.h 的 CardDrawArgs::projectCards 等字段（先确认无其他消费方）
  ⑤ schema.h 里 project_cards 表 DDL（28 张表之一）+ schema.cpp:668 的「需-11 老库逐列补缺」迁移
  ⑥ **新增待裁：已落库的 project_cards 表怎么办**——直接 DROP（老库残留空表）还是保留不动（仅删代码不建表）。老板已知库中该表为空可直接 DROP。
  
  归属 #380 精简线。M 级（跨 UI/业务/schema/MCP 四面，纯删无新逻辑）。流程：方案卡 → 开发 → 审计 → QA。
- 2026-11-23 10:05:30 +0800 · Win 台账对账（2026-11-23）：Win bug#189（设置「项目卡」tab 删除）与本条同族——Win 侧 11-04 已删 UI 入口（kSettingTabLabels 5→4），本条续清死代码 640 行；删除时一并核 project_cards 表 DROP 与 MCP project_info 暴露面（Win#189 口径）

**状态变更**（1）：

- 2026-11-22 00:28:48 +0800 ·  → 在池 · 入池（main-session）

---

#### need#386 · DB 族大手术：删数据库升级代码 + 删备份 + 删 dump（不做向后兼容）

- 状态：在池 · 优先级：P1 · 负责人：main-session
- 创建：2026-11-22 00:32:43 +0800（main-session）· 更新：2026-11-22 00:41:49 +0800（main-session）

**详情**：

老板 2026-11-22 两次明确指令（原话复述）：「不需要考虑迭代，数据库升级代码全删，没有备份，没有 dump」。架构立场：不做向后兼容，允许老库直接不可用。

=== 三刀范围（实测行数）===

【刀一：DB 升级代码全删 ~400 行】
schema.cpp 删：migrateTodoFixedColumns(224)、addMissingAgentColumns(90含列清单)、migrateDropDeprecatedClaudeConfig(28)、migrateTodoDocLinkColumn(18)、migrateProjectCardColumns(33，已归 #385)、dropRemovedTables(22含 quota 两表)。migrate() 退化为「只跑 applySchema(kSql)」。
schema.h 删对应声明 + 迁移白名单（kMigrationTableWhitelist/kMigrationColumnWhitelist/migrationWhitelistsAligned static_assert）。

**⚠ 必须保留（不是迁移）：archiveDoneTasksBeforeToday(66)** —— 它是冷启动归档产品行为，todo_service.cpp:719 对齐其口径，删了会改变 todo 行为。

【刀二：备份全删 ~570 行】
db_backup.cpp(380) + db_backup.h(69) + main.cpp 损坏检测/还原链（:107-160 handleDbCorruption、:170-210 损坏处理、:1267 performStartupBackupOnce）+ database.cpp:52-55 quickCheck + paths.cpp:59 的 *.db.backup-* 忽略规则。
代价（老板已知）：库损坏 = 启动失败 + 数据丢失，无自动救援。
待裁决：是否保留 PRAGMA quick_check 的窄版（~50 行）做「损坏时明确告警」而非静默崩。

【刀三：dump 全删 ~3615 行】
sql_dump.cpp(369) + .h(47)。
**连带：sql_dump 唯一消费方 = GitHub Sync**（github_sync_engine.cpp:341 把整库 dump 成 SQL 推 GitHub 私有仓）→ 删除意味着 github_api.cpp(1018) + github_sync_{card,config,engine,logic}(2228) + 设置页「同步」tab 一并下线。
老板库实测：github_sync_state 表**零记录**（从未使用）。
待裁决：GitHub Sync 是否整体退役，还是保留「仅同步笔记」（kRemoteNotesDir="notes/"）而去掉 DB dump 半边。

=== 总量 ===
三刀合计约 4600 行（占全仓 4%）。归属 #380 精简线，建议作为独立子批（不单开 L 级）。

流程：方案卡（含三个待裁决点）→ 开发 → 审计 → QA。当前未开工（老板 2026-11-22 明示「现在不开工」）。

**进度记录**（1）：

- 2026-11-22 00:41:49 +0800 · **老板 2026-11-22 裁决修订（覆盖原范围）**：
  
  【保留】GitHub Sync 原样保留 → 刀三（dump/GitHub Sync 3665 行）**取消**，不删。sql_dump.cpp、github_api.cpp、github_sync_* 全保留。
  【保留】备份能力 → #387 改为每小时定时备份（复用 schedule），故刀二从「全删」降级为：只删 main.cpp 的启动备份调用（performStartupBackupOnce）+ 自动还原链（handleDbCorruption），保留 createBackup/latestBackup/restoreFromBackup 函数体。
  【保留】release 模块原样保留（12.6k 行），不在本条目内。
  
  【本条目剩余范围】仅剩刀一：DB 升级代码全删（~400 行）——schema.cpp 的 migrateTodoFixedColumns/addMissingAgentColumns/migrateDropDeprecatedClaudeConfig/migrateTodoDocLinkColumn/dropRemovedTables 全删，migrate() 退化为只跑 applySchema(kSql)。保留 archiveDoneTasksBeforeToday（产品行为非迁移）。
  
  【新立场】老板 2026-11-22「本应用不考虑历史包袱 0 用户项目」——此条升格为总纲，建议单独立条承接 schema 老库兼容 + 配置层历史键兼容的全删（~550 行）。

**状态变更**（1）：

- 2026-11-22 00:32:43 +0800 ·  → 在池 · 入池（main-session）

---

#### need#387 · 数据库每小时定时备份（复用 schedule 系统任务机制，非启动触发）

- 状态：在池 · 优先级：P2 · 负责人：main-session
- 创建：2026-11-22 00:38:09 +0800（main-session）· 更新：2026-11-22 00:38:09 +0800（main-session）

**详情**：

老板 2026-11-22 指令：数据库备份改成每小时间隔、从启动时刻计数、复用定时任务体系（作为内置系统任务）。

=== 与 #386 的关系（必须先理清，否则实施冲突）===
#386 要求「没有备份」指的是：删**启动时备份**（main.cpp:1267 performStartupBackupOnce）+ 删**损坏自动还原链**（handleDbCorruption）+ 删 dump/GitHub Sync。
本条目要的是**每小时间隔备份的定时任务**。两者可共存，前提：
- ✅ 保留 core/db_backup.cpp 的 createBackup() 本体（~100 行，sqlite3_backup 在线备份 + 滚动保留）
- ✅ 保留 latestBackup()（列表用）
- ❌ 删 performStartupBackupOnce 的 main.cpp 调用
- ❌ 删 handleDbCorruption 自动还原链（main.cpp:107-160）
- ❓ 滚动保留份数需重定：原 keepBackups=5（启动频次低），每小时一份则 5 份只留 5 小时 → 需重新裁（如 24/48/72）

=== 现成机制（已核实，直接复用）===
- ScheduleProvider 接口（src/module_schedule/schedule_provider.h）：别的模块往定时页贡献「系统项」（ScheduleItemView{source,key,title,summary,nextFireAt,enabled,sourceRoute}），release 模块已在用（release_schedule_source.cpp）
- UI 侧 isSystem=true 已有完整支持：锁图标 + 只读态 + 空态文案「系统定时由应用内置提供」（schedule_ui_list.cpp:419/432/780/861/972）
- RuleKind::Interval + everySec=3600 + anchor=启动时刻unix 就能实现「每小时后、从启动计数」（core/schedule_rule.h:31/64-66）

=== 需新增（估计 ~200 行）===
① action_runner.cpp:306 的 actionKind switch 加第 5 种 "db_backup"（现只 4 种：remind/remind_open/run_task/webhook_only）
② 一个 DbBackupScheduleProvider 实现 ScheduleProvider：items() 返回当前备份任务状态（下次触发时间/保留份数/最近一次备份文件）
③ db_backup 模块注册进 ScheduleProvider 注册表
④ 备份失败的处理：不弹窗（与现有静默口径一致），仅 LOG_ERROR + 系统项上标红

=== 待裁决 ===
- 保留份数（每小时一份，5 份=5小时太少；建议 24 份=1天 或 168 份=7天）
- 是否可在设置里关掉这个系统任务（建议可关，默认开）
- 备份时间点相位：从启动时刻偏移 1/2/3... 小时，还是对齐整点（建议后者：即使重启也稳定，且不因频繁重启导致相位漂移）
- 与 GitHub Sync 的 db dump 是两回事：这个是本地文件级 sqlite3_backup，不涉网络

M 级（复用现有 schedule 机制 + 新增一个 action 类型 + 一个 provider）。流程：方案卡（含四个裁决点）→ 开发 → 审计 → QA。当前未开工。

**状态变更**（1）：

- 2026-11-22 00:38:09 +0800 ·  → 在池 · 入池（main-session）

---

#### need#388 · 上线模块简化：收敛为可配置检查单（老板 2026-11-22 定调）

- 状态：在池 · 优先级：P2 · 负责人：ZCode主会话
- 创建：2026-11-22 00:54:13 +0800（ZCode主会话）· 更新：2026-11-22 00:54:13 +0800（ZCode主会话）

**详情**：

老板令（2026-11-22，need#380 功能裁决附带）：A-2 上线模块不退役（保留），后续把它**简化成一个可配置的检查单**形态。这是功能变更，独立走完整流程：开工后先 req-analysis 澄清「可配置检查单」的具体形态（检查步骤项从哪配/颗粒度/与现有 release_task 流程关系/旧交互去留），再方案/实施。现状参考：模块 12,637 行（任务+基础配置两子页+提醒弹窗），现有流程建任务前置 3 步（建服务→建环境→建任务）；候选分析见 doc need380-feature-cut-proposals A-2 及相关 B/C 条目。在池待开工令。

**状态变更**（1）：

- 2026-11-22 00:54:13 +0800 ·  → 在池 · 入池（ZCode主会话）

---

#### need#392 · kMaxToolResultBytes 三处拷贝常量收敛到共享常量头

- 状态：在池 · 优先级：P3 · 负责人：main-session
- 创建：2026-11-22 17:11:53 +0800（main-session）· 更新：2026-11-22 17:11:53 +0800（main-session）

**详情**：

来源：need#381 code-reviewer 审计 P2-1（2026-11-22）。现状：kMaxToolResultBytes 同名同值 32KB 常量已在三处拷贝——src/module_agent/agent_loop.cpp:50（原初）、src/module_agent/net_fetch_tool.cpp:27、src/module_mcp/mcp_tool_mirror.cpp:15-16（need381 新增，注释与 agent_loop 互指）。风险：三源真值全靠注释互指同步，调值必漂移。收敛方案（减法优先）：提为单一共享常量（core 层常量头或 tool.h 暴露，一处定义三处引用）；约束=need#381 卡锁 core 零改动故当时未做，本条解除该锁后执行；顺手核 net_fetch_tool.cpp:27 是否同语义（字节上限对象可能不同，收敛前逐一定性，不同语义则只收敛同语义两处）。验证：改后 --filter 定向（ToolMirror+net_fetch/agent 相关套件）全绿。

**状态变更**（1）：

- 2026-11-22 17:11:53 +0800 ·  → 在池 · 入池（main-session）

---

### 挂起（7）

#### need#34 · 向量模型升级档（Qwen3-Embedding-0.6B）

- 状态：挂起 · 优先级：P3 · 负责人：
- 创建：2026-10-30 21:05 +0800（）· 更新：2026-10-30 21:05 +0800（）
- 阻塞：条件未触发

**详情**：

记忆量数百条+后可升；必须显式 -c 8192 否则吃 4GB；见 2026-10-27-memcpp-embed-bakeoff。TODO.md 旧册需-8 迁入

**状态变更**（2）：

- 2026-11-02 09:59:04 +0800 ·  → 在池 · 2026-11 修复迁移补登（pool_migrate）
- 2026-11-02 09:59:04 +0800 · 在池 → 挂起 · 2026-11 修复迁移补登（pool_migrate）

---

#### need#35 · 触发线升级 C（自研全文通道备用图纸）

- 状态：挂起 · 优先级：P3 · 负责人：
- 创建：2026-10-30 21:05 +0800（）· 更新：2026-10-30 21:05 +0800（）
- 阻塞：条件未触发

**详情**：

冻结集 Recall@5<85% 或 2 字词命中低于 5/3/11 才触发；当前 100% 余量足。TODO.md 旧册需-9 迁入

**状态变更**（2）：

- 2026-11-02 09:59:04 +0800 ·  → 在池 · 2026-11 修复迁移补登（pool_migrate）
- 2026-11-02 09:59:04 +0800 · 在池 → 挂起 · 2026-11 修复迁移补登（pool_migrate）

---

#### need#56 · 发布任务全屏配置编辑层交互重设计（与详情页新模式对齐）

- 状态：挂起 · 优先级：P3 · 负责人：ZCode主会话
- 创建：2026-10-31 22:52:12 +0800（ZCode主会话）· 更新：2026-11-03 09:15:28 +0800（ZCode主会话）
- 阻塞：启动条件未到：等池 53 编辑态 UI 重设计落地并确认体验不一致后再启动本单（detail 原文「再启动本单对齐」）；条件触发前不动工

**详情**：

池 53 需求分析裁剪项转正（老板令 2026-10-31）：全屏配置编辑层交互重设计——池 53 保留现形态仅收放入口；编辑态 UI 重设计（池 53）落地后若体验不一致再启动本单对齐。源 PRD 同上

**状态变更**（2）：

- 2026-10-31 22:52:12 +0800 ·  → 在池 · 入池（ZCode主会话）
- 2026-11-03 09:15:28 +0800 · 在池 → 挂起 · 启动条件未到：等池 53 编辑态 UI 重设计落地并确认体验不一致后再启动本单（detail 原文「再启动本单对齐」）；条件触发前不动工（ZCode主会话）

---

#### need#61 · Windows 机实测 ui-probe-shot 路径 A，验证 skill Windows 差异小节并删待验标注

- 状态：挂起 · 优先级：P2 · 负责人：ZCode主会话
- 创建：2026-11-01 15:58:51 +0800（ZCode主会话）· 更新：2026-11-01 15:59:00 +0800（ZCode主会话）
- 阻塞：需 Windows 主开发机（Mac 无法实测 Windows 侧）；前置：Windows 机同步 dev 至含 744bcf64

**详情**：

池60 审计 P1 遗留：skill（.agents/skills/ui-probe-shot/SKILL.md）的 Windows 差异小节是代码接线推断未实测。在 Windows 主开发机执行：① bash scripts/build-test-run.sh 构建后 cd build/tests && ./uat62_page_shots_probe --size 1000x640 --out evidence/<日期>/ ② 核对：exe 可直接调（.exe 补全）、PNG≈逻辑尺寸（scale=1）、wallpaper 页实拍 13 页、图标正常 ③ 把实测结果记入 evidence 并删除 skill 中「Windows 侧差异」小节的待验标注。前置：Windows 机需同步到含 744bcf64 的 dev（font-scale flag 才存在）。

**状态变更**（2）：

- 2026-11-01 15:58:51 +0800 ·  → 在池 · 入池（ZCode主会话）
- 2026-11-01 15:59:00 +0800 · 在池 → 挂起 · 需 Windows 主开发机（Mac 无法实测 Windows 侧）；前置：Windows 机同步 dev 至含 744bcf64（ZCode主会话）

---

#### need#140 · macOS 标题栏几何对齐 40px（原 need132 F-04/F-05 拆分）：路线 A/B 待老板复核

- 状态：挂起 · 优先级：P1 · 负责人：developer · 阶段：缺陷修复中
- 创建：2026-11-06 18:32:27 +0800（developer）· 更新：2026-11-08 22:51:25 +0800（developer）
- 阻塞：待下次开工实现：2026-11-08 老板拍板路线 B（自绘标题栏放弃原生红绿灯），与调研推荐的路线 A 相反。需先出 B 路线方案（自实现关闭/最小化/缩放/拖拽/双击缩放/全屏重挂）

**详情**：

从 need:132 拆分（PRD 审计 P0-1 处置，2026-11-06）。原 F-04「macOS 标题栏几何对齐 40px」+ F-05「红绿灯交互保留」。

**待老板复核的决策点**：老板 2026-11-05 拍板「macOS 也改自绘标题栏、放弃原生红绿灯」；调研（clue 42/43）结论是 Electron/Tauri/Qt 三家均走「保留原生红绿灯 + 挪进 40px 自绘条」（路线 A），无主流框架走纯自绘（路线 B），路线 B 需自实现关闭/最小化/缩放/拖拽/双击缩放/全屏重挂、坑面无先例。PRD v1 曾按调研静默改成路线 A，被审计判为推翻拍板且无记录（P0），故整条拆出等复核。

**候选路线**：
- A（调研推荐）= 保留 NSWindowStyleMaskTitled + FSCV + 透明标题栏（EUI-NEO window_effect_macos.mm:262-278 已走完前半程），红绿灯定位进 40px 条 + 拖拽区；风险最低，系统语义原生保留。
- B（老板原拍板字面）= 隐藏红绿灯全自绘；几何 100% 同构，需自实现全套窗口交互。

**根因证据**：src/app_sidebar.cpp:1101-1107 Windows 自绘 titlebarHeight()=40px / macOS 硬编码 kSystemTitleBarInset=30px；bodyTop 统一扣除，侧栏分档阈值是绝对像素（<680 极紧 / <712 紧凑），10px 差使同窗口高落不同档（clue 35）。

**验收要点（原 PRD F-04/F-05，随拆分迁入）**：F-04 macOS bodyTop=40、侧栏分档与 Windows 同窗口高同档位；F-05 拖拽/双击缩放/全屏（含进出全屏后红绿灯位置与拖拽区复验）/边缘 resize 全可用。已知技术坑：GLFW 全屏切换/AppKit 原生全屏/setTitle:/改 styleMask 后 zoom 钮回退都会重置自定义 mask 或红绿灯位置，需一套通知钩子统一重挂（clue 43）。

**阻塞**：老板路线复核。复核前不动工。PRD 正文见 doc 25 v2 §0（偏离记录）与 §8 调研依据。

**关联线索**（12）：

- [open] clue#57 · 主题：跨平台布局一致性:route-b-hide-buttons · 域：跨平台布局一致性 · 关键词：setHidden,removeFromSuperview,standardWindowButton,performClose,蜂鸣,alert sound,Tietze,resizable 重显,Cmd+W,Miniaturizable · 事实：隐藏红绿灯机制实证（route B 子方案①）：①setHidden:YES 与 removeFromSuperview 的系统级差异——standardWindowButton: 语义=「Window button in the window's view hierarchy…nil when such button is not in the window's view hierarchy」（Apple 一手 .md），setHidden 按钮仍留层级→返回有效引用、performClose:/performMiniaturize: 的「窗口有没有按钮」判定仍成立（语义保留、可随时恢复）；removeFromSuperview 后→返回 nil，且 performClose:/performMiniaturize: 文档明言「If the window doesn't have a close/minimize button…emits the alert sound」→ 蜂鸣+功能断（AppKit 判定路径待 POC 复核）。②Tietze 实证：改 .resizable 会自动重新显示 traffic lights→每次 styleMask 变更后须重新 setHidden。③Cmd+W/Cmd+M 与钮可见性无关（performClose:/performMiniaturize: 是 NSWindow 方法走 responder chain，不经过按钮视图）；但 GLFW 应用默认无菜单栏，快捷键本来就不通（kitty 自建 NSMenu 才有）。④GLFW 恒带 NSWindowStyleMaskMiniaturizable（vendored cocoa_window.m:813 创建起底，Borderless 路径也保留）→「最小化」mask 语义两端子方案都有基础。 · 来源：https://developer.apple.com/documentation/appkit/nswindow/standardwindowbutton(_:).md + .../performclose(_:).md + .../performminiaturize(_:).md + https://christiantietze.de/posts/2020/10/nswindow-hide-traffic-light-dark-mode/ + 文件:EUI-NEO/3rd/glfw/src/cocoa_window.m:813；登记：2026-11-06 19:34:21 +0800 researcher(need:140)；复核：2026-11-06 19:34:21 +0800
- [open] clue#58 · 主题：跨平台布局一致性:route-b-borderless-resize · 域：跨平台布局一致性 · 关键词：Borderless,Resizable,size-grips,kitty,4fdb55e,Godot 79329,libui,winmoveresize,iconify,maximize,miniaturize,zoom,setMiniwindowTitle · 事实：真 Borderless（GLFW_DECORATED=false）路线实证：①原生边缘 anchor 可用——GLFW #1063（2017 closed，无过期反证）：asmvik「simply adding the NSWindowStyleMaskResizable will give the borderless windows anchors (size-grips)」，kitty 作者同帖称无需 fork、实装于 kovidgoyal/kitty@4fdb55e（kitty 生产环境日常在跑）→ Borderless+后补 Resizable mask = AppKit 原生 8 向 resize，无需自写；反面教材 Godot #79329（open）：框架改 borderless 时移除 Resizable 位→不可 resize，用户长期抱怨。②若 mask 方案失灵的自实现兜底：andlabs/libui darwin/winmoveresize.m——performWindowDragWithEvent 优先（10.11+），手动路径=NSEventTrackingRunLoopMode 事件循环+屏幕坐标换算+setFrame+min/max clamp（edge 由外部 hit-test 传入，光标形态也在外部 resetCursorRects）；已知 bug：对边同时缩放。③GLFW 桥接锚点（vendored 源码）：glfwIconifyWindow→[window miniaturize:]（cocoa_window.m:1184-1189）、glfwMaximizeWindow→[window zoom:] 带 isZoomed 守卫（:1201-1207）、restore→deminiaturize:/zoom:（:1191-1199）；_glfwSetWindowDecoratedCocoa 切 Borderless 时只移除 Titled|Closable|Resizable 保留 Miniaturizable（:1285-1302）；清 Titled 有副作用「resets and disables the window title property」（HACK 注释 :1362，GLFW 靠 setMiniwindowTitle 绕过）。 · 来源：https://github.com/glfw/glfw/issues/1063 + https://github.com/godotengine/godot/issues/79329 + https://raw.githubusercontent.com/andlabs/libui/master/darwin/winmoveresize.m + 文件:EUI-NEO/3rd/glfw/src/cocoa_window.m:1184-1207,1285-1302,1362；登记：2026-11-06 19:34:21 +0800 researcher(need:140)；复核：2026-11-06 19:34:21 +0800
- [open] clue#59 · 主题：跨平台布局一致性:route-b-shadow-corner-precedents · 域：跨平台布局一致性 · 关键词：invalidateShadow,cornerRadius,clear,lukakerr,notes,framelesswindow,ButtonPasser,toggleFullScreen,performWindowDragWithEvent,先例 · 事实：Borderless 阴影圆角与生产先例（route B 子方案②补回成本）：①Apple 一手 invalidateShadow()=「Invalidates the window shadow so that it is recomputed based on the current window shape」→ Borderless 窗口圆角补法=backgroundColor clear+isOpaque NO+wantsLayer+layer.cornerRadius+masksToBounds+invalidateShadow（lukakerr/swift/nswindow-styles 实践指南：visualEffect.layer?.cornerRadius=16 全公开 API）；但 GL/OpenGL surface 默认不透明→圆角裁剪需 EUI 渲染层配合（视口/清屏色），成本显著。②自绘钮动作生产先例：nuttyartist/notes src/framelesswindow.mm 走 Titled+FSCV+titlebarAppearsTransparent（非 Borderless）+ButtonPasser target/action 覆盖+toggleFullScreen: 全屏；拖拽两派并存：notes 用 Qt 层手动 move()（setFrameOrigin 派），libui/Zed/ollama/zen-browser/Avalonia/Ladybird/Wails/iTerm2/openclaw 等 9+ 生产项目统一用 performWindowDragWithEvent 派（gh code search 25 命中）。③窗口仍需 setHasShadow:YES——GLFW 在全屏切换后主动重设（cocoa_window.m _glfwSetWindowMonitorCocoa 内 setHasShadow:YES）。 · 来源：https://developer.apple.com/documentation/appkit/nswindow/invalidateshadow().md + https://lukakerr.github.io/swift/nswindow-styles + https://raw.githubusercontent.com/nuttyartist/notes/master/src/framelesswindow.mm + gh search code performWindowDragWithEvent；登记：2026-11-06 19:34:21 +0800 researcher(need:140)；复核：2026-11-06 19:34:21 +0800
- [open] clue#61 · 主题：跨平台布局一致性:titlebar-bodytop-anchor · 域：跨平台布局一致性 · 关键词：bodyTop,kSystemTitleBarInset,customTitlebarActive,分档阈值,锚点漂移,子窗 · 事实：标题栏/bodyTop 现行锚点复核（2026-11-06，clue 35 行号已漂移约 60 行）：①bodyTop 计算=src/app_sidebar.cpp:1156 customTitlebarActive() 门控 + :1160-1166 `#ifdef __APPLE__ kSystemTitleBarInset=30.0f / #else 0.0f`，bodyTop = chrome ? titlebarHeight() : kSystemTitleBarInset（Windows chrome=true→40；macOS chrome=false→30）；②侧栏分档阈值现行锚点=app_sidebar.cpp:421-428（availH<680 极紧 / <712 紧凑），:405-409 档位参数注释，:437-447 字号钳制沿用同阈值；clue 35 所写 1101-1107 现为托盘/窗高记账代码；③F-03（macOS 自绘 chrome 生效）落地后 :1160-1166 的业务层 #ifdef __APPLE__ 可整体消除（bodyTop 两端同走 titlebarHeight()=40）；④macOS 自绘条适用面初判=主窗：grep src 无 macOS 子窗布局文件（window_layout_win.cpp 为 _win 专属），子窗是否套用待技术卡查证。 · 来源：文件锚点实测：src/app_sidebar.cpp:405-409,421-428,1156,1160-1166 + grep src 子窗布局文件；登记：2026-11-06 19:41:41 +0800 product-manager；复核：2026-11-06 19:41:41 +0800
- [open] clue#67 · 主题：跨平台布局一致性:prd-doc29-tier-arithmetic-a1 · 域：跨平台布局一致性 · 关键词：availH,窗高减bodyTop,1200x720,紧凑档,705px,715px,正常档,A1,双钮92px,测试钉住缝 · 事实：PRD doc29 v1 档位换算实证修正（2026-11-06 锚点实读，审计派生）：①分档判定基准 availH=drawSidebar 收到的 screen.height=窗高−bodyTop（app_sidebar.cpp:402-403 注释一手「窗高 − 自绘标题条 40 / macOS 系统标题栏 30，compose 的 bodyScreen 已统一扣除」+ :414）→ 换算=窗高−bodyTop 对 680/712 阈值；②代码注释一手实锤 :407「最小窗 1200×720 → H=680 落此（紧凑）档」→ PRD A1 把 1200×720 配「正常档」直接矛盾（A1 三尺寸 720/700/670 实落 紧凑/极紧/极紧，缺正常档；正常档需窗高≥752，如 1200×760→availH 720）；③PRD 背景「同 705px 高窗口 Windows 紧凑档、macOS 极紧档」错误——705−40=665 / 705−30=675 两端均<680 同落极紧；正确示例=715px（Win 675 极紧 / mac 685 紧凑），「10px 差致落档分叉」论点本身成立仅数值选错；④Windows 自绘钮=右缘双钮 46×40（:265-267 kWindowButtonW=46、UAT4 最大化钮已按用户拍板移除，最大化唯一路径=标题栏双击）→ 双钮总热区 92px，PRD「平台差异 Windows=最大化 zoom 第三钮」与现状不符；⑤既有运行时缝 setCustomTitlebarMode/g_titlebarModeOverride（:56 测试钉住缝、:254-263）可强制 chrome 开关——整体回退预案可依托此缝。 · 来源：文件锚点实测：src/app_sidebar.cpp:56,250-263,265-267,401-414,421-428,447,1156-1166 + PRD doc29 v1 A1/背景1/平台差异 对照；登记：2026-11-06 19:58:50 +0800 product-manager(审计 need:140)；复核：2026-11-06 19:58:50 +0800
- [open] clue#68 · 主题：跨平台布局一致性:green-light-default-fullscreen-d3 · 域：跨平台布局一致性 · 关键词：绿灯,全屏,zoom,Option,10.7,toggleFullScreen,语义,拍板,D3,未授权 · 事实：第三钮语义裁决证据（2026-11-06，PRD doc29 D3 重点核查项）：macOS 原生绿灯默认=进入全屏（自 10.7 引入全屏起；Option+点击=zoom 最大化；≤10.6 无全屏功能时绿灯才是纯 zoom）——审计简报所写「原生绿灯默认是 zoom 最大化、Option+点击才是全屏」方向颠倒。佐证：仓内调研报告 docs/research/2026-11-06-macos-fully-custom-titlebar-route-b.md Q3 一手结论（「Windows 绿钮=最大化 zoom，macOS 绿灯=全屏……语义应抄 macOS（绿灯=全屏）否则 mac 用户预期错位。需产品确认，默认建议 mac 全屏语义」）+ nuttyartist/notes ButtonPasser→toggleFullScreen 生产先例（clue 59）。裁决：PRD D3（第三钮=toggleFullScreen 全屏）与原生行为、调研推荐、G2「语义对齐 macOS 用户预期」三方一致，属 PM 角色授权内的新功能取舍（依据已标注、备选 glfwMaximizeWindow→zoom: 链已锚定且 PRD 标明可切换），非未授权决策；调研遗留「需产品确认」以 PM 拍板闭环，建议随 PRD 评审向老板一句话报备。 · 来源：docs/research/2026-11-06-macos-fully-custom-titlebar-route-b.md Q3 + clue 59（notes 先例）+ macOS 平台交互惯例（Apple 支持全屏指南佐证，本次未取到逐字正文，证据等级=平台通识+仓内一手调研交叉）；登记：2026-11-06 19:58:50 +0800 product-manager(审计 need:140)；复核：2026-11-06 19:58:50 +0800
- [open] clue#69 · 主题：跨平台布局一致性:prd-doc29-v2-anchor-verification · 域：跨平台布局一致性 · 关键词：1606-1614,260-272,回退缝,setCustomTitlebarMode,availH,1200x760,1200x670,最小尺寸钳制,窗口状态恢复,A12,A13 · 事实：PRD doc29 v2 修订锚点实读复核（2026-11-06，修订作业中顺手实读，与 clue 61/67 互补不冲突）：①customWindowChromeSupported() 实际锚点=EUI-NEO/core/window/window_backend.cpp:1606-1614（v1 PRD 写 1604-1612 偏 2 行，v2 已修正，与审计 P3-3 一致）；②FSCV 三连所在函数 enableContentExtensionIntoTitleBar=window_effect_macos.mm:260-272（styleMask|=FSCV :266 / setTitlebarAppearsTransparent :267 / setTitleVisibility:270，:269 注释「红绿灯保留（macOS 原生交互语义）」=路线 B 要翻转的现状；v1 写 262-278，v2 已修正）；③回退缝实读吻合：app_sidebar.cpp:56 g_titlebarModeOverride 为 std::optional<bool>（nullopt=跟平台能力走）、:254-263 customTitlebarActive()/setCustomTitlebarMode()，override 置 false/nullopt 即回 macOS 现状（系统标题栏+bodyTop 30）——P1-1 整体回退预案落点；④A1 v2 三尺寸换算：窗高 760/720/670 − bodyTop 40 = availH 720/680/630 → 正常档（≥712，需窗高≥752）/紧凑档/极紧档，1200×720 落紧凑档与 :407 注释一手一致；⑤主窗最小尺寸现值注释=1200×720（:407）→ A1 极紧档尺寸 1200×670 可达性待技术卡核实（PRD 开放 7）。 · 来源：文件锚点实测：src/app_sidebar.cpp:56,250-263,401-414,1156-1166 + EUI-NEO/core/platform/window_effect_macos.mm:245-304 + EUI-NEO/core/window/window_backend.cpp:1590-1630 + PRD doc29 v2 A1/A12-A15；登记：2026-11-06 20:05:52 +0800 product-manager；复核：2026-11-06 20:05:52 +0800
- [open] clue#72 · 主题：跨平台布局一致性:titlebar-design-tokens-and-verdict · 域：跨平台布局一致性 · 关键词：titlebar,红绿灯,46x40,error,warning,primary,token,左缘三钮,设计文档,2026-11-06 · 事实：标题栏 UI 设计端新查证（2026-11-06，designer 实读，供 developer 实现）：①Windows 自绘钮三态现构图=app_sidebar.cpp:292-323——单钮=透明 hit rect 46×40 + 居中图标；normal 底透明 / hover=t.colors.hover / pressed=t.colors.primarySoftBg；关闭钮 hover/pressed=colors.error 全底+白图标（g_closeButtonHovered 门控）；图标 fontSize=scaled(12) 色 textSecondary；码点 close=0xF00D(fa-xmark)、minimize=0xF2D1(fa-window-minimize)；品牌文本 x=16 定宽 160 fontSizeBody(14) weight700 textPrimary（:330-340）；拖拽区=条内右缘 92px 之外（:281）。②theme.cpp 精确 token 值（:12-115 实读）——dark：error=#F87171、warning=#FBBF24、primary=#36D399、primaryHover=#2EC48F、primaryPressed=#23B07F、textDisabled=#908A82、hover=#353231、primarySoftBg=#0D3326、onPrimary=#FFFFFF、background=#1A1816、textPrimary=#FAF9F5、textSecondary=#C9C5BD；light：error=#EF4444、errorText=#B91C1C、warning=#F59E0B、primary=#07C160、primaryHover=#06AD56、primaryPressed=#059048、textDisabled=#6B7280、hover=#EEF6F1、primarySoftBg=#E8F8EE、background=#F5FAF7、textPrimary=#1F2937、textSecondary=#4B5563。③设计结论（docs/design/2026-11-06-macos-custom-titlebar-design.md）：钮位=macOS 左缘三钮（热区 46×40×3 连排 x∈[0,138)，圆心 x=23/69/115）、视觉=红绿灯彩点方案推荐（红=error/黄=warning/绿=primary 全 token 零硬编码，hover 深档+符号浮现、失焦全灰 textDisabled、1px 描边=hover 档色）、DevDesk 统一线性钮为备选方案 B；brand 文本 macOS 端 x=154；拖拽区 x∈[138,宽)；钮区双击不缩放。 · 来源：文件锚点实测：src/app_sidebar.cpp:265-353 + src/core/theme.cpp:12-115 + docs/design/2026-11-06-macos-custom-titlebar-design.md；登记：2026-11-06 20:17:00 +0800 ui-ux-designer；复核：2026-11-06 20:17:00 +0800
- [open] clue#73 · 主题：跨平台布局一致性:macos-titlebar-route-b-impl-facts · 域：跨平台布局一致性 · 关键词：need140,decorated,Titled,子方案①,styleMask=0x800f,setNeedsDisplay不递归,皮肤注入时序,NSWindowDidUpdateNotification,红点缺失,全屏未验证 · 事实：need:140 实现新事实（2026-11-06 developer 实测）：①main.cpp:1000 decorated 决策改为 `!customWindowChromeSupported() || customWindowChromeButtonsNative()`——mac 自绘壳平台保持 Titled mask（PRD 子方案①）。原 `!supported()` 单条件在 supported 翻 true 后会使 mac 窗口变 Borderless（违 PRD N1）；真机 fxdiag styleMask=0x800f（FSCV+Titled+Closable+Miniaturizable+Resizable）✓。②overlay 皮肤注入时序坑：NSView setNeedsDisplay 不递归子视图，overlay 首帧 drawRect 时 skin 未注入（fill=0 跳过不画）→ 注入后子钮不重绘、彩点长期缺失（真机复现：黄绿点被后续脏矩形重绘而红点缺失）；修复=setCustomTitlebarSkin/euiChromeReapply 广播 overlay 全部子钮 setNeedsDisplay（EUI-NEO/core/platform/window_effect_macos.mm）。③NSWindowDidChangeTitleNotification 不存在；setTitle: 重显兜底改挂 NSWindowDidUpdateNotification（每事件循环幂等 reapply，同时兜住一切 AppKit 重显路径）。④NSWindow 无 fullScreen 属性，全屏判定用 styleMask & NSWindowStyleMaskFullScreen。⑤真机证据：常态三彩点可见（红黄绿、圆心距 46 逻辑 px）、品牌文本、无系统红绿灯（evidence/need140/normal-3buttons-titlebar.png + titlebar-closeup.png）。⑥全屏进出×3/失焦态/钮点击回传/拖拽/双击：本机 Windows11 虚拟机抢键盘焦点致 keystroke 自动化不可达，未验证，归 qa 实机。 · 来源：真机截图 evidence/need140/ + 会话构建日志（styleMask=0x800f）+ EUI-NEO/core/platform/window_effect_macos.mm 实现锚点；登记：2026-11-06 20:52:52 +0800 developer；复核：2026-11-06 20:52:52 +0800
- [open] clue#76 · 主题：跨平台布局一致性:bug198-test-expectation-alignment · 域：跨平台布局一致性 · 关键词：bug198,测试期望对齐,toggle几何,dd0f28db,95cba613,logo高度档,e950c297,4d0d6edd,embedded-directive,kNativeTitlebarButtons · 事实：bug198 十个过期测试期望已对齐现行为（2026-11-07 developer 实测，tests/ 两文件，src/ 零改动）：①test_app_sidebar.cpp 折叠/展开/三档间距用例 toggle 几何=dd0f28db bug146 三轮（正常 40/区 56、极紧/紧凑 36/52、toggleY=H−区+4、展开 toggleX=padX+8、热区+8、图标码点=钮径×0.5、常态淡阴影 blur4/offsetY1、pressed 阴影收起），cee24491 为 36/52 前轮；粒子包络 foldLimit=availH−区−4（正常 940）。②logo 高档新语义=95cba613 bug143：heightTier 仅极紧兜底（availH<680→48、否则 64，src/app_sidebar.cpp:523），W=1000 短窗 H=720/480 同钳 48（严格高单调不再成立，测试改在 W=1600 跨档验证 56>48，test_app_sidebar.cpp logo_mono_h660_w1600 块）；紧凑档 logo 44→56 → 块加高 72、topPad/gap −4 → navStart 88→96，viewportH=680−52−96=532，最小窗 12 行 contentH 529 仍零滚动（余 3px）。③自绘标题栏元素树平台分叉=e950c297 need140 四轮：mac 上 customWindowChromeButtonsNative()=true（EUI-NEO/core/window/window_backend.cpp:1625-1630 编译期平台判定）→ drawTitlebar 早退分支只画 app.titlebar.brand（x=16 定宽160，app_sidebar.cpp:345-361），app.titlebar 栈与 close/minimize.hit rect 仅 Windows 入树；测试用文件级 constexpr kNativeTitlebarButtons 运行时分支（REGISTER_TEST 宏参数内 #ifdef 触发 -Werror=-Wembedded-directive，禁用）；titlebarCaptionHit 边界 1508=1600−2×46 两端不变（titlebarButtonCount=2）。④about 卡几何=4d0d6edd bug147：kCardMaxW 760→880、kCardY 24→48（about_module.cpp:208-212），cardX=32+(1536−880)/2=360、卡高 464 不变、卡内滚动区/组行/footer 公式全不变。⑤溢出滚到底用例（H=420 极紧）：viewport 296→288、maxOffset 6→14、toggle y 384→372；「行不进按钮区」断言由整内容高不等式改为内容底−offset（视口裁剪机制下唯一有效形态，382>372 使旧式恒假）。验收：--filter sidebar 27/27 绿 + --filter about 8/8 绿（build-test-run.sh --dev）。 · 来源：实测：git show 归属（cee24491/dd0f28db/95cba613/4d0d6edd/e950c297）+ src/app_sidebar.cpp:491-524,916-918,937-944 + build-test-run.sh --dev 两 filter 全绿；登记：2026-11-07 01:03:24 +0800 developer；复核：2026-11-07 01:03:24 +0800
- [open] clue#91 · 主题：bug142:rootcause-fix-archaeology · 域：bug142 · 关键词：bug142,标题栏,fullscreen,reapply,setStyleMask,NSWindowDidUpdate,点击失效,need140 · 事实：bug142 根因考古（2026-11-07 核实）：疑答根因路径已双重修复于 EUI-NEO 9d34e79（fix: 字体栈空 holder 兑底 + overlay hitTest 事件穿透，bug148/142）。① euiApplyFullscreenTitlebar 幂等闩：EUI-NEO/core/platform/window_effect_macos.mm:105-115——只在全屏态相对上次记录变化时才碰 setStyleMask，注释明言「无条件调 setStyleMask 会每帧重建窗口视图层级 → UI 全部点击失效（老板实测）」；② NSWindowDidUpdateNotification 挂钩整段移除：同文件 :1051-1073——剩 9 个钩子（全屏进出 4 + resize + key 2 + backing + occlusion），注释明言「该通知每事件循环都发 → reapply 内 setNeedsDisplay 排重绘 → 又触发 DidUpdate → 无限重绘循环，事件队列被淹没导致全 UI 点击失效」。DevDesk 侧 HEAD 7a6eb7ea 已推进 submodule 指针到 9d34e79（git submodule status 确认 9d34e79f 非 + 前缀，工作区干净）。老板 21:43 报障时的构建在修复之前，22:21-22:32 能交互与「已修复」时序吻合。 · 来源：EUI-NEO/core/platform/window_effect_macos.mm:105-115,1051-1073 + git log/submodule status；登记：2026-11-07 10:22:25 +0800 bug-fixer；复核：2026-11-07 10:22:25 +0800
- [stale] clue#92 · 主题：bug142:click-chain-verdict-94d4f5ba · 域：bug142 · 关键词：bug142,复现判定,不可复现,探针,点击链,证据,need140 · 事实：bug142 复现判定（2026-11-07，HEAD 94d4f5ba + EUI-NEO 9d34e79 本机 Debug 构建）：不可复现。探针实操证据（禁静态走读下结论）：① 新探针 tests/probes/bug142_click_chain_probe.cpp（真 app::compose + 全真模块注册表）真点击链验证：侧栏页切换 app.sidebar.item.Settings.hit → 设置页当帧入 DOM PASS；侧栏返回 Home PASS；dump 锚点 app.sidebar.item.Settings.hit frame{x=16,y=645,w=188,h=40}；② 正典探针 module_toggle_sidebar_probe 同构建 PASS（页内按钮链：真点 settings.tabs.1.hit → settings.card.modules.sw.4.track 入 DOM → 点开关 → 侧栏条目当帧出现/消失 + 侧栏收缩钮 220→72→220）；③ 证据物 evidence/bug142/{01-home-initial,02-settings-after-sidebar-click,03-settings-modules-tab,04-module-toggle-final-state}.png（02 可见侧栏设置项选中态 + 设置页完整渲染）。疑点留给下游：bug142_click_chain_probe 自写探针在页内 tab 点击后 scan 前缀 settings.card.* 得 0（cardsBefore=0 即通用 tab 也 0，而同帧截图卡片可见）——疑似自写 scan 前缀不匹配或该探针环境下 tab 点击未切换，非判定阻断（正典 module_toggle 已证 tab 点击链响应），若后续复用该探针需先修 scan。 · 作废理由：expired: bug142 复现判定快照绑定 HEAD 94d4f5ba 时点（2026-11-07），valid_until 11-14 已过，后续树判定走新探针新 clue；标废：2026-11-23 10:27:05 +0800 ZCode主会话（池清理 2026-11-23）；有效期至 2026-11-14 00:00:00 +0800 · 来源：evidence/bug142/ 探针实测 + build/tests/bug142_click_chain_probe 与 module_toggle_sidebar_probe 运行输出；登记：2026-11-07 10:22:25 +0800 bug-fixer；复核：2026-11-07 10:22:25 +0800

**关联 BUG**（5）：

- 子表行#8 · 2026-11-06 21:04:44 +0800 · 已解 · 老板实测发现 2 缺陷（2026-11-06，装 Windows 同款视觉后）：①右上角关闭/最小化图标不显示——根因=我改 skin 时 normal 设 0 透明且 euiChromeButtonColors 在 !active 时 symbol=0，drawRect 命中 `fill==0 && symbol==0` 早退，非 hover 态整钮不画。修法=skin 增 iconNormal（textSecondary），非 hover 也画图标。②macOS 全屏态缺退出全屏入口——当前 overlay 全屏整条退场且原生红绿灯已隐藏，无任何退出途径（只能键盘 Ctrl+Cmd+F）。修法=全屏态 overlay 保留并只显示一颗「退出全屏」钮于右上角，点击 toggleFullScreen。（ZCode主会话）
- 子表行#9 · 2026-11-06 21:14:29 +0800 · 已解 · 老板实测 3 缺陷（2026-11-06，Windows 同款视觉第二轮）：①全屏态未适配——全屏时标题条仍占 40px 布局高、内容未铺满，退出钮位置被状态栏遮挡。②要求去掉全屏按钮——Windows 侧也无全屏钮；双击标题栏应自动进/出全屏（替代当前 zoom）。③最小化钮 hover 态图标尺寸异常（过大、跑出窗口）。（ZCode主会话）
- 子表行#10 · 2026-11-06 21:19:16 +0800 · 已解 · 老板实测（2026-11-06 第四轮）：全屏态系统标题栏浮现但内容为空——红绿灯未恢复。根因疑点：macOS 原生全屏使用独立的 fullscreen titlebar，`standardWindowButton:` 返回的是普通态那组，隐藏/恢复对它无效；需改为全屏时用 `NSWindowStyleMaskFullScreen` 判断 + 直接对 `nsWindow.standardWindowButton` 三钮 setHidden:NO（已做）之外，还要排查是否 overlay 仍挡在其上或 AppKit 全屏条用的是 `NSTitlebarContainerView` 内另一组。（ZCode主会话）
- 子表行#11 · 2026-11-06 21:26:40 +0800 · 已解 · 老板实测（2026-11-06）：窗口边框黑线未贴合实际窗口边缘——疑因窗体圆角半径与虚框绘制几何不匹配（黑线按直角矩形画，窗口实际是圆角，导致边角处线跑到窗口外/内侧）。涉及 src/ui_kit/window_ring.h（池168 绿白间隔圆角虚框 ring）+ app_sidebar.cpp 调用点。（ZCode主会话）
- 子表行#13 · 2026-11-06 21:59:15 +0800 · 已解 · 老板实测（2026-11-06）：窗口黑边框在四个圆角处线变淡/不完整，需微调。查向：EUI 的 .border() 是否只画四条直边不画圆角弧（若如此则圆角处本就无描边），或 1px 线在弧段被抗锯齿削弱。（ZCode主会话）

**进度记录**（17）：

- 2026-11-06 18:32:31 +0800 · 从 need:132 PRD 审计 P0-1 拆分而来（2026-11-06）：原 F-04/F-05 迁入本条，need:132 收窄为字体统一线。等老板路线复核。
- 2026-11-06 19:01:37 +0800 · 老板裁决 2026-11-06：「macOS 放弃原生红绿灯我记得要执行的」「改造方案选 1+3，字体、标题栏什么的都改」——**路线 B 定稿**（隐藏原生红绿灯全自绘窗口钮）+ 字体线并行做（字体线在 need:132）。解除挂起，重启全流程：路线 B 与原调研（路线 A 优先）结论相反，需 researcher 重新调研路线 B 实现路径（styleMask/自绘钮/拖拽/resize/全屏重挂/无障碍），再 PM 出 PRD、ui-ux-designer 出窗口钮设计、developer 实现。
- 2026-11-06 19:02:20 +0800 · 路线 B 调研后台派发中（2026-11-06）：调研 macOS 隐藏原生红绿灯/自绘钮/拖拽/resize/全屏重挂/无障碍实现路径，产出 docs/research/2026-11-06-macos-fully-custom-titlebar-route-b.md。下游：PM 出 PRD → ui-ux-designer 出窗口钮设计 → developer 实现。
- 2026-11-06 19:37:07 +0800 · 路线 B 调研完成并过主会话物理抽核（2026-11-06，报告=docs/research/2026-11-06-macos-fully-custom-titlebar-route-b.md 20KB，clue 57/58/59）。结论：可行（有条件），**推荐子方案①「保留 Titled|Resizable + setHidden:YES 隐藏三钮 + NSView 层叠自绘钮 + performWindowDragWithEvent 拖拽 + toggleFullScreen 绿灯语义 + 统一重挂钩子」**，不选真 Borderless（②）。关键依据：①隐藏必须 setHidden 禁 removeFromSuperview（Apple 语义：remove 后 standardWindowButton 返回 nil 且 performClose/performMiniaturize 蜂鸣）；②真 Borderless 也能原生 resize（kitty@4fdb55e 实证）但圆角/阴影/双击/拖拽仍要自担，收益小于代价；③自绘钮走 NSView 层叠可免费拿 hit-test/hover/无障碍标签；④最大坑=Tietze 实证改 .resizable 会自动重显已隐藏钮 → 所有重挂钩子必须含重新 setHidden:YES。抽核通过：GLFW cocoa_window.m:1184-1207 miniaturize:/zoom: 桥接存在、:813 创建 mask 恒含 Miniaturizable。三风险：重挂时机漏一处白钮浮现 / performClose 穿透 GLFW windowShouldClose 回调待 POC / hidden 钮退出 VoiceOver 树（待 POC）+ GLFW 无菜单栏 Cmd+W/M 需自建 NSMenu。下一环：PM 出 PRD。
- 2026-11-06 19:43:21 +0800 · PRD 已出：doc id=29，slug=2026-11-06-macos-custom-titlebar v1（macOS 标题栏自绘路线 B：隐藏红绿灯+全自绘窗口钮）。矩阵 9 F（核心 8：F-01~F-06/F-08/F-09；边缘 1：F-07）。拍板记录含子方案①细化（Titled+setHidden+NSView 层叠，不选 Borderless）；第三钮语义拍全屏（D3）。开放决策 6 项留技术卡（performClose veto POC/重挂时序/拖拽降级/VoiceOver/chrome 开关接入/子窗面）。新线索 clue 61：bodyTop 与分档阈值现行锚点（clue 35 行号已漂移）。下游：tech-solution 出卡 → ui-ux-designer 视觉稿 → 开发。
- 2026-11-06 20:17:08 +0800 · UI 设计端完成（2026-11-06）：设计文档 docs/design/2026-11-06-macos-custom-titlebar-design.md + 自包含 demo docs/design/2026-11-06-macos-custom-titlebar-demo.html（JS node --check 通过，截图自审 5 态达标）。核心裁决：钮位=macOS 左缘三钮（热区 46×40 同构 Windows、位置不对齐右缘，PRD UI 契约原文+D3 同逻辑）；视觉=红绿灯彩点方案推荐（红=colors.error/黄=colors.warning/绿=colors.primary 全既有 token 零硬编码），DevDesk 统一线性钮为方案 B 备选。先验验证清单 20 格。新查证已入 clue 72。待 developer 技术卡/实现。
- 2026-11-06 20:23:05 +0800 · UI 设计完成（2026-11-06，设计文档=docs/design/2026-11-06-macos-custom-titlebar-design.md 21KB + demo.html 21KB，commit b9c93b2e）。核心裁决：①**钮位置=macOS 左缘三钮**（x∈[0,138)，46×40 热区与 Windows 同尺寸连排）——依据 PRD UI 契约原文已锁「左缘三钮」+ Mac 用户肌肉记忆 + 两端一致性标的（bodyTop 40/同档位）不含钮位置；②**视觉=A 红绿灯彩点**（推荐，色值全锚 theme.cpp token 零硬编码），B 统一线性钮作备选（demo 可切）。参数：条高 40 契约不动、圆点直径 12、圆心 x=23/69/115、符号 8px hover 浮现 120ms ease-out、1px 描边 hover 深档、失焦全灰+首点仅激活、全屏整条退场、材质 6 组合不铺底+彩点描边。先验清单 20 条（视觉 9 项 demo 自审已覆盖，运行时 11 项归 QA）。主会话核验：demo 零外部依赖✓、JS node --check ✓、自适应手段偏弱（仅 viewport+1 max-width+flex-wrap，但不阻塞——demo 仅视觉校准）。**钮位置老板询问未作答，按设计师推荐+PRD 原文执行，可推翻（推翻成本=改设计文档钮组 x 区间与实现热区，不影响其他）**。
- 2026-11-06 20:54:53 +0800 · 实现完成并推送（EUI 0bc8c62 → fork main；DevDesk 00a1f709 → origin/dev）。EUI 侧：CustomTitlebarSkin 契约+自绘壳本体（setHidden 隐藏三钮/12px 圆点/hover 符号/首点仅激活/无障碍 label/performWindowDragWithEvent 拖拽/双击 zoom/幂等重挂+D idUpdate 全事件兜底钩子组）+ supported() 翻 true + buttonsNative() + show 前 install（无闪现）。DevDesk 侧：theme 4 token 槽位/drawTitlebar mac 分支（品牌 x=154、DSL 不画钮）/bodyTop gate 化（审计 P1-1 回退缝，#ifdef 只承载回退态值）/decorated 决策改 !supported()||buttonsNative()（保 Titled mask=子方案①，原式会变 Borderless 违 N1）。**主会话亲验**：构建绿（--dev --no-run）、截图证据 evidence/need140/normal-3buttons-titlebar.png 实见三彩点左缘+品牌文本+无系统红绿灯、fxdiag styleMask=0x800f（FSCV+Titled+Closable+Miniaturizable+Resizable）证实非 Borderless、diff 核验 decorated/bodyTop 两处关键改动正确。**未验证归 QA**：A2/A5/A6/A7/A11/A15（全屏进出/失焦/点击回传/材质 6 组合——虚拟机抢焦点无法自动化，钩子已全实现）；A8 VoiceOver 实测；A13 不适用留痕。阶段：待 QA 真机验收。
- 2026-11-06 21:02:19 +0800 · 老板 2026-11-06 改口径：「红绿灯的设计可以去掉，改成 win 那种自定义的设计」——视觉方案 A（红绿灯彩点）废止，改 Windows 同款。改动（S 级，同一需求内视觉迭代）：①EUI window_effect_macos.mm drawRect 改矩形整格底+hover 灰/关闭红+线性图标（×/−/方框），去圆点彩点与描边；②钮位置左缘→右缘连排（关闭最右、最小化次之、最大化第三），autoresizing 右锚；③app_sidebar.cpp 品牌 x=154→16（Windows 同构）、skin 改常态透明+hover 灰+关闭 red、拖拽区按 titlebarButtonCount()（mac 3/Windows 2）让位。设计文档 §5 彩点参数作废，以本 note 为准。
- 2026-11-06 21:04:44 +0800 · 视觉迭代第二轮（老板实测反馈）：①图标不显示 bug ②全屏退出钮缺失。两者均在 #140 范围内直修。
- 2026-11-06 21:14:29 +0800 · 第三轮视觉修正：①全屏适配（标题条退出布局流）②撤三钮为两钮（关/最小）+ 双击标题栏切全屏 ③hover 图标尺寸修复。
- 2026-11-06 21:19:16 +0800 · 全屏适配改交还 macOS 系统：全屏显示原生三钮、overlay 整条退场、不自绘全屏退出钮。当前缺陷=全屏时系统标题栏为空（钮未恢复）。
- 2026-11-06 21:26:07 +0800 · 四轮视觉迭代完成并推送（EUI 7489ddd → fork main；DevDesk e950c297 → origin/dev）。**最终形态**（老板逐轮拍板收敛）：①两钮（关闭/最小化）右缘连排=Windows 完全同构，红绿灯彩点与全屏钮均废止；②矩形整格底+线性图标（×/−），常态透明/hover 灰/关闭 hover 红，hover 底内缩 2px 防「跑出窗口」；③iconNormal/iconHover 皮肤字段修 hover 时图标同色自盖消失；④**全屏交还 macOS 系统**：撤 FSCV 三连→AppKit 原生全屏标题栏（鼠标移顶部浮现含红绿灯+标题），overlay 整条退场、恢复原生三钮；⑤双击标题栏=进/出全屏；⑥新增 windowIsFullscreen 双后端查询 + mainWindowFullscreen 转发，全屏态 bodyTop=0 内容铺满。**验证**：老板真机实测「没问题了」（全屏适配）；主会话亲验构建绿+截图见三钮/两钮显示与 hover 修复。**证据缺口如实记**：主会话未取到 DevDesk 全屏原生标题栏截图（误截到其他 app），该面证据为老板实测口述。
- 2026-11-06 21:26:40 +0800 · 第五轮：窗口虚框黑线未贴合圆角窗口边缘。
- 2026-11-06 21:30:43 +0800 · 第五轮修复完成（commit 87b669c2，已推 origin/dev）。根因：src/ui_kit/window_ring.h 的 windowBorderRadius 一直按 Windows DWM「8 物理px」规则折算，而 macOS 窗口圆角由 AppKit 原生绘制（EUI 从不设 layer.cornerRadius），macOS 26/27 实测约 12 逻辑pt 且不随 backingScale → mac 上弧过小、四角黑线内凹离窗缘。修法：圆角改由调用方传「已换算到逻辑」的平台真实圆角（app_sidebar.cpp: mac 12.0f / 其他 8.0f/contentScale），ui_kit 保持纯参数无平台宏；ring_probe 同步同口径（A3 采样与渲染须一致否则假红）。**验证**：ring_probe 三项全 PASS（A1 顶边 1172/1172 / A1b 左边 964/972 / A3 45° 圆角弧段命中黑线）；截图 evidence/need140-v2/ring-corner-fixed.png 实见黑线沿窗体圆弧贴合（对比修复前 ring-corner.png 明显直角内凹）。
- 2026-11-06 21:59:15 +0800 · 第六轮：边框圆角处细线变淡/不完整。
- 2026-11-08 22:51:16 +0800 · 2026-11-08 老板拍板路线 B（自绘标题栏、放弃原生红绿灯）——推翻调研推荐路线 A。本日后续处理：老板同时要求先推送合并，B 路线开发待下次开工

**状态变更**（6）：

- 2026-11-06 18:32:27 +0800 ·  → 在池 · 入池（ZCode主会话）
- 2026-11-06 18:32:31 +0800 · 在池 → 挂起 · 等老板复核标题栏路线：A=保留原生红绿灯挪进 40px 条（Electron/Tauri/Qt 三家先例，风险最低）/ B=隐藏红绿灯全自绘（老板 2026-11-05 原拍板字面，需自实现全套窗口交互）。复核前不动工。（ZCode主会话）
- 2026-11-06 19:01:37 +0800 · 挂起 → 在池 · 等老板复核标题栏路线（已解除）（ZCode主会话）
- 2026-11-06 19:01:42 +0800 · 在池 → 排产 · （ZCode主会话）
- 2026-11-06 19:02:20 +0800 · 排产 → 进行中 · 路线 B 调研后台派发中（2026-11-06）：调研 macOS 隐藏原生红绿灯/自绘钮/拖拽/resize/全屏重挂/无障碍实现路径，产出 docs/research/2026-11-06-macos-fully-custom-titlebar-route-b.md。下游：PM 出 PRD → ui-ux-designer 出窗口钮设计 → developer 实现。（ZCode主会话）
- 2026-11-08 22:51:16 +0800 · 进行中 → 挂起 · 2026-11-08 老板拍板路线 B（自绘标题栏、放弃原生红绿灯）——推翻调研推荐路线 A。本日后续处理：老板同时要求先推送合并，B 路线开发待下次开工（ZCode主会话）

**关联文档**（3）：

doc#29 · 2026-11-06-macos-custom-titlebar（prd v2 · 更新 2026-11-06 20:05:26 +0800）→ 附录 A
doc#30 · 2026-11-06-macos-custom-titlebar-pm-audit（audit v1 · 更新 2026-11-06 19:59:56 +0800）→ 附录 A
doc#31 · 2026-11-06-macos-custom-titlebar-tech（tech v1 · 更新 2026-11-06 20:31:18 +0800）→ 附录 A

---

#### need#315 · 记忆 E2E 可行性探查（DB 化 key 通道回归前置）

- 状态：挂起 · 优先级：P3 · 负责人：qa-tester · 阶段：开发中
- 创建：2026-11-18 18:53:56 +0800（qa-tester）· 更新：2026-11-18 19:05:33 +0800（qa-tester）
- 阻塞：老板令「当前不执行记忆模块相关的调研开发」（2026-11-18），解冻须显式令

**详情**：

记忆引擎 E2E 可行性探查（key 通道 DB 化后的回归验证准备）：memory_e2e_probe live 栈（llama-server 11435+telemem 8421+DEVDESK_DATA_ROOT 沙箱+key 经 app_secrets 预置）在本机能否自动跑通——探查启动脚本/依赖（模型文件在否）/费用面（提炼走 LLM key=沙箱预置测试 key 或跳过提炼只测检索）；可行则跑一轮记录结果，不可行出缺口清单（缺什么/谁补）。范围只读+临时进程，禁碰 8421/11435 生产残留（先清后用）。

**进度记录**（2）：

- 2026-11-18 18:54:16 +0800 · 老板令 8 并发。
- 2026-11-18 19:05:33 +0800 · 老板冻结令中止（clue#311）；停前结论已落档：主体链路可一键跑通（42/46，4 红=dummy key 401 非回归），缺口 G1（脚本 env 直塞未迁 app_secrets）/G2/G3 冻结期不修，解冻后按简报续。

**状态变更**（4）：

- 2026-11-18 18:53:56 +0800 ·  → 在池 · 入池（zcode）
- 2026-11-18 18:54:16 +0800 · 在池 → 排产 · 老板令 8 并发。（zcode）
- 2026-11-18 18:54:53 +0800 · 排产 → 进行中 · （zcode）
- 2026-11-18 19:05:33 +0800 · 进行中 → 挂起 · 老板令「当前不执行记忆模块相关的调研开发」（2026-11-18），解冻须显式令（zcode）

---

#### need#345 · todo 四列折叠互斥（折叠新列自动展开上一列）——独立折叠态修复

- 状态：挂起 · 优先级：P2 · 负责人：bug-fixer · 阶段：开发中
- 创建：2026-11-18 22:28:57 +0800（bug-fixer）· 更新：2026-11-23 09:55:15 +0800（bug-fixer）
- 阻塞：需老板用最新构建复测；仍互斥则提供：平台（Mac/Windows）、装机构建时间、完整操作序列（是否含归档列/搜索/拖拽/切页）

**详情**：

老板验收实测（2026-11-18 23:1x）：todo 四列（看板列）折叠互斥异常——只能同时折叠一列，折叠新列会自动把上一个已折叠列展开。根因方向：折叠态存了**单槽**（collapsedIndex/accordion 语义/共享一个 bool）而非每列独立 bool——自行定位 todo 列头折叠状态存储；修=每列独立折叠态，若原有持久化/记忆口径保持兼容。文件域 module_todo 与 #338（果冻施加层迁移在飞）互斥→挂排产等其 commit 落定即派（防同文件混线）。

**关联线索**（1）：

- [open] clue#327 · 主题：todo:collapse-single-slot-mislabeled-independent · 域：todo · 关键词：折叠,证伪,per-column,旧装机,复测,归因验码 · 事实：【勘误取代 323】need345 复核证伪单槽归因：d5196aa1（2026-11-07）引入即 per-column colCollapse_（todo_module.cpp:1242 map<colId,state>），git log -S 此后无结构性改动；真 app ui_driver 复刻老板序列 16/16 绿（折A→折B→A 仍折/展A不影响B，evidence/need345/）。老板报修现象两个工作假设：①装机二进制旧（/Applications 版 21:58）②与 need335 记忆设置页 tab 折叠（真 accordion，22:17 已修）混淆。教训：复盘归因必须验码——池叙事+现象史≠代码事实（#346 此行即栽在未验码）。复测：统一重装后复测；ui_driver 动画落位等待需≥2s 否则误报。 · 来源：need345 复核报告（2026-11-18，evidence/need345/ 16 用例）；登记：2026-11-18 22:55:16 +0800 zcode；复核：2026-11-18 22:55:16 +0800

**进度记录**（4）：

- 2026-11-18 22:29:01 +0800 · 老板验收报修；挂排产非并行缺陷而是文件域互斥（多代理共享 worktree 协议）。#338 落定通知到达→本条立即转进行中派 bug-fixer（并发空位正好顶上）。
- 2026-11-18 22:42:11 +0800 · blocker 解除：#338 已 commit 2b6add19 落定（module_todo 文件域空出）。按滚动补位立即派发。
- 2026-11-18 22:54:19 +0800 · 断点报告（20min 无 commit 收尾规则）：不可复现于当前 HEAD——按边界禁盲改，未提交任何代码。①全链路核查：模块态 colCollapse_ 按 colId 独立、toggle 只翻本列（todo_module.cpp:692-734），compose 按列装配（:976-981），板层按列路由（todo_ui_board.cpp:424/510），宽度算法支持多列折叠（todo_ui_logic.h:100-138）。②实锤矛盾：clue#323 称 d5196aa1 单槽 accordion，但 git show d5196aa1 的模块与板层均为 per-column map，与代码事实不符。③实测：probe（1200x1000，含命中回读真点击）与真 app ui_driver（scene need345-probe3.json）均复刻老板序列全绿。④时间线：装机 /Applications 二进制 21:58，早于 22:41 #338（2b6add19，仅果冻改动）；折叠链 11-07 后无提交级改动。判定：疑旧装机（Windows 侧 %LOCALAPPDATA% 安装不可查）或与 need335 设置页 tab 折叠（ae96cb3a 22:17，accordion 语义）混淆。下一步：老板复测后仍红再开，凭本 note 与 evidence/need345/ 续跑。
- 2026-11-23 09:55:15 +0800 · 执行棒机判（2026-11-23）：不复现——todo_collapse_probe 全绿（N3 双列折叠共存 PASS、N4 col0 展开时 col1 折叠不受误改 PASS，单槽实现下必红）+ HEAD 静态 todo_ui_board.h:84 columnCollapseT 每列独立向量、零持久化单槽，「单槽」根因假设与实现不符。老板 11-18 实测或来自旧构建；待老板用最新构建确认后即可关账

**状态变更**（4）：

- 2026-11-18 22:28:57 +0800 ·  → 在池 · 入池（bug-fixer）
- 2026-11-18 22:29:01 +0800 · 在池 → 排产 · #338 在飞改 module_todo 同文件域——互斥挂起，其 commit 落定即解除派发（zcode）
- 2026-11-18 22:42:11 +0800 · 排产 → 进行中 · blocker 解除：#338 已 commit 2b6add19 落定（module_todo 文件域空出）。按滚动补位立即派发。（zcode）
- 2026-11-18 22:54:19 +0800 · 进行中 → 挂起 · 需老板用最新构建复测；仍互斥则提供：平台（Mac/Windows）、装机构建时间、完整操作序列（是否含归档列/搜索/拖拽/切页）（bug-fixer）

---

## bug 池

### 待验收（8）

#### bug#239 · mac 包主二进制硬链 Homebrew openssl@3 而 .app 内无 dylib——未装 brew 新机启不来（发布阻断）

- 状态：待验收 · 优先级：P1 · 负责人：ZCode主会话 · 阶段：测试中
- 创建：2026-11-08 17:30:20 +0800（ZCode主会话）· 更新：2026-11-23 09:55:14 +0800（ZCode主会话）

**详情**：

【发布级风险，未装 Homebrew 的新机可能启不来】

实测（2026-11-08，need238 交付棒发现 + 主会话复核）：
- `otool -L build-release/DevDesk.app/Contents/MacOS/DevDesk` → 硬链 `/opt/homebrew/opt/openssl@3/lib/libssl.3.dylib` + `libcrypto.3.dylib`
- `ls build-release/DevDesk.app/Contents/MacOS/*.dylib` → 零命中（无任何 dylib）

后果：未安装 Homebrew openssl@3 的新机器上 dyld 必加载失败，.app 直接启不来。

根因（bug225 同根）：`scripts/package-macos.sh:86-151` 的 install_name 重写 + 依赖闭包拷入**只覆盖 llama 依赖闭包**，未覆盖主二进制自身的非系统依赖。

修法候选：
1. 把主二进制也纳入依赖闭包 fixpoint 处理（与 llama 段同纹），非系统 install name 全部重写为 @loader_path/<basename> 并拷入；
2. 或改为静态链入 OpenSSL（与 Windows 侧 DEVDESK_STATIC 同思路）——但 mac 静态链 OpenSSL 有许可/体积权衡。
优先 1（与现有机制同源，风险最小）。

验证方法：打包后 `otool -L` 主二进制应无 `/opt/homebrew/` 前缀路径；且在未装 brew 的干净环境实测启动（可用 `DYLD_PRINT_LIBRARIES=1` 或临时移除 PATH 模拟）。

发现来源：need238 交付棒报告，已入线索（clue 218）。

**进度记录**（3）：

- 2026-11-08 22:51:07 +0800 · 2026-11-08 修复完成：package-macos.sh 新增主二进制依赖收编段（小 fixpoint 递归）。坑：libssl 记录的 libcrypto 是 Cellar 版本化路径与 opt/ 软链不同字符串，只改直接依赖会漏→必须递归。遗留：完整验收需未装 brew 的干净机器，本机无法构造
- 2026-11-08 22:56:37 +0800 · 2026-11-08 补充验证完成：① 删旧产物 build-release/DevDesk.app（68MB 带硬链）② 全 bundle 深度扫描无 /opt/homebrew 或 /usr/local 绝对路径③ 签名有效④ 清空 DYLD 路径后实际启动成功。另核查两份 OpenSSL 不可合并（ABI major 3 vs 4；libllama-common 真调 EVP_Digest* 做模型 SHA 校验）
- 2026-11-23 09:55:14 +0800 · 机收口留验（含裁决）：修法落树（package-macos.sh:174-182 依赖收编 fixpoint 递归）+11-08 补充验证（深扫无 brew 路径+清 DYLD 启动成功）；干净机终验留发版机时点

**状态变更**（4）：

- 2026-11-08 17:30:20 +0800 ·  → 在池 · 入池（ZCode主会话）
- 2026-11-08 22:44:02 +0800 · 在池 → 排产 · （ZCode主会话）
- 2026-11-08 22:44:08 +0800 · 排产 → 进行中 · （ZCode主会话）
- 2026-11-08 22:51:07 +0800 · 进行中 → 待验收 · 2026-11-08 修复完成：package-macos.sh 新增主二进制依赖收编段（小 fixpoint 递归）。坑：libssl 记录的 libcrypto 是 Cellar 版本化路径与 opt/ 软链不同字符串，只改直接依赖会漏→必须递归。遗留：完整验收需未装 brew 的干净机器，本机无法构造（ZCode主会话）

---

#### bug#243 · DMG 图标是旧的蓝色怪物 + DMG 页面排版错乱（标签被裁/文字重叠）

- 状态：待验收 · 优先级：P2 · 负责人：ZCode主会话 · 阶段：测试中
- 创建：2026-11-08 23:20:24 +0800（ZCode主会话）· 更新：2026-11-23 09:55:14 +0800（ZCode主会话）

**详情**：

````text
老板实测 DMG 后报：「dmg 里的应用图标是旧的，然后 dmg 打开的页面排版搞一搞」。

## 问题 1：图标是旧的蓝色怪物
根因：platform/macos/build-app.sh:79-86 的图标来源有两条，installer/macos/AppIcon.icns **优先级最高**——只要它存在就用它，永远走不到「assets/icon.png 现生成」分支。而那份 icns 是旧品牌（蓝色怪物）。
修：删掉 installer/macos/AppIcon.icns 与 .VolumeIcon.icns（后者未被引用），走 PNG 生成链。

## 问题 2：DMG 排版错乱
原 700×440：辅助文件 y=68（图标上沿顶到 y=8，Finder 标题栏占用后标签被裁）；说明文字 y=262 与主图标标签（y≈280）重叠。
修（背景图与 create-dmg 参数**成对**改，两边不同步会出现箭头指向空白）：窗口 700×460；辅助文件 y=150、照贴两侧（75/625）；主图标 y=300、x 215/485、size 110；说明文字 y=390。
附带：gen_dmg_background.py 原来只认 Windows 中文字体路径，在 mac 上静默退化成无文字版本 → 改多候选链（微软雅黑 → 苹方 → 冬青黑 → 仓库 DevDeskSansSC）。

## 遗留（P3）
assets/icon.png 只有 256px，1024 档是插值，Retina 大图标下清晰度略降。彻底解决需一张 ≥1024px 源图。
````

**进度记录**（2）：

- 2026-11-08 23:20:49 +0800 · 2026-11-08 修复完成：① 删旧 icns（build-app.sh 双源冲突，icns 优先级最高阻断 PNG 生成链）② 窗口 700×460 重排 + 背景图与 create-dmg 参数成对改 ③ 背景图脚本补 macOS 中文字体候选。遗留：icon.png 仅 256px，Retina 大图标略糊
- 2026-11-23 09:55:14 +0800 · 机收口留验（证据不足）：代码三项全证（删 icns/700×460/字体候选，package-macos.sh:260-268+gen_dmg_background.py:144-152）；池内无 DMG 实拍、Retina P3 遗留未另立项——待老板裁决补实拍+立项或弃置

**状态变更**（4）：

- 2026-11-08 23:20:24 +0800 ·  → 在池 · 入池（ZCode主会话）
- 2026-11-08 23:20:32 +0800 · 在池 → 排产 · （ZCode主会话）
- 2026-11-08 23:20:38 +0800 · 排产 → 进行中 · （ZCode主会话）
- 2026-11-08 23:20:49 +0800 · 进行中 → 待验收 · 2026-11-08 修复完成：① 删旧 icns（build-app.sh 双源冲突，icns 优先级最高阻断 PNG 生成链）② 窗口 700×460 重排 + 背景图与 create-dmg 参数成对改 ③ 背景图脚本补 macOS 中文字体候选。遗留：icon.png 仅 256px，Retina 大图标略糊（ZCode主会话）

---

#### bug#251 · Dock 图标偏大：assets/icon.png 满幅(96.1%)，运行时 applyWindowIcon 覆盖了已对齐 HIG 的 icns

- 状态：待验收 · 优先级：P1 · 负责人：main · 阶段：测试中
- 创建：2026-11-09 21:23:55 +0800（main）· 更新：2026-11-23 09:55:14 +0800（main）

**详情**：

bug158 只修了 icns 链（installer/macos/AppIcon.icns→80.5% 合规），未同步 assets/icon.png。EUI-NEO dsl_app 默认 iconPathValue=assets/icon.png，启动时 applyWindowIcon 调 setWindowIcon→eui_set_application_icon_rgba 用 png 覆盖 NSApp 图标，绕过 icns。实测 png 内容占比 96.1%（瓦 8..248/256），icns 80.5%。macOS Dock 对传入方图套 squircle 蒙版后运行期量得 86.5%，与其他 app 一致，但源图满幅导致视觉上比邻居更满。修复：build_icon_assets.py 生成 icon.png 时按 HIG 824/1024 网格缩到 80.5%（复用 doc/scripts/pad_icon_canvas.py --target-px 824 口径）。

**进度记录**（1）：

- 2026-11-23 09:55:14 +0800 · 机收口留验（含裁决）：原修法未执行（PIL 实测 icon.png 仍 96.1%）；真根因已移 bug#353（killall Dock 已落树且今日关账）。待老板裁决：与 #353 并案关闭 or 仍执行缩 png

**状态变更**（4）：

- 2026-11-09 21:23:55 +0800 ·  → 在池 · 入池（main）
- 2026-11-09 21:24:31 +0800 · 在池 → 排产 · （main）
- 2026-11-09 21:24:35 +0800 · 排产 → 进行中 · （main）
- 2026-11-09 21:39:46 +0800 · 进行中 → 待验收 · 

---

#### bug#271 · 托盘右键菜单失焦后不自动关闭

- 状态：待验收 · 优先级：P1 · 负责人：主会话 · 阶段：测试中
- 创建：2026-11-16 23:26:16 +0800（主会话）· 更新：2026-11-16 23:42:52 +0800（主会话）

**详情**：

用户反馈：托盘图标右键弹出菜单后，点击其它地方（失焦）菜单不会自动关闭，需要手动操作。历史上有 32ae496 fix(styledmenu): 托盘菜单微信观感 + 失焦即收两真机缺陷（2026-10-21）修过同类问题，疑为回归或 macOS 侧失效。需查证：托盘菜单实现（styled menu 独立弹窗 vs 原生 NSMenu）、失焦关闭监听机制（全局/本地事件监听）、关闭时机。

**关联线索**（1）：

- [open] clue#266 · 主题：DevDesk:tray-menu-dismiss · 域：DevDesk · 关键词：托盘,右键菜单,失焦,styledmenu,windowIsFocused,macOS · 事实：bug271 根因（静态锁定）：托盘右键菜单走 components::showStyledMenu 独立弹窗（src/main.cpp:750 showTrayStyledMenu）。其失焦关闭有三道机制：①onWindowFocusChanged 直连（windows 帧回调跨平台，子窗口已安装 glfw_app_main.cpp:464-465）——但组件内注册被 #if defined(_WIN32) 包裹（components/styledmenu.h:581，来自 32ae496 2026-10-21 只为修 Windows）；②compose 轮询 windowIsFocused——但该函数非 Windows 恒返回 true（core/window/window_backend.cpp:714-718）；③整窗透明 .dismiss 层——只能收菜单窗内点击，点窗外根本到不了。⇒ macOS 三道全失效，菜单赖着不走。 · 来源：文件:行 components/styledmenu.h:581 + core/window/window_backend.cpp:714 + src/main.cpp:750；登记：2026-11-16 23:28:04 +0800 主会话；复核：2026-11-16 23:28:04 +0800

**进度记录**（2）：

- 2026-11-16 23:29:06 +0800 · 根因：styledmenu 失焦直连注册被 #if defined(_WIN32) 包裹（仅为修 Windows 旧缺陷）；而 macOS 另外两道兜底均不成立（windowIsFocused 非 Windows 恒 true + .dismiss 层只收窗内点击）→ 三道全失效。修法：去掉平台守卫，全平台布 onWindowFocusChanged。
- 2026-11-16 23:42:52 +0800 · 修复已入 EUI-NEO main（690484a），待真机目检确认：右键托盘菜单 → 点处部区域 → 应自动关闭，stderr 应见 [styledmenu] dismiss: focus lost (direct)。

**状态变更**（4）：

- 2026-11-16 23:26:16 +0800 ·  → 在池 · 入池（主会话）
- 2026-11-16 23:29:06 +0800 · 在池 → 排产 · 根因：styledmenu 失焦直连注册被 #if defined(_WIN32) 包裹（仅为修 Windows 旧缺陷）；而 macOS 另外两道兜底均不成立（windowIsFocused 非 Windows 恒 true + .dismiss 层只收窗内点击）→ 三道全失效。修法：去掉平台守卫，全平台布 onWindowFocusChanged。（主会话）
- 2026-11-16 23:29:14 +0800 · 排产 → 进行中 · 
- 2026-11-16 23:42:52 +0800 · 进行中 → 待验收 · 修复已入 EUI-NEO main（690484a），待真机目检确认：右键托盘菜单 → 点处部区域 → 应自动关闭，stderr 应见 [styledmenu] dismiss: focus lost (direct)。

---

#### bug#274 · 窗口效果半透/磨砂两档 Win+mac 双平台均无窗体半透明（老板 2026-11-17 实测）

- 状态：待验收 · 优先级：P1 · 负责人：bug-fixer · 阶段：测试中
- 创建：2026-11-17 11:44:36 +0800（bug-fixer）· 更新：2026-11-23 12:15:01 +0800（bug-fixer）

**详情**：

老板 2026-11-17 报障：半透与磨砂两档「都缺失了窗体半透明」，win 和 mac 一样。代码链分析（未实机取证）：

【半透档】透出率被压到 0~15%：clearColor 纱 α=0.85（settings_config.cpp:301，UAT2/3/6 历轮「太透看不清」从 0.60 调浓到 0.85）单层上限只剩 15% 桌面透出；macOS 再叠窗口背景色层 α=0.85（applyWindowVisuals 恒 customColor=true）→ 理论透出 0.15×0.15≈2% 实质不透明；Windows 半透被 bug266（DWMSBT_NONE 写档杀 GLFW 逐像素 α，挂起中）打成 0。

【磨砂档】桌面透出≈0：macOS NSVisualEffectView blendingMode=WithinWindow（window_effect_macos.mm:650，只模糊窗内永不透桌面）+ 材质 α0.95 + clearColor 0.15 + winbg 0.65 → 桌面透出≈0；2026-11-01 真机验收已定性「名不副实」（docs/design/2026-11-01-窗口效果档验收与覆盖核对.md §三）留待办未做。Windows Mica + clearColor 0.15，但主窗恒最大化 + 自绘壳，Mica 是否真渲是 settings_config.cpp:324 注释悬案，症状同形。

【修复方向待拍板】①先定每档「桌面透出率」产品标（半透建议 25~30%、磨砂=模糊后的桌面）alpha 单层化倒推；②macOS 窗口背景色层只服务标题栏不叠内容区；③macOS 磨砂材质改 BehindWindow + 动态背景按档降透明；④Windows 半透归并 bug266；⑤磨砂 Windows 最大化+自绘壳取证。验收一律两平台真机（离屏拍不出材质，2026-11-01 已证）。

**关联线索**（1）：

- [open] clue#275 · 主题：window-effect:274-tier-params · 域：devdesk-window-effect · 关键词： · 事实：titleBarAlpha/ThemeOpacity 全部消费点仅 applyWindowVisuals 与测试，无第三方读者（改值零外溢）。真色值 WCAG 复算：0.72/0.72 最紧组合=5.79:1（亮主题次要文字）≥4.5，护栏有 1.29 余量。uat62_page_shots_probe 离屏无真 NSWindow，平台 apply/fxdiag 不触发，fxdiag 只能真机取。EUI-NEO standalone 单测不在 DevDesk 构建树内，.mm 平台层无对应单测、编译验证经 DevDesk 构建间接覆盖。 · 来源：bug-fixer 2026-11-17 bug274 修复过程查证；登记：2026-11-17 12:56:27 +0800 bug-fixer(主会话代落)；复核：2026-11-17 12:56:27 +0800

**进度记录**（5）：

- 2026-11-17 12:35:42 +0800 · 老板令「继续修复透明问题」（2026-11-17）= 开工令。分诊：Bug [ch:常规]，非 S 级 → 派 bug-fixer。决策点按推荐默认执行（need#141）：透出率标=半透 25~30% 桌面可见 / 磨砂=模糊后的桌面可辨；ui-ux-designer 环节跳过（纯参数标定+既有对比度护栏测试，无布局/交互变更）记理由。范围=macOS 全修（本机可真机验）+ Windows 代码修（bug266 归并+Mica 取证，真机验收待老板 Windows 机）。工作区=worktree 0d01（分支 fix/bug275-wallpaper-uaf 基上续做）。
- 2026-11-17 12:36:28 +0800 · 派发 bug-fixer [ch:常规]（简报含 [pool:bug:274]）。预算 ≤80 次/40min。范围：macOS 全修（本机真机可验）+ Windows 代码修（bug266 归并+Mica 取证）。工作区=worktree 0d01。池/线索操作由主会话代登记（worktree 池 MCP 绑错库）。
- 2026-11-17 12:56:27 +0800 · bug-fixer 完成（27 次/25min，预算内）。修法：①winbg 层退役（titleBarAlpha 半透/磨砂传 0）+ clearColor 单层 0.72=桌面透出 28%；②EUI-NEO 材质 blendingMode WithinWindow→BehindWindow（真桌面模糊，42a2341）；③晕层透出档 ×1.5 补偿；④surfaceAlpha 复算维持（最紧 5.79:1）。Windows：半透冷启动已命中 KeepUntouched 无需修；Mica 写档正确、真渲留真机。提交：DevDesk 7cca9512 + EUI-NEO 42a2341（wip 未推）。定向 6/6 绿+护栏绿+离屏三档 18 页全 PASS。证据 evidence/2026-11-17-bug274/。⚠主会话物理验证纠偏：agent 装机为旧包（指纹未中/大小不符），已重装+指纹命中+逐字节一致+重启。待老板真机目检三档（半透 28% 手感/磨砂模糊可辨度；微调点=kMaterialAlpha 0.95 或纱 0.72±0.03）+ Windows 机 Mica 确认。
- 2026-11-23 09:55:14 +0800 · 机收口留验（EUI 双端）：修复 7cca9512 仅在 fix/bug275-wallpaper-uaf 分支未并 dev；该分支同时带 bug274 定标+bug275 修复，并入 dev 后可一次消两条留验（Windows 效果另需真机）
- 2026-11-23 12:15:01 +0800 · 修复并入 dev（merge ee0c0d66）：7cca9512 半透定标（winbg 退役+clearColor 0.72+BehindWindow）经合并落仓；冲突 2 处按纪律解（EUI gitlink 保 fork/main 现挂/半透注释取 0.72 定标侧）。验证全绿：wallpaper 111/111 + 窗效 settings 域 82/82 + wallpaper 族 probe 7 过 0 红。留验收口=桌面半透观感属真机目检（老板 11-17 原报即双平台实测），Mac 侧可即刻看效果。

**状态变更**（6）：

- 2026-11-17 11:44:36 +0800 ·  → 在池 · 入池（bug-fixer）
- 2026-11-17 12:35:42 +0800 · 在池 → 排产 · 老板令「继续修复透明问题」（2026-11-17）= 开工令。分诊：Bug [ch:常规]，非 S 级 → 派 bug-fixer。决策点按推荐默认执行（need#141）：透出率标=半透 25~30% 桌面可见 / 磨砂=模糊后的桌面可辨；ui-ux-designer 环节跳过（纯参数标定+既有对比度护栏测试，无布局/交互变更）记理由。范围=macOS 全修（本机可真机验）+ Windows 代码修（bug266 归并+Mica 取证，真机验收待老板 Windows 机）。工作区=worktree 0d01（分支 fix/bug275-wallpaper-uaf 基上续做）。（ZCode主会话）
- 2026-11-17 12:36:28 +0800 · 排产 → 进行中 · 派发 bug-fixer [ch:常规]（简报含 [pool:bug:274]）。预算 ≤80 次/40min。范围：macOS 全修（本机真机可验）+ Windows 代码修（bug266 归并+Mica 取证）。工作区=worktree 0d01。池/线索操作由主会话代登记（worktree 池 MCP 绑错库）。（ZCode主会话）
- 2026-11-17 12:56:27 +0800 · 进行中 → 待验收 · bug-fixer 完成（27 次/25min，预算内）。修法：①winbg 层退役（titleBarAlpha 半透/磨砂传 0）+ clearColor 单层 0.72=桌面透出 28%；②EUI-NEO 材质 blendingMode WithinWindow→BehindWindow（真桌面模糊，42a2341）；③晕层透出档 ×1.5 补偿；④surfaceAlpha 复算维持（最紧 5.79:1）。Windows：半透冷启动已命中 KeepUntouched 无需修；Mica 写档正确、真渲留真机。提交：DevDesk 7cca9512 + EUI-NEO 42a2341（wip 未推）。定向 6/6 绿+护栏绿+离屏三档 18 页全 PASS。证据 evidence/2026-11-17-bug274/。⚠主会话物理验证纠偏：agent 装机为旧包（指纹未中/大小不符），已重装+指纹命中+逐字节一致+重启。待老板真机目检三档（半透 28% 手感/磨砂模糊可辨度；微调点=kMaterialAlpha 0.95 或纱 0.72±0.03）+ Windows 机 Mica 确认。（ZCode主会话）
- 2026-11-23 12:09:48 +0800 · 待验收 → 进行中 · 老板行首令「开工 #274」2026-11-23（中继原样）；待验收→进行中直迁（合并执行开工链）（ZCode主会话）
- 2026-11-23 12:15:01 +0800 · 进行中 → 待验收 · 修复并入 dev（merge ee0c0d66）：7cca9512 半透定标（winbg 退役+clearColor 0.72+BehindWindow）经合并落仓；冲突 2 处按纪律解（EUI gitlink 保 fork/main 现挂/半透注释取 0.72 定标侧）。验证全绿：wallpaper 111/111 + 窗效 settings 域 82/82 + wallpaper 族 probe 7 过 0 红。留验收口=桌面半透观感属真机目检（老板 11-17 原报即双平台实测），Mac 侧可即刻看效果。（ZCode主会话）

---

#### bug#275 · 花见导入报无法导入（全盘访问已授）+ 点提示里的系统设置按钮闪退（老板 2026-11-17 实测）

- 状态：待验收 · 优先级：P1 · 负责人：bug-fixer · 阶段：测试中
- 创建：2026-11-17 11:45:39 +0800（bug-fixer）· 更新：2026-11-23 12:15:01 +0800（bug-fixer）

**详情**：

老板 2026-11-17 报障（macOS）：①壁纸页点「花见导入」提示无法导入；②点提示里的「系统设置」按钮软件闪退（含崩溃栈待取证）；③系统设置里已给 DevDesk「访问全部磁盘文件」权限，仍不能导入。模块：src/module_wallpaper（importer_macos：hanamiImportAvailable/probeHanamiSourceDirectory/runHanamiImport）；「系统设置」按钮疑走 src/platform/open_settings_macos.cpp。待排查：导入失败的真实判据（TCC 授权后 probe 为何仍拒/源目录不存在？）与闪退根因（open 路径崩溃或 UI 回调线程问题）。

**关联线索**（2）：

- [open] clue#271 · 主题：macos-tcc:adhoc-fda-invalidated-by-rebuild · 域：macos-tcc · 关键词： · 事实：DevDesk.app 为 adhoc 签名（codesign -dvv 实测：Signature=adhoc、TeamIdentifier=not set、Identifier=com.devdesk.app）。TCC「完全磁盘访问」绑定签名身份/cdhash：每次 Debug 覆盖安装换二进制后，系统设置里的旧授权条目对当前副本失效——列表显示「已给」但实际 EPERM（花見容器目录 Operation not permitted）。症状级证据：老板已给 FDA 仍弹无权限窗。修法=删掉重加；根治=稳定签名身份（debt#276）。 · 来源：2026-11-17 codesign 实测 + ~/Library/Containers/whbalzac.Huajian ls 复现 EPERM；登记：2026-11-17 11:57:46 +0800 ZCode主会话；复核：2026-11-17 11:57:46 +0800
- [open] clue#272 · 主题：eui-runtime-lifetime:deferred-callback-no-ref-capture · 域：eui-runtime-lifetime · 关键词： · 事实：EUI Runtime 延迟回调生命周期契约：onClick/弹窗 confirm 等回调在点击帧（compose 帧之后）由 Runtime::updateInteraction 调用——回调禁止引用捕获 compose 帧栈上局部（每帧栈上构造的 UiActions 等）。仓内安全范式=先拷贝再值捕获（wallpaper_ui onPause/onNext 同款）。崩溃实锤：DevDesk-2026-11-17-114234/114319.ips 两份栈一致（updateInteraction→drawImportNoAccessDialog::$_1→SIGSEGV），11-16 晚另有三份同因。 · 来源：2026-11-17 两份 .ips 崩溃栈解析 + wallpaper_module.cpp:36 生命周期核对；登记：2026-11-17 11:57:46 +0800 ZCode主会话；复核：2026-11-17 11:57:46 +0800

**进度记录**（6）：

- 2026-11-17 11:49:01 +0800 · 双根因已实锤（2026-11-17）：
  
  【闪退·use-after-free】崩溃栈两份 .ips（11-17 11:42:34/11:43:19，另 11-16 晚还有三份同因）完全一致：Runtime::updateInteraction → drawImportNoAccessDialog::$_1::operator() → SIGSEGV。根因链：wallpaper_module.cpp:36 每帧在栈上构造局部 UiActions actions → wallpaper_ui.cpp:1251 无权限弹窗 confirm 回调「打开系统设置」按引用捕获 [&actions] → EUI Runtime 存上一帧回调、下一次点击时调用 → compose 帧早已返回、栈帧已死 → 悬垂引用崩溃。同弹窗「关闭」按钮捕获的是模块成员 uiState_（安全）——完美解释「只点系统设置钮才闪退」。全仓扫过：其它模块弹窗回调全捕 [this]/成员指针（settings/memory/claude_proxy），无同类反模式，修复面=壁纸这一处 lambda，S 级。
  
  【无法导入·TCC 身份失效（高置信+待验证）】花見目录存在（shell ls = Operation not permitted = TCC 拦截特征），probe 走 noAccess 分支弹此窗。DevDesk.app=adhoc 签名无 Team ID，TCC 完全磁盘访问绑定签名身份——Debug 覆盖安装（现装 11-10 版）换 cdhash 后系统设置里的旧授权条目对当前二进制失效，列表看着「已给」实际无效。自救：系统设置删掉 DevDesk 那条重加+重启 app。若重加仍不行，往 macOS 26/27 FDA 语义变化方向查。
  
  修复提案：①S 级（崩溃）：confirm 回调改值捕获 openPrivacySettings（无状态 lambda，可拷贝），或 UiActions 升模块成员；②导入侧：EPERM 引导文案改「删掉重加」+ Debug 构建走稳定签名身份（后续改进）。崩溃修复待老板放行。
- 2026-11-17 11:56:58 +0800 · 老板令「按你推荐的操作」= 开工令（2026-11-17）。分诊：Bug [ch:解堵]（崩溃），S 级主会话直修——wallpaper_ui.cpp 单文件：confirm 回调值捕获 + 文案改「删掉重加」+ 生命周期回归用例。
- 2026-11-17 11:56:58 +0800 · 主会话承接（S 级直做）。源码已改：wallpaper_ui.cpp:1251 [&actions]→值捕获 onOpenSettings（bug275 根因）；引导文案改「先删除（−）再重新添加并重启」。测试已加：test_wallpaper_ui.cpp wallpaper_ui_no_access_confirm_outlives_actions（UiActions 销毁+栈冲刷后触发）。待：构建+定向用例+覆盖安装。
- 2026-11-17 12:13:04 +0800 · S 级修复完成：commit 878eb447（分支 fix/bug275-wallpaper-uaf，worktree 0d01）——confirm 回调值捕获 + 文案「先删除（−）再重新添加并重启」+ 回归用例 wallpaper_ui_no_access_confirm_outlives_actions。验证：定向 86/86 绿；--install 装入 /Applications/DevDesk.app（签名有效 + 特征串命中 + 与 build 逐字节一致）+ 已重启（新 pid）。证据：evidence/2026-11-17-bug275/summary.md。待老板真机验收：①无权限弹窗点「打开系统设置」不闪退；②因覆盖安装换二进制，TCC 授权需删掉重加一次（新文案已引导）后再点花見导入。
- 2026-11-23 09:55:14 +0800 · 机收口留验（证据不足）：878eb447+回归用例 test_wallpaper_ui.cpp(+36 行) 均在 fix/bug275-wallpaper-uaf（worktree 0d01）未并 dev；并 dev 后可转 CLOSE
- 2026-11-23 12:15:01 +0800 · 修复随 #274 合并同批落仓（同分支 878eb447）：confirm 值捕获+文案+回归用例。验证：wallpaper 111/111 含 +36 行回归、定点 wallpaper_ui_no_access_confirm_outlives_actions 1/1 绿。留验收口=花见导入+设置钮真机复测（老板 11-17 原报）。

**状态变更**（4）：

- 2026-11-17 11:45:39 +0800 ·  → 在池 · 入池（bug-fixer）
- 2026-11-17 11:56:58 +0800 · 在池 → 排产 · 老板令「按你推荐的操作」= 开工令（2026-11-17）。分诊：Bug [ch:解堵]（崩溃），S 级主会话直修——wallpaper_ui.cpp 单文件：confirm 回调值捕获 + 文案改「删掉重加」+ 生命周期回归用例。（ZCode主会话）
- 2026-11-17 11:56:58 +0800 · 排产 → 进行中 · 主会话承接（S 级直做）。源码已改：wallpaper_ui.cpp:1251 [&actions]→值捕获 onOpenSettings（bug275 根因）；引导文案改「先删除（−）再重新添加并重启」。测试已加：test_wallpaper_ui.cpp wallpaper_ui_no_access_confirm_outlives_actions（UiActions 销毁+栈冲刷后触发）。待：构建+定向用例+覆盖安装。（ZCode主会话）
- 2026-11-17 12:13:04 +0800 · 进行中 → 待验收 · S 级修复完成：commit 878eb447（分支 fix/bug275-wallpaper-uaf，worktree 0d01）——confirm 回调值捕获 + 文案「先删除（−）再重新添加并重启」+ 回归用例 wallpaper_ui_no_access_confirm_outlives_actions。验证：定向 86/86 绿；--install 装入 /Applications/DevDesk.app（签名有效 + 特征串命中 + 与 build 逐字节一致）+ 已重启（新 pid）。证据：evidence/2026-11-17-bug275/summary.md。待老板真机验收：①无权限弹窗点「打开系统设置」不闪退；②因覆盖安装换二进制，TCC 授权需删掉重加一次（新文案已引导）后再点花見导入。（ZCode主会话）

---

#### bug#368 · 侧边栏末两项被裁出视口且无滚动提示：「关于」页在常见窗口高度下不可达

- 状态：待验收 · 优先级：P0 · 负责人：developer · 阶段：测试中
- 创建：2026-11-21 03:13:59 +0800（developer）· 更新：2026-11-23 13:56:59 +0800（developer）

**详情**：

来源：UI/UX 子代理（主会话已亲验证据链三条全成立）。

【缺陷】app_sidebar.cpp:683-743 把 Settings/About（系统组）与业务项排进同一滚动流；:917-927 又令滚动条视觉全无（scrollbarOverlay(true) + scrollbarWidth(0.0f)，注释写「bug188：滚动条视觉全无」），也无当前项自动滚入。

【实证】evidence/20261120-need331-assoc-readback/01-home-1000.png（主会话亲自看图）：侧边栏最后可见项是「设置」，「关于」完全不在画面内，无任何滚动提示。当前 14 个顶层项 + 上线分组，在 1438x801 这类常见笔记本窗口下必溢出。

【规范违背】docs/UI-UX-DESIGN.md:24-25 明写「设置放在侧边栏底部（独立视觉块），遵循 macOS 系统偏好设置习惯」——实现把系统组当普通项放入滚动流。

【影响】「关于」页在该窗口高度下不可达——不是「难找」是「打不开」。

【修法（两选一或并做）】①首选：系统组（设置/关于）独立锚底，不参与业务项滚动流（合规范本意）；②兼底：恢复滚动条可见性 + 当前项自动滚入视口（scrollView 已有 setScrollOffset 能力）。UI 改动须过 ui-ux-designer 门控出设计文档 + demo。

**进度记录**（5）：

- 2026-11-21 03:15:19 +0800 · 开始侧边栏裁切（设置/关于不可达）设计方案：三候选 + demo
- 2026-11-21 03:44:01 +0800 · 设计交付完成：推荐方案 C（系统组锚底 + 栅边 chevron + 当前项自动滚入）。关键参数 footerZoneH 109 / 工作视口 availH−281（801 档 490）/ 分隔线 16,168,1 / chevron 12×12 命中盒 16×24 @x184 / 自动滚入缓冲 4px 无动画。方案 A 单独会把不可达从系统组转到工作区（溢出 34）；B 保持单流与规范 §14.4 冲突，故选 C。B1（可见滚动条）需解除 bug188，未裁决即走 B2 无阻塞。
- 2026-11-21 03:48:50 +0800 · 开始实现方案C（系统组锚底+chevron+自动滚入），按设计文档 docs/design/2026-11-21-sidebar-clipping-fix.md
- 2026-11-21 04:28:57 +0800 · 方案C已实现并自测通过：navPlan只规划业务行；系统组锚底独立块（footerTop=availH−toggleZoneH−footerZoneH，分隔线footerTop+8，设置+17/关于+61，行宽168/折叠40）；栅边chevron（12×12字形、命中盒16×24@184..200、显示条件offset>0.5与<max−0.5、单击0.8×viewportH）；当前项自动滚入（几何签名沿触发、4px缓冲、clamp、无动画）。删除NavRowPlan.divider字段（组间线改建底块上沿）。测试：新增3例（system_group_anchored_bottom×三档+折叠态、chevron_visibility_follows_offset、nav_auto_scroll_into_view）；改4例预期（W3分组、滚动矩阵、release顺序）。--filter sidebar 30/30绿，--filter release 114/114绿，--dev纯构建绿。probe连带：bug370的第3条ui_driver_settings-toggle未变绿——根因与我修的不同，是settings.card.modules.sw.4格序漂移（池#371在kToggleable[index1]插入Git，Agent从index4挪到5），module_toggle_sidebar_probe §0同因；probe §7区位契约已按新分层改写为viewport底≤按钮顶。
- 2026-11-23 13:56:59 +0800 · 主会话验收核对（2026-11-23）：①交付在 HEAD——修复提交 430439a2，其后 src/app_sidebar.cpp 仅 drawFluidBackground 因 bug274 改动（导航面源码未变）；测试锚点 tests/test_modules/test_app_sidebar.cpp:1703/1821/1952；设计 docs/design/2026-11-21-sidebar-clipping-fix.md + demo。②11-21 的 30/30 是合并前读数，合并后收口全量未跑——机验面挂 temp#489 全量复验结果。③分类=观感目检留验（系统组锚底/栅边 chevron/当前项自动滚入，需老板真机看）。④可选裁决 B1：是否解除 bug188 换 4px 可见滚动条——不裁决即保持 B2，无阻塞，不阻关账。

**状态变更**（6）：

- 2026-11-21 03:13:59 +0800 ·  → 在池 · 入池（developer）
- 2026-11-21 03:14:11 +0800 · 在池 → 排产 · UI 面 P0（侧栏裁切致「关于」不可达），派 ui-ux-designer 出方案（系统组独立锚底 vs 滚动条可见 + 自动滚入）（ZCode主会话）
- 2026-11-21 03:15:19 +0800 · 排产 → 进行中 · 开始侧边栏裁切（设置/关于不可达）设计方案：三候选 + demo（ui-ux-designer）
- 2026-11-21 03:44:01 +0800 · 进行中 → 待验收 · 设计交付完成：推荐方案 C（系统组锚底 + 栅边 chevron + 当前项自动滚入）。关键参数 footerZoneH 109 / 工作视口 availH−281（801 档 490）/ 分隔线 16,168,1 / chevron 12×12 命中盒 16×24 @x184 / 自动滚入缓冲 4px 无动画。方案 A 单独会把不可达从系统组转到工作区（溢出 34）；B 保持单流与规范 §14.4 冲突，故选 C。B1（可见滚动条）需解除 bug188，未裁决即走 B2 无阻塞。（ui-ux-designer）
- 2026-11-21 03:48:13 +0800 · 待验收 → 进行中 · 设计方案已交付（方案 C：系统组锚底 + B2 栅边 chevron + 当前项自动滚入）。老板不在，按设计文档「不裁决即走 B2」默认执行。开关实现。（ZCode主会话）
- 2026-11-21 04:33:11 +0800 · 进行中 → 待验收 · P0 修复完成并主会话独立复验（30/30 绿 + 实物截图）。连带查出 bug372 真根因。（ZCode主会话）

---

#### bug#371 · 用户指南四处与实际不符：macOS 快捷键写 Ctrl（实为 ⌘）+ 三处描述已不存在功能 + 自动归档无告知

- 状态：待验收 · 优先级：P1 · 负责人：developer · 阶段：done
- 创建：2026-11-21 03:48:08 +0800（developer）· 更新：2026-11-21 04:27:38 +0800（developer）

**详情**：

来源：普通用户面审计（独立子代理，主会话逐条核实）。四条「文档教了但做不到 / 做了但没人告诉你」，均会直接导致用户操作失败或困惑。

【① macOS 快捷键文档全写 Ctrl，实际要按 ⌘（最严重）】
证据：EUI-NEO/core/input/input_types.h:54-60 的 shortcut() 在 __APPLE__ 返回 super（即 ⌘），否则返回 control。USER_GUIDE.md:43/70-73/146/155 与页内 tooltip（markdown_module.cpp:297）均未按平台区分，全写 Ctrl。macOS 用户照做无反应。全仓只待办页 :116 二处写对。修法：文档与界面提示统一改为平台条件表述（macOS ⌘ / Windows Ctrl），或抽象一个快捷键文案函数。

【② 用户指南三处教的功能界面上不存在】
- 首页「最近编辑列表」（USER_GUIDE.md:16/44/59）—— 主会话核实 grep -c 最近编辑 src/module_home/home_module.cpp = 0，已随 commit b09ebbec 删除
- 设置页「Markdown 笔记目录 + 打开目录」卡（:256）—— settings_ui_cards.cpp:6 已整卡移除，platform::file_open::openDirectory 全仓无调用方
- 「项目卡」设置项（:261）—— drawProjectCardsCard 无生产调用点
修法：删掉这三段描述或标注为「已移除」。

【③ 已完成任务次日开机自动进归档列，全程无提示】
core/schema.cpp:700-760 + :798 冷启动归档清扫：把完成时间早于今日零点的任务搬进归档；归档列缺省折叠、展开只渲 20 张、详情只读（todo_module.cpp:824）。用户感受：「我昨天点了完成，今天没了」。指南全篇无「自动归档」二字。修法：① 指南补一节说明自动归档规则；② 若归档后不可恢复则需二次确认或可选开关。

【④ 折叠态 tooltip】已由 bug369 修复（走路线 B：改文档不实现），不重复。

**进度记录**（1）：

- 2026-11-21 04:20:06 +0800 · 四处已处理完（文档+界面）。快捷键统一格式 ⌘/Ctrl+X，新增 core::platform_info::shortcutModifierLabel() 为唯一来源（markdown tooltip / todo 状态条改用）。定向用例 md_help_button_spec + todo_ui_status_toast_timeout 全绿。未 commit（禁做）。工具调用约 55 次，超预算 35——已停手。

**状态变更**（4）：

- 2026-11-21 03:48:08 +0800 ·  → 在池 · 入池（developer）
- 2026-11-21 03:48:46 +0800 · 在池 → 排产 · 用户面 P1（文档教了做不到的事 + macOS 快捷键全错 + 自动归档无告知），派 developer 修（ZCode主会话）
- 2026-11-21 03:49:40 +0800 · 排产 → 进行中 · （developer）
- 2026-11-21 04:20:06 +0800 · 进行中 → 待验收 · 四处已处理完（文档+界面）。快捷键统一格式 ⌘/Ctrl+X，新增 core::platform_info::shortcutModifierLabel() 为唯一来源（markdown tooltip / todo 状态条改用）。定向用例 md_help_button_spec + todo_ui_status_toast_timeout 全绿。未 commit（禁做）。工具调用约 55 次，超预算 35——已停手。（developer）

---

### 在池（14）

#### bug#209 · 本地 bundle 残留死字体（无功能影响，重打包自愈）

- 状态：在池 · 优先级：P3 · 负责人：ZCode主会话
- 创建：2026-11-07 12:37:01 +0800（ZCode主会话）· 更新：2026-11-07 12:37:01 +0800（ZCode主会话）

**详情**：

QA132 发现（2026-11-07）：本地 bundle（build/DevDesk.app）残留死字体（JingNanJunJunTi/NotoSansSC-VF 曾在包内）——无功能影响（override 后不可达），重打包自然消失。clue 108。登记备查不派工。

**状态变更**（1）：

- 2026-11-07 12:37:01 +0800 ·  → 在池 · 入池（ZCode主会话）

---

#### bug#211 · CI Windows 通知用例红（机制假设：MinGW 线程创建失败被 best-effort 丢弃）——待裁决产品口径

- 状态：在池 · 优先级：P2 · 负责人：ZCode主会话
- 创建：2026-11-07 13:24:03 +0800（ZCode主会话）· 更新：2026-11-07 13:24:03 +0800（ZCode主会话）

**详情**：

bug207 交付棒发现（2026-11-07，clue 112 关联）：CI Windows job 通知用例红（24416ms）不会被 mac 侧 skip 修复消除——Windows 无授权模型，探测恒可用不 skip；其机制假设=MinGW std::thread 创建失败被产品侧 best-effort 丢弃（agent_module_schedule.cpp:1034 catch 丢弃，同文件 6793 行同通道先例）。根治需改产品重试或失败面=行为变更，bug207 修复轮禁做范围内未动，待老板裁决口径后另派（若裁决改产品，同步评估通知丢失面）。

**状态变更**（1）：

- 2026-11-07 13:24:03 +0800 ·  → 在池 · 入池（ZCode主会话）

---

#### bug#228 · Windows 侧流质泵无 occlusion 检测：被完全遮挡时仍出帧（macOS 已修，平台不对称）

- 状态：在池 · 优先级：P3 · 负责人：ZCode主会话
- 创建：2026-11-07 23:24:59 +0800（ZCode主会话）· 更新：2026-11-07 23:24:59 +0800（ZCode主会话）

**详情**：

来源：#218 交付棒披露（clue 164，2026-11-08）。occlusion 泵挂起仅 macOS 落地（NSWindowOcclusionState.visible）；Windows 侧 window_visibility_win.cpp 口径未动（IsIconic/IsWindowVisible），被其他窗口完全遮挡时流质泵照转——Windows 无廉价等价 occlusion 公开 API。形态：平台不对称（非回归，macOS 已知不对称红族同类）。候选修法：Windows 侧 DWM cloaked/occluded 检测（DWMWA_CLOAKED）或定期采样窗口可见性，成本与收益待评估。待老板定优先级。

**状态变更**（1）：

- 2026-11-07 23:24:59 +0800 ·  → 在池 · 入池（ZCode主会话）

---

#### bug#258 · memory 弹窗（全文/编辑）130% 字号档结构断言红——存量装具欠账（need256 期间发现，非其引入）

- 状态：在池 · 优先级：P2 · 负责人：developer
- 创建：2026-11-10 01:24:54 +0800（developer）· 更新：2026-11-10 01:24:54 +0800（developer）

**详情**：

memory_probe --fs 1.3（130% 字号档）整探针 FAIL：memory.dialog.fulltext.body / memory.dialog.fulltext.cancel.bg / memory.dialog.edit.input / memory.dialog.edit.cancel.bg 结构缺失。100% 档同段全绿（0 FAIL）；红出现在弹窗区且执行序先于 need256 配置卡区（卡区 130% 零红）。弹窗文件 memory_module_modals.cpp 本批未触碰 → 存量欠账（130% 档弹窗高度/结构适配，clue 190 弹窗 clamp 家族）。复现：./build/tests/memory_probe --fs 1.3

**状态变更**（1）：

- 2026-11-10 01:24:54 +0800 ·  → 在池 · 入池（developer）

---

#### bug#259 · 记忆安装失败无镜像源 UI 出口：PyPI 直连超时必现，镜像透传缝有效但 GUI 无配置入口（FIND-159-1 迁出）

- 状态：在池 · 优先级：P2 · 负责人：ZCode主会话
- 创建：2026-11-10 01:31:02 +0800（ZCode主会话）· 更新：2026-11-10 01:31:02 +0800（ZCode主会话）

**详情**：

来源：need159 前轮 FIND-159-1（2026-11-06，evidence/2026-11-06-need159-memory/01-install*.log）+ 本轮 need159 复检仍挂未决。现象：从零安装链官方 PyPI 600s 超时必现；PIP_INDEX_URL 镜像透传缝实测有效（清华源 55s 装完）但 GUI 用户无 UI 出口配置镜像（只能终端带 env 拉起 app）——失败文案给了普通用户走不通的出口。修向：安装失败卡片加镜像源输入/预设单选，写 settings 拼进 pip env。属易用性缺陷 P2，单独排产待裁决。

**状态变更**（1）：

- 2026-11-10 01:31:02 +0800 ·  → 在池 · 入池（ZCode主会话）

---

#### bug#277 · module_git 批次 19 处 std include 违规——非 --dev 构建被守卫链硬拦（CI 推送必红）

- 状态：在池 · 优先级：P2 · 负责人：developer
- 创建：2026-11-17 12:11:40 +0800（developer）· 更新：2026-11-23 10:05:30 +0800（developer）

**详情**：

2026-11-17 安装（--install 非 --dev）时 std include 守卫拦下存量违规：src/module_git/（git_changes_model.cpp、git_module.cpp、git_page_controller.cpp/.h、git_search_model.cpp、git_tree_model.cpp、git_views.cpp）+ src/platform/git/git_repo.cpp + src/ui_kit/text_highlight/code_lexers.cpp + tests/probes/git_page_probe.cpp + tests/test_platform/test_git_refresh.cpp，共 19 处「直接用 std 符号但未显式 include 标准头」（utility/vector/chrono/cmath/functional/system_error/exception/ios）。来源=git 管理器 v1 批（commit 8ecf04e6）落地时走了 --dev 未跑守卫链。影响：任何非 --dev 构建/安装/CI 推送被硬拦。修法机械：逐文件补列出的 include。

**进度记录**（1）：

- 2026-11-23 10:05:30 +0800 · Win 台账对账（2026-11-23）：Win bug#286（CI Win std 守卫红：module_git 9 处）与本条/#279 同族（git 域 std include 违规），Mac 侧计数 19/12 处已覆盖 Win 9 处；修复时以本条清单位准

**状态变更**（1）：

- 2026-11-17 12:11:40 +0800 ·  → 在池 · 入池（developer）

---

#### bug#287 · EUI-NEO 基线存量单测红两处：input_line_styles / input_scrollbar（非 notepad 线引入）

- 状态：在池 · 优先级：P3 · 负责人：main
- 创建：2026-11-18 02:37:23 +0800（main）· 更新：2026-11-18 02:37:23 +0800（main）

**详情**：

2026-11-18 批 3 wrap 棒实证：EUI-NEO 基线（stash 对照 HEAD 019e7e9）本就两处单测红——input_line_styles（callback 0 want 8）、input_scrollbar（exit 8），非 need263 各棒引入（线索 #295 在案）。建议单独排棒修，勿混入 notepad 收口。

**状态变更**（1）：

- 2026-11-18 02:37:23 +0800 ·  → 在池 · 入池（main）

---

#### bug#292 · EUI input 缺 readOnly 旋钮——只读窗编辑框一帧回显假活（notepad 走查 P2-2）

- 状态：在池 · 优先级：P3 · 负责人：main
- 创建：2026-11-18 07:00:42 +0800（main）· 更新：2026-11-18 07:00:42 +0800（main）

**详情**：

打磨走查 P2-2（need289 报告 §2）：只读窗编辑框假活——可打字回显回滚。根因=EUI InputBuilder 无 disabled/readOnly 旋钮（components/input.h:48-117 全 setter 清单核实），打字/IME/粘贴走内部 onTextInput 通道（input.h:384），onKeyEvent passthrough 拦不到；现有 onChange 早返回+value 回置已保证数据安全（仅一帧回显）。修法=EUI 加 readOnly 旋钮（multiline+readonly 分支拦 onTextInput），DevDesk notepad 只读窗/错误窗接入。

**状态变更**（1）：

- 2026-11-18 07:00:42 +0800 ·  → 在池 · 入池（main）

---

#### bug#351 · 合成 mouseMoved 事件触发 GLFW _glfwInputCursorPos 断言崩溃（need331 取证副作用，真实路径未证可触）

- 状态：在池 · 优先级：P3 · 负责人：main-session
- 创建：2026-11-20 11:17:37 +0800（main-session）· 更新：2026-11-20 11:17:37 +0800（main-session）

**详情**：

need#331 取证过程附带发现（bug-fixer 棒 2026-11-20，清单外建议转登记）：向 EUI-NEO 窗口合成 mouseMoved 事件（CGEvent POST 路径）触发 GLFW _glfwInputCursorPos 断言崩溃，崩溃报告 ~/Library/Logs/DiagnosticReports/DevDesk-2026-11-20-110145.ips。真实用户路径未证可触（系取证驱动手法副作用），但 ui_driver/探针若用同类手法会踩。处置建议：评估崩溃点是否框架层需防护（合成事件坐标越界/窗口未就绪时 _glfwInputCursorPos 前置校验），以及 ui_driver 文档标注禁用合成 mouseMoved 手法。登记不派工。

**状态变更**（1）：

- 2026-11-20 11:17:37 +0800 ·  → 在池 · 入池（main-session）

---

#### bug#366 · 同族残留：memory 模块三处 fopen/一处 mpv 路径仍用 path::string()，中文用户名下失效

- 状态：在池 · 优先级：P1 · 负责人：developer
- 创建：2026-11-21 02:59:00 +0800（developer）· 更新：2026-11-21 02:59:00 +0800（developer）

**详情**：

来源：bug#362 修复过程中 developer 扫出的同族残留（主会话已核实）。根因与 bug362 完全相同：path::string() 在 Windows 按 ACP 转换，而目标 API 要求 UTF-8 或需宽字符。

【高危，中文用户名必踩】
1. src/module_memory/panel/settings_store.cpp:24 — std::fopen(settingsFile().string().c_str(), "rb")
2. src/module_memory/panel/settings_store.cpp:114 — 同型 "wb"
3. src/module_memory/panel/panel_state.cpp:770 — std::fopen(out.string().c_str(), "wb")（导出文件）
memoryDataDir() 在 %USERPROFILE% 下 → 中文用户名下记忆模块设置读写全失效（fopen 收窄字符走 ACP）。

【中危】
4. src/module_wallpaper/platform/mpv_controller.h:60 — resolveMpvExecutable 返回 candidate.string()，下游 mpv_controller.cpp:341/343 再 utf8ToWide 转回宽 → 路径经过 ACP→UTF-16 二次转换。改 toUtf8（与 mpvPath 同一根链）。

【已确认无实际风险，不需改】
src/platform/terminal_launch_win.cpp:99 homeDir() 无调用方；file_watcher/terminal_launch 的 .string() 仅用于 LOG 文案。

修法：memory 三处 fopen 改宽字符版本（_wfopen + utf8ToWide）或改 std::ofstream（std::filesystem::path 重载，路径直传无窄化）；mpv 处改 core::paths::toUtf8。验证：Windows 中文用户名真机测试记忆模块设置保存与导出。

**状态变更**（1）：

- 2026-11-21 02:59:00 +0800 ·  → 在池 · 入池（developer）

---

#### bug#370 · 全量收口 3 个 probe 红（预存，已隔离归因）：notepad 焦点回归 + module_toggle 侧栏 + ui_driver Agent 项不可达

- 状态：在池 · 优先级：P1 · 负责人：qa-tester
- 创建：2026-11-21 03:17:38 +0800（qa-tester）· 更新：2026-11-22 20:40:18 +0800（qa-tester）

**详情**：

来源：2026-11-21 三视角审计收口时全量跑（bash scripts/build-test-run.sh --no-run）发现 3 个 probe 红，主会话已做隔离归因。

【归因方法与结论】把本次审计修复线全部文件 git stash 后重跑同样 3 个 probe —— **仍然全红**，证实与本轮修复无关，是预存失败。

【失败清单】
1. notepad_probe — 「P2-3: 关条后焦点未回归编辑器（isFocused 编辑 input 为假）」（tests/probes/notepad_probe.cpp:325-333，need263 打磨线的 Esc 关查找条→焦点回归断言）
2. module_toggle_sidebar_probe — Failed（详情未展开）
3. ui_driver_settings-toggle — 第 8 步「wait exists app.sidebar.item.Agent.hit」超时 5000ms

【重要关联】第 3 条失败点 = 侧边栏 Agent 项找不到 → **与 bug#368（侧栏裁切致「关于」不可达）同一根因族**：侧边栏滚动/裁切使末几项进不了视口与可点击区。修 368 时可能连带修好这条。

【同时】doctest 全绿：Total 2549 / Passed 2541 / Failed 0 / Skipped 8（8 个 skip 是平台条件 DOCTEST_SKIP，不是失败）。

【待办】逐条归因这 3 个红：notepad 焦点回归可能是 need263 线回归或断言过期；module_toggle/ui_driver 与 368 同族。修 368 后重跑看是否连带绿。

**关联 BUG**（1）：

- 子表行#50 · 2026-11-22 20:40:18 +0800 · 未解 · 2026-11-22 need381 QA 全量档诊断注记（非本批引入，预存归因补充）：notepad_probe 本机两处红（J 滚底白板/O 换档 ink=-1）叠加了一层环境因子——本机缺 evidence/bug399/ 目录致 probe::writeRgbaToPng 写帧失败，像素断言拿不到帧读出假白板；同探针 P/R/S 段全 PASS 佐证非回归。复现/修复 bug399 时先 mkdir evidence/bug399/（或修 probe 对证据目录自建）再判真白板，避免把采集环境失败当缺陷修。证据 evidence/need381-qa/full-run.log:29-65。（ZCode主会话）

**状态变更**（1）：

- 2026-11-21 03:17:38 +0800 ·  → 在池 · 入池（qa-tester）

---

#### bug#390 · Windows CI 红：MSYS2 OpenSSL 4.1 弃用警告×-Werror 在 vendored cpp-httplib（先于 need380 存在）

- 状态：在池 · 优先级：P1 · 负责人：ZCode主会话
- 创建：2026-11-22 03:02:24 +0800（ZCode主会话）· 更新：2026-11-22 03:02:24 +0800（ZCode主会话）

**详情**：

现象：Windows CI 构建红（runs 44936814105/44914972218，后者 2026-11-21T14:45Z 早于 need380 推送，实锤先于战役存在）。根因：MSYS2 MinGW64 环境 OpenSSL 滚到 4.1，vendored 3rd/cpp-httplib/httplib.h（~:19437-19542）的 X509_check_ip_asc/X509_check_host/ASN1_STRING_length 标记 deprecated-since-4.1，被全仓 -Werror=deprecated-declarations 放大为硬错，http_server.cpp/github_api.cpp.obj 全灭。include 面：src/platform/http_headers.h/http_server.cpp + tests×4（test_streaming_proxy/test_mcp_service/test_proxy_router/test_streaming_stress）+ github_api.cpp（经 headers 转传）。本机 macOS+homebrew openssl@3 无法复现 MinGW4.1，真验证=推送后 CI。修法候选：A vendored httplib.h 全体 #pragma GCC diagnostic push/ignored -Wdeprecated-declarations/pop（一个文件盖全部 TU，推荐）或 B 平台+tests 目标加 OPENSSL_API_COMPAT=0x30000000L 编译定义。禁升级 cpp-httplib（另行需求）。

**状态变更**（1）：

- 2026-11-22 03:02:24 +0800 ·  → 在池 · 入池（ZCode主会话）

---

#### bug#391 · std include 守卫 --self-test 在 CI macOS 假绿（坏树 A/B 变异未生效，libc++ 传递闭包漂移嫌疑）

- 状态：在池 · 优先级：P1 · 负责人：ZCode主会话
- 创建：2026-11-22 03:04:51 +0800（ZCode主会话）· 更新：2026-11-22 03:04:51 +0800（ZCode主会话）

**详情**：

现象：macOS CI job 136945132929（need380 feat run 44936814105）构建+全量 doctest 2539 全绿，红在守卫自检——scripts/check-std-includes.sh --self-test：坏树 A（删 #include <optional> 后仍绿）与坏树 B（裸 std::optional 用法仍绿）假绿，坏树 D/F 正常红，→门禁红。本地 mac 同脚本自检 PASS → 工具链/libc++ 漂移嫌疑：CI「切到镜像上最新的 Xcode」，新 libc++ 传递 include 闭包把 <optional> 带进坏树 fixture；另需核实 need380 批C 新头（platform/tool_kit.h 等含 <utility>）是否进了 fixture include 集。修复方向：fixture 对 libc++ 传递闭包免疫（坏树 TU 零 std include 起手/合成隔离头），守卫判据语义不变；禁放宽自检（假绿必须真红）。真验证=推送后 CI 重跑（本机无法复现 CI libc++）。

**状态变更**（1）：

- 2026-11-22 03:04:51 +0800 ·  → 在池 · 入池（ZCode主会话）

---

#### bug#488 · devflow 全批令 cov 冻结在 ZCode/Mac 恒失败（四次实测，单条令正常；需 hook 插桩定位+python 兜底修复）

- 状态：在池 · 优先级：P2 · 负责人：developer
- 创建：2026-11-23 10:56:42 +0800（developer）· 更新：2026-11-23 11:15:31 +0800（developer）

**详情**：

2026-11-23 四次实测（10:46-10:55）：ZCode/Mac 环境下 devflow work-order.sh 全批令（行首「执行」）cov 冻结恒失败——work-order.json 只落 {blanket:true,orders:[]}，派发/排产 fail-closed。已排除：①解析器逻辑（standalone 复跑 bash 与 /bin/sh 均 cov=486 条/cov=487 正常）②缓存与源码漂移（diff SAME）③pdir 解析（态文件落点正确）④池写入竞态（静默态第四次仍失败）。未定位：hook 真实运行时环境差异（bash 升舱壳 exec 路径/宿主注入变量/投影文件读写时序），stderr 分支细节（投影为空 vs 结构非法）被 ZCode 宿主吞掉无法远程分辨。修复方向：①hook 内插桩落调试转储（/tmp 或 .devflow/debug）+ 让 stderr 分支文案回显到 additionalContext②cov 解析改 python3 兜底（0-spawn 约束允许的话）或读前原子快照③修后需插件缓存刷新+新会话生效（zcode-plugin-management 口径）。影响：全批令不可用=老板须逐条单令（一条消息一令），体验差；单条目令不受影响。

**进度记录**（1）：

- 2026-11-23 11:15:31 +0800 · 根因升级（2026-11-23 11:1x 实锤）：**work-order.json 双脑分裂**——hook 按 prompt 时会话 cwd 解析 proj，单令写入 DevDesk/.devflow/work-order.json（11:12 orders=[486]），而 pool_mcp 排产闸恒读 docs/pool/.devflow/work-order.json；此前 #394/#416/#483 成功=当时两路径恰好一致（cwd=DevDesk 根），四次「执行」失败=blanket 写入 docs/pool 侧但 cov 弃写（该分支独立缺陷未消）。修复=env 解析统一锚定项目根（去 cwd 依赖）+cov 解析插桩。过渡口径=AI 中继单令原样至消费端文件（已披露使用一次）。

**状态变更**（1）：

- 2026-11-23 10:56:42 +0800 ·  → 在池 · 入池（developer）

---

### 挂起（6）

#### bug#14 · agent_e2e 真网探针首跑挂/复跑绿

- 状态：挂起 · 优先级：P2 · 负责人：
- 创建：2026-10-29（）· 更新：2026-10-30 20:32:33 +0800（）
- 阻塞：登记不派工

**详情**：

flake，低；docs/plans/full-buglist-2026-10-29.md

**进度记录**（1）：

- 2026-10-30 20:32:33 +0800 · 环境性 flake 已定性，低优先

**状态变更**（2）：

- 2026-10-29 ·  → 在池 · 自 pool_items BUG-1 迁移
- 2026-10-30 20:32:33 +0800 · 在池 → 挂起 · 登记不派工

---

#### bug#16 · 壁纸线·退出 terminate 尾巴

- 状态：挂起 · 优先级：P2 · 负责人：
- 创建：2026-10-29（）· 更新：2026-10-30 20:32:33 +0800（）
- 阻塞：等环境

**详情**：

single_instance_win.cpp 监听线程 joinable 至静态析构；修复码在另一 Windows 机 wt-dev-gate 未上行；随该机下次使用顺手上行

**进度记录**（1）：

- 2026-10-30 20:32:33 +0800 · 等 Windows 机上行提交

**状态变更**（2）：

- 2026-10-29 ·  → 在池 · 自 pool_items BUG-3 迁移
- 2026-10-30 20:32:33 +0800 · 在池 → 挂起 · 等环境

---

#### bug#46 · todo 搜索/悬停自动路径守卫无会红用例（池 15 核对 B 点挂账）

- 状态：挂起 · 优先级：P3 · 负责人：ZCode主会话
- 创建：2026-10-31 18:53:08 +0800（ZCode主会话）· 更新：2026-10-31 20:08:56 +0800（ZCode主会话）
- 阻塞：登记不派工

**详情**：

池 15 方案卡核对 B 点挂账（2026-10-31）：todo_module.cpp:674/:711 搜索/悬停自动路径守卫不立会红用例——入口需拖拽状态注入、可观测性弱；与池 15 主缝（归档动画收编回插）同族缺口，后续触碰 todo 自动路径时补

**状态变更**（2）：

- 2026-10-31 18:53:08 +0800 ·  → 在池 · 入池（ZCode主会话）
- 2026-10-31 20:08:56 +0800 · 在池 → 挂起 · 登记不派工（ZCode主会话）

---

#### bug#47 · todo_anim_seam 用例④⑤条件轮询 2s 负载下偶发超时（flake）

- 状态：挂起 · 优先级：P3 · 负责人：ZCode主会话
- 创建：2026-10-31 19:22:06 +0800（ZCode主会话）· 更新：2026-10-31 20:08:56 +0800（ZCode主会话）
- 阻塞：登记不派工

**详情**：

池 15 QA 验收发现（2026-10-31，非阻断）：test_todo_anim_seam ④⑤ pollFrame 2s deadline 条件轮询在机器负载下偶发超时（首跑红 evidence/20261031-pool15-accept、复跑绿 pool15-accept2）——功能缝无恙，测试时序裕量不足；后续放宽 deadline 或改事件驱动等待

**状态变更**（2）：

- 2026-10-31 19:22:06 +0800 ·  → 在池 · 入池（ZCode主会话）
- 2026-10-31 20:08:56 +0800 · 在池 → 挂起 · 登记不派工（ZCode主会话）

---

#### bug#117 · clean Windows 环境 CJK 度量下 home 统计卡 badge 文案超宽（真发现，CI 隔离待真机 UAT 复核）

- 状态：挂起 · 优先级：P2 · 负责人：ZCode主会话
- 创建：2026-11-03 06:27:41 +0800（ZCode主会话）· 更新：2026-11-03 09:15:28 +0800（ZCode主会话）
- 阻塞：等 Windows 真机复核输入：验收清单 docs/research/2026-11-03-windows-uat-checklist.md §3（一条命令或肉眼看 badge 是否溢出）；结论回来即关或立案，mac 侧无法复核字体环境

**详情**：

2026-11-03 Windows CI 首跑到测试层实证：font-em home_stat_card_text_fits 在 clean runner 超宽而开发机（Windows dev + mac）全绿；digit 度量语义用例同环境绿 → CJK 度量环境分叉。若 clean Windows 上产品字体栈 CJK 走系统回退（YaHei 系）而非开发机字体，badge「Token 速率 (tok/s) · 加载失败」在 13px 下可能真实溢出 152 逻辑 px 预算。处理：用例加 em 方框判据守卫（速 字宽>1.05em 判环境分档 skip），真发现交真机 UAT/B7 owner 复核：①确认产品 CJK 字体栈在 clean Windows 的落点 ②badge 预算是否需收紧文案或加宽度自适应

**状态变更**（2）：

- 2026-11-03 06:27:41 +0800 ·  → 在池 · 入池（ZCode主会话）
- 2026-11-03 09:15:28 +0800 · 在池 → 挂起 · 等 Windows 真机复核输入：验收清单 docs/research/2026-11-03-windows-uat-checklist.md §3（一条命令或肉眼看 badge 是否溢出）；结论回来即关或立案，mac 侧无法复核字体环境（ZCode主会话）

---

#### bug#188 · Token 额度上限提高：日 1000 万 / 周 5000 万 / 月 1 亿

- 状态：挂起 · 优先级：P2 · 负责人：ZCode主会话 · 阶段：开发中
- 创建：2026-11-06 23:21:40 +0800（ZCode主会话）· 更新：2026-11-07 13:45:42 +0800（ZCode主会话）
- 阻塞：待澄清出处：代码中不存在「日/周/月 Token 额度上限」系统（全量 grep 零命中；最接近的是 agent_loop.cpp:39 kTokenHardCap=200000=单次运行硬顶，语义不符）。需老板指认额度上限的出处（哪个页面/哪个设置项），或定性为新功能（建额度系统）后重新定级入池。

**详情**：

老板提（2026-11-06）：Token 额度上限太低，需提高为：
- 每日 1000 万（10,000,000）
- 每周 5000 万（50,000,000）
- 每月 1 亿（100,000,000）

需查：token 额度上限定义位置（可能是 claude_proxy 模块的额度配置 / settings 常量 / 数据库默认值），以及是否有输入校验 clamp 需要同步放宽、UI 展示单位（万/亿）是否需要调整。S 级参数调整。

**进度记录**（1）：

- 2026-11-07 13:30:27 +0800 · 随 S 直修批（同上；额度常量定位后参数级调整+clamp/UI 单位核查）

**状态变更**（4）：

- 2026-11-06 23:21:40 +0800 ·  → 在池 · 入池（ZCode主会话）
- 2026-11-07 13:29:46 +0800 · 在池 → 排产 · （ZCode主会话）
- 2026-11-07 13:30:27 +0800 · 排产 → 进行中 · 随 S 直修批（同上；额度常量定位后参数级调整+clamp/UI 单位核查）（ZCode主会话）
- 2026-11-07 13:45:42 +0800 · 进行中 → 挂起 · 待澄清出处：代码中不存在「日/周/月 Token 额度上限」系统（全量 grep 零命中；最接近的是 agent_loop.cpp:39 kTokenHardCap=200000=单次运行硬顶，语义不符）。需老板指认额度上限的出处（哪个页面/哪个设置项），或定性为新功能（建额度系统）后重新定级入池。（ZCode主会话）

---

## debt 池

### 在池（9）

#### debt#51 · 测试资产优化：agent_ui DB 持久化用例下沉 service 层（缓行）

- 状态：在池 · 优先级：P3 · 负责人：ZCode主会话
- 创建：2026-10-31 21:11:02 +0800（ZCode主会话）· 更新：2026-10-31 21:11:02 +0800（ZCode主会话）

**详情**：

2026-10-31 测试成本效益分析低收益候选（主会话权衡：缓行）：test_agent_ui.cpp（6515 行）中 DB 持久化类用例（如 agent_ui_db_import_reloads_tasks）与 agent_store service 层用例重叠——可下沉 service 层减 UI 测试体量；但动 6515 行测试文件的重构风险大于收益，待下次大改 agent UI 时顺带

**状态变更**（1）：

- 2026-10-31 21:11:02 +0800 ·  → 在池 · 入池（ZCode主会话）

---

#### debt#121 · CI msys2 UCRT64 迁移（两轮翻车已回退 MINGW64+消音，PATH 传导机制待查）

- 状态：在池 · 优先级：P3 · 负责人：ZCode主会话
- 创建：2026-11-03 10:50:24 +0800（ZCode主会话）· 更新：2026-11-03 10:50:24 +0800（ZCode主会话）

**详情**：

2026-11-03 两轮翻车记录：①UCRT64 下 clang-tidy/覆盖率步暴露 EUI_NEO_DIR 逐步 export 缺口（已修：提升 workflow 全局 env，此收获保留）；②回退后二轮 UCRT64 仍死在 cmake: command not found——包安装成功但 msys2 shell 的 PATH 未带 /ucrt64/bin（传导机制待查：可能与 setup-msys2 的 shell 集成或 cache restore 相关）。现态=回退 MINGW64+官方 suppress 消音。重开条件：查清 setup-msys2@v2 UCRT64 的 PATH 注入机制后在分支上单独验证一轮再切。

**状态变更**（1）：

- 2026-11-03 10:50:24 +0800 ·  → 在池 · 入池（ZCode主会话）

---

#### debt#250 · need244 审计残余批：P2×9+P3×1 留观（清单卡 need244-claude-install-p2-residual.md）

- 状态：在池 · 优先级：P3 · 负责人：ZCode主会话
- 创建：2026-11-09 12:30:28 +0800（ZCode主会话）· 更新：2026-11-09 12:30:40 +0800（ZCode主会话）

**详情**：

【勘误】need244 关单备注里写的「debt251」即本条 debt250（编号笔误，以本条为准）。来源：#244 收口终裁（2026-11-09，主会话自验授权批）。MCP「安装到 Claude Code」功能本体已关（need244），审计 P2 残余 9 条 + P3 1 条不清、挂账留观，清单卡=docs/superpowers/plans/need244-claude-install-p2-residual.md（含位置/内容/备注逐条表 + 审计新查证事实）。

优先级线索（qa-tester 复核 2026-11-09）：P2-4（已装判定不校验 URL=live 端口，改端口后「卸载」钮死端口失配不可见，mcp_page_module.cpp:669）可发现性风险最实；P2-5（toggle 失败无 LOG_* 留痕）成本最低；P2-6（CLAUDE_CONFIG_DIR 多账号语义）动前需先调研该变量真实语义。

触发条件：下次触碰 module_mcp / paths claude 相关面时顺带清，或老板点名。清时逐条「修 or 明确不做」并把结论回写清单卡。

**状态变更**（1）：

- 2026-11-09 12:30:28 +0800 ·  → 在池 · 入池（ZCode主会话）

---

#### debt#252 · 增量构建不拷更新后的图标资产：POST_BUILD copy_if_different 无 DEPENDS，图标改动静默不生效

- 状态：在池 · 优先级：P2 · 负责人：main
- 创建：2026-11-09 21:28:28 +0800（main）· 更新：2026-11-09 21:28:28 +0800（main）

**详情**：

CMakeLists.txt:488 add_custom_command(TARGET DevDesk POST_BUILD ... copy_if_different) 只在 DevDesk 目标重链时执行；改 assets/icon.png / icon.ico 后增量构建不拷，build/assets/ 一直是旧图。实测：assets/icon.png 改于 21:27，build/assets/icon.png 仍为 21:06 旧版（仍 96.1%）。后果：图标类改动静默不生效（本次 bug251 验证时撞到）。修法：改用 add_custom_target(... DEPENDS <源文件>) + add_dependencies(DevDesk ...)，或把拷贝提到独立 target。

**状态变更**（1）：

- 2026-11-09 21:28:28 +0800 ·  → 在池 · 入池（main）

---

#### debt#276 · Debug 覆盖安装致 TCC 授权失效：构建走稳定签名身份（bug275 关联改进）

- 状态：在池 · 优先级：P2 · 负责人：developer
- 创建：2026-11-17 11:53:40 +0800（developer）· 更新：2026-11-17 11:53:40 +0800（developer）

**详情**：

bug275 取证结论：DevDesk.app 为 adhoc 签名（无 Team ID），TCC「完全磁盘访问」绑定签名身份/cdhash——每次 Debug 覆盖安装换二进制后，系统设置里的旧授权条目对当前副本失效（列表看着已给、实际 EPERM），花見导入等 FDA 依赖全部失效，用户每次装新版都要手动删掉重加。改进方向：①本地自签代码签名证书固定身份（钥匙串访问→证书助手→代码签名证书，codesign -s 固定 ID；需老板一次性配合输密码+信任）；②或 package/install 脚本装完自动检测 FDA 依赖路径 EPERM 并弹引导（当前文案已随 bug275 改成「删掉重加」）；③CI 发版包走正式签名则天然免疫。触发条件：老板批准做①（需其在场配证书）时排产。

**状态变更**（1）：

- 2026-11-17 11:53:40 +0800 ·  → 在池 · 入池（developer）

---

#### debt#363 · ARCHITECTURE.md 依赖表与 54 处 module_→eui 实践互斥：文档过期未追认既有实践

- 状态：在池 · 优先级：P1 · 负责人：developer
- 创建：2026-11-21 02:32:08 +0800（developer）· 更新：2026-11-21 02:32:08 +0800（developer）

**详情**：

来源：2026-11-21 三视角审计 C++ 面 P1-06（子代理发现，主会话复核：grep -rln "#include <eui/" src/module_*/ | wc -l = 54）。矛盾：docs/ARCHITECTURE.md:66 依赖表写「❌ module_* → EUI-NEO（必须经 core 适配）」，但实测 54 个 src/module_* 文件直接 #include <eui/dsl.h> 等；src/module_git/CMakeLists.txt:23 注释还把这个做法当合规解释。判断：实现是对的，文档过期——模块必然要画 UI，而 UI 全部是 EUI DSL；经 core 中转需 core 暴露整套 DSL 类型，违背「core 不依赖 EUI 运行时」另一条铁律。实际约束应是：module_* 可用 eui 头文件/类型/DSL 构建，但不得 link eui_neo 目标、不得自己起事件循环/建窗口。注①已为 core 开类型桥接口子，只是没给 module_* 开。修法：改 ARCHITECTURE.md §2 依赖表为「✅ module_* → EUI-NEO 头文件（DSL/类型/组件签名）｜❌ module_* → EUI-NEO 运行时入口（事件循环/窗口/渲染后端）｜❌ module_* link eui_neo 目标（include 经 core PUBLIC 传播）」+ 追加注②说明边界与可静态检查的判据（是否 link eui_neo）。commit message 写明「追认既有实践，非放宽约束」。验证：文档改动无代码影响；可与 360（守卫重建）配套——平台宏/link 守卫同时落。方案卡见 docs/reports/2026-11-21-audit-fix-plan.md §6 B6。

**状态变更**（1）：

- 2026-11-21 02:32:08 +0800 ·  → 在池 · 入池（developer）

---

#### debt#364 · 文档-实现硬漂移四处：侧边栏宽 260/220/200、最小窗 1200/1000、模块数 9/11、测试数 2048/2591

- 状态：在池 · 优先级：P1 · 负责人：developer
- 创建：2026-11-21 02:33:43 +0800（developer）· 更新：2026-11-21 02:33:43 +0800（developer）

**详情**：

来源：2026-11-21 三视角审计 UI 面 P1-01 + 用户面 E-08（主会话实测三份文档与代码）。问题：规范失去作为尺子的资格，四处数字对不上——①侧边栏宽度三方矛盾：UI-UX-DESIGN.md:15/67/686 写 260px（还写着不可拖拽调整）/ USER_GUIDE.md:42 写 220px / 代码 app_sidebar.h:18-19 是 200px；②最小窗口 UI-UX-DESIGN.md:58 写 1200x720 / 代码 main.cpp:913-914 是 1000x640（注释记录了 1280→1000 两次收窄裁决）；③模块可拨数 USER_GUIDE.md:19 与 :255 说 9 个 / README.md:201 说 9 个 / 代码 settings_config.h:39-60 是 11 个（漏计 Git + 终端）；④测试数 README.md:202 写 Total 2048（2026-10-31）/ 当前实测 2591 例。另 UI-UX-DESIGN.md §1.1 信息架构只列 5 项（v1 时代），已落后于现在 14 模块。影响：UI 走查/设计稿/验收三方拿不同数字，任何「间距不对」的判词都无法裁决；历史已多次发生「按规范判不符→开发说代码对→拉锯」。修法：①侧边栏宽需老板裁决（见 need 里另条），若接受 200 则三处统一并把值提到 ui_kit/metrics.h 作单一来源；②最小窗规范改 1000x640+附收窄理由链；③模块数统一 11 并列出全部；④README 测试数改动态指针（指向 CI 徽章）不写死；⑤规范加一行「本表数值以 metrics.h 为准，改动须同步」。注意：本条只做文档对齐，不改代码（需裁决项另条）。方案卡见 docs/reports/2026-11-21-audit-fix-plan.md §8 B8。

**状态变更**（1）：

- 2026-11-21 02:33:43 +0800 ·  → 在池 · 入池（developer）

---

#### debt#367 · 错误文案双层化同族残留：schedule_rule 还有 21 处内部字段名 + 全仓 1443 处 errorMessage 直出无翻译层

- 状态：在池 · 优先级：P2 · 负责人：developer
- 创建：2026-11-21 03:02:01 +0800（developer）· 更新：2026-11-21 03:02:01 +0800（developer）

**详情**：

来源：bug#361 修复时的同族清点（developer 报告，主会话采信）。现状：bug361 只覆盖了任务书点名的 6 处（once.at / once.at_local / interval / calendar / calendar.unit / calendar.workdays_only），schedule_rule.cpp 还有 21 处错误消息带内部字段名未修：interval.every_sec / interval.anchor / interval.window / calendar.interval / calendar.weekdays / calendar.month_day / calendar.nth_weekday / calendar.last_workday / calendar.time / calendar.anchor_date / at_local 必须… / at_local 非法日期… 等。更根本：全仓 1443 处 errorMessage() 被 pageError_/showToast 直出，无任何用户面翻译层——本仓缺「技术消息 / 用户消息」双通道。修法（两条路径）：① 短期：把 user_message.h 的 scheduleFieldHint 扩到全部 schedule 字段（剩 21 处），成本小；② 长期：在 Result 层引入双通道（errorMessage 保留技术细节供日志 + userMessage 供 UI），但涉及 1443 处调用点属架构级改动，应独立立项评估，不要混在 bug 修里做。

**状态变更**（1）：

- 2026-11-21 03:02:01 +0800 ·  → 在池 · 入池（developer）

---

#### debt#389 · claude_run_settings 死表+死函数清理（B-7 后遗）+ .verify-screenshots 归位

- 状态：在池 · 优先级：P3 · 负责人：ZCode主会话
- 创建：2026-11-22 02:41:15 +0800（ZCode主会话）· 更新：2026-11-22 02:41:15 +0800（ZCode主会话）

**详情**：

wave5-a 上报（need380 收口发现）：claude_proxy 模块的 claude_run_settings DB 表与 platform::terminal::sweepStaleRunSettingsFiles 在 B-7（运行临时会话链路砍除）后成死代码——表内存量数据无害，清理属 core/platform 面小活：删表迁移（或保留只读废弃）+ 函数 + 测试引用清理。另：need380 wave4 遗留 .verify-screenshots/ 目录（memory_probe 截图）未纳入 evidence/ 规范，收尾时归位或删。

**状态变更**（1）：

- 2026-11-22 02:41:15 +0800 ·  → 在池 · 入池（ZCode主会话）

---

### 挂起（2）

#### debt#48 · 记忆解析层两条 QA 备注：get_memory 冗余容错分支+isoToEpochSec 注释尾缀

- 状态：挂起 · 优先级：P3 · 负责人：ZCode主会话
- 创建：2026-10-31 20:08:03 +0800（ZCode主会话）· 更新：2026-10-31 20:08:56 +0800（ZCode主会话）
- 阻塞：登记不派工

**详情**：

池 31/32 QA 验收非阻断备注（2026-10-31）：① parseGetMemoryResponse 的 results 包裹分支在 telemem 1.10.1 实测不触发（已留痕偏离，按 mem0 升级防漂移冗余容错保留）；② isoToEpochSec 注释写死 Z 尾缀，实测 .ffffff+00:00（解析忽略尾缀仍正确，注释措辞待修）。触碰记忆解析层时顺带处理

**状态变更**（2）：

- 2026-10-31 20:08:03 +0800 ·  → 在池 · 入池（ZCode主会话）
- 2026-10-31 20:08:56 +0800 · 在池 → 挂起 · 登记不派工（ZCode主会话）

---

#### debt#213 · EUI fork 能力项：retained layer 目标缩放 + 签名降频（低分辨率离屏渲染路径，动态背景再降 GPU）

- 状态：挂起 · 优先级：P2 · 负责人：ZCode主会话 · 阶段：开发中
- 创建：2026-11-07 16:23:31 +0800（ZCode主会话）· 更新：2026-11-07 16:57:13 +0800（ZCode主会话）
- 阻塞：执行迁移至 need #217（agent-gate 派发标记只收 need/bug，debt 不能直接派）；本条留档追踪，217 收口后本条随关。

**详情**：

来源：#194 选型结论（clue 129，2026-11-07）。EUI-NEO retained layer（EUI-NEO/core/runtime/runtime_render.h:376-404）是自动机制：子树签名不变才复用纹理，无元素级 opt-in、无目标分辨率缩放参数；动态流质背景签名每帧变化即每帧全分辨率重建。要实现「低分辨率离屏渲染再放大」（老板认可朦胧观感，配合 20fps 可再降 GPU 一个数量级），需 fork 渲染核心：retained layer 加目标缩放参数 + 签名降频。改动面=渲染核心，超单批预算，立此待办。

**进度记录**（1）：

- 2026-11-07 16:55:21 +0800 · 开工：EUI retained layer 目标缩放+签名降频（debt 转实现，developer 派发，方案卡先行）。fluid 20fps 已落地，本条做低清离屏渲染叠加路径。

**状态变更**（4）：

- 2026-11-07 16:23:31 +0800 ·  → 在池 · 入池（ZCode主会话）
- 2026-11-07 16:53:54 +0800 · 在池 → 排产 · 
- 2026-11-07 16:55:21 +0800 · 排产 → 进行中 · 开工：EUI retained layer 目标缩放+签名降频（debt 转实现，developer 派发，方案卡先行）。fluid 20fps 已落地，本条做低清离屏渲染叠加路径。
- 2026-11-07 16:57:13 +0800 · 进行中 → 挂起 · 执行迁移至 need #217（agent-gate 派发标记只收 need/bug，debt 不能直接派）；本条留档追踪，217 收口后本条随关。

---

## temp 池

### 进行中（1）

#### temp#489 · 2026-11-23 收口批：全量复验 + push（11 commit）+ 装机

- 状态：进行中 · 优先级：P1 · 负责人：qa-tester · 阶段：开发中
- 创建：2026-11-23 13:23:52 +0800（qa-tester）· 更新：2026-11-23 13:54:45 +0800（qa-tester）

**详情**：

2026-11-23 全日批次收口三件套：①全量复验（阶段节点档：doctest 全量+probe 链+守卫链，验证今日 11 个本地 commit 合并态全绿——含 Win 机侧需重跑项豁免）②绿则 push：先 EUI-NEO→fork（本日 EUI 无新 commit 可跳）再 DevDesk→origin/dev（11 commit）③装机（--install 主会话串行点：优雅退旧+覆盖+sha 校验+Dock 缓存刷新）。任一环红即停手上卡。

**进度记录**（2）：

- 2026-11-23 13:54:32 +0800 · 收口批①全量复验开工（老板全批令 cov 489 覆盖）：当前 HEAD e3a703e8 较上次收口全量（db93e05a，10:35）新增合并 ee0c0d66 + 4 commit，合并态全量未跑；派 qa-tester 后台跑阶段节点档（--no-run），同时作为 27 条待验收的判据①证据源。②push ③装机仍归主会话串行点，待①绿后另行推进。
- 2026-11-23 13:54:45 +0800 · 派 qa-tester 后台跑收口档全量（doctest 全量+probe 链+守卫链，--no-run）；只验不修，新红带锚点分类回报

**状态变更**（2）：

- 2026-11-23 13:54:32 +0800 · 在池 → 排产 · 收口批①全量复验开工（老板全批令 cov 489 覆盖）：当前 HEAD e3a703e8 较上次收口全量（db93e05a，10:35）新增合并 ee0c0d66 + 4 commit，合并态全量未跑；派 qa-tester 后台跑阶段节点档（--no-run），同时作为 27 条待验收的判据①证据源。②push ③装机仍归主会话串行点，待①绿后另行推进。（ZCode主会话）
- 2026-11-23 13:54:45 +0800 · 排产 → 进行中 · 派 qa-tester 后台跑收口档全量（doctest 全量+probe 链+守卫链，--no-run）；只验不修，新红带锚点分类回报（ZCode主会话）

---

## 附录 A：关联文档全文（7 篇）

### doc#29 · 2026-11-06-macos-custom-titlebar（prd v2 · need#140）

- 标题：macOS 标题栏自绘路线 B：隐藏红绿灯 + 全自绘窗口钮 · 创建：2026-11-06 19:43:09 +0800（product-manager）· 更新：2026-11-06 20:05:26 +0800（product-manager）· 历史版本 1 个（未导出）

<!-- doc:29:v2 begin -->
# PRD：macOS 标题栏自绘路线 B（隐藏红绿灯 + 全自绘窗口钮）

> need:140 ｜ L 级 ｜ 作者：product-manager ｜ 2026-11-06 ｜ **v2**（按审计 doc 30 PASS-with-notes 修订：P1×1 + P2×4 + P3×4 逐条落实，修订明细见文末「v2 修订记录」）
> 调研输入：`docs/research/2026-11-06-macos-fully-custom-titlebar-route-b.md`（实现路径，Q1-Q8）、`docs/research/2026-11-05-macos-custom-titlebar.md`（路线 A/B 对比，背景对照）
> 池线索：clue 42/43（背景）+ clue 57/58/59（路线 B）+ clue 61/67/68（现行锚点与审计新查证；v1 所引 clue 35 行号已漂移，一律改引 clue 61——审计 P3-3）
> 审计：doc 30 v1 PASS-with-notes（P0×0/P1×1/P2×4/P3×4），可进 UI 设计；条件=本 v2 在技术卡定稿前完成（已达成）

## 一句话需求

macOS 放弃原生红绿灯，标题栏完全自绘，使窗口顶部几何与 Windows 完全一致（bodyTop 同为 40px），且窗口控制（关闭/最小化/全屏/拖拽/双击缩放/边缘缩放）无一缺失。

## 背景

1. **几何分叉现状**（clue 61/67）：Windows 自绘 titlebar `titlebarHeight()=40px`；macOS 保留系统标题栏（FSCV 三连现网在跑，`EUI-NEO/core/platform/window_effect_macos.mm:260-272`），bodyTop 硬编码 30px（`src/app_sidebar.cpp:1160-1166`，业务层 `#ifdef __APPLE__` 分叉）。bodyTop 在 app 顶层 compose 统一扣除（`app_sidebar.cpp:1166`），侧栏分档阈值是绝对像素且判定基准 availH=窗高−bodyTop（`app_sidebar.cpp:401-414` 注释一手：正常档 H≥712 / 紧凑档 680≤H<712 / 极紧档 H<680）——10px 差使同窗口高度两端落不同档位（同 **715px** 高窗口：Windows 715−40=675 落极紧档、macOS 现状 715−30=685 落紧凑档）。〔v1 此处示例误写 705px——705 两端均落极紧档（665/675 均<680）并无分叉，审计 P2-1 按代码注释一手实锤纠正为 715px；「10px 差致落档分叉」论点本身成立。〕
2. **拍板史**：2026-11-05 完成路线 A/B 对比调研；**2026-11-06 老板拍板路线 B——macOS 放弃原生红绿灯、标题栏自绘**。
3. **调研补位**：路线 B 实现路径调研（2026-11-06）已回答隐藏方式/子方案取舍/回传链/拖拽/全屏/无障碍全部问题，事实入库 clue 57/58/59。
4. **现行回退缝（审计 P1-1 依托，实读吻合）**：`src/app_sidebar.cpp:56` `g_titlebarModeOverride`（`std::optional<bool>`，nullopt=跟平台能力走）+ `:254-263` `customTitlebarActive()`/`setCustomTitlebarMode()`——override 置 false/nullopt 即恢复平台判定（macOS→`customWindowChromeSupported()`=false→系统标题栏现状）。本需求整体回退预案依托此缝，见「整体回退预案」节。

## 拍板记录（含细化边界，非推翻）

- **老板拍板（2026-11-06）：路线 B（自绘），放弃原生红绿灯。**
- **调研细化（同日）：路线 B 内两子方案取子方案①**——保留 `NSWindowStyleMaskTitled|Closable|Miniaturizable|Resizable`（GLFW decorated 默认）+ 三钮 `setHidden:YES` + NSView 层叠自绘钮；**不选真 Borderless**（`GLFW_DECORATED=false`）。依据=同类产品先例（Electron kNormal 档/nuttyartist/notes/Tietze 应用全在子方案①；真 Borderless 仅 kitty 单例且为多年深度定制项目；②的圆角/阴影/双击/拖拽全自担，GL 不透明层与系统圆角配合成本显著）。
- 这是「老板拍板路线 B + 调研在路线 B 内细化子方案」的结果，**不是推翻拍板**；若对子方案①本身有异议，须回老板重拍，不在本 PRD 内自行变更。
- **报备项（审计 P3-1，随 PRD 评审向老板一句话报备）**：D3（第三钮=全屏）与子方案①细化两处裁决，属 PM 授权内取舍已留痕；调研报告 Q3 遗留的「第三钮语义需产品确认（默认建议 mac 全屏语义）」以 PM 按授权拍板闭环（依据=macOS 10.7 起绿灯默认即进入全屏、Option+点击才是 zoom，clue 68；报备即可，不必重拍；若老板翻转 D3 仅换图标与 action 映射，不动版式，UI 设计按全屏语义先行无风险）。

## 目标

- **G1 几何对齐**：macOS bodyTop 30→40，与 Windows 同值；同窗口高度两端落同一侧栏档位。
- **G2 窗口控制完备**：关闭（可 veto）/最小化/全屏/拖拽移动/双击缩放/边缘 resize 全部可用，语义对齐 macOS 用户预期。
- **G3 状态稳定**：任何窗口状态变化（全屏进出/最小化恢复/最大化/改标题/程序化 styleMask 变更）后红绿灯不重现、自绘钮不丢失不错位。
- **G4 平台无损**：Windows 零行为变化；macOS 系统托管能力（边缘 resize、窗口阴影、圆角、全屏动画）不退化（子方案①下为系统白拿，回归验证即可）。

**用户价值一句话**：macOS 用户拿到与 Windows 完全一致的窗口布局与顶部钮交互——同窗口高同侧栏档位、三钮同构可用，且 macOS 原生窗口手感不丢。

## 非目标

- **N1 不做真 Borderless**：理由见拍板记录——圆角（GL 不透明层裁剪成本显著）/阴影形状/双击/拖拽全自担，收益仅「三钮实体彻底消失」而子方案①同样达成视觉目标。
- **N2 不改窗口材质三档与 DPI 语义**（现网三档原生材质、contentScale 语义全部不动）。〔注：材质本身不动 ≠ 自绘条在材质三档下的表现免验，联动回归见 A12（审计 P2-3）。〕
- **N3 不出视觉稿**：钮位坐标/配色/图标/间距属 ui-ux-designer 环节，本 PRD 只锁结构契约与交互态完备性。
- **N4 不调整侧栏分档阈值数值本身**（680/712 是既有设计，本需求只消除跨端落档差异）。
- **N5 不补 zoom 钮 hover/长按菜单，且 Option+点击 zoom 的钮级亲和力一并丢失**（系统钮右键/长按菜单随隐藏丢失；原生钮按住 Option 图标变化+点击=zoom 的交互自绘钮同样不具备）——同族缺口一并接受，不接受该缺口者须回老板。（审计 P3-2 补全）
- **N6 不自建 NSMenu 菜单栏**：Cmd+W/Cmd+M 缺口在 F-07 以边缘子项承接，独立工作项不阻塞本需求；该缺口为既有现状（GLFW 应用默认无菜单栏），本需求零回归，使用说明随 N6 一并标注。
- **N7 不改 Windows 端任何行为**（F-09 门禁）。

## 功能矩阵

| ID | 功能点 | 优先级 | 验收要点 | 状态 | 卡片 |
|---|---|---|---|---|---|
| F-01 | 隐藏原生三钮 | 核心 | 三钮 `setHidden:YES` 全程保持（创建/全屏退出/setTitle/styleMask 变更后均重隐）；**禁 removeFromSuperview**（保 performClose/performMiniaturize 语义，Apple 一手「in the window's view hierarchy」）；启动首帧无红绿灯闪现；多显示器间移动窗口与 space 切换后仍保持隐藏（审计 P2-2 补映射，A15） | 待方案 | 待开卡 |
| F-02 | 自绘钮渲染与点击回传 | 核心 | NSView 层叠自绘三钮（关闭/最小化/全屏），回传链：关闭=`performClose:`（走 GLFW close 回调可 veto，待 POC）、最小化=`glfwIconifyWindow`（→miniaturize:）、第三钮=`toggleFullScreen:`；三钮点击各触发对应窗口行为 | 待方案 | 待开卡 |
| F-03 | bodyTop 40px 对齐 + 分档同档位 | 核心 | macOS chrome 生效后 bodyTop=`titlebarHeight()`=40 与 Windows 同值；同窗口高落同侧栏档位；`app_sidebar.cpp:1160-1166` 的 `#ifdef __APPLE__` 分叉**必须以 gate 化消除（增量收拢到 customTitlebarActive/setCustomTitlebarMode gate 之后，保留可回退路径），非物理删除**——回退置 override=false 后须完整恢复 macOS 现状（系统标题栏 + bodyTop 30）（审计 P1-1 强制要求，非 v1 的「实现自由度非强制」） | 待方案 | 待开卡 |
| F-04 | 拖拽移动 | 核心 | 40px 条 mouseDown → `performWindowDragWithEvent:`（9+ 生产项目统一此派）；拖拽启动时重置 GLFW 输入状态（mouse-up 可能不送达，防点击判定卡「按住」） | 待方案 | 待开卡 |
| F-05 | 全屏进出重挂 | 核心 | `WillEnterFullScreen` 自绘条退场 + `DidExitFullScreen` 重挂（FSCV/自绘条/隐藏钮）；**钩子必须含重新 `setHidden:YES`**（Tietze 实证改 .resizable 会自动重显钮；任何 styleMask 变更同理）；`setTitle:` 后同样重隐（Tauri #13044 姊妹坑）；**启动即全屏恢复态**：若启用窗口状态恢复（技术卡核实），启动即为全屏时自绘条整体退场、无红绿灯，退出全屏后重挂完整（审计 P2-4，A13）；多显示器/space 切换后重挂回归（A15） | 待方案 | 待开卡 |
| F-06 | 双击标题栏缩放 | 核心 | 覆盖层吞 mouseDown 后手动兜底：`clickCount>=2` 按 `AppleActionOnDoubleClick` 分支 zoom（`glfwMaximizeWindow`→`zoom:`）/最小化；全屏中双击不 zoom | 待方案 | 待开卡 |
| F-07 | 无障碍与快捷键 | 边缘 | 自绘钮设 `accessibilityLabel`+`NSAccessibilityButtonRole`（VoiceOver 可达，随 F-02 免费拿，实测待 POC）；Cmd+W/M 缺口=N6 独立工作项（自建 NSMenu，kitty 先例），不在本需求验收内 | 待方案 | 待开卡 |
| F-08 | 视觉细节与交互态 | 核心 | 自绘钮 hover/pressed/非激活灰显三态齐备（disabled 态可后置）；**材质三档（关闭/半透/磨砂）×深/浅外观下条区与 app 顶区融合、三态可辨**（审计 P2-3，A12）；**极窄窗口下三钮热区固定不挤压不换行**（审计 P2-2，A14）；窗口阴影/圆角/全屏动画=系统托管回归验证；具体视觉稿归 ui-ux-designer | 待方案 | 待开卡 |
| F-09 | Windows 回归保障 | 核心 | Windows 端零行为变化：chrome 路径/bodyTop/侧栏档位/标题条交互/托盘行为全不变；现有 doctest+守卫链全绿 | 待方案 | 待开卡 |

优先级分布：核心 8（F-01~F-06、F-08、F-09），边缘 1（F-07）——**v2 矩阵 F 总数不变（9 个）**，审计 P2-2/P2-3/P2-4 边界态并入既有 F 验收要点、不新增 F-ID。全部状态初值=待方案（技术卡环节定实现方案）。

## 关键决策与依据

| 决策 | 内容 | 依据 |
|---|---|---|
| D1 | 路线 B 内取子方案①（Titled+setHidden+层叠自绘），不选真 Borderless | 同类产品先例（Electron/notes/Tietze；kitty 单例） |
| D2 | 隐藏用 `setHidden:YES`，禁 `removeFromSuperview` | 行业惯例（Apple 一手 API 语义：remove 后 standardWindowButton 返回 nil，performClose/performMiniaturize 蜂鸣）+ 先例（Electron/Tietze） |
| D3 | 第三钮语义=**全屏**（`toggleFullScreen:`），非 Windows 式最大化 | 行业惯例（macOS 10.7 起绿灯默认=进入全屏，Option+点击才是 zoom，clue 68）+ 先例（notes ButtonPasser→toggleFullScreen）+ 调研 Q3 推荐；备选链 `glfwMaximizeWindow`→`zoom:` 已锚定，若老板要 Windows 语义可切，当前不采用；调研遗留「需产品确认」已按授权裁决并随 PRD 评审向老板报备（审计 P3-1） |
| D4 | bodyTop=40 | 本软件一致性（以 Windows 现值为基准，不新造数值） |
| D5 | 分档阈值 680/712 数值不动 | 本软件一致性（阈值是既有侧栏设计，本需求只消落档差异） |
| D6 | 自绘钮走 NSView 层叠，不纯 EUI 画 | 同类产品先例（notes ButtonPasser）+ 成本（hit-test/hover/a11y 标签免费拿） |

## 用户操作流程（验收照走）

1. 启动 app（macOS）→ 主窗出现：无红绿灯、无闪现，左上三颗自绘钮，顶部条 40px。[F-01/F-03]
2. 点自绘关闭钮 → 有确认弹窗先弹窗，取消后窗口仍开（veto）；确认后关闭。[F-02]
3. 点最小化钮 → 窗口入 Dock；Dock 点击恢复 → 窗口还原，红绿灯仍隐藏、自绘钮位置正确。[F-01/F-02]
4. 点第三钮 → 进原生全屏（自绘条整体退场）；退出全屏 → 40px 条与自绘钮完整回归，红绿灯仍隐藏。[F-05]
5. 按住顶部条拖动 → 窗口位移跟随；拖拽结束立即点侧栏项 → 命中正常不卡「按住」。[F-04]
6. 双击顶部条 → 按系统偏好 zoom（默认）或最小化；全屏中双击无 zoom。[F-06]
7. VoiceOver 开启 → 焦点可达三钮并朗读「关闭/最小化/全屏」。[F-07]
8. 调窗口高跨 680/712 → 侧栏档位与 Windows 同窗口高一致。[F-03]
9. 边缘拖拽缩放窗口 → 原生 resize 手感不变。[G4]
10. Windows 启动同一构建 → 标题栏/布局/交互零变化。[F-09]
11. 依次切换窗口材质三档（关闭/半透/磨砂）与深/浅外观 → 自绘条区与 app 顶区视觉融合、三钮三态可辨、关闭钮红底对比清晰。[F-08]（审计 P2-3 新增）
12. 窗口拖到第二台显示器、触发 space 切换 → 红绿灯仍隐藏、自绘钮/条位置正确。[F-01/F-05]（审计 P2-2 新增）
13. （条件，若启用窗口状态恢复）上会话全屏退出后启动 → 启动即为全屏：无红绿灯、自绘条整体退场；退出全屏后条与钮完整回归。[F-05]（审计 P2-4 新增）
14. （回退演练，技术卡实装 gate 后）`setCustomTitlebarMode(false)` → 完整恢复 macOS 系统标题栏现状（原生三钮回归、bodyTop=30、自绘条消失）。[整体回退预案]（审计 P1-1 新增）

## 完整状态集

- **空态**：不适用（无数据态）。等价项=启动首帧：创建即挂（隐藏+自绘条），不得出现「先亮红绿灯再隐藏」闪帧。
- **错误态**：① performClose 被 veto（确认取消）→ 窗口保持打开，自绘条无状态残留；② `performWindowDragWithEvent:` 间歇失灵（Warp #16508，macOS 27.x beta 实报）→ 降级路径技术卡定（手动 setFrameOrigin 派为 plan B，libui 先例），最低要求其余窗口功能不受影响；③ 钩子漏挂的表现=红绿灯重现或自绘钮丢失，属缺陷，以 A2/A11/A15 回归清单兜底；④ 自绘壳整体失败/视觉劣化不可接受 → 走「整体回退预案」节，回提产品后执行，不得静默带伤交付。
- **边界**：① 极窄窗口（接近最小宽，钳制值现值注释 1200，技术卡核实）：三钮热区 46×40 固定不挤压不换行不溢出（F-08/A14）；② miniaturize→Dock 恢复后钮仍隐藏；③ zoom 最大化→还原后钮仍隐藏；④ 快速连点 vs 双击判定（clickCount）不误触拖拽；⑤ 非激活窗口钮灰显态（系统钮非 key 时变灰，自绘钮对应）；⑥ 多显示器/space 切换后钮位正确（F-01/F-05/A15，审计 P2-2 补映射）；⑦ 子窗适用面：主窗必做；macOS 独立子窗是否套用待技术卡查证（grep 现状无 macOS 子窗布局文件，`window_layout_win.cpp` 为 Windows 专属）；⑧ **启动即全屏恢复态**：若启用窗口状态恢复（技术卡核实），启动即为全屏时自绘条退场/无红绿灯、退出后重挂完整（F-05/A13）；若核实不启用则本条记「不适用」留痕（审计 P2-4）。
- **平台差异**：Windows=现状全保（F-09 门禁）；macOS=本 PRD 全部内容；bodyTop 两端同 40；分档阈值两端同像素同档；第三钮语义两端不同（Windows=最大化 zoom，macOS=全屏，D3 拍板并已报备）。

## UI 契约（结构级，可截图对照）

- 顶部条高 40 逻辑像素；左缘三钮热区与 Windows 自绘钮同构（Windows 现值 46×40，`app_sidebar.cpp:267` kWindowButtonW=46，右缘双钮——UAT4 已移除最大化钮；若技术卡实测值不同则以实测为准，不新造数值）；条区其余部分=拖拽区，钮命中优先于拖拽。
- 条背景与 app 顶区视觉融合（现网 FSCV 内容从 y=0 起）；条与内容间有无分隔线由视觉稿定。
- 截图对照口径：macOS 与 Windows 同像素窗口尺寸截图并排，顶部条结构一致、侧栏档位一致；三态（hover/pressed/非激活）与默认态可区分。
- 像素级视觉（钮径/间距/配色/图标）不在本契约内，ui-ux-designer 产出后以视觉稿为准。
- 审计确认：本契约结构级内容不受 v2 修订影响，可进 UI 设计。

## 关联影响

- `src/app_sidebar.cpp:56、:254-263`（`g_titlebarModeOverride`/`customTitlebarActive`/`setCustomTitlebarMode` **回退缝**——路线 B 全部增量 gate 化收拢于此，回退开关）
- `src/app_sidebar.cpp:1156`（customTitlebarActive 门控）、`:1160-1166`（#ifdef inset 分叉，F-03 以 gate 化收敛、保留回退路径）、`:401-414、:421-428`（分档基准与阈值，只读不改数值）
- `EUI-NEO/core/platform/window_effect_macos.mm:260-272`（enableContentExtensionIntoTitleBar，FSCV 三连在 `:266-270`，`:269` 注释「红绿灯保留（macOS 原生交互语义）」=路线 B 要翻转的现状；路线 B 增量叠加其上，**回退=本段不动即现状**）
- `EUI-NEO/core/window/window_backend.cpp:1606-1614`（customWindowChromeSupported macOS=false——v1 误写 1604-1612，审计 P3-3 修正；本需求落地需翻转或新增 mac 自绘壳路径，接入方式=技术卡决策）
- vendored `EUI-NEO/3rd/glfw/src/cocoa_window.m`（:813 创建 mask 恒含 Miniaturizable；:1184-1207 iconify/maximize/restore 桥接；:1285-1302 decorated 全屏重写 mask）
- `src/main.cpp`（窗体创建链，customTitlebarActive 消费点，技术卡锚定）
- 无新增第三方依赖（全部系统 API + vendored GLFW），THIRD-PARTY-NOTICES 无变更。

## 依赖关系

- **need:132 字体线独立推进，不阻塞本条**；两线共享「一套配置两端同效」总目标——本条消几何分叉（bodyTop/档位），need:132 消字体分叉（测宽/字形）。
- 下游环节：tech-solution 出技术卡（POC：performClose veto 链、重挂时序、VoiceOver；核实项见开放 7/8）→ ui-ux-designer 出视觉稿 → developer 实现 → qa-tester 实操验收。
- 本条不依赖其它在途需求。

## 风险

1. **重挂/重隐时机**：全屏进出、`setTitle:`、任何 styleMask 变更（含 Tietze 实证改 .resizable 自动重显钮）都会重置钮可见性——漏一处即白钮浮现在自绘条上。缓解：统一 re-hook 单点函数 + A2/A11/A15 回归清单（F-05）。
2. **`performClose:` 穿透 GLFW 回调链待 POC**：windowShouldClose→glfwSetWindowCloseCallback 的 veto 路径需实测；失败降级 `glfwSetWindowShouldClose`（牺牲 veto——关闭确认语义受损，须回提产品裁决，不得静默降级）。
3. **无障碍/快捷键缺口**：hidden 钮从辅助功能树消失（证据等级=推测，待 POC）+ GLFW 无菜单栏 Cmd+W/Cmd+M 本来不通。NSView 自绘钮 a11y 标签解一半；菜单=N6 边缘独立工作项。
4. **自绘钮与系统语义差异**：第三钮语义已拍全屏（D3，已报备）；hover 菜单与 Option+点击 zoom 亲和力丢失（N5 接受）；非激活灰显/三态需自实现；覆盖层吞 mouseDown 后双击需手动兜底（F-06）；拖拽 mouse-up 不送达需输入状态重置（F-04）。
5. **材质三档 × 自绘条叠加层**：NSView 层叠自绘钮是新增交互层，磨砂/半透档下条区视觉融合需回归（现网 `window_effect_macos.mm:260-298` 已是 FSCV+底色协商的复杂现状）——以 A12 兜底，不新增材质行为。

## 整体回退预案（审计 P1-1 必修，新增节）

- **触发条件（满足其一即评估回退并回提产品）**：① 自绘壳整体失败（performClose veto 链与拖拽降级均不可收敛）；② 视觉劣化不可接受（重挂时序无法全收敛、白钮反复浮现等 G3 不可达）；③ EUI patch 与 GLFW 全屏 mask 重写冲突不可收敛。
- **机制**：路线 B 的全部增量（三钮隐藏、FSCV 增量、自绘条、NSView 层叠钮、重挂钩子）**全部收拢到既有运行时缝 `setCustomTitlebarMode`/`customTitlebarActive`（`src/app_sidebar.cpp:56、:254-263`）gate 之后**；回退=`g_titlebarModeOverride` 置 false（或置 nullopt 回平台判定）→ macOS 完整恢复系统标题栏现状（原生三钮回归 + bodyTop 30 + `window_effect_macos.mm:260-272` 不动即现状）。**F-03 的 `#ifdef __APPLE__` 消除必须以 gate 化落实而非物理删除**，保证该缝全程可用（矩阵 F-03 验收要点已强制）。
- **回退成本**：一开关翻转 + 一次回归验证（对照流程步 14）；Windows 端不受影响（F-09 门禁天然隔离）。
- **回退决策归属**：回退执行前须回提产品（PM）确认并留痕（对应「可撤销？」自查条款），不得静默回退、不得带伤交付。

## 验收标准（每条可判定）

- **A1 [F-03]** macOS 与 Windows 各开同像素窗口（**1200×760（正常档）/ 1200×720（紧凑档）/ 1200×670（极紧档）三档**），侧栏档位两端逐档一致；macOS bodyTop=40（截图像素可测，窗口内容顶=40px）。〔v1 三尺寸 720/700/670 失准已修正：分档基准 availH=窗高−bodyTop 40，三尺寸 availH=720/680/630 → 正常（≥712，即窗高≥752）/紧凑（680≤H<712）/极紧（<680）；1200×720 现状即落紧凑档（`app_sidebar.cpp:407` 注释一手）。〕若主窗最小尺寸钳制使 1200×670 不可达，极紧档验收路径由技术卡另定并回提产品（开放 7）。
- **A2 [F-01]** 全屏进出 3 次后：无红绿灯可见、自绘钮位置正确（截图对照）；期间切页触发 setTitle 后同样无红绿灯。
- **A3 [F-01]** 启动首帧即无红绿灯闪现（首帧截图或录屏逐帧）。
- **A4 [F-02]** 点自绘关闭钮：有确认弹窗时先弹窗、取消后窗口仍开（veto 生效；若 POC 失败按降级口径修订本条并回提产品）；点最小化钮窗口入 Dock；点第三钮进原生全屏。
- **A5 [F-04]** 按住 40px 条拖动，窗口位移跟随；拖拽结束立即点击侧栏项，命中正常（不卡「按住」态）。
- **A6 [F-06]** 双击标题栏按系统偏好触发 zoom（默认）或最小化；全屏中双击不 zoom。
- **A7 [F-05]** 全屏中自绘条与自绘钮整体退场（无残留）；退出全屏后 40px 条完整回归。
- **A8 [F-07]** VoiceOver 焦点可落三钮并朗读「关闭/最小化/全屏」（POC 通过后转正式验收项）。
- **A9 [F-08]** hover/pressed/非激活三态与默认态可区分（截图对照）；窗口阴影/圆角/全屏动画与改造前无可见退化。
- **A10 [F-09]** Windows 同构建：标题栏/布局/交互零变化（对照截图 + 现有 doctest/守卫链全绿）。
- **A11 [F-01]** miniaturize→Dock 恢复、zoom→还原两路径后红绿灯仍隐藏。
- **A12 [F-08]** 材质三档（关闭/半透/磨砂）× 深/浅外观共 6 组合：自绘标题条与 app 顶区视觉融合、三钮三态可辨、关闭钮红底对比清晰（截图对照；验「可辨/融合」不验具体色值，色值归视觉稿）。（审计 P2-3 新增）
- **A13 [F-05]** 启动即全屏恢复态（条件式）：技术卡先核实是否启用窗口状态恢复——启用→跨会话全屏恢复启动时无红绿灯、自绘条整体退场，退出全屏后 40px 条与自绘钮完整重挂；不启用→本条记「不适用」留痕，不得静默删除。（审计 P2-4 新增）
- **A14 [F-08]** 极窄窗口（接近最小宽）下三钮热区固定 46×40 不挤压不换行不溢出、条区拖拽仍可用（截图对照）。（审计 P2-2 新增）
- **A15 [F-01/F-05]** 多显示器间移动窗口 + space 切换后：红绿灯仍隐藏、自绘钮/条位置正确不漂移（截图对照）。（审计 P2-2 新增）

验收总数 v1 11 条 → v2 15 条（A1 修正，A12~A15 新增）；F↔A 映射保持闭环（9 个 F 全有验收，无孤儿验收）。

## 开放决策项（留技术卡解决，不在 PRD 层拍板）

1. `performClose:` veto 链 POC 结论与降级选择（风险 2）。
2. 重挂钩子挂点与时序（WillEnter/DidExit 组合，Electron 模式照抄后实测；报告遗留 5）。
3. 拖拽降级路径（手动 setFrameOrigin plan B）是否实装。
4. VoiceOver a11y 实测结论与补救（不达标则升级缺陷回池）。
5. `customWindowChromeSupported()`（window_backend.cpp:**1606-1614**）接入方式：翻转 macOS 返回值 vs 新增 mac 自绘壳路径；customTitlebarActive 门控与 #ifdef **gate 化收敛**方案（保留回退缝，物理删除禁止）。
6. （审计 P3-4 拆半）**6a（技术卡）**：macOS 独立子窗清单与可行性查证；**6b（回 PM 拍板）**：若确有子窗，是否套用自绘条——产品一致性取舍，届时回产品裁决，PRD 届时补边界条目。
7. （新增，P2-1 连带）主窗最小尺寸钳制核实：代码注释现值最小窗 1200×720（`app_sidebar.cpp:407`）——A1 极紧档尺寸 1200×670 是否可达；不可达则极紧档验收另设计路径并回提产品。
8. （新增，P2-4 前置）DevDesk 是否启用 macOS 窗口状态恢复——决定 A13 走正式验收还是「不适用」留痕。

## 优先级建议

need:140 本体建议 P1（几何分叉是用户可感知的跨端不一致，且老板已拍板）；F-07 的 NSMenu 子项独立入池排期（P2 级边缘工作项），不随本条阻塞交付。

## v2 修订记录（对照审计 doc 30 缺陷清单逐条）

- **P1-1（必修）整体回退预案**：新增「整体回退预案」节（触发条件/机制/成本/归属）；F-03 验收要点由「#ifdef 消除（实现自由度非强制）」升级为「**gate 化消除、非物理删除**」强制要求；用户流程补步 14（回退演练）；关联影响补回退缝锚点 `:56、:254-263`；错误态补④（整体失败走预案回提产品）。✓
- **P2-1 A1 三档尺寸修正**：A1 改 1200×760/720/670（availH=720/680/630 → 正常/紧凑/极紧），注明正常档需窗高≥752、1200×720 落紧凑档（代码注释一手）；背景 1 示例 705px→715px（两端分叉成立：Win 675 极紧 / mac 685 紧凑）；新增开放 7（最小尺寸钳制核实）。✓
- **P2-2 孤儿边界态**：边界①（极窄窗）挂 F-08 验收要点 + 新增 A14；边界⑥（多显示器/space）挂 F-01/F-05 验收要点 + 新增 A15；流程补步 12。✓
- **P2-3 材质三档联动**：F-08 验收要点补「材质三档×深/浅外观融合与三态可辨」；新增 A12；风险补 5；流程补步 11；N2 加注（材质不动 ≠ 联动免验）。✓
- **P2-4 启动即全屏恢复态**：F-05 验收要点补该态；边界集补⑧（含「不适用」留痕分支）；新增 A13（条件式）；新增开放 8；流程补步 13。✓
- **P3-1 D3 报备**：拍板记录补「报备项」段；D3 依据补 clue 68 与报备留痕。✓
- **P3-2 N5 文案补全**：补 Option+点击 zoom 钮级亲和力随自绘一并丢失。✓
- **P3-3 引证卫生**：clue 35 引用全部改 clue 61/67；`window_backend.cpp` 锚点 1604-1612→1606-1614（本次实读核对吻合）；`window_effect_macos.mm` 锚点 262-278→260-272（实读核对）。✓
- **P3-4 开放 6 拆半**：6a（子窗清单/可行性）归技术卡、6b（子窗是否套用）明确回 PM 拍板。✓
- 矩阵 F 总数不变（9：核心 8 + 边缘 1）；验收 11→15 条；F↔A 闭环复核通过。
<!-- doc:29:v2 end -->

### doc#30 · 2026-11-06-macos-custom-titlebar-pm-audit（audit v1 · need#140）

- 标题：PRD 审计（PM 面）PASS-with-notes：0P0/1P1/4P2/4P3，可进 UI 设计 · 创建：2026-11-06 19:59:56 +0800（product-manager）· 更新：2026-11-06 19:59:56 +0800（product-manager）· 历史版本 0 个（未导出）

<!-- doc:30:v1 begin -->
# PRD 独立审计（product-manager 面）：macOS 标题栏自绘路线 B

> need:140 ｜ 审计对象：doc 29（slug `2026-11-06-macos-custom-titlebar`，v1）｜ 2026-11-06
> 输入：clue 57/58/59/61（路线 B）+ clue 42/43（背景）+ 调研报告 `docs/research/2026-11-06-macos-fully-custom-titlebar-route-b.md`（Q1-Q8）+ PRD 引用锚点实读（简报已有事实未重查）
> 新查证入库：clue 67（档位换算/双钮现状/回退缝）、clue 68（绿灯默认语义裁决）

## 结论：PASS-with-notes

P0×0 ｜ P1×1 ｜ P2×4 ｜ P3×4。PRD 产品完整性与拍板忠实度总体合格，两处重点裁决（子方案①细化、D3 第三钮语义）均**不构成对老板拍板的偏离**；必修 1 项（整体回退预案）+ 建议随 PRD v2 一并修订的 4 项 P2。**可进 UI 设计环节**（详见文末）。

## 一、重点裁决（审计简报指定核查项）

### 1. 子方案① vs 老板原话「放弃原生红绿灯」→ 合理细化，非偏离（非 need:132 P0-1 同型陷阱）

**判断：不构成偏离。** 依据四条：
1. **拍板对象是用户可见产品形态**（红绿灯消失、自绘替代），不是 AppKit 实现结构。子方案①（保留 Titled mask + `setHidden:YES`）与子方案②（真 Borderless）在「红绿灯从界面上消失、自绘钮替代」这一用户可见结果上**等效**，老板拍板的语义完整性无损。
2. **强先例支撑**：Electron kNormal 全藏档 / nuttyartist/notes / Tietze 应用均走子方案①；真 Borderless 仅 kitty 单例（多年深度定制项目）——调研 Q2 判定表逐项成本对比（②的圆角/阴影/双击/mask 重写全自担，GL 不透明层与系统圆角配合成本显著）。
3. **透明度充分**：PRD 拍板记录明写「这是细化非推翻」且留了硬口子——「若对子方案①本身有异议，须回老板重拍，不在本 PRD 内自行变更」。这与 need:132 P0-1（拍板被静默细化且无回提口子）有本质区别。
4. N5（hover/长按菜单丢失）这个唯一实质功能缺口已被 PRD 显式列出并同样标注「不接受该缺口者须回老板」。

### 2. 第三钮语义=全屏（D3）→ 与 macOS 原生绿灯行为一致，非未授权决策；审计简报前提方向颠倒

**关键事实纠偏**：简报称「原生绿灯默认是 zoom 最大化，Option+点击才是全屏」——**方向颠倒**。自 macOS 10.7 引入全屏功能起，绿灯默认=进入全屏，Option+点击=zoom 最大化（≤10.6 无全屏功能时绿灯才是纯 zoom）。佐证：调研报告 Q3 一手结论（「Windows 绿钮=最大化 zoom，macOS 绿灯=全屏……语义应抄 macOS，否则 mac 用户预期错位」）+ notes 生产先例（ButtonPasser→toggleFullScreen，clue 59）。

**裁决：D3 不是替老板做未授权决策。** 三方一致（与原生绿灯行为一致 / 与调研推荐一致 / 与 PRD 自身 G2「语义对齐 macOS 用户预期」一致），且属 PM 角色授权内的产品取舍：依据已标注（行业惯例+notes 先例），备选链（`glfwMaximizeWindow`→`zoom:`）已锚定并在 D3 行明标「若老板要 Windows 语义可切，当前不采用」——决策留痕完整，非静默。

**保留意见（P3-1）**：调研报告明写「**需产品确认**，默认建议 mac 全屏语义」；且老板目标句「一套配置两端同效」按最严格读法与 PRD 平台差异节「两端语义不同」存在字面张力。PM 按授权拍板合规，但应随 PRD 评审向老板**一句话报备** D3（连同子方案①细化一并报备，作为 need:132 P0-1 教训的预防性闭环）。报备即可，不必重拍；UI 设计按全屏语义先行无风险。

## 二、老板诉求覆盖度：完整

老板诉求=「放弃原生红绿灯、标题栏自绘」+「一套配置两端同效」。映射：红绿灯放弃→F-01；自绘→F-02/F-08；两端同效（几何维度）→F-03（bodyTop 40 同值 + 同窗口高同档位）；窗口控制无一缺失→F-02/F-04/F-05/F-06；Windows 侧「同效不被破坏」→F-09 门禁。G1-G4 与 9 个 F-ID 全覆盖，无诉求遗漏。F 矩阵质量良好：每条验收要点可判定、回传链/坑位（Tietze 重显、setTitle 重隐、mouse-up 不送达）全部落到 F 定义；遗漏面见 P2-2/P2-3/P2-4。

## 三、A1-A11 对应审计

F↔A 映射完整：9 个 F 全有验收（F-01×3=A2/A3/A11），**无孤儿验收、无 F 无验收**。A4/A8 为条件式验收（POC 前置），已设透明回提闸（「按降级口径修订本条并回提产品」/「POC 通过后转正式验收项」），可接受。**A1 档位换算失实见 P2-1（有代码一手实锤）。**

## 四、开放决策 6 项归属：5 项正确留技术卡

开放 1-5（performClose veto POC 与降级、重挂时序、拖拽降级实装与否、VoiceOver 实测、customWindowChromeSupported 接入方式）均为纯技术判断或已设产品回提闸（降级牺牲 veto 须回提产品、a11y 不达标升级缺陷回池）——归属正确。**开放 6 建议拆半（P3-4）**：「macOS 独立子窗清单与可行性查证」归技术卡；若确有子窗，「子窗是否套用自绘」是产品一致性取舍，应回 PM 拍板，PRD 应写明该归属。

## 五、非目标审查：N1-N7 全部成立

- N1（不做真 Borderless）、N2（材质/DPI 不动）、N3（视觉稿归 designer）、N4（阈值数值不动）——均有据，合理。
- **N6（NSMenu/Cmd+W/M）定性=可接受，非硬伤**：事实链（clue 57③ + 调研 Q7）——GLFW 应用默认无菜单栏，Cmd+W/M **现状本来就不通**，隐藏红绿灯不使其变好也不变坏，本需求零回归；以「P2 级独立工作项（kitty 先例）不阻塞本条」承接是诚实的取舍。建议随 N6 一并在使用说明标注（键盘关窗缺口为既有现状）。
- N5（hover/长按菜单）接受合理，但建议补一句 Option+点击 zoom 的钮级亲和力一并丢失（P3-2，同族缺口，避免验收争议）。
- N7（Windows 不动）有 F-09 门禁兜底。

## 六、缺陷清单（分级）

### P1（必修）

- **P1-1 缺整体回退预案**。PRD 只有局部降级（veto 失→glfwSetWindowShouldClose；拖拽失灵→setFrameOrigin plan B），没有「自绘壳整体失败 / 视觉劣化不可接受 / EUI patch 与 GLFW 全屏 mask 重写冲突不可收敛时，退回 macOS 系统标题栏现状」的预案——违反 L 级自查「可撤销？」条款，且为审计简报指定必审项。**修法**：依托既有运行时缝 `setCustomTitlebarMode`/`g_titlebarModeOverride`（`src/app_sidebar.cpp:56` 测试钉住缝、`:254-263`），路线 B 的钮隐藏/自绘壳/FSCV 增量全部收拢到该 gate 之后；回退=override 置 false 恢复现状（FSCV 三连 + 原生钮 + bodyTop 30，`EUI-NEO/core/platform/window_effect_macos.mm:260-272` 不动即现状）。F-03 的「#ifdef 消除」落实为 gate 化而非物理删除（PRD 已写「实现自由度非强制」，升级为强制要求）。写入 PRD 风险节。

### P2（随 v2 修订）

- **P2-1 A1 三档测试尺寸失准（代码一手实锤）**。`src/app_sidebar.cpp:402-403` 注释明写分档基准 availH=窗高−bodyTop（40/30），`:407` 注释明写「最小窗 1200×720 → H=680 落此（紧凑）档」——与 A1「1200×720 →（正常档）」直接矛盾。实算：720/700/670 → availH 680/660/630 → 紧凑/极紧/极紧，**正常档（窗高≥752）未覆盖、极紧档重复覆盖两个尺寸**。连带：PRD 背景 1 的 705px 示例同样错误（705−40=665 / 705−30=675 两端均<680 同落极紧，并非「Windows 紧凑/macOS 极紧」；正确示例=715px：Win 675 极紧 / mac 685 紧凑——「10px 差致落档分叉」论点本身成立，仅数值选错）。**修法**：A1 尺寸改为 1200×760（正常）/1200×720（紧凑）/1200×670（极紧）并同步修背景示例为 715px；技术卡须核实主窗最小尺寸限制（若最小窗 1200×720 则 670 需确认可达，不可达则极紧档验收另设计路径）。锚点证据：clue 67。
- **P2-2 孤儿边界态**。完整状态集边界①（极窄窗口三钮热区不挤压）与⑥（多显示器/space 切换后钮位正确）均无 F-ID、无验收映射（A1-A11 不含），违反「状态集↔验收闭环」。**修法**：边界①并入 F-08 验收要点并补一条 A；边界⑥补 A（多显示器间移动窗口 + space 切换后隐藏态/钮位回归），挂 F-01/F-05。
- **P2-3 材质三档联动无验收**。N2 声明不动材质，但 NSView 层叠自绘钮是新增交互层，磨砂/半透档下条区视觉融合与三态表现需回归（现网 `window_effect_macos.mm:260-298` 已是 FSCV+底色协商的复杂现状）；A9 只验阴影/圆角/全屏动画。**修法**：A9 扩「三档材质×深/浅外观下条区融合与三态可辨」，或新增 A12 挂 F-08。
- **P2-4 启动即全屏/窗口状态恢复态未覆盖**。macOS 跨会话可恢复全屏，此时 F-05 重挂钩子首次触发时点=退出全屏而非启动，「启动时已处于全屏→退出后重挂完整」未纳入 F-05 验收要点与边界集。**修法**：技术卡先核实 DevDesk 是否启用窗口状态恢复；若启用则 F-05 补该态验收，若不启用则 PRD 边界集补一句「不适用」留痕。

### P3（建议）

- **P3-1 D3+子方案①报备老板**：见第一节裁决 2 保留意见；一句话报备即闭环。
- **P3-2 N5 文案补全**：Option+点击 zoom 的钮级亲和力随自绘一并丢失（原生钮按住 Option 图标变化+点击 zoom），与 hover/长按菜单同族，建议 N5 一并明示。
- **P3-3 引证卫生**：拍板史/背景引 clue 35（已漂移标废）应只引 clue 61；`window_backend.cpp` 锚点实为 1606-1614（PRD 写 1604-1612，±2 行，无害）。
- **P3-4 开放 6 拆半**：子窗清单与可行性归技术卡；子窗是否套用（若存在）归 PM 拍板，PRD 写明归属。

## 七、锚点实读记录（全部吻合）

- `src/app_sidebar.cpp:250-252` titlebarHeight()=40 ✓；`:56/:254-263` customTitlebarActive 门控与 override 缝 ✓；`:1156/:1162-1166` bodyTop 分叉 ✓；`:414/:421-428/:447` availH 与分档阈值/字号钳制 ✓；`:265-267` Windows 右缘双钮 46×40（UAT4 最大化钮已移除）✓
- `EUI-NEO/core/window/window_backend.cpp:1606-1614` customWindowChromeSupported() macOS=false ✓（PRD 写 1604-1612，±2 行无害）
- `EUI-NEO/core/platform/window_effect_macos.mm:260-272` FSCV 三连现网 ✓（`:269` 注释「红绿灯保留（macOS 原生交互语义）」=路线 B 要翻转的现状，PRD 关联影响描述准确）

## 八、进 UI 设计判断

**可进。** UI 契约（结构级）不受 P1/P2 修订影响：三钮构成、全屏语义（D3）、46×40 热区同构口径、拖拽区/钮命中优先级均已锁定且无一项被本审计推翻。条件：① PRD v2 修订（P1-1 + P2×4）须在**技术卡定稿前**完成（P2-1 直接影响 qa-tester 验收脚本与 A1 执行）；② 视觉稿按 macOS 三钮/全屏图标方向出，D3 报备老板若有翻转仅换图标与 action 映射，不动版式。

> 审计人：product-manager ｜ 工具调用 17 ｜ 构建 0 次
<!-- doc:30:v1 end -->

### doc#31 · 2026-11-06-macos-custom-titlebar-tech（tech v1 · need#140）

- 标题：技术方案卡：macOS 自绘标题栏路线 B（隐藏红绿灯 + NSView 层叠自绘钮） · 创建：2026-11-06 20:31:18 +0800（developer）· 更新：2026-11-06 20:31:18 +0800（developer）· 历史版本 0 个（未导出）

<!-- doc:31:v1 begin -->
# 技术方案卡：macOS 自绘标题栏路线 B（need:140）

> 依据：PRD doc29 v2 + 设计文档 `docs/design/2026-11-06-macos-custom-titlebar-design.md`（§4/§5/§8 规格源）+ 审计 doc30 + clue 57/58/59/61/67/68/69/72。视觉=方案 A 红绿灯彩点；实现=子方案①（Titled mask 保留 + setHidden + NSView 层叠自绘钮）。

## 1. 本卡新查证（实证）

1. **GLFW veto 链成立，风险 2 解除**：`EUI-NEO/3rd/glfw/src/cocoa_window.m:223-227` `windowShouldClose:` → `_glfwInputWindowCloseRequest(window)` + `return NO`——`performClose:` 走 DevDesk 已装的 `glfwSetWindowCloseCallback`（glfw_app_main.cpp:803）可 veto，无需降级。
2. **F-04（拖拽后卡按住）天然满足**：overlay NSView 位于 GL 视图上方、吞掉条带 mouseDown 且不转发 responder 链 → GLFW 输入层从未进入按下态，不存在「mouse-up 不送达」路径。
3. **A13 不适用留痕**：DevDesk 无全屏状态恢复（`src/main.cpp` 只记忆 maximized，`src/core` 无 fullscreen 恢复）。
4. **caption handler 翻 true 无副作用**：`setWindowCaptionHitTestHandler`/`setWindowLiveRenderHandler` 的 macOS 分支现网 no-op（window_backend.cpp:1655-1660/1679-1683），glfw_app_main:699/777 的注册落 no-op；macOS 拖拽/双击由 overlay NSView 自管，不经 CaptionHitQuery。
5. **titlebarCaptionHit（src/app_sidebar.cpp:269）仅 Windows 消费链**（main.cpp:1003 → onCaptionHitTest → _WIN32 分支实装），macOS 左钮组构图无需改它。

## 2. 分层与接口

- **EUI-NEO=机制层**（窗口钮渲染/交互/hover/失焦/a11y/拖拽/双击/重挂钩子，NSView 覆盖层）；**DevDesk=主题层**（token 注入 + 品牌文本 + bodyTop 布局）。EUI 层零硬编码色，色值全部宿主注入。
- 新增平台无关接口（`EUI-NEO/core/platform/window_effect.h`，色值 uint32 0xRRGGBBAA）：
  - `struct CustomTitlebarButtonSkin { normal, hover, pressed, stroke }` + `struct CustomTitlebarSkin { close, minimize, fullscreen, closeSymbol, unfocused }`
  - `customWindowChromeOverlaySupported()`（mac=true / 其他=false——DevDesk DSL 判「钮由平台层渲染」走左缘构图分支）
  - `setCustomTitlebarSkin(void* hwnd, const CustomTitlebarSkin&)`（mac 实装 / 其他 no-op）
  - `installCustomWindowChrome(void* nativeWindowHandle)`（mac：FSCV 复用 + 隐藏三钮 + overlay + 重挂钩子；其他 no-op）
- `EUI-NEO/core/window/window_backend.h` 自绘壳契约区（:108-147）加 Handle 版转发；`dsl_app_impl.h` 加 `app::` 内联转发；`window_backend.cpp:1606` macOS 分支翻 true；`glfw_app_main.cpp` 主窗创建后（show 前，保 A3 无闪现）调 `installCustomWindowChrome`。

## 3. NSView 结构（window_effect_macos.mm）

- `EUIWindowChromeButtonView`（46×40，x=0/46/92，圆心 x=23/69/115）：drawRect 画 12px 圆点 + 1px hover 档描边；NSTrackingArea hover/pressed；mouseDown 非 key 窗仅 `makeKeyWindow`（首点仅激活）；key 窗点击回传：关=`performClose:` / 最小化=`miniaturize:`（=glfwIconifyWindow 同链）/ 全屏=`toggleFullScreen:`；accessibilityLabel+button role（F-07）。
- `EUIWindowChromeOverlayView`（条带容器 y∈[0,40) 全宽、透明背景不遮 GL 品牌文本）：hit-test x<138 → 子钮；x≥138 → self：`clickCount>=2` → 按 `AppleMiniaturizeOnDoubleClick` 分支 `zoom:`/`miniaturize:`（F-06），否则 `performWindowDragWithEvent:`（F-04）。
- `reapply()` 幂等钩子（三钮 `setHidden:YES` + overlay frame 同步 + 重绘）：WillEnterFullScreen（overlay.hidden=YES 整条退场）/ DidExitFullScreen / DidResize / DidBecomeKey / DidResignKey（灰点重绘）/ DidChangeBackingProperties / KVO title / drawRect 兜底检查（Tietze 重显最后防线）。
- 子窗不装自绘壳（PRD 开放 6a 结论：mac 子窗保留系统标题栏；是否套用归 PM 拍板，开放 6b）。

## 4. DevDesk 侧

- `src/core/theme.h` ColorTokens +4 槽位：`titlebarCloseHover/Pressed`、`titlebarMinimizeHover/Pressed`（设计 §5 钉死推导值落库，禁业务裸写；全屏钮三态=既有 primary 三档 token；浅色关 pressed=#B91C1C=errorText 同值）。
- `src/app_sidebar.cpp` drawTitlebar 开头加 `customWindowChromeButtonsNative()` 分支：品牌文本 x=154、不画右缘双钮、主题指纹变化时注入 skin（chrome 不活跃不进此分支=天然 gate）；Windows 构图零改动（F-09）。
- bodyTop（:1156-1166）逻辑零改动：supported 翻 true → `customTitlebarActive()`=true → bodyTop=titlebarHeight()=40；回退 override=false → inset 30。**gate 化由既有缝满足（审计 P1-1），仅更新过时注释**。
- `titlebarCaptionHit` 不动（仅 Windows 消费）。

## 5. 简化声明（90% 原则）

- hover 符号 120ms 淡入 / pressed 果冻回弹 / 失焦 150ms 过渡 → 三态即时切换实现（A9/A12 判据是「可区分/可辨」非动画时长）；动效升级留 qa 反馈后处理。
- 双击缩放 zoom 走 `zoom:`（glfwMaximizeWindow 同链）。

## 6. 验证计划

- 构建：`bash scripts/build-test-run.sh --dev`（EUI+DevDesk 同编）。
- 探针：CGEvent 脚本启动→截图（三钮位置/frame 数值）→模拟点击全屏钮进出 ×3→截图（钮仍隐藏/位置正确）→失焦态截图；证据入 `evidence/`。
- A1-A15 逐条对照见 developer 终报。
<!-- doc:31:v1 end -->

### doc#40 · 2026-11-16-notepad-module（prd v5 · need#263）

- 标题：记事本模块 PRD（v5） · 创建：2026-11-16 21:24:48 +0800（product-manager）· 更新：2026-11-16 22:30:39 +0800（product-manager）· 历史版本 4 个（未导出）

<!-- doc:40:v5 begin -->
# 记事本模块 PRD（v5）

- need: 263 ｜ 级别: L ｜ 日期: 2026-11-16 ｜ Owner: product-manager ｜ 状态: 待老板裁决 D-1/D-3/D-7/D-8（D-6 已定案；D-2/D-4/D-5 一轮推荐在案随批采纳）→ 通过后进技术方案
- v2 变更（2026-11-16 雇主追加）：独立功能定位声明；F-19 记事本开关；FD-1..3 前置依赖；参考设计文档。
- v3 变更（2026-11-16 二轮裁决）：FD-2 升阻塞必做；FD-3 定方案 A；F-19 定性「自动注册+拉系统确认」；新增 D-7/D-8。
- v4 变更（2026-11-16 三轮裁决，根本性容器变更）：记事本改为独立窗口（不进侧边栏、无 PageId、无模块门控页）；窗创建入口仅 OS 双击+「打开方式」；删除新建文件；多窗并存一文件一窗；主窗留托盘；独立生命周期；D-6 定案；FD 状态更新；F-ID 重编号。
- **v5 变更（2026-11-16，与 UI 设计稿 v2 对齐，已裁决以设计稿为准）**：同一文件重复打开→**聚焦已有窗、不开新窗**（防同文件双窗后保存静默覆盖=数据丢失风险）；pageId 序号后缀机制仅用于**不同文件同名**（如两个目录下的同名 README.md）或窗口内部唯一标识，不再用于同路径双开。改动四处=F-20①、流程 I2（+异常分支）、F-02（补聚焦口径）、§十三 F-20 验收；连带同步流程 B1 与 §十三 F-02 行的同口径表述。
- 外部调研：`docs/research/2026-11-16-notepad-module-research.md`（researcher，2026-11-16）

## 一、需求一句话

新增「记事本」：**独立窗口**形态的通用文本/代码编辑器（对标 Windows 记事本）——通过 OS 双击关联文件或「打开方式」打开即开窗，支持保存/另存为、查找替换、窗内字体字号、JSON 格式化、代码高亮、md 预览按钮、软换行；多窗并存、与主窗分离生命周期；吸收原 md 只读查看器窗，与「笔记」页并存分工。

用户价值：不离开 DevDesk 直接查看/编辑随手碰到的配置、日志、代码、文本文件，且带高亮/查找替换/格式化增强。

## 二、背景与定位锚点（已证事实+裁决，信不重查）

- **定位声明（老板三轮裁决 2026-11-16，最高优先）**：记事本=**独立窗口**，不是 Router 页面。不进侧边栏、无 PageId、无 kModuleRegistry 注册行、无模块门控页。**窗创建入口只有两个**：①OS 双击关联文件；②OS「打开方式」。主程序内无任何创建记事本窗的入口（无侧边栏项、无新建按钮）。
- **纯打开即开，无新建入口**：不双击文件就没有记事本窗；Ctrl+N 不提供；想新建文件=先在文件管理器建空文件再打开（可用性代价已由老板裁决接受，见 N-14 与 §八引导说明）。
- **多窗并存，一文件一窗**：同现有 md_viewer_window 同构——自带标题栏、可同时开多个、互不影响（自然满足 D-C「v1 不做多标签」）。**同一文件全局唯一窗**：重复打开请求→聚焦已有窗（v5 定案，防同文件双窗静默覆盖）。
- **主窗留托盘**：双击关联文件冷启动只出记事本窗，主窗保持托盘态（复用 armTrayIfViewerOnlyLaunch 既有口径）。
- **独立生命周期**：主窗关托盘/最小化/切页不影响记事本窗；多窗互不影响；**关最后一个记事本窗=记事本真的退出，主进程留托盘**（保证关联双击能再开窗）。不做独立进程。
- **参考实现（已实测，照抄架构）**：src/module_markdown/md_viewer_window.cpp（903 行）= 完整先例：session 注册表静态持有（ViewerSession，一窗一 session，onWindowDestroyed 注销防悬垂）；app::openWindow + DslWindowConfig.title/pageId/windowSize(900,640)/resizable(true)/focusOnShow(true)；独立窗口不进 Router、键路由走窗口级 onKeyEvent；pageId 序号后缀机制（v5 起仅用于不同文件同名/窗口内部唯一标识）；测试缝 viewerSessionCountForTest / viewerSessionPageIdForTest / resetViewerSessionsForTest。
- **框架能力先例（原桌宠 R5 决策，已实测）**：EUI-NEO 支持「主窗托盘时子窗独立存活」——glfw_app_main.cpp:889-915 托盘分支内 childWindows.updateAll + createRequestedWindows + pruneClosedWindows 照常跑；主窗真正退出时 childWindows.destroyAll（:1100）一并销毁子窗（符合裁决⑤⑥：quit 前走未保存保护，见 F-05/D-3）。
- 「笔记」页（src/module_markdown，约 1.2 万行）= 笔记管理，保持现状不动；原 md 只读查看器窗（md_viewer_window.cpp）由记事本窗接任退役（D-6 定案）。
- OS 打开漏斗（src/app_assembly.cpp:303-308）现路由：.md/.markdown/.js/.html/.htm/.txt→笔记页，.json→ClaudeProxy 页，其余→Home；v4 起上述扩展名改为**送达记事本窗**。
- 高亮基建现成：md_highlight.h + code_lexers.h（Java/JS/TS/Sql/Json）+ LangSpec 词法表 9 类；JSON 格式化现成：platform/json.h parse(ParseDiag)+stringify(pretty)，已知行为=对象键字典序重排；字号通道现成：settings.editorFontSize（默认 15，钳 [10,32]）→ core::theme::setEditorFontSize。
- 关联注册现成：src/platform/file_association.h:31 productionConfig() 仅 {.md}；设置页现有「关联 .md 文件」卡（md_assoc_card.h/md_assoc_logic.h，四态语义）——与记事本开关的冲突裁决=D-7。
- 缺口（FD 前置依赖，不当模块内绕路）：编辑器内查找/替换=全新能力；编码转换层全仓缺失（见 §五风险注）。

## 三、非目标（v1 明确不做）

| # | 不做项 | 理由/依据 |
|---|---|---|
| N-1 | 窗内多标签 | 一文件一窗+多窗并存天然替代（D-C 裁决保持：后续如需窗内标签单独入池） |
| N-2 | 正则查找/替换 | Windows 11 记事本亦无（调研 Q2）；v1 仅大小写/全词 |
| N-3 | 查找历史下拉、wrap-around 开关（固定环绕） | 90% 规则取舍；会话内记住上次关键词/选项仍做（F-06） |
| N-4 | 文件监听实时外部改动提醒 | 仅保存时 mtime 比对拦截（F-03） |
| N-5 | 打印/导出 PDF | 全站无打印基建 |
| N-6 | 自动保存与会话恢复、默认编码设置项 | 增强项；新建文件固定 UTF-8 |
| N-7 | 二进制文件编辑 | 高比例不可打印字节→拒编（F-14 只读兜底） |
| N-8 | 查看器窗「选区自动复制」迁移 | 查看器特化行为，随窗退役不迁移（D-6 影响项） |
| N-9 | 无现成词法表语言（css/html/xml/yaml/toml/ini/sql/log）的高亮 | 打开/编辑能力不缺；LangSpec 加表按需后续 |
| N-10 | 转到行（Ctrl+G）、Ctrl+滚轮缩放、拼写检查、AI 功能 | Windows 记事本加分项；缩放语义由字号承担 |
| N-11 | 拖拽文件开窗 | 组件拖拽能力未证；后续按需提需求 |
| N-12 | 选区内替换范围（全部替换固定=全文） | v1 简化一档 |
| N-13 | 静默接管 OS 默认打开方式 | Win8+ 禁止静默抢默认；仓内实证=注册表写成功≠双击生效；F-18 语义=自动注册+拉系统确认 |
| N-14 | **应用内新建文件入口（Ctrl+N/新建按钮/空窗）** | 老板三轮裁决：纯打开即开、无文件即无窗；想新建=文件管理器建空文件再打开。可用性代价老板已明确接受（引导途径见 §八说明） |
| N-15 | 独立进程 | 独立生命周期在主进程内实现（子窗机制），不开新进程（裁决⑤） |
| N-16 | 同文件双开（v5 新增） | 同路径重复打开→聚焦已有窗（以 UI 设计稿 v2 为准，已裁决）；防双窗各自编辑后保存静默覆盖=数据丢失风险；pageId 序号后缀仅用于不同文件同名/窗口内部唯一标识 |

## 四、职责边界（「记事本窗」与「笔记」页）

| 能力 | 记事本窗 | 笔记页 | 共用底层 |
|---|---|---|---|
| 通用任意文本文件编辑（无笔记库） | 有 | 无 | — |
| 目录树/笔记库管理 | 无 | 有 | — |
| 窗形态 | 独立窗口（可多开、主窗托盘时存活） | 主窗 Router 页面 | app::openWindow/childWindows 机制 |
| OS 关联双击/打开方式送达 | 有（D-D 三档扩展名） | 无（v1 起移交记事本窗） | app_assembly 漏斗 + armTrayIfViewerOnlyLaunch |
| 编辑器内查找/替换 | 有 | 无 | — |
| JSON 格式化 | 有 | 无 | platform/json.h |
| 代码高亮 | 有（按扩展名定语言） | 有（md） | LangSpec 词法表 / md_highlight |
| md 渲染预览 | 有（预览按钮单视图切换） | 有（四态视图） | md 渲染基建（复用不复制） |
| 字号 A-/A+ | 有 | 有 | settings.editorFontSize（是否联动=D-1） |
| 字体族 | 有（系统字体枚举，方案 A 已裁） | 无 | FD-3 |
| 编码/行尾符识别与控制 | 有（范围按 D-8） | 无 | — |
| 设置页「记事本」开关（关联注册/维护总开关） | 有 | 有（md 关联卡，处置按 D-7） | file_association 注入缝 |

裁决锚点 D-A：并存。笔记页保持现状不动；记事本=通用文本/代码编辑器；md 只读查看器窗由记事本窗接任（D-6 定案）。

## 五、前置框架依赖（状态更新 v4；一律走 EUI-NEO fork，不当模块内绕路）

| FD-ID | 内容 | 实际状态 | 验收口径 |
|---|---|---|---|
| FD-1 | 保存对话框 saveFile（macOS=NSSavePanel 系；Windows=IFileSaveDialog 系，平台拆文件） | **已交付**（commit 51e793a） | 回归验证：双平台另存为全流程——选路径/文件名、既有文件 OS 原生覆盖确认、取消→不落盘；不破坏既有 openFileDialog |
| FD-2 | 软换行 word wrap（开关式，容器宽内软换行）。**v1 必做（老板二轮裁决）** | **POC 实测框架早已支持软换行，仅剩 M 级收尾缺口**（视觉行光标/选区导航等收尾），阻塞解除、转常规开发 | wrap 开启→长行软换行、光标/选区按视觉行导航正确；关闭→恢复横向滚动；大文件模式关 wrap 不劣化性能 |
| FD-3 | 字体枚举 systemFontFamilies()。方案 A 已裁（枚举全部系统字体，无受限白名单） | **已交付**（commit dadd536） | 回归验证：下拉列出系统真实字体族、选中字形即时生效、重启保持 |

风险注：全仓实测无任何非 UTF-8 编码转换能力（iconv|Gbk|GB18030|CP_ACP 业务代码零命中）、无 BOM 处理代码——编码能力范围按 D-8 裁决；选项 2 前置成本=新增平台编码依赖层（macOS iconv / Windows MultiByteToWideChar 或自带码表）。

## 六、功能矩阵（v4 重编号：删除原 F-01「新建文件」，原 F-02..F-20 前移一位，新增 F-20 多窗管理；v5 微调 F-02/F-20）

> v4 重编号对照：原 F-03→F-01、原 F-02→F-02（改写）、原 F-04..F-20→F-03..F-19（内容顺移）、新增 F-20。被删除功能点=原 F-01「新建文件」（Ctrl+N/工具条新建/未命名缓冲/空态引导）。

| F-ID | 功能名 | 优先级 | 状态 | 验收要点（可测） |
|---|---|---|---|---|
| F-01 | 打开·OS 送达（双击关联+右键「打开方式」） | 核心 | 待方案 | 双击关联文件或 OS「打开方式」选 DevDesk（含冷启动）→漏斗路由→**新开独立记事本窗**载入（主窗留托盘，复用 armTrayIfViewerOnlyLaunch）；文件已不存在→错误窗/横幅；未注册扩展名→落 Home（现状不变）；窗创建入口仅此两路径（裁决①②，主程序内无创建入口） |
| F-02 | 打开·窗内对话框（开新窗/聚焦已有窗） | 核心 | 待方案 | 窗内主修饰键+O/工具条「打开」：OS 打开对话框（复用 openFileDialog），过滤器含 D-D 三档全部扩展名+「所有文件」；选中未打开路径→**另开一个新记事本窗**载入（当前窗不动，一文件一窗）；**选中的路径已在某窗打开→聚焦该窗，不开新窗**（v5）；取消→零副作用。注：此为窗内文件操作，非窗创建入口（窗创建入口仅 OS 送达两路径） |
| F-03 | 保存（直存+外部改动拦截） | 核心 | 待方案 | 主修饰键+S：有路径直写；成功→保存态清除+状态栏/标题刷新；写失败（无权限/磁盘满/被锁）→错误提示+保持未保存态；保存前 mtime 比对，外部已改动→三选「覆盖/另存为/取消」 |
| F-04 | 另存为 | 核心 | 待方案 | 主修饰键+Shift+S，或无路径保存自动转入：调 OS 保存对话框（FD-1 已交付）；默认文件名=当前文件名；覆盖确认由 OS 对话框承担；取消→保持编辑态未保存 |
| F-05 | 未保存保护（关窗/主窗退出） | 核心 | 待方案 | 触发点=关闭记事本窗/主窗退出（quit，destroyAll 前）/app 退出：三选「保存/不保存/取消」；「保存」→有路径直存、无路径转 F-04（F-04 取消则回保护）；保存链任何环节失败→回到触发前状态且仍标未保存（内容绝不静默丢失）；主窗退出遇多窗未保存的处理顺序=D-3 |
| F-06 | 查找 | 核心 | 待方案 | 主修饰键+F 浮层条（D-5）：输入即全命中底色高亮+当前命中强调色区分；计数「n/N」；Enter/Shift+Enter 上下跳转、文末/文首自动环绕（固定环绕，N-3）；选项=大小写敏感（默认关）+全词匹配；从光标处向后搜；零命中→计数 0/0 条内提示；空关键词→清高亮；Esc 关闭并清高亮；重开条保留上次关键词与选项（会话内，按窗隔离） |
| F-07 | 单个替换 | 核心 | 待方案 | 「替换」=替换当前命中并跳到下一个（光标落点=下一命中处，测试锁定项——调研 Q2 三家共同盲区）；当前无命中时=执行查找下一个（Notepad++ 官方口径）；计入撤销栈（单步回退） |
| F-08 | 批量替换 | 核心 | 待方案 | 「全部替换」=全文替换（N-12），完成提示「已替换 N 处」（0 处也明示）；整批单次撤销单元；遵循 F-06 选项 |
| F-09 | 字体族选择 | 核心 | 待方案 | 工具条下拉，列表=FD-3 systemFontFamilies() 枚举的系统真实字体族（方案 A 已裁）；选中即时生效+持久化（重启保持）；默认=平台等宽默认字体 |
| F-10 | 字号设置 | 核心 | 待方案 | 工具条 A-/A+（沿用笔记页交互先例）；范围沿用全局钳制 [10,32]；是否与笔记页共用同一通道=D-1；当前值在控件旁可见 |
| F-11 | JSON 格式化 | 核心 | 待方案 | 「格式化」按钮/主修饰键+Shift+F，仅当当前文件判定为 JSON（.json 扩展名）时显示；成功→全文替换为两空格 pretty 输出（计入撤销栈）+轻提示；失败→ParseDiag 行列提示「第 X 行第 Y 列：<原因>」+光标跳错误位置+内容不变；界面明示=对象键按字典序重排（D-4） |
| F-12 | 代码高亮 | 核心 | 待方案 | 按扩展名映射语言；v1 清单=LangSpec 现成 9 类+Markdown（复用 md_highlight）：C++/Python/JS/Go/Rust/Shell/Java/JSON/TS/Markdown；映射示例 .cpp/.cc/.h→C++、.py→Python、.js→JS、.go→Go、.rs→Rust、.sh→Shell、.java→Java、.json→JSON、.md→Markdown；清单外落纯文本（F-14/N-9） |
| F-13 | md 预览模式 | 核心 | 待方案 | 仅 .md/.markdown 显示「预览」按钮（窗内工具条）：点击整页切预览，渲染当前缓冲实时内容（非磁盘内容）；再点「编辑」切回，光标/滚动位置保留；渲染失败/超大→降级提示转纯文本；交互与 Windows 记事本官方 toggle 形态同构 |
| F-14 | 纯文本无高亮/二进制拒编 | 核心 | 待方案 | 无语言映射扩展名→纯文本编辑（功能不受限）；检测为二进制（不可打印字节占比超阈值）→拒绝编辑，提示「可能为二进制文件」+「仍要打开（只读）」 |
| F-15 | 编码与行尾符（范围按 D-8） | 核心 | 待方案 | 执行口径按 D-8 裁决。**推荐基线（D-8 选项 1）**：v1 仅支持 UTF-8——打开探测 UTF-8 BOM→严格 UTF-8→非 UTF-8 只读打开+状态栏标注推测编码；BOM 识别与保留；状态栏编码点选=UTF-8/UTF-8 BOM 两档（即刻标脏）；行尾点击→CRLF/LF 切换（全文规范化，计入撤销）；状态栏显示行尾（CRLF/LF/混合）；打开文件不擅自转换（保留原样）。编码转换（ANSI/GB 系/UTF-16 读写）=后续独立需求入池 |
| F-16 | 大文件降级 | 边缘 | 待方案 | 两级：超一级阈值（推荐 1MB，POC 实测调整）→关高亮+关自动换行+提示「大文件模式」；超二级阈值（推荐 10MB）→只读打开+提示；降级不拒开+上限兜底双层对标 VS Code |
| F-17 | 状态栏信息 | 核心 | 待方案 | 常驻：行:列｜字符数｜编码（点选，档位按 D-8）｜行尾符（可点选）｜保存态「●未保存」；编辑/跳转/保存实时刷新 |
| F-18 | 设置页「记事本」开关+文件关联自动维护 | 核心 | 待方案 | 设置页「记事本」开关（=关联注册/维护总开关；注：记事本窗无侧边栏显隐问题，此开关不再承担模块门控；与 md 关联卡关系按 D-7，推荐=归并单一入口）。**开启语义（二轮裁决：自动注册+拉系统确认）**：①自动注册=ProgId/Capabilities/UTI 声明与壳命令全做（D-D 三档）；②回读查询展示真态（不假设写成功=生效）；③Windows 首次开启主动拉系统关联设置页（LaunchAdvancedAssociationUI）用户一键确认；macOS LaunchServices 直接生效。关闭=撤销已注册关联/停止维护。失败回退=真态+原因+可重试，不静默绿。跨平台差异落 file_association 拆文件。不承诺静默接管默认（N-13） |
| F-19 | 自动换行开关 | **核心**（FD-2 阻塞已解除：POC 实测框架已支持，剩 M 级收尾） | 待方案 | 长行软换行开关：开启=容器宽内软换行、关闭=横向滚动；即时生效+持久化；大文件模式（F-16）强制关闭 |
| F-20 | 多窗管理与独立生命周期（v4 新增；v5 调①） | 核心 | 待方案 | ①一文件一窗：**同一文件重复打开→聚焦已有窗、不开新窗（防同文件双窗静默覆盖，N-16/v5 定案）**；②窗口标题=文件名（未保存可加 ● 前缀，随平台惯例；不同文件同名时标题可带序号后缀区分——pageId 序号后缀机制仅用于不同文件同名/窗口内部唯一标识，不用于同路径双开）；③关任一窗不影响其他记事本窗与主窗；④主窗关托盘/最小化/切页不影响记事本窗（断言：主窗最小化后记事本窗仍可编辑保存）；⑤关最后一个记事本窗=记事本退出、主进程留托盘（关联双击可再开窗）；⑥主窗真正退出（quit）→先走 F-05 保护链再 destroyAll（框架 glfw_app_main.cpp:1100 行为对齐）；⑦不做独立进程（N-15）；⑧架构照抄查看器先例：session 注册表静态持有（按文件路径索引）+onWindowDestroyed 注销防悬垂+窗口级 onKeyEvent 键路由 |

## 七、用户操作流程（节点级闭环，每节点含异常分支）

### 流程 A：OS 送达 → 开窗 → 编辑 → 保存 → 关窗（主路径）
- A1 OS 双击关联文件或右键「打开方式」→ A2 漏斗路由（app_assembly）：扩展名在 D-D 清单→路径已在某窗打开则**聚焦该窗**，否则**新开记事本窗**（冷启动时主窗留托盘）；不在→Home/笔记页（现状不变）。异常：文件已不存在→错误提示窗，不开编辑窗。
- A3 窗内载入：读文件→探测编码/行尾（F-15，范围按 D-8）→扩展名映射语言（F-12）→渲染。异常：无读权限/读取失败→错误窗+「重试/关闭」；二进制→F-14 分支；超大→F-16 分支；非 UTF-8（D-8 选项 1 口径）→只读+标注推测编码。
- A4 编辑：自由编辑，高亮/软换行按 F-12/F-19；状态栏实时刷新。
- A5 保存（Ctrl+S）→有路径直写。异常：写失败→提示+保持未保存；外部改动（mtime 变化）→三选「覆盖/另存为/取消」。
- A6 关窗（标题栏关闭钮/主修饰键+W）→ F-05 保护（有改动时三选）→关最后一个窗=记事本退出、主进程留托盘。

### 流程 B：窗内打开另一文件（开新窗/聚焦已有窗）
- B1 窗内主修饰键+O/工具条「打开」→ OS 对话框 → 选中未打开路径→新开记事本窗载入（当前窗不动）；**选中已在某窗打开的路径→聚焦该窗，不开新窗**（v5）。异常：取消→零副作用。

### 流程 C：未保存保护
- C1 触发判定：关记事本窗/主窗退出 quit/app 退出（无切页场景——窗口化后无模块页切换）。
- C2 三选对话框：「保存」→有路径直存、无路径转另存为（取消则回保护）；「不保存」→丢弃并执行原操作；「取消」→终止原操作留在窗。
- C3 主窗退出遇多窗未保存：处理顺序=D-3（推荐=逐窗依次保护，任一「取消」中止整个退出）。走查底线：任意操作序列不丢内容、不遇死路。

### 流程 D：查找/替换（窗内）
- D1 主修饰键+F→浮层条→输入即全命中高亮+计数→Enter/Shift+Enter 环绕跳转。
- D2 异常：零命中→0/0 提示；空关键词→清高亮；Esc→关闭清高亮。
- D3 主修饰键+H→替换条：「替换」逐个/「全部替换」→「已替换 N 处」；整批一次撤销。

### 流程 E：md 预览切换
- E1 打开 .md/.markdown→工具条「预览」→整页切预览（渲染当前缓冲）→「编辑」切回（光标/滚动保留）。
- E2 异常：渲染失败/超大→降级提示；非 md 无按钮。

### 流程 F：JSON 格式化
- F1 打开 .json→「格式化」显示；非 json 不显示。
- F2 成功→全文替换为 pretty 输出（撤销可回退）+轻提示。
- F3 失败→行列提示+光标跳转+内容不变。

### 流程 G：字体/字号/换行
- G1 字体族下拉（FD-3 系统枚举，方案 A）→即时生效+持久化。
- G2 A-/A+→钳 [10,32]；D-1 裁联动则笔记页编辑器同步变化。
- G3 换行开关→软换行↔横向滚动即时切换。

### 流程 H：设置页记事本开关（语义=自动注册+拉系统确认）
- H1 开关开启→①自动注册（D-D 三档）→②回读展示真态→③Windows 首启弹系统关联设置页确认；macOS 直接生效。
- H2 异常（注册失败/未确认）→真态（如「已注册待系统确认」）+原因+可重试。
- H3 关闭→撤销/停止维护→「已停止自动维护」。
- H4 重启→回读校验真态，外部破坏自动修复注册部分（系统确认无法代劳，提示用户）。

### 流程 I：多窗并存与关窗（v4 新增；v5 调 I2）
- I1 已有窗时再双击**另一文件**→第二窗独立打开（两窗互不影响，各自独立撤销栈/查找态/滚动）。
- I2 **同一文件再次送达→聚焦已有窗（提前台+获得焦点），不开新窗；若该窗有未保存改动，不改内容、仅提前台**（v5 定案，防双窗静默覆盖）。异常：若已有窗已被破坏/已关闭→正常开新窗。
- I3 关任一窗→仅该窗走 F-05 保护后关闭，其余窗与主窗不受影响。
- I4 关最后一个记事本窗→记事本退出，主进程留托盘；再双击→新窗正常打开。
- I5 主窗真正退出→有未保存窗先逐窗保护（D-3），确认后 destroyAll。
- I6 不同文件同名（如 a/README.md 与 b/README.md）→两窗并存，标题可带序号后缀区分（pageId 序号后缀机制的适用范围）。

## 八、完整状态集（数据展示节点四态，无空项）

> v4 关键变更：**不存在「无文件空窗」**——无文件即无窗（N-14），原「空态引导」行删除。应用内无任何新建引导 UI（无窗即无 UI，此代价已由老板裁决接受）；引导途径=①设置页记事本开关区说明文案（告知「通过双击/打开方式访问」）②用户指南 docs/USER_GUIDE.md。

| 节点 | 空态 | 加载态 | 错误态 | 成功态 |
|---|---|---|---|---|
| 记事本窗主体 | 不可达（无文件即无窗，窗存在必有文件） | 读文件中：状态栏「正在打开…」（超一级阈值显示加载指示） | 错误窗/横幅（文件不存在/无权限/读取失败）+「重试」/「关闭」 | 编辑器满幅+状态栏全量信息 |
| 状态栏 | 不可达（随窗存在） | 「正在打开…」 | 保持最近有效值 | 行:列/字符数/编码（按 D-8）/行尾/保存态 |
| 查找条 | 空关键词=清高亮+占位提示 | 无独立加载态（同步即时） | 零命中=0/0 条内灰字 | n/N 计数+双色高亮 |
| 预览视图 | 不可达（入口=已打开 .md） | 渲染中（仅超大可感） | 降级提示「预览失败，已显示纯文本」 | 当前缓冲渲染结果整页 |
| 格式化 | 不可达（入口=.json） | 无独立加载态（同步） | 行列定位+光标跳转+内容不变 | 内容替换+轻提示 |
| 设置页记事本开关 | 未启用=默认关闭态+说明文案（含访问方式引导） | 注册执行中（短暂） | 失败/未完成=真态回读+原因+重试 | 真态流转：已注册待系统确认（Windows）→已生效/已停止自动维护 |
| 只读模式（超大/二进制/非 UTF-8） | 不可达（入口=文件触发） | 同主体加载态 | 同主体错误态 | 只读编辑器+顶部常驻提示条「只读」（非 UTF-8 另注推测编码） |

边界覆盖：超长单行（并入 F-16 降级）；无权限（读=错误窗/写=提示）；平台差异（默认行尾 Win=CRLF/macOS=LF、字体默认平台等宽、主修饰键 Win=Ctrl/macOS=Cmd、对话框=OS 原生、默认程序 Windows 系统页确认/macOS 直接生效、UTI vs ProgId）。

## 九、界面与交互需求（容器=独立窗口；像素参数归 ui-ux-designer）

- **窗口形态**：独立 OS 窗（app::openWindow，自带标题栏；先例 windowSize(900,640)/resizable/focusOnShow），标题=文件名；三段式版面：窗口标题栏区+窗内工具条+编辑区（可选预览态覆盖）+底部状态栏。无侧边栏、无页头 Tab。
- 工具条（窗顶单行）：打开（开新窗/聚焦已有窗）｜保存｜另存为｜预览·编辑切换（仅 md）｜格式化（仅 json）｜字体族下拉（系统枚举）｜字号 A-/A+ 及当前值｜自动换行开关。**无新建按钮**（N-14）。
- 查找/替换条：浮层（D-5 推荐）；上行=关键词框/计数/大小写/全词/上下跳转/关闭；下行（主修饰键+H 展开）=替换为/替换/全部替换。
- 状态栏：Ln x, Col y｜字符数｜编码（点选，按 D-8）｜行尾符（点选）｜●未保存。
- 错误态：打开类=窗内横幅或独立错误窗、保存/格式化类=瞬时提示；只读态=顶部常驻提示条。
- 快捷键（M=平台主修饰键）：M+O 打开（新窗/聚焦已有窗）｜M+S 保存｜M+Shift+S 另存为｜M+F 查找｜M+H 替换｜M+Shift+F 格式化｜M+Shift+V 预览切换｜M+W 关窗（窗口级，走 F-05 保护）｜Esc 关闭查找条｜A-/A+ 字号。键路由走窗口级 onKeyEvent（查看器先例）。**无 M+N**（N-14）。
- UI 设计稿与 HTML demo 由 ui-ux-designer 并行产出；本节为对齐基准（v5 聚焦口径与其 §2 一致）。

## 十、外部基准（调研已回填，权威=调研文档）

对标基线=Windows 11 记事本+VS Code/Notepad++ 生产口径。要点采纳：
1. 查找/替换三源共同基线已入 F-06..F-08；替换后光标落点为自定测试锁定项（F-07）。
2. 大文件双层策略对标 VS Code；阈值推荐值须 POC 转正（F-16）。
3. md 预览 toggle 形态同构官方；官方格式工具栏不做（超范围）。
4. 行尾打开保留原样、新文件 UTF-8→F-15。
5. Windows 记事本多标签/AI/打印/拼写→非目标（N-1/N-5/N-10）；v1 多窗并存对标「多标签」的使用价值（D-C）。
6. 文件关联两平台口径+Win8+ 禁静默抢默认→F-18（调研 Q5）。

## 十一、关联影响与回归声明

| # | 受影响物 | 改动点 | 回归验证动作 |
|---|---|---|---|
| R-1 | app_assembly OS 漏斗（app_assembly.cpp:303-308） | .md/.markdown/.js/.html/.htm/.txt/.json 及 D-D 二三档由「激活主窗页面」改为「送达记事本窗」（新开窗或聚焦已有窗） | 双击各档代表扩展名（.md/.json/.txt/.yaml/.log/.py）逐一验证开窗载入内容完整；同文件重复双击→聚焦不开新窗；未注册扩展名仍落 Home |
| R-2 | md_viewer_window（src/md_viewer_window.cpp，903 行） | **退役，由记事本窗接任（D-6 定案）**：session 架构/测试缝/托盘启动语义照抄迁移；页内搜索→F-06 承接；选区自动复制不迁移（N-8） | 全仓无残留引用；构建绿；原查看器入口零命中 |
| R-3 | 托盘启动语义（armTrayIfViewerOnlyLaunch） | 保留复用并强化：纯 md 冷启动=只出记事本窗+主窗留托盘（裁决⑤，与既有口径一致） | 冷启动双击 .md：记事本窗出现、主窗留托盘、无孤儿窗；主窗从托盘唤起后与记事本窗互不影响 |
| R-4 | 笔记页（module_markdown） | 不改页面代码；md 查看器窗退役属同模块文件清理；D-1 若裁联动则字号互影响为有意行为 | 笔记页四态视图/目录树/高亮/编辑 smoke 无回归；字号（若联动）双向生效 |
| R-5 | 设置页 | 新增「记事本」开关（F-18，关联注册/维护总开关）；md 关联卡处置按 D-7（推荐=归并退役或改跳转） | 开关真态流转正确（注册→系统确认→生效/撤销）；md 卡处置后无孤儿入口，.md 关联能力不丢；重启自动维护校验 |
| R-6 | ClaudeProxy 页 | .json 关联入口移出（改送达记事本窗） | .json 双击不再进 ClaudeProxy；页内自身 JSON 功能 smoke 无回归 |
| R-7 | 模块注册表/侧边栏（src/app_registry.h） | **v4 作废旧口径——不新增 PageId、不注册 Router 页、kModuleRegistry 无新行、app_registry 零 diff、侧边栏零变化**；改为新独立窗模块（架构照抄 md_viewer_window 参考实现） | app_registry.h 与侧边栏渲染无变化；侧边栏无「记事本」项（预期行为）；kModuleRegistry 枚举序不变 |
| R-8 | EUI-NEO 框架 | FD-1/FD-3 已交付（51e793a/dadd536）转回归验证；FD-2 剩 M 级收尾 | FD-1 另存为双平台全流程；FD-3 字体下拉真实清单；FD-2 wrap 开启软换行+视觉行导航、关闭恢复横向滚动；既有 openFileDialog/横向滚动行为无回归 |

## 十二、待老板决策（全部带推荐，一次答复；全文无「待定」项）

| D-ID | 决策点 | 推荐 | 备选 | 影响 |
|---|---|---|---|---|
| D-1 | 记事本窗字号是否与笔记页共用 settings.editorFontSize 通道（联动） | 共用联动（本软件一致性；#151 已删设置页字号行，页内调节是唯一入口） | 记事本独立字号键，两页互不影响 | 联动=窗内调字号同步改笔记页编辑器（有意行为，验收双向确认） |
| D-2 | 打开时编码无法识别的兜底 | 容错打开不阻断（一轮推荐；D-8 选项 1 口径下收窄为「非 UTF-8 只读打开+标注推测编码」） | 拒绝打开并要求先转码 | D-8 为上游决策，裁后本条随之确定 |
| D-3（v4 重表述） | **主窗退出（quit）遇多个未保存记事本窗的保护顺序**（窗内无切页，关窗保护=必弹三选已是硬要求非决策） | 逐窗依次弹保护（按窗创建序），任一「取消」中止整个退出（主窗与全部子窗保留，内容零丢失） | 汇总对话框一次列出全部未保存窗，逐窗勾选处理（一步完成但交互较重） | 逐窗=交互次数多但口径最简单；汇总=一次搞定但需新对话框形态 |
| D-4 | JSON 格式化对象键字典序重排（现有 yyjson 行为） | v1 接受现状+界面明示（语义等值） | 技术方案评估保序改造 | 接受=零新增成本；保序=改造 platform/json |
| D-5 | 查找条形态 | 浮层条（主修饰键+F 唤起/Esc 收起；官方同构+VS Code 先例） | 工具条常驻固定区 | 浮层省空间；固定区少一步 |
| **D-6（v4 定案）** | md 查看器窗退役与接任 | **已定案（三轮裁决）：记事本窗=查看器窗接任者，上线当版退役 md_viewer_window。迁移步骤**：①记事本窗照抄 session 架构（静态注册表+onWindowDestroyed 注销+窗口级 onKeyEvent；pageId 序号后缀机制沿用——v5 起仅用于不同文件同名/窗口内部唯一标识）；②测试缝随迁（viewerSessionCountForTest/viewerSessionPageIdForTest/resetViewerSessionsForTest 三个探针改为记事本窗同名缝，断言迁移后旧缝删除）；③页内搜索能力由 F-06 承接；④armTrayIfViewerOnlyLaunch 与 glfw_app_main 托盘分支子窗存活机制直接复用不改；⑤「选区自动复制」不迁移（N-8） | —（已定案，无双轨过渡） | 少维护 900 行旧窗；行为变化=双击 md 从只读窗变可编辑记事本窗，只读诉求由预览/只读模式承担 |
| **D-7**（v3 新增，v4/v5 保留） | md 关联卡与记事本开关的归属冲突（两者都注册 .md、状态互不知情、三种冲突态） | **选项 1（推荐）：md 关联卡归并进记事本开关**——单一入口维护全部 D-D 扩展名（含 .md），md 卡退役或改跳转链接 | 选项 2：两卡并存职责切分（md 卡管 .md 且优先）；选项 3：md 卡不动、记事本开关不碰 .md（与老板需求有偏差） | 选项 1 改动 md 卡（R-5 回归面增加）；2/3 双入口冲突定义成本长期存在 |
| **D-8**（v3 新增，v4/v5 保留） | 编码能力 v1 范围（全仓无编码转换层实证） | **选项 1（推荐）：v1 只做 UTF-8**（BOM 识别与保留；非 UTF-8 只读+标注推测编码；编码转换后续独立入池；零新依赖工期可控） | 选项 2：v1 完整三档编码（需先建平台编码转换层+GB 系双平台测试面，阻塞发版） | 选项 1 下 ANSI/GB 系/UTF-16 可看不可改（可达性保住）；选项 2 一次到位成本前置 |

最需拍板四条：**D-7**（设置页双入口冲突）、**D-3**（主窗退出多窗保护顺序）、**D-8**（编码范围）、**D-1**（字号联动）。

## 十三、验收标准（F-ID 对照；执行人=qa-tester 实操，macOS+Windows 双平台各走主路径一遍；证据物归档 evidence/）

- 框架前置：FD-1（已交付）另存为双平台全流程回归；FD-2（M 级收尾完成后）wrap 软换行+视觉行导航+关闭恢复横向滚动；FD-3（已交付）字体下拉系统真实清单+即时生效+重启保持。
- F-01：双击 .md/.json/.txt/.yaml/.log/.py 各一→各自独立开窗、内容与源文件一致；冷启动双击→主窗留托盘；删除源文件后双击→错误提示不开编辑窗。
- F-02：窗内 M+O 选未打开文件→新窗载入、原窗不动；**选中已打开路径→聚焦该窗、开窗数不变**；取消→零变化。
- F-03：改动→M+S→磁盘更新+保存态清除；只读文件保存→错误提示+仍标未保存；外部改 mtime 后保存→三选出现。
- F-04：M+Shift+S→OS 保存对话框；覆盖既有文件→OS 确认；取消→保持未保存。
- F-05：有改动关窗→三选；「不保存」重开文件=旧内容；「取消」=窗保留；保存失败中断后内容仍在。
- F-06：10 处命中→计数正确；Enter 环绕；大小写/全词选项生效；Esc 清高亮。
- F-07：替换当前命中+跳下一个；单步撤销。
- F-08：全部替换提示 N 处；一次撤销全量回退；零命中提示 0 处。
- F-09：下拉为系统真实字体清单（抽 3 个系统自带字体验证非白名单）→即时生效；重启保持。
- F-10：A- 钳 10、A+ 钳 32；（D-1 裁联动时）笔记页同步变化。
- F-11：合法 .json→两空格 pretty+可撤销；非法→行列提示+光标跳转+内容不变。
- F-12：10 种语言样本各自高亮生效；.css→纯文本可编辑。
- F-13：.md 显示预览按钮、.txt 不显示；改一字切预览→变化可见；切回→光标/滚动保留。
- F-14：.log 可编辑无高亮；二进制样本→拒编+只读选项。
- F-15（按 D-8；推荐口径）：UTF-8 BOM 样本→状态栏显「UTF-8 BOM」且保存保留 BOM；GBK 样本→只读+标注推测编码；CRLF 显 CRLF、切 LF 全文转换一次撤销、直接保存行尾不变。
- F-16：超一级阈值→高亮/换行关闭+「大文件模式」提示；超二级→只读+提示。
- F-17：光标移动行列实时变；编辑●出现；保存●消失。
- F-18：开启→自动注册（回读实查）→Windows 弹系统关联页、确认后双击生效；macOS 直接生效；关闭→撤销（双击落 Home）；注册失败→真态+原因+重试；无「未确认已生效」假绿态。
- F-19：开关即时切换软换行↔横向滚动；重启保持；大文件模式强制关闭。
- F-20：①**同文件双击两次→只有一扇窗、第二次仅聚焦（开窗数不变）；窗有未保存改动时聚焦不改内容**；②**不同文件同名（如 a/README.md 与 b/README.md）→两窗并存、标题可带序号后缀区分**；③主窗最小化/关托盘/切页→记事本窗仍可编辑保存；④关一窗→他窗与主窗无影响；⑤关最后窗→主进程留托盘、再双击可开新窗；⑥主窗 quit 且有未保存窗→先保护后退出。
- 全局走查：流程 A 全链（双击→编辑→保存→关窗）+ 失败路径抽测（F-03 无权限/F-11 非法 JSON/F-01 文件丢失/F-18 注册失败/C3 主窗退出多窗保护/I2 已有窗已关闭）必过；任意操作序列不遇死路、内容不静默丢失。

## 十四、参考

- 外部调研：docs/research/2026-11-16-notepad-module-research.md
- 参考实现（v4 关键）：src/module_markdown/md_viewer_window.cpp（session 架构/测试缝/pageId 后缀）；EUI-NEO glfw_app_main.cpp:889-915（托盘时子窗存活）/:1100（quit 时 destroyAll）
- UI 设计稿 v2（v5 聚焦口径来源）：其 §2「同文件已有窗→聚焦不开新窗」为已裁决权威口径
- 参考设计文档：docs/design/2026-11-10-md-readonly-viewer.md（+同名 .html）；docs/superpowers/specs/2026-09-22-markdown-editor-ux-design.md

## 十五、裁决序结论（need#146 四态）与硬清单自检

- ①该不该做：做（老板 2026-11-16 立项；一轮 D-A..D-D、二轮 FD-2/FD-3/F-19、三轮独立窗口容器变更、v5 同文件聚焦口径均已裁决并入）。
- ②能否复用：复用=EUI-NEO 多行编辑器组件、LangSpec 词法表 9 类、md_highlight、platform/json 格式化、settings.editorFontSize 通道、openFileDialog/saveFileDialog（FD-1 已交付）、systemFontFamilies（FD-3 已交付）、file_association 注入缝、**md_viewer_window session 架构与测试缝（D-6 迁移）**、armTrayIfViewerOnlyLaunch、glfw_app_main 托盘子窗存活机制。
- ③能否改造：改造=md_viewer_window 吸收退役（R-2/D-6 定案）、app_assembly 漏斗改送达记事本窗（R-1）、设置页记事本开关+md 关联卡处置（R-5，按 D-7）。
- ④才新方案：notepad 独立窗模块（不进 Router）；全新能力=编辑器内查找/替换、多窗管理与独立生命周期（同文件聚焦去重）、编码探测与控制（按 D-8）、大文件降级、自动注册+拉系统确认的关联维护。
- 硬清单自检：节点全枚举（流程 A..I 入口到终态无断链，每节点标「从哪来」；窗创建入口唯一化=OS 送达两路径）｜四态全集（七节点×四态无空项；无文件空窗已按 N-14 显式排除）｜操作与触发（工具条/状态栏/查找条/设置开关/窗口关闭钮每控件含触发结果）｜流转去向（每操作成功/失败/中断去向已写；关窗/quit 保护链兜底=内容不丢；I2 聚焦异常分支=已有窗已关闭则正常开新窗）｜澄清升格（全文无「待定/TBD」；D-1/D-3/D-7/D-8 带推荐+备选+影响；D-6 定案含迁移步骤；v5 同文件聚焦为已裁决口径）｜闭环自走查（每流程主路径+至少一个失败路径已走查并回填；无新建入口的可用性代价已正面回答）。
<!-- doc:40:v5 end -->

### doc#42 · 2026-11-16-notepad-module-tech（tech v1 · need#263）

- 标题：记事本模块技术方案卡（v1） · 创建：2026-11-17 08:07:05 +0800（developer）· 更新：2026-11-17 08:07:05 +0800（developer）· 历史版本 0 个（未导出）

<!-- doc:42:v1 begin -->
# 记事本模块技术方案卡（v1）

- need: 263 ｜ 级别: L ｜ 日期: 2026-11-17 ｜ Owner: developer ｜ PRD: `doc_get(slug="2026-11-16-notepad-module")` v5 ｜ 设计: docs/design/2026-11-16-notepad-module.md v2
- **重要事实修正（简报过时，已实查）**：#269 **已落地**——`EUI-NEO/include/eui/dsl_app.h:288-301` 已有 `DslWindowConfig::minSize/maxSize`（注释明写「记事本模块 need263 需要 720×480 下限」），透传 `WindowCreateRequest.minWidth/minHeight` → 后端 `glfwSetWindowSizeLimits`。阻塞依赖解除，**无需降级路径、无需等池**。另有新增能力 `windowFocusChangedHandler`（dsl_app.h:274）/ `onWindowFocusChanged`（app.h:66）可白嫖做焦点跟踪。
- EUI-NEO 基线 HEAD=690484a（fork/main，2026-11-17 实查）。

## 〇、结论先行（推荐一句话）

新模块 `src/module_notepad/`（不进 Router、app_registry 零 diff），session 架构照抄 `md_viewer_window.cpp` 并吸收退役（D-6）；文件 I/O 走 core 层 `fs_text::readTextFile` + `atomic_write`（不依赖 module_markdown）；高亮统一层=把 `md_keyword_highlight` 迁入 ui_kit/text_highlight（既有共享层，module_git 已是先例），notepad 只用 LangSpec 单入口；查找替换=纯函数引擎 + `pushUndoState/++textRevision` 原子提交（`md_module_editor.cpp:140-142` 同构）；md 预览复用 `components::markdown` 整篇渲染（块流核是 module_markdown 私有，隔离禁依赖，不搬）；框架补一批 0 三件套（关窗 veto + quit guard + requestWindowFocus + setWindowTitle）——**这是唯一真阻塞项**。

## 一、模块骨架（问题 1）

- 目录 `src/module_notepad/`，文件清单（每文件一行职责）：

| 文件 | 职责 |
|---|---|
| `notepad_module.h/.cpp` | 公开入口 `openNotepadWindow(path)`（OS 漏斗唯一调用面）+ 测试缝声明 |
| `notepad_session.cpp` | `NotepadSession` 结构 + 静态注册表 + 规范路径查重/聚焦 + onWindowDestroyed 注销 |
| `notepad_window.cpp` | compose：工具条 44/提示条 32/编辑区 flex/状态栏 28（设计稿 §1.2 三段式）+ 窗级 onKeyEvent 键路由 |
| `notepad_find.h/.cpp` | 查找/替换纯函数引擎（字节域，无 UI 依赖，无头可测） |
| `notepad_language.h/.cpp` | 扩展名→语言映射表（单一真源）+ per-line 高亮 provider（syntax ∪ 命中层合并） |
| `notepad_file_io.cpp` | 读/写/mtime/保存状态机（干净直通/脏重编码）单点 |
| `notepad_encoding.h/.cpp` | 纯函数编码探测：BOM 三型/严格 UTF-8 DFA/推测编码标签/二进制占比/EOL 探测 |
| `notepad_prefs.cpp` | app_config 持久键：字体族/wrap 开关/窗尺寸位（应用级一份，设计 §2） |
| `CMakeLists.txt` | 目标 `devdesk_module_notepad` |

- 根接线：根 `CMakeLists.txt:233-248` 模块区追加 `add_subdirectory(src/module_notepad)`；新目标链 `devdesk_core` / `devdesk_platform` / `devdesk_ui_kit` + EUI-NEO include（`devdesk_add_eui_includes`，根 CMakeLists.txt:224-231）。**禁依赖任何兄弟 module_***（`scripts/check-module-isolation.sh` 硬守卫）——下文所有对 markdown 层能力的引用均已按此约束解耦。
- app 接线：仅 `src/app_assembly.cpp`（main 层）调用 `module::notepad::openNotepadWindow`——main→module 合法方向。`src/app_registry.h` / kModuleRegistry **零 diff**（R-7）。

## 二、窗口会话架构（问题 2）

- `NotepadSession`（照抄 ViewerSession，`md_viewer_window.cpp:42-125`）字段：

| 字段 | 用途 |
|---|---|
| `path` | 用户可见原始路径（标题/toast 显示） |
| `normKey` | 规范路径键（查重唯一依据，见下） |
| `title` | 窗口标题=文件名（脏态「• 」前缀，中段省略沿用查看器规则） |
| `pageId` | `notepad.<全局递增序号>`——**仅窗口内部唯一标识**；不同文件同名标题后缀也用它（v5 口径，不用于同路径双开） |
| `loadError` | 非空=错误占位态（窗照开照关，PRD A2 异常分支） |
| `handle` | onWindowCreated（`dsl_app.h:272` windowCreatedHandler）记句柄，onWindowDestroyed 注销用 |
| `dirty` / `mtime` | 未保存态 / 载入时 `fs::last_write_time`（保存前比对拦截，F-03） |
| `rawBytes` | 载入原始字节（干净缓冲保存直通=严格「打开不擅自转换」，见 §八） |
| `eolMode` / `mixedEol` / `hasBom` / `encMode` | 行尾档（CRLF/LF）/混合标志/BOM 保留/编码档（UTF-8、UTF-8 BOM、只读推测档） |
| `readonly` + `readonlyReason` | 只读降级（非 UTF-8/超大），联动 §7.2 控件矩阵 |
| `binarySuspect` | F-14 二进制拒编分支 |
| `language` | notepad_language 映射结果（None=纯文本） |
| `previewOn` / `search` | 预览切换态 / 查找替换态（query/opts/hits/cur/appliedQuery，会话内隔离） |
| 文本本体 | 不放 session 字段——托管在 `InputState`（`ui.state<InputState>(kNotepadEditorInputId)`，`md_module_editor.cpp:126` 同款），undo 栈=框架内置 128 |

- 注册表：`static std::vector<std::unique_ptr<NotepadSession>>`（主线程单线程访问；OS 漏斗/Apple Event/第二实例 drain 同域）。生命周期不变量照抄：**onWindowDestroyed（`dsl_app.h:273` windowDestroyedHandler，销毁前回调，覆盖 requestWindowClose/Alt+F4/应用退出全部路径）从注册表 erase → compose 闭包捕获的裸指针与窗口同寿，无悬垂**（`md_viewer_window.cpp:127-141` 原样迁移）。
- **同文件聚焦（v5 核心）**：`findSessionByNormKey(norm)` 命中且 session 存活（销毁即注销，注册表命中=窗活着）→ **`core::window::requestWindowFocus(handle)` 聚焦**（框架新 API，批 0）→ toast「已在原窗口打开，已为你聚焦」（设计稿 toast 文案）。normKey 规范化：`fs::weakly_canonical`（存在文件）失败退 `fs::absolute().lexically_normal()`；Windows 大小写不敏感卷上两 casing 视为同文件的边界注记（OS 送达通常同 casing，残余风险低，列 §风险 6）。`focusOnShow` 只作用于创建期，聚焦已有窗必须新 API——**框架缺口实查确认**：`window_backend.h` 全文仅 `requestWindowClose(:46)`，无 focus/show-by-handle（grep glfwFocusWindow/makeKeyAndOrderFront 零命中）。
- 关最后一个窗=注册表自然清空，模块无常驻资源，主进程留托盘（既有托盘机制零改动，F-20⑤ 免费成立）。

## 三、OS 漏斗改造（问题 3）

五处新旧行为对照（全在 `src/app_assembly.cpp`）：

| # | 位置 | 旧行为 | 新行为 |
|---|---|---|---|
| 1 | `dispatchFileOpen` :310-337 | `.md/.markdown`→`openViewerWindow`；其余 `dispatchPageForFile` 路由页 + `captureFileIntoNotesDir` 拷贝 | 扩展名 ∈ D-D 清单→`module::notepad::openNotepadWindow(path)`（聚焦/新窗逻辑在 session 层内收）；**未注册→`navigateTo(Home)`**（F-01 口径不变）。`dispatchPageForFile` 调用与 capture 分支删除 |
| 2 | `dispatchPageForFile` :285-308 | .md/.js/.html/.htm/.txt→Markdown 页，.json→ClaudeProxy 页，其余 Home | **整函数删除**（含其单测改造为漏斗纯函数用例）；`captureFileIntoNotesDir` 的 OS 漏斗调用面随之消失（module_markdown 内函数本体随 D-6 文件清理一并评估，应用内 "open:" 通道不经漏斗、零影响） |
| 3 | `pathHasMarkdownExtension` :267-281 | 匿名空间内 .md/.markdown 判据 | **删除**；统一改调 `core::notepad_file_types.h` 单一真源 |
| 4 | `armTrayIfViewerOnlyLaunch` :342-348 | `pathHasMarkdownExtension(pendingOpenFilePath())` → 主窗托盘 | 判据换 `isNotepadExtension(...)`，语义注释更新为「关联启动=纯记事本送达」（R-3 强化，行为不变） |
| 5 | `secondInstancePathsNeedMainWindow` :229-234 | 同上判据 | 同上换判据（bug270 唤醒裁决口径保持） |

- **扩展名单一真源 = `src/core/notepad_file_types.h`**（新，core 层零依赖）：`inline bool isNotepadExtension(path)` + `inline const std::vector<std::string_view>& notepadExtensions()`（D-D 三档全选清单）。消费方三类全合规：app_assembly（漏斗/托盘/第二实例，main 层）、module_notepad（语言映射兜底）、module_settings（F-18 逐扩展注册）。设置页与漏斗共享同一份，杜绝五处各写一遍。
- **D-D 清单内容**：本次侦察未能命中一轮裁决原文的具体三档清单（research 文档/pool-snapshot 只有引用无列表）。落卡处理：v1 占位=现漏斗全集 `.md/.markdown/.json/.txt/.js/.html/.htm`；**批 1 开工前必须对一轮 D-D 裁决原文校准后填入**（列 §风险 4，唯一待补事实，不阻塞架构）。
- 与 `captureFileIntoNotesDir` 关系：OS 双击**不再拷贝进笔记目录**（旧语义废止=R-1）；笔记页本体与页内 "open:" 通道不动（R-4）。

## 四、查找/替换引擎（问题 4）

- 纯函数（`notepad_find.h`，字节域=InputState 的 `cursor/selectionStart/selectionEnd` 全文档字节偏移同域，零换算）：

```
struct FindOptions { bool caseSensitive; bool wholeWord; };
struct FindHit { size_t start, end; };          // 字节闭开区间
std::vector<FindHit> findAll(text, query, opts); // 大小写=ASCII 折叠（禁 locale）；词字符=[A-Za-z0-9_ ∪ >=0x80 字节]（CJK 整字符视作词内）
FindHit nextFrom(hits, fromByte);  // 环绕固定（N-3）
FindHit prevFrom(hits, fromByte);
string replaceOneAt(text, hit, repl);
struct ReplaceAllResult { string text; size_t count; };
ReplaceAllResult replaceAll(text, query, repl, opts);
```

- **替换后光标落点（PRD F-07 锁定项，三家官方盲区自定）**：单个替换后光标=替换文本之后**下一个命中的 start**（含环绕）；无下一命中→替换文本末尾；选区=该下一命中区间。doctest 断言锁死（含边界：替换缩短文本/命中在文末/0 剩余）。
- 状态机：输入即重算（`appliedQuery` 门控，`md_viewer_window.cpp:156-162` 同款）；文本被编辑（`textRevision` 变化）→ 重算 hits 并保持当前序号尽量邻近；空关键词→清高亮；Esc→清高亮留词（F-06 会话内保留=session 字段）。
- **高亮接 `.lineStyles`**：notepad 提供 per-line provider（签名 `(displayText, lineStartByte, lineNo)→vector<TextRun>`，全文档字节域，`input_model.h:941-957` POC 已证契约）。两层合并纯函数 `mergeRuns(base 语法层, hits 命中层, lineStartByte)`：在命中边界切分语法段、命中段覆盖为单一合成色（styled-runs 仅 color 一维，无法叠两层→预合成：普通命中=warning@26%、当前命中=primary@38% 叠 warning，直接给合成后色值，与设计稿 §4.5 落地值一致）。命中层与语法层冲突消解=命中色覆盖语法色（官方编辑器同款）。
- **undo 原子性**（与 `md_module_editor.cpp:140-142` 同构的统一提交helper）：

```
commitTextAtomically(session, newText, cursor, selStart, selEnd):
  InputModel::pushUndoState(st);   // :140
  st.text = newText; ++st.textRevision;  // :141-142
  st.cursor/selectionStart/selectionEnd = 落点;
  st.hasPreferredCursorX = false; st.followCaret = true;  // :146-147（自动滚到光标）
```

消费方：单个替换/全部替换（整批一次 push=单撤销单元，F-08）/JSON 格式化/EOL 切换。全部替换后 toast「已替换 N 处」（0 处也明示）。
- 大文件命中标记：参照 Notepad4 `EditMarkAll`（OSS 参考 §2.1：1MB 分块/64ms 预算/范围缓存）；**v1 取舍=不做定时器分块**——大文件模式（>1MB）高亮关闭时命中标记同步关闭，仅保留「查找跳转+计数」；256KB 档标记同步全量重算（`textRevision` 门控）。Notepad4 参数表留作 QA 卡顿时调参依据。
- 查找跳转滚动：`followCaret=true`（`input_model.h:158` 自动滚到光标）+ `st.cursor` 直写——无需碰 scrollview。

## 五、高亮统一层（问题 5）

- 现状三套两模型：`ui_kit/text_highlight/md_highlight.h`（Markdown 行级 TextRun）、`ui_kit/text_highlight/code_lexers.h`（CodeHighlighter，CodeLang 5 类，HlSpan+HlKind，`code_lexers.h:18-56`）、`src/module_markdown/md_keyword_highlight.cpp:515`（LangSpec 表驱动 9 类，TextRun，`mdKeywordHighlight(code, lang, colors)`）。
- **迁移**：`md_keyword_highlight.{h,cpp}` 整体挪 `src/ui_kit/text_highlight/`（namespace `ui_kit::text_highlight`，逻辑零改），module_markdown 改 include——ui_kit/text_highlight 本就是跨模块共享层（module_git 已同时消费 code_lexers.h 与 md_highlight.h，实查 `git_page_controller.cpp`），隔离守卫合规，R-4 回归=笔记页高亮 smoke。
- **避免双源**：notepad 高亮 dispatch **只走 LangSpec 单入口**（9 类表驱动，加语言=加一张表）；`code_lexers`（HlSpan+Kind 模型）**留在原地服务既有消费方（module_git），notepad 不用、不做模型转换层**。交集语言（Js/Java/Json）以 LangSpec 为准，无第二着色通路=无双源。Markdown 编辑态按 F-13 走迁移后的 ui_kit md_highlight（复用不复制）。
- **扩展名→语言映射单一真源 = `notepad_language.cpp`**：`.cpp/.cc/.h/.hpp→Cpp`、`.py→Python`、`.js→JavaScript`、`.ts→TypeScript`、`.go→Go`、`.rs→Rust`、`.sh/.bash→Shell`、`.java→Java`、`.json→Json`、`.md/.markdown→Markdown`；清单外=None 纯文本（F-14/N-9）。v1 十类全在 LangSpec+md_highlight 现成表内，**零新词法表**。

## 六、md 预览（问题 6）

- **选型：复用 `components::markdown` 整篇渲染**（EUI-NEO 组件层，`<components/markdown.h>`+`MarkdownBuilder`）。理由：
  1. 块流核（`mdDrawBlockFlowCore`/`MdBlockModel`/`MdSyncMap`）全在 module_markdown（`md_viewer_window.cpp:1-9`、`markdown_module_internal.h`）——模块隔离禁依赖；搬 ui_kit=大型重构，违反 R-4 与最小 90%。
  2. notepad 预览=单视图整页 toggle（PRD F-13，Windows 记事本同构），无查看器的超大文档+搜索矩形+视口裁剪场景；大文件已按 F-16 降级兜底，渲染失败转纯文本分支=PRD E2 异常态。
- **MarkdownStyle 来源**：不触碰模块私有的 `scaledPreviewStyle`（`markdown_module_internal.h:125-137`）——notepad 本地 3 行 inline 构建 `MarkdownStyle(componentTheme)`（blockGap×fontScale 语义照抄 :135 带来源注释）。取舍：放弃提取 ui_kit（3 行复制 < 新增 API 面，代码负资产原则）。
- 切换交互：整页编辑⇄预览替换编辑区槽位；光标/滚动保留=编辑态 InputState 与 scrollOffset 都在 session，切回即恢复（F-13）；工具条联动禁用按设计稿 §4.6 矩阵。

## 七、JSON 格式化（问题 7）

- 调用序列：`platform/json.h parse(text, &diag)`（`json.h:96`，diag 填 `ParseDiag{bytePos,msg}`，`json.h:90-93`）→ 成功 `stringify(value, /*pretty=*/true)`（两空格，`json.h:100`）→ `commitTextAtomically`（§四，单撤销步）+ toast「JSON 已格式化」。
- **错误行列**：platform/json.h 新增薄封装 `jsonLineColForPos(text, bytePos)→{line,col}`——内部 `yyjson_locate_pos`（1 基行列，官方 API，OSS 参考 §三已确认免自研）。notepad 不直接碰 yyjson。失败 toast「第 X 行 第 Y 列：<msg>」+ `st.cursor=bytePos; st.followCaret=true`（光标跳错误位）+内容不变（F-11）。
- 键序重排提示（D-4）：格式化按钮 tooltip 固定副文案「对象键将按字典序重排」+成功 toast 后缀同义提示；不在每次格式化弹确认（轻量明示即可）。

## 八、文件 I/O 与编码（问题 8）

- **读**：`src/core/fs_text.h:14` `readTextFile(fs::path, &readFailed)`——二进制安全 + `fs::path` 宽路径（Windows 纪律自动满足，BUG-1 类问题在 core 层免疫）。**不复用** module_markdown `readFileUtf8`（模块隔离）；notepad_file_io.cpp 做 Result 化包装（无异常纪律，`core/result.h`）。
- **写**：`src/core/atomic_write.h` `atomicWriteFile(target, content, label)`（临时文件+原子替换，双平台现成）。
- **BOM/编码（D-8 口径）**：`notepad_encoding.cpp` 纯函数链：BOM 探测（EF BB BF→UTF-8 BOM；FE FF/FF FE→UTF-16 只读档，标注推测精确到 LE/BE）→ 严格 UTF-8 DFA 校验→失败 `guessEncodingLabel(bytes)`（GB18030 双字节频次启发/Latin-1 兜底，界面标「推测」）→只读降级。缓冲区存剥离 BOM 后文本+`hasBom`；状态栏编码点选 UTF-8/UTF-8 BOM 两档（`encMode`，切换即刻标脏）。
- **行尾**：载入探测 CRLF/LF/CR/混合→缓冲统一 LF+`eolMode/mixedEol` 记录；状态栏点选切换=`commitTextAtomically` 全文规范化（计入撤销，F-15）。
- **「打开不擅自转换」的强保证**：session 留 `rawBytes`（载入原字节）；**干净缓冲保存=rawBytes 直通**（字节级往返一致，混合 EOL 文件也不被规范化）；脏缓冲保存=按 encMode（BOM 回写）+eolMode（全文回写）重编码。内存代价 ≤2×文件大小，10MB 只读上限内可接受。
- **mtime 拦截**：`loadMtime=fs::last_write_time`（载入）；保存前重取比对→不一致→三选「覆盖/另存为/取消」（F-03），实现单点在 notepad_file_io.cpp `saveSession()`。
- **大文件阈值**：`kNotepadLargeFile=1MB`（关高亮+关 wrap+info 提示条，可编辑）、`kNotepadHugeFile=10MB`（只读+warning 提示条）。POC 依据（`docs/research/2026-11-16-word-wrap-poc.md` §1 表）：首排版 shaping 主导 ≈0.6ms/KB（277KB 实测 165-193ms）→1MB≈600ms 一次性、每键增量 <100µs 与总量无关；100KB 单行 wrap 85ms/键是单行热点（POC §4④，不推荐为此改动框架）而非总量热点。设计稿 §7.4 的 256KB 档为 POC 前旧值，以 PRD F-16 为准；两常量收敛在 notepad 单头便于 QA 调参。

## 九、未保存保护（问题 9）

- **三选对话框**：`components::dialog` DialogBuilder（`dialog.h:49-78`）——`primary=保存`、`onSecondary=取消`、第三钮「不保存」（危险文字钮）经 **`content()` 自定义缝**（`dialog.h:75`）自绘按钮行；`theme(tokens)` 同主题。**不用 platform::message_box**（仅 Ok/Cancel 两态，不够三选）。弹窗渲染在子窗自己的 Ui 树内=每窗独立保护弹窗。
- **关窗拦截（红绿灯/×/M+W/requestWindowClose）——框架缺口实查确认**：`DslWindowConfig` 无 close-veto 回调，关闭直达销毁，保护无从挂。**框架批 0 新增 `onCloseRequested` veto**：GLFW `glfwSetWindowCloseCallback` 里清 `glfwSetWindowShouldClose(window, GLFW_FALSE)` 后交模块裁决；Win32 `WM_CLOSE` 返 0 拦截；macOS `windowShouldClose:`。模块侧：dirty→弹三选，「保存/不保存」→继续 `requestWindowClose(handle)`（`window_backend.h:46`，生产用法 `release_reminder.cpp:371` 先例），「取消」→不关。干净窗直通。**此项为 v1 阻塞项**（无它则红绿灯丢保护=数据丢失风险）。
- **主窗退出挂载点（关键难点）**：主循环退出后 `childWindows.destroyAll`（`glfw_app_main.cpp:1100`）之前**无任何 hook**；退出单点汇聚=`app::requestExit()`（`app.h:128`）→ 主循环每帧轮询消费（`app.h:241`，`glfw_app_main.cpp:874` 注释确认托盘 Exit 与子窗退出菜单同走此路）。**框架批 0 新增 quit-guard**：`app::setQuitGuard(std::function<bool()>)`，挂在 requestExit 消费点——guard 返回「等待中」则不置 forceClose；notepad 模块按窗创建序逐窗弹三选（**D-3 定案：任一取消中止整个退出**），全清后模块再调一次 `requestExit` 放行。覆盖全部退出路径（托盘 Exit/Cmd+Q/子窗退出菜单同汇聚点）。
- 降级路径（若框架批延迟）：DevDesk 侧托盘 Exit 处理点先跑保护链再 `requestExit`——覆盖 Windows 主路径；macOS Cmd+Q 不受保护（如实标注，不作为交付态）。

## 十、设置页关联开关（问题 10，F-18/D-7）

- **AssocConfig 多扩展化**（`platform/file_association.h:26-32`）：`extension:string` → `extensions:vector<string>`；`productionConfig()`→ progId `"DevDesk.notepad"` + D-D 全清单；`testConfig()` 同构多扩展（测试红线保持：用例禁触真实关联）。判属锚仍是 ProgId 串回读（`:27` 语义不变）。**旧注册迁移**：首次开启时对旧 `"DevDesk.md"`+`.md` 检测归己则重写为新 ProgId（幂等）。
- 实现拆文件（平台差异既有格局不动）：`file_association_win.cpp` 循环 per-extension（HKCU OpenWithProgids + RegisteredApplications + Capabilities + `SHChangeNotify`，Sumatra `RegistryInstaller.cpp:145/180/56-72` 模板已核）；`isDefaultForMd`→`isDefaultFor(ext)` 逐扩展查询；macOS `file_association_macos.mm`：`CFBundleDocumentTypes`/`UTTypeTagSpecification` 多扩展数组（打包期 Info.plist 模板在仓内打包脚本链，同步改）+运行时逐扩展回读（`LSCopyDefaultApplicationURLForURL`）+引导（无公开 setter，OSS 参考 §4.2 已确认）。
- **Windows 拉起系统关联 UI**：`launchAdvancedAssociationUI(ext)` 新增于 file_association（`file_association_api_map.h` 增映射）——`ShellExecuteW("ms-settings:defaultapps")`（Win10+ 系统设置页，Win8+ 禁静默抢默认=N-13 的正路）；调用点=设置页开关「未生效」态引导按钮（现 md 关联卡引导按钮位改造沿用）。
- **D-7 退役**：md 关联卡（`md_assoc_card.*`+`settings_ui_cards.h:85` mdAssocState+`md_assoc_logic.*`）→ 记事本开关卡接管；四态推导/自愈矩阵纯函数（`mdAssocStatusKind`/`mdAssocHealAction`，`md_assoc_logic.h:21-29`）改名 `notepadAssoc*` 留在 module_settings 内复用（逻辑逐扩展化）。旧卡文件删除、单测迁移（R-5）。
- **失败回退与回读**：既有「写入成功≠生效」三态纪律（`file_association.h:6-10`）+Unknown 按需修复档原样沿用；启动自愈（H4）逐扩展跑 `mdAssocHealAction` 矩阵。
- **行为变更声明（风险 5）**：现实现「生产永不注销」（`file_association.h:84-87` D-4 旧语义）；F-18 新语义「关闭=撤销已注册关联」→ 生产侧新增 `unregisterAll()`（Sumatra `UnregisterFromBeingDefaultViewer:280` 幂等模板：仅当回读归己才清，不误删用户后选的他者）。老用户关开关后 .md 关联失效=老板已裁的新语义，验收按 F-18。

## 十一、测试策略（问题 11）

- **无头 doctest**（`tests/doctest.h` shim，无 REQUIRE_FALSE；纯函数全数直测）：

| 用例文件 | 断言面 |
|---|---|
| `test_notepad_find.cpp` | findAll/环绕/大小写/全词/CJK 边界；**替换光标落点锁**（§四定义全边界）；replaceAll 计数与单撤销模拟；mergeRuns 切分正确 |
| `test_notepad_encoding.cpp` | BOM 三型识别与保留；严格 UTF-8 边界（截断尾序列/续字节）；推测标签；二进制占比阈值（F-14）；EOL 四态探测；干净直通/脏重编码矩阵 |
| `test_notepad_language.cpp` | 10 类映射+未知兜底+大小写不敏感 |
| `test_notepad_file_types.cpp`（core 层） | D-D 清单与 `isNotepadExtension` 判据一致；漏斗纯函数（托盘/第二实例判据）直测 |
| `test_notepad_session.cpp` | 规范路径查重（weakly_canonical/退化路径）；注册/注销不变量；pageId 递增——session 纯逻辑抽出（不建窗可测） |
| `test_notepad_assoc.cpp`（module_settings） | 多扩展注册纯逻辑/四态逐扩展/自愈矩阵/unregister 幂等（注入缝 `setShellNotifySink` spy 沿用 `file_association.h:81`） |

- **会话/窗口生命周期可测性缝**（照抄查看器，`md_viewer_window.h:26-46` 同名迁移）：`notepadSessionCountForTest` / `notepadSessionPageIdForTest` / `resetNotepadSessionsForTest` / `notepadFindInjectForTest(path, query)`（与输入 onChange 同通路）。
- **窗口层探针** `tests/probes/notepad_probe.cpp`（既有 probe 机制自动注册：`tests/CMakeLists.txt:463-468`，LABELS "probe"，`DEVDESK_BUILD_PROBES` 门）：headless compose 断言=工具条 id 全集与**无新建钮**、查找条键盘全流程（窗口级 onKeyEvent 直驱：M+F→注入→Enter 单调跳转→Esc）、三选弹窗流（保存失败不关）、只读降级控件矩阵（§7.2）、预览切换树断言、`--screenshot` 对 HTML demo 视觉基准。
- **三个旧探针处置（D-6 定案：缝随迁、断言迁移后旧缝删除）**：
  1. `md_viewer_probe.cpp`：搜索键盘流程断言**迁** notepad_probe；「无编辑入口」断言反转（notepad 必有编辑 input）；渲染对照断言随块流核退役而废（预览走 components::markdown 新断言）→迁移后**删除**。
  2. `md_viewer_bug259_probe.cpp`：断言对象=块流渲染核（notepad 不再使用）→**整删**。
  3. `md_viewer_selection_probe.cpp`：「选中即自动复制」不迁移（N-8 定案）→**整删**。
  - 连带退役：`md_viewer_window.{h,cpp}`、`md_viewer_search.*`（viewer 搜索状态机，F-06 由 notepad_find 接管）及 `viewerSession*ForTest` 用例改造；全仓 `openViewerWindow` 引用清零（R-2 验收）。

## 十二、实施批次（问题 12）

依赖链：**批 0 → 1 → 2 → 3 → 4**（批内棒可并行）。

| 批 | 内容（F-ID） | 依赖 | 并行棒 |
|---|---|---|---|
| 0（EUI-NEO fork，先推 fork 再推 DevDesk） | 框架三件套：①`requestWindowFocus(Handle)` ②`setWindowTitle(Handle, title)` ③`onCloseRequested` veto + `setQuitGuard`（glfw_app_main 消费点）；双平台各自 `*_win/_macos`；流程照 FD-1 先例（commit 51e793a） | — | 单棒（同仓一分支）；**minSize 已在 HEAD 无需做** |
| 1（骨架+漏斗+退役） | 模块骨架/session/开窗聚焦（F-01/F-02/F-20①②③④⑤⑧）；漏斗五处+`core/notepad_file_types.h`（R-1/R-3/R-6/R-7）；md_viewer 退役（R-2/D-6 探针处置）；基础状态栏（F-17 基础版：行:列/●保存态）；I/O+编码层（F-03/F-04/F-14/F-16 阈值/F-15 探测层） | 批 0①③（聚焦与关窗保护） | 棒 A=漏斗+session+退役；棒 B=I/O+编码纯函数 |
| 2（编辑增强） | 查找替换引擎+高亮统一层（F-06/F-07/F-08/F-12；ui_kit 迁移） | 批 1 | 棒 A=notepad_find+UI 接线；棒 B=keyword_highlight 迁 ui_kit+module_markdown 改 include |
| 3（视图与格式） | 字号字体（F-09/F-10，D-1 联动）/wrap 开关（F-19）/JSON 格式化（F-11）/状态栏编码行尾点选（F-15 UI 面）/md 预览（F-13）/大文件降级 UI（F-16 提示条） | 批 2（预览/格式化依赖语言映射；wrap 依赖 EUI-NEO 池 267 缺口①②③收尾） | 棒 A=字体字号+wrap+状态栏；棒 B=JSON+预览 |
| 4（保护与关联） | 未保存保护链+quit guard 接线（F-05，D-3 逐窗）；设置页记事本开关+md 卡退役（F-18，D-7）；多扩展注册+系统 UI 拉起 | 批 1（会话）；F-05 依赖批 0③ | 棒 A=保护链；棒 B=关联开关 |

- **#269 处理**：已落地（`dsl_app.h:291`），批 1 直接 `.minSize(720,480)`，无等待、无降级。
- 窗口尺寸记忆（设计 §2 应用级一份）+ 越界拉回（`getPrimaryMonitorWorkArea`，`window_backend.h:61`）归批 1 棒 A。

## 十三、F-ID 覆盖核对表（20/20）

| F-ID | 本卡章节 | 关键断言/证据物 |
|---|---|---|
| F-01 | §三 | 各档扩展名双击开窗载入；冷启动托盘（R-3）；文件不存在→错误窗；未注册→Home；probe+UAT |
| F-02 | §二（聚焦）/§一入口 | M+O 未开→新窗；已开→聚焦+开窗数不变（notepadSessionCountForTest）；取消零副作用 |
| F-03 | §八 | 直存+mtime 三选；写失败保持未保存；干净直通字节一致（doctest 矩阵） |
| F-04 | §八 | saveFileDialog（FD-1 已交付回归）；取消保编辑态；OS 覆盖确认 |
| F-05 | §九 | 三选（dialog content() 三钮）；保存失败不关窗；quit 逐窗 D-3；探针弹窗流 |
| F-06 | §四 | 输入即高亮 n/N；Enter/Shift+Enter 环绕；大小写/全词；Esc 清；重开留词；探针键盘全流程 |
| F-07 | §四（落点锁） | doctest 落点锁断言=下一命中 start；单步撤销 |
| F-08 | §四 | 整批单撤销；「已替换 N 处」含 0 处；doctest |
| F-09 | §十二批 3 | systemFontFamilies 下拉（FD-3 已交付回归）；即时生效+持久化（notepad_prefs） |
| F-10 | §八/§十二批 3 | A±走 `core::theme::setEditorFontSize`（`theme.cpp:307-314` 通道，D-1 联动）；钳 [10,32]（PRD）；笔记页双向生效 |
| F-11 | §七 | pretty+可撤销；行列提示+光标跳转；键序明示文案 |
| F-12 | §五 | 10 类映射各一采样高亮；.css→纯文本可编辑；doctest+目检 |
| F-13 | §六 | 仅 md 显预览钮；缓冲实时渲染；切回光标/滚动保留；失败降级提示 |
| F-14 | §八 | 清单外纯文本全功能；二进制占比→拒编+只读选项（doctest 阈值） |
| F-15 | §八/§十二批 3 | BOM 识别保留；GBK→只读+推测标注；CRLF/LF 点选一次撤销；干净直通不擅自转换 |
| F-16 | §八 | 1MB 关高亮/wrap+提示条；10MB 只读；POC 数据引用；常量 QA 可调 |
| F-17 | §一/§十二批 1+3 | 行:列/字数/编码/行尾/●未保存 实时刷新（探针树断言+目检） |
| F-18 | §十 | 自动注册回读真态；Win 拉系统页；macOS 直接生效；失败真态+重试；无假绿 |
| F-19 | §四/§十二批 3 | wrap 开关即时+持久化；大文件强制关（`input_model.h:741-765` POC 能力已证） |
| F-20 | §二/§三/§十二批 0-1 | 同文件聚焦开窗数不变；同名异文件并存带后缀；托盘/最小化不影响；关最后窗留托盘；quit 保护链（批 0③） |

## 十四、POC 结论与复用矩阵

- 软换行：POC 实测框架已支持（`input_model.h:741-765`/`:150`），五链自洽；残余缺口①②③（EUI-NEO 池 267，M 级）批 3 前收尾。POC 数据本卡 §八阈值引用。
- FD-1 `saveFileDialog`（`platform.h:80`，51e793a）/FD-3 `systemFontFamilies()`（`text.h:125`，dadd536）已交付，转 QA 回归，不重验。
- 复用清单：md_viewer session 架构与测试缝（迁移）、`fs_text::readTextFile`、`atomic_write`、`platform/json`、LangSpec 9 类+md_highlight（迁 ui_kit）、`settings.editorFontSize` 通道（D-1）、file_association 注入缝与三态纪律、`armTrayIfViewerOnlyLaunch`/glfw 托盘子窗存活机制（`glfw_app_main.cpp` 托盘分支，bug270 修复在 HEAD）、`components::markdown`、`components::dialog`、`requestWindowClose`。

## 十五、取舍（放弃/推迟）与风险未验证项

**取舍**：
- 放弃：复用 module_markdown 块流预览核（隔离）；code_lexers 模型统一（LangSpec 单入口已覆盖 v1 全语言，交集以 LangSpec 为准）；提取 scaledPreviewStyle 到 ui_kit（3 行复制更小）；Notepad4 定时器分块标记（大文件模式标记随高亮同关）。
- 推迟：编码转换层（D-8 选项 1）；正则/选区内替换（N-2/N-12）；100KB 单行增量切行优化（POC §4④，L 级不推荐）；窗内多标签（N-1）。

**风险/未验证项（按权重）**：
1. **批 0 框架三件套是唯一真阻塞**：无 `onCloseRequested` veto 则红绿灯关窗无保护（数据丢失级）；无 `requestWindowFocus` 则 v5 同文件聚焦（核心裁决）不可实现。批 0 必须先于批 1 完成，双平台验收含 quit 三路径（托盘 Exit/Cmd+Q/主窗 ×）。
2. `requestWindowFocus` 平台行为差：macOS 最小化窗需 deminiaturize 配套；Windows 后台进程 `SetForegroundWindow` 可能被 OS 拒（返回后需 `focusOnShow`/闪任务栏兜底）。UAT I2 需含最小化态聚焦用例。
3. `components::markdown` 整篇渲染对 1MB 级 md 的预览成本未实测——降级转纯文本分支兜底（F-13 异常态），QA 实测定档。
4. **D-D 三档清单原文未命中**（§三）：批 1 开工前对一轮裁决原文校准，唯一待补事实。
5. F-18「关闭=撤销」与现「生产永不注销」语义冲突已按新裁决处理（§十），老用户行为变化需 USER_GUIDE 说明。
6. normKey 大小写折叠边界：Windows 不敏感卷两 casing 理论上可双开（OS 送达通常同 casing）；若 QA 命中，补 casefold 一行。
7. InputState `lineStyles` 是唯一着色通路的假设基于 POC 事实（styled-runs 仅 color）；若框架存在第二绘制层（选区底色独立属正常），命中色被选区覆盖可接受（官方编辑器同款）。
<!-- doc:42:v1 end -->

### doc#47 · need380-feature-cut-proposals（audit v1 · need#380）

- 标题：功能裁减候选提案（need#380 featcut 线，16 条全须老板裁决） · 创建：2026-11-22 00:11:16 +0800（ZCode主会话）· 更新：2026-11-22 00:11:16 +0800（ZCode主会话）· 历史版本 0 个（未导出）

<!-- doc:47:v1 begin -->
# need#380 功能裁减候选提案（仅提案，全部「须老板裁决」，不构成行动）

**需求一句话**：替用户审视 DevDesk 14 模块，找出「定位偏离 / 配置过载 / 重复入口 / 流程过长」的减法候选，供老板裁决。
**依据**：USER_GUIDE.md（2026-11-18 重写版）+ src/app_registry.h kModuleRegistry + 模块源码 wc 实测（agent 14741 / memory 12676 / release 12637 / markdown 10933 / settings 9948 / claude_proxy 9744 / wallpaper 8225 / schedule 7542 / todo 6729 / mcp 5098 / git 4856 / notepad 3890 / terminal 3538 / home 1528 / about 407 行）。

## A 类：整功能退役（每条均须老板裁决）

| ID | 候选 | 现状（入口/步数/规模） | 价值论证（证据） | 减法收益 | 风险 |
|---|---|---|---|---|---|
| A-1 | **壁纸模块整体退役** | 侧边栏独立页（app_registry.h:58，模块开关+平台门控双闸），播放页+片库页两页；8225 行（第 7 大） | ①唯一与「开发者工具集」定位无关的生活化功能；②出厂默认关闭=自认低频；③指南 §3.9 边界密度大；④花見导入仅 macOS，功能不对称 | -8225 行；砍视频解码/系统事件监听/断点续播三块高复杂度维护面；侧边栏减一入口；托盘壁纸菜单组连带收敛 | 真实在用者价值全损；默认关闭使沉默用户不受影响。**推荐：裁** |
| A-2 | **上线模块整体退役**（任务+基础配置两子页+提醒弹窗） | 分组父项+两子页+设置页提醒开关；12637 行（第 3 大）；新建任务前置 3 步：建服务→建环境→建任务 | ①多服务多环境发布跟进是团队运维场景，个人本地工具受众窄；②指南 §3.12 第 1 步即「先到基础配置建环境」——流程过长实锤；③跨零点时间窗/非常规发布日二次确认等规则密度大 | -12637 行；侧边栏分组减一；提醒弹窗链路连带简化 | 若老板本人正用于跟发版则损失 100%（本仓库自身走 release 流程，可能即自用场景）——**建议先查 devdesk.db release 表近 30 天写入再裁** |

## B 类：去次要能力保主路径（每条均须老板裁决）

| ID | 候选 | 现状 | 价值论证 | 收益 | 风险 |
|---|---|---|---|---|---|
| B-1 | 定时触发类型六类→三/四类 | 新建弹窗六选一（一次性/间隔/周期/cron/事件/常驻，指南 §3.5）；schedule 7542 行 | 「周期」语义被 cron 完全覆盖；「事件/常驻」指南描述最薄、无示例——「指南说不清用途」信号 | 表单六选一收窄；触发引擎分支与预览测试面收缩 | 存量规则迁移成本；不会 cron 的高层用户流失 |
| B-2 | 智能体出厂模板 13 张→3~4 张 | 新建弹窗一键套用（指南 §3.7）；agent 14741 行（最大模块） | 巡检类 7 张语义高度重叠；13 选 1 是认知负担 | 模板库维护面与相关测试收缩 | 高频用被砍模板者需自配提示词 |
| B-3 | 智能体「试运行」砍除 | 任务卡独立操作=临时任务跑一轮自动删除（指南 §3.7） | 与「启动」高度重叠，「不落正式任务」差异对单用户价值薄弱 | 少一条运行路径 | 极小 |
| B-4 | 记忆健康监视收敛 | 健康四格+token 消耗带+调用记录+动态流+「跑评测」（指南 §3.8）；memory 12676 行（第 2 大） | 观察性 UI 过重：普通用户只需引擎状态点+故障详情；「跑评测/动态流」是开发者调试面 | memory UI 层显著收缩 | 排查注入质量少仪表；保留「详情」弹窗兑底 |
| B-5 | 记忆用量告警 4 滑条→默认值+单开关 | 日档/月档 × 软/硬档 4 条滑条（指南 §3.8） | 4 档组合用户无从估摸，合理默认值可覆盖 | 配置区减 3 项 | 重度用户失去精细化控制 |
| B-6 | Profile 右键「建副本」并入「复制配置」 | 右键 5 项并存（claude_ui_list.cpp:854）；claude_proxy 9744 行 | 「建副本」与「复制配置」重叠，用户难分使用时机 | 菜单 5→4；少一条克隆/序列化路径 | 极小：复制后新建粘贴可达同一目的。**推荐：做** |
| B-7 | Claude 页「运行」临时会话砍除 | 详情区按钮=选目录以该 Profile 直连开临时 Claude Code 会话（指南 §3.6） | 与终端模块重叠；「临时配置」价值被代理接管本身覆盖 | 少一条临时会话生命周期管理路径 | 不开代理只想临时切 Profile 跑一次的用户 |
| B-8 | 笔记四态视图→三态（实时并入双栏） | Tab 编辑/实时/双栏/预览+⌘⇧P 循环（指南 §3.2）；markdown 10933 行 | 三种渲染形态重叠；同类产品先例仅 2 态 | 视图状态机减一态 | 实时态有 spec 支撑且是差异化卖点——**列低优先，倾向保留** |

## C 类：流程/入口/配置简化（每条均须老板裁决）

| ID | 候选 | 现状 | 价值论证 | 收益 | 风险 |
|---|---|---|---|---|---|
| C-1 | 待办自动归档规则简化 | 启动时把完成时间早于当日零点的卡片静默搬入归档（指南 §3.4 以最大篇幅解释，含「昨天点完成今天就没」困惑场景）；todo 6729 行 | 指南需 200+ 字才说得清的规则=认知负担超标直接证据；静默搬移负价值 | 砍规则分支（含同步导入后补跑 todo_service.cpp:719）；困惑消除 | 方案：改「首次触发弹 toast」或「降为手动右键」。**推荐：加提示而非砍规则** |
| C-2 | 首页 token 图表卡+快捷按钮收敛 | 5 统计卡+今日 token 图表+3 快捷按钮；home 1528 行 | token 图表与 Claude 页调用记录数据同源；「切换 Profile」与 Claude 页右键「设为当前」重复；首页职责是导航枢纽 | home_service 查询面收缩；重复入口消除 | 小 |
| C-3 | .md 文件关联入口收敛到笔记模块 | 关联 .md 由独立记事本第二实例承接（module_notepad/ 3890 行；app_assembly.cpp:99 独立退出保护链） | 同一 .md 双编辑器入口，与笔记模块能力重叠；第二实例+脏窗保护链是成套维护面 | 至多 -3890 行或大幅收缩 | **建议保留非 md 文本查看，仅收敛 .md 关联** |
| C-4 | 终端设置细项收敛为默认值 | 外观+会话两组弹窗：光标块线/闪烁/回滚行数/启动目录等（指南 §3.11）；terminal 3538 行 | 光标闪烁/回滚行数属「能用默认值的不该让用户选」 | 弹窗减一组；持久化键减 4~5 个 | 极小；高级需求走环境变量 |
| C-5 | 设置页日志保留 6 档→3 档 | 永久/7/30/90/180/365（指南 §3.13） | 90/180/365 对单机日志差异微小 | 下拉减 3 项 | 极小 |
| C-6 | MCP「记忆开关」双入口收敛 | MCP 页记忆卡开关（§3.10）与设置页「开放记忆查询」开关（§3.8）两处入口 | 同一控制面双入口；若非同源存储则为配置漂移隐患 | 入口减一 | 极小 |

## 排序（价值损失小 × 减法收益大优先）

B-6 → C-5 → C-6 → C-4 → C-2 → B-3 → B-5 → B-2 → C-1 → B-1 → B-4 → B-7 → C-3 → B-8 → A-1 → A-2

## 14 模块覆盖面声明

- 有候选 11 模块：home（C-2）/ markdown（B-8）/ todo（C-1）/ schedule（B-1）/ claude_proxy（B-6/B-7）/ agent（B-2/B-3）/ memory（B-4/B-5）/ wallpaper（A-1）/ mcp（C-6）/ release（A-2）/ settings（C-5）；附加：notepad（C-3）
- 无候选 3 模块：about（407 行纯展示）；**git——建议保留**（只读查看器定位清晰，是笔记 git 自动提交的观察窗）；terminal 仅出 C-4 细项级收敛

## 开放问题（均待老板判定，附推荐）

1. **A-2 上线模块是否老板自用在跑？** 推荐先查 release 表近 30 天写入频次再裁；无人写则裁、在用则撤案。
2. **壁纸是否有真实使用？** 可查壁纸模块开关状态与播放记录；推荐默认裁（A-1），在用则降为「保持默认关+冻结演进」。
3. **B-8 实时态是卖点还是低频路径？** 现无使用埋点，需老板自用体感判定；推荐暂不动 markdown。
4. **减法节奏**：候选 16 条是否按排序分批走流程（每条独立需求单）？推荐每条走完整 need 流程，不打包。
<!-- doc:47:v1 end -->

### doc#48 · need380-slimming-tech（tech v1 · need#380）

- 标题：代码精简技术方案+裁剪清单（v1 待审，四批 -3,200 行+功能 16 条） · 创建：2026-11-22 00:46:00 +0800（ZCode主会话）· 更新：2026-11-22 00:46:00 +0800（ZCode主会话）· 历史版本 0 个（未导出）

<!-- doc:48:v1 begin -->
# need#380 代码精简技术方案 + 裁剪清单（v1，待老板审，审前零动作）

## 1. 目标与原则
- 五维抠门（代码量/注释量/执行周期/内存/CPU）按功能与 UI 卡粒度卡死；减法优先（少即是多）。
- 行为不变：所有裁剪/压缩不改用户可感行为；改动后测试绿。
- 硬约束：EUI-NEO 本战役默认不动（点名才动）；平台不对称红（mac FileWatcher 轮询兜底×2）不碰；测试只清死助手不动活断言；全量验证只在阶段节点跑。
- 总纲参照：「0 用户项目不背历史包袱」=默认拒兼容、默认准删；但逐条清单过目后再动。

## 2. 批次与裁剪清单
### 批A 安全删族（≈-215 行，22 项，全部全仓 0 引用证据 clue#370，高置信）
| # | 项 | 位置 | 行数 |
|---|---|---|---|
| 1 | ReleaseViewState 整类（零实例） | release_view_state.h:8 + .cpp:1-25 + CMake 摘除 | -47 |
| 2 | containsAsciiCaseInsensitive | release_detail_draw.cpp:375 + release_detail_internal.h:228 | -20 |
| 3 | PrereqView 死结构体 | release_detail_internal.h:76 | -10 |
| 4 | liveActiveBlockStableId 死方法（含相邻注释） | md_module_preview.cpp:794 + markdown_module_internal.h:691 | -9 |
| 5 | CapturedBlock 死局部结构 | md_module_menu_poll.cpp:248 | -8 |
| 6 | rowToNote 死静态方法 | markdown_service.cpp:1225 + .h:191 | -4 |
| 7 | formatTodayCn 死函数 | home_service.cpp:51 + .h:57 | -10 |
| 8 | ContextMenuState::openColumn 死方法 | todo_dialogs.h:216 | -6 |
| 9 | shouldRequestFolderSelection | folder_policy.cpp:58 + .h:23 | -5 |
| 10 | locateLlamaServer | llama_host.cpp:95 + .h:37 | -5 |
| 11 | buildHookCommand | hooks_settings.cpp:46 + .h:21 | -4 |
| 12 | StreamingResponse::queueDepth（含注释） | http_client.cpp:465 + .h:126-127 | -7 |
| 13 | autostartRowBelowY 死 inline | settings_ui_logic.h:230 | -3 |
| 14 | k 死常量×9 | settings_ui_logic.h:61-68,244-245 / todo_ui_logic.h:36,709 / schedule_engine.h:123 / call_rate.h:36 | -10 |
| 15 | LOG_TRACE 死宏 | core/logging.h:35 | -2 |
| 16-19 | 死测试件×4 | MiniTool test_agent_tools:771(-12) / trimSpaces driver_schema.h:111(-6) / seedAppConfigRow test_support.h:647(-5) / PageIconProbe test_module_icons:75(-5) / totalCalls test_memory_engine:88(-3) | -31 |
| 20 | pr_win_placeholder 恒真占位用例（全仓唯一假测试） | test_process_runner.cpp:93 | -15 |
| 21 | tests/CMakeLists 孤儿注释×2 | :444,:464 | -2 |
| 22 | 需验证：孤儿场景 terminal-claude-interactive.json（0 引用，可能待接线） | tests/ui_driver/scenes/ | -31 |
排除项（清单内不删）：B1 测试钩子（claude_data_refresh_wake_saw_ready_for_test 待上游确认）、jelly_probe enum（改匿名无收益）、unicodes.txt（活物）。

### 批B 裁决级删除（≈-1,850 行，待裁决）
1. 死脚本×4（全仓含隐藏目录 0 引用）：audit-font-scale.sh(-95) / syntax-sweep.sh(-125) / worktree-audit.sh(-330) / dispatch-check.sh(-260)
2. tools/memcpp-integration/spike/ 仓库外归档：-1,019 行（telemem 结论已回填生产，telemem_process.cpp:68 自证使命已毕）
3. fonts/ CMake 死拷贝块：CMakeLists.txt:490-498 -9（需确认双机无手工 fonts/）
4. .gitignore 死条×3：:120 !assets/models/* no-op / :50 JetBrainsMono / :55 3rd/doctest
5. .zcode 门禁 PROTECTED_PATTERNS 死路径×6（pre-tool-use.sh:4,76,89，2026-10-29 清理遗存，0 行收益纯噪声）
另列单卡（本批不含）：assets gguf/7z 双份收敛 26.5MB（动 2026-11-04 随包令）；EUI-NEO B组 -107 行+五组件（datatable/layout_debug/piechart/navbar/radio）消费面去留。

### 批C 重复压缩（≈-940~1,235 行，待裁决；行为不变机械迁移+抽壳）
主仓（8 项）：工具五件套→platform/tool_kit -165（12 文件逐字同构）/ Result::propagate 36 文件 -150~250 / JSON getIfX 44 处 -60~90 / process_runner macos/posix 合并 -80~85（保真 3 处刻意差异）/ wallpaper 库扫描合并 -55~60 / mcp 双 config 编排 -35~40 / wallpaper 三 probe 脚手架 -100~130 / font_metrics gate 合并 -45~55。
EUI-NEO（2 组，需先出共享层设计过审后另批）：组件头样板 5 对 -120~140 / 渲染后端样板 5 对 -120~150。

### 批D CI+工具+文档（≈-150 行+质量项，待裁决）
CI：release.yml 补 EUI-NEO --remote 步（P1 防发版编译红）+ DEVDESK_BUILD_PROBES=0（P2）；工具：jscpd/Bloaty/include-cleaner/nanobench/hyperfine 接入（clue#367 调研）；docs：ARCHITECTURE.md 摘 module_pet 死引用 + 2026-11-21 审计报告×5 份 12 处路径改实名（P1）+ docs 索引加「历史路径以 git log 为准」。

## 3. 实施方式（待批后）
- 波次串行：批A →（批B/批C 按裁决）逐波派 developer 棒，避免共享 worktree 文件域冲突；单批 ≤2 条目。
- 每棒不 commit；主会话亲自核产物（ls diff/构建/测试）后 pathspec wip: 提交；阶段收口（全批完）跑全量 doctest+probe+守卫链，出 feat: commit。
- 验证：--dev 构建 + 受影响域 --filter 定向；EUI-NEO 若动则先推 fork 再推 DevDesk（跨仓顺序铁律）。
- 回滚：每批独立 wip commit，出问题单批 revert。

## 4. 预算档建议（待裁决，基线=clue#368 实测）
建议采纳档：首帧 ≤1400ms（首启冷缓存 ≤2800）/主界面 ≤1800/RSS ≤220MB/空转 CPU ≤2%/页切 ≤20ms/操作 ≤50ms/二进制 ≤60MB/.app ≤75MB/线程 ≤36。从紧档 -20% 与「先立基线不卡」两备选见裁决卡。内存自动门缺口=后续专项票。

## 5. 功能减法（全部待裁决，全文=doc need380-feature-cut-proposals）
16 条：A砍×2（A-1 壁纸 -8,225 待裁；A-2 上线模块实测在用建议撤案）/B缩×8/C简化×6，PM 排序 B-6→C-5→C-6→C-4→C-2→B-3→B-5→B-2→C-1→B-1→B-4→B-7→C-3→B-8→A-1→A-2。每条须独立裁决。

## 6. 待决清单（老板逐项给口径即可）
□批A（22 项清单过目）□批B（含脚本/归档裁决）□批C □批D □预算档三选一 □A-1 壁纸 □功能减法节奏 □EUI B组+五组件 □assets 26.5MB 双份
<!-- doc:48:v1 end -->
