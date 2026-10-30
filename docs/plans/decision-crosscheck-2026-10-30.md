# 决策/状态文档 vs 代码现状 矛盾清单（2026-10-30 只读审计）

**总判定**：模块口径类矛盾最严重——CLAUDE.md/README/USER_GUIDE 仍写「8 个 Router 页面」，代码已是 14 行 kModuleRegistry（多出 定时/记忆/MCP/上线组）；战略层「MCP 全协议不做」已被 module_mcp 完整实现突破但路线图未更新；另有桌宠守卫描述、EUI-NEO 能力表过时、单 exe 残留等 9 条。ADR 0006/0007 与 submodule 现状一致（含修订注记），无矛盾。TODO.md 日期新鲜。

## 矛盾清单（按严重度）

1. **页面数 8 vs 14**
   - CLAUDE.md:15「kModuleRegistry 现有 **8 个 Router 页面**（首页/笔记/待办/Claude/智能体/壁纸/设置/关于）」；CLAUDE.md:111 架构图同错
   - 代码：src/app_registry.h:47-63 实为 **14 行**（+定时/记忆/MCP/上线组 3 行）
   - 修法：改为 14 并补全页面清单与插拔集合。
2. **可插拔模块 5 vs 9**
   - CLAUDE.md:15「可插拔的有 笔记/待办/Claude/智能体/壁纸 五者」
   - 代码：app_registry.h:49-61 gatingKey 非空者 **9 个**（+定时/记忆/MCP/上线）
   - 修法：同步为 9。
3. **「src/ 下有 8 个 module_* 目录」**
   - CLAUDE.md:15 同句；实际 `ls -d src/module_*/` = **12 个**（+schedule/memory/mcp/release）
   - 修法：改为 12。
4. **README/USER_GUIDE 页面数同错**
   - README.md:206「八个模块全功能运行（…五项可插拔）」；docs/USER_GUIDE.md:9「8 个 Router 页面…五项插拔」
   - 代码：同 #1/#2（14/9）
   - 修法：随 #1 批量同步。
5. **路线图「MCP 全协议不做」已被代码突破**
   - docs/strategy/05-技术路线图.md:166「MCP 全协议（自己实现完整的 MCP 协议栈）｜不做」
   - 代码：src/module_mcp/mcp_protocol.cpp + mcp_http_handler.cpp（自建 MCP server）+ src/core/mcp_tool.cpp；docs/superpowers/specs/2026-10-29-三池mcp决策文档.md 已立项
   - 修法：按维护规则走 ADR 更新路线图边界（031 项裁决记录）。
6. **工具数文档互斥**
   - README.md:51「8 个内置工具」 vs docs/strategy/05:13「约 21 个内置动作能力」
   - 代码：工具注册散布 agent/mcp/markdown/todo/claude_proxy/memory 六模块（如 src/module_mcp/mcp_tools_builtin.cpp 含 web_fetch_markdown 等，已超两文档口径）
   - 修法：以代码实测数为准改 README，05 标日期戳。
7. **TIN-01 守卫描述含已删除的桌宠断言**
   - CLAUDE.md「模块隔离静态守卫（TIN-01）」段：「断言 module_pet_bridge.h 恰好 include 一次且四条桥函数都真被调用」
   - 代码：scripts/check-module-isolation.sh:12「module_pet_bridge.h 已于 2026-10-13 随桌宠整模块摘除一并删除，本组断言随之移除」
   - 修法：删 CLAUDE.md 中 pet 桥相关断言描述。
8. **EUI-NEO 能力表过时（40 天未核对）**
   - docs/EUI-NEO-CAPABILITIES.md:43「托盘菜单只有 Show/Exit…自定义菜单项**不可行**」；CLAUDE.md「EUI-NEO 真实能力盘点」表同列
   - 代码：EUI-NEO/include/eui/dsl_app.h:190 `onTrayMenu()` 已存在；CLAUDE.md:常用命令段自列 styledMenu/onTrayMenu 新 API——自相矛盾
   - 修法：更新能力表 ❌ 项并按 30 天纪律补核对日期戳。
9. **「单 exe」残留**
   - docs/USER_GUIDE.md:9「C++ 原生应用，单 exe」 vs CLAUDE.md 发布形态段「不强制单 exe…已不再作为约束」
   - 修法：删「单 exe」表述。
10. **测试总数快照过期（临界）**
    - README.md:207 / docs/USER_GUIDE.md:3「Total 1259（2026-10-03）」——今天实际 1954（TODO.md:18，2026-10-29 全量）
    - 修法：刷新记录日期或删数字只留口径。
11. **战略四清单缺新模块（低）**
    - docs/strategy/00-总览索引.md 功能清单 grep「壁纸/记忆/上线/定时」零命中；代码已各有完整模块
    - 修法：01/00 清单补登记或注明派生 spec 位置。

## 已核无矛盾
ADR 0006（submodule 修订注记与 .gitmodules/EUI-NEO/ 现状一致）、ADR 0007（fork-and-freeze）、TODO.md（2026-10-29 改版，日期新鲜）、specs 2026-10-26~29 批次（≤30 天）。
