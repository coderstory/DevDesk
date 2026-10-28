# 审计：MCP 代理参数批 + CA 批（eab2c496 / cf990da3）

- 审计者：独立只读审计子任务（GLM-5.3-Flash），与实现者 devdesk-builder 无会话延续关系，未参与本轮任何改动。唯一写动作 = 本文件 + RESEARCH-INDEX 登记。
- 对象：worktree `D:\wt-mcp-skeleton`，分支 `feat/mcp-server-skeleton`
  - `eab2c496` feat(mcp): web_fetch_markdown 本地代理参数（8 文件 +389/-45，7+1 新测试）
  - `cf990da3` fix(mcp): Windows CA 覆盖口 DEVDESK_CA_BUNDLE（6 文件 +143/-4，3 新测试）
- 方法：`git show` 全量 diff 亲手取证 + 现行提交态源码逐点核对（工作区 `src/module_mcp/html_to_markdown.cpp` 为 builder 未提交内容，未触碰、未构建、未跑测）。六维度按 CLAUDE.md「功能开发生命周期」阶段 2（CLAUDE.md:330-341）。
- 行号基准：HEAD = cf990da3 的提交态（除上述 builder 脏文件外与现行文件一致）。

---

## 六维度逐条结论

### 1. 正确性 — 已核，核心场景成立，3 处缺陷

对照诉求逐条核：useProxy 自动探测（7897→10808→51295、300ms、调用内缓存、全落空直连附注）、显式 proxy（形态限定、TCP 预检失败报错不触网、与 useProxy=false 冲突报错）、SSRF 每跳复检不动（mcp_tools_builtin.cpp:692 起，eab2c496 未触碰循环体）、CA 覆盖口（未设→NATIVE 不变；存在→CAINFO 换根不叠 NATIVE；不存在→可读 fail）——主路径实现与 spec §6.1 / plan §8/§9 口径一致。**但**：

- F1（探测在环境代理在场时失真）：`tcpReachable` 未钉扎代理环境变量，见 findings。
- F4（并发首装竞态）：`tcpReachable` 未调 `globalInit()`，MCP HTTP 面是线程池并发派发（platform/http_server.cpp:3），见 findings。
- F6（CA 路径校验缺口）：目录路径 / 异常路径绕过「可读报错」契约，见 findings。

isValidProxySpecifier（mcp_tools_builtin.cpp:444-466）逐形态核过：端口 0/负/越界（`value<1||value>65535`，5 位上限无溢出）、空白/控制字符（非数字点即拒）、IPv6/方括号/@（显式拒）、纯数字无点整数形（拒）、冒号切分用 `rfind`（`1.2.3.4:8080:9090` 拒）、`:8080` 空主机（colon==0 拒）、首尾空白（:647 trimAscii 先行）——未发现可绕过形态。`1.2.3.4.5:8080` 这类非合法点分四段可通过校验，但 curl 侧实连失败会如实报错，不构成绕过（informational，不立 finding）。

### 2. 回归面 — 已核，无断言级回归，2 处卫生问题

- NOPROXY 钉扎改动有 `req.proxy.empty()` 守卫（http_client.cpp:92-99）：`Request.proxy` 全仓唯一赋值点 = mcp_tools_builtin.cpp:724，其余全部 http::send 调用方（claude_proxy 等）走原 else 分支，逐字节不变。已核。
- 既有 34 处用例经 `legacyProxyOpts()`（tests/test_modules/test_mcp_fetch_tool.cpp，实测 34 调用点 + 1 定义 = 35 行命中）注入恒真探针，ScriptedFetcher 忽略 `request.proxy`，输出确定性不变。已核。
- 但 **2 处既有用例漏注入**（test_mcp_fetch_tool.cpp:619、745 用默认 `ProxyOptions{}` → 生产探针真触 127.0.0.1:7897/10808/51295），见 F5。输出仍确定（探针结果只影响 request.proxy，ScriptedFetcher 不读），无红绿影响。
- CA 改动对未设 env 的默认路径：`resolveCaPolicy("")` → Native → `applyCaOptions(curl)` 原样（http_client.cpp:107-108、454-455），既有行为零变化。已核。

### 3. 平台对称性 — 已核，1 处风险未验证

- 代理批：探测与接线全在 libcurl 层，跨平台同路径。已核。
- CA 批：`resolveCaPolicy` 平台无关纯函数（http_client_ca.h:33），send/sendStreaming 双调用点同口径；http_client_ca_win.cpp 注释修正如实（libcurl 库不读 CURL_CA_BUNDLE 属实，为 curl CLI 约定）。macOS 分支 `applyCaOptions` 为 no-op，`DEVDESK_CA_BUNDLE` 在 macOS 同样生效（CAINFO 显式覆盖默认路径），对称。已核。
- F7：非 ASCII bundle 路径在 Windows 的 `getenv` 窄字节 → `std::filesystem::path` 窄转换 → `CURLOPT_CAINFO` 窄路径三级编码链路存在 ACP/UTF-8 错配风险，零测试覆盖，见 findings。

### 4. 静默失败面 — 已核，3 处

