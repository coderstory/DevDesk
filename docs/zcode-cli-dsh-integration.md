# ZCode CLI 接入 DSH：安装 + 配置全流程（标准化文档）

> 版本基线：zcode-app-cli 3.16.3-41（zcode-runtime 0.17.0）/ dsh-zcode-plan-bridge 0.5.0 ｜ 2026-10-30 核对
> 配套 skill：`zcode-dsh-setup`（本文件的命令行速查形态）；批处理派单走 `zcode-dispatch`（另一条线，见其 skill）。
> 池条目：需-14（池 27）。

## 目标

把 ZCode CLI（烧 Coding Plan 订阅额度，不走 API key）桥接进 DSH 当模型供应商。

**前提**：容器内只有 `/data` 可写（无 sudo），npm 全局必须 `--prefix /data`；npm 一律走国内镜像 `--registry=https://registry.npmmirror.com`。

## 1. 安装 ZCode CLI

```bash
npm install -g --prefix /data --registry=https://registry.npmmirror.com zcode-app-cli
```

- 入口：`/data/lib/node_modules/zcode-app-cli/bin/zcode.js`，软链 `/data/bin/zcode`
- 验证：`/data/bin/zcode --version`

## 2. 登录（CN 站点）

```bash
/data/bin/zcode login bigmodel
```

- 命令打印授权链接 → 用户在浏览器完成授权（可后台盯回调）
- 用 **bigmodel**（CN 站），不要 zai（国际站）
- 凭据：`/home/node/.zcode/v2/credentials.json`（`zcodejwttoken` = Start Plan 鉴权）
- 凭据会失效/被清，**重跑 login 即可，无需重装**

## 3. 安装 DSH 插件

```bash
cd /data/profiles/web && npm install --registry=https://registry.npmmirror.com dsh-zcode-plan-bridge
```

前置：CLI 已装且已登录。

## 4. 配置（两层 patch，层次别写错）

### profile 层 `/data/profiles/web/cordis.patch.yml` —— 插件配置

```yaml
- id: dsh-zcode-plan-bridge
  config:
    zcodeCliPath: /data/lib/node_modules/zcode-app-cli/bin/zcode.js
    authorizationPolicy: allow-all   # 默认 deny：agent 跑 Bash/写文件/联网被拒；allow-all 放开，不可信任务勿开
```

### 家目录层 `/data/cordis.patch.yml` —— 默认模型（优先级更高，写 profile 层会被盖回）

```yaml
- id: agent-default-model
  config:
    provider: zcode-cli
    model: GLM-5.3-Flash   # 满血换 GLM-5.3；均 1M ctx / 128K out / 支持图片
```

## 5. 生效与验证

- 插件首次加载需**重启 DSH**；之后的配置改动 HMR 热加载，不用重启
- 重启后刷新浏览器（旧标签页报「加载提供商目录失败: settings are unavailable」，刷新即消）
- 离线体检（不烧额度）：

```bash
ZCODE_CLI_PATH=/data/lib/node_modules/zcode-app-cli/bin/zcode.js \
  node /data/profiles/web/node_modules/dsh-zcode-plan-bridge/bin/dsh-zcode-plan-bridge.js doctor
```

同命令换 `models` 子命令列模型；报 "no models" = 未登录/凭据失效。
- 端到端：模型选择器出现 "ZCode CLI (account bridge)"，选 GLM-5.3-Flash，会话标记 `[model changed: … zcode-cli/…]`

## 6. 踩坑速查

| 症状 | 根因/修法 |
|---|---|
| npm 报 EACCES | 忘了 `--prefix /data` |
| 回合报 "Select a model before continuing" | 会话无模型，多为凭据失效（doctor 看 CREDENTIALS_MISSING）→ 重跑 `zcode login bigmodel`，失败的旧会话下次调用自动重建 |
| 配置改了不生效 | 被家目录层 `/data/cordis.patch.yml` 盖回（它优先级更高） |
| "Permission requests are denied by the plugin policy" | `authorizationPolicy` 还是 deny，改 allow-all（HMR 即时生效） |
| deny 死锁：agent 写不了自己的授权配置 | 用户手动 sed 一行，或把会话切回非桥接模型（如 zhipu）再改 |
| 直跑 CLI 报「无法定位内置配置」 | 补 env 对：`ZCODE_BUILTIN_PROVIDER_CONFIG_FILE=/Applications/ZCode.app/Contents/Resources/config/provider/zcode-builtin.json` + `ZCODE_PERSONAL_PROVIDER_CONFIG_FILE=~/.zcode/v2/provider_config.json` |
| headless `-p` 裸跑=全放开 | 必须显式 `--mode edit\|build\|plan`（批处理纪律见 zcode-dispatch） |
| 会话内 Bash 被权限层拦（债-7） | 闭环=zcode 写码+我方验证提交，别硬刚 |

## 7. 配套件

- **供应商自动切换**：`tools/zcode_provider_switch.py`（需-16，探测即判断/白嫖到底/失败告警，双 cron 已注册）
- **池工具面**：`tools/pool_mcp.py`（需-4，四工具+个人 TODO）
- 已知桥侧 bug 与 fork 自修路线：需-15（池 28），patch 已冻结待应用
