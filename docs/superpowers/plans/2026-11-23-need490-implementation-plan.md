# need#490 网页终端实现切片计划（W1–W7）

- 对齐基线：PRD `2026-11-23-web-terminal` **v12**（PM 升版中，收编 F-27/F-28 与功能干跑 P1；v11 未发布）、技术卡 `2026-11-23-web-terminal-tech` **v9**、ADR 0009、ADR 0010 **v2**、UI 设计 + 39 屏 demo。
- 范围：技术卡 §16 的 F-01～F-26，加老板 2026-11-24 裁决的 **F-27（网页全屏）**、**F-28（手机最小键带 Esc+Tab）** 与功能干跑 P1 十一条（映射见 §1.4）。本计划只切实现顺序，不改产品/协议裁决；冲突以 ADR > PRD > 技术卡为序。
- 已有资产（直接复用，不重造）：前端四视图骨架 `src/module_terminal/web/www/`（1422 行）、自研二维码 `src/module_terminal/web/qr/qr_encode.{h,cpp}` + `tests/test_modules/test_qr_encode.cpp`、Playwright 冒烟 `tests/e2e/`、`platform::http::Server` 路由/流式 provider/Host 白名单/客户端地址能力。

## 0. 前置状态

- **bug#491 已关闭**（commit `fa5212ef`，补 `release_detail_internal.h` 中 `prereqSectionHeight` 声明；`bash scripts/build-test-run.sh` 跑 2558 例、0 失败）。W1–W7 的构建断点已清，不再有外部阻塞。
- **新前置：bug#493 Windows CI 红**（HTTP 客户端侧 httplib 撞 OpenSSL 4.1 弃用 + `-Werror`，与已登记 bug#390 同根因，合并为一条修）。只阻塞 W7 的「双平台门禁绿」判据；W1–W6 在 macOS 单平台推进，W7 开工前必须至少一次 Windows CI 绿（macOS 本机 homebrew openssl@3 无法复现，只能 CI 实跑验证）。

## 1. 全局约定

1. **构建/测试唯一入口**：`bash scripts/build-test-run.sh`；开发内循环 `--dev --filter file:<文件子串>`；收口/发版才不带 `--dev` 完整跑。
2. **测试框架**：自制迷你 doctest（`REGISTER_TEST`，fail-fast）。每条用例必须能答"删掉被测代码会不会红"；不写覆盖率式用例。
3. **模块隔离**：`module_*` 之间零 include、零链接边；跨模块一律经 `core` 接口 + `core::ServiceLocator`（先例：`core::IClaudeProxyService`）。
4. **平台拆分**：平台差异只放 `*_posix.cpp` / `*_win.cpp`，业务代码零 `#ifdef`。
5. **证据**：每片产出定向用例输出摘要；UI 片用探针 `--screenshot` 出图；证据存 `evidence/`，路径写回测试清单。
6. **提交**：每片完成以 `wip:` commit 落盘（显式 pathspec），收尾 squash 为单个 `feat:`；禁 `git add -A`。
7. **注释纪律**：只写契约/反重构警告，不复述代码行为；本计划新增注释同样受约束。
8. **worktree 环境**：新建 worktree 内子模块全空（`EUI-NEO` / `third_party/libgit2` / `third_party/libvterm` 未初始化，`git submodule status` 显示 `-`，`CMakeLists.txt:209` 的 `add_subdirectory` 直接失败）。开工前固定执行 `git -c http.proxy= -c https.proxy= submodule update --init --remote`（含 third_party 全部 4 个）。`build-test-run.sh` 只对 EUI-NEO 给提示，其余两个子模块缺失时只报 CMake 原始错误——别误判成代码问题。
9. **代理绕过**：本机全局 `http.proxy=http://127.0.0.1:7879` 无监听，任何网络 git 操作必须显式 `git -c http.proxy= -c https.proxy= …`，否则拉取必失败。
10. **证据口径**：证据一律取 `--filter file:<子串>` 输出摘要；**禁把绝对 build 路径写进命令**（`*/build/*` 被 bash 闸拦）。探针放 `tests/probes/`（`tests/CMakeLists.txt` 用 `file(GLOB probes/*.cpp)` 注册）；CI headless 档 `DEVDESK_BUILD_PROBES=headless` 只编 3 条白名单探针——**新增 web 探针仅本地全量门禁编译运行，CI 不覆盖**。
11. **测试入口（E2E 启动形态）**：服务开关按 P-02 不持久化，E2E 无法靠点桌面 UI 开启，故新增 **debug-only 启动参数**（release 构建不编译、不识别，与既有 `DEVDESK_WEB_TEST_HOOKS` 同门控）：
    - `DEVDESK_WEB_AUTOSTART=1`：启动即开网页服务（等价设置页开关置开）；
    - `DEVDESK_WEB_PORT=<n>`：固定监听端口（缺省/未设 = 0 自动），E2E 用它拼确定 URL；
    - 数据根沿用既有 `DEVDESK_DATA_ROOT`（`src/core/paths.h` 优先级链），E2E 每次起独立目录，绝不碰真实数据根。
    - 实现落 W3（`web_terminal_service.cpp` 读环境 + `module_terminal` onStart 接线），接口形态在 W1 头文件注释里定死；W7 的 Playwright/协议客户端消费；`scripts/check-product-deps.sh` 断言 release 二进制不含这三个符号。

### 1.1 复用决定（reuse-first，命中即停）

