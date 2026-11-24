# need#490 网页终端威胁模型与验证方案

日期：2026-11-24
范围：DevDesk 终端模块的网页访问（局域网 HTTP + 应用层每会话加密 + 扫码绑定 / TOTP）
关联：ADR 0009（暴露面与鉴权）、ADR 0010 v2（传输加密）、PRD v16、技术卡 v11
文档性质：**威胁模型 + 可执行验证方案**。本文不修改代码；文中“已确认”= 代码或测试可核对，“待实测”= 本次未运行，“推测”= 仅逻辑推断。

---

## 0. 一句话结论

这套设计**挡得住同网段被动偷看**（抓包看不到终端内容、输入、TOTP 码、TOTP 密钥、绑定令牌、会话票据），**挡不住主动中间人、本机同用户进程、有权限的浏览器扩展**；它把“局域网明文 shell”的风险从“任何人插上就能看”降到“必须主动劫持或已控制本机/浏览器”。**不可信网络不要直接用**，要加 TLS 反向代理或 SSH 隧道。

---

## 1. 资产清单

“资产”= 泄露后会让攻击者看到或操作这台机器的东西。

| 编号 | 资产 | 存放位置 | 生命周期 | 泄露后果 |
|---|---|---|---|---|
| A1 | 终端输出内容（屏幕、标签页快照、历史回显） | 服务端内存 → SSE 加密帧 | 会话期间 | 看到 shell 里的全部输出，可能含密钥、源码、内部地址 |
| A2 | 终端输入（按键、粘贴、命令） | 浏览器 → 加密 POST → 服务端内存 | 即时 | 看到用户输入，可还原命令与密码 |
| A3 | 会话票据（登录通行字符串） | 浏览器 `localStorage`、服务端内存、盘上 `web_tickets.dat` | 签发后 7 天绝对有效（能否跨进程重启取决于 `cookieKey` 是否持久化，见下） | 等同已登录：可打开终端、发命令，**不再需要 TOTP** |
| A4 | TOTP 密钥（验证器种子，20 字节） | 服务端进程内存；绑定确认前经加密帧下发给手机 | 进程存活期间（不落盘） | 攻击者可自己生成六位码，第二因子永久失效 |
| A5 | 绑定令牌（二维码里的 32 字节随机串） | 桌面内存、二维码 URL `#b=` 片段 | 5 分钟、成功绑定即作废 | 有效期内可抢先绑定自己的验证器，接管登录入口 |
| A6 | `cookieKey`（32 字节，票据签名密钥） | **工作树版本**：SQLite `app_secrets` 表（全平台明文进列，靠文件权限）；**已提交 HEAD**：仅进程内存、每进程随机 | 工作树：长期；HEAD：进程存活期间 | 工作树：可为任意“仍在吊销表里的票据 ID”伪造合法票据，绕过 TOTP；HEAD：进程退出即失效 |
| A7 | TOTP 六位码 | 验证器 App、加密帧内瞬时传输 | 30 秒（服务端容忍前后各 1 步） | 30 秒内可重放登录；需配合主动中间人或已控制端 |
| A8 | SSE 流票据（60 秒、单次、只读） | 加密帧内下发，随后出现在 `/api/stream?t=` 查询串 | 60 秒，用一次即废 | 可打开一条只读事件流；读不到明文，价值低 |
| A9 | 终端配置、标签列表、启动目录、字体等 | 加密帧 / 服务端内存 | 会话期间 | 泄露本机路径、项目名、使用习惯 |
| A10 | 运行状态（是否绑定、令牌倒计时、在线数） | `/api/status`（未鉴权）与桌面 UI | 实时 | 低：只暴露“服务存在、是否已绑定”，不暴露内容 |
| A11 | 桌面屏幕上的绑定二维码 | 物理屏幕 | 显示期间 | 被拍照后可抢绑（需 5 分钟内且能连到服务） |
| A12 | 进程内存中的会话密钥、TOTP 密钥、票据 | 进程堆内存 / 可能进 core dump、swap | 进程存活期间 | 内存读取者可解密当前会话或伪造身份 |

