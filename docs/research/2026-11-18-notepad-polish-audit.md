# 记事本模块全交付面走查报告（need#289 走查棒）

- 日期：2026-11-18 ｜ 角色：qa-tester ｜ 池条目：289 ｜ 上游：need263（20 F-ID 全交付基线）
- 走查范围：src/module_notepad 全 10 文件 + app_assembly 漏斗 + settings/notepad_assoc_card + platform/file_association（线索面）；基准 = PRD v5（doc 40）+ 设计稿 v2（docs/design/2026-11-16-notepad-module.md）
- 结论先行：**总判定 = 完成度高的半成品收口面——主链路（开窗/查重聚焦/保存/保护/查找替换/预览/降级矩阵）全部实证达标；发现 P1×2（另存为整条缺失、快捷键面缺口）、P2×6、P3×10，无 P0。**
- 证据目录：`evidence/2026-11-18-notepad-polish/walkthrough/`
- 实际工具调用约 46 次（预算 90）/墙钟约 35 分钟（预算 60）。

## 0. 走查通道与证据形态说明（先读，决定各条结论的可信档位）

| 通道 | 状态 | 说明 |
|---|---|---|
| ui_driver（场景走查） | 部分可用 | **结构性限制：notepad 子窗在 ui_driver 不可达**——driver 是单窗主程序 Router 驱动（ui_driver_main.cpp:265 composeFn 直连 app::compose，无 createRequestedWindows 子窗泵），且记事本按 N-14 无应用内入口。本报告 ui_driver 证据仅覆盖设置页关联卡（F-18 主窗面）。 |
| notepad_probe（无头 compose） | 可用 | 6 session PASS 本轮复跑 + `--screenshot` 真实 GL 帧落图（glReadPixels，非桌面截屏） |
| 真机 argv 冷启 + screencapture | **弃用** | 本会话 screencapture 输出 7 张全黑（107KB 级），按简报判据弃用；真机启动链本身验证正常（沙箱数据根 /tmp/npwalk/data，日志证实 argv 捕获+odoc 双通道去重） |
| 代码面静态走查 | 全量 | 全部 10 文件逐行走读，P2/P3 多数为此形态（标注「代码面判定」） |

证据形态标注约定：〔E〕=本轮实测证据（截图/运行输出）；〔C〕=代码面判定（只读走查，无交互通道实证，修复棒修完按同锚复验）。

## 1. 流程演练表（20 F-ID × 异常分支脑内演练 + 核销判定）

> 每行=预期主流程 → 异常分支应然行为 → 实际判定。判定符号：✓=达标（证据见 §3）；✗=发现（编号进 §2）；△=PRD 口径偏差（P3 级）；—=已知残留有归属（勿报，简报口径）。

