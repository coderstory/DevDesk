# 网页正文主干提取（boilerplate removal）调研与最小规则集

- 日期：2026-10-27（检索核对日）
- 类型：一次性课题（实现参考型调研）
- 服务对象：`src/module_mcp/html_to_markdown.cpp`（D:\wt-mcp-skeleton，git HEAD `6f130768`，885 行）
- 触发问题：抓 OSChina / bigmodel 文档页时，输出的 Markdown 里混着导航栏、页脚、面包屑等一大堆无意义链接
- 阅读对象：下一批实现本功能的 builder + 拍板的用户

---

## 0. 一句话结论

业界成熟做法（Mozilla Readability.js、trafilatura、jusText、boilerpipe）收敛在三类信号上：**语义标签与类名关键词**、**链接密度**（一段文字里链接文字占的比例）、**块文本量**。我们的单趟扫描器做不了 Readability 那种"整棵 DOM 树打分选最像正文的大块"，但前两类信号**逐标签、逐块就能判**，移植规则即可解决用户反馈的问题。推荐规则集：R1 语义标签整块剔除 + R1b 类名/ID 关键词剔除（必做）→ R2 小块高链接密度剔除（必做）→ R3 主容器锚定保护（建议）→ R4 无障碍属性（可选）。所有阈值取业界收敛区间的保守端，保证"规则全不命中时输出与现在完全一致"。

---

## 1. 问题与我们的约束

### 1.1 现状（读码结论，HEAD `6f130768`）

- 单趟流式扫描器：从前往后逐字符扫，遇到开标签查 `handleOpen(name, attrs)`、闭标签查 `handleClose(name)`、文本进 `emitTextRun`。**没有 DOM 树**——不知道"当前节点的父节点、兄弟节点是什么"。
- 但有三个现成的抓手（本次调研方案全部建立在其上）：
  1. **属性已经拿到了**：`parseOpenTag` 把 class / id / role / aria-* 全解析进 `attrs` 传给 `handleOpen`，判关键词零额外解析成本；
  2. **行模型天然分块**：正文文本攒在 `inline_`，块级标签（p/div/li…）触发 `flushBlock()` → `flushInlineLines()` 把整块文本写进 `lines_`。"块"这个粒度现成存在，正好是链接密度的计算单元；
  3. **模式机已有多级丢弃先例**：`Mode::RawSkip`（script/style 内容整体吞掉）、`Mode::Head`（head 区只留 title）、`Mode::Pre`（pre 保原文）——再加一级 `Mode::Suppress`（渲染性标签整体丢弃）与现有结构同构。
- 输入上限 2 MiB（`kMaxInputBytes` 截断），全部在内存里——将来真需要"第二遍扫"也有退路，但下面的方案不需要。

### 1.2 术语（就地解释）

- **boilerplate（样板文字）**：每个页面长得差不多的那部分——导航、页脚、版权行、广告位。它是"网页的皮肤"，不是"网页的内容"。
- **链接密度（link density）**：一段文字里，包在 `<a>` 里的文字占整段文字的比例。导航栏几乎全是链接，比例接近 1；正文段落里偶尔一两个链接，比例通常低于 0.25。这个比值与语言无关（分子分母同涨同跌）。
- **主容器锚定**：先找到"正文住哪个盒子"（`<main>`、`<article>` 或 class/id 含 main-content 之类的块），只在盒子里找内容。

---

## 2. 业界算法拆解（一手来源）

### 2.1 Readability.js（Mozilla，Firefox 阅读模式内核）

来源等级：官方仓库源码直读（`Readability.js`，main 分支，Apache-2.0；README 确认"powers Firefox Reader View"；仓库最近提交 2026-07-09）。以下全部为逐字核对。

**第一步：unlikelyCandidates 预剔除**（对象 = class+id 拼接字符串）：

```
unlikelyCandidates = /-ad-|ai2html|banner|breadcrumbs|combx|comment|community|
  cover-wrap|disqus|extra|footer|gdpr|header|legends|menu|related|remark|
  replies|rss|shoutbox|sidebar|skyscraper|social|sponsor|supplemental|
  ad-break|agegate|pagination|pager|popup|yom-remote/i
okMaybeItsACandidate = /and|article|body|column|content|main|mathjax|shadow/i
```

剔除条件链（逐字核对）——**五道保险丝**，比"命中关键词就删"谨慎得多：

```js
unlikelyCandidates.test(matchString)       // 1. 命中负面关键词
&& !okMaybeItsACandidate.test(matchString) // 2. 但同时含正面词 → 保留（词表冲突时存疑不删）
&& !_hasAncestorTag(node, "table")         // 3. 表格里不删
&& !_hasAncestorTag(node, "code")          // 4. 代码里不删
&& tagName !== "BODY" && tagName !== "A"   // 5. 整页 / 单个链接不删
```