补充事实（**注意工作树与已提交基线的差异**）：
- 当前工作树把 `cookieKey` 写入 `app_secrets`，重启后旧票据仍可验签（`src/module_terminal/web/web_terminal_service.cpp:224-255`，用例 `web_cookie_key_persists_across_restart`）。**该改动尚未提交**；已提交基线 HEAD `0fd8ce92` 仍是“每进程随机 `cookieKey`”，进程重启后旧票据全部验签失败（更新 `docs/superpowers/plans/2026-11-24-need490-impl-audit2.md` 记为部分修复）。
- TOTP 密钥只放内存（`src/module_terminal/web/web_terminal_service.h:177`），进程退出即丢；工作树中由持久化 `cookieKey` 签发的 7 天票据却可跨重启，二者生命周期不一致（见 R6）。
- 票据文件只存“票据 ID + 到期时间 + 创建时间”，不存票据原文（`src/module_terminal/web/web_auth.h:5-10, 97`）。

---

## 2. 攻击面

“攻击面”= 攻击者能碰到的地方。

### 2.1 网络面（服务开启时）

监听全部网卡 `0.0.0.0`，默认端口 **7878**，可改为 1024–65535（ADR 0009；`web_terminal_service.cpp:1327-1328`）。默认关闭，必须用户在设置页开启。

| 入口 | 是否需要登录 | 说明 |
|---|---|---|
| `GET /`、`/app.js`、`/app.css`、`/protocol.js`、`/render.js`、`/crypto.js` | 否，明文 | 静态页面与前端加密代码；无业务数据 |
| `GET /api/hs`、`POST /api/hs` | 否，有限流 | 握手：交换一次性公钥、拿会话 ID 与盐（`web_routes.cpp:134-141`） |
| `POST /api/e` 及 `/api/input`、`/api/settings`、`/api/login`、`/api/logout`、`/api/bind/*` 等别名 | 是（加密帧 + 非回环票据） | 所有业务消息走 AEAD 加密帧（`web_routes.cpp:143-163`） |
| `GET /api/stream` | 是（60 秒单次流票据） | SSE 持续推送，事件体全部为密文（`web_terminal_service.cpp:939-995`） |
| `GET /api/status` | 否 | 只回 `running/port/bindState/bound/tokenRemainingSec`，不含在线数、绑定时间、设备名（`web_terminal_service.cpp:1004-1020`） |

网络层固定开销：请求行、Header、静态资源、握手公钥、SSE 流票据查询参数都是明文（ADR 0010 §9）。

### 2.2 本机面

- 回环地址（127.0.0.1 / ::1）**免票据、免 TOTP**，但仍必须握手与加密（`web_terminal_service.cpp:609-610`）。本机任何同用户进程都能走回环。
- 盘上文件：`app_secrets` 表（含 `cookieKey` 明文）、`web_tickets.dat`（票据 ID 与到期时间）、日志。
- 进程内存：会话密钥、TOTP 密钥、票据签发密钥。

### 2.3 浏览器面

- 页面脚本持有解密后的终端内容与 `localStorage` 里的票据。
- 浏览器扩展：有对应站点权限的扩展可读 DOM、`localStorage`、拦截/改写页面 JS。
- 恶意网页：受同源策略（SOP）约束，且服务端校验 `Host` / `Origin`（`web_routes.cpp:70-85`），跨站读不到终端数据。

### 2.4 物理 / 近场面

- 桌面屏幕上的二维码（含 `#b=<绑定令牌>`）。
- 手机屏幕上的 TOTP 六位码、解锁后的终端页面。
- 手机丢失 / 被借用。

### 2.5 配置面

- 端口配置、是否开启网页访问、绑定/解绑操作都在桌面端；默认关闭是最大的一道门（ADR 0009 第 1 条）。

---

## 3. 攻击者模型

