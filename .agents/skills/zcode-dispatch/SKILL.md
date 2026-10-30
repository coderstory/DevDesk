---
name: zcode-dispatch
description: 全队唯一大模型执行通道：双 env 入口+gate 收尾+空转判定+禁碰用户配置
---

# zcode-dispatch：全队唯一大模型执行通道（headless CLI 派单）

**全队最高口径（老板 2026-10-30 令）：所有大模型流量统一切入本通道**——一切需要大模型的分析、生成、写码、调研任务，一律经 zcode-dispatch 走免费额度执行；各 Bot 只做任务书、验收和集成，不自己直接烧付费 token。夜间窗口（23:00-09:00）烧 3 亿免费额度，白天正常档照旧。走不通的 case 报通道 owner（惨品）解决，不许悄悄绕回付费通道。
**通道定型：ZCode 写码、派单方收尾提交、QA 独立验收。**

## 入口（已验证，禁用 claude）
- 必带双 env：`ZCODE_BUILTIN_PROVIDER_CONFIG_FILE=/Applications/ZCode.app/Contents/Resources/config/provider/zcode-builtin.json` + `ZCODE_PERSONAL_PROVIDER_CONFIG_FILE`（个人套餐档，缺任一会身份缺失复现老问题；路径见登录后 ~/.zcode/v2/）
- 命令形：`<双env> node /Applications/ZCode.app/Contents/Resources/glm/zcode.cjs -p "$(cat 任务书)" --mode <edit|build|plan> --cwd <worktree路径> --json`
- `--mode` 必须显式传（--prompt 默认 yolo）；禁 yolo。续跑 `--resume sess_…`；机读 `--json`。前置自检 `zcode doctor`；`zcode login bigmodel` 保凭据（~/.zcode/v2/credentials.json）。
- 沙盒：要写 ~/.zcode，需全盘权限跑。

## 流程
1. 任务书 `docs/plans/<slug>-task.md`：目标、范围、验收标准（F-xx）、worktree 纪律（显式「从 dev 新建分支」）、回单三件套（sha+文件清单+先红后绿）、一行 `mode:`。禁 yolo。
2. 任务书加防空转硬性要求：**每完成一个产出立即 git commit，勿攒批**；写明总时长预算（纯文档 60 分钟量级）。
3. **收尾纪律**：代提交前必须过一次 `--filter gate` 全目标编译（定向 filter 是假绿灯）；判据=gate 全绿+相关域出 Total 行。ZCode Bash 权限层偶发「No permission client configured」——构建/测试/提交段由派单方接手收尾，排产即按此预算。
4. **空转判定**：进程活着但 worktree 零 commit + json 0 字节 + err 只有 not-due 心跳，超合理时长即杀重发（重发加防空转要求）。2 分钟零进展即杀（pgrep -f）。
5. 回单审计：sha+清单+先红后绿缺一即打回；QA 独立复跑，ZCode 产出代码变异抽查比人写高一档。
6. 并发纪律：同一时刻一个执行体；构建槽串行；同文件单写者；按池序逐条干，干完取下条。

## 已知坑
- 别传 `--model`（[1m] 报 unrecognized_model）；模型由客户端「默认模型」决定。
- 自建 worktree 可能复用旧分支基底——回单必核 diff 相符性；任务书写「从 dev 新建分支」可防。
- 信任前置：`~/.claude.json` projects 加 `hasTrustDialogAccepted:true`（改前备份）。
- **禁碰用户配置**（10-30 事故教训）：`~/.dsh` profiles、DSH 插件、模型配置任何人不得动；动配置类需求单独立项走老板授权。

## 边界
- ZCode 与 Claude 两个独立软件勿混配；入口统一 ZCode（老板 2026-10-30 令禁 claude）。
- 免费窗口 23:00-09:00 优先；bigmodel 套餐留白天；错峰纪律照旧。
- 供应商档位由切换脚本管（tools/zcode_provider_switch.py），CLI 不按单指定。