另有 `UNLIKELY_ROLES = ["menu","menubar","complementary","navigation","alert","alertdialog","dialog"]`（按 role 属性删）、`aria-modal=true && role=dialog` 无条件删。

**第二步：DOM 树打分选主干**（我们没有 DOM，整体不移植，仅记录思想供对照）：

- 按标签初始化分：DIV +5；PRE/TD/BLOCKQUOTE +3；ADDRESS/OL/UL/DL/DD/DT/LI/FORM −3；H1–H6/TH −5；
- class/id 命中 `negative` 正则 −25、命中 `positive = /article|body|content|entry|hentry|h-entry|main|page|pagination|post|text|blog|story/i` +25（class 与 id 各算一遍）；
- 每个段落基础 +1 分，**每个逗号 +1 分**，每 100 字符再 +1 分（封顶 +3）；祖先节点按"本节点 / 父 / 祖父"拿全额 / 一半 / 三分之一——这就是"祖父节点提升"；
- 候选分最终乘以 `(1 − 链接密度)`——链接越多分越低；
- 选出最高分节点后：其兄弟节点若得分超过 `max(10, 主干分 × 0.2)` 就"回捞"进正文（兄弟节点补捞）；段落保留条件 = 长度 > 80 且链接密度 < 0.25，或长度 < 80 但链接密度为 0 且以句号结尾。

**第三步：_cleanConditionally 分类清扫**（链接密度阈值的直接出处）：

- 保险丝：数据表格内不动、`<code>` 祖先内不动、`类名权重 + 内容分 < 0` 不动、逗号 ≥ 10 的块不动（长文当正文保护）；
- **删除条件（逐字核对）：类名权重 < 25 且链接密度 > 0.2+修饰量 → 删；权重 ≥ 25 且链接密度 > 0.5+修饰量 → 删**。修饰量 `linkDensityModifier` 默认 0，是对外暴露的调节旋钮（README："a number that is added to the base link density threshold"）；
- 广告词正则（`ad|广告|Реклама|Anuncio…`）与"loading"占位词命中直接删。

**链接密度的算法（逐字核对 `_getLinkDensity`）**：链接文字长度求和 ÷ 全部文字长度；**锚链接（`href="#..."` 页内跳转）的文字按 0.3 倍重量计**——这是官方对"目录/页内导航不算垃圾"的让步，下文 R2 直接沿用。

**CJK 逗号问题（专项核实，坊间说法已过时一半）**：现行 `REGEXPS.commas` 逐字为
`/\u002C|\u060C|\uFE50|\uFE10|\uFE11|\u2E41|\u2E34|\u2E32|\uFF0C/g`——**全角逗号 U+FF0C（，）已在内**，2023-04-25 由 PR #796（commit `726381c`，"Added comma regular expressions"）加入，同时覆盖阿拉伯逗号、竖排标点。但**顿号 U+3001（、）与句号 U+3002（。仍不在词表**；每 100 字符加分项与语言无关。结论：Readability 对中文的"逗号加分"缺陷已部分修复、未根治，但这不构成我们不移植的理由——**我们的最小规则集根本不用逗号特征**（见 §4.3）。

### 2.2 trafilatura（Python，规则 + 算法混合）

来源等级：官方文档基准页 + 官方仓库源码（xpaths.py，master）。

**规则差异（相对 Readability）**：trafilatura 反过来走——**先正着找正文盒子（BODY_XPATH），找不到才全文兜底**；找盒子时按 id/class 词表匹配 `article` 系、`story` 系、`main-content` / `page-content` 系、以及 class/id/role 以 `main` 开头或 `<main>` 标签本身。剔除侧（OVERALL_DISCARD_XPATH）按 class 词表删：`navigation/navbar/navbox/menu/bar`、`-ad- / dfp / outbrain / taboola / criteo / widget`、`footer`、`byline`、`share-/sociable/embed`、`tag-list`、`consent`、`most-popular/-stories/next-/more-on`、UI 杂项（`meta/rating/timestamp/user-info/-icon/overlay`…）；属性侧删 `role=nav`、`aria-hidden="true"`、`display:none` 样式。精度模式额外删 `.//header`。可见它比 Readability 多吃两类东西：**aria 属性**和**更细的 class 词表**。

**基准结论（官方评测页，2026-10-02 更新，990 个文档，Python 3.13）**：

| 工具 | 精确率 | 召回率 | F1 |
|---|---|---|---|
| html2text 2025.4.15 | 0.525 | 0.900 | 0.663 |
| 原样 HTML | 0.528 | 0.906 | 0.667 |
| boilerpy3（boilerpipe 的 Python 版，article 模式） | 0.818 | 0.796 | 0.807 |
| readability-lxml 0.9（Readability 的 Python 移植，注意非 Mozilla JS 版） | 0.892 | 0.817 | 0.853 |
| justext 3.0.2 | 0.864 | 0.859 | 0.862 |
| magic-html 0.1.8 | 0.887 | 0.891 | 0.889 |
| **trafilatura 2.3.0（standard）** | 0.906 | 0.946 | **0.926** |