| 编号 | 攻击者 | 位置与能力 | 想拿到什么 | 结论 |
|---|---|---|---|---|
| T1 | 同网段被动嗅探者 | 同一 Wi-Fi / 交换机，能抓包但不能改包 | 终端内容、输入、TOTP 码、TOTP 密钥、绑定令牌、票据 | **防住**（见 4.1） |
| T2 | 主动中间人 | 能冒充网关 / 热点 / DNS，能改包、插包、替身服务器 | 同上，并可直接接管会话 | **不防**（见 4.2）；可窃取一切 |
| T3 | 局域网扫描者 | 能扫端口、发请求，但没有绑定令牌、TOTP、票据 | 找入口、碰运气登录、耗尽资源 | **登录防住；发现与拒绝服务挡不住**（见 4.3） |
| T4 | 本机其它进程 / 同用户恶意程序 | 与 DevDesk 同一操作系统账户 | 回环直连终端；读 `app_secrets`、`web_tickets.dat`；读进程内存 | **不防**（见 4.4） |
| T5 | 浏览器扩展 | 用户安装了有站点权限的扩展 | 解密后的 DOM、`localStorage` 票据、按键 | **取决于扩展权限，应用层无法防**（见 4.5） |
| T6 | 恶意网页 | 用户访问的攻击者页面，跨源请求本服务 | 读终端数据、伪造登录 | **基本防住**：SOP + `Host`/`Origin` 白名单 + CSP + 无 CORS（见 4.6） |
| T7 | 拿到票据的人 | 通过 T2/T4/T5 或备份拿到 7 天票据 | 直接使用终端 | **票据即身份，不防**；只能靠吊销/到期（见 R5） |
| T8 | 拿到手机或 TOTP 种子的人 | 物理或云端备份 | 登录第二因子 | **不防**；属终端设备安全（见 R8） |
| T9 | 拍下二维码的人 | 能看到桌面屏幕，且 5 分钟内能连到服务 | 抢绑自己的验证器 | **有限防**：一次性、5 分钟、绑定成功即作废（见 4.7） |
| T10 | 控制桌面系统的人 | 管理员 / 恶意软件 / 调试器 | 一切 | **不防**；这是所有本机应用的共同边界 |

---

## 4. 能防什么 / 不能防什么（逐条依据）

### 4.1 防：同网段被动嗅探（T1）

**依据（已确认）**
- 每个网页连接先做一次临时 X25519 密钥交换（一种双方各自临时生成、用完即弃的密钥协商算法），服务端每连接换新密钥（ADR 0010 第 1 条；`handleHandshakeGet/Post`，`web_terminal_service.cpp:433-558`）。
- 共享秘密经 HKDF-SHA256 派生成两条方向独立的 256 位密钥（`info` 带方向标签），每个方向独立 64 位计数器 nonce（ADR 0010 第 2-3 条）。
- 业务帧用 ChaCha20-Poly1305（带完整性校验的流加密；改一个字节就解密失败）密封；认证失败直接丢帧断开，无明文回退、无版本降级（ADR 0010 第 4、7 条）。
- TOTP 码、TOTP 密钥、绑定令牌、会话票据只出现在密文帧或 URL `#` 片段；`#` 片段浏览器不会发给服务器（ADR 0010 第 5 条；`web_terminal_service.cpp:1371`）。
- 篡改、错会话 ID、重放同一 nonce 均被拒（用例 `web_crypto_tamper_rejected`、`web_crypto_replay_rejected`，`tests/test_modules/test_terminal_web_service.cpp:192-215`）。
- 抓包仍能看到：请求行、Header、静态资源、握手公钥、SSE 流票据查询参数；这些不含上述秘密（ADR 0010 §9）。

**边界**：被动嗅探者能看出“有人在用网页终端”、大致输入节奏（帧大小/时序），但看不到内容。

### 4.2 不防：主动中间人（T2）

**依据（已确认设计取舍）**
- 首轮握手在明文 HTTP 上交换 X25519 公钥，**没有证书、没有预共享指纹、没有签名**。中间人可以把客户端的公钥换成自己的、把服务端的公钥换成自己的，双方各自与中间人协商，中间人读到全部明文再转发。
- ADR 0010 第 9 条明确列出“不防：主动 MITM、ARP / DNS 欺骗、连接劫持”。
- 因此 T2 不仅能看，还能改（注入命令、替换输出）、能偷（TOTP 码、票据、TOTP 密钥）。

**处置**：只在可信局域网使用；不可信网络必须在前面加 TLS 反向代理（见 R1）。

### 4.3 防登录 / 不防发现与拒绝服务（T3）

**防住的部分（已确认）**
- 未绑定者没有 TOTP 密钥，`auth.login` 必失败；没有票据，业务消息返回内层 `401 bad_ticket`（`web_terminal_service.cpp:609-628`）。
- 登录失败按客户端地址限流：60 秒内 5 次失败即锁定，返回内层 `429 locked` 与 `retryAfter`（`web_auth.h:131-132`；`web_terminal_service.cpp:796-836`）。
- 六位码空间 100 万，服务端只接受当前步及前后各一步（共 3 个有效码，`web_auth.h:59-62`；`web_auth.cpp:156-165`），5 次/分钟窗口下暴力猜中概率可忽略。
- 握手限流：全站 32 次/60 秒、单客户端 12 次/60 秒；超限 HTTP 429 `too_many_requests` + `Retry-After`（`web_auth.h:128-129`；`web_terminal_service.cpp:433-479`）。

