# 调研报告：Windows/macOS 统一打包静态字体，消除文本宽度布局漂移

- 日期：2026-11-05
- 池条目：`[pool:need:132]`（新查证线索已入池：#44 OFL/字体候选、#45 FreeType 平台一致性、#46 许可与 VF 机制）
- 调研人：researcher；工具调用 32 次（预算 40 内），无构建
- 证据等级标注：【精读】=一手来源全文抓取核对；【实测】=gh api/本地文件实测数字；【扫描】=搜索结果摘要

## 结论先行（≤5 条）

1. **第 5 问（最关键）：能保证一致。** 前提是两端同一字体文件 + 同一 FreeType 源码构建——本项目已满足：FreeType 2.13.3 vendored 同源编译、编译开关两端完全相同（`EUI-NEO/3rd/dependencies.cmake:565-580`，线索 #40/#41）。官方 FAQ 明言平台差异唯一归因是「不同发行版编译选项不同」，本项目从源头排除了这一点。**统一字体可真正消除漂移。**
2. **字体首选 Noto Sans CJK SC（或同源思源黑体）静态 OTF**：OFL 1.1 明文允许随软件捆绑分发（含修改版）；静态版官方可得；单字重 15.7MB、子集化后可压到 2-4MB。**Google Fonts 上的「Noto Sans SC」只有 VF，静态版要从 `notofonts/noto-cjk` GitHub releases 拿**（08_NotoSansCJKsc.zip，2.004）。
3. **「VF 在 FreeType 加载不稳」的既有结论：结论成立、理由要修正**（见问 2）。静态字体仍是正确决策，因为消除了「坐标状态」这一整类不确定性，且零成本。
4. **子集化合法但必须改名**：OFL 下子集=Modified Version，不得再用 Reserved Font Name（思源/Noto 均声明 RFN），需附 OFL 文本+版权声明。
5. **两个残余风险**要关：① `FREETYPE_PROPERTIES` 环境变量可在用户机器上运行时改 hinting 引擎/解释器版本（官方文档确认），缓解=启动时 `FT_Property_Set` 显式锁定；② MSVC vs Clang 编译器差异理论上影响浮点路径，度量主路径纯定点、风险极低，但**必须 POC 两端 advance hash 逐字节比对**作为验收证据物。

---

## 问 1：字体候选（3-5 个，静态版、许可允许随应用分发）

| 字体 | 静态版 | 版本 | 下载来源 | License | 单字重大小 |
|---|---|---|---|---|---|
| **思源黑体 Source Han Sans SC** | OTF 全字重 | 2.005R（2025-06-18 发布，活跃） | GitHub `adobe-fonts/source-han-sans` releases，`09_SourceHanSansSC.zip`（95.2MB/7 字重）；区域子集版 `19_SourceHanSansCN.zip`（50.7MB） | SIL OFL 1.1（Adobe 声明 RFN） | ≈13.6MB（zip 均摊推算，待验）【实测 zip】 |
| **Noto Sans CJK SC**（与思源同源同设计） | OTF 全字重 | 2.004（2021-04，2022-01-27 打包发布，CJK 已稳定不再常更） | GitHub `notofonts/noto-cjk` releases，`08_NotoSansCJKsc.zip`（90.1MB/7 字重） | SIL OFL 1.1 | **15.7MB 实测**（NotoSansCJKsc-Regular.otf = 16,437,364B）【实测】 |
| **文泉驿微米黑** | ttc（Regular+Light 双字重） | 1.x（2015 后未更新，基于 Droid Sans Fallback） | wenq.org / Debian 包 / GitHub 镜像 `anthonyfok/fonts-wqy-microhei` | Apache-2.0 **或** GPLv3+字体嵌入例外（双许可，可任选 Apache 选项） | **4.9MB 实测**（wqy-microhei.ttc = 5,177,387B）【实测】 |
| **HarmonyOS Sans SC**（华为） | TTF 全字重 | 随官方下载页更新 | `developer.huawei.com/consumer/en/design/resource`（包内带 LICENSE.txt） | **自定义许可**（免费商用，非 OFL，AUR 归类 LicenseRef-custom）；字体文件再分发条款须逐条核对【扫描】 | 8.1-8.4MB 实测（镜像 `ajacocks/harmonyos-sans-font`）【实测】 |
| **MiSans**（小米）——**出局** | TTF | HyperOS 配套 | hyperos.mi.com/font | 免费商用**但明文禁止再分发字体文件本身**（"may not separately rent, sublicense, give, loan, or further distribute"）且 **revocable**；随应用捆绑=再分发 → 不合格【精读 FAQ+许可协议要点】 | — |

