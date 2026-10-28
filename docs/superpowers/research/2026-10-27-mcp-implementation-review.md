# MCP 骨架 B1+B2 自研实现「睁眼看世界」对照调研

- 日期：2026-10-27（检索与核验同日完成，来源均标注检索日期）
- 分析对象：`D:\wt-mcp-skeleton`，分支 `feat/mcp-server-skeleton`，HEAD `a572947e`。B1 实现 = `4bccde3c`→`3cc1cd61`（源码+门禁），B2 = `1bf64e4f`（抓取能力）+`e4a61901`（测试对齐）；B3 合并（`a572947e`）之后无触及 B1/B2 实现文件的 fix 提交（git log 逐条核对）。
- 审读文件：`src/module_mcp/` 下 `html_to_markdown.cpp`（790 行）、`mcp_tools_builtin.cpp`（620 行）、`mcp_protocol.cpp`（292 行）、`mcp_http_handler.cpp`（156 行）、`mcp_service.cpp`（110 行）、`mcp_session_store.cpp`（95 行），以及 `platform/http_client.cpp`（libcurl 封装）、设计 spec `docs/superpowers/specs/2026-10-26-mcp-server-skeleton-design.md`（§6.1/§7/§14.2）。
- 性质：只读调研，不改代码。所有改进建议措辞为「供对应批次参考」。

来源分级：【A】官方规范 / 官方仓库源码 / 官方文档；【B】权威 cheat sheet / 标准组织 / 一手库官方渠道；【C】技术博客、镜像、论坛。每条结论附 URL。

---

## 维度一：html_to_markdown 自研（790 行单趟扫描器）vs 现成方案

### 实现现状（读码归纳）

单趟容错扫描器，行模型输出。已覆盖：嵌套 `ul/ol`（ListFrame 栈、每层 2 空格缩进、有序表自增序号）、嵌套 `blockquote`（`> ` 前缀按深度叠加）、表格（首行当表头 + 分隔行、单元格内 `|` 转义）、`pre` 围栏代码块、`script/style` 跳过、`<title>` 兜底 h1、23 个命名实体 + 十进制/十六进制数值实体（拒绝 0、代理区、>0x10FFFF）、2 MiB 输入截断（`html_to_markdown.cpp:12,785`）。

### 候选对比

| 候选 | 形态 | 成熟度 | license | 维护活跃度 | 判定 |
|---|---|---|---|---|---|
| turndown | JS 事实标准 | 高（规则集+测试集丰富） | MIT【B】 | 活跃 | 规则可移植；JS 本体不可用 |
| JohannesKaufmann/html-to-markdown | Go | 高（提供 turndown 兼容模式） | MIT【C】 | 活跃 | 机制参考 |
| html2text（Alir3z4） | Python | 高 | MIT【C】 | 活跃；dl/dt/dd 至今是 issue #107 | 机制参考 |
| rust html2text（jugglerchris） | Rust | 中高（文本渲染向） | MIT【C】 | 活跃 | 方向不同（非 markdown） |
| htmd-convert | Rust | 新 | 见 crates.io【C】 | 新 | dl+实体自动转换的示范实现 |
| lexbor | C 解析器 | WHATWG HTML5 解析，v3.0.0 | Apache-2.0【A】 | 活跃 | 仅解析，仍需自写 markdown 发射器 |
| gumbo-parser | C 解析器 | 曾为标准选择 | Apache-2.0【B】 | 上游归档（read-only） | 不可作基座 |
| litehtml | C++ 渲染引擎 | 中 | BSD-3【B】 | 有活动 | 渲染引擎，过重 |

来源：lexbor 官网 https://lexbor.com/license （2026-10-27 检索）；gumbo 归档 https://github.com/google/gumbo-parser ；litehtml https://github.com/litehtml/litehtml ；turndown https://github.com/mixmark-io/turndown ；html2text issue https://github.com/Alir3z4/html2text （issue #107）。

### 判定：自研方向正确，但规则集没站在巨人肩膀上

