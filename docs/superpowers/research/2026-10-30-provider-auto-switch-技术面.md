# 需-16 供应商自动切换·技术面分析（2026-10-30）

执行：老学究 ｜ 配套惨品业务稿（需-16「探测即判断」口径）｜ 只调研未动码

## 关键实查：切换点在哪（本机配置直读）

1. **zcode 侧供应商选择是一个 JSON 字段**：`~/.zcode/v2/provider_config.json` 的 `config.defaultModelSelection = {providerId: "account:bigmodel-start-plan", modelId: "GLM-5.3-Flash"}`。免费 Start Plan（`account:bigmodel-start-plan`）与付费个人编码套餐（credentials 里的 `account-provider:account:bigmodel-individual-coding-plan` 身份）就是在这个字段间切换——**自动切换 = 改这一个文件的一个字段**，无需动 DSH/bridge 任何东西。
2. **凭据侧**：`~/.zcode/v2/credentials.json` 的 `oauth:active_provider` 键随 provider 走；失效自愈路径已知（`zcode login bigmodel`）。
3. **DSH/bridge 侧不用切**：bridge 只是把 zcode CLI 包成 DSH 的 `zcode-cli` provider；DSH 层默认模型走哪边是 GUI 选择项，改 zcode 层的 defaultModelSelection 后 bridge 转述的即新选择。

## 探测方案（「探测即判断」的落地形态）

- **可用性探测**：最小真实回合——`zcode` CLI 起一次极小 prompt（几十 token）走 Start Plan；成功=可用，报错且错误含 quota/402/余额类=不可用。比猜 HTTP 配额接口可靠（接口未公开、易碎）。
- **余额不足信号**：探测回合的失败报文即判定源；另可在 bridge 日志（`~/.zcode/v2/logs`）做副证。
- **防误切**：探测失败先重试 1 次再切，避免网络抖动把免费档误切走；切回方向同理。

## 切换执行器形态（供 spec 采）

- 一个薄 Python/Node 脚本：读 `provider_config.json` → 探测 → 决定 providerId → 写回（原子写 tmp+rename）→ 记一行 status_log；挂 DevDesk agent 定时调度（23:05 / 9:05 两个 cron 点，同一「探测即判断」逻辑）。
- 失败兜底：写回失败或探测异常 → 告警不静默（业务稿已定）。
- **权限口径仍是前置卡点**（惨品已列）：写 `~/.zcode/v2/provider_config.json` 属跨软件配置写入，需老板授权；授权后本方案全自动；不授权则降级为探测+提醒半自动。

## 成本

零新依赖（字段读写+CLI 调用+定时调度全是现成件），脚本+验收（真探测真切换各一轮）半天级。需-14/15 的 bridge 修复不影响本需求——切换动作在 zcode 配置层，不碰 bridge 代码。

## 来源

本机配置直读：`~/.zcode/v2/provider_config.json`、`credentials.json` 键清单、`dsh-zcode-plan-bridge/lib/index.js` provider 注册注释。
