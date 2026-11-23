# need#490 网页终端实现切片计划（W1–W7）

- 对齐基线：PRD `2026-11-23-web-terminal` **v10**、技术卡 `2026-11-23-web-terminal-tech` **v9**、ADR 0009、ADR 0010 **v2**、UI 设计 + 39 屏 demo。
- 范围：技术卡 §16 的 F-01～F-26。本计划只切实现顺序，不改产品/协议裁决；冲突以 ADR > PRD > 技术卡为序。
- 已有资产（直接复用，不重造）：前端四视图骨架 `src/module_terminal/web/www/`（1422 行）、自研二维码 `src/module_terminal/web/qr/qr_encode.{h,cpp}` + `tests/test_modules/test_qr_encode.cpp`、Playwright 冒烟 `tests/e2e/`、`platform::http::Server` 路由/流式 provider/Host 白名单/客户端地址能力。

## 0. 前置阻塞：bug#491（必须先行）

- 现状：`src/module_release/release_env_card.cpp:51` 调用 `prereqSectionHeight`，该函数定义在 `src/module_release/release_detail_draw.cpp:377`，但共享声明缺失 → `dev` 构建失败。
- 影响：**修好前本计划任何切片都无法构建与验证**。该修复不属本计划，须由主会话先派 bug-fixer 关闭 bug#491；本计划所有"完成判据"以 bug#491 已关闭为前提。

## 1. 全局约定

1. **构建/测试唯一入口**：`bash scripts/build-test-run.sh`；开发内循环 `--dev --filter file:<文件子串>`；收口/发版才不带 `--dev` 完整跑。
2. **测试框架**：自制迷你 doctest（`REGISTER_TEST`，fail-fast）。每条用例必须能答"删掉被测代码会不会红"；不写覆盖率式用例。
3. **模块隔离**：`module_*` 之间零 include、零链接边；跨模块一律经 `core` 接口 + `core::ServiceLocator`（先例：`core::IClaudeProxyService`）。
4. **平台拆分**：平台差异只放 `*_posix.cpp` / `*_win.cpp`，业务代码零 `#ifdef`。
5. **证据**：每片产出定向用例输出摘要；UI 片用探针 `--screenshot` 出图；证据存 `evidence/`，路径写回测试清单。
6. **提交**：每片完成以 `wip:` commit 落盘（显式 pathspec），收尾 squash 为单个 `feat:`；禁 `git add -A`。
7. **注释纪律**：只写契约/反重构警告，不复述代码行为；本计划新增注释同样受约束。

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

## 2. 切片总览

| 片 | 主题 | 依赖 | 构建次数（预计） | 产物 |
|---|---|---|---|---|
| W1 | 平台底座：crypto / net / core 接口 | bug#491 | 1–2 | `platform::web_crypto`、`platform::net`、`core::IWebTerminalService` |
| W2 | 鉴权：令牌 / TOTP / 票据 / 限流 | W1 | 1–2 | `web_auth`、`web_settings` + 单测 |
| W3 | 协议：握手 / 信封 / 路由 / SSE / 服务骨架 | W1,W2 | 2–3 | 可握手的 HTTP 服务 + 服务级单测 |
| W4 | 网格与会话桥：codec / 增量 / tab / 尺寸 | W3 | 1–2 | `web_grid_codec`、`web_bridge` 完整事件 |
| W5 | 前端接线：纯 JS 加密 / SSE / 四视图 | W3,W4 | 1 | `www/crypto.js` + RealClient + 视图 |
| W6 | 桌面端：设置卡 / QR 渲染 / 启动目录 | W1,W2,W3 | 2 | 设置页专区 + 启动目录设置项 |
| W7 | 测试收口：协议客户端 / Playwright / 探针 / 真机清单 | W1–W6 | 2–3 | S-01～S-33 自动化 + 人工清单 |

串行派发顺序：`W1 → W2 → W3 → W4 → (W5 ∥ W6) → W7`。W5 与 W6 无共享文件，可并行（同一时刻只有一个构建任务）。