- 好面先记：显式代理预检失败如实 isError 不回落直连；自动探测全落空追加「未检测到本地代理，已直连」附注（mcp_tools_builtin.cpp:809-811）——降级有痕，是好设计。
- F1 的失败形态：env 代理在场时探测结果指向「代理的回环」，全误报 → 静默降级直连（有附注但根因不可见）或误选候选。
- F3：`sendStreaming` 完全忽略 `Request.proxy`（http_client.cpp:444 无 CURLOPT_PROXY），未来流式调用方静默直连。
- F2：CA 接线无断言，回退不会红（归入测试有效性，此处指其静默性：删掉 CURLOPT_CAINFO 分支后三测仍绿，静默假绿）。
- F6：`std::filesystem::exists` 无 error_code 重载（http_client.cpp:56），非法/权限路径抛 `filesystem_error` 穿透 Result 语义——非静默，但错误形态脱离既定可读 fail 口径。

### 5. 测试有效性 — 已核，11 新测中 8 真断言，1 组假绿风险

- 代理 7 测（test_mcp_fetch_tool.cpp:401-530）变异核过：`script.requests()[0].proxy == "127.0.0.1:7897"` 精确断言代理已下发（删 `request.proxy = proxy` 即红）；`probe.probed.size()==1` 断言优先级短路（候选表重排即红）；全落空用例断言精确输出 `ok\n未检测到本地代理,已直连` 与 `probed.size()==3`；冲突/预检败用例断言 `script.requestCount()==0` + `probe.probed.empty()` 双证不触网；形态矩阵 6 个坏形态逐个断言 isError+「仅支持」+ 零请求。均为真断言，非恒真。
- schema 测（test_mcp_tools_builtin.cpp:175-198）：断言 properties 键、类型、required 恒为 url 一项——真断言。
- CA 3 测（test_http_client.cpp:209-250）：resolveCaPolicy 直测三态，断言 useNative/caInfo/错误串含变量名与路径——纯函数层真断言。**但接线层（CA-02）零覆盖**：变异「删 else CURLOPT_CAINFO 分支」三测全绿，F2 立假绿 finding。先红后绿证据：代理批 6 红（台账 plan §8 批次证据回填）、CA 批编译期红（符号未声明）——对纯函数成立，对 send/sendStreaming 接线不成立。
- `--filter http` 23/23 等门禁计数为 builder 自报，本审计只读未复跑（约束：不跑构建）。

### 6. 文档口径 — 已核，2 处过宽/欠准

- spec §6.1 第 13 条、plan §8/§9、schema 描述、http_client_ca.h/ca_win.cpp 注释修正均与新行为一致；「libcurl 库不读 CURL_CA_BUNDLE」实测论断技术上正确。已核。
- F5 关联口径：提交信息「既有用例经 legacyProxyOpts……输出逐字节不变」过宽——619/745 两处并未经 legacyProxyOpts。
- F3 关联口径：http_client.h:26 契约注释「非空 = 经此 HTTP 代理」未写「仅 send() 生效」，与 sendStreaming 实际行为矛盾。
- 小口径（informational，不立 finding）：平台级 CA 口径登记在 spec §6.1（web_fetch_markdown 工具行为清单）之下，正文已写明「适用于 send 与 sendStreaming 全部出站请求」，可接受。

---

## Findings 清单（severity 按难维护程度定）