**挡不住的部分（已确认）**
- 端口扫描能发现服务存在（静态页、`/api/status` 均可达）。
- 资源上限会被占用：会话 64、SSE 订阅 8、票据 64、流票据 64（`web_crypto_session.h:18-22`、`web_bridge.h:23`、`web_auth.h:79-81`）。攻击者可反复握手占满会话（空闲 24 小时才清理，`web_crypto_session.cpp` 的 prune），让正常用户收到 409 `too_many_sessions`；已登录者也可占满 8 条 SSE，让后续查看失败（503 `too_many_streams`）。这是可用性问题，不是机密性问题（见 R7）。

### 4.4 不防：本机其它进程 / 同用户程序（T4）

**依据（已确认）**
- 回环请求免票据、免 TOTP（`web_terminal_service.cpp:609-610` 的 `if (loopback) return true`），本机同用户进程直接连 `127.0.0.1:7878` 即可操作终端；仍需实现握手与加密，但攻击者就在本机，可以照做。
- `cookieKey` 存在 SQLite `app_secrets`，当前全平台按明文进列，保密只靠文件权限（`src/platform/credential_protection.h:6-10`；`credential_protection_posix.cpp:2`；`credential_protection_win.cpp:2-4`）。
- `web_tickets.dat` 存票据 ID 与到期时间（`web_auth.h:5-10`）。拿到 `cookieKey` + 未吊销的票据 ID，即可重算 HMAC 伪造合法票据（`web_auth.cpp:194-229`），**绕过 TOTP**。
- 进程内存里的密钥可被同用户调试器 / 内存抓取工具读取；进程被要求 `OPENSSL_cleanse` 只解决正常退出，不解决被读（`web_terminal_service.cpp:163-170`）。

**处置**：把“同用户进程”视为已失守；不要在同一账户下运行不可信程序；操作系统账户隔离与磁盘加密是这一层的实际边界（见 R2）。

### 4.5 取决于扩展权限：浏览器扩展（T5）

**依据（逻辑推断，应用层无法验证）**
- 扩展若拥有本站点的 host 权限，可在页面上下文读取 DOM、`localStorage`（票据就在 `devdesk.web.ticket`，`src/module_terminal/web/www/protocol.js:388-396`）、按键事件，甚至改写解密函数拿到明文。
- 应用不能区分“人操作”与“扩展代操作”；票据是 bearer 语义（谁持有谁可用），没有绑定设备指纹或浏览器密钥。

**结论**：不装不可信扩展；安全边界在浏览器与扩展商店，不在本应用。

### 4.6 基本防：恶意网页 / 跨站读取（T6）

**依据（已确认）**
- `Host` 白名单：仅回环、localhost 与“本机网卡 IP”命中；其他一律 HTTP 403，不进路由（`web_routes.cpp:70-78`；`src/platform/http_server.h:88`）。
- `Origin` 白名单：只接受 `http://` + 允许的 Host，其他 403（`web_routes.cpp:81-85`；`web_terminal_service.cpp:418-430`）。
- 无 CORS 放行头；页面 CSP 为 `default-src 'self'; script-src 'self'; style-src 'self'; img-src 'self' data:`，并带 `nosniff`、`no-referrer`（`web_routes.cpp:10-19`）。
- 业务请求必须带加密帧；跨站页面即使能发简单请求，也无法构造合法密文（`web_terminal_service.cpp:560-568`：解密失败一律 400 `bad`，不区分原因）。
- 恶意域名解析到本机也会因 `Host` 不在白名单被 403，挡住常见 DNS 重绑定路径。

**残余**：跨站仍可发“盲请求”消耗握手/会话配额（见 R7）；若用户浏览器已被扩展或恶意软件控制，回到 T5/T10。

### 4.7 有限防：二维码抢绑（T9）

**依据（已确认）**
- 绑定令牌 32 字节随机、5 分钟有效；`bind.claim` 在有效期内可重复取回同一密钥（幂等），`bind.confirm` 成功即消费（`web_auth.h:28-31, 38-56`）。
- 同一令牌再次 claim 返回 `bind_used`（内层 409），确认错码累计 5 次令牌作废（`web_terminal_service.cpp:1285-1301`；用例 `webAuthConfirmConsumes`、`web_bind_totp_revoke_nonloopback_http`）。
- 绑定成功前没有票据；绑定后旧令牌失效。

**残余**：拍照者若在合法用户之前完成 claim + confirm，可抢绑；这是“物理在场”假设的边界（见 R8）。