项目硬约束「零依赖静态单 exe」下，链接 lexbor/gumbo 均破坏约束（vendor 也可行但引入 C 库规模不小，收益仅是解析器健壮性）；C++ 生态确无「拿来即用」的 HTML→Markdown 库（.lexbor 等只管解析，发射器仍要自写）。**所以自研扫描器本身是正解**——但成熟规则集里两样最值钱的东西（markdown 转义表、列表缩进规则）没有被移植。

### 缺口清单（对照 turndown 规则集，npm 官方包 dist 直读核实）

turndown 转义表（官方包原文，2026-10-27 检索 https://cdn.jsdelivr.net/npm/turndown@7.2.0/dist/turndown.js 【A·一手】）：

```
var escapes = [
  [/\\/g, '\\\\'], [/\*/g, '\\*'], [/^-/g, '\\-'], [/^\+ /g, '\\+ '],
  [/^(=+)/g, '\\$1'], [/^(#{1,6}) /g, '\\$1 '], [/`/g, '\\`'],
  [/^~~~/g, '\\~~~'], [/\[/g, '\\['], [/\]/g, '\\]'],
  [/^>/g, '\\>'], [/_/g, '\\_'], [/^(\d+)\. /g, '$1\\. ']
];
```

1. **[major] 正文零 Markdown 转义**。本实现全文件只转义表格单元格里的 `|`（`html_to_markdown.cpp:317`）。后果：网页正文里出现的 `# 标题`、`3. 安装`、`- 项目`、`[链接](x)`、`*强调*` 会按字面进入输出并被 markdown 渲染器重新解释——轻则结构错乱，重则构成对下游 LLM 的「markdown 注入」通道。turndown 用上表 13 条规则（多数是行首上下文相关）解决。
2. **[major] 嵌套列表缩进固定 2 空格/层**（`html_to_markdown.cpp:284`）。turndown listItem 续行/嵌套用 **4 空格**（官方包 `.replace(/\n/gm, '\n    ')` 原文同上）。有序项 `1. ` 标记宽 3 起，2 空格缩进的子列表在严格 CommonMark 渲染器里会被解析成与父项平级（CommonMark 列表续行缩进须达「标记+后随空格」宽度；spec.commonmark.org 本次抓取失败，未直读，标未证实），有序嵌套列表结构会被展平。另：`<br>` 造成的 li 内换行会重复 `- ` 前缀（`flushInlineLines` 对每段都套 `liPrefix`，`html_to_markdown.cpp:250`），turndown 对续行只缩进不打标记。
3. [minor] `dl/dt/dd` 未处理（透明拼接，term 与定义挤在一起）。turndown core 同样不支持、html2text 也是多年 issue——**与业界持平的已知缺口，非本实现独有**。
4. [minor] 命名实体仅 23 条（HTML5 全表 2231 条）；实体体长 >12 字符不识别（`html_to_markdown.cpp:97`）。代码注释已声明「表外原样保留」的取舍，方向可接受。
5. [minor] `<a href>` / `<img src>` 属性值未转义 `()`/空白/`<>`，URL 含 `)` 即断链；turndown 对 URL 做百分号编码。
6. [minor] 围栏代码块无反引号冲突处理：pre 内容含 ``` 时围栏被击穿（turndown 用双反引号/更长围栏兜底）。

**结论（供后续批次参考）**：不换库、不引入依赖；把上面 1/2 按 turndown 规则移植（含把 turndown 的 HTML→Markdown 测试样例拷来当黄金对照集，成本低于重新设计用例），3/4/5/6 视排期。

---

## 维度二：SSRF 防护对照 OWASP SSRF Prevention Cheat Sheet

OWASP 原文（2026-10-27 检索 https://cheatsheetseries.owasp.org/cheatsheets/Server_Side_Request_Forgery_Prevention_Cheat_Sheet.html 【B】）关键句：
- 「Deny-lists are bypass-prone. Prefer allow-lists.」
- 「Disable the support for the following of the redirection in your web client in order to prevent the bypass of the input validation」
- 「Domain allowlisting alone does not prevent DNS rebinding. … A second, unchecked DNS lookup between validation and connection can bypass these checks. Use an HTTP client mechanism that connects to the validated IP…」
- 「Reject a URL whose host is not read identically by every parser in play」

### 已落实（读码逐条对上）

- scheme 白名单 http/https，且**重定向每跳复检**：`Location` 经 `resolveRedirect` 归一（绝对/`//`/`/`/相对+点段消解）后回到主循环，重新过 scheme + SSRF 检查才触网（`mcp_tools_builtin.cpp:496-557`）。等价满足 OWASP「禁自动跟随+逐跳校验」的意图（本仓 libcurl 未设 `CURLOPT_FOLLOWLOCATION`，手动循环跟随）。
- 字面私网段：IPv4 `127/8`、`0/8`、`10/8`、`172.16/12`、`192.168/16`、`169.254/16`（云元数据 169.254.169.254 被覆盖）；IPv6 `::`、`::1`、`fe80::/10`、`fc00::/7`，且 **IPv4 映射地址 `::ffff:0:0/96` 还原成 v4 再查表**（`ssrfRejectReason`，`mcp_tools_builtin.cpp:355-362`）——OWASP 表未点名该段而实现主动覆盖，是加分项。
- 响应侧：2 MiB 上限、10 s 总时限、Content-Type 白名单、charset 白名单——OWASP 该页无响应侧条目，本实现自备，超界即挡。
- 入站（服务自身）：仅绑 `127.0.0.1`（`mcp_service.cpp:20`）+ Origin 白名单三回环拼写、带 `@` 判不可解析即拒（`mcp_http_handler.cpp:54-77`）+ **Host 头白名单**（`hostWhitelistForBind`，`http_server.h:21`）——规范 Security Warning 的两条 MUST/SHOULD 与 DNS rebinding 经典路径（伪造 Host）都有闸。