| F-ID | 主流程预期 | 异常分支应然 | 判定 |
|---|---|---|---|
| F-01 OS 送达 | 双击关联文件→漏斗（app_assembly.cpp:280-281 isNotepadExtension 单一真源）→新窗载入；冷启主窗留托盘（:297-307 armTrayForNotepadAssociation） | 文件不存在→错误占位窗（notepad_module.cpp:113-123） | ✓〔E〕argv+odoc 双通道去重日志实拍；错误占位 probe session PASS |
| F-02 窗内打开 | M+O/打开钮→OS 对话框→未开路径新窗、已开路径聚焦、取消零副作用 | 对话框取消→return 不落任何态（notepad_window.cpp:1316） | ✓〔C〕已开路径聚焦+toast 代码面复核（probe 键盘流覆盖 M+O 入口） |
| F-03 保存 | M+S：干净短路「无改动」/脏重编码落盘/基线前移/●清除 | 写失败→toast+保持未保存；mtime 撞车→拦截 toast 不写 | ✓〔C〕mtime 三选不扩=已知残留—；拦截路径代码面复核 |
| F-04 另存为 | M+Shift+S 或保存钮→OS 保存对话框 | 取消→保持编辑态 | **✗ P1-1：整条缺失**（§2） |
| F-05 未保存保护 | 关窗脏→三选；保存失败不关窗；quit 逐窗保护任一取消中止 | 三选「保存」失败→留窗+退出链中止（notepad_window.cpp:896-909 → protect.cpp:13-22） | ✓〔E〕probe G 段 6 session PASS + 截图（弹窗三钮序正确） |
| F-06 查找 | M+F 浮层→输入即高亮+n/N+Enter 环绕+选项 | 零命中 0/0+无匹配；空词清高亮；Esc 清高亮留词；重开留词全选 | ✓〔E〕probe 查找键盘流 PASS + 截图（0/0 无匹配态） |
| F-07 单替换 | 替换当前命中+光标落下一命中 | 无当前命中零动作 | ✓〔C〕replaceOneAt 探测点 afterByte-1 语义正确（notepad_find.cpp:88-93） |
| F-08 批替换 | 全文替换+「已替换 N 处」+整批单撤销 | 0 处也明示（notepad_window.cpp:375-378） | ✓〔C〕pushUndoState 单点单撤销 |
| F-09 字体族 | 下拉系统全量枚举+即时生效+持久化 | 预览态禁用；长名截断（18 字节码点安全，notepad_window.cpp:223-233） | ✓〔C〕prefs 写穿复核；弹层交互缺陷见 P2-4 |
| F-10 字号 | A±钳[10,32]+当前值可见+toast 反馈 | 触界禁用（>32 / <10 早退+钮 disabled，:956-975） | ✓〔E〕截图（A- 14 A+ 三件套渲染正常） |
| F-11 JSON 格式化 | 仅 .json 显示；成功 pretty+可撤销+键序明示；失败行列+光标跳转 | 非法 JSON→toast「第X行 第Y列」+光标跳错误位+内容不变（:393-415） | ✓〔C〕D-4 tooltip 明示+成功 toast 后缀复核 |
| F-12 代码高亮 | 16 扩展映射 8 语言；.ts→Js 收敛—；.md→None—（clue #288 待办） | 清单外纯文本 | ✓〔C〕映射表复核；跨行块注释断层=P3-8 |
| F-13 md 预览 | 仅 .md 显示；整页切换；切回光标/滚动保留 | 空文件「（空文件）」；大文件禁预览 | ✓〔C〕probe 预览切换 session PASS；快键缺失并入 P1-2 |
| F-14 二进制拒编 | NUL 检测→拒编+只读选项 | 「仍要打开（只读）」三选 | △ P3-9：实现直接只读打开（无三选，行为更宽） |
| F-15 编码行尾 | BOM 识别保留；非 UTF-8 只读+标注推测；点切即刻标脏 | GBK/UTF-16 标注精确（LE/BE/GB18030/Latin-1） | ✓〔C〕载入基线回翻=干净态（loadHasBom/loadEol）；UTF-16 乱码展示=P3-9 注记 |
| F-16 大文件降级 | >1MB 关高亮+关换行+提示；>10MB 只读 | 换行钮禁用兜底（notepadToggleWrap early-return，:169-174） | ✓〔C〕双阈值常量+applyLoadedState 矩阵复核 |
| F-17 状态栏 | 文件名｜●未保存｜行:列｜字符数｜编码｜行尾实时刷新 | 窄窗<900 隐编码/行尾 | ✓〔E〕截图实拍；三态/胶囊缺口=P3-4 |
| F-18 关联开关 | 开启→自动注册+真态回读+Win 拉系统页；关闭撤销 | 失败真态+摘要（前4项+…）+重试 | ✓〔E〕ui_driver 10/10（四态 None 契约：无状态字/无引导带）+ assoc probe OK |
| F-19 换行开关 | 开=软换行/关=横滚；即时+持久化；大文件强制关 | — | ✓〔E〕截图（选中态高亮）；横滚无滚轮=已知残留— |
| F-20 多窗管理 | 一文件一窗查重聚焦；关一窗不影响他窗；quit 先保护 | 已有窗已关闭→正常开新窗（注册表存活=窗活） | ✓〔E〕查重聚焦日志实拍+probe；M+W 关窗代码面复核 |

## 2. 分级发现

### P1 功能性缺陷（2）