| 候选 | 决定 | 理由 |
|---|---|---|
| 二维码编码器 | **复用** `web/qr/qr_encode`，不建 `web_qr.*` | 已交付且 4 条单测绿；只需在服务内做 URL 拼接（`#b=`）与 78/106B 容量判断 |
| HTTP 服务 | **复用** `platform::http::Server`，不改 `http_server.*` | 现有 `route` / `setChunkProvider`（writer + isWritable + releaser）/ `RequestCtx.clientAddress` / `setAllowedHosts` / `actualPort` 已覆盖需求；SSE 15s 心跳阻塞在 `web_bridge` 内用条件变量实现，不污染通用 server |
| 终端快照 | **复用** `SessionController` / `TerminalFrame` / `pollAll` | 网页不另建终端引擎，不给后端原始字节出口 |
| 设置持久化 | **复用** `app_config` 单行 JSON 模式 | 与 `terminal_settings` 同款；无 schema 变更 |
| 核心颜色映射 | **复用** `terminal_grid_widget` 内颜色解码，导出为公共函数 | 网页与桌面同色，禁止两份映射漂移 |

### 1.2 与技术卡的一处实现层偏离（需主会话知悉）

技术卡 §2.1 把桌面「网页访问」卡放在 `module_terminal/web/web_settings_card.cpp`。本计划改为 **`src/module_settings/web_access_card.{h,cpp}`**，由 settings 页持有绘制，数据经 `core::IWebTerminalService` 只读快照获取。
- 理由：卡片是设置页组成单元，settings 模块已有一套卡样式/几何；若放 terminal 模块，settings 必须 include/链接兄弟模块，违反模块隔离铁律。
- `core/web_terminal_service.h` 因此只放**数据接口**（无 `eui::Ui&` 透传），QR 以模块矩阵（`std::vector<uint8_t>` + size）跨层，不把 EUI 类型塞进 core。
- 尺寸/位置/文案不变，仍按 PRD §9.1 与 UI 设计稿；QR 绘制由 settings 卡用 `ui.rect` 完成。

### 1.3 文件总量估算

新建约 31 个、修改约 17 个，合计约 48 个文件（不含 Playwright lockfile 变更）。

### 1.4 功能干跑 P1 并入矩阵（11 条，来源 `2026-11-23-need490-feature-dryrun.md`）

| ID | 缺口 | 落入 | 落点要点 |
|---|---|---|---|
| B1 | 断线期间输入丢失 | W5 | `protocol.js` RealClient 本地待发队列 + 「未发送」标记，重连后按序补发 |
| B2 | 重连后滚动位置回底 | W5 | `render.js` 记住 scrollOffset，重连 `full` 帧后按 scrollback 对齐到原偏移 |
| A1 | 桌面重启后 PTY 全灭、网页只看到空态 | W4+W5 | W4 `session` 事件带 `reason=desktop_restart`；W5 空态文案 + 「重开」入口 |
| F1 | 握手全站 32 次/分钟可被同网段他人耗尽 | W2+W3 | W2 限流键改 per-client（回环/局域网按 IP，**叠加** 12 次/分钟；全站 32 保留）；W3 从 `RequestCtx.clientAddress` 取键 |
| B3 | 切后台/锁屏/来电回前台不主动重连 | W5 | `visibilitychange` / `pageshow` 立即重建连接，不等 45s 半开判定 |
| A2 | 换 Wi-Fi / 改端口后旧地址失效，文案误导为桌面关闭 | W5+W6 | W5 断线覆盖层区分「连不上，可能换了网络或端口」；W6 地址列表随网卡变化刷新、换端口后 QR 与地址重建 |
| C2 | tab 标题固定「终端 N」不可辨识 | W4+W5 | W4 `session.title`（OSC 0/2 标题，回落前台进程名/目录名）；W5 tab 显示标题 + 双击重命名（会话内存态，不持久化，重开回默认） |
| D1 | 手机断开后桌面仍保持手机列宽 | W4 | bridge 记录网页接管前的桌面行列，最后一个网页订阅退出时恢复桌面尺寸 |
| E1 | 无自诊入口 | W3+W6 | W3 快照 `error` 槽写最近一条失败原因；W6 设置卡显示最近错误 + 「打开日志目录」 |
| E2 | Windows 首次监听被防火墙拦，桌面无提示 | W6 | 卡内常驻「手机连不上？检查防火墙」帮助文案；平台判定经 `core` 暴露，不写 `#ifdef` |
| F2 | 二维码截图被转发，他人可抢先绑定且桌面无告警 | W6 | 绑定成功显式提示「已绑定新验证器」+ 一键撤销重绑 + 风险文案 |

P2 二十一条留档在干跑报告，本轮不做。

## 2. 切片总览

| 片 | 主题 | 依赖 | 构建次数（预计） | 产物 |
|---|---|---|---|---|
| W1 | 平台底座：crypto / net / core 接口 | —（bug#491 已关闭） | 1–2 | `platform::web_crypto`、`platform::net`、`core::IWebTerminalService` |
| W2 | 鉴权：令牌 / TOTP / 票据 / 限流（含 per-client） | W1 | 1–2 | `web_auth`、`web_settings` + 单测 |
| W3 | 协议：握手 / 信封 / 路由 / SSE / 服务骨架（含 E2E 启动参数） | W1,W2 | 2–3 | 可握手的 HTTP 服务 + 服务级单测 |
| W4 | 网格与会话桥：codec / 增量 / tab 标题 / 尺寸恢复 | W3 | 1–2 | `web_grid_codec`、`web_bridge` 完整事件 |
| W5 | 前端接线：纯 JS 加密 / SSE / 四视图 / 全屏 F-27 / 键带 F-28 | W3,W4 | 1 | `www/crypto.js` + RealClient + 视图 |
| W6 | 桌面端：设置卡 / QR / 启动目录 / 自诊与防火墙提示 | W1,W2,W3 | 2 | 设置页专区 + 启动目录设置项 |
| W7 | 测试收口：协议客户端 / Playwright（CI 步骤补齐）/ 探针 / 真机清单 | W1–W6 + bug#493 | 2–3 | S-01～S-33 自动化 + 人工清单 |