---

## 3. W1 平台底座

**目标**：把加密原语、网卡枚举、跨模块接口一次立住，后续切片只调不改。

**依赖**：bug#491 关闭。

**新建**
| 文件 | 内容 |
|---|---|
| `src/core/web_terminal_service.h` | `IWebTerminalService` 抽象接口 + POD 快照（`WebAccessSnapshot`：running/port/addresses/bindState/bindTime/lastLogin/onlineCount/qrMatrix/qrSize/tokenRemainingSec/error）；`TerminalSettingsPatch`（fontFamily/fontSize/colorMode/palette/shell/startupDir）；`writeSettings()` 单一写入入口声明；`start/stop/refreshToken/revokeBinding` |
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
| `src/module_terminal/web/web_auth.h/.cpp` | 令牌 32B→base64url、5 分钟、`unclaimed/claimed/terminal` 三态、claim 幂等；TOTP（20B 密钥、base32、HMAC-SHA1、T=unix/30、±1 步、定长比较）；票据签发/校验（`ver1‖expiry8‖ticketId16` + HMAC-SHA256）；吊销表（≤64、7 天、退出删 ID、撤销清表）；登录锁定 5 次/60s；握手限流 32 次/60s 滑动窗口；失败事件仅记日志 |
| `src/module_terminal/web/web_settings.h/.cpp` | `app_config` 键 `terminal_web`：端口、绑定密钥/绑定时间、最近登录、票据表；服务开关**不持久化**（P-02，每次启动为关）；损坏 JSON 回默认并 `LOG_ERROR` |
| `tests/test_modules/test_terminal_web_auth.cpp` | RFC 6238 向量、±1/±2、base32、claim 两次同密钥、confirm 消费、5 次错作废、票据签名/过期/吊销/64 上限、锁定、限流窗口、回环免验纯函数 |
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
| `src/module_terminal/web/web_bridge.h/.cpp` | 命令队列 ≤256（满 503）；只读快照加锁拷贝；SSE 订阅者表（每订阅 ≤8，控制事件独立优先、增量按行合并）；单调 seq；`hello/meta/full/rows/cursor/session/revoke/shutdown`；15s 心跳、写失败/45s 未收半开判定；订阅全站 64 上限 |
| `src/module_terminal/web/web_routes.cpp` | 路由表（技术卡 §4）；Host/Origin 403；`/api/e` 按 `X-DevDesk-Session` 取密钥；HTTP 码 vs 密文内 `s` 分离；响应安全头（nosniff/no-referrer/no-store/CSP） |
| `src/module_terminal/web/web_terminal_service.cpp` | `core::IWebTerminalService` 实现：启停 HTTP、ticker 排空命令、采样请求 UI 刷新、`writeSettings` 单一入口（校验→写内存→落库→广播）、快照线程安全 |
| `src/module_terminal/web/embed_assets.cmake` | `file(READ HEX)` 生成 `web_assets_generated.cpp`；ETag/304；`add_custom_command` 依赖 `www/` |
| `tests/test_modules/test_terminal_web_service.cpp` | `/api/hs` 明文与字段完整；`/api/e` 无 hid=400；回环仍需密文；Host/Origin=403；静态 ETag=304；队列满=503；订阅 64→503；会话 64→409；票据 64→409；设置同步槽超时=503 |
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
| `src/module_terminal/CMakeLists.txt` / `tests/CMakeLists.txt` | 追加 codec 源与测试 |

