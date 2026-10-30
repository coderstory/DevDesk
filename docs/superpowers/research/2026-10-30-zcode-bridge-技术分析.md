# zcode bridge 两需求技术分析（需-14 接入标准化 / 需-15 fork 自修）

执行：老学究 ｜ 与惨品业务分析（`2026-10-30-zcode-bridge-两需求业务分析.md`，81adee19）合成决策稿 ｜ 只调研未动码

## 实查环境

- fork 源码：`~/dev/dsh-zcode-plan-bridge`（tangyixiao fork，v0.5.x，HEAD 2026-10-27 PR#4 合并后）
- 上游：`kyle123740/dsh-zcode-cli-proxy`（AGPL-3.0）；fork README 自述双内核耦合（ZCode CLI 3.16.3 + DSH 0.2.1-rc.3），每次任一内核升级都要跟通道层
- 存量补丁：`~/dev/bridge-fix-abort-reasoning.patch`（84 行，未应用）

## 需-15：三个病灶逐一实锤（fork 现源 file:line）

| 病灶 | 现源证据 | 判 |
|---|---|---|
| ① stop 懒检查 | `lib/app-server.js:1397-1400`——注释自述「stop is sent from finally for every path」+ 循环内才查 `options.signal?.aborted`；DSH 中止后不再拉流，生成器 finally 永不执行 → session/stop 发不出，回合锁占住、服务端跑完整轮烧额度 | **实锤**（无人值守额度安全风险，最高优先） |
| ② reasoning 跨 delta 过滤漏 | `lib/app-server.js:1391-1393`——reasoning-delta 对**每个 item** 单独 `sanitizeLeakedCalls(item.text)`，无尾缓冲；伪标签（如 `<antml:` 切在两个 delta 之间）两边都匹配不到即漏。text 通道有 `tail` 尾缓冲（:1370-1373），reasoning 没有——同一套标准两套实现 | **实锤** |
| ③ 收尾 drain 与流中不一致 | `lib/app-server.js:1429-1431`——block-end 的 `finalText` 单独走一次净化，与流中 emitText 出口不同源；文本被扣住/切断时收尾路径行为不一致 | **实锤** |

**补丁适用性**：`git apply --check` **通过**（0 冲突，patch 上下文与现源逐行吻合）——patch 与 fork HEAD 同代，直接可用。patch 修法：abort 事件监听即时补发 stop（①）+ reasoning 与 text 共用尾缓冲过滤出口（②③ 一并归一）。

## fork 自修 vs 等 upstream 成本对比

| 维度 | fork 自修 | 等 upstream |
|---|---|---|
| 修码成本 | 补丁现成且可干净应用，剩验收：真会话回归 3 场景（中止补 stop / 标签切 delta / 收尾一致性），**半天级** | 不可控——上游是外部仓（kyle123740），双内核耦合重写是我们 fork 的特有层，上游未必有同病同修 |
| 后续税 | fork 本就自扛双内核同步税（README 自述），+84 行不显著加重 | 无 |
| 许可 | AGPL-3.0：fork 修改须保持开源——公司内部使用无碍，若对外发布须开源 | — |
| 风险 | 流式管线手改，每条必须真会话回归（mock 不算数，照 CLAUDE.md 智能体测试两段制） | 拖着不修=无人值守场景持续烧额度 |

**结论**：**fork 自修，半天级**。分级按惨品业务稿：① stop 懒检查必修自扛（额度安全）；②③ 随 patch 一并带上（同一出口归一，拆开反而是两次动管线），验收三场景真会话回归。

## 需-14：接入标准化（技术口径）

- 素材已就位：小萝莉 `zcode-dsh-setup` skill 已带安装/登录/两层 patch 配置/验证全套踩坑速查；本仓 README 已有双内核版本矩阵表（CLI/DSH 各自实测版本+声明范围+共存坑 3.14.x）。
- 产出形态：S 级纯文档+skill 合并（`zcode-dispatch` 已是合并后名），**半天级**，与需-15 共享验收环境（bridge 起得来、真会话通）——先做它正好是需-15 的验证前置。
- 唯一技术注意：文档须写死「CLI ≥3.15.3（3.14.x 与新桌面端共存启动即崩）」与预发布 semver 枚举两条坑，都是 README 里实锤过的。

## 建议排产顺序

需-14（半天）→ 需-15 应用 patch+三场景真会话回归（半天）→ 两单各 commit+push。合计一天级，零新依赖。