### 4.8 明确不防（ADR 0010 第 9 条同款）

- 主动重放：60 秒内重放 SSE 流票据只能打开一条读不到明文的只读流；主动中间人可重放整个连接并读到明文（回到 T2）。
- 流量分析：帧大小与时序可推断按键节奏、是否在输出。
- 本机被控、浏览器被控、手机被控、操作系统/驱动/硬件被控。
- 用户主动把二维码、票据、TOTP 种子发给别人。

---

## 5. 验证方案（可执行步骤）

前置：
- 应用已构建并启动；桌面设置页已开启网页访问（默认端口 7878）。
- 手机与电脑同一 Wi-Fi；已完成扫码绑定并能用 TOTP 登录。
- 命令在仓库根执行；Windows 把 `build/tests/devdesk_tests` 换成 `build\tests\devdesk_tests.exe`。
- **证据分级**：标“已有用例”的是仓库内自动化用例；标“待实测”的是本次文档未运行的现场步骤。**未运行不等于通过。**

### V1 抓包看 SSE 与 POST 密文（对应 T1 / 4.1）

```bash
# 1) 桌面机抓包（macOS 用 en0，先用 tcpdump -D 确认上网网卡；需要 sudo）
sudo tcpdump -i en0 -s 0 -A 'tcp port 7878' > /tmp/devdesk-web-cap.txt

# 2) 保持抓包，在手机网页终端里输入并回车：
#    echo DEVDesk-SENTINEL-9f3a
#    同时完成一次登录（输入当时的六位码），并等终端有输出。

# 3) 停止抓包（Ctrl-C），逐条断言：
grep -c 'GET /api/hs'            /tmp/devdesk-web-cap.txt   # 期望 >=1，证明抓到了握手
grep -c 'text/event-stream'      /tmp/devdesk-web-cap.txt   # 期望 >=1，证明抓到了 SSE
grep -c 'DEVDesk-SENTINEL-9f3a'  /tmp/devdesk-web-cap.txt   # 期望 0，终端内容不可读
grep -c 'otpauth://'             /tmp/devdesk-web-cap.txt   # 期望 0，TOTP 密钥不可读
grep -c '"secret"'               /tmp/devdesk-web-cap.txt   # 期望 0，密钥字段不可读
# 把登录时用的六位码原样替换到下面；期望 0
grep -c '<当时的六位码>'          /tmp/devdesk-web-cap.txt
# 把二维码里的 #b= 后面的令牌原样替换到下面；期望 0（片段不会发给服务器）
grep -c '<绑定令牌>'              /tmp/devdesk-web-cap.txt
# 证明密文帧确实存在（SSE 的 data: 后是 base64url 密文，长度 >=20）
grep -Ec 'data: [A-Za-z0-9_-]{20,}' /tmp/devdesk-web-cap.txt  # 期望 >=1
```

判定：前 4 条秘密断言全 0，且两条“抓到了”的守卫断言 >=1，才算 V1 通过；抓包为空时不得判通过（守卫断言会拦住）。
明文可见项（预期内，不算失败）：`GET /app.js`、`GET /api/hs`、`GET /api/stream?t=...`、握手 JSON 里的 `hid/spk/salt/alg/v`。

Windows 替代：用 Wireshark 过滤 `tcp.port == 7878`，Follow TCP Stream，执行同样的“搜哨兵串/`otpauth`/六位码”断言；或 `pktmon start --capture --comp nics` 后导出 pcapng。

### V2 回环明文 POST 被拒（对应 4.1 / 4.6）

```bash
# 不带握手、直接发明文业务消息：必须被拒
curl -sS -i -X POST http://127.0.0.1:7878/api/e \
  -H 'Content-Type: application/octet-stream' \
  --data '{"m":"auth.login","b":{"code":"123456"}}'
# 期望：HTTP/1.1 400，响应体含 "e":"bad"；不得返回票据或任何秘密

# 未鉴权状态接口只回非秘密字段
curl -sS http://127.0.0.1:7878/api/status
# 期望：含 running/port/bindState/bound/tokenRemainingSec；
#       不含 secret / t / hid / online / 设备名 / 绑定时间
```

### V3 旧票据撤销后失效（对应 T7 / R5）

