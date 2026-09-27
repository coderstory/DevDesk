# 2026-09-27 任务编排板

主线：**先把代理打通验证（用户硬性前置）→ 并行修 UI 三线 → 审计 → 提交合 master → 排后续大项**。

## 规则（本编排执行纪律）

1. **build 是稀缺资源**：同一时刻只允许一个 agent 持有 build 目录；撞 `Permission denied` 的 agent 自行 `sleep 20` 重试（≤5 次）。
2. **每个子任务产物必须独立审计**（不信自报）：编译产物核对 + 读 diff + 跑定向测试 → 判 ACCEPT / REJECT。
3. **提交按原子单元**：一个需求一个 commit，禁 `git add -A`，禁并发 amend。
4. 修复范围 = 需求交付物缺陷；历史遗留/非需求项只记录不修。
5. 卡点（阻塞）单独列，不擅自替用户决定。

## 已完成（今日）

- **本地代理鉴权彻底移除**（无 token/401；仅监听 127.0.0.1）
- **TLS CA 修复**：`platform/http_client.cpp` 用 `CURLSSLOPT_NATIVE_CA`（静态 libcurl 无可用 CA 路径致 `Problem with the SSL CA cert`）
- **空值头不发送**：`http_client.cpp` 两个 header 循环加 `if (v.empty()) continue;`（代理用空 Host 表示让 libcurl 填，真发出去是畸形请求 → Cloudflare 400）
- **settings.json 写入面关闭**：接管默认关 + 启动补还原也关（app 启动不碰该文件，指纹前后一致已验证）
- **Claude HUD 安装**：`~/.claude/settings.json` 的 statusLine 换成 claude-hud 0.8.0（node `D:/software/node-v24.19.0-win-x64/node.exe`）；GSD 旧 statusline 备份在 `settings.json.bak.20260927-143445` + `plugins/claude-hud/previous-statusline.txt`
- **字体系统优先**（EUI-NEO `core/render/text.cpp`）：Win 微软雅黑 / macOS 苹方 / Linux Noto CJK 优先，打包装饰体降兜底（未提交、未肉眼验证）
- **布局审计已回（原 #3）**：9/9 条对得上原文行（主会话逐条核实），清单见下「布局缺陷清单」
- 规则写入 memory：**主会话禁止直接改文件**（写入全派子任务）、**并发子任务上限 7**

## 进行中（5 个 agent，上限 7）

| # | 任务 | 负责范围 | 验收标准 |
|---|------|----------|----------|
| 1 | 上游方言抽象 | `upstream_dialect`：UA 覆盖 / 会话头注入 / 模型名归一 / 鉴权头收敛 | 用 `python-urllib` 的 UA 打 8888，非流式与流式均 `OK status=200`，且带 `[1M]` 的模型名也 200 |
| 2 | 布局 9 条修复 | 按本文件「布局缺陷清单」段逐条改 | 逐条改 + 定向测试 + 全量无回归 |
| 3 | 托盘菜单窗高根因（只读分析） | 查「窗体底边↔状态栏」间距缺项 | 现状公式 + 缺项 + 改法位置 |
| 4 | Markdown Phase 3 spec 草稿 | 设计（2-3 周大项，≤500 行） | spec ≤500 行 + 里程碑拆分 + 开放问题 |
| 5 | 未提交 diff 独立审计 | 复核主会话先前直接改的 31 项 | 逐项读 diff 核对 → 判 ACCEPT / REJECT |

## 布局缺陷清单（原 #3 审计产物，主会话已核实）

| # | 位置 | 症状 | 严重度 |
|---|------|------|--------|
| 1 | `claude_module.cpp:646-697` | env 行 `ey += 54.0f` 无上限 → `actY = paneBottom+20` 把按钮组/表格顶出面板被裁，**按钮点不到**（1000 高窗 ≥11 行触发；表单同款编辑器有 `kMaxVisibleRows=5` 先例） | 功能 |
| 2 | `claude_module.cpp:1782/1856` | 导入 notes（y=486+i*15）与校验错误行（y=h−104，`formTab_!=0` 时画）env tab 同屏重叠 | 视觉 |
| 3 | `markdown_module.cpp:1199` | 状态栏左框 `w*0.6` + 右框 `w−244` 需 w≥670，双栏态必然重叠，两侧均无 clip | 视觉 |
| 4 | `todo_module.cpp:721` | 副标题 `contentW−560` 装 545px 文案，窗口 ≲1400 溢出压搜索框 | 视觉 |
| 5 | `todo_module.cpp:1171` | `+N` chip 在循环 break 后仍无条件画，越卡片右缘 ≤19px | 视觉 |
| 6 | `claude_module.cpp:680` | env 值 `.size(valW,48)` 无 clip，最小窗 valW≈194px | 视觉 |
| 7 | `settings_module.cpp:620` | 桌宠卡高 168（穿透行已删→内容底沿 104）/ 代理卡 202~254（内容 150），空白 50~100px | 视觉 |
| 8 | `home_module.cpp:222` | 顶栏标题列+actions=contentW−28，与下方满宽卡右缘差 28px | 视觉 |
| 9 | `markdown_module.cpp:327` | 顶栏三段和=contentW−4，右缘比 `md.body` 内缩 4px | 视觉 |