**P1-1 F-04「另存为」整条缺失** 〔E+C〕
- 事实：全仓无 saveAs 实现（grep `另存|saveAs|SaveAs` 生产代码零命中，仅注释提及）；工具条无另存为钮（notepad_window.cpp:944-1013 工具条装配全量：打开/保存/字体/字号/格式化/预览/换行）；键路由无 M+Shift+S（notepad_window.cpp:1258-1259 `shortcut` 显式排除 shift，S 仅无 shift 分支）；保存链无「无路径转另存为」分支（notepad_file_io.cpp:62-110，该分支因 N-14 无新建确实不可触发，但钮与快捷键是 PRD 明文交付面）。测试面同样零覆盖（tests/ grep 无 notepad 另存为用例）。
- 锚点：PRD v5 F-04（核心级）+ PRD §九工具条清单「打开｜保存｜另存为｜…」+ 设计稿 §4.2 btnSaveAs；复现：任意记事本窗按 M+Shift+S → 无动作；看工具条 → 无另存为钮。
- 证据：evidence/2026-11-18-notepad-polish/walkthrough/notepad-probe-final-state.png（工具条实拍无另存为钮）。
- 影响：用户无法「改动后存为新文件」；F-05 三选的另存为支路（PRD C2）同样无处落。

**P1-2 PRD §九快捷键面缺口（M+H / M+Shift+F / M+P）** 〔C〕
- 事实：notepadHandleKeySession（notepad_window.cpp:1255-1303）键路由仅 O/S/W/F/Equal/Minus 六支。PRD §九快捷键表明文列 M+Shift+S 另存为、M+H 替换（流程 D3「M+H→替换条」）、M+Shift+F 格式化、M+Shift+V 预览（设计稿 §6 用 M+P）——四者全未接线。M+H 影响最大：替换行目前唯一入口=M+F 开条后点「＋」展开（两步且无快捷键）；格式化有按钮兜底、预览有按钮兜底。
- 复现：.json 窗按 M+Shift+F → 无动作；任意窗按 M+H → 无动作。
- 影响：键盘流断裂（PRD 快捷键表是验收要点面）；与 P1-1 同批修键路由。

### P2 体验粗糙/交互假活/性能（6）

**P2-1 错误占位窗字体下拉未禁用** 〔C〕
- notepad_window.cpp:951 `drawFontTrigger(s, ui, t, matrix.fontControlsEnabled)`，而 `fontControlsEnabled = !s.previewOn`（notepad_session.cpp:153）不含 errorState；drawFontPopup（:690-698）亦无 errorState 守卫。错误窗可弹字体面板并写 prefs（无视觉反馈但污染全局偏好）。A± 已守（:954 canStep 含 !errorState）——同类守卫漏了触发器。违反设计稿 §7.2 联动矩阵（错误态：语言/字体/字号禁用）。修法：matrix.fontControlsEnabled 加 `!errorState`（一行）。

**P2-2 只读窗编辑框假活（可打字、回显闪现后回滚）** 〔C，待实证〕
- notepad_window.cpp:1089-1097：readonly/binarySuspect 窗的编辑 input 未设 disabled，onChange 早返回（镜像不更新）→ 用户打字回显一帧后被 value 镜像重置覆盖（「闪现后消失」）。设计稿 §7.2 只读=编辑类全禁用。修法：readonly/binary 态 editor 加 `.disabled(true)`（保存钮同款真禁用口径，clue #274）。probe 只读 session 无输入回滚断言，修复时补。

**P2-3 Esc 关查找条后焦点不回归编辑器** 〔C，待实证〕
- notepadFindClose（notepad_window.cpp:251-260）不 requestFocus；查找输入随条销毁后焦点目标丢失，用户需点击编辑区才能继续输入。对比：开条有 pendingSeed 播种+requestFocus（:475-484），关条无对称回收。修法：关条时 requestFocus(kNotepadEditorInputId)。probe 键盘流未覆盖「关条后输入」，修复时补断言。

**P2-4 字体下拉弹层无「点击外部/Esc」收起** 〔C〕
- 弹层 hit rect 仅覆盖弹层自身 260×300（notepad_window.cpp:714-718 blockPointer 只防穿透），无全窗遮罩层；Esc 分支仅 find.visible 时处理（:1298-1301），FontPopupState.open 无任何 Esc/外点收起路径。违反设计稿 §5.2「点击外部/Esc 收起不改值」。开合只能靠再点触发器（:681-684）。修法：弹层开时铺全窗透明点击层（收起语义）+ 键路由 Esc 分支加 ps.open 收起。