第三方评测（页面引用 Bevendorff et al. 2023《An Empirical Comparison of Web Content Extraction Algorithms》）同样排 trafilatura 第一。**对 Readability 的优势场景**：页面没有明显 `<article>` 语义、但 class 命名规范的现代站点（这正是国内站点常态），以及 aria/隐藏元素处理。页面原话："Rule-based approaches … obtain balanced results despite a lack of precision"，与算法手段结合后"perform significantly better than the other tested solutions"。

### 2.3 jusText（段落三分类 + 上下文回捞）

来源等级：官方仓库源码（miso-belica/jusText，core.py，master；BSD-2；GitHub API 显示最近推送 2026-10-21，828 stars——维护非常活跃）。

**思想**：把每个段落标记为 good / near-good / bad，再按"邻居是谁"修正。默认参数（逐字核对）：

```
MAX_LINK_DENSITY = 0.2   # 链接密度超过它 → 直接 bad（不可救）
LENGTH_LOW  = 70         # 短段落界（字符数）
LENGTH_HIGH = 200        # 长段落界
STOPWORDS_LOW/HIGH = 0.30 / 0.32  # 停用词密度界（常用词占比）
```

分类顺序：链接密度超限 → bad；含版权符号 → bad；DOM 路径含 `select` → bad；短于 70 字符且含链接文字 → bad，否则标 short；停用词密度高 → 长段落 good / 短段落 neargood；……最后上下文修正：夹在两个 good 之间的 short 变 good，两个 bad 之间的变 bad，后面 200 字符内跟着正文的短标题可"获救"。**这些阈值给了我们两个依据**：链接密度的激进界（0.2）与"短块"的业界取值（70 字符）。

**CJK 硬伤（核实）**：jusText 靠停用词表打分，官方自带约 100 种语言的停用词表，**没有中文**（目录里有 Korean，无 zh/cn）——停用词系方案对中文开箱即废。这是它基准 F1 落后 trafilatura 0.064 的原因之一，也再次指向：**我们的规则必须只用语言无关特征**。

### 2.4 boilerpipe（文本块分类器的源头）

来源等级：官方仓库源码（kohlschutter/boilerpipe，master；LICENSE 逐字确认为 Apache-2.0，"Copyright (c) 2009, 2014 Christian Kohlschütter"）。

**思想**：把页面切成文本块（TextBlock），每块记录两个"浅层特征"——**每块词数**与**链接密度**——然后用论文《Boilerplate Detection using Shallow Text Features》（WSDM 2010）里 C4.8 决策树（一种自动学决策规则的经典算法）学出的规则分类"内容 / 样板"。两个分类器的实际规则（源码逐字核对）：

- `NumWordsRulesClassifier`：**当前块链接密度 > 0.333333 → 直接不是内容**（无条件）；否则按前块链接密度 ≤ 0.555556 与否，套"当前块 > 16 词 / 后块 > 15 词 / 前块 > 4 词"或"当前块 > 40 词 / 后块 > 17 词"的分支判内容；
- `DensityRulesClassifier`：同样是"链接密度 ≤ 0.333333 才有资格当内容"，再叠文字密度（9/10/4/11）分支。

**对 R2 的意义**：机器学出来的决策边界也在 0.33 上下——业界对"链接占比超过 1/3 就基本不是正文"高度一致。外围机制（KeepLargestBlockFilter 最大块保留、BlockProximityFusion 相邻块融合、BoilerplateBlockFilter 兜底过滤）都依赖全页块序列，单趟扫描器不移植。

### 2.5 html2text 类（对照项：只有"开关"没有"判断"）

来源等级：PyPI 包元数据（JSON API）。python html2text v2025.4.15，**GPL-3.0-or-later**（license_expression 逐字核对——**与本仓不兼容，只可参考行为、不可抄码**），提供 `--ignore-links`（"Don't include any formatting for links"）/ `ignore_images` 等开关。它回答的是"链接要不要渲染成 Markdown 链接"，不回答"这块是不是正文"——与我们课题正交，列此仅作边界对照：**我们的问题必须靠内容判断解决，格式开关救不了**。

### 2.6 候选对比表