### 缺口

1. **[major] 非 dot-decimal 的 IPv4 编码形态放行**。`parseIpv4` 只认严格点分十进制（`mcp_tools_builtin.cpp:234-254`），解析失败一律按「域名」放行。而业界绕过清单里全部数字变体（PortSwigger SSRF cheat sheet 全录，页面反爬未直读，经镜像 https://cheatdocs.org/ssrf-payloads 、https://github.com/shahenshah99-zz/web-payloads/blob/master/SSRF%20injection/README.md 佐证【B/C】）：`2130706433`（十进制整数）、`0177.0.0.1`（八进制段）、`0x7f.0.0.1`（十六进制段）、`127.1`（缩写）、`127.000000000000000.1`（填充零）——libcurl 把它们当主机名交给系统解析器，多数平台会解析回 `127.0.0.1`。测试（`test_mcp_service.cpp`）只覆盖 spec §6.1 点名的字面形态，对上述变体零覆盖。**更要害的是它架空了 §14.2 的风险声明**：spec 声明接受的是「DNS 域名放行 → rebinding TOCTOU」（前提：私有地址只能经攻击者 DNS 才够到），而数字编码让攻击者不碰 DNS、用 URL 字面量直达回环——接受声明的边界条件被绕过。
   修复方向（供对应批次参考）：host 由纯数字/点/十六进制前缀构成（不含字母域名形态）即拒；或按 OWASP 推荐改为「解析后按连接 IP 复检」（curl 可用 `CURLOPT_RESOLVE`/`CURLOPT_CONNECT_TO` 把连接钉在已验证地址上）。
2. **[major] IPv4-compatible IPv6（`::/96`，如 `::127.0.0.1`）放行**。`ssrfRejectReason` 对 IPv6 只拦：全零、`::1`、`::ffff:` 映射、fe80、fc00。`::127.0.0.1` 前七组全零、末组非 0/1、非 ffff 映射——四条都不中，放行（该形态 RFC 4291 已废弃但系统解析器普遍仍接受，是 PortSwigger 清单在列变体）。与缺口 1 同根同修。
3. [minor] IPv4 deny 表缺 `100.64.0.0/10`（CGNAT）、`198.18.0.0/15`、`192.0.0.0/24`、`224.0.0.0/4`（OWASP 点名 multicast）、`240.0.0.0/4`、`255.255.255.255`。桌面本机场景风险低，补表是低成本动作。
4. [minor] host 含 `@`（userinfo）未显式拒绝——当前行为恰是 fail-safe（带 `@` 的 host 解析失败、连不上），但按 OWASP「各解析器读法不一致即拒」应显式拒掉。
5. [accepted，维持] DNS rebinding TOCTOU：出站 fetch 无地址钉扎，spec §14.2 已声明接受。在修复缺口 1/2 之后该声明重新成立：残余路径只剩「攻击者控制 DNS」一条，且出站数据只以工具输出文本形式回流；入站服务另有 Origin/Host/绑定三闸。建议修完 1/2 时把 §14.2 注释的措辞同步改准（当前注释「DNS 域名放行」掩盖了编码绕过不依赖 DNS 的事实）。

