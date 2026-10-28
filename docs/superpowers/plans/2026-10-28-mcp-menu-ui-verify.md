# MCP 菜单页 UI 终验报告（交付验收闸 · 领导视察视角）

- 日期：2026-10-28
- 验证者：功能实证子代理（独立于实现/tester，本轮无自证）
- 对象：worktree `D:\wt-mcp-skeleton`，HEAD `3fd18737`（chore(mcp): 收官门禁与 B5-04 核销）
- 凭证复核：`build/DevDesk.exe` 208,785,578 B，mtime 2026-10-28 14:08:32，
  sha256 `93ab24743ce71c296c03718c43b73926a4e240b26dd0bf975ac2122fef1841cd`，
  特征串 = 节点 id 模板 `mcp.card.`（1 个模板）+ 17 个唯一 `mcp.dialog.*` 节点 id（与 tester 凭证 mcp.card=1/mcp.dialog=17 吻合）。验证结束后 sha256 复测一致（未被本次验证污染）。
- 方法：专用探针 `mcp_ui_probe`（真 GLFW 窗口 + 真 `app::compose` 侧栏/标题栏/Router + 真指针/键盘注入 + 进程内帧捕获 PNG；装配 = 生产同款：`registerBuiltinMcpTools` + `app::mcp` 桥注入 `globalMcpService` + `dispatchModuleToggle`）。配置隔离走 `ScopedConfigPath` 临时库 + `DEVDESK_DATA_ROOT` 指向临时数据根。服务真监听 8931，探针在三个状态点落 marker 挂起，验证脚本在存活窗口内从**外部进程**真实 curl 取证。
- 证据目录：`%TEMP%\mcp-verify-ui\`（shots/ 截图与步进日志、curl/ 探活原始输出、probe-src/ 探针源与 CMake diff、probe_stdout.log）
- 总结论：**可交付（附 1 个非阻塞视觉瑕疵单）**。场景 6/6 通过；探针两轮各 37 断言全 PASS、0 FAIL、退出码 0。

## 场景明细（每场景：操作 / 预期 / 实际 / 证据 / 结论）

### U1 门控与侧栏 — PASS
- 操作：临时数据根启动（MCP 默认关）→ 观察侧栏 → 设置→模块 tab 真点 `settings.card.modules.mcp.hit` 开关。
- 预期：默认关 = 侧栏无 MCP；开 = 侧栏出现 MCP（fa-plug，work 组尾）+ 服务 8931 监听。
- 实际：两轮一致。默认关：无 `app.sidebar.item.Mcp.hit`、桥 status 未运行；开：条目当帧出现、status running port=8931、持久化 enabledModules 含 mcp。外部 GET `http://127.0.0.1:8931/mcp` → **HTTP/1.1 405 Method Not Allowed（Allow: POST, DELETE）**。
- 证据：shots/u1_sidebar_off.png、u1_sidebar_on_settings.png；curl/u1_get_405.txt。
- 注：fa-plug 字形在 run1 缺失，定性为探针 CWD/exeDir 字体候选链脱靶伪象（FA otf 在 `build/assets`，真实 DevDesk.exe exeDir=build 可命中）；run2 以 build 为 CWD 复跑，fa-plug/fa-house/fa-gear 全部正常渲染。

### U2 菜单页 — PASS
- 操作：真点侧栏 MCP 项 → 菜单页。
- 预期：页头 + 状态行「运行中 · 127.0.0.1:8931」+ 两张工具卡（图标/名称/描述/内置 chip/状态点）+ 计数 2。
- 实际：全部命中。`mcp.title`=「MCP 服务」、`mcp.status.text`=「运行中 · 127.0.0.1:8931」、卡 `devdesk_server_info`（fa-server）/`web_fetch_markdown`（fa-globe）、`mcp.section.count`=2、内置 chip + 状态点在。
- 与 tools/list 一致性：对**同一存活服务实例** POST initialize（200 + Mcp-Session-Id）→ tools/list → name 集合 = `["devdesk_server_info","web_fetch_markdown"]`，与卡片集合一致。
- 证据：shots/u2_mcp_page.png；curl/mcp_initialize.json、mcp_tools_list.json、tools_list_names.txt。

