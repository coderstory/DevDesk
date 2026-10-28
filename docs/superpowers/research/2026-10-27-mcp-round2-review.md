# MCP 骨架第二轮增量对照调研(B3 装配 / 排他绑定 / 安全收口权威参照 / SSRF 第三遍 / 架构五维)

- 日期:2026-10-27(检索与核验同日,来源标注检索日期)
- 分析对象:`D:\wt-mcp-skeleton`,分支 `feat/mcp-server-skeleton`。基线:第一轮 `a572947e` 之后三个 fix——`a028fb1e`(排他绑定)、`e1b22986`(include 守卫)、`43b983ac`(SSRF IP 编码拦截)。builder 正在改 B2 四文件,本文对 B2 文件一律读 `git show a028fb1e:` 提交版,SSRF 收口判定额外参考已落的 `43b983ac`(diff 逐行核对)。
- 性质:只读调研,不改码;全部建议为「供对应批次/builder 校准参考」。第一轮文档:同目录 `2026-10-27-mcp-implementation-review.md`,本轮只做增量、不重复其结论。
- 来源分级:【A】官方规范 / 官方文档 / 官方仓库源码(含本仓 vendored `3rd/cpp-httplib/httplib.h` 直读);【B】权威 cheat sheet / RFC / 一手技术长文;【C】社区验证帖。每条关键结论附 URL + 原文引文。

---

## 一、B3 装配对照:main 模块装配模式 vs 业界内嵌本地服务生命周期

### 实现现状(读码归纳,文件行号以工作区为准)

- **按 moduleEnabled 启动 + 失败不阻断**:`src/main.cpp:336-348`——MCP 装配段在 `buildGatedEntries`/`assembleAndStart` 之后:`registerBuiltinMcpTools`(常驻注册表)→ `app::mcp::setHandlers` 注桥 → `moduleEnabled(cfg,"mcp")` 才 `globalMcpService().start(port)`,失败仅 `LOG_ERROR`(注释明写「与代理自启同口径」)。
- **atexit 退出链(LIFO)**:注册序 = `armExitHook`(assembleAndStart 内,最早)→ githubSyncExitPush → saveWindowGeometry → **mcp stop(最晚)**。按 C++ 反序执行,mcp stop **先跑**、`router->stopAll()` **最后**——与 `src/main.cpp:350-352`、`src/app_assembly.h` 顶部注释一致。
- **桥默认桩 fail**:`src/mcp_service_bridge.cpp`——未装配时 `start/applyPort` 返回 fail("MCP 服务未装配"),`status` 返回全空 `LiveStatus`(running=false);四槽 `static_assert` 签名哨兵。
- **mcp 特例先于注册表遍历**:`settings_config.h` 明写 mcp「不是 Router 页面(不进注册表遍历),开关即时生效走 app_assembly 的 mcp 特例分发」;settings 模块只经桥读状态/发指令,不 include module_mcp。

### 业界对照