**P2-5 状态栏统计每帧全量扫描、无节流** 〔C〕
- notepadComposeSession 每帧执行 notepadUtf8Length(全文)+notepadByteOffsetToLine(全文)+notepadLineStartOffset(全文)（notepad_window.cpp:1143-1173），>10MB 只读窗=每帧约 3×10MB 字节遍历（约 3-5ms/帧，60fps 预算吃两成以上）；1MB 大文件档同样每帧 2×1MB。设计稿 §11.4 明文「统计节流 200ms」未实现。修法：统计结果按 textRevision+cursor 缓存，或 200ms 节流。

**P2-6 字重两档制违例：关联卡状态字 500** 〔E〕
- notepad_assoc_card.cpp:181 `.fontWeight(500)`——400/600 两档制第三档（need#284 走查同款漏盘，老板口径：关键内容 600、明细 400，无第三档）。notepad 域其余文件 grep 零 fontWeight 调用（默认 400）——全域仅此一处。修法：改 600（状态字属关键语义档）。

### P3 打磨项（10）

| # | 发现 | 锚点 | 形态 |
|---|---|---|---|
| P3-1 | CJK 文件名脏态「●未保存」定位估宽偏小：码点×7px 粗估，CJK 12px 字号实宽约 12px/字，文件名 >4 个 CJK 字时 ● 叠上标题文字 | notepad_window.cpp:1129-1131（注释自认「CJK 偏宽按余量吸收」仅 20px 余量） | C，视觉未证（真机截图通道黑屏） |
| P3-2 | 状态栏编码段「GB18030（推测）」约 109px 超 102px 固定段宽 → 截断/溢出 | notepad_window.cpp:1180-1204（段宽 102 硬编码） | C |
| P3-3 | 工具条为 64×28 文本钮，设计稿 §4.2 为 24×24 图标钮+tooltip 必带；实现仅格式化钮有 tooltip；设计稿 menuMore（重新加载/复制文件路径）整块未落（PRD 未列，属设计稿单方承诺） | notepad_window.cpp:51-84/944-1013 | C，设计稿偏差注记 |
| P3-4 | 状态栏三态「✓已保存/●未保存/✕保存失败」只实现●态；「只读」胶囊未实现（提示条代替）；状态栏 title 12/400 与设计稿 §4.7 13/600 不符 | notepad_window.cpp:1119-1141 | C |
| P3-5 | 大文件提示条不可关且统一 warning 色；设计稿 §7.4 高亮关闭档=info 可关、只读档才 warning 不可关 | notepad_window.cpp:1016-1033 | C |
| P3-6 | 保护弹窗 460×200 vs 设计稿 §4.9 宽 420；文案为问句式（设计稿陈述式，语义等值） | notepad_window.cpp:798-799 | C，E（截图） |
| P3-7 | .json 窗换行钮固定 x=564 为预览钮位让位，预览钮不渲染时留 80px 布局空洞 | notepad_window.cpp:1007-1013 | C |
| P3-8 | 软换行切段+跨行 token（块注释）第二行起高亮丢失——per-line 词法契约固有（notepad_language.cpp:54 契约位注释明示），与 clue #288 词法收敛待办邻接 | notepad_language.cpp:51-68 | C |
| P3-9 | ①非 UTF-8/UTF-16 只读展示=原字节乱码（v1 无转换层已知限制，推测标注到位；PRD「可看不可改」实际「看」是乱码——范围注记非缺陷）②二进制实现直接只读打开，PRD F-14「拒编+仍要打开（只读）」三选未做（行为更宽：不拒开） | notepad_module.cpp:60-72 / applyLoadedState | C，PRD 口径偏差 |
| P3-10 | 干净态保存钮灰显（editable=false）但「✓已保存」无载体；保存失败 ✕ 态同样无载体（并 P3-4） | notepad_window.cpp:947/1128 | C |

## 3. 正面确认清单（已达标项——修复棒勿误伤）