**「Noto Sans SC 是否只有 VF」专项答复**：Google Fonts 现在只发 VF 版 Noto Sans SC（Fontsource 也主推 VF）【扫描】；**静态版从 `notofonts/noto-cjk` GitHub releases 取**（`Sans2.004` tag → `08_NotoSansCJKsc.zip`，或仓内 `Sans/OTF/SimplifiedChinese/` 目录逐字重 OTF）。注意该 release 页自带警告：Windows 10/11 对 CFF2 VF 有 bug、建议 Windows 用 TTF——侧面证明 VF 有平台级坑，静态版是稳路。

## 问 2：VF vs 静态在 FreeType 上的真实差异（修正既有结论的理由）

机制【精读 ft2-multiple_masters.html】：

- VF 加载后**不调** `FT_Set_Var_Design_Coordinates` 时，FreeType 暴露的是**默认实例**（"the nth axis coordinate is equal to the nth default axis coordinate (i.e., axis[n].def)"）——这是**确定性状态，不是「不稳」**。
- 设置坐标后 outline 插值改变 → 度量（含 advance）随之改变；advance 是否随坐标变化还取决于字体是否带 HVAR/gvar 的 advance deltas（只有 hmtx 时坐标变了 advance 也不变）。
- **真正的风险**：measure 与 render 两条路径若有一处设置了坐标、另一处没有 → 度量分叉；以及 VF 在部分平台渲染栈有已知坑（上节 Windows CFF2 bug 警告）。

**结论**：`src/main.cpp:938` 的「② VF 加载不稳 → 测宽≠渲染」表述应修正为「VF 多一个坐标状态维度，两端/两路径状态一致性难保证」；**「换静态」的决策本身仍然成立**——静态字体以零成本消除这一整类问题，无需再审计两条路径的坐标状态。

## 问 3：TTF vs OTF(CFF) 的 hinting/反锯齿差异是否影响 advance

- FreeType 对 TTF 走 TrueType 引擎（bytecode interpreter v35/38/40 或 autofit），对 OTF/CFF 走 Adobe 引擎（默认）【扫描，两路搜索一致】。TTF bytecode 可修改 advance（MDRP/CVT round），CFF Adobe 引擎 hinting **不改** advance（用 hmtx 线性缩放值）。
- **对两端一致性：不影响。** 同一字体文件 + 同一 FreeType 构建 → 引擎行为逐位相同（见问 5）。它影响的是「绝对渲染质量与混排观感」，不是「两平台是否一致」。
- 选型建议：OTF(CFF) 静态版久经 FreeType Adobe 引擎考验；若走 TTF（如用 fonttools instancer 从 VF 转 static TTF），bytecode 路径的 advance 微调两端同样一致。统一字体后无系统字体混排，此差异无下游影响。

## 问 4：子集化（工具与许可）

- **工具**：pyftsubset（fonttools）成熟方案：`pyftsubset NotoSansCJKsc-Regular.otf --text-file=chars.txt --output-file=out.otf`，chars.txt 装 GB2312 常用 6763 汉字 + ASCII + 全半角标点 + 产品已知文案全字符；输出单字重约 2-4MB。GB18030 全集更大，按需分层。
- **许可（OFL 1.1 原文【精读】source-han-sans/LICENSE.txt）**：
  - "Modified Version" 定义 = "any derivative made by adding to, **deleting**, or substituting -- in part or in whole -- any of the components ... by changing formats or by porting" → **删字的子集 = 修改版**；
  - 条款 2：Original **或 Modified** Versions "may be bundled, redistributed and/or sold with any software"——**随软件捆绑分发明确允许**，条件是每份拷贝附版权声明 + 许可证文本；
  - 条款 3：Modified Version "**不得使用 Reserved Font Name**"，Adobe 已声明 RFN（"Copyright 2014-2025 Adobe, with Reserved Font [Name]"【实测原文】）→ **子集后必须改名**（如 "DevDesk Sans SC"），并保留 OFL 文本与版权声明随包分发。
- 文泉驿走 Apache-2.0 选项无改名义务；HarmonyOS 自定义许可对「修改+再分发」条款未确认（待验）。

## 问 5（最关键）：同一字体两端 FreeType 测宽能否逐像素一致

**结论：能，前提在本项目已成立。** 分层论证：