串行派发顺序：`W1 → W2 → W3 → W4 → (W5 ∥ W6) → W7`。W5 与 W6 无共享文件，可并行（同一时刻只有一个构建任务）。

---

## 3. W1 平台底座

**目标**：把加密原语、网卡枚举、跨模块接口一次立住，后续切片只调不改。

**依赖**：无（bug#491 已关闭，见 §0）。

**新建**
| 文件 | 内容 |
|---|---|
| `src/core/web_terminal_service.h` | `IWebTerminalService` 抽象接口 + POD 快照（`WebAccessSnapshot`：running/port/addresses/bindState/bindTime/lastLogin/onlineCount/qrMatrix/qrSize/tokenRemainingSec/error）；`TerminalSettingsPatch`（fontFamily/fontSize/colorMode/palette/shell/startupDir）；`writeSettings()` 单一写入入口声明；`start/stop/refreshToken/revokeBinding`；注释固定 debug-only 启动参数契约（`DEVDESK_WEB_AUTOSTART` / `DEVDESK_WEB_PORT`，仅 Debug 编译，§1.11） |
| `src/platform/net_interfaces.h` | `std::vector<std::string> localIpv4Addresses()`；`bool isLoopbackAddress(const std::string&)`（去重、排除 0.0.0.0） |
| `src/platform/net_interfaces_posix.cpp` | `getifaddrs` 枚举 AF_INET，过滤 down/loopback 按需 |
| `src/platform/net_interfaces_win.cpp` | `GetAdaptersAddresses(AF_INET)` 枚举 |
| `src/platform/web_crypto.h` | OpenSSL 封装：`randomBytes`、`constantTimeEquals`、HMAC-SHA1/SHA256、HKDF-SHA256（extract/expand）、X25519 生成/共享、ChaCha20-Poly1305 seal/open、base64url、base32 |
| `src/platform/web_crypto.cpp` | 上述实现（两端同一份，OpenSSL 已链） |
| `tests/test_modules/test_terminal_web_crypto.cpp` | RFC 7748 §5.2/6.1、RFC 5869 A.1–A.3、RFC 8439 §2.8.2/A.5 向量；往返、篡改、错密钥、nonce 方向隔离；base64url；AEAD 信封开销 = 29B |
| `tests/test_platform/test_net_interfaces.cpp` | 至少含 127.0.0.1；无重复；loopback 判定 |

**修改**
| 文件 | 最小接线理由 |
|---|---|
| `src/platform/CMakeLists.txt` | 追加 `web_crypto.cpp` 与 net 平台源（按 WIN32 选 `_win`/`_posix`） |
| `tests/CMakeLists.txt` | 直编 `web_crypto.cpp`（与 `qr_encode.cpp` 同款）+ 追加两个测试文件 |

**完成判据（可判）**
- `--filter file:test_terminal_web_crypto` 全绿；三条 RFC 套件逐字节命中官方向量。
- 解封失败路径返回错误而非空数据；同一密钥复用 nonce 的封装测试被拒绝（或调用方断言不可达）。
- `localIpv4Addresses()` 在两平台各返回含回环的地址集；`isLoopbackAddress("127.0.0.1")==true`、`("::1")==false`（IPv4 only 口径）。
- `core/web_terminal_service.h` 可被任意模块 include 且不引入 platform/EUI 头。

**证据**：定向用例输出 + 向量来源注释（RFC 章节号）。
**构建次数**：1–2。
**回滚**：删除本片新增文件、还原两处 CMake；无数据库/配置变更。

---

## W2 鉴权

**目标**：绑定令牌状态机、TOTP、会话票据与吊销表、限流与锁定全部以纯逻辑落定，先不接 HTTP。

**依赖**：W1。

**新建**
| 文件 | 内容 |
|---|---|
| `src/module_terminal/web/web_auth.h/.cpp` | 令牌 32B→base64url、5 分钟、`unclaimed/claimed/terminal` 三态、claim 幂等；TOTP（20B 密钥、base32、HMAC-SHA1、T=unix/30、±1 步、定长比较）；票据签发/校验（`ver1‖expiry8‖ticketId16` + HMAC-SHA256）；吊销表（≤64、7 天、退出删 ID、撤销清表）；登录锁定 5 次/60s；握手限流两层——全站 32 次/60s + **per-client 12 次/60s**（P1-F1，键由调用方给，回环与局域网同口径；无键时只走全站口径）；失败事件仅记日志 |
| `src/module_terminal/web/web_settings.h/.cpp` | `app_config` 键 `terminal_web`：端口、绑定密钥/绑定时间、最近登录、票据表；服务开关**不持久化**（P-02，每次启动为关）；损坏 JSON 回默认并 `LOG_ERROR` |
| `tests/test_modules/test_terminal_web_auth.cpp` | RFC 6238 向量、±1/±2、base32、claim 两次同密钥、confirm 消费、5 次错作废、票据签名/过期/吊销/64 上限、锁定、全站限流窗口、**per-client 限流键隔离与 12 次上限**、回环免验纯函数 |
| `tests/test_modules/test_terminal_web_settings.cpp` | 往返、缺键/坏 JSON 回默认、旧键不读、票据表 64 上限保持 |

**修改**
| 文件 | 最小接线理由 |
|---|---|
| `src/module_terminal/CMakeLists.txt` | 追加 `web/web_auth.cpp`、`web/web_settings.cpp` |
| `tests/CMakeLists.txt` | 追加两个测试文件 |