| 实践 | 业界口径 | 本仓 |
|---|---|---|
| 优雅停机 | Go `http.Server.Shutdown`:「先关闭所有监听器→关闭空闲连接→等待在途连接回到空闲」(https://pkg.go.dev/net/http#Server.Shutdown 【A】,正文抓取截断,引文经检索摘要复核,标部分未逐字核验) | `McpService::stop()` 先摘引用再锁外 join(`mcp_service.cpp:57-76`);vendored httplib 在 accept 循环退出时 `task_queue->shutdown()`——**先排干任务队列再 join 全部工作线程**(httplib.h:11848-11877,13889) |
| stop 与在途收敛的竞态 | 监听 socket 关闭后 keep-alive 空闲连接靠读超时收尾(cpp-httplib issue #1548,https://github.com/yhirose/cpp-httplib/issues/1548 【A】) | 空闲 keep-alive 由 `CPPHTTPLIB_KEEPALIVE_TIMEOUT_SECOND=5` 兜底(httplib.h:25);在途抓取受 10s 总时限 → **退出最坏挂起 ≤ ~10s,有界**;唯一发现的良性竞态:`stop()` 在排干**前**清会话表(`sessions_.clear()` 先于 `server->stop()`),在途请求的后续会话校验会 404——语义无害(客户端重握手),标 minor |
| atexit 顺序 | 「后注册先执行」;C++11 起 handler 与静态析构可并发,仅保持注册序 sequenced-before(https://en.cppreference.com/w/cpp/utility/program/atexit.html 【A】) | 退出路径依赖的三个单例(`globalMcpService`/`globalMcpTools`)用「堆分配永不析构」规避静态析构竞态(`mcp_service.cpp:98-101`、`mcp_tool.cpp:50-53`)——与 Chromium/Google leaky-singleton 惯例同构,静态析构顺序地雷已拆除 |
| 单实例 | Windows 命名互斥量是桌面惯例 | `platform/single_instance.h`:`Local\DevDesk.SingleInstance.Mutex` + 首实例监听线程;与 EXCLUSIVEADDRUSE 构成进程级 + socket 级两道独立防线 |
| 端口冲突处置 | JetBrains 内嵌 server 冲突时自动换端口(客户端经 IDE 生成 URL 感知);通用桌面内嵌服务常 fail-fast 如实上报 | 本仓选 fail-fast + 如实报错(`Failed` + lastError),与 EXCLUSIVEADDRUSE 的「占用如实报错」意图自洽;非 JetBrains 场景(客户端手工配端口),选择成立 |
| settings 键值存储 | 配置读边界 sanitized(越界钳制/未知枚举收敛/白名单过滤)是成熟惯例 | `settings_config.h/.cpp`:`load()` 返回值已 sanitized(头文件契约),`sanitized()` 静态可单测;SQLite `app_config` 键前缀 + 每顶层键一行防多写者互踩 + 事务 UPSERT + 清旧键;模块 ID 显式白名单。**无偏差** |

### 结论(供 B5 参考,不构成改动指令)

装配模式与业界生命周期实践**同构**:启动非阻断、停机 LIFO 次序正确且注释与实现一致、在途收敛有界(≤10s)、静态析构地雷已用 leaky singleton 拆除、settings 边界 sanitized 完整。仅两处 minor:`stop()` 清会话先于排干(良性);`ExitProcess`(安装器强杀)不跑 atexit,靠 OS 收 socket(桌面场景可接受)。无 major。

---

## 二、fix 批排他绑定对照:EXCLUSIVEADDRUSE vs REUSEADDR 业界取舍

### 本仓实现(`a028fb1e`,`http_server.cpp:186-206`)

Windows 设 `SO_EXCLUSIVEADDRUSE`;POSIX 不设任何 reuse 位;注释如实写明代价「同端口快速重启需等旧连接 TIME_WAIT 清空」。

### 业界权威口径

- **微软官方明确站边排他**:「The SO_REUSEADDR socket option allows a socket to forcibly bind to a port in use by another socket…behavior for all sockets bound to that port is indeterminate」「A malicious program can use SO_REUSEADDR to forcibly bind sockets already in use」「**All server applications must set SO_EXCLUSIVEADDRUSE for a strong level of socket security**」(https://learn.microsoft.com/en-us/windows/win32/winsock/using-so-reuseaddr-and-so-exclusiveaddruse 【A】,2026-10-27 检索)。
- **TIME_WAIT 代价是微软文档明写的、被接受的成本**:「a socket with the SO_EXCLUSIVEADDRUSE set cannot necessarily be reused immediately after socket closure…cannot bind to the same port…until the original connection becomes inactive」(同上)。Windows TIME_WAIT 默认时长 = `TcpTimedWaitDelay`,现代系统默认 120 秒(旧文档 240 秒),可调 30-300(https://learn.microsoft.com/en-us/biztalk/technical-guides/settings-that-can-be-modified-to-improve-network-performance 【A】)。
- **SO_LINGER「更优姿势」被微软官方劝退**:「may be set…to prevent the port from transitioning to an 'active' wait state; **however, this is discouraged as it can lead to undesired effects, such as reset connections**」(同上)。微软给的替代是优雅关闭:`shutdown(SD_SEND)` + recv 到零字节。
- **REUSEADDR+启动前探测**:无需——bind 失败本身已如实上报(EXCLUSIVE 正是让 bind 失败可信的前提),探测属重复防御。
- **业界默认值对照**:vendored httplib 上游默认 `default_socket_options` 设 `SO_REUSEPORT`(无则 `SO_REUSEADDR`)(httplib.h 直读)——Windows 上即 REUSEADDR,**正是 a028fb1e 注释所指「双绑定不报错假阴性」的来源**;libuv/Node、Python `http.server`、Go 在 POSIX 侧都用 SO_REUSEADDR 换快速重启(Windows 侧 Python 文档明示 REUSEADDR 有劫持语义)【B】。POSIX(Linux/macOS)的 TCP LISTEN 语义下 REUSEADDR **不**允许双活监听(那需要 SO_REUSEPORT),收益仅 TIME_WAIT 快速重启、几乎没有 Windows 式劫持面。

### 判定

- Windows 侧 EXCLUSIVEADDRUSE = 微软官方对服务端 socket 的 MUST 级要求,本仓选了安全侧;TIME_WAIT 窗口(2-4 分钟)是文档明写、业界常规接受的成本。触发条件 = 「服务端主动关闭连接后窗口期内同端口重启」,对 settings 改端口这类低频操作可接受,如实报错让用户可诊断。
- **可优化点(供 B5,非必修)**:POSIX 分支可考虑补 `SO_REUSEADDR`(Linux/macOS TCP listen 语义下不引入 Windows 式劫持,只消 TIME_WAIT 重启等待),Windows 维持 EXCLUSIVE——两平台各取「安全且更顺」的主流姿势;现实现(两平台都严格)只是重启体验略糙,不是错误。
- 竞态核对:`start()` 失败路径由 `McpService::start` 的局部对象逆序析构兜住(`mcp_service.cpp:33-38` 注释声明),无半开 server 泄漏。

---

## 三、安全收口批权威参照(供 builder 校准,不改码)

### (a) CommonMark/GFM 规范口径:转义与嵌套列表缩进

一手来源【A,2026-10-27 检索】:GFM 规范全文 https://github.github.com/gfm/ (Version 0.29-gfm);CommonMark spec.txt https://raw.githubusercontent.com/commonmark/commonmark-spec/master/spec.txt 。GFM 自述:「This formal specification is based on the CommonMark Spec」「GFM is a strict superset of CommonMark」——列表缩进与反斜杠转义规则两者一致,以 GFM 为准。

**转义规则(权威原文)**:
- 「Any ASCII punctuation character may be backslash-escaped」;「Backslashes before other characters are treated as literal backslashes」——转义 ASCII 标点是**处处合法的超集**,转义字母/数字则**非法**(反斜杠原样出现在输出)。
- 「Escaped characters are treated as regular characters and do not have their usual Markdown meanings」,官方示例直接覆盖注入面:`\# not a heading`、`1\. not a list`、`\<br/> not a tag`、`\[foo]: /url "not a reference"`、`\&ouml; not a character entity`(GFM §6.1 Example 310)。
- 块级优先:「Indicators of block structure always take precedence over indicators of inline structure」(GFM §3.1)——行首以 `\` 开头的行不会被解析为任何块构造,故**行首转义一个标点即终止块级解释**;turndown 的 13 条行首上下文规则(第一轮所引 escapes 表)是这个合法超集的低噪声子集,移植它仍是正解。
- 「Backslash escapes do not work in code blocks, code spans, autolinks, or raw HTML」——围栏代码块内容**不该**再做反斜杠转义(现有实现未转义,方向碰巧正确)。

**嵌套列表缩进(权威原文,GFM §5.2)**:
- 规则一:「a list marker of width W followed by 1 ≤ N ≤ 4 spaces…indenting subsequent lines of Ls by **W + N spaces**, is a list item」——子内容缩进必须 ≥「标记宽 + 标记后空格数」。
- 「The most important thing to notice is that the position of the text after the list marker determines how much indentation is needed in subsequent blocks」+「**A sublist must be indented the same number of spaces a paragraph would need** in order to be included in the list item」。
- 官方三连例:`- foo` + 2 空格子列表**合法**(Example 272);`10) foo`(标记宽 3)需 4 空格(Example 274);只缩 3 空格 → **裂成两个列表**(Example 275「Three is not enough」)。
- **对第一轮结论的修正与定谳**:「2 空格嵌套被严格渲染器展平」只对**有序列表**(及一切「标记+空格」宽 >2 的形态)成立;**无序列表 `- ` 的 2 空格嵌套本身规范合法**。turndown 统一 4 空格 = 对一切宽 ≤4 标记(`- `、`1. `、`10. `)安全的超集;唯一失效是单列表起点 ≥100(标记宽 5)——自增序号从 1 起,不触雷。
- **校准建议**:统一 4 空格(与 turndown 黄金集逐字节对齐,免做宽度计算);li 内 `<br>` 续行只缩进不打 `- ` 前缀(第一轮已提)。

### (b) Windows/MinGW 下数字编码 IPv4 的实际接受规则(第一轮待核①,已核)

1. **libcurl 自己就归一,与系统解析器无关(决定性证据)**:curl 在 URL 解析阶段由 `lib/urlapi.c` 的 `ipv4_normalize()`(约 L524-627)把 `127.1`/`0x7f.1`/`0177.0.0.1`/`2130706433` 改写成 `127.0.0.1` 再进解析——「在 URL 解析阶段,127.1 就已经被规范化成了 127.0.0.1」(https://blog.dreamfever.me/posts/2026-09-27-curl-127-1 【B】,2026-09-27 发布,2026-10-27 检索,调用链 `curl_url_set→parseurl→parse_authority→ipv4_normalize` 文中直读)。**第一轮 major「数字编码放行」在本仓 libcurl 栈上是平台无关实锤,`43b983ac` 修复方向正确且必要。**
2. **Windows 官方文档**:legacy `inet_addr` 官方支持四种形态(`a.b.c.d a.b.c a.b a`;三段末段 16 位、两段末段 24 位、单段直接 32 位),且「The parts…can be decimal, octal or hexadecimal as specified in the C language. Numbers that start with '0x' or '0X' imply hexadecimal. Numbers that start with '0' imply octal」,示例表含 `004.003.002.020`(八进制)与 `4.003.002.0x10`(混合)(https://learn.microsoft.com/en-us/windows/win32/api/winsock2/nf-winsock2-inet_addr 【A】)。`getaddrinfo` 文档口径收窄:「the numeric host address string is a dotted-decimal IPv4 address or an IPv6 hex address」(https://learn.microsoft.com/en-us/windows/win32/api/ws2tcpip/nf-ws2tcpip-getaddrinfo 【A】),但 Winsock 家族数字串解析实践宽松(WSAStringToAddress 接受 `1.2.3` 的社区实证:https://stackoverflow.com/questions/29285954 【C】)。
3. **glibc(POSIX 侧)**:`getaddrinfo` 走 `__inet_aton_exact`(glibc 2.29,2019;修 CVE-2016-10739 尾部垃圾注入),短形态仍按 inet_aton 语义接受(同上 curl 机制文 【B】);RFC 6943 §3.1.1:「allowing octal and hexadecimal, and addresses with less than four parts…'10.0.258', '0xA000102', and '012.0x102' all represent the same IPv4 address」,建议:「New protocols and data formats should similarly consider using the **strict form** rather than the loose form」(https://www.rfc-editor.org/rfc/rfc6943.txt 【B】)。
4. **RFC 3986 口径**:URI 里合法 IPv4 只有严格点分四段,其余形态属「may be interpreted on some platforms」的平台歧义(经上引 curl 机制文转引 §3.2.2/§7.4 【B】)。
5. **`43b983ac` 逐条核对**:自实现 inet_aton 语义解析(1-4 段 / 0x 十六进制 / 前导 0 八进制 / 末段加权),解析失败但「长得像数字」即 fail-closed(`isNumericEncodedHost`),与「实际可连通形态」覆盖一致。**一处残留见第四章新发现①(尾点)**。
6. **zone-id**:`parseIpv6` 解析失败改判 fail-closed(`43b983ac` diff:「zone-id([fe80::1%25eth0])、畸形 v6 等…不得走『放行当域名』分支」)——覆盖第一轮待核项;`127.0.0.1%1` 类 v4 带区号非合法形态,系统解析必失败,现走域名放行分支但连不通,无害。

### (c) RFC 3986 userinfo 权威语义与安全惯例

- 语法与弃用(RFC 3986 §3.2.1,https://www.rfc-editor.org/rfc/rfc3986.txt 【A】):「userinfo = *( unreserved / pct-encoded / sub-delims / ':' )」;「Use of the format "user:password" in the userinfo field is deprecated.」;「Applications should not render as clear text any data after the first colon (":")…Such rendering will assist the user in cases where the userinfo has been **misleadingly crafted to look like a trusted domain name**」。
- OWASP 口径(https://cheatsheetseries.owasp.org/cheatsheets/Server_Side_Request_Forgery_Prevention_Cheat_Sheet.html 【B】,2026-10-27 检索):未设「一律拒 userinfo」普适规则,但把 `@` 双解析歧义作范例——「Treat parser disagreement as a rejection」「Reject a URL whose host is not read identically by every parser in play」,示例 `http://example.com\@evil.com`。
- 解析器现实:WHATWG URL 仍解析 userinfo(http 产生 invalid-credentials 校验错误后照常设 username/password,继续连 @ 后真主机)(https://url.spec.whatwg.org/ 【A】)——`http://a@127.0.0.1/` 各端实连 127.0.0.1,自研解析若把整段当 host 即 SSRF 绕过。
- **校准**:`43b983ac` 对 authority 含 `@` 一律拒(先于 scheme 判,报错不掩盖形态),强于 OWASP 最低要求;代价是合法带凭据 URL 被拒——对匿名抓取工具无合法用例,方向正确,保持。


---

## 四、SSRF 第三遍交叉验证(合并第一轮 + auditor 发现,对照 OWASP 全清单)

对照 OWASP SSRF Prevention Cheat Sheet 全清单(上引 【B】)逐项扫当前 HEAD(含 `43b983ac`):

| OWASP 条目 | 我们的状态 |
|---|---|
| Deny list(127/8、0/8、10/8、172.16/12、192.168/16、169.254/16、fc00::/7、fe80::/10、::1、**224.0.0.0/4、ff00::/8**) | 前八项全拦 + ::/96、::ffff:/96 还原(`43b983ac`);**multicast 两段缺**(v4 224.0.0.0/4、v6 ff00::/8)——第一轮 minor#3 的精确化 |
| Allow list / 连已验证 IP | 场景是任意网页抓取,域白名单不适用;「连已验证地址」未做,残余路径 = 攻击者 DNS(rebinding),见下 |
| 重定向 | 手动循环 ≤5 跳,每跳重新 parseUrl + scheme 白名单 + SSRF 复检 + userinfo 拒——逐跳全检,闭合 |
| DNS rebinding | 仍按 spec §14.2 声明接受(攻击者控制 DNS 前提);数字编码修复后该声明边界重新成立(第一轮已论证) |
| 解析器分歧(@、反斜杠) | `@` 一律拒(超 OWASP 最低要求);`\@` 形态含 `@` 同拒——闭合 |
| 十六进制/八进制/Dword/混合编码 | `43b983ac` inet_aton 语义解析 + fail-closed——闭合(除尾点,见新发现①) |
| scheme/其他协议 | scheme 白名单 http/https——闭合 |

**新发现(第三批)**:

1. **[major·一行修] 数字 IP 尾点形态 `http://127.0.0.1./` 放行**。`isNumericEncodedHost` 对尾点直接返回 false(`43b983ac` 版:`host.back() == '.'` 即 return false),`parseIpv4`/`parseIpv4NumericForms` 也都拒 → 落进「DNS 域名放行」分支。而 WHATWG URL 主机解析**明文容忍尾点**(剥掉最后一个空段再按 IPv4 解析):「If the last item in parts is the empty string…that empty item is removed」(https://url.spec.whatwg.org/ 【A】),curl 的 urlapi 对齐 WHATWG;系统解析器对「点分数字+尾点」也普遍接受。即该形态与 `127.0.0.1` 同连通、却绕过字面拦截。修法一行:把「尾点」从「不算数字形态」改为「剥一个尾点后再判数字形态,判中即拒」。建议随收口批带掉,配测试用例。
2. [minor] deny 表补 OWASP 点名的 multicast v4 `224.0.0.0/4` 与 v6 `ff00::/8`;第一轮的 100.64.0.0/10(CGNAT)等可一并(非 OWASP 条目,低成本)。
3. [minor·待实测] IDN 面:Windows 8+ `getaddrinfo` 默认对入参做 IDN/Punycode 转换(「Winsock performs Punycode/IDN encoding and conversion」,MS getaddrinfo 文档 【A】),全角数字经 UTS46 映射可变 ASCII 数字;但本链路是 ANSI 字节串经 libcurl,实际可达性低。标「理论存在、待实测」,不阻塞。
4. **响应侧内容嗅探缺失不是缺口**:OWASP 全文无响应侧条目;text/html 与 text/plain 两分支产物都以工具文本回流 LLM,嗅探不改变暴露面;charset/尺寸/时限三闸已在。
5. **「接受完整 URL」与 OWASP Case-2「只收 host、自行拼请求」的偏差**:这是抓取工具的存在意义,属场景性接受,记录即可。

**总判定:除新发现①(尾点)外,防线闭合于已修复项;①为一行 fail-closed,建议随收口批带掉。**

---

## 五、MCP 架构质量五维评估(权重最高项;结论供 B5/后续演进,均不阻塞 B4)

评估对象行数取 `a028fb1e` 提交版:module_mcp 6 文件(html_to_markdown 790 / mcp_tools_builtin 620 / mcp_protocol 292 / mcp_http_handler 156 / mcp_service 110 / mcp_session_store 95,合计 2133 行)+ core 接缝(mcp_tool.h/.cpp 共 141)+ app 桥(mcp_service_bridge 103)。

### 1.【精简】评级:优

层次 = platform/http(httplib 封装)→ handler(传输判序)→ protocol(JSON-RPC 纯逻辑)→ registry(core 依赖反转接缝)→ tools(实现),每层单一职责、无可合并的间接层(app 桥是 settings 模块隔离的必要缝,不算冗余);零第三方依赖(JSON/UTF-8 自研、httplib 为 vendored fork)。对照:官方 Python SDK 仅 `streamable_http.py` 一个传输文件就 ~1100 行(第一轮直读),我们全栈(含转换器)2377 行。唯一「层间杂味」:`HttpOutcome{status:404/202/400}` 把 HTTP 语义留在 protocol 层(见第 3 项),是精简换来的耦合,不是多余层。

### 2.【高效】评级:优(本规模)

热路径每请求:自研 JSON 单次解析 O(n) → 会话表 `unordered_map` 单锁查一次 → 注册表 `std::map` 单锁 find(返回 shared_ptr 保生命周期)→ 工具执行受 10s 预算。vendored httplib 线程池 = max(8, 核数-1) 基础线程 + 按需动态扩(httplib.h:185-186,11850-11856),无每连接一线程开销。三把锁(会话/注册表/路由)在单用户本地量级无竞争面;对照官方 SDK:Python anyio+starlette、TS 走 Node 事件循环,均为通用框架常数开销,本地单客户端场景我们天然更低。注册表单锁在工具数 <100 时无优化必要。

### 3.【灵活与可插拔】评级:合格

- 新增一个工具 = 写一个 `IMcpTool` 实现 + 在 `registerBuiltinMcpTools` 加一行(`mcp_tools_builtin.cpp:611-621`);更符合架构意图的路径是**任意 module 在 onStart/onStop 经 `globalMcpTools()` 注册/注销**(`core/mcp_tool.h` 头注释明写该设计),协议层与 module_mcp 对工具集合零感知,tools/list 动态反映注册表。
- 运行期插拔:`registerTool/unregisterTool` 线程安全可用(find 返回 shared_ptr,注销不悬垂在途调用);`listChanged` 恒 false(规范可选;工具多起来可补 notify)。
- **新传输(如 stdio)的侵入面**:protocol 返回值 `HttpOutcome{status,body,notificationOnly}` 携带 HTTP 状态码语义(404/400/202/415 在 `mcp_protocol.cpp` 决定),stdio 传输需一层「HttpOutcome → stdio 帧」适配;JSON-RPC 分发主体可复用。**这是当前最值得记账的传输耦合**——改法(供 B5,非现在):把「HTTP 状态码选择」从 protocol 挪到 handler,protocol 只产出 JSON-RPC 结果 + 语义枚举。
- 判据核对:「核心层对工具集合改动零感知」= 成立(protocol 只见 registry 引用)。

### 4.【能力独立性】评级:合格

- 依赖图干净:`web_fetch_markdown` → `html_to_markdown`(单向叶子模块)+ 注入 `Fetcher`(测试缝,`mcp_tools_builtin.h:20`);`devdesk_server_info` 只持注入的注册表只读引用(`mcp_tools_builtin.cpp:32-34`,注释明写「不读全局注册表」)——工具间零共享可变状态、零互相调用。
- 错误隔离:统一 `Result<ToolResult>` + 输出边界 `utf8::sanitize`;但**进程级隔离不存在**:execute 契约要求「线程安全/禁触 UI/长任务自行限时」(`mcp_tool.h`),未约定「不可抛」而 handler 链无 catch——一个工具抛未捕获异常会让工作线程 std::terminate 带走整个应用。官方 SDK 同为进程内执行(隔离靠「一 server 一进程」部署形态),与业界持平;演进:handler 适配层加 try/catch 映射 -32603(一行级,B5 可做)。
- 资源隔离:各工具自带限额(抓取 2 MiB/10s/5 跳),无全局资源池——成立。

### 5.【声明式】评级:欠佳(对照业界)/ 约束内合理

现状:命令式 C++ 类,每工具 4 个虚方法(name/description/inputSchemaJson/execute)+ 手写 schema JSON 字符串(`mcp_tools_builtin.cpp:42-45,469-472`),单工具样板约 30-40 行。

业界参照(全部一手):
- **TS SDK**:`server.registerTool("name", {title, description, inputSchema: z.object({...})}, cb)`——一条声明 + 一个函数,schema 用 zod 形状(https://raw.githubusercontent.com/modelcontextprotocol/typescript-sdk/main/README.md 【A】,2026-10-27 检索)。
- **Python SDK**:「`@mcp.tool()` def add(a: int, b: int)…——no JSON Schema (`a: int, b: int` *is* the schema), no request parsing, no validation code」,类型注解即 schema、docstring 即 description(https://raw.githubusercontent.com/modelcontextprotocol/python-sdk/main/README.md 【A】)。
- **Notion 官方 server**:OpenAPI 规范驱动,「MCP tools are discovered automatically when the server starts」,22 个工具由 spec 生成(https://raw.githubusercontent.com/makenotion/notion-mcp-server/main/README.md 【A】)。
- **GitHub 官方 server(Go)**:21 个 toolset 分组(--toolsets 开关)+ --read-only 跳过写工具 + README 目录自动生成(https://raw.githubusercontent.com/github/github-mcp-server/main/README.md 【A】)。

**距离**:距「工具 = 一条声明 + 一个纯函数」差一层样板。零依赖 C++ 演进路径(按性价比排序):
1. **表驱动注册函数(推荐,B5 时做)**:`registerMcpTool(registry, "web_fetch_markdown", "描述…", R"({"type":"object",…})", [](const std::string& argsJson) -> Result<ToolResult> {…})` + 泛型 `LambdaTool : IMcpTool` 适配器(约 40 行一次性代码),此后每工具 5-10 行;schema 仍为字符串,可加「parse 通过」单测锁漂移。
2. 宏/constexpr schema 构建器:工具上两位数再考虑,现在做是过度设计。
3. spec 驱动(OpenAPI 式):仅在「把整个产品面暴露为 MCP」战略出现时评估。
**值不值得**:第 1 步值得(成本一个适配器 + 一处注册点改造,收益是 B5 后每工具省 30 行样板、消 schema/实现漂移);时机 = 第 3 个工具落地时顺手做,**不阻塞 B4**。


---

## 六、新增问题清单(分级)

| # | 级别 | 问题 | 建议 |
|---|---|---|---|
| 1 | **major(建议随收口批)** | 数字 IP 尾点形态(`127.0.0.1.`)绕过 `43b983ac`:三道解析都拒它 → 落「域名放行」,而 WHATWG/curl 剥尾点后按 IPv4 实连 | `isNumericEncodedHost` 改为「剥一个尾点后再判,判中即拒」或 `ssrfRejectReason` 对去尾点后数字形态直接拒;补测试 |
| 2 | minor | deny 表缺 OWASP 点名的 multicast:`224.0.0.0/4`(v4)、`ff00::/8`(v6);可顺手补 100.64.0.0/10 | 补表,一行级 |
| 3 | minor | IDN/全角数字经 Windows 8+ getaddrinfo 默认 IDN 转换的理论绕过 | 待实测;可「host 含非 ASCII 即拒」(抓网页场景无合法用例) |
| 4 | minor | `McpService::stop()` 清会话表先于在途排干,在途请求的后续会话校验 404 | 语义无害(客户端重握手);求严谨则把 clear 挪到 `server->stop()` 之后 |
| 5 | minor | 工具 execute 未约定不可抛,handler 链无 catch → 未捕获异常 terminate 整进程 | handler 适配层加 try/catch 映射 -32603(B5) |
| 6 | info(接受) | DNS rebinding TOCTOU(spec §14.2 声明接受);接受完整 URL(OWASP Case-2 偏差,场景性);退出最坏挂起 ≤10s(有界) | 维持,无需动 |

第一轮遗留 minor 复核:嵌套列表 2 空格问题仅有序列表成立(见三(a),收口批按 4 空格修);「2 空格在无序列表也错」的说法由本轮规范原文**修正**。

## 七、对收口批 builder 的校准建议摘要

1. **markdown 转义**:权威口径 = 「任何 ASCII 标点可反斜杠转义、字母数字不可转义、行首转义终止块级解释」(GFM §6.1/§3.1);turndown 的 13 条上下文规则是合规低噪声子集,按第一轮计划移植;围栏代码块内容保持不转义。
2. **嵌套列表缩进**:统一 4 空格(turndown 对齐;规范安全域 = 标记宽 ≤4);有序/无序都按此;li 内续行只缩进不打标记。2 空格方案只对无序列表规范合法,不采用。
3. **SSRF**:在 `43b983ac` 基础上补尾点 fail-closed(新发现①)+ multicast 两段(②);userinfo 现状(一律拒、先于 scheme 判)已强于 OWASP 最低要求,保持不动。
4. **会话头**:无新增偏差,第一轮「无会话头请求 400」建议维持。
5. **架构演进**:全部标 B5+:LambdaTool 表驱动注册(第 3 个工具落地时)、protocol 去 HTTP 状态码耦合(加 stdio 传输前)、handler 兜底 catch——均不阻塞 B4。

## 待核清单(本轮仍未联网核验项)

1. Windows `getaddrinfo` 对 `127.1`/`2130706433` 的**实测**接受度:文档口径(dotted-decimal)与 Winsock 家族实践(WSAStringToAddress 接受 `1.2.3`)有张力;因 curl 已在客户端归一,此项只影响「其他直连解析路径」的理论评估,不影响本仓正确性——建议保持「数字形态一律拒」的 fail-closed,不依赖平台实测。
2. Go `http.Server.Shutdown` 文档逐字引文(pkg.go.dev 正文抓取截断,引文经检索摘要复核)。
3. Windows TIME_WAIT 现代默认 120s(旧文档 240s)两说并存,本文按「2-4 分钟(TcpTimedWaitDelay 可调)」表述。
4. safeurl 类 SSRF 库「默认拒 credentials」惯例佐证:仓库抓取 404,未证实;userinfo 结论不依赖它(OWASP '@' 范例 + RFC 3986 已足)。

## 引用清单

| # | 来源 | 等级 | 用途 |
|---|---|---|---|
| 1 | https://learn.microsoft.com/en-us/windows/win32/winsock/using-so-reuseaddr-and-so-exclusiveaddruse | A | REUSEADDR 劫持语义 / EXCLUSIVEADDRUSE MUST / TIME_WAIT 代价 / SO_LINGER 劝退原文 |
| 2 | https://learn.microsoft.com/en-us/windows/win32/api/winsock2/nf-winsock2-inet_addr | A | 四种形态 + 十/八/十六进制 + 混合进制官方定义 |
| 3 | https://learn.microsoft.com/en-us/windows/win32/api/ws2tcpip/nf-ws2tcpip-getaddrinfo | A | numeric string 口径、AI_NUMERICHOST、Win8+ IDN 默认转换 |
| 4 | https://www.rfc-editor.org/rfc/rfc6943.txt | B | §3.1.1 loose vs strict、`10.0.258`/`0xA000102` 同址例、strict form 建议 |
| 5 | https://www.rfc-editor.org/rfc/rfc3986.txt | A | §3.2.1 userinfo 语法、user:password 弃用、误植主机名原文 |
| 6 | https://url.spec.whatwg.org/ | A | IPv4 解析器全规则(进制/段数/尾点容忍/ends-in-a-number)、userinfo 解析 |
| 7 | https://github.github.com/gfm/ | A | 列表缩进 W+N 规则、Example 272/274/275、反斜杠转义、块级优先 |
| 8 | https://raw.githubusercontent.com/commonmark/commonmark-spec/master/spec.txt | A | 同上规则的 CommonMark 底本复核 |
| 9 | https://cheatsheetseries.owasp.org/cheatsheets/Server_Side_Request_Forgery_Prevention_Cheat_Sheet.html | B | deny 全表(含 multicast)、重定向、rebinding、解析器分歧 '@' 范例 |
| 10 | https://blog.dreamfever.me/posts/2026-09-27-curl-127-1 | B | curl `urlapi.c ipv4_normalize` 客户端归一机制、glibc `__inet_aton_exact`/CVE-2016-10739 |
| 11 | 本仓 `3rd/cpp-httplib/httplib.h`(v0.58.0 fork,直读) | A | ThreadPool::shutdown 排干+join、keep-alive 5s、上游默认 REUSEADDR、listen_internal 退出调 task_queue->shutdown |
| 12 | https://github.com/yhirose/cpp-httplib/issues/1548 | A | stop 与 listen 状态交互的上游语义佐证 |
| 13 | https://pkg.go.dev/net/http#Server.Shutdown | A(引文部分未逐字核验) | 优雅停机「关监听→清空闲→等在途」口径 |
| 14 | https://en.cppreference.com/w/cpp/utility/program/atexit.html | A | atexit 反序执行、与静态析构的关系 |
| 15 | https://learn.microsoft.com/en-us/biztalk/technical-guides/settings-that-can-be-modified-to-improve-network-performance | A | TcpTimedWaitDelay 控制 TIME_WAIT 时长 |
| 16 | https://stackoverflow.com/questions/29285954 | C | WSAStringToAddress 接受 `1.2.3` 的社区实证 |
| 17 | https://raw.githubusercontent.com/modelcontextprotocol/typescript-sdk/main/README.md | A | registerTool 声明式模式、transport connect() 可插拔 |
| 18 | https://raw.githubusercontent.com/modelcontextprotocol/python-sdk/main/README.md | A | @mcp.tool()「类型注解即 schema」原文 |
| 19 | https://raw.githubusercontent.com/github/github-mcp-server/main/README.md | A | 21 toolsets 分组、--read-only、--tools、README 目录自动生成 |
| 20 | https://raw.githubusercontent.com/makenotion/notion-mcp-server/main/README.md | A | OpenAPI 规范驱动工具发现、22 工具 |