存疑未定 5 条（未取证，暂不修）：scrollView 贴左框线是否刻意 / `EUI-NEO/core/layout.h:565` row 的 cross/main offset 疑似轴互换 / 预览 CodeFence 高度按字节估算偏差 / 130% 字号档 dialog lineHeight 相撞 / 预览逐块高度恒等未实测。

## 排队

| # | 任务 | 依赖 | 备注 |
|---|------|------|------|
| 6 | 托盘窗高修复 | 依赖 #3 报告 | 单点改动 + 定向测试 |
| 7 | 二级目录实现（两层 + 空目录可见） | 依赖基座提交（worktree 并行） | 验收：新建嵌套目录可见 + 树按层级缩进 + 新增用例绿 |
| 8 | settings 三处改动 | 依赖基座提交 | ① 起代理自动接管 `settings.json`：写 `BASE_URL=http://127.0.0.1:8888` + 占位 key + 模型 env，停代理还原；② `claudeProxyAutoStart` 改回 **false**（启动默认不起代理，设置页「随应用启动代理」开关控制）；③ UI 文案恢复「启动代理将接管…」 |
| 9 | 提交 + 合 master | 依赖 #1/#2 落地 + #5 审计 ACCEPT | 去鉴权 + 字体回退 + 接管默认关 + 二级目录 + UI 修复，分 commit |
| 10 | Phase 3 实施 | 依赖 spec 评审 | 2-3 周大项 |
| 11 | 测试脆弱点（`md_preview_highlight:310` / `md_highlight:622` 的 `<8ms` 墙钟断言、`test_claude_ui:1347` 固定端口 28888） | 待用户点头 | 阈值放宽或端口改 0（原 #8） |
| 12 | D 全量重构 | 排最后 | 架构级，单独 spec |

## 后续大项（未开，需单独 spec）

| # | 任务 | 规模 | 备注 |
|---|------|------|------|
| 13 | Markdown Phase 3 块级实时渲染 | 2-3 周 | 需 brainstorming → spec |
| 14 | D 全量重构（OOP/设计模式，逐模块增量） | 架构级 | 排最后，单独 spec，禁大爆炸 |
| 15 | EUI-NEO 字体改动回上游 + 提交 | 小 | 本次改的 `resolveDefaultUiFontPath` 在 EUI-NEO 仓库，需在其仓库 commit |
| 16 | EUI-NEO push origin | — | **阻塞（非项目内可解）**：本机无 gh/SSH/credential helper（且 DevDesk 仓库无 remote）；只能用户 Web UI 或提供 PAT |
| 17 | macOS 全量回归 | — | **阻塞（非项目内可解）**：本机无 Mac 环境 |
| 18 | 终验人工点验清单 | — | **阻塞（非项目内可解）**：用户侧执行 |

## 已定决策（本轮）

- **代理**：只留 cc-switch(15721)；DevDesk 接管 `settings.json` **默认关**（`setSettingsTakeOverEnabled`，默认 false），代理仅转发不改写配置
- **代理鉴权**：**彻底移除**（无 token、无 401、仅监听 127.0.0.1）
- **字体**：系统字体优先（Win 微软雅黑 / macOS 苹方），打包装饰体降兜底；改在 EUI-NEO `core/render/text.cpp`
- **二级目录**：只做两层；空目录要显示
- **提交**：提交并合 master
- **工作方式**：主会话不直接改文件（写入全派子任务）；实现类任务在**基座提交后**走 worktree 并行（各自独立 build 目录，`-DEUI_NEO_DIR=D:/project/gui/EUI-NEO`），产物由主会话审计后合回

## 追加（2026-09-27 下午·代理攻坚）

**8888 不通的四层根因（逐层实测取证）**

| 层 | 根因 | 处置 |
|----|------|------|
| 1 | 本地代理带 auth token，客户端要 Bearer | 已**彻底移除**鉴权（仅监听 127.0.0.1） |
| 2 | 上游域名 `codingxrui.geely-test.com` 不解析（公司网域名，当前不在该网） | 用户已把 profile 换成 `opencode.ai/zen/go` |
| 3 | 静态 libcurl 无可用 CA 路径 → `Problem with the SSL CA cert` | 修：`http_client.cpp` 用 `CURLSSLOPT_NATIVE_CA`（Windows 证书库） |
| 4 | **客户端 UA 被透传 → Cloudflare 1010 browser_signature_banned**（实测 `Python-urllib` 403 / `claude-cli` 200）；缺 `x-opencode-session` → 400；模型名带 `[1M]` → 400（`Model is unavailable`），裸名 `deepseek-v4.1-flash` → 200 | 派 agent 做 `upstream_dialect` 抽象层（UA/会话头/模型名归一/鉴权头收敛一处） |

另修：`http_client.cpp` 空值头不发送（代理用空 Host 表示"让 libcurl 填"，真发出去是畸形请求 → CF 400）。

**settings.json 写入面**：接管默认关 + 启动补还原也关 → app 启动不碰该文件（指纹前后一致已验证）。
**待做（等方言 agent 验收通过后）**：① `settingsTakeOverEnabled` 默认改回开（起代理→写 `BASE_URL=127.0.0.1:8888`，停→还原）② `claudeProxyAutoStart` 改回 false ③ UI 文案恢复"启动代理将接管…"。

**agent 阵亡与重派**：6 个子任务因网络/API（ENOTFOUND）中途死亡，产物为零 → 已重派无构建类（托盘菜单根因、Phase3 spec 草稿）；需构建类（布局 9 条修复、二级目录实现）排队等 build 窗口。
