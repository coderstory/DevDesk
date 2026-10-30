# docs/ 文档时效审计报告（2026-10-30 夜班批量）

执行：老学究 ｜ 依据：CLAUDE.md 时效约定（「当前状态」类段落必须带日期戳；时点性快照 >30 天未核对移入 `docs/archive/`；本审计基准日 2026-10-30，即最后核对 < 2026-09-30 者为过期待处置）。
方法：全 docs/ 341 个 md 逐文件取 git 最后提交日期（`/tmp/docs-dates.txt` 全量表），对「当前状态/能力盘点」类陈述抽样对照代码现状实查。

## 一、已确认失实陈述（需修，file:line 实证）

| 位置 | 失实陈述 | 实查证据 | 判 |
|---|---|---|---|
| `docs/EUI-NEO-CAPABILITIES.md:43` | 「托盘菜单只有 Show/Exit……不能加自定义菜单项」 | `EUI-NEO/include/eui/app.h:108-112` 已有 `trayMenuItems()` / `onTrayMenu` 回调（`DslAppConfig::trayMenu` 注册），自定义托盘菜单**已可行** | **失实**（该文档调研基准 2026-09-20，最后补充 09-27，其后 EUI-NEO 新增了 trayMenu/onTrayMenu 等 API，文档未跟上） |
| `docs/EUI-NEO-CAPABILITIES.md:355-366`（§系统托盘） | 「想加自定义托盘菜单项**不可行**——必须用窗口 UI 替代」 | 同上 | **失实** |
| `docs/EUI-NEO-CAPABILITIES.md:628` | 速查表「系统托盘 ⚠️ 部分支持（菜单只有 Show/Exit）」 | 同上 | **失实** |
| `CLAUDE.md`「EUI-NEO 真实能力盘点」表（非 docs/，顺带报） | 同样的「自定义托盘菜单 ❌」行 | 同上 | **失实**（CLAUDE.md 自己后文「当前状态」段已列 trayMenu/onTrayMenu 为新 API，同文件内自相矛盾） |

## 二、待验证（疑似过期，需深核再定性）

| 位置 | 陈述 | 现状 |
|---|---|---|
| `docs/EUI-NEO-CAPABILITIES.md:42/202/309` | `fontWeight()` runtime 不切 face（只打包 Bold） | `EUI-NEO/core/render/text_types.h:62`、`runtime_instances.h:85` 均有 `fontWeight` 字段，是否真切换 face 需跑渲染取证，本轮未定性 |
| `docs/EUI-NEO-CAPABILITIES.md` 其余 ❌ 项（文件拖入/drop target/tree 组件/Collapse 等） | 2026-09-20 盘点 | 本轮只抽查托盘/字重/全局热键三项；**整份文档建议按条重审**——它是 CLAUDE.md 指定的 UI 问题第一查询入口，失实代价最高 |
| 全局 OS hotkey（`:36/464`） | 无全局热键 | EUI-NEO include 下未检索到 hotkey 头文件，**该陈述维持成立** |

## 三、归档清单（>30 天未核对的时点性快照）

| 文件 | 最后提交 | 处置建议 |
|---|---|---|
| `docs/EUI-NEO-PR-PLAN.md` | 2026-09-21（39 天） | PR 筹划快照；其中托盘条目已被上游实现部分兑现。建议移 `docs/archive/`（或就地加「2026-10-30 起过期，重审后再用」横幅） |
| `docs/superpowers/specs/2026-09-22~09-29` 批次（23 份） | 09-22~09-29 | **不归档**——spec 是历史设计权威记录（CLAUDE.md「关键文件」表点名引用），非状态快照；仅 09-28 installer/design 等被 CLAUDE.md 引用者须保留顶层 |
| `docs/plans/开发计划-2026-10-25.md` | 10-25（5 天） | 未过期，不动 |
| `docs/strategy/00-13` 全系 | 2026-10-21（9 天） | 未过期（权威底稿另有 00-总览索引管），不动 |
| `docs/USER_GUIDE.md:1` 内嵌「最近一次记录 2026-10-03 测试数」 | 10-03（27 天） | 临界未超 30 天；下次全量跑测试时顺手刷新数字即可 |

顶层其余文档（ARCHITECTURE / ACCESSIBILITY / UI-UX-DESIGN / MACOS-APP-BUNDLE / ccache-setup-guide / RESEARCH-INDEX）最后提交均在 10-18~10-29，全部未过期。

## 四、动作建议（待拍板，本文档只读不动手）

1. `EUI-NEO-CAPABILITIES.md` 托盘三条失实陈述修正 + 文档头补「最后核对 2026-10-30」日期戳；fontWeight 等 ❌ 项列一条「重审票」进池。
2. `EUI-NEO-PR-PLAN.md` 归档或加过期横幅。
3. `CLAUDE.md` 能力盘点表的托盘行同步修正（改 CLAUDE.md 属基线文件，须走守卫流程，单独一票）。