| 候选 | 成熟度 | License | 维护活跃度 | 我们能拿什么 |
|---|---|---|---|---|
| Readability.js（Mozilla） | Firefox 阅读模式内核 | Apache-2.0（兼容） | 最近提交 2026-07-09 | R1/R1b 全部词表与保险丝顺序、R2 的 0.5/0.2/0.3(锚链接) 阈值、容错思想 |
| trafilatura | 基准第一（F1 0.926） | Apache-2.0 | 官方评测 2026-10-02 仍在更新 | R3 正向锚定词表（main-content/page-content…）、R4 aria-hidden/display:none/role 词表 |
| jusText | 基准 F1 0.862 | BSD-2（兼容） | 最近推送 2026-10-21 | "短块"阈值依据（70 字符）、上下文回捞思想（我们简化为 R3 锚定） |
| boilerpipe | 学术源头，基准 F1 0.807 | Apache-2.0（兼容） | 仓库长期维护 | R2 的 0.333 学界决策边界佐证、文本块两特征（词数+链接密度） |
| python html2text | 老牌转换器 | **GPL-3.0-or-later（不兼容）** | 活跃（2025.4.15） | 不可抄码；仅佐证"格式开关≠内容判断" |

**复用阶梯判定**：①直接复用/封装——否（四者全是 Python/Java/JS，本仓 C++20 单文件零依赖约束，且都需要完整 DOM）；②移植——**是，移植规则与阈值，不移植代码**：本方案逐条对照上述源码的词表/阈值/保险丝顺序忠实移植，引用文件已在 §2 标注；③自研——仅"单趟无 DOM 下的执行机制"（Suppress 模式、块计数器）确无现成单趟实现可抄，属最小必要自研。防重复造轮子四问均已覆盖：现成实现（上表）、license（三兼容一不兼容，均不抄码）、维护度（上表）、直接用还是抄机制（抄机制）。

---

## 3. 给我们的最小规则集（按优先级分级）

设计总原则（从 §2 归纳）：**只用语言无关特征**（标签语义、class/id/role 关键词、链接密度、块字符数）——不用逗号数（CJK 缺陷）、不用停用词（中文无表）；**每条删除规则都带保险丝**（存疑不删）；**所有规则可整体关断，关断时输出与现状逐字节一致**（保底不劣化）。

### R1（必做）语义标签整块剔除

**依据**：Readability 虽不按 `<header>/<footer>/<nav>` 标签整删（它删的是 class），但 trafilatura 精度模式显式删 `.//header`，且 HTML5 语义里这些标签就是"页面级导航/页脚"；这类标签在 OSChina/bigmodel 类页面覆盖率最高、误伤率最低。

**伪代码**（挂在现有模式机上）：

```
kSuppressTags = { header, footer, nav, aside, form, noscript, iframe,
                  svg, template, dialog, button, select }
    # svg 必须在列：<svg><text> 的文字会漏进正文
    # form/button/select：登录框/搜索框/下拉框，Markdown 无意义

handleOpen(name, attrs):
  若 name 属于 kSuppressTags:
    若 mode_ 为 Pre 或 cellOpen_:  return        # 保险丝：表格单元格内/pre 内不启动
                                                 # （对应 Readability 的 table/code 祖先保险丝）
    flushBlock()                                  # 先把当前块落盘，行模型不脏
    mode_ = Suppress; suppressStack_.push(name)
    suppressBytes_ = 0
    return
  若 mode_ == Suppress:
    若 name 属于 kSuppressTags: suppressStack_.push(name)   # 嵌套抑制：只计数
    return                                        # 抑制中其他标签照常，不产出

handleClose(name):
  若 mode_ == Suppress:
    若 name 在 suppressStack_ 中:
      弹栈至该 name（含）——乱序闭合（<header><div></header>）一次弹到匹配层
      若栈空: mode_ = Normal
    return
```

**未闭合标签的深度回退策略**（必须有，否则一个没闭合的 `<header>` 会吞掉半篇正文）——双保险：

1. **内容信号触发释放**：Suppress 中遇到 `<article>`、`<main>`、`<h1>`/`<h2>`/`<h3>` 开标签 → 认定抑制起点是残缺标签，清空 suppressStack_、mode_=Normal，该标签照常处理。依据：trafilatura 的 RAW_TREE_PRUNE 对"含 h1 的节点网开一面"同思路；正文开头几乎必有标题。
2. **字节上限硬释放**：suppressBytes_ > 64 KiB（约 2 MiB 输入上限的 3%）→ 释放恢复 Normal。数值为本仓自定保守值（业界无对应单趟参数），实现时随 fixture 调。

**预期效果**：文档页——顶部导航/底部页脚/侧栏整体消失；列表页——站头站尾消失，新闻列表本身不受影响（列表在 `<main>`/内容区，不在上述标签里）。中文页面无语言依赖。

### R1b（必做，与 R1 同机制）class/id 关键词剔除

**依据**：Readability unlikelyCandidates 正则逐字移植（词表见 §2.1），`okMaybeItsACandidate` 反向保险丝一并移植；trafilatura OVERALL_DISCARD 词表补充国内站点常见命名。

