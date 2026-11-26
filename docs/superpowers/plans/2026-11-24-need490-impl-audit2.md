# need#490 实现层审计核销（2026-11-24，复审）

对象：池 slug `2026-11-24-need490-impl-audit`；基线 dev 0e40903d，修复提交 0fd8ce92（HEAD f44b9d3a 无后续代码改动）。只读静态核验，未跑构建/测试（执行闸）。

## P0（已修2 / 部分2 / 未修0）
1. P0-1 QR：**已修**。`web_terminal_service.cpp:265-279` 由 `bindUrlLocked()` + `encode()` 填 qrMatrix；`web_access_card.cpp:354` 消费。旁证：探针仍自造矩阵，见 P1-9。
2. P0-2 revoke/持久化：**部分**。revoke 清 bound_/boundAt_/lastLogin_/TOTP 且落盘、bind.revoke 已加锁：`web_terminal_service.cpp:192-209`、`:801-812`；票据 load/save 已接：`:116`、`:1231`、`web_auth.cpp:269-292`。**未修**：`cookieKey_` 每进程随机且不持久化（`:119`），旧票据 HMAC 重启后必 `bad_ticket`（`:512/:746`）；bound_/totpSecret_ 不持久化 → 重启后票据表与绑定态不一致。影响：PRD「重启免登录」仍不成立（同进程刷新可用）。
3. P0-3 跨线程：**部分**。HTTP 侧已不碰 controller_（input 只看 activeSessions_ `:559`；settings 入命令队列 `:663-686`；releaser 只置位 `web_session_bridge.cpp:195`，UI tick 还原 `:108-112`）。残留 2 处数据竞争见「新问题」。
4. P0-4 prune：**已修**。握手前 `:373`，tick `:957`（表自带锁）。

## P1（已修5 / 部分3 / 未修2）
1. stop 持锁调 `server_->stop()`：**未修** `web_terminal_service.cpp:169-175`（关服务/退出仍可死锁）。
2. 两层限流：**已修** `:349-350`，键 `__global__` 与客户端地址不混。但限流器无锁见新问题。
3. seq 锁内：**已修** `web_bridge.cpp:47-49`。
4. 播种/词表：**部分**。`module_terminal.cpp:52-58` 已播种，wire 值 followApp/independent/dark/light 与 `app.js:707-721` 一致；服务端选项表仍未下发（state 只回当前值 `:531-536`）。
5. Host 白名单刷新：**已修** tick 每 30s `:961-964` + `:1240`；成本=30s 一次网卡枚举，叶子锁无反转。
6. tab 标题：**部分**。改名与桌面同步已修（`web_session_bridge.cpp:90-94`、`terminal_page.cpp:303-313`）；OSC 0/2 仍未实现（仅注释 `web_session_bridge.h:53`）。
7. markDesktopRestart：**已修** `module_terminal.cpp:66-67`、`web_terminal_service.cpp:891/944`、前端 `protocol.js:603/818`、`app.js:217`。
8. OPENSSL_cleanse：**已修** `web_crypto_session.cpp:10-18`、`web_terminal_service.cpp:129-133`；残留 totpSecret_.clear() 与 bindToken_.token 未擦（P2 级）。
9. 测试缺口：**未修**。无 web_access_card 单测；E2E 无 bind/TOTP/revoke/限流；探针仍自造快照 `tests/probes/web_access_probe.cpp:145`。
10. 注释过期：**部分**。`protocol.js:7` 仍称 RealClient 只留签名；`web_session_bridge.h:53` 仍称 OSC 后续补。

## P2（已修0 / 部分4 / 未修2）
1. **部分**：save/load 已非死代码；`broadcastSettingsLocked` 仍只有声明 `web_terminal_service.h:140`；`kReplayWindow=128` 未与实现 64 位对齐 `web_crypto_session.h:20`。
2. **部分**：`/api/status` 已删 online/bindTime/lastLogin `:886-892`；tokenRemainingSec 仍明文。
3. **未修**：限流计数表条目只增 `web_auth.cpp:300-305`、`web_terminal_service.cpp:353-357`。
4. **部分**：补发队列已改 4KB `protocol.js:411-413`；发送失败仍丢队列 `:994-997`；`reconnect()` 无在流守卫 `:943-946`。
5. **部分**：自诊已改「0.0.0.0:port」`web_access_card.cpp:484`；`setConfiguredPort` 非法值仍静默返回 `:1187`。
6. **未修**：测试固定端口 47883 `test_terminal_web_service.cpp:332`；`scripts/check-product-deps.sh` 不存在；生产 `?demo=1` 仍可用 `app.js:13`（X-DevDesk-Test-Addr 已移除）。

## 新发现问题
- P1 `SlidingWindowLimiter` 无互斥：httplib 线程池并发调 `web_terminal_service.cpp:349-350`，std::map/deque 数据竞争（可崩）。
- P1 `CryptoSession::ticketId`（std::string）HTTP 线程写 `:514/:735/:794`，`authenticatedCount` 无同步读 `web_crypto_session.cpp:173` → UB。
- P2 unbound 时每帧 `snapshotLocked` 重跑 QR 编码（未缓存）。

## 结论
**仍需修**：P0-2 重启免登录不成立（cookieKey 未持久化）、P1 stop 死锁未修、2 处并发数据竞争。合计 已修7 / 部分9 / 未修4。