**完成判据（可判）**
- `--filter file:test_terminal_web_auth` 全绿；RFC 6238 官方向量（T=59/1111111109/…）命中。
- claim 在 `unclaimed` 与 `claimed` 均返回同一 `otpauth` 密钥；`terminal` 拒绝。
- 票据：第 65 条新增返回"票据满"语义（由 W3 映射 409），已有 64 条不被清理。
- 握手限流第 33 次/分钟内被拒；锁定期间提交不校验、不计次。
- per-client 限流（P1-F1）：同一键第 13 次被拒；**一个键耗尽不影响另一个键**，也不额外消耗全站额度的判定顺序固定（先全站后 per-client 或反之，测试钉死其一）。
- 旧 `workDir` 键不读（本片先保证 `web_settings` 不碰它；启动目录在 W6）。

**证据**：定向用例输出；claim 幂等与 64 上限各一条显式断言。
**构建次数**：1–2。
**回滚**：删除新增文件、还原两处 CMake；无 schema 变更。

---

## W3 协议层与服务骨架

**目标**：网页服务可启停；明文握手 → 加密信封 → 路由 → SSE 首帧链路打通；四项上限有统一返回路径。

**依赖**：W1、W2。

**新建**
| 文件 | 内容 |
|---|---|
| `src/module_terminal/web/web_crypto_session.h/.cpp` | 每连接 X25519 临时密钥；HKDF-SHA256 盐 + `ddweb/v1/c2s\|…`/`s2c` 方向标签；方向 nonce 计数器；128 位重放窗口；信封 `ver1+nonce12+ct+tag16`；hid（22 字符 base64url）；会话表 `hid→{keys,ticketId,createdAt,lastSeen}`，64 上限/24h 空闲回收；流票据 `unused→in_use→consumed`（60s） |
| `src/module_terminal/web/web_bridge.h/.cpp` | 命令队列 ≤256（满 503）；只读快照加锁拷贝；SSE 订阅者表（每订阅 ≤8，控制事件独立优先、增量按行合并）；单调 seq；`hello/meta/full/rows/cursor/session/revoke/shutdown`；15s 心跳、写失败/45s 未收半开判定；订阅全站 64 上限；`session` 事件带 `title`（OSC 0/2 标题，回落前台进程名/目录名，P1-C2）与 `reason`（含 `desktop_restart` / `process_exit`，P1-A1）；记录网页接管前桌面行列，最后一个网页订阅退出时恢复桌面尺寸（P1-D1） |
| `src/module_terminal/web/web_routes.cpp` | 路由表（技术卡 §4）；Host/Origin 403；`/api/e` 按 `X-DevDesk-Session` 取密钥；握手限流键取 `RequestCtx.clientAddress`（P1-F1）；HTTP 码 vs 密文内 `s` 分离；响应安全头（nosniff/no-referrer/no-store/CSP） |
| `src/module_terminal/web/web_terminal_service.cpp` | `core::IWebTerminalService` 实现：启停 HTTP、ticker 排空命令、采样请求 UI 刷新、`writeSettings` 单一入口（校验→写内存→落库→广播）、快照线程安全、`error` 槽写最近一条失败原因（P1-E1 数据面）；**debug-only 读 `DEVDESK_WEB_AUTOSTART` / `DEVDESK_WEB_PORT` 自动开服并固定端口（§1.11，release 分支不编译）** |
| `src/module_terminal/web/embed_assets.cmake` | `file(READ HEX)` 生成 `web_assets_generated.cpp`；ETag/304；`add_custom_command` 依赖 `www/` |
| `tests/test_modules/test_terminal_web_service.cpp` | `/api/hs` 明文与字段完整；`/api/e` 无 hid=400；回环仍需密文；Host/Origin=403；静态 ETag=304；队列满=503；订阅 64→503；会话 64→409；票据 64→409；设置同步槽超时=503；**同一 clientAddress 触发 per-client 429 而其它地址仍可握手**；**debug 启动参数：AUTOSTART 后快照 running=true、PORT=n 时 `actualPort()==n`** |
| `tests/support/web_test_client.h/.cpp`（初版） | 握手/密封/解封/单请求；W7 扩为完整协议客户端 |

**修改**
| 文件 | 最小接线理由 |
|---|---|
| `src/module_terminal/module_terminal.h` | 增 `WebTerminalService` 成员与 include；析构注销（技术卡 ≤10 行） |
| `src/module_terminal/module_terminal.cpp` | 构造建服务、`onStart` 注册 `ServiceLocator::set<IWebTerminalService>`、`onStop` 停监听并注销、`compose` 末尾 tick（≤50 行） |
| `src/module_terminal/CMakeLists.txt` | 追加 web 源、`embed_assets` 生成步骤 |
| `tests/CMakeLists.txt` | 追加服务测试 + `tests/support/web_test_client.cpp` |

**完成判据（可判）**
- 测试内启动服务（回环 + 端口 0 自动端口）→ 完成握手 → 加密 POST 往返成功；抓取响应断言 body 不含明文业务字段。
- 无 hid / 错 hid = 400；回环请求不走明文旁路。
- Host/Origin 伪造 = 403；四项上限分别命中 503/429/409/409，已有连接不被清理。
- per-client 限流（P1-F1）：同一 clientAddress 超限返回 429，另一地址同一分钟仍能握手并登录。
- debug 启动参数（§1.11）：`DEVDESK_WEB_AUTOSTART=1` + `DEVDESK_WEB_PORT=<n>` 启动即 running 且 `actualPort()==n`；不设时行为与手动开关一致（起为关）。
- SSE 首帧为 `full` 且带 seq；心跳 15s 内至少一帧；客户端断开后订阅计数回落。
- 停服务后端口释放，旧 hid 请求 400。