```
kUnlikelyTokens = { footer, header, nav, menu, sidebar, banner, breadcrumb,
                    comment, related, share, social, sponsor, promo, ads,
                    advert, pagination, pager, widget, copyright, newsletter,
                    subscribe, cookie, gdpr, drawer, toolbar }
      # 取 Readability 词表的高置信子集；去掉易误伤项（extra, legends,
      # media, meta, text... 在其 negative 表里但太泛，只适合打分不适合整删）
kMaybeTokens    = { article, content, main, post, entry, body, column,
                    story, docs, doc, markdown, readme, code, highlight, snippet }
      # 前半来自 Readability okMaybeItsACandidate/positive 词表；
      # docs/code/highlight/snippet 为本仓扩展：文档站正文常包在
      # .doc-content / .code-highlight 里，词表冲突时存疑不删

handleOpen(div|section|ul|ol|table|p, attrs):
  s = lowerAscii(class + " " + id)
  若 s 含任一 kMaybeTokens: return                  # 保险丝 1：正词在身，不删
  若 cellOpen_ 或 mode_ 非 Normal: return           # 保险丝 2：表格/抑制中不启动
  若 s 含任一 kUnlikelyTokens:
    启动 Suppress（同 R1 机制，日志记命中词）
```

**与 Readability 的差异声明**：它对 class 命中节点先删、误伤了再靠 DOM 打分找回；我们无打分环节，故（a）词表只取高置信子集，（b）正词保险丝前置，（c）**绝不删 `<a>`/`<body>` 本身、表格单元格内不启动**（后两者逐字对应其源码保险丝）。误伤率用 fixture 集（§5）回归验证。

**预期效果**：语义标签缺失但类名规范的老页面（`<div class="nav-menu">`、`<div id="footer">`）被覆盖；`article/content/main` 命中的正文容器零误伤。

### R2（必做）链接密度分块剔除

**依据**：阈值业界收敛区间 0.2–0.5——Readability `_cleanConditionally` 低权重 0.2+ / 高权重 0.5+（源码逐字）；boilerpipe C4.8 学得边界 0.333；jusText 默认 0.2。**我们取 0.5（保守端）**：我们没有"类名权重 ≥ 25"这种加分上下文来支撑 Readability 的激进分支，且 OSChina 列表页的列表项本身高链接，取保守端才能配合 R3 保住列表（见 §4.2）。

**块长度上限 150 字符**：jusText 短段落界 70、Readability 段落保留线 80，导航项/面包屑/页脚链接行几乎都在 100 字符内，正文段落极少短于 150——150 为本仓取值（经验值，fixture 校准），标注可调。

```
成员: blockChars_ = 0; blockLinkChars_ = 0; linkDepth_ = 0; blockHasVisual_ = false

appendText(raw):            # 现有函数，Normal 模式唯一文本入口，天然是计数漏斗
  解码/转义后逐字符追加 inline_ 的同时:
    blockChars_ += 字节数
    若 linkDepth_ > 0: blockLinkChars_ += 字节数

handleOpen(a):  ++linkDepth_                # 计数用深度，不用现有单槽 href_
handleClose(a): 若 linkDepth_>0: --linkDepth_
handleOpen(img): blockHasVisual_ = true     # 保险丝：含图块不判（截图链接是正文）

flushInlineLines():                          # 现有函数，块 = 分割决策点
  body = move(inline_)                        # 先取走，决策不污染输出
  密度 = blockChars_ > 0 ? blockLinkChars_ / blockChars_ : 0
  若 非(标题块/引用块) 且 blockChars_ ≤ 150 且 密度 > 0.5
     且 blockHasVisual_ == false:
       丢弃 body（不入 lines_）
       重置 blockChars_/blockLinkChars_/linkDepth_/inCodeSpan_/href_
         # 交互点①：inCodeSpan_/href_ 必须随块复位，否则半截 code span
         # 会把后续正文带成不转义
       若 该块属于有序列表项: lists_.back().next--   # 交互点②：防编号跳号
       pushBlockBreak(); return
  …（原有逐行 push 逻辑不动）
```

**锚链接 0.3 系数（建议一并移植）**：`href="#..."` 的链接文字按 0.3 倍计入 blockLinkChars_（逐字对应 Readability `_getLinkDensity` 的 hashUrl 系数）。效果：文档页右侧目录（TOC，全是 `#锚点` 链接）密度降到 0.3 以下自动存活——目录对 Markdown 笔记是有用信息，删了可惜。

**对两类样本的预期**：文档页——顶部标签栏/面包屑/页脚链接行（小块、密度接近 1）全灭，TOC 因锚系数存活；列表页——OSChina 新闻列表每项 = 标题链接（30–60 字）+ 摘要纯文本（60–120 字），单块密度通常 0.3–0.5 **低于阈值**，列表存活；页脚"友情链接/关于我们"行（密度 1、100 字符内）被删。

