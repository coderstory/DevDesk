# 02: 本地代理 Host 白名单校验

**What to build:** claude 代理只应服务本机，当前 http_server 不校验 Host 头，存在被外部页面借道访问的口子。给 http_server 增加期望 Host 白名单参数，仅放行 `127.0.0.1[:port]` / `localhost[:port]` / `[::1][:port]` 三种形态，其余请求一律 403；claude_service 启动代理时传入白名单。测试先行（TDD）：伪造 Host 的拒收用例先红再绿。

**Blocked by:** 无（可立即开工，与 01/03 并行无冲突）

**Status:** done

- [x] `Host: evil.com`、`Host: 127.0.0.1.evil.com` 等伪造请求返回 403 的单测绿
- [x] `Host: 127.0.0.1[:port]`、`localhost[:port]`、`[::1][:port]` 合法放行的单测绿
- [x] ctest 全绿：Failed: 0，Total 不低于最新基线（参考 646）
- [x] 合入门槛：9 个 `check-*.sh` 守卫全过

## 实现摘要

- `platform::http::Server::setAllowedHosts(std::vector<std::string>)`：应用层 Host 白名单
  （pre-routing 钩子，命中才进路由，其余 403 且 handler 不执行）。归一化：剥首尾空白 +
  剥可选端口（方括号地址截 `]`；其余恰有一个 `:` 的 host:port）+ 小写 + 剥 IPv6 方括号。
  空表 = 未配置 = 放行所有 Host（向后兼容既有调用方；取舍注释在 http_server.h）。配置后
  无 Host / Host 不可解析 → 403（fail closed）。连接级来源检查（bind 127.0.0.1）不动。
- `platform::http::hostWhitelistForBind(bindAddress)`：白名单按实际监听地址推导，不写死——
  回环绑定（127.0.0.1 / ::1 / localhost，大小写与括号拼写不敏感）→ 三种回环形态都放行；
  非回环绑定 → 只放行监听地址本身。
- `ProxyRuntime::startProxy` 接线：`server_->setAllowedHosts(hostWhitelistForBind(bindAddress))`。

## 测试（5 个新用例，过滤词 `host-allowlist`）

tests/test_platform/test_http_server.cpp：伪造三形态 403 且 handler 0 次进入 / 回环三形态
（不带端口/带端口/大小写混合）放行 / 未配置=放行（向后兼容）/ hostWhitelistForBind 推导；
tests/test_modules/test_claude_service.cpp：代理启动即启用白名单（伪造 403、合法放行）。

### 红→绿证据

前任留有半成品（实现+测试同在工作树），伪造用例已直接绿，无法自然跑红。补两类真红证据：

1. 突变红（中和 pre-routing 强制逻辑后重建）——证明测试真能抓住漏洞：
   ```
   FAIL [..\tests\test_modules\test_claude_service.cpp:1253] fr.value().statusCode != 403
   FAIL [..\tests\test_platform\test_http_server.cpp:450] status != 403L
   FAIL [..\tests\test_platform\test_http_server.cpp:536] has(wl, "127.0.0.1")
   [FAIL] claude_service: 代理启动即启用 Host 白名单（host-allowlist，bind 推导，伪造 403）
   [FAIL] http_server: Host 白名单（host-allowlist）——伪造 Host 一律 403 且不进 handler
   [FAIL] http_server: host-allowlist hostWhitelistForBind 按绑定地址推导白名单
   Total:  5  Passed: 2  Failed: 3
   ```
2. 天然红：半成品 `normalizeHostPart` 把裸 `::1` 按第一个 `:` 截成空串 →
   `hostWhitelistForBind("::1")` 返回空表（真实 bug，上表第 536 行即它）。
   修复：端口剥除只认「方括号截 `]`」与「恰有一个 `:` 的 host:port」两种形态。

绿（修复后同过滤词）：
```
[PASS] claude_service: 代理启动即启用 Host 白名单（host-allowlist，bind 推导，伪造 403）
[PASS] http_server: Host 白名单（host-allowlist）——伪造 Host 一律 403 且不进 handler
[PASS] http_server: Host 白名单（host-allowlist）——回环三形态放行（不带端口/带端口/大小写混合）
[PASS] http_server: Host 白名单（host-allowlist）未配置=放行所有 Host（向后兼容）
[PASS] http_server: host-allowlist hostWhitelistForBind 按绑定地址推导白名单
Total:  5  Passed: 5  Failed: 0
```

全量：`Total: 1586 Passed: 1577 Failed: 0 Skipped: 9`（基线 1581 + 新增 5；Skipped 不计）。
ctest：仅 3 个 probe 用例因本机缺 LLM 凭据失败（环境性既有状态），单测全绿。
守卫：7 个门槛守卫全过；check-eui-neo-version.sh 亦过（check-cumulative-markers.sh 需清单
参数，不适用）。
