# 01: 开发纪律包（gsd/superpowers 形态的流程基建）立项评估

**What to build:** 把本项目已成型但停留在口头/记忆层的开发纪律打包成机器资产：①六阶段流程技能集（实现→独立审计→修→测试→合并→真机，补 cs-pdca-auditor / prd-audit 未覆盖的缺口，不重复造 superpowers 已有的）；②强制 hooks（无 plan 不许写码、merge 后必跑合并树门禁、追加消息对账提醒、commit 前先红后绿检查）；③spec / plan / 审计 / 决策卡四套模板；④同文件多轮改动的累积核对脚本。形态 = Claude Code 插件/技能包，先服务 rs-host 与 DevDesk 两条开发线，将来经 C 期技能导入器被智能中枢复用（E 期 PM/QA/Dev 角色的雏形即打包的流程技能——与 08 号报告"技能是角色种子"结论咬合）。

**动机（2026-10-21 会话结论）:** AI 走预定流程靠四层机制（注入/强制/对账/结构）；现状注入层与结构层已齐，对账层与强制层最薄——本票即补后两层，把"保证 AI 走流程"从概率约定变成机器强制。

**Blocked by:** 无（可与 A 期并行，最小件先行）

**Status:** in-progress——最小件已落地 2026-10-21（核对脚本 scripts/check-cumulative-markers.sh + 常备清单 docs/strategy/cumulative-markers.txt + 项目级 hooks .claude/settings.json 四条：基线守卫(唯一可拦)/合并树门禁/先红后绿/plan 先行），对新会话生效；完整技能包（模板×4 + 六阶段技能集）待与 C 期合流开题

- [x] 用户拍板：批准最小件先行（2026-10-21），hooks 落项目级（用户原话「hooks写本项目里」）；完整技能包待 A 期/C 期排期再拍
- [ ] 盘点既有资产缺口清单（x-coding-tools / superpowers / GSD 已覆盖 vs 缺）
- [x] 最小件（hooks×4 + 核对脚本）已落地并验收：管道实测两条路径（守卫拦截 exit=2 列缺失 / 放行 exit=0 静默）+ 三提醒模式 JSON 合法 + jq 校验 settings.json 结构 + 累积清单回归 PASS。已知限制（v1 接受）：hooks 需会话重启或 /hooks 刷新才装载（settings watcher 只盯会话启动时已存在的 settings 文件）；if 前缀匹配盖不住复合命令（如 cd X && git merge）