1. **查重聚焦链**：argv+odoc 双通道同一文件冷启，第二路查重命中聚焦不开新窗（本轮真机沙箱日志实拍：`记事本开窗 pageId=notepad.1` + `查重命中（pageId=notepad.1）` 同秒双行）；托盘态聚焦先 requestWindowFocus 拉前台，toast 窗内渲染可见性成立。
2. **保护链全语义**：干净直通/脏拦截（onCloseRequested veto）/「不保存」一次性豁免+再编辑作废（sessionSetDirty 开头清旗标，含早返回路径）/quit 语境 rearmExit 再入队/取消中止整个退出/保存失败留窗——probe G 段 6 session PASS〔E〕+截图三钮序（不保存红字|取消|保存主钮）与设计稿一致。
3. **保存链**：干净短路「无改动」幂等；mtime 拦截不写+toast；写失败保持未保存；落盘成功基线前移+mtime 重取；原子写。
4. **查找替换引擎**：输入即重算（appliedQuery 门控）；0/0+「无匹配」；Esc 清高亮留词；重开留词全选+聚焦；替换后落点=下一命中（探测点语义正确）；整批单撤销；命中层覆盖语法层+相邻同色合并。
5. **降级矩阵**：>10MB 只读+>1MB 大文件模式（高亮/换行双关+钮禁用+提示条）；UTF-16 LE/BE/GB18030/Latin-1 精确标注；二进制 NUL 检测；BOM 剥离+回写；行尾 Mixed 检测+归一。
6. **编码/行尾点切**：元数据翻转+即刻标脏+回翻到载入基线=干净（loadHasBom/loadEol 双基线设计）；窄窗 <900 隐段守卫（900 窗实算无重叠）。
7. **预览态联动**：查找/字体/A±/格式化禁用（matrix 纯函数）+弹层残留强制收起+切回光标/滚动保留（InputState 不动）+空文件占位。
8. **窗级规格**：minSize(720,480) 落地；几何记忆+越界拉回+脏库钳制 [720,8192]；prefs 三键写穿；错误占位窗可开可关不掏空。
9. **F-18 关联卡 fresh 态契约**：ui_driver 10/10——标题/说明/开关在轨；关+从未开过=状态带整带不渲染+无引导带〔E〕；assoc 探针四态几何+截图判据 OK。
10. **基线复证**：notepad 域 doctest 47/47（本轮 `--dev --filter notepad` 实测）+ notepad_probe 6 session PASS + settings_notepad_assoc_probe OK。

## 4. 修复批次建议（按文件域分组）

| 批 | 内容 | 文件域 | 预估 |
|---|---|---|---|
| 批 1（P1） | 另存为全套：工具条钮+M+Shift+S 键路由+notepadSaveAsSession（复用 FD-1 saveFileDialog）+F-05 三选另存为支路；键路由补 M+H（直开替换行）/M+Shift+F/M+P | notepad_window.cpp（键路由+工具条）+ notepad_file_io.cpp + notepad_session.h | M |
| 批 2（P2 小改集合） | ①matrix.fontControlsEnabled 加 !errorState（session.cpp:153 一行）②只读态 editor .disabled③关条 requestFocus 回编辑器④字体弹层全窗点击层+Esc 收起⑤状态栏统计缓存/节流 | notepad_window.cpp + notepad_session.cpp（矩阵一行） | S |
| 批 3（P2 一行+P3 收尾） | notepad_assoc_card.cpp:181 fontWeight(500)→600；●估宽 CJK 加权；编码段宽自适应/截断；状态栏三态+胶囊；提示条分档文案/可关性；换行钮布局空洞；tooltip 补齐 | notepad_assoc_card.cpp + notepad_window.cpp | S-M |
| 不动（有归属） | mtime 三选不扩、F-19 横滚滚轮（clue #294）、TS/Markdown 词法（clue #288）、字体下拉真机 UAT、EUI 基线红（#287） | — | — |

- 修复验收锚：P1-1 修完必测「另存为→OS 对话框→取消不落盘→覆盖确认→本窗转新文件」全链；P2-2/P2-3 修复时 probe 补「只读输入回滚」与「关条后输入直达编辑器」两断言。
- 回归面：批 1/2 改键路由与 matrix——probe 6 session + 47 doctest 全量复跑为收口门。

## 附：通道事故记录

1. 真机 argv 冷启×7（沙箱数据根）链路正常，但 screencapture 全黑（107KB 级）弃用——真机窗口像素通道本会话不可得，视觉证据改由 probe --screenshot（GL 帧落图）承担。
2. ui_driver 截图在「点击 tab 后立即截屏」会捕到未重组空帧（首轮 assoc-card 截图内容区全空）；加 `wait ms≥1` 缓冲帧后正常——后续场景编写注意。
3. 沙箱确认：全部真机启动走 DEVDESK_DATA_ROOT=/tmp/npwalk/data（日志 `应用数据目录: /tmp/npwalk/data` 实证），未触真实 ~/DevDesk，未触真实系统关联（F-18 开关只断言未切换）。