**结论**：结构正确、优于常见字面拦截基线（映射地址还原、逐跳复检都是对的）；但缺口 1/2 属「业界已收录的绕过手法」，必须补。定级 major 而非 blocker：攻击面是「网页内容诱导工具抓内网」，本机高危暴露面取决于用户环境，且修复面窄（一个 host 形态校验）。

---

## 维度三：Streamable HTTP 对照官方规范 2025-06-18 与官方 SDK 参考实现

对照一手来源【A，2026-10-27 检索】：规范 https://modelcontextprotocol.io/specification/2025-06-18/basic/transports 与 /basic/lifecycle；官方 Python SDK `src/mcp/server/streamable_http.py`（raw 直读 https://raw.githubusercontent.com/modelcontextprotocol/python-sdk/main/src/mcp/server/streamable_http.py ；TS SDK 同名文件路径已 404，疑重构，未取到，以 Python SDK 为参考实现）。

### 已落实（逐条对上规范/SDK）

- 单端点 `/mcp` 挂 POST/GET/DELETE；GET 与 OPTIONS 返回 405 + `Allow: POST, DELETE`——规范明文允许「or else return HTTP 405 Method Not Allowed, indicating that the server does not offer an SSE stream at this endpoint」。
- 通知（无 id）一律 **202 无 body**——对上「the server MUST return HTTP status code 202 Accepted with no body」；Python SDK 同为 202。
- JSON 数组批处理 → 400/-32600——2025-06-18 已移除批处理，与官方行为一致。
- `Mcp-Session-Id`：initialize 响应头下发、未知/过期 → **404**（规范「it MUST respond to requests containing that session ID with HTTP 404」）、DELETE 已知 → 200 移除/未知 → 404（规范允许 405 的反向选择，允许终止是合规的）；stop 时清空会话表。
- `MCP-Protocol-Version`：头存在且不受支持 → **400**（规范「it MUST respond with 400 Bad Request」）；缺省行为：initialize 在 params 里协商、不受支持时回服务端最新支持版——对上 lifecycle 原文「Otherwise, the server MUST respond with another protocol version it supports. This SHOULD be the latest version supported by the server」；任务书所述「缺头默认按 2025-03-26」在规范里是服务端对无头请求的 SHOULD 假设，本实现支持的两组版本恰好覆盖。
- Content-Type 非 `application/json`（允许 charset 后缀、大小写不敏感）→ 415。
- Origin 校验：带 Origin 且非三回环拼写（`127.0.0.1`/`localhost`/`::1`，忽略 scheme/端口、拒 userinfo）→ 403；无 Origin（非浏览器）放行——对上规范「Servers MUST validate the Origin header」，与「localhost 拼写歧义」的业界经验一致。
- 仅绑 `127.0.0.1` + Host 头白名单——规范 Security Warning 1/2 全落实。

### 缺口/偏差