**证据**：服务级定向用例输出；一次抓包/日志证明请求体为 base64 密文（仅回环）。
**构建次数**：2–3（本片最大，SSE 契约与线程接线易返工）。
**回滚**：停用服务开关即停监听；删除本片文件、还原 module/cmake 接线；`app_config` 仅新增键。

---

## W4 网格编码与会话桥

**目标**：把 `TerminalFrame` 编成网页可渲染的确定性帧，tab/尺寸/退出态与桌面双向同步。

**依赖**：W3。

**新建**
| 文件 | 内容 |
|---|---|
| `src/module_terminal/web/web_grid_codec.h/.cpp` | 24B 帧头（magic/version/flags/rows/cols/cursor/scroll/generation）；逐 run（16B 头 + UTF-8）；相邻格 width/attrs/fg/bg 任一不同即切；宽字占位格并入前 run；行哈希 FNV-1a 64；增量 `rows`、`full` 清增量；>256KB 降级纯文本行 + `LOG_WARN` |
| `tests/test_modules/test_terminal_web_codec.cpp` | run 切分/宽字占位/颜色默认位/行哈希/增量/seq 连续/体积上界/base64 后体积（29B 开销） |

**修改**
| 文件 | 最小接线理由 |
|---|---|
| `src/module_terminal/terminal_grid_widget.h/.cpp` | 导出 `decodeCellColor`（≤12 行）：网页与服务端必须共用同一 ANSI/默认色映射，复制即漂移 |
| `src/module_terminal/web/web_bridge.cpp` | 接 codec：ticker 内取激活会话快照 → 编码 → 广播；tab 增删切/尺寸/退出事件入 `session` 事件 |
| `src/module_terminal/web/web_terminal_service.cpp` | `pollAll` 与命令排空顺序固定（先收 spawn 结果再取帧）；`resize` 钳制 20–200/8–200 在服务侧收口 |
| `tests/test_modules/test_terminal_web_service.cpp` | 追加 `title` / `reason=desktop_restart` / 网页退出恢复桌面尺寸三条服务级断言 |
| `src/module_terminal/CMakeLists.txt` / `tests/CMakeLists.txt` | 追加 codec 源与测试 |

**完成判据（可判）**
- codec 单测全绿；同一 frame 编码两次字节一致；只改一行时 `rows` 仅含该行。
- 宽字（CJK/emoji 占位）不产生重复列；默认前景/背景位与桌面渲染一致。
- `resize` 传入 10/1000 分别落 20/200；桌面窗口尺寸路径不受钳制。
- 关闭最后一个 tab → `session` 事件后 `full` 为空网格 + 空态标志；进程退出事件含 exitCode。
- 增量缺口触发 `resync` → 服务端回 `full`。
- tab 标题（P1-C2）：子进程 `OSC 0/2` 设标题后 `session.title` 随之更新；从未设置时回落目录名/进程名，不显示「终端 N」占位。
- 桌面重启（P1-A1）：服务侧模拟「无任何会话」的启动态 → `session.reason == "desktop_restart"`，`full` 为空网格 + 空态标志。
- 尺寸恢复（P1-D1）：桌面 160×50 → 网页 resize 80→ 网页最后一个订阅退出后，桌面恢复 160×50（非 80×24）。

**证据**：codec 用例输出；服务测试中"改一行只发一行"的断言。
**构建次数**：1–2。
**回滚**：删除 codec 文件；桥回退到仅 `full` 全量帧（不破坏协议）。

---

## W5 前端接线（纯 JS）

**目标**：四视图从 mock 切到真实服务：握手、登录/绑定、终端、配置；加密全部纯 JS。

**依赖**：W3、W4（可与 W6 并行）。

**新建**
| 文件 | 内容 |
|---|---|
| `src/module_terminal/web/www/crypto.js` | 纯 JS：SHA-256/HMAC-SHA1/256、HKDF-SHA256、X25519、ChaCha20-Poly1305、base64url/hex；随机数只用 `crypto.getRandomValues`；**零 `crypto.subtle` 引用** |
| `tests/e2e/crypto_vectors.mjs` | Node 侧跑 RFC 7748/5869/8439 向量 + 与 C++ 输出对照（S-32 的 Node 半边） |

**修改**
| 文件 | 最小接线理由 |
|---|---|
| `src/module_terminal/web/www/protocol.js` | `MockClient` 之后补 `RealClient`：GET/POST `/api/hs`、`/api/e`、EventSource、seq/gap→`resync`；保留帧编解码不变；断线期间输入入**本地待发队列**（上限 256 条，满则拒绝并提示），重连成功后按序补发（P1-B1） |
| `src/module_terminal/web/www/app.js` | 去掉 mock 装配；接握手状态、登录/绑定提交、设置读写、四项上限覆盖层文案；退出登录；**全屏按钮 F-27**（`requestFullscreen`/`exitFullscreen` + `fullscreenchange` 同步按钮态，API 缺失时显示「添加到主屏幕」引导）；**手机最小键带 F-28**（仅 Esc、Tab 两键，点按发送 `0x1b` / `0x09`；只在小视口 + 触摸设备显示，硬件键盘用户可收起）；`visibilitychange`/`pageshow` 立即重建连接（P1-B3）；断线覆盖层文案区分网络/端口变更（P1-A2）；tab 标题展示与双击重命名（内存态，P1-C2）；桌面重启空态文案 + 「重开」入口（P1-A1） |
| `src/module_terminal/web/www/index.html` | 引 `crypto.js`；补登录/绑定/配置所需控件与 CSP meta；补全屏按钮与键带容器（默认隐藏，由 JS 按视口/能力显示）；保持单页四视图 |
| `src/module_terminal/web/www/app.css` | 上限覆盖层、握手错误页、锁定倒计时、空态、未发送徽标等新态样式；键带固定于 `visualViewport.height` 上方，不遮光标行；全屏态布局无残留外边距 |
| `src/module_terminal/web/www/render.js` | 对齐 W4 帧字段（默认色位/宽字、`title`），不新增渲染能力；重连 `full` 帧后按保存的 `scrollOffset` 对齐到原位置（P1-B2） |