### R3（建议）主容器锚定保护

**依据**：trafilatura BODY_XPATH 的正向找盒子（article/main/`main-content`/`page-content`/role 或 class 以 main 开头——源码逐字核对）；Readability positive 正则同级词表。我们做**降配版**：只做"锚定保护"，不做打分选盒。

```
handleOpen(main|article, attrs)
  （含 role=main / class|id 命中 kMaybeTokens 的 div）:
  进入锚定状态，锚点入栈
锚定期间:
  ① R2 剔除暂停（列表页的 <main> 就是列表本身——主干即链接列表，
     密度判据在这里天然失义，必须豁免）
  ② 若 Suppress 激活且锚点出现 → 触发 R1 的"内容信号释放"（双保险第二支）
  ③ 锚定区内出现的链接密度只记数不删（留观测，为将来调参）
```

**为何是"建议"不是"必做"**：R1+R2 已能解决用户报告的两种页面；R3 防的是"正文区恰好套了 nav 字样类名"之类的长尾误伤，收益要靠 fixture 长尾验证，故放第二批。

**预期效果**：列表页在"站点 class 里带 nav/menu 字样的容器包住新闻列表"这种不幸命名下仍能保住列表；文档页正文区被关键词误命中时自愈。

### R4（可选）无障碍属性与隐藏惯例

**依据**：trafilatura OVERALL_DISCARD_XPATH 逐字含 `@aria-hidden='true'`、`display:none` 样式；Readability `UNLIKELY_ROLES = ["menu","menubar","complementary","navigation","alert","alertdialog","dialog"]` 逐字（注意其表**不含** banner/contentinfo——header/footer 标签已由 R1 覆盖）。

```
handleOpen 任意标签（R1b 保险丝照常适用）:
  若 aria-hidden="true" 或 属性 hidden 存在 → Suppress（同机制）
  若 style 属性含 "display:none" / "visibility:hidden" → Suppress（子串匹配即可，不必解析样式）
  若 role 属于 { menu, menubar, complementary, navigation, alert,
                 alertdialog, dialog, banner, contentinfo, search } → Suppress
      # banner/contentinfo/search 为本仓在 Readability 表上的扩展，
      # 语义对应 W3C 无障碍角色里的页眉/页脚/搜索区（HTML5 header/footer 的等价物）
```

放可选的原因：国内站点 aria 使用率低于欧美，收益密度低；但实现成本约等于 R1 的零头（attrs 已在手上），建议实现时顺手带上，单独开关。

---

## 4. 三个专项适配

### 4.1 中文页面（CJK）

| 业界特征 | 中文可用性 | 我们的取舍 |
|---|---|---|
| 逗号计数加分（Readability） | 2023-04 起已认全角逗号（，），但顿号（、）、句号（。）仍不认（源码+commit 核实） | **不用**。规则集不含逗号特征，问题整体绕开 |
| 停用词密度（jusText / boilerpipe 旁支） | 官方约 100 语言停用词表无中文（目录核实）；boilerpipe 分类器按英文词训练 | **不用**。语言无关是硬约束 |
| 链接密度（比值特征） | 分子分母同时受 UTF-8 膨胀影响，比值不变——**天然语言无关** | **核心特征**（R2） |
| 标签/类名/role 语义 | HTML5 语义全球通用；国内站点 class 命名多为英文 | **核心特征**（R1/R1b/R4） |
| 块字符数 | 字符数对中文反而更"密"（同字符数信息量更高），只用作上限不作质量分 | 辅助（R2 的 150 上限） |

若将来需要"正文似然打分"（R3 完全体），中文替代逗号的特征应是**句末标点密度**（。！？；的出现频率）而非逗号——但目前无任何业界基准证明其收益，标记为"未证实，不引入"。

### 4.2 列表页（OSChina news 类）

"主干"对列表页 = 列表本身。三道保护：① R2 的 150 字符上限 + 0.5 阈值让"标题链接+摘要"的列表项（密度 0.3–0.5）不触发；② R3 锚定区内暂停 R2；③ 锚链接 0.3 系数保住分页/分类锚链接。残余风险（标题超长、无摘要、密度破 0.5 的列表项）由 R3 兜住；若 fixture 实测仍有误删，第一步把 150 调到 100，不动 0.5。

### 4.3 与既有转义/嵌套列表规则的交互

