# need244 残余清单：审计 P2×11 + P3×1（2026-11-09 复核 cffffb72→0c302d99）

> 来源：code-reviewer 对 #244 的 M 级抽查（evidence/2026-11-08-need244/ 同源证据）。
> P1×5 已随 0c302d99 修复；本清单为 P2/P3 残余，随 #244 验收时裁决：逐条修 or 挂池 or 明确不做。

## P2（挂账待裁决）

| # | 位置 | 内容 | 备注 |
|---|---|---|---|
| 1 | claude_mcp_config.cpp:40-44,155 | 备份 .bak-devdesk 每次覆盖：卸载时的备份实为「已装版」非安装前干净副本；软链目标会被 rename 换成普通文件 | dotfiles 用户注意 |
| 2 | claude_mcp_config.cpp:112-118 | 无 mcpServers 容器的文件装→卸后残留 `"mcpServers": {}`，非字节级往返守恒 | 语义等价，观感问题 |
| 3 | mcp_page_module.cpp:664-670,1069 | 详情弹窗跨侧栏切页仍存活（同实例）：期间改端口/外部改文件 → 回到页看到旧快照 | 需 focus/帧级轻量刷新 |
| 4 | mcp_page_module.cpp:669,677-683 | 已装判定只看键存在，不校验 URL=当前 live 端口：改端口后钮仍「卸载」，死端口失配不可见 | 可发现性问题 |
| 5 | mcp_page_module.cpp:989-1000 | hint 槽 208px Ellipsis：含完整路径的 errorMessage 必截断；toggle 失败无 LOG_* 留痕（同文件 :1043 有先例） | 加日志低成本 |
| 6 | paths_{macos,posix,win}.cpp | 经 claudeConfigDirOverride().parent_path() 反推耦合；且未识别真实 Claude Code 的 CLAUDE_CONFIG_DIR 环境变量（env_var_hints.h:283 自有先例）→ 多账号用户装了不生效 | 需先调研该变量语义再动 |
| 7 | tests/CMakeLists.txt:707 | 场景注释与断言面不符（已随 0c302d99 修正注释——本条已消化） | ✅ 已消化 |
| 8 | mcp_page_module.cpp:680 | 行号注释腐烂（已随 0c302d99 修正——本条已消化） | ✅ 已消化 |
| 9 | tests/test_core/test_paths.cpp | claudeConfigFile() 无直接单测：override 分支与 fallback 文件名三平台无锁 | 顺带补 |
| 10 | test_mcp_claude_config.cpp / test_mcp_page.cpp | 真文件「副本安装/卸载」逻辑两份重复（smell）；合并可 -40 行 | Lean 建议 |
| 11 | test_mcp_claude_config.cpp:36-46 | parseObj 解析失败归一空对象 / serversJson 失败返回 ""：失败现场伪装成 null 比对（现实现 kSampleJson 恒可解析，无假绿） | 低危 |

## P3

| # | 位置 | 内容 |
|---|---|---|
| 1 | claude_mcp_config.h:19 | 命名空间别名 `cc` 语义过短，可改全名或更明确别名 |

## 审计新查证事实（下游免重查）

- 真实 ~/.claude.json：107451B / 51 顶层键 / 无 uint64 溢出 / 两空格缩进无 BOM——全量 stringify 在本文件无数值失真
- 弹窗恒 600 宽：最小窗 1000 逻辑宽（main.cpp:781）− overlay 48 → 952 ≥ 600
- 设置页改端口走 applyPort 自动 restart（settings_module.cpp:609-620）；UI 无「已装 URL 跟随」
- ui_driver 断言能力仅 exists/missing/text/geometry（driver_schema.h:495-499），无 disabled 断言
- 模块关闭走 dispatchModuleToggle onStop + 注销/重建（app_assembly.cpp:337-352）；陈旧态只出现在「同实例未重开详情」窗口