1. **[major] 不带 `Mcp-Session-Id` 头的非初始化请求被放行**。`handlePost` 只在「头非空且不认识」时 404（`mcp_protocol.cpp:97`），头缺失 + method 非 initialize → 直接处理。规范：「Servers that require a session ID SHOULD respond to requests without an Mcp-Session-Id header (other than initialization) with HTTP 400」；Python SDK 参考实现即 400 `"Bad Request: Missing session ID"`。本服务 initialize 必发会话 id，等于「发了会话却不要求回带」——会话记账可被绕过。一行判空的修复。
2. [minor] POST 不校验 `Accept` 头。规范对服务端未设 MUST（只约束客户端 MUST 携带），但 Python SDK 返回 406 `"Not Acceptable: Client must accept both application/json and text/event-stream"`。宽松不算违规，但与参考实现行为不一致，联调排障时可能误导。
3. [minor] 同会话内二次 `initialize` 会另起新会话且旧会话留存（`mcp_protocol.cpp:146-156` 无已有会话检查）；参考实现对重复 initialize 拒绝。无安全影响，仅占会话表（`kMaxSessions=64`，LRU 兜底）。
4. [minor] 会话 id 用 `std::random_device`（非密码学随机）——规范 SHOULD「globally unique and cryptographically secure」；代码注释已声明本地信任模型取舍，自洽。注意：MinGW 老工具链的 `std::random_device` 有确定性输出缺陷，MSVC 无虞——换构建链时需复核（`mcp_session_store.cpp:28-38`）。
5. [minor] 版本头校验对 `initialize` 豁免（先探 body 再拒，`mcp_protocol.cpp:101-112`）——规范 MUST 无豁免条款、SDK 同样直接 400。宽松方向，无害。

**结论（供对应批次参考）**：协议面完成度高，规范 MUST 项全部落实；缺口 1 是唯一建议必修项（SHOULD→参考实现 400），其余为可选对齐。

---

## 维度四：超时与截断

业界口径【A，2026-10-27 检索】：Go `http.Client.Timeout`「includes connection time, any redirects, and reading the response body」（https://pkg.go.dev/net/http#Client ）——总预算覆盖全链路；libcurl `CURLOPT_TIMEOUT_MS` 为单次传输上限（含连接与收体，重定向是另一次传输）（https://curl.se/libcurl/c/CURLOPT_TIMEOUT_MS.html ）。

### 已落实

- **10 s 总预算跨跳记账正确**：`deadline = steady_clock::now() + totalBudget`，每跳 `timeoutMs = min(kPerHopTimeoutMs, remainMs)`（`mcp_tools_builtin.cpp:492,507-519`）。因本仓手动跟随重定向、libcurl 每跳独立计时，`min(剩余, 单跳上限)` 恰是 deadline 传播的标准做法——与 Go 单一 Timeout 全链路封顶同思想，无累计放水路径；派发前再查一次 deadline，超时即断。
- **2 MiB 字节截断**：先按字节截（`mcp_tools_builtin.cpp:561-562`），截断可能劈开 UTF-8 序列，但随后 `platform::utf8::sanitize`（`:579`）清掉残字节，输出良构；`html_to_markdown` 入口还有一层 2 MiB 兜底（`html_to_markdown.cpp:785`），双闸一致。
- **UTF-8 边界安全**：错误前缀 200 字符用 `utf8SafePrefix` 按续字节（0x80-0xBF）回退（`:423-428`）；输出截断 `truncateByChars` 按码点计数（`:431-451`），截断附注明示。边界处理正确。
- **charset 白名单归一**：去空白/引号、小写后仅放行 `utf-8/utf8/us-ascii/ascii`（`:574-578`），其余报错。

### 缺口

1. [minor] charset 语义 vs WHATWG Encoding 标准（https://encoding.spec.whatwg.org/ 【A】）：`ascii`/`us-ascii` 在 WHATWG 里是 windows-1252 的标签（原文「'latin1' and 'ascii' are just labels for windows-1252」）；本实现当纯 ASCII → UTF-8 处理，高位脏字节由 sanitize 兜底，实际无害。UTF-8 的冷门别名标签（`unicode-1-1-utf-8` 等）不识别，影响可忽略。
2. [minor] GBK/GB2312/GB18030 全拒——中文存量网页受影响面不小；spec 已明写该取舍（测试用例含 `charset=gbk → isError`）。零依赖约束下的后续改进路径：Windows `MultiByteToWideChar` / macOS `CFString` 系统自带转码，不引第三方库——供后续批次评估。
3. [minor] 截断附注自身不计入 `max_chars`（输出可比上限长几十字节）——纯口径问题。

**结论**：该维度实现质量最高，与 curl/Go 业界语义严丝合缝，无需返工。

---

## 总体评估：站得住，不建议打回