- **转义**：删除决策发生在 `inline_` 整块取走之后、push 之前——半截 `**`/`[` 不会漏进输出；转义本身逐文本节点进行（`chunkStart` 锚定），不受影响。唯一注意：丢弃块时**必须同步复位 `inCodeSpan_` 与 `href_`**（见 R2 伪代码交互点①），否则后续文本会被错误带入"码 span 内不转义"态或链接未闭合态。
- **嵌套有序列表**：丢弃 li 行会跳号，按 R2 交互点②回退 `next` 计数；嵌套无序列表无此问题。导航列表几乎全为 `<ul>`，实际影响面极小。
- **表格**：`cellOpen_` 期间禁启 R1/R1b（逐字对应 Readability 的 table 祖先保险丝）；表格行缓存在 `rows_`，不经 `inline_`/`flushInlineLines`，R2 天然不触碰表格。
- **代码块**：`Mode::Pre` 期间禁启一切删除；`<pre>` 内容不经 `inline_`，R2 不会误删代码。
- **title 兜底**：`assemble()` 的"无 h1 时补 # 标题"在删除之后运行，顺序不变——正文被删光的最坏情形退化为只输出标题，不会空崩。

---

## 5. 测试策略（无网 fixture）

fixture = 从真实页面浏览器"另存"后**裁剪保留关键区块**的静态 HTML，落 `tests/fixtures/html_to_markdown/`，配 `.md` 期望输出做金样对比：

| fixture | 来源裁剪 | 覆盖点 |
|---|---|---|
| `docs-content.html` | bigmodel 类文档站：header 导航 + 正文区（含代码块/表格/TOC）+ footer | R1 语义标签、R2 面包屑删除、锚系数保 TOC、Pre/表格不受扰 |
| `portal-list.html` | OSChina 类门户列表页：站头 + 新闻列表（标题链接+摘要）+ 友链/页脚 | R2 阈值恰在列表项之下（存活）、页脚链接行（删除）、R3 锚定 |
| `bare-page.html` | 无语义标签、无 class 线索的裸页面 | **回退保证**：全规则不命中 → 输出与现状金样逐字节一致 |
| `malformed.html` | 未闭合 `<header>`（其后再无闭合）、乱序闭合、Suppress 内嵌 `<main>` | R1 深度回退双保险（信号释放 + 64 KiB 上限） |
| `link-heavy.html` | 大量正常链接的正文（技术博客友链区/更新日志列表） | R2 误删防线：超 150 字符块或含图块存活 |
| `class-keyword.html` | `<div class="nav-menu">` 包正文、`.doc-content` 类名 | R1b 正词保险丝（kMaybeTokens 存疑不删） |

自动化三件套：① 金样对比（全部 fixture）；② **差分保底测试**——规则开关关闭时对全部 fixture 的输出与改造前基线逐字节相等（防"比现在差"）；③ 病理输入回归（截断、空串、纯标签）沿用现有测试口径。判收指标：`docs-content` 与 `portal-list` 输出中链接行数下降 ≥ 80%，`bare-page` 与 `link-heavy` 零变化。

---

## 6. 风险清单

| 风险 | 场景 | 缓解 | 残余 |
|---|---|---|---|
| 正文误删（R1b） | 正文容器 class 同时含危险词与正词，如 `sidebar-layout` 包正文 | kMaybeTokens 正词保险丝 + 表格/Pre 保险丝 | 长尾靠 fixture 回归与规则开关 |
| 正文误删（R2） | 友链区/更新日志（小块高密度正常内容） | 150 字符上限 + 含图不删 + 锚系数 | 纯文字短链接行必删——语义上判为样板，接受 |
| 未闭合标签吞正文 | `<header>` 不闭合 | 双保险：内容信号释放（article/main/h1-h3）+ 64 KiB 字节上限 | 极端病理页最多损失 64 KiB |
| 规则全不命中 | 裸页面 | 规则天然不触发；差分保底测试锁死 | 无（等价现状） |
| 转义/列表交互 | 半截行内标记、有序列表跳号 | 块级决策不漏标记；`next--` 回退；`inCodeSpan_`/`href_` 随块复位 | 无序导航列表不受影响 |
| 阈值过保守 | 样板删不干净 | 150 / 0.5 / 64KiB 三参数集中成常量区 + `linkDensityModifier` 式调节旋钮（Readability 同款设计） | 迭代成本只是一个常量文件 |
| 上游页面改版 | 站点重构 | 词表移植自多年实战的 Mozilla/trafilatura 词表，天然抗漂移；fixture 快照锁行为 | 定期对齐上游词表 |

---

## 7. 引用清单（等级 | 核对日期 2026-10-27）

一手来源（官方仓库源码/文档直读）：