1. **官方 FAQ 定性**【精读 ft2faq.html】："Why does FreeType render differently on different platforms?" 的答案只有一个原因：**不同发行版用不同编译选项**（ftoption.h 差异：hinter 开关、bytecode interpreter、SIMD 等）。不存在「macOS 版 FreeType 天生不同」。
2. **本项目从源头排除该原因**：FreeType 是 vendored 同一 tarball（`EUI-NEO/3rd/dependencies.cmake:574-580`，freetype-2.13.3.tar.xz，`add_subdirectory` 源码构建，非系统库、非 find_package），编译开关两端完全一致（`:565-570`：FT_DISABLE_ZLIB/BZIP2/HARFBUZZ=ON、BROTLI=ON 等，线索 #40/#41）。macOS/Windows 均不自带 FreeType，app 静态链接自带那份 → **不存在系统覆盖**。
3. **度量路径确定性**：advance = hmtx 字体单位 × FT_MulDiv 定点缩放（26.6 格式），当前加载 flag `FT_LOAD_DEFAULT|FT_LOAD_COLOR|FT_LOAD_NO_SVG|FT_LOAD_TARGET_LIGHT`（`EUI-NEO/core/render/text_internal.h:30`）为定点/整数运算，度量主路径不依赖平台相关浮点行为 → 同一二进制行为跨 x86_64/arm64 一致。
4. **测宽=渲染同源**（既有已证事实，线索 #39）：measureTextWidth 与渲染共用 shapeTextWithFontStack → 两端字体一致 ⇒ 测宽一致且等于渲染宽度 → **漂移消除成立**。
5. **残余风险（不能 100% 文档排除的部分）**：
   - MSVC vs Clang 优化差异理论上影响浮点边界情况——度量主路径纯定点，风险极低但非零；
   - **`FREETYPE_PROPERTIES` 环境变量**【精读 ft2-properties.html】：官方文档确认可在运行时改 CFF hinting-engine（adobe/freetype）、TrueType interpreter-version（35/38/40）等 → 若用户机器设了不同值会引入分叉。**缓解：app 字体初始化时用 `FT_Property_Set` 显式锁定这些属性**（代码内默认值已相同，显式设置是防御性收敛）。
   - **验收口径**：两端各跑同一测试字形集（全子集字符 × 2-3 个字号），序列化 advance 做 hash 逐字节比对——这是 POC 证据物，完成定义要求。

## 问 6：兜底做法（若问 5 存在残差）

按兜底强度递增：

1. **`FT_LOAD_NO_HINTING`**【精读 ft2-glyph_retrieval.html】：不做 grid-fitting，advance = 纯线性缩放（linearHoriAdvance，16.16 定点），完全独立于 hinting 引擎 → 绝对确定性。代价：官方文档明言抗锯齿模式下 "blurrier"（渲染略糊）。注意**渲染端要同步改**，否则测宽≠渲染（回到漂移）。
2. **直接读字体表 advance**：`FT_Load_Glyph(..., FT_LOAD_NO_SCALE)` 拿字体单位 advance（或解析 hmtx），自行 `FT_MulDiv(advance, ppem, unitsPerEm)` 缩放——纯整数运算，与任何 hinting/引擎无关，最彻底。
3. **布局改用 linearHoriAdvance**（16.16 未 hint 值）代替取整后的 `slot->advance`，配合子像素定位——官方文档称 linearHoriAdvance 是 "important to perform correct WYSIWYG layout" 的值。

当前实现（TARGET_LIGHT + slot->advance 整数像素）在「同构建同字体」前提下已确定，**不必现在就改**；兜底只在 POC 比对发现残差时启用。

---

## 对比表（选型面）