现场步骤（待实测）：
1. 手机登录后，在浏览器开发者工具 → Application → Local Storage 记下 `devdesk.web.ticket` 的值（这就是 7 天票据）。
2. 桌面设置页点“解除绑定 / 撤销验证器”（或网页内 `bind.revoke`），服务端执行 `tickets_.revokeAll()` 并广播 `revoke`（`web_terminal_service.cpp:914-936`）。
3. 手机刷新页面。预期：落到登录/绑定页；任一业务请求的内层返回 `401 bad_ticket`，前端清掉本地票据（`www/protocol.js:922-923`）；终端内容不再出现。
4. 若页面仍能进终端，判失败。

自动化证据（已有用例，本次未运行）：
```bash
bash scripts/build-test-run.sh --dev --no-run --filter webAuthTicketIssueValidate
# 断言：revoke 后 validate 失败、revokeAll 后全部失败、7 天到期即失败
bash scripts/build-test-run.sh --dev --no-run --filter web_bind_totp_revoke_nonloopback_http
# 断言：非回环全链路 bind.claim → TOTP confirm → bind.revoke 成功，绑定时间清零
# 注意：web_cookie_key_persists_across_restart 只存在于当前工作树，未提交；
# 已提交 HEAD 上不要拿它当“重启后票据仍有效”的证据。
```

### V4 绑定令牌复用被拒（对应 T9 / 4.7）

现场步骤（待实测）：
1. 用手机 A 完成扫码绑定。
2. 在手机 B（未绑定）打开同一个二维码 URL（含同一个 `#b=<令牌>`），完成握手后让前端发起 `bind.claim`。
3. 预期：内层 `409 bind_used`（前端文案“二维码已使用”）；不得下发新的 TOTP 密钥。
4. 另测：绑定页连续输错 5 次六位码，预期第 5 次后令牌进入终态，内层 `409 bind_invalid`，后续必须回桌面重新生成二维码。

自动化证据（已有用例）：
```bash
bash scripts/build-test-run.sh --dev --no-run --filter webAuthConfirmConsumes
bash scripts/build-test-run.sh --dev --no-run --filter web_bind_totp_revoke_nonloopback_http
```

### V5 握手限流触发（对应 T3 / 4.3）

```bash
# 同一客户端 60 秒内第 13 次握手应被拒（单客户端上限 12/60s）
for i in $(seq 1 13); do
  curl -s -o /tmp/hs-body -w "%{http_code}\n" http://127.0.0.1:7878/api/hs
done
# 期望：前 12 次 200，第 13 次 429；响应头有 Retry-After，体含 "e":"too_many_requests"
```
注意：该循环也会消耗全站 32/60s 配额，测完等 60 秒再测别的。

自动化证据（已有用例）：
```bash
bash scripts/build-test-run.sh --dev --no-run --filter web_rate_limit_per_client
bash scripts/build-test-run.sh --dev --no-run --filter webAuthHandshakeRateLimit
bash scripts/build-test-run.sh --dev --no-run --filter webAuthLoginLockout
# 登录锁定断言：5 次失败/60s 后 isLocked，同窗口内正确码也不放行
```

### V6 Host / Origin 伪造被拒（对应 T6 / 4.6）

```bash
curl -s -o /dev/null -w '%{http_code}\n' -H 'Host: evil.example' http://127.0.0.1:7878/
# 期望 403
curl -s -o /dev/null -w '%{http_code}\n' -H 'Origin: http://evil.example' http://127.0.0.1:7878/
# 期望 403
```
自动化证据（已有用例）：`bash scripts/build-test-run.sh --dev --no-run --filter web_origin_host_rejected`。

### V7 传输层正确性 / 篡改 / 重放（对应 4.1）

```bash
bash scripts/build-test-run.sh --dev --no-run --filter web_crypto_handshake_roundtrip
bash scripts/build-test-run.sh --dev --no-run --filter web_crypto_tamper_rejected
bash scripts/build-test-run.sh --dev --no-run --filter web_crypto_replay_rejected
```
预期：握手双向往返、篡改信封/错会话 ID 一律拒绝且不可区分、同一 nonce 第二帧被重放窗口拒绝。

### V8 票据与资源上限（对应 4.3）

```bash
bash scripts/build-test-run.sh --dev --no-run --filter webAuthTicketTableCap
# 期望：64 条上限拒新、不淘汰有效票据；过期清理后放行
bash scripts/build-test-run.sh --dev --no-run --filter web_session_cap_64
bash scripts/build-test-run.sh --dev --no-run --filter web_stream_ticket_single_use
bash scripts/build-test-run.sh --dev --no-run --filter web_bridge_subscriber_cap
# 期望：会话 64、流票据单次、SSE 订阅 8 的第 9 个失败
```