| # | 维度 | severity | file:line | 问题 | 影响 | 建议 |
|---|---|---|---|---|---|---|
| F1 | 正确性/静默失败面 | **中** | `src/platform/http_client.cpp:154-171` | `tcpReachable` 未设置 `CURLOPT_PROXY`/`CURLOPT_NOPROXY`：libcurl 默认读 http_proxy/https_proxy/all_proxy 环境变量（同文件 ：98 注释自己记载了这一坑）。env 代理在场时，探测 127.0.0.1:X 实为「经第三方代理连**代理的**回环」 | 探测结果失真：全误报 → 静默降级直连（附注掩盖根因）；或候选端口恰在代理主机侧开放 → 误选本地不可达代理 → 该会话所有 fetch 失败 | 探测句柄加 `CURLOPT_NOPROXY, "*"`（或空 PROXY + NOPROXY 钉扎），一行修 |
| F2 | 测试有效性 | **中** | `src/platform/http_client.cpp:70-77,107-112,407-415,454-459` | CA 接线（双调用点先解析后套用）零自动化断言；3 测只直测 `resolveCaPolicy` 纯函数。变异验证：删掉 else 的 `CURLOPT_CAINFO` 分支，全部测试仍绿 | CA-02 的「接入」无红绿证据，后续重构可静默回退 NATIVE-only（Windows 缺根证书场景复发而 CI 不可见）；bundle-missing 的 send() 可读 fail 路径（:70-77）无需网络即可测却未测 | 补用例：测试内 setenv 指向缺失路径 → 断言 send() 返回 fail 且错误串含变量名与路径（顺带覆盖接线入口）；CAINFO 下发本身可留 live 层 |
| F3 | 静默失败面/对称性 | 中低 | `src/platform/http_client.cpp:444`；`src/platform/http_client.h:26` | `sendStreaming` 忽略 `Request.proxy`（仅 send() 有 CURLOPT_PROXY 分支）；Request 字段契约注释未声明「仅 send 生效」 | 未来流式调用方（如 SSE 抓取）设 proxy 后静默直连，行为与契约相反而无任何报错/日志；维护者需跨文件推理才能发现 | 短期：注释写明「仅 send() 生效，sendStreaming 忽略」；长期：sendStreaming 同样接线或显式 fail |
| F4 | 正确性（并发） | 中低 | `src/platform/http_client.cpp:154`（对照 ：67、:405） | `tcpReachable` 未调用 `globalInit()`；send/sendStreaming 均先调（:46-49 注释明言 curl_global_init 非线程安全）。MCP HTTP 面由 cpp-httplib 线程池并发派发（platform/http_server.cpp:3） | 进程首个 libcurl 触碰若为并发探测，`curl_easy_init` 隐式 `curl_global_init` 并发 = 数据竞争（UB），且仅在「MCP 先于一切其他 HTTP」时触发，极难复现 | 函数首行补 `globalInit();`，一行修 |
| F5 | 回归面/文档口径 | 低 | `tests/test_modules/test_mcp_fetch_tool.cpp:619,745` | 两处既有用例用默认 `ProxyOptions{}`，单元测试真触 127.0.0.1:7897/10808/51295（最坏 3×300ms）；提交信息「既有用例经 legacyProxyOpts……逐字节不变」口径过宽 | 单测触真端口违背本文件 SSRF 用例「不触网」纪律；跑测时长受开发机环境影响；口径失实误导后续维护者 | 两处补 `legacyProxyOpts()`（或显式 useProxy=false 参数），提交台账口径勘误 |
| F6 | 静默失败面 | 低 | `src/platform/http_client.cpp:56` | `std::filesystem::exists(envValue)` 用抛异常重载：非法/权限路径抛 `filesystem_error` 穿透 Result 语义；目录路径 `exists()==true` 通过校验 → CAINFO 指向目录，perform 期才报 libcurl 英文错 | 脱离「可读中文报错」契约（spec §6.1-13 只写「文件不存在」形态）；异常路径未定义由谁接 | 改 `exists(p, ec)` + `is_regular_file`，ec 非空并入可读 fail |
| F7 | 平台对称性 | 低 | `src/platform/http_client.cpp:70,408`；`src/platform/http_client_ca.h:22` | 非 ASCII bundle 路径零覆盖：Windows `std::getenv` 返回 ACP 窄字节，经 `std::filesystem::path` 窄转换与 `CURLOPT_CAINFO` 窄路径，三级编码链路（ACP↔UTF-8）存在错配风险，本次未验证 | 中文等路径可能 exists() 误判不存在（fail-closed 不静默，可接受），但报错串路径乱码影响可诊断性；macOS/Linux UTF-8 链路无此忧 | 加一条非 ASCII 路径用例验证，或在 http_client_ca.h 注释声明「bundle 路径仅保证 ASCII」 |
| F8 | 正确性 | informational | `src/module_mcp/mcp_tools_builtin.cpp:641-647` | `useProxy`/`proxy` 类型不符时静默按默认处理（`"false"` 字符串、`0` 数字均视同默认 true 并真探测），不报 -32602 | 模型侧传错类型时行为与调用者意图相反且无提示；与既有 max_chars 静默钳制惯例（:636-637）一致，非新债 | 可不修；schema 描述补「必须 boolean/string」即可 |
| F9 | 正确性 | informational | `src/module_mcp/mcp_tools_builtin.cpp:672-687` | 探测缓存 `std::map` 在候选表全唯一时永不命中（探测本就在循环前完成一次） | 死防御代码，无害；「重定向不重复探」注释口径实为「解析在循环前仅一次」 | 可不修；候选表未来扩充时保留亦可 |

「未发现问题」亦须写明已核的项：SSRF 每跳复检未动且经 legacyProxyOpts 用例守护（已核）；CURLOPT_PROXY 显式代理时 `NOPROXY ""` 语义正确（空 no-proxy 表 = 全目标过代理，env NO_PROXY 被显式清空）（已核）；`tcpReachable` 句柄单实例 `curl_easy_cleanup` 全路径释放、CONNECT_ONLY + FRESH_CONNECT + TIMEOUT_MS 组合正确（已核）；CA 双调用点解析先于句柄创建、流式失败可读 fail（已核）；`isValidProxySpecifier` 形态面（端口越界/注入/IPv6/@/整数形）不可绕过（已核）。

---

## 判定

**有条件通过**。核心设计（探测注入缝、显式代理 fail-loud、直连附注、CA 换根不弱化）成立且测试主断言为真断言；不构成打回。进入阶段 3 时：

- 建议必修：F1（一行）、F4（一行）、F2（补一个无网用例）。
- 修或写明不修理由：F3、F5、F6、F7。
- F8、F9 informational，可不动。

每条处置后按阶段 3 由审计方（或另一独立子任务）复查。