1. [Mozilla Readability.js 源码（main 分支）](https://raw.githubusercontent.com/mozilla/readability/main/Readability.js) —— REGEXPS 词表、UNLIKELY_ROLES、正负 25 类名权重、段落打分、`_getLinkDensity`（锚链接 0.3 系数）、`_cleanConditionally` 0.2/0.5 阈值、五道删除保险丝（逐字核对）｜官方仓库｜Apache-2.0
2. [Mozilla Readability README](https://github.com/mozilla/readability/blob/main/README.md) —— "powers Firefox Reader View"；`linkDensityModifier` / `classesToPreserve` / `keepClasses` 选项语义｜官方文档
3. [Readability commit 726381c / PR #796（2023-04-25）](https://github.com/mozilla/readability/commit/726381cd58c684561f340d958691439e8c776ff0) —— "Added comma regular expressions"：全角逗号 U+FF0C 等入表｜官方提交历史（GitHub commits API 核实）
4. [trafilatura 评测页（官方文档）](https://trafilatura.readthedocs.io/en/latest/evaluation.html) —— 2026-10-02 版基准表（§2.2 全部数字出处）、外部评测引用（Bevendorff et al. 2023）｜官方基准
5. [trafilatura xpaths.py（master）](https://raw.githubusercontent.com/adbar/trafilatura/master/trafilatura/xpaths.py) —— BODY_XPATH 正向锚定、OVERALL_DISCARD 词表、aria-hidden / display:none 剔除（逐字核对）｜官方仓库
6. [jusText core.py（miso-belica/jusText，master）](https://raw.githubusercontent.com/miso-belica/jusText/master/justext/core.py) —— MAX_LINK_DENSITY 0.2 / LENGTH_LOW 70 / LENGTH_HIGH 200 / 停用词 0.30-0.32 / 分类与回捞逻辑（逐字核对）｜官方仓库｜BSD-2
7. [jusText 停用词表目录](https://github.com/miso-belica/jusText/tree/master/justext/stoplists) —— 约 100 语言无中文（GitHub API 目录核实）｜官方仓库
8. [boilerpipe NumWordsRulesClassifier.java（master）](https://raw.githubusercontent.com/kohlschutter/boilerpipe/master/boilerpipe-common/src/main/java/com/kohlschutter/boilerpipe/filters/english/NumWordsRulesClassifier.java) 与 [DensityRulesClassifier.java](https://raw.githubusercontent.com/kohlschutter/boilerpipe/master/boilerpipe-common/src/main/java/com/kohlschutter/boilerpipe/filters/english/DensityRulesClassifier.java) —— C4.8 规则：链接密度 > 0.333333 → 非内容（逐字核对）｜官方仓库
9. [boilerpipe LICENSE](https://github.com/kohlschutter/boilerpipe/blob/master/LICENSE) —— "Apache License, Version 2.0 / Copyright (c) 2009, 2014 Christian Kohlschütter"（逐字核对）｜官方仓库
10. [html2text PyPI 元数据（JSON API）](https://pypi.org/pypi/html2text/json) —— license_expression "GPL-3.0-or-later"、`--ignore-links` 描述、v2025.4.15｜官方包注册表

元信息（GitHub API，2026-10-27 查询）：mozilla/readability 最近提交 2026-07-09；miso-belica/jusText 最近推送 2026-10-21、828 stars。

二手来源（仅佐证背景，未作结论依据）：[webcrawlerapi 对 Readability 算法的解读](https://webcrawlerapi.com/blog/mozilla-readability-algorithm-readabilityjs)（2026-01，其中"逗号失效"说法经源码核实已部分过时）。

未证实项（明确标注）：R2 的 150 字符上限、R1 的 64 KiB 抑制上限、R4 的 banner/contentinfo/search role 扩展——均为本仓经验值/自定值，业界无对应单趟参数，须 fixture 校准。

---

## 8. 给下批 builder 的实现建议摘要

1. **动线**：`Mode` 枚举加 `Suppress`；`kSuppressTags`/`kUnlikelyTokens`/`kMaybeTokens`/阈值集中放文件头常量区（`constexpr`），预留 `linkDensityModifier` 式整定参数；`handleOpen`/`handleClose` 各加一个分支，`appendText` 加两行计数，`flushInlineLines` 开头加决策块——预计净增 150–200 行，仍是零依赖单趟。
2. **顺序**：先 R1（语义标签，风险最低收益最大）→ 差分保底测试锁死 → R1b（类名）→ R2（密度）→ R3/R4 看 fixture 长尾再定。
3. **纪律**：每条删除路径都过 §3 保险丝；丢弃块必须复位 `inCodeSpan_`/`href_`/计数器；任何规则关断时输出必须与基线逐字节一致（CI 断言）。
4. **先写 fixture 再写码**：§5 的 6 个 fixture 与金样先行，实现全程对着判收指标调参。

## 9. 更新日志

- 2026-10-27 初版：Readability / trafilatura / jusText / boilerpipe / html2text 拆解，规则集 R1–R4，CJK 与列表页适配，测试策略与风险清单。
