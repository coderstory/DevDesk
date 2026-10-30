# ZCode CLI 接入 DSH：安装 + 配置全流程（标准化文档）

> 版本基线：zcode-app-cli 3.16.3-41（zcode-runtime 0.17.0）/ dsh-zcode-plan-bridge 0.5.1（fork main，含修复）｜ **2026-10-31 按本机装机实测改定（容器 /data 旧版已废弃）**
> 配套 skill：`zcode-dsh-setup`（`~/.claude/skills/zcode-dsh-setup/SKILL.md`，本机实态速查）；批处理派单走 `zcode-dispatch`（另一条线，见其 skill）。
> 池条目：需-14（池 27，已关）。本机装机实例：tauri profile（见下）。

## 目标

把 ZCode CLI（烧 Coding Plan 订阅额度，不走 API key）桥接进 DSH 当模型供应商。

## 本机关键事实（先读，防踩坑）

1. **profile 以运行实例的 `--profile` 参数为准**：桌面版 DSH 后端是 `--profile tauri`，插件必须装进 `~/.dsh/profiles/tauri`（装错 profiles/web 等于没装——已踩过）。
2. **桌面版 CLI 路径**：`/Applications/ZCode.app/Contents/Resources/glm/zcode.cjs`（容器 `/data` 路径不适用本机）。
3. **CLI 必须带两个 env 才能起**（裸跑报「无法定位 ZCode Built-in Provider Config」）：
   - `ZCODE_BUILTIN_PROVIDER_CONFIG_FILE=/Applications/ZCode.app/Contents/Resources/config/provider/zcode-builtin.json`
   - `ZCODE_PERSONAL_PROVIDER_CONFIG_FILE=~/.zcode/v2/provider_config.json`
   - 注意 provider_config.json 里多一层 `config` 包裹（`config.defaultModelSelection`）。
   - GUI 版 DSH 进程要吃到这两个 env：用 `launchctl setenv` 注入（本机已配 LaunchAgent `cn.coderstory.zcode-env`/`-env2` 持久化）。
4. **安装只认官方途径**：插件管理 GUI 或 profile 根 pnpm 正路登记；**禁止 rsync/npm pack 直拷 node_modules**（加载器不认，装了等于没装——已踩过）。
5. **动 `~/.dsh` 任何配置前**：先带时间戳备份、先报备要改的键、增量改不重写（OOBE 事故教训）。

## 1. 登录 CLI（CN 站 bigmodel）

```bash
export ZCODE_BUILTIN_PROVIDER_CONFIG_FILE=/Applications/ZCode.app/Contents/Resources/config/provider/zcode-builtin.json
export ZCODE_PERSONAL_PROVIDER_CONFIG_FILE="$HOME/.zcode/v2/provider_config.json"
node /Applications/ZCode.app/Contents/Resources/glm/zcode.cjs login bigmodel
```

- 命令打印授权链接 → 用户在浏览器完成授权
- 用 **bigmodel**（CN 站），不要 zai（国际站）
- 凭据：`~/.zcode/v2/credentials.json`（`zcodejwttoken` = Start Plan 鉴权）；失效重跑 login，无需重装

## 2. 安装桥插件（官方途径）

- 首选：DSH 界面 Settings → Plugins 页搜索安装 `dsh-zcode-plan-bridge`；
- 无 GUI 时：profile 根 `~/.dsh/profiles/tauri` 执行 `pnpm add dsh-zcode-plan-bridge`（npm 在 pnpm workspace 会报错）。

**亲验文件实态**（三条都过才算装上）：

```bash
ls ~/.dsh/profiles/tauri/node_modules/dsh-zcode-plan-bridge/lib/app-server.js   # 文件在
grep -q dsh-zcode-plan-bridge ~/.dsh/profiles/tauri/package.json                # 依赖登记在
```

## 3. 双层配置（增量，不动既有行）

- **profile 层** `~/.dsh/profiles/tauri/cordis.patch.yml`（追加条目，不覆盖文件）：

```yaml
- id: dsh-zcode-plan-bridge
  config:
    zcodeCliPath: /Applications/ZCode.app/Contents/Resources/glm/zcode.cjs
    authorizationPolicy: allow-all   # 默认 deny；allow-all 才能让 agent 跑 Bash
```

- **家目录层** `~/.dsh/cordis.patch.yml`（优先级更高，改键先备份）：默认模型 `provider: zcode-cli` / `model: GLM-5.3-Flash`。

## 4. 启用

插件首载需**重启 DSH**；之后的配置改动 HMR 热加载。重启后刷新浏览器（旧标签页可能报「加载提供商目录失败」，刷新即消）。

## 5. 验收判据（两条齐 = 完成）

1. DSH 供应商列表出现 **ZCode CLI (account bridge)**；
2. 选 GLM-5.3-Flash 发真回合，**返回内容非报错**（token 实出）。

离线辅助（不烧额度）：桥的 `doctor` 子命令看前置（凭据 JWT、activeProvider）；注意 doctor/models 需要 CLI 报目录，**同样要带双 env**。models 报 no models = 凭据失效，重跑 login。

## 6. 踩坑速查

| 症状 | 修法 |
|---|---|
| 供应商列表没有 zcode | 先查插件装在哪个 profile（运行实例 `--profile` 参数为准），再查 node_modules/package.json 实态 |
| CLI 起不来「无法定位内置配置」 | 缺双 env，见「本机关键事实 3」 |
| "Select a model before continuing" | 凭据失效 → 重跑 login，旧会话自动重建 |
| 配置改了不生效 | 家目录层优先级更高盖回；或插件首载未重启 |
| "Permission requests are denied by the plugin policy" | authorizationPolicy 还是 deny（HMR 即时生效） |
| deny 死锁：agent 写不了自己的授权配置 | 用户手动 sed，或切回非桥接模型再改 |
| 会话内 Bash 被权限层拦 | zcode 写码 + 我方验证提交（闭环预案） |

## 7. 配套件

- **供应商自动切换**：`tools/zcode_provider_switch.py`（需-16，探测即判断/白嫖到底/失败告警，双 cron 已注册）
- **池工具面**：`tools/pool_mcp.py`（四工具 + 个人 TODO + 审计四件套）；ZCode CLI 侧注册在全局 `~/.zcode/cli/config.json` 的 `mcp.servers.devdesk-pool`（stdio，venv python 直跑脚本；**ZCode 不支持项目级 MCP**——`.mcp.json` 是插件包专用、项目 `zcode.json`/`.zcode/config.json` 的 `mcp.servers` 均不生效，2026-10-31 实测），工具名 `mcp__devdesk-pool__pool_add/fetch/update/search/soft_delete/trace/stats/todo_set/todo_get`
- 已知桥侧 bug 与 fork 自修：fork `coderstory/dsh-zcode-plan-bridge` main（de8c28e，含修复），需-15 三场景真会话回归在装机实例上补验