### V9 TOTP 正确性与时钟窗口（对应 A7 / 4.3）

```bash
bash scripts/build-test-run.sh --dev --no-run --filter webAuthTotpRfc6238
# 期望：RFC 6238 官方向量通过；T-1/T/T+1 接受，更远拒绝；长度不符直接拒绝
```

### V10 状态接口不泄露秘密（对应 A10）

```bash
curl -sS http://127.0.0.1:7878/api/status | tee /tmp/status.json
grep -Ec '"(secret|t|hid|online|boundAt|device)"' /tmp/status.json   # 期望 0
```
自动化证据（已有用例）：`--filter web_status_plaintext_fields`。

### V11 交付前整套网页安全用例

```bash
bash scripts/build-test-run.sh --dev --no-run --filter web_
```
预期：网页终端相关用例全绿；任一条红即不得宣称威胁模型已落实。**本次未运行**，需 QA 在收口阶段补跑并留输出。

---

## 6. 残余风险与处置

“状态”含义：**已接受** = 产品已明确取舍，不再改；**待决策** = 需要产品/安全拍板；**建议改** = 低成本可降低风险。

| 编号 | 残余风险 | 影响 | 现有缓解 | 处置建议 | 状态 |
|---|---|---|---|---|---|
| R1 | 主动中间人 / ARP / DNS 欺骗可完全接管会话 | 高：读、改、偷 TOTP 与票据 | 无（ADR 0010 明确不防） | **不可信网络必须加 TLS 反向代理**（Caddy/Nginx，自签或内网 CA）或 SSH 隧道；界面文案只能写“会话加密（不防主动中间人）” | 已接受 |
| R2 | 本机同用户进程可回环直连；工作树版本还可读明文 `cookieKey` + 票据 ID 伪造票据 | 高：回环即绕过 TOTP；工作树版本可离线伪造票据 | 回环免验；文件权限 0600 / NTFS ACL；票据吊销表；HEAD 的每进程随机密钥让盘上票据重启即废 | 不把不可信程序跑在同一账户；磁盘加密；若威胁模型升级，考虑回环也验 TOTP；`cookieKey` 持久化与系统保护后端的取舍需产品重新裁决 | 待决策 |
| R3 | 有站点权限的浏览器扩展可读解密后 DOM、`localStorage` 票据并注入操作 | 高：等同已登录 | 无（浏览器边界） | 不装不可信扩展；高风险环境用独立浏览器配置文件；票据加设备绑定/短轮换可降低单次泄露价值 | 已接受 |
| R4 | 流量分析可推断按键节奏、输出活跃度 | 低：泄漏行为模式，不泄漏内容 | 无 | 若场景敏感，考虑固定长度填充/随机延迟（会增开销），当前不做 | 已接受 |
| R5 | 票据是 bearer、7 天绝对有效、不轮换、不绑设备 | 中：被偷后长期可用 | 可手动撤销全部票据；退出登录删单条；吊销表 ≤64 | 缩短 TTL（如 24 小时）并滚动续签；或把票据与浏览器生成的非导出密钥绑定 | 待决策 |
| R6 | 密钥生命周期不一致：TOTP 密钥只存内存；工作树又把 `cookieKey` 持久化 | 中：工作树下重启后旧票据仍可验签、但 TOTP 不可用，“重启免登录”语义不成立；被盗票据不因重启失效。HEAD 下重启全部失效，安全但体验差 | 工作树 `web_cookie_key_persists_across_restart`；HEAD 每进程随机 | 明确产品口径：要么 TOTP 密钥也进 `app_secrets`（恢复免重绑），要么重启即吊销全部票据（安全优先）；二选一写进 PRD，并先提交对应代码 | 待决策 |
| R7 | 资源上限可被占满：64 会话 / 8 SSE / 64 票据 / 64 流票据 | 中：可用性拒绝服务 | 握手限流、24 小时空闲清理、单客户端限流 | 降空闲 TTL、按 IP 限并发会话、对未认证会话更短超时 | 建议改 |
| R8 | 二维码被拍照后 5 分钟内抢绑；手机/TOTP 种子被拿到 | 中：登录入口被接管或第二因子失效 | 32 字节随机、5 分钟、单次消费、5 次错码作废 | 桌面显示二维码时提示“不要拍照/投屏”；绑定成功后明确提示；高风险场景改用更短 TTL | 已接受 |
| R9 | `cookieKey` 与票据文件可能进用户备份、磁盘镜像、core dump | 中：离线读取后可伪造票据 | `app_secrets` 不随 SQL dump 上 git（`secrets_store.h` 注释）；文件权限收紧 | 明确备份范围；关闭/限制 core dump；磁盘加密；见 R2 | 待决策 |
| R10 | 登录锁定与限流状态只在内存，应用重启即清零 | 低：本机攻击者可重启应用重置计数；网络攻击者无法重启 | 仅内存计数 | 若把本机攻击者纳入模型，失败计数落盘并带时间窗；否则接受 | 已接受 |
| R11 | 纯 JS 密码学实现（X25519 / HKDF / ChaCha20-Poly1305）存在实现错误与侧信道风险 | 中：实现错会导致“看似加密实际可破” | RFC 7748/5869/8439 测试向量、三引擎 E2E（`tests/e2e/crypto_vectors.mjs`）、篡改/重放用例 | 每次改前端加密代码必须跑向量与三引擎矩阵；不做“自己优化”的数学改动；上线前第三方审计 | 建议改 |
| R12 | 前端静态资源被篡改（本地文件替换、构建污染）可窃取票据与明文 | 高：等同 XSS | 资源嵌入二进制、CSP `'self'`、无第三方脚本、发布包校验 | 发布产物签名/校验和；安装目录权限；构建链可复现 | 待决策 |

