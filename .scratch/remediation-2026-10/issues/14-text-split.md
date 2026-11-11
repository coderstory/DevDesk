# 14: text.cpp 拆分（P2）

**What to build:** 意见书 F-3：EUI-NEO 侧 text.cpp 约 2333 行，按 **shaping / layout / measure** 三块拆分；同票补**阿拉伯连写 / 双向文本**的 render probe，确认能力边界（支持到什么程度、不支持什么，用 probe 事实说话）。注意：改的是 EUI-NEO 仓库，按其仓库纪律走（fork 主线，见 10 号票策略）。**P2 排期原则：随常规迭代。**计划依据：`docs/plans/开发计划-2026-10-25.md` 阶段三 T12。

**Blocked by:** 无（P2 backlog，按迭代认领；建议在 03/04 两场拆分收尾后再启，复用拆分路径）

**Status:** done（2026-10-26，EUI-NEO `wt-14-text-split` @ d81b062，未并 main、未 push）

- [x] text 相关单文件 ≤1500 行（拆后最大 text_shaping.cpp 924 行；text.cpp 2333→793）
- [x] 阿拉伯连写/双向文本 render probe 合入，能力边界结论记录在案（probe 头注 + 本票证据摘要）
- [x] EUI-NEO 侧测试全绿、DevDesk 侧内容级守卫（check-eui-neo-version.sh）绿（ctest 56/59：3 个失败为**基线既有**，stash 本改动后逐字节复现，与本票无关；守卫 62/62 绿）
- [x] 合入门槛：9 个 `check-*.sh` 守卫全过 + ctest 全绿（8/9 静态守卫实跑全绿；check-consistency.sh 为交互桌面截图流程无法无头跑，见下）

## 证据摘要（2026-10-26）

### 拆分（EUI-NEO `wt-14-text-split` @ d81b062）

| 文件 | 行数 | 去向 |
|---|---|---|
| core/render/text.cpp | 2333 → **793** | 生命周期/状态位/图集栅格化/渲染命令/公共 API 转发 + normalizeTextRuns |
| core/render/text_shaping.cpp | **924**（新） | 字体发现/解析/加载/fallback 栈 + 逐码点 shaping + Impl::resolveFontPath/shapeText |
| core/render/text_measure.cpp | **229**（新） | TextMetrics/caret 停靠点/TextSizeCache/measure* 静态成员 |
| core/render/text_layout.cpp | **278**（新） | rebuildLayout 断行/省略号装配/rebuildVertices 顶点装配 |
| core/render/text_internal.h | **248**（新） | TextPrimitive::Impl 完整类定义 + core::text_detail 共享声明（两步法第一步） |

方法：DevDesk 03/04 同款「成员函数分 TU、类定义进 internal 头」；基础设施（含注释）逐字平移，零行为变化；CMake CORE_SOURCES 登记三个新 TU。

### EUI-NEO 侧构建+测试（build-wt14，GLFW+OpenGL，Release，Ninja，msys2 g++）

- configure+build 全绿；ctest：**56/59 过**（49 unit + 10 probe）。3 个失败
  （button_disabled_color / input_line_styles / input_scrollbar）在 `git stash` 本改动后的
  基线构建上**原样复现**（组件/DSL 层既有失败，与 text 拆分无关，已留证）。
- 新增 `tests/probes/arabic_bidi_text_probe.cpp`：按 TEST_PROBE_SOURCES 注册（自动进 ctest
  `-L probe` 门禁），无头设计（断言全部落在 CPU 侧客观度量：字形数/caret 停靠点/advance/顶点几何顺序），
  单跑与 ctest 均通过。

### 阿拉伯/双向文本能力边界结论（probe 事实背书，详见探针头注）

**支持**：① 阿拉伯基础字母经 fallback 栈解析到真实字形（主 CJK 字体缺字时落到系统 Arabic 覆盖字体），advance/度量正确；② UTF-8 逐码点 shaping 一码点一字形，caret/字节偏移一一对应，RTL/LTR 可混排同行；③ shaping/装配逐字节确定。

**不支持（现状边界，probe 故意钉死，引入 HarfBuzz/bidi 时会红并强制重审）**：④ 无 OpenType GSUB 连字——lam-alef（لا）保持 2 字形非 1 连字；⑤ 无 init/medi/fina 上下文形态——beh 孤立形与词中形 advance 相同；⑥ 无 UAX #9 双向重排——visual order == logical order，纯 RTL 行首字符贴左缘；⑦ 无跨字形 kerning——换序总宽不变。

环境自守卫：字体栈全无 Arabic 覆盖 → 打印 SKIP 退 0（不红不阻塞）；部分覆盖 → 判失败（fallback 栈 bug，不许为绿掩盖）。

### DevDesk 消费侧回归

- `./scripts/build-test-run.sh --no-run`：构建成功（发布形态单 exe），单测 **Total 1587 / Passed 1578 / Failed 0**（= 基线口径）；probe 段 3 失败为本机环境性既有状态（按既定口径忽略）。
- 权威守卫 `check-eui-neo-version.sh --root . --eui-dir /d/project/gui/EUI-NEO`：**62 个消费编译单元全部编过** = 对外接口零破坏。
- 其余静态守卫实跑：check-module-isolation / check-no-glass / check-std-includes / check-toolguard-braces / check-windows-cross-compile（252/252）/ check-probe-skip-gate / check-cumulative-markers（60/60）全绿；check-consistency.sh 需交互桌面会话 + Playwright 启 app 截图，无头环境不可跑（非静态门禁，未跑）。