**完成判据（可判）**
- codec 单测全绿；同一 frame 编码两次字节一致；只改一行时 `rows` 仅含该行。
- 宽字（CJK/emoji 占位）不产生重复列；默认前景/背景位与桌面渲染一致。
- `resize` 传入 10/1000 分别落 20/200；桌面窗口尺寸路径不受钳制。
- 关闭最后一个 tab → `session` 事件后 `full` 为空网格 + 空态标志；进程退出事件含 exitCode。
- 增量缺口触发 `resync` → 服务端回 `full`。

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
| `src/module_terminal/web/www/protocol.js` | `MockClient` 之后补 `RealClient`：GET/POST `/api/hs`、`/api/e`、EventSource、seq/gap→`resync`；保留帧编解码不变 |
| `src/module_terminal/web/www/app.js` | 去掉 mock 装配；接握手状态、登录/绑定提交、设置读写、四项上限覆盖层文案；退出登录 |
| `src/module_terminal/web/www/index.html` | 引 `crypto.js`；补登录/绑定/配置所需控件与 CSP meta；保持单页四视图 |
| `src/module_terminal/web/www/app.css` | 上限覆盖层、握手错误页、锁定倒计时等新态样式 |
| `src/module_terminal/web/www/render.js` | 对齐 W4 帧字段（默认色位/宽字），不新增渲染能力 |

**完成判据（可判）**
- `node tests/e2e/crypto_vectors.mjs` 全绿；同一明文 JS 密文可由服务端解、反之亦然（S-32）。
- 页面源码 grep 无 `crypto.subtle`；`crypto.getRandomValues` 存在。
- `--dev` 构建后静态资源内嵌成功；浏览器打开回环地址完成握手并进入终端（mock 不再被加载）。
- 断线：≤1s 顶部细条，连续 2 次失败全屏；退避 1/2/4/8/10。
- 输入三路（普通/组合/特殊键）与粘贴框双路径按 PRD §9.4/§9.5 工作；组合输入不误发拼音。

**证据**：Node 向量输出；探针/浏览器截图（登录页、终端首屏、断线覆盖层）；embed 后的资源哈希。
**构建次数**：1（含资源重新生成）。
**回滚**：`app.js` 切回 MockClient 即回原型；`crypto.js` 可整体删除。

---

## W6 桌面端与启动目录

**目标**：设置页「网页访问」专区可用；启动目录设置项桌面/网页同源；QR 由 EUI 渲染。

**依赖**：W1、W2、W3（服务接口与状态）。

**新建**
| 文件 | 内容 |
|---|---|
| `src/module_settings/web_access_card.h/.cpp` | 四态卡：关闭/运行未绑定/运行已绑定/错误；开关、端口（运行中只读）、地址列表 + 复制、QR 矩阵 `ui.rect` 渲染（4 模块静区）、倒计时/刷新、删除绑定确认 |
| `tests/test_modules/test_web_access_card.cpp` | 卡高三态/端口越界红边判定/地址空态/绑定态隐藏 QR/动作回调触发 |

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

**证据**：探针 `--screenshot` 出四态图（关闭/未绑定/已绑定/错误）；`--filter file:test_web_access_card,file:test_terminal_settings` 输出；QR 渲染耗时打点（目标 ≤8ms）。
**构建次数**：2（模块 UI + 探针图）。
**回滚**：删除设置卡与 `startupDir` 字段、还原 `workDirUsableForSpawn`；`terminal_web` 配置键可留（无迁移）。

---

## W7 测试收口

**目标**：S-01～S-33 有自动化或明账人工覆盖；协议客户端可复跑；双平台门禁通过。

**新建**
| 文件 | 内容 |
|---|---|
| `tests/support/web_test_client.h/.cpp`（完整版） | TOTP、claim/confirm、登录、流票据、SSE、input、重连重握手、重放窗口、resync、四项上限；双路径（回环免验 / 本机非回环 TOTP） |
| `tests/e2e/protocol.spec.mjs` | 绑定各态/登录锁定/票据/四项上限/多标签接管（Chromium 移动模拟 + Edge + WebKit） |
| `tests/e2e/ui.spec.mjs` | 画布取帧对照、tab 增删切、粘贴框、断线两段式、键盘/横竖屏、配置页 |
| `tests/e2e/perf.spec.mjs` | S-33：`page.evaluate` 调 `crypto.js` 固定帧跑批，p50/p95/样本数 |
| `probes/web_terminal_probe.cpp` | 设置卡四态截图、启动目录行布局、QR 帧耗时；`LABELS "probe"` |
| `scripts/check-product-deps.sh` | 断言产品二进制不含 cotp/sodium/Playwright/测试客户端符号 |