---

## 7. 来源与核对表

| 结论 | 依据 | 等级 |
|---|---|---|
| 传输 = 每会话 X25519 + HKDF-SHA256 + ChaCha20-Poly1305，HTTP 明文 | ADR 0010 v2；`web_terminal_service.cpp:487`；`web_routes.cpp:134-163` | 已确认 |
| 被动嗅探看不到内容/凭据；主动 MITM 不防 | ADR 0010 §9；握手无证书/无指纹 | 已确认（设计取舍）+ 待实测（V1） |
| 回环免票据/TOTP，但不免握手与加密 | `web_terminal_service.cpp:609-610`；V2 | 已确认 |
| 票据 16B ID、7 天绝对有效、≤64、HMAC 常量时间校验 | `web_auth.h:79-81`；`web_auth.cpp:194-229`；`web_cookie_key_persists_across_restart` | 已确认 |
| TOTP 6 位、30 秒、±1 步、RFC 6238 | `web_auth.h:59-62`；`web_auth.cpp:156-165`；`webAuthTotpRfc6238` | 已确认 |
| 绑定令牌 32B、5 分钟、claim 幂等、confirm 消费、5 次错码作废 | `web_auth.h:28-31,38-56`；`web_terminal_service.cpp:867-913` | 已确认 |
| 握手 32/分全站 + 12/分单客户端；登录 5 次/分锁定 | `web_auth.h:128-132`；`web_terminal_service.cpp:433-479,796-836` | 已确认 |
| 会话 64、SSE 8、票据 64、流票据 64/60 秒 | `web_crypto_session.h:18-22`；`web_bridge.h:23`；`web_auth.h:79-81` | 已确认 |
| 工作树：`cookieKey` 明文进 `app_secrets`，靠文件权限；HEAD：每进程随机、不落盘 | `credential_protection.h:6-10`；`credential_protection_posix.cpp:2`；`web_terminal_service.cpp:224-255`；`git show HEAD:src/module_terminal/web/web_terminal_service.cpp` | 已确认（工作树为未提交改动） |
| TOTP 密钥仅内存、不落盘 | `web_terminal_service.h:177`；`web_terminal_service.cpp:163-170,881,904` | 已确认 |
| `Host`/`Origin` 白名单 403；CSP `'self'` | `web_routes.cpp:10-19,70-85`；`http_server.h:88` | 已确认 |
| 现场抓包、回环拒绝、撤销票据、令牌复用、限流触发 | 本文 §5 V1–V6 | 待实测（本次未运行） |

---

## 8. 交付边界（诚实声明）

- 本文依据的是**当前工作树代码**与仓库内既有用例；工作树中 `cookieKey` 持久化、绑定/TOTP 相关改动**尚未提交**，已提交 HEAD 的差异已在 A6/R6 标明。本次只写文档，**未运行抓包、未跑测试、未做真机验证**。
- V1–V11 中标注“待实测”的步骤必须由 QA 在收口阶段执行并留输出；未执行不等于通过。
- 本文不改变 ADR 0009 / 0010 的既有决策；R2、R5、R6、R9、R12 需要产品/安全拍板后才能动代码。