### U3 详情弹窗 + 试一下 — PASS（附瑕疵单，见「发现」）
- 操作：真点 web_fetch_markdown 卡 → 弹窗 → 真点「试一下」。
- 预期：弹窗（hero 图标/名称/chips/描述/schema monospace）+ 后台执行 + 结果区出现且 UI 不冻结。
- 实际：弹窗结构全命中；「试一下」= `execute("{}")`——UI 无参数输入口，schema `required:["url"]` → 结果区 2.78ms 返回红色业务失败文本「失败：\n缺少 url（必须是非空字符串）」（按工具 SSRF 设计，url 必填且拒本机/内网地址，故试一下对该工具即为该形态，如实记录）。等待期主线程持续泵帧（maxFrameMs=2.77ms），无冻结。
- 证据：shots/u3_dialog_webfetch.png、u3_try_result.png；步进日志 U3 elapsed/frames/maxFrameMs 行。

### U4 三路关闭 — PASS
- 操作：关闭钮 / Esc（queueKey 注入，Router 派发与 main.cpp 同链）/ backdrop 左上角真点，各开关一轮。
- 预期：三路均关。实际：三路全关（dialog panel disabled/opacity=0 判据）。
- 证据：shots/u4_closed_btn.png；步进日志 U4a/U4b/U4c PASS 行。

### U5 开关联动 — PASS
- 操作：设置页关 MCP → 观察 + 外部探活 → 再开 → 外部探活。
- 实际：关 = 侧栏 MCP 条目当帧消失 + 服务停；外部 curl **exit 7 连接拒绝**（curl/u5_off_refused.txt）。再开 = 条目恢复 + 服务恢复 8931；外部 GET 复获 **405**（curl/u5_on_405.txt）。
- 证据：shots/u5_sidebar_off.png、u5_sidebar_on.png。

### U6 清理 — PASS
- 探针进程自然退出（probe_exit=0）；无我方启动的残留进程；8931 释放；临时数据根（实测为空目录）已删；`build/testtmp/mcpui_cfg-*` 已删；探针源/CMake 改动已回滚（`git status` 净），build.ninja 已重生成（无 mcp_ui_probe 目标）；`build/DevDesk.exe` sha256 复测一致。探针源与 diff 副本留存于证据目录 probe-src/。
- 备注：发现常驻 DevDesk.exe PID 8384（2026-10-28 15:40:35 起，`D:\project\gui\wt-wp-img\build-release\DevDesk.exe`）属并行 worktree 会话，非本验证启动、未占 8931，按「只杀自己启动的 PID」纪律未动。

## 发现（非阻塞）
1. [minor·视觉] **U3 弹窗 schema 文本溢出**：长 schema（web_fetch_markdown，`mcp.dialog.detail.schema.text` 102px 高）wrap 后溢出 schema 框下界，压盖「运行结果」标题（run1/run2 截图均复现：u3_dialog_webfetch.png 下部）。建议 schema.text 加 clip 或改滚动/加高。功能与可读性不受影响，不构成否决。
2. [environment·已排除] FA 图标字体解析链（`core/render/text_shaping.cpp` exeDir/assets 与 CWD 候选）在非 build 目录 CWD 的探针进程脱靶 → 字形全缺。真实 DevDesk.exe（exeDir=build）不受影响；已用 run2（CWD=build）取得字形正确证据。若 probe 体系在意，可给 probe 目标加 assets 拷贝或 CWD 约定。
3. [observation·越界记录] 1280 宽下设置「模块」卡横向溢出，标题右 MCP 开关被窗口右缘裁切（DOM 坐标可点、功能正常，横向滚动可达）；另在 settings 为当前页的截帧中观察到侧栏「首页」行与动态背景装饰瞬时重叠（首页图标/首字被盖，MCP 页稳态帧正常）。两者均非 MCP 范围，建议另行核查。

## 未验证
- mcp_live --serve 独立进程对比：未另起（改用对同一存活服务实例直接 JSON-RPC tools/list 对比，口径更强）；mcp_live --check 的 405/415 断言属既有测试门禁，未重复执行。
- 真实外网抓取（web_fetch_markdown 带合法 url 的成功路径）：UI 无参数输入口（execute("{}")），无法从 UI 达成；成功路径由 mcp_live/单测覆盖，本闸如实记录失败文本形态。
- /Applications/DevDesk.app 等安装产物：不在本平台范围。

## 证据清单
- 截图（run2 = 字形正确轮）：u1_sidebar_off / u1_sidebar_on_settings / u2_mcp_page / u3_dialog_webfetch / u3_try_result / u4_closed_btn / u5_sidebar_off / u5_sidebar_on（.png，1280x800）
- 日志：shots/mcp_ui_probe_steps.log（两轮 37 PASS/0 FAIL）、probe_stdout.log（run1/run2）、curl/*.txt|json
- 探针副本：probe-src/mcp_ui_probe.cpp、probe-src/CMakeLists.mcp_ui_probe.diff