**完成判据（可判）**
- `node tests/e2e/crypto_vectors.mjs` 全绿；同一明文 JS 密文可由服务端解、反之亦然（S-32）。
- 页面源码 grep 无 `crypto.subtle`；`crypto.getRandomValues` 存在。
- `--dev` 构建后静态资源内嵌成功；浏览器打开回环地址完成握手并进入终端（mock 不再被加载）。
- 断线：≤1s 顶部细条，连续 2 次失败全屏；退避 1/2/4/8/10。
- 输入三路（普通/组合/特殊键）与粘贴框双路径按 PRD §9.4/§9.5 工作；组合输入不误发拼音。
- **F-27 全屏**：桌面 Chrome/Edge/Safari 点按钮进入全屏、再点退出；退出后按钮态与布局无残留；`fullscreenchange` 由系统 Esc/手势触发时按钮同步；全屏态下 Esc 被浏览器用于退出全屏（已知交互重叠，键带仍可向 PTY 发 Esc）；无 Fullscreen API 的 iOS Safari 显示「添加到主屏幕」引导，不报错、不出现死按钮。
- **F-28 键带**：视口 ≤768px 且触摸设备显示；点 Esc 发送单字节 `0x1b`、点 Tab 发送 `0x09`，服务端收到与硬件键盘一致；软键盘弹起/收起时键带贴可视区上沿、不遮光标行；横竖屏切换后仍在位；桌面宽视口不显示。Ctrl/方向键本轮不做（P0 剩余项，留档）。
- **P1-B1 待发队列**：断线 → 输入框仍可输入并显示「未发送」计数 → 重连后按序补发且只发一次；队列满拒绝新输入并提示。
- **P1-B2 滚动位置**：上滚回看历史时断线重连 → 回到原偏移，不被拉到最新行；原本在底部则仍在底部跟随。
- **P1-B3 回前台**：模拟 `visibilitychange` 回前台 → 立即可见重连动作（不等 45s 心跳超时）。
- **P1-A1/A2 文案**：桌面重启空态显示「桌面端已重启，会话已丢失」+ 重开入口；连接失败覆盖层区分「桌面未开启/网络或端口可能已变更」两种原因。
- **P1-C2 tab 标题**：显示 OSC 标题或目录名；双击重命名后 tab 立即更新，刷新页面回默认（不持久化）。

**证据**：Node 向量输出；浏览器截图（登录页、终端首屏、断线覆盖层、全屏态、移动模拟下键带贴软键盘）；embed 后的资源哈希。
**构建次数**：1（含资源重新生成）。
**回滚**：`app.js` 切回 MockClient 即回原型；`crypto.js` 可整体删除。

---

## W6 桌面端与启动目录

**目标**：设置页「网页访问」专区可用；启动目录设置项桌面/网页同源；QR 由 EUI 渲染。

**依赖**：W1、W2、W3（服务接口与状态）。