**修改**
| 文件 | 最小接线理由 |
|---|---|
| `tests/CMakeLists.txt` | 注册协议客户端/探针；e2e 不进 ctest 的部分在 README 说明 |
| `tests/e2e/package.json`（若需） | 只加 npm script，不加运行期依赖 |
| `.github/workflows/ci.yml` | Windows Edge 可用性探测缺省降级 Chromium 并标 `EDGE_MISSING` |

**完成判据（可判）**
- 协议客户端全场景绿：绑定→TOTP→多设备→撤销→限流→篡改/重放→seq gap→四项上限；S-32 两端互通。
- Playwright 三引擎矩阵通过；S-33 各基线达标或按技术卡 §6.6 降级后复测记录。
- `bash scripts/build-test-run.sh --release --no-run` 双平台完整门禁绿（守卫链 + 全量 doctest + probe 子集）。
- 人工清单（§6）完成并记录结论；未完成项明写"未验证"。

**证据**：完整门禁输出末 15 行；Playwright report/trace 路径；probe 截图；`CAPABILITY-MATRIX.md` 更新（S-33 实测）。
**构建次数**：2–3（含 release 门禁）。
**回滚**：测试与探针不进产品二进制；删除 `tests/support`、`tests/e2e` 新增用例与 probe 目标即可。

---

## 4. 依赖链与并发

```
bug#491（外部前置）
   └─ W1 ─ W2 ─ W3 ─ W4 ─┬─ W5（前端；只动 www/）
                          └─ W6（桌面；动 module_settings/module_terminal）
                                    └─ W7（依赖全部产物）
```
- 同时只允许 1 个构建任务；W5/W6 可并行开发，但构建串行。
- W6 与 W5 无共享文件；W6 依赖 W3 的服务接口，不依赖 W5。
- W7 必须最后；其协议客户端基于 W3 初版扩展。

## 5. 风险与应对

| 风险 | 等级 | 应对 |
|---|---|---|
| bug#491 未修 → 全计划不可验证 | 阻塞 | 主会话先修；本计划不启动任何构建 |
| SSE provider 契约（热旋/半开/生命周期）返工 | 高 | W3 用条件变量 + 15s 心跳 + 写失败三兜底；以服务级用例先钉行为 |
| 纯 JS 密码学正确性/性能 | 高 | W5 先跑 RFC 向量与 Node 对照；W7 用 S-33 门禁；超限按 §6.6 降级，绝不降明文 |
| 模块隔离（settings 卡位置） | 中 | 本计划 §1.2 已裁决：卡在 settings，数据走 core 接口；派 code-reviewer 专门核隔离 |
| HTTP 线程碰会话状态 | 中 | 命令队列 + UI ticker 排空；HTTP 只碰加锁快照（技术卡 §1 不变量 2） |
| 票据表/会话表跨重启与 64 上限 | 中 | W2 单测钉 64 上限不淘汰；W3 服务测试钉 409 |
| 前端骨架与 v9 帧格式漂移 | 低 | 现有 `protocol.js` 24B 帧头与卡一致；W4 单测逐字节钉死 |
| macOS/Windows 非回环测试差异 | 中 | 回环主路径 + debug-only `DEVDESK_WEB_TEST_HOOKS` 合成非回环地址（release 不存在） |
| Playwright WebKit/Edge 环境缺失 | 低 | 本地门禁 + CI 降级标记 `EDGE_MISSING`，不静默绿 |

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

## 7. 回滚

- 功能级：设置页关闭开关即停监听；网页连接全部断开。
- 代码级：按片删除新增文件并还原 §2.3 所列既有文件接线；`app_config` 只增键（`terminal_web`、`startupDir`），不删不改旧键，回滚无需数据迁移。
- 依赖级：无新增第三方运行期依赖；测试链依赖（Playwright/Edge）不进产品。