- **协议面（B1）**：规范 MUST 全落实、错误码语义与官方 SDK 对齐，结构分层（handler 判序 → protocol 纯逻辑 → store 并发安全）便于测试。合格。
- **能力面（B2）**：SSRF 骨架正确（逐跳复检、映射地址还原、响应侧四闸、入站三闸），超时/截断是四个维度里质量最高的部分；扣分集中在两个可低成本修复的 major——IP 编码绕过（含 `::/96`）与「无会话头请求放行」。
- **转换器（B2）**：自研方向在零依赖约束下成立（C++ 无现成轮子，对比表为证），但缺 turndown 规则集里最关键的两样（转义表、列表缩进），对 LLM 消费场景的输出质量有实际影响。

**建议**：不打回；开一个小「收口批」合并修复 3 个 major（SSRF host 形态校验 + `::/96`、无会话头 400、markdown 转义与嵌套有序列表缩进）+ 顺手补 deny 表与 Accept 校验两个 minor。所有建议均供对应批次参考，本文不构成改动指令。

## 待核清单（本次未能联网/未能实测，交回时请留意）

1. 数字 IP 编码在当前构建的 libcurl + 目标平台解析器下是否真的连通 `127.0.0.1`（Windows `getaddrinfo` 对 `0177.0.0.1`/`2130706433`/`127.1` 的接受度有版本差异）——建议实测而非只修代码。
2. MCP TS SDK `streamableHttp.ts` 现行路径（仓库疑似重构，raw 404），二次 initialize 的拒绝行为以 TS SDK 为准复核。
3. CommonMark 列表缩进原文（spec.commonmark.org 抓取失败）——「2 空格嵌套有序列表被展平」的结论目前由 turndown 4 空格实践反推，未逐字对规范。
4. turndown escapes 引文取自 npm 官方包 dist（https://cdn.jsdelivr.net/npm/turndown@7.2.0/dist/turndown.js ），与 GitHub 仓库 src 同源；GitHub raw 多镜像 404 未复核。

## 引用清单

| # | 来源 | 等级 | 用途 |
|---|---|---|---|
| 1 | https://modelcontextprotocol.io/specification/2025-06-18/basic/transports | A | 单端点/Accept/202/会话 404/版本头 400/Origin/绑 localhost 原文 |
| 2 | https://modelcontextprotocol.io/specification/2025-06-18/basic/lifecycle | A | 版本协商 MUST 原文 |
| 3 | https://raw.githubusercontent.com/modelcontextprotocol/python-sdk/main/src/mcp/server/streamable_http.py | A | 参考实现错误码与错误串（406/400/404/202） |
| 4 | https://cheatsheetseries.owasp.org/cheatsheets/Server_Side_Request_Forgery_Prevention_Cheat_Sheet.html | B | deny 表、重定向、rebinding、「连已验证 IP」原文 |
| 5 | https://portswigger.net/web-security/ssrf/ssrf-cheat-sheet（反爬未直读）+ https://cheatdocs.org/ssrf-payloads + https://github.com/shahenshah99-zz/web-payloads | B/C | IP 编码绕过清单 |
| 6 | https://cdn.jsdelivr.net/npm/turndown@7.2.0/dist/turndown.js | A（官方包产物） | escapes 表、listItem 4 空格缩进、无 dl/table 规则 |
| 7 | https://github.com/mixmark-io/turndown + https://github.com/mixmark-io/turndown/issues/89 | B | turndown core 不含表格，需 plugin-gfm |
| 8 | https://github.com/Alir3z4/html2text（issue #107） | C | html2text 的 dl 支持同为长期缺口 |
| 9 | https://curl.se/libcurl/c/CURLOPT_TIMEOUT_MS.html | A | 单次传输超时语义 |
| 10 | https://pkg.go.dev/net/http#Client | A | Timeout 含连接/重定向/收体原文 |
| 11 | https://encoding.spec.whatwg.org/ | A | ascii/us-ascii → windows-1252、UTF-8/GBK 标签表 |
| 12 | https://lexbor.com/license ；https://github.com/google/gumbo-parser ；https://github.com/litehtml/litehtml | A/B | C/C++ 解析器生态 license 与维护状态 |
| 13 | 本仓 `docs/superpowers/specs/2026-10-26-mcp-server-skeleton-design.md` §6.1/§7/§14.2 | 内部 | 实现声明的取舍口径核对 |
