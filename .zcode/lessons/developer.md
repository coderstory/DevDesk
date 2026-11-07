# developer 角色经验沉淀（一事一行带日期与来源，与既有规则重复不记）

- 2026-10-29（memcpp MCP 集成，审查 P1 打回 R1）：`platform/json` 的 JsonImpl Int/Double 字段互斥写入（json.cpp valToImpl），`asInt()` 对 Double 形态恒返回 0——凡「客户端可传浮点」的数值字段禁止裸 `asInt()`：先按形态分路，Double 走 `asDouble()` + `floor(d)==d` 整值判定，域内钳制后再收窄；单测必须覆盖 double 形态（3.7→默认 / 2.0→采纳 / 1e20→钳顶）。
- 2026-10-29（memcpp MCP 集成 R2）：「写盘失败可上报」类修复要查全 `fwrite`/`fclose` 返回值——磁盘满最常在短写/冲刷显形，只查 fopen 等于没堵住；半截文件 + 下次 load 全默认 = 静默状态复活链。
- 2026-10-31（老板定调，docs/TESTING-DISCIPLINE.md 成文）：测试禁形式主义/覆盖率导向——每条用例必须防「特定功能×特定场景×特定边界」的回归，新增前过三问（防什么功能回归/什么场景/卡哪条边界），答不出不写；会红性铁律=删被测代码必红；不看行覆盖率，看功能×场景×边界覆盖矩阵，未钉格子显式记明账。
- 2026-11-03（UI批次IV B3/B4）：EUI hover 父子不互继承、hover 全局唯一——卡 hover 态会被 zIndex 更高的命中面/卡内控件抢走：命中面代报态（onHover/onPress 写集合、卡底每帧读集合）+ 卡内控件与命中面**兄弟集合取并集**（单集合在指针移入文字那一帧被 erase 抹掉，B4 S4b 断言钉住）；开 set 不开单 bool（父先子后同帧覆盖）。
- 2026-11-03（UI批次IV B2，B3/B4 复用有效）：验证轮廓/局部变化禁用区域均值（被大片同色区稀释，实测 0.95/通道跨不过阈值）——用「逐像素通道差>阈值计数」+ 直接读 compose 写入的 element 目标值（radius/scale/opacity）双保险；像素采样必须先稳帧（首帧布局未跟上会假红）。
- 2026-11-03（池73）：FontAwesome 码点 cmap 有映射≠字形存在——0xF46D 的 charstring 就是实心方块；ink/绿通道判据对此无区分力（新旧码点都「有墨迹非绿主导」），有效判据=字形包围盒满行占比（方块 65.4% vs 正常 31.8%，阈值 55%）。新码点上线前必跑形状分布判据。
- 2026-11-03（池72）：删功能时连根查数据写入方——删「最近调用」UI 把 claude_call_logs 唯一落库段一并删掉，下游图表静默空态三个月才暴露；「有数据不画图」类问题先复现+查数据层再查绘制层。
- 2026-11-03（need114 批次修复）：第三方 MCP 工具先读 venv 源码再定修复路线——telemem search_memories 原生支持 threshold 参数（[0,1] 相似度下限，缺省 None），「融合段无逐条 score」只堵死后过滤不堵引擎侧截断，M 级预闸方案直接降 S 级传参；阈值必须真数据校准（POC 0.5 对 2 条记忆完美、探针 27 条库即过杀弱相似正例，终值 0.4）。
bash 3.2 (macOS)：双引号串内 $var 紧跟全角字符（，/）等）会被误并入变量名报 unbound variable——紧跟非 ASCII 一律写 ${var}（来源：2026-11-03 run-memory-e2e.sh，3 处实撞）
- 2026-11-03（池110 CI 红）：改 UI 几何后定向过滤词会漏跨文件用例——home 图表断言住在 test_ui_modern（词「Home Token 分布」）而非 test_home，--filter home 全绿照样 CI 红；几何类改动收尾前按「被改元素 id」反查全测试域 grep 一遍（如 grep home.api.chart tests/）再挑过滤词。
- 2026-11-03 池140批次：tests/doctest.h 是自研极简框架（无 namespace doctest/无 Approx/REQUIRE=CHECK 别名、断言即 return false）——写测试断言禁用真 doctest API（doctest::Approx 等），浮点比较走 std::fabs(a-b)<eps 仓内口径（来源：统一QA 首红 test_schedule_ui_list.cpp 六处）
- 2026-11-03（统一QA 三连红）：EUI column/scrollView content 里的带 position() 子件必须 .ignoreLayout() 且流式高度由占位子件贡献——三次复发（池133 release cardbox → 池140 schedule masonry 卡全贴 x=0 → 池131 about 滚动列 label/value 竖叠）；写「绝对定位子件进流式容器」时先查该容器是不是 column/scroll content，是则套行栈习语（行=流式 stack、行内子件 ignoreLayout 绝对定位）
- 2026-11-04（B8 内存采样器）：Windows PowerShell 5.1 跑 .ps1 文件按 ANSI(GBK) 读无 BOM 文件——UTF-8 中文注释会偶发 ParserError（语法合法也炸）；采样/自动化 ps1 一律纯 ASCII。另：bash 双引号里 `$var\evidence\x.csv` 混合分隔符喂 cygpath 会产出粘连路径（evidencexxx）——统一写 `$(cygpath -w "$var/evidence/x.csv")` 全正斜杠。
- 2026-11-04（B8 体积批实测）：strip/objcopy --add-gnu-debuglink 对新写的大 PE（68-298MB）偶发 Invalid argument（AV 瞬时锁），sleep 2 重试即过，且两次复跑中招点不同（一次 debuglink、一次 strip）——装机脚本对这两个调用各带 3 次重试；strip 后必须做体积断言（dst < src，否则 rm 目标文件退出）防「假 strip」静默出货（复跑实测抓到过：strip 失败但文件以原尺寸留在目标）。
- 2026-11-04（bug185 白帧回归）：EUI DSL Element 默认填充色是不透明白 {1,1,1,1}（core/dsl.h:133）——border-only 覆盖层（窗口环/描边框类）不显式 .color(透明)，一旦全窗尺寸+compose 末尾构建就整帧盖白只露边框；写 border-only rect 时先补 .color({0,0,0,0})，且别信「目检绿」：探针判据必须带非白占比下限（白底+细线 AA 能凑够颜色桶假绿）。
- 2026-11-04 need:122 终端模块：Swift↔C++ interop 三坑——闭包参数/自定义 struct 指针不上生成头（边界统一 void*+C 函数指针 trampoline）、failable init 变 swift::Optional、GLFW 不泵 GCD 主队列（唤醒主路径=generation 轮询，回调仅加速）；SwiftPM 产物路径会漂移禁硬编码（.build/out/Products 与 Modules 并存）；corta-exec 严格兄弟目录寻址无 env 兜底（部署须与主程序同级）。来源：池 clue #22-#25。
- 2026-11-07（池199）：ctest add_test 裸注册「需要子命令」的探针恒红——argless 打印用法退 2 会被守卫链当失败；QA 子命令驱动型探针（install 需网络/mcpserve 起服务）argless 必须退 77（SKIP_RETURN_CODE 跳过），子命令误用仍退 2。
- 2026-11-07（bug145 三轮误测复盘）：GL 帧回读类离屏探针读的是合成前 framebuffer，**结构性看不到窗口服务器的圆角裁剪/合成效果**——「探针 PASS」≠「合成态正确」（cee24491 探针绿但老板实测四角黑线全消）；涉及窗口形状/材质/圆角的验证必须真机目检，探针只证「画了」不证「可见」。
- 2026-11-07（need169）：EUI 元素 onClick/onKeyEvent 存 lambda 禁捕获 &BoardView（compose 栈上每帧临时，元素跨帧调用=UAF，无头用例实崩）——回调必须按值拷贝 std::function；普通列折叠泛化走 std::map<列id,状态> 与归档列同映射（colState 缺省按 kind 分口径），列宽用 boardColumnWidthsFlex 纯函数（Σ宽恒=span，回流无空洞靠构造不靠 clamp）。
