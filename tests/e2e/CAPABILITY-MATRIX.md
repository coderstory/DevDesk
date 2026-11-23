# 浏览器能力矩阵（need#490）

网页终端方案（技术卡 v3：X25519 + HKDF-SHA256 + AES-256-GCM）的浏览器端落地前提实测。
结果由 `tests/e2e/smoke.mjs` 生成，原始数据 `tests/e2e/artifacts/results.json` 与截图（artifacts 不入库）。

## 方法

- 引擎：Chromium（Playwright 自带，iPhone 移动模拟：375×667 / DPR 3 / touch / iPhone UA）、
  Microsoft Edge（`channel: 'msedge'`，本机已装）、WebKit（Playwright 自带，同 iPhone 模拟）。
- 每个引擎跑四个来源：`data:`、`file:`、`http://127.0.0.1:<port>`（localhost，安全上下文）、
  `http://<局域网IP>:<port>`（非安全上下文）。WebCrypto 只在安全上下文存在，故四来源都测。
- 权限：Chromium 系对 `http://127.0.0.1` origin 授予 `clipboard-read` / `clipboard-write`；
  `file:` / `data:` 无法按 origin 授权。WebKit 不支持 Playwright 的该授权，按无授权记录。
- 复跑：`cd tests/e2e && npm install && npx playwright install chromium webkit && npm run smoke`。

## 结果

引擎版本：Chromium（Chrome for Testing 159.0.8154.6，playwright build 1255）、
Edge 158.0.4370.33、WebKit（playwright webkit 27.2 / build 2388，见下）。

### 安全上下文与 WebCrypto（X25519 / HKDF-SHA256 / AES-256-GCM）

| 来源 | Chromium-mobile | Edge | WebKit |
|---|---|---|---|
| `data:` | secure=false，crypto.subtle 不存在 → 三项不可用 | 同左 | 同左 |
| `file:` | secure=true，X25519 ok / HKDF ok / AES-GCM ok | 同左 | 同左 |
| `http://127.0.0.1` | secure=true，X25519 ok / HKDF ok / AES-GCM ok | 同左 | 同左 |
| `http://<局域网IP>` | secure=false，crypto.subtle 不存在 → 三项不可用 | 同左 | 同左 |

### 剪贴板（navigator.clipboard）

| 来源 | Chromium-mobile | Edge | WebKit |
|---|---|---|---|
| `data:` | API 不存在 | API 不存在 | API 不存在 |
| `file:` | API 存在；writeText / readText 均 `NotAllowedError: permission denied` | 同左 | writeText ok；readText `NotAllowedError`（平台拒绝） |
| `http://127.0.0.1`（Chromium 系已授权） | writeText ok / readText ok | 同左 | writeText ok；readText `NotAllowedError`（平台拒绝） |
| `http://<局域网IP>` | API 不存在 | API 不存在 | API 不存在 |

### visualViewport

四来源、全部引擎：均存在（DOM API，不要求安全上下文）。

## 结论

1. **X25519 / HKDF-SHA256 / AES-256-GCM 三引擎全部可用**（Chromium / Edge / WebKit），前提是安全上下文：
   `https:`、`http://127.0.0.1` 或 `file:`。`data:` 与局域网明文 HTTP 下 `crypto.subtle` 直接不存在。
2. **网页终端必须以 HTTPS（或 localhost）提供**：局域网明文 HTTP 会同时失去 WebCrypto 与剪贴板。
   自签证书 / 反向代理是必要部署条件，不是可选项。
3. 剪贴板：`data:` / 局域网 HTTP 下 API 完全不存在。WebKit 下 `writeText` 在 `file:` 与 localhost 直接可用，
   但 `readText` 被平台拒绝（`NotAllowedError`，需用户手势 / 系统权限）；Chromium 系需对 origin 授权后读写均可。
4. 移动模拟下 DPR 3 与 visualViewport 均正常，375×667 视口可用作移动端 UI 基准。

## CI 可用性与降级建议

- 现有 CI（`.github/workflows/ci.yml`）跑 macOS `xcode-27` 与 `windows-2025`，**未预装 Playwright 浏览器**；
  需新增步骤 `npx playwright install --with-deps chromium webkit`（版本由 `package.json` 锁 1.65.1）。
  建议缓存 `~/Library/Caches/ms-playwright`（macOS）/ `%USERPROFILE%\AppData\Local\ms-playwright`（Windows）。
- **Edge**：`windows-2025` 自带 Edge，`channel: 'msedge'` 直接可用；GitHub macOS 镜像是否自带 Edge **需 CI 实测**。
  缺失时两种降级：`npx playwright install msedge`，或把 Edge 任务限定在 Windows job。
- **WebKit**：Playwright 自带构建，macOS/Windows 均可下载，无需系统 Safari；CI 与 Chromium 同一安装步骤。
- **CN 网络实测坑（2026-11-23）**：`cdn.npmmirror.com` 镜像缺 `webkit/2388/webkit-mac-26-arm64.zip`（404），
  Chromium 可从镜像快速装；WebKit 需走官方 `cdn.playwright.dev`（本机实测约 460 KB/s）。CI（海外）无此问题。

## 未测项

- 真机 iOS Safari / Android Chrome（当前只有 Playwright 模拟与桌面引擎）。
- 局域网 **HTTPS**（自签 / 内网 CA）下的 crypto 与剪贴板；本次只测了局域网明文 HTTP。
- `file:` 与备案/授权流程下带真实用户手势（click）后的剪贴板写读，以及 WebKit `readText` 的放行条件。
- CI 实跑（Edge 在 macOS runner 的存在性、WebKit 下载耗时）。