| 维度 | Noto Sans CJK SC | 思源黑体 SC | 文泉驿微米黑 | HarmonyOS Sans SC | MiSans |
|---|---|---|---|---|---|
| 静态版可得 | ✅ releases 2.004 | ✅ releases 2.005R | ✅ ttc | ✅ TTF | ✅（但许可出局） |
| 许可随应用分发 | ✅ OFL 1.1 明文允许 | ✅ 同左 | ✅ Apache-2.0 选项 | ⚠️ 自定义许可待核 | ❌ 禁止再分发 |
| 活跃度 | CJK 已稳定（2022） | 活跃（2025-06） | 停更（2015） | 活跃 | 活跃 |
| 引入价值 | 高（全字重、质量高、与思源同源） | 同左 | 低（字重弱、停更） | 中（美观但许可风险） | — |
| 功能利用率（本项目） | Regular+Medium 2 字重够用 | 同左 | 双字重但质量弱 | 多字重 | — |
| 子集化 | ✅ 2-4MB/字重 | ✅ 同左 | 已小(4.9MB)无需 | 未验证 | — |
| 集成可行性 | 高（替换 assets + setDefaultFontFiles 即可） | 高 | 高 | 中（许可核对） | 不可 |
| 链接 | [noto-cjk releases](https://github.com/notofonts/noto-cjk/releases) | [source-han-sans releases](https://github.com/adobe-fonts/source-han-sans/releases) | [wenq.org](http://wenq.org) | [Huawei Design Resources](https://developer.huawei.com/consumer/en/design/resource) | [MiSans](https://hyperos.mi.com/font/en/download) |

## 推荐方案

1. **字体**：Noto Sans CJK SC 2.004 静态 OTF（`NotoSansCJKsc-Regular.otf` 15.7MB + `Medium.otf` 16.5MB，或含 DemiLight 按需）；追求更新版本选思源黑体 2.005R（同源同设计，二选一即可，勿混）。
2. **子集化**：pyftsubset 按 GB2312 6763 汉字 + ASCII + 标点 + 产品全量文案字符；**子集后改名**（避开 RFN，如 "DevDesk Sans SC"），附 OFL.txt + 版权声明入 `EUI-NEO/assets/`。
3. **接线**：`setDefaultFontFiles` 指向打包静态字体（系统 CJK 解析路径降级为兜底）；删除 `NotoSansSC-VF.ttf`（17.8MB，正是问 2 风险源）；同步调整 `src/main.cpp:938` 注释里的理由表述。
4. **防御**：字体初始化时 `FT_Property_Set` 显式锁定 hinting 引擎与 interpreter 版本（对冲 FREETYPE_PROPERTIES 环境变量）。
5. **POC 验收**（完成定义=证据物）：同一字形集 × 2-3 字号，两端 advance hash 逐字节比对 + 截图对照。
6. **另立卡片建议**（清单外，仅建议）：等宽字体漂移（线索 #38，CascadiaMono vs SFNSMono）同法打包 OFL 等宽字体（如 JetBrains Mono），不在本卡执行。

## 风险清单

| 风险 | 等级 | 对策 |
|---|---|---|
| 用户机器 `FREETYPE_PROPERTIES` 环境变量改 hinting/interpreter | 中 | 启动时 FT_Property_Set 显式锁定（已列推荐方案 4） |
| 编译器（MSVC/Clang）浮点边界差异 | 低（主路径定点） | POC hash 比对兜住；残差则 NO_HINTING/hmtx 兜底 |
| 子集遗漏生僻字 → 回退系统字体 → 漂移回归 | 中 | 字符集用产品全量文案 + GB2312；回退仅限冷僻字并可接受 |
| OFL RFN 违规（子集未改名） | 法务 | 改名 + 附许可文本，流程写死 |
| HarmonyOS Sans 自定义许可不确定性 | 中 | 若作备选须先核 LICENSE.txt；默认不选 |
| Windows CFF2 渲染坑（release 页警告） | 低 | 用静态 OTF（非 VF）避开；POC 覆盖 |

## 来源清单

精读（一手全文抓取）：
- FreeType FAQ：https://freetype.org/freetype2/docs/ft2faq.html（平台差异归因）
- FreeType ft2-glyph_retrieval.html（load flags / advance 取整 / linearHoriAdvance / NO_HINTING）
- FreeType ft2-multiple_masters.html（VF 默认实例 / 坐标 / 命名实例）
- FreeType ft2-properties.html（FREETYPE_PROPERTIES 运行时属性）
- FreeType glyphs-3.html（advance 概念 / light hinting 不动横向）
- OFL 1.1 原文（adobe-fonts/source-han-sans LICENSE.txt：Modified Version 定义 / 捆绑条款 / RFN 条款）

实测（精确数字）：
- GitHub API：noto-cjk `Sans2.004` assets（08_NotoSansCJKsc.zip=94,523,633B；18_NotoSansSC.zip=50,077,940B）；source-han-sans `2.005R` assets（09_SourceHanSansSC.zip=95,177,106B）；noto-cjk `Sans/OTF/SimplifiedChinese` 单字重 OTF（Regular=16,437,364B）；wqy-microhei.ttc=5,177,387B；HarmonyOS_Sans_SC_*.ttf≈8.1-8.4MB；本地 `EUI-NEO/assets/NotoSansSC-VF.ttf`=17,773,244B

扫描（二手，仅作线索）：
- Google Fonts/Fontsource 只发 Noto Sans SC VF；HarmonyOS Sans 许可定性（AUR LicenseRef-custom、appshotmaker）；MiSans 许可要点（hyperos.mi.com FAQ + 许可协议 PDF）；文泉驿双许可（wenq.org/Debian/SourceForge 一致）

仓内（已采信不重查）：线索 #34-#41 + 本轮定位 `text_internal.h:30`、`dependencies.cmake:565-580`