**新建**
| 文件 | 内容 |
|---|---|
| `src/module_settings/web_access_card.h/.cpp` | 四态卡：关闭/运行未绑定/运行已绑定/错误；开关、端口（运行中只读）、地址列表 + 复制、QR 矩阵 `ui.rect` 渲染（4 模块静区）、倒计时/刷新、删除绑定确认；**最近错误 + 「打开日志目录」**（P1-E1）；**「手机连不上？检查防火墙」帮助文案**（P1-E2，平台判定经 `core`）；**绑定成功显式提示「已绑定新验证器」+ 一键撤销重绑 + 绑定风险文案**（P1-F2）；**地址列表随网卡变化与端口变更刷新、QR 随之重建**（P1-A2） |
| `tests/test_modules/test_web_access_card.cpp` | 卡高三态/端口越界红边判定/地址空态/绑定态隐藏 QR/动作回调触发/**最近错误文案与空错误占位**/**绑定成功提示与撤销重绑回调**/**地址变更触发重绘** |

**修改**
| 文件 | 最小接线理由 |
|---|---|
| `src/module_settings/settings_module.cpp` | 注册新卡与动作；经 `ServiceLocator::requireService<IWebTerminalService>()` 取服务；缺失时卡内显示"终端模块未启用"（≤40 行） |
| `src/module_settings/settings_ui_cards.h` | 卡入参/动作枚举追加项（结构按位置聚合，字段只许追加末尾） |
| `src/module_terminal/terminal_settings.h` | 增 `startupDir` 字段（空=主目录）；`workDirUsableForSpawn` 语义改为"存在+目录+可进入" |
| `src/module_terminal/terminal_settings.cpp` | 新键 `startupDir` 读写、sanitize；`workDir` 旧键不读（≤30 行） |
| `src/module_terminal/terminal_settings_platform_posix.cpp` | `canEnterDirectory`：`access(R_OK|X_OK)+opendir` |
| `src/module_terminal/terminal_settings_platform_win.cpp` | `canEnterDirectory`：目录属性 + `CreateFileW(FILE_LIST_DIRECTORY)` |
| `src/module_terminal/terminal_page.cpp` | 会话组加"启动目录"行（≤40 行），保持面板滚动与行高 |
| `src/module_terminal/terminal_session_controller.cpp` | 非空不可用目录 → `Failed` 覆盖卡，不静默回退主目录（≤10 行） |
| `src/module_terminal/module_terminal.cpp` | 新会话参数传 `startupDir`；设置提交仍走 `writeSettings`（≤10 行增量） |

**完成判据（可判）**
- 设置页可见专区四部分；首启开关为关；端口 1024–65535 越界红边；运行中端口只读。
- 开启后地址列表含全部 IPv4 + 127.0.0.1；无局域网显示提示；QR 显示且矩阵非空、倒计时递减、刷新换码。
- 已绑定态显示绑定时间/最近登录，QR 消失；删除绑定需确认，确认后服务端票据全失效。
- 启动目录：空=主目录；普通文件/不存在/不可进入均被拒绝保存并保留原值；保存后被删 → 新建 tab 失败覆盖卡。
- 卡在服务不可用/终端模块关闭时有降级文案，不崩。
- P1-E1 自诊：快照有最近错误时显示原文与时间，空时显示占位；「打开日志目录」调到现有日志目录打开能力。
- P1-E2 防火墙：Windows 构建显示防火墙帮助文案，macOS 不显示（平台判定单一来源，无 `#ifdef`）。
- P1-F2 绑定：确认绑定成功即显示「已绑定新验证器」提示与绑定风险说明；撤销重绑一键可达，撤销后回二维码态。
- P1-A2 地址刷新：拔网/换网后下一次采样地址列表更新；改端口保存后地址与 QR 同步重建，旧地址不再展示。

**证据**：探针 `--screenshot` 出四态图（关闭/未绑定/已绑定/错误）+ 错误行与防火墙提示态；`--filter file:test_web_access_card,file:test_terminal_settings` 输出；QR 渲染耗时打点（目标 ≤8ms）。
**构建次数**：2（模块 UI + 探针图）。
**回滚**：删除设置卡与 `startupDir` 字段、还原 `workDirUsableForSpawn`；`terminal_web` 配置键可留（无迁移）。

---

## W7 测试收口

**目标**：S-01～S-33 有自动化或明账人工覆盖；P1 十一条各有回归点；协议客户端可复跑；双平台门禁通过。

**依赖**：W1–W6；**bug#493（Windows CI 红）必须已绿**，否则「双平台门禁绿」判据不可达。
**E2E 启动**：一律用 §1.11 的 debug-only 参数（`DEVDESK_WEB_AUTOSTART=1` + 固定 `DEVDESK_WEB_PORT` + 独立 `DEVDESK_DATA_ROOT`）起产品；不得驱动桌面 UI 点开关。

**新建**
| 文件 | 内容 |
|---|---|
| `tests/support/web_test_client.h/.cpp`（完整版） | TOTP、claim/confirm、登录、流票据、SSE、input、重连重握手、重放窗口、resync、四项上限、per-client 限流；双路径（回环免验 / 本机非回环 TOTP） |
| `tests/e2e/protocol.spec.mjs` | 绑定各态/登录锁定/票据/四项上限/多标签接管（Chromium 移动模拟 + Edge + WebKit） |
| `tests/e2e/ui.spec.mjs` | 画布取帧对照、tab 增删切与重命名、粘贴框、断线两段式、**待发队列补发**、**重连滚动位置**、**回前台重连**、**手机键带 Esc/Tab**、**全屏开关与 iOS 降级**、键盘/横竖屏、配置页 |
| `tests/e2e/perf.spec.mjs` | S-33：`page.evaluate` 调 `crypto.js` 固定帧跑批，p50/p95/样本数 |
| `tests/probes/web_terminal_probe.cpp` | 设置卡四态 + 错误行 + 防火墙提示截图、启动目录行布局、QR 帧耗时；`LABELS "probe"`。**CI headless 档不编本探针**（白名单仅 3 条），仅本地全量门禁 |
| `scripts/check-product-deps.sh` | 断言产品二进制不含 cotp/sodium/Playwright/测试客户端符号与 §1.11 的 debug 启动参数符号；在 `build-test-run.sh` 或 CI 有固定执行点 |
| `tests/e2e/README.md` | 复跑步骤：npm install、浏览器安装、registry/代理、Edge 缺失降级、固定端口与独立数据根用法 |

**修改**
| 文件 | 最小接线理由 |
|---|---|
| `tests/CMakeLists.txt` | 注册协议客户端/探针；e2e 不进 ctest 的部分在 README 说明 |
| `tests/e2e/package.json`（若需） | 只加 npm script，不加运行期依赖 |
| `.github/workflows/ci.yml` | 补 Playwright 步骤：① `npm ci`（cache key 含 `tests/e2e/package-lock.json` 哈希）；② 浏览器缓存 key 含 playwright 版本 + 安装 `chromium webkit`；③ Edge 可用性探测，缺失时降级 Chromium 并标 `EDGE_MISSING`（不静默绿）；④ 新增 `check-product-deps.sh` 执行点 |
| `tests/e2e/smoke.mjs` | Edge 缺失改打结构化 `EDGE_MISSING`，不 `FATAL` 退出；支持由外部启动的产品 URL（不再只起静态服务器） |

**完成判据（可判）**
- 协议客户端全场景绿：绑定→TOTP→多设备→撤销→限流（含 per-client）→篡改/重放→seq gap→四项上限→桌面重启空态；S-32 两端互通。
- Playwright 三引擎矩阵通过；S-33 各基线达标或按技术卡 §6.6 降级后复测记录；CI 上 `npm ci` 与浏览器缓存二次运行命中、Edge 缺失时输出 `EDGE_MISSING` 且不失败退出。
- E2E 全部经 §1.11 debug 启动参数起真实产品；`scripts/check-product-deps.sh` 绿且 release 二进制不含 debug 启动参数与测试符号。
- 本地全量门禁编译并运行 `tests/probes/web_terminal_probe.cpp` 出图；CI headless 不覆盖本探针，在证据里写明。
- `bash scripts/build-test-run.sh --release --no-run` 双平台完整门禁绿（守卫链 + 全量 doctest + probe 子集），**前提 bug#493 已关闭、Windows CI 已至少一次绿**。
- 人工清单（§6）完成并记录结论；未完成项明写"未验证"。

**证据**：完整门禁输出末 15 行；Playwright report/trace 路径；probe 截图；`CAPABILITY-MATRIX.md` 更新（S-33 实测）。
**构建次数**：2–3（含 release 门禁）。
**回滚**：测试与探针不进产品二进制；删除 `tests/support`、`tests/e2e` 新增用例与 probe 目标即可。

---

## 4. 依赖链与并发

```
W1 ─ W2 ─ W3 ─ W4 ─┬─ W5（前端；只动 www/）
                    └─ W6（桌面；动 module_settings/module_terminal）
                              └─ W7（依赖全部产物 + bug#493 已绿）
```
- 前置已清：bug#491 已关闭（§0）；worktree 首建先绕代理初始化子模块（§1.8/§1.9）。
- 同时只允许 1 个构建任务；W5/W6 可并行开发，但构建串行。
- W6 与 W5 无共享文件；W6 依赖 W3 的服务接口，不依赖 W5。
- W7 必须最后；其协议客户端基于 W3 初版扩展。

## 5. 风险与应对

| 风险 | 等级 | 应对 |
|---|---|---|
| bug#493（Windows CI 红，OpenSSL 4.1 + `-Werror`）→ W7 双平台门禁不可达 | 阻塞（仅 W7） | 与 bug#390 合并为一条在 W7 前修；W1–W6 macOS 单平台推进；W7 以 CI 首次绿为开工条件 |
| worktree 子模块全空 + git 代理指向死端口 → 首建 configure 失败 | 中 | §1.8/§1.9 固定命令；每个 worktree 首建前执行 |
| E2E 无启动入口（开关不持久化、无头不可点） | 中 | §1.11 debug-only 参数，W3 实现、W7 消费 |
| 全屏/键带浏览器差异（iOS 无 Fullscreen API、软键盘视口） | 低 | 特性检测 + 「添加到主屏幕」降级；键带用 `visualViewport`；真机项进 §6 |
| SSE provider 契约（热旋/半开/生命周期）返工 | 高 | W3 用条件变量 + 15s 心跳 + 写失败三兜底；以服务级用例先钉行为 |
| 纯 JS 密码学正确性/性能 | 高 | W5 先跑 RFC 向量与 Node 对照；W7 用 S-33 门禁；超限按 §6.6 降级，绝不降明文 |
| 模块隔离（settings 卡位置） | 中 | 本计划 §1.2 已裁决：卡在 settings，数据走 core 接口；派 code-reviewer 专门核隔离 |
| HTTP 线程碰会话状态 | 中 | 命令队列 + UI ticker 排空；HTTP 只碰加锁快照（技术卡 §1 不变量 2） |
| 票据表/会话表跨重启与 64 上限 | 中 | W2 单测钉 64 上限不淘汰；W3 服务测试钉 409 |
| 前端骨架与 v9 帧格式漂移 | 低 | 现有 `protocol.js` 24B 帧头与卡一致；W4 单测逐字节钉死 |
| macOS/Windows 非回环测试差异 | 中 | 回环主路径 + debug-only `DEVDESK_WEB_TEST_HOOKS` 合成非回环地址（release 不存在） |
| Playwright WebKit/Edge 环境缺失 | 低 | 本地门禁 + CI 降级标记 `EDGE_MISSING`，不静默绿；CI 补 npm/浏览器缓存步骤（W7） |

## 6. 需人工验证清单（自动化不可替代）

1. **iOS Safari 真机**：软键盘弹起/收起时 `visualViewport` 时序、光标行保持可见、顶/底栏不被遮挡（F-19）。
2. **iOS 剪贴板权限**：局域网手动粘贴框；回环可预填剪贴板（P-23）。
3. **长按选择**：400ms 移动判滚动、500ms 进入选择、系统菜单复制（P-21/S-29）。
4. **真实验证器 App**：Google Authenticator / Microsoft Authenticator 扫桌面二维码并生成可登录六位码（F-06）。
5. **第二台同网段机器抓包**：绑定、登录、输入、看屏、退出全程明文层无 TOTP/密钥/令牌/票据/终端文本/输入（S-24）。
6. **S-33 基线实测**：中端 Android（Snapdragon 7 系 / Dimensity 8000 系）Chrome p95 达标。
7. **Windows 真机**：防火墙首次放行提示、Edge 158 登录与渲染。
8. **双平台 UAT**：S-01～S-31 全流程（macOS + Windows 各一遍），含端口占用、撤销绑定、服务关闭时序。
9. **二维码实扫**：手机相机直接扫，覆盖 v4/v5 边界（78/79/106B）。
10. **手机键带真机**：iOS Safari / Android Chrome 软键盘弹起收起、横竖屏切换下键带位置；Esc/Tab 在 claude code 中断与补全场景实测送达（F-28）。
11. **全屏真机**：Android Chrome 与 iOS Safari 全屏进入/退出/旋转后布局；iOS 降级「添加到主屏幕」引导可读可操作（F-27）。

## 7. 回滚

- 功能级：设置页关闭开关即停监听；网页连接全部断开。
- 代码级：按片删除新增文件并还原 §2.3 所列既有文件接线；`app_config` 只增键（`terminal_web`、`startupDir`），不删不改旧键，回滚无需数据迁移。
- 依赖级：无新增第三方运行期依赖；测试链依赖（Playwright/Edge）不进产品。
- 测试入口：`DEVDESK_WEB_AUTOSTART` / `DEVDESK_WEB_PORT` 仅 Debug 编译，回滚即删除读取分支与常量，release 产物无符号可取（W7 守卫断言）。
