# devflow 审计④ 文件级实效 — code-reviewer 面

- 日期 2026-11-02 ｜ 条目 `[pool:need:78]` ｜ 对象 `/Users/coderstory/dev/zcode-plugins/devflow` 全部非 skill 文件 + 仓根 `marketplace.json`（源码 HEAD `9f2ee1b` 0.33.0）
- 方式：只读。静态守卫（`bash -n`/`sh -n` ×5、`node --check` ×2、`ast.parse` ×5、JSON 解析 ×3 全过）、样例 JSON 喂 gate / session-start / auto-continue.mjs、E4 锚点全量复核、`zcode plugins validate --source`、缓存 vs 源码 `diff -rq`。仓内零改动（`git status` 仅既有脏文件 `docs/eval-log.md`，非本次产生）。
- 尺子：`docs/research/2026-11-01-skill-quality-baseline.md` A1/A2/A4/A5/A6.1-6.6/C.6/C.9/D.3/D.6/E.1；官方 diagnosing-hooks / -commands / -plugins 0.3.0。

## 简报前提勘误（先于五轴）

简报「已验证事实」称**已安装缓存为 0.17.0**——实况不符：`~/.zcode/cli/plugins/cache/dev-zcode-plugins/devflow/` 下存在 `0.33.0/`，`installed_plugins.json` 记录 `"version": "0.33.0", "installPath": ".../devflow/0.33.0"`，本会话技能表路径亦为 `.../devflow/0.33.0/skills/...`；`diff -rq CACHE/0.33.0 SRC -x docs -x evidence -x __pycache__ -x .DS_Store` **零差异**。结论：**没有「尚未对用户生效」的改动**，源码即生效版本。请主会话更新该背景事实（缓存目录另有 0.1.0→0.32.0 共 41 个旧版本目录残留，属宿主清理范畴，不计入本面）。

---

## ① 清单声明 vs 磁盘实况（A1 / A2）

硬断言全过：`agents`→`./agents`（7 文件）、`skills`→`./skills`（11 目录 11 SKILL.md，扁平 A3.1）、`mcpServers.pool/monitor` 两条 args 文件均在，全部相对路径、未逃逸插件根（A1.4）；manifest 无 `hooks` 键（A1.5）；`commands/` 未声明但为标准位置、已被宿主发现（简报已验证）；三处版本 `plugin.json / marketplace.json / pool_mcp.py:959 serverInfo` 均 `0.33.0`（A1.3/A2.4）；marketplace `source: "./devflow"` 相对（A2.2）、展示元数据（displayName/_i18n/description_i18n/category）留在市场条目而非 manifest（A2.3）；`zcode plugins validate --source /Users/coderstory/dev/zcode-plugins` 输出 `Plugin validation completed.` 无错误行（A2.6）。描述计数「7 角色 / 11 skill（5+1+1+1+3）」与磁盘一致。

- [P2] `.zcode-plugin/plugin.json:4` ＆ `README.md:82` — 组件计数口径不一：manifest 描述只提「子任务白名单 hook」（单数），README 写「4 hook」，实况 `hooks.json` 4 事件 / 5 条 hook / 3 个脚本 — 证据 `python3 -c ... print(events, hook-entries)` → `events= 4 hook-entries= 5`；README 唯一命中 `[0-9] hook` 为 `82:4 hook` — 建议统一为「4 事件 hook（门禁 / 注入 / 自动续跑 / 监控拉起）」，并在 E1 加计数锚。
- [P2] `.zcode-plugin/plugin.json:4` — description 为 ~300 字符单句运行说明，非官方范式的能力动词列举（A1.8，E1-E4 对照）；市场卡片展示面会被折叠 — 建议 ≤120 字符动词列举，长说明下放 README。
- [P2] `marketplace.json:8` vs `:14` — `description` 与 `description_i18n.zh-CN` 是**两段不同的中文**文案（后者多出「卡片链协作、证据物实证…」一句），中文用户在不同界面看到不一致 — 建议 zh-CN 变体 = description 原文，或删 zh-CN 键（主文案已是中文）。

## ② hooks 实效（A5 全组 / C.6 / D.6）

硬断言全过：4 事件均在宿主 7 事件集内（A5.1）；`PreToolUse` matcher `Agent|Task` 大小写正确且覆盖别名（A5.2）；command 型字段集 {command,timeoutMs,statusMessage}、process 型 {command,args,timeoutMs,statusMessage} 各归各无混用（A5.3）；全部用 `timeoutMs` 毫秒（A5.4，10000/10000/5000/120000/10000）；模板变量仅 `${ZCODE_PLUGIN_ROOT}`（A5.5）；脚本全部经解释器 `bash`/`python3`/`node` 调用、未依赖 `async`（A5.10）；gate 退出码 2=阻断（A5.6）；README:9/70 写明「开新会话激活」（A5.8）。实跑：`bash hooks/session-start.sh | python3 json.load` → 单键 `additionalContext`、265 字符、门禁自检通过（warn 未触发）；`Stop` 样例 `{"responseText":"当前系统繁忙，请稍后再试。"}` → `{"decision":"block","reason":…}` rc 0。

- [P1] `hooks/hooks.json:18` — `SessionStart` matcher `"startup|resume"` 未覆盖宿主另两个匹配值 `clear` / `compact`：用户 `/clear` 后上下文清空、devflow 激活纪律**不再注入**（本会话实际发生过 `/clear`），压缩后注入文本也可能被摘要掉而不补发；门禁自检同样不重跑 — 证据 diagnosing-hooks §2「`SessionStart` → one of `startup`, `resume`, `clear`, `compact`」；且 `tests/eval-run.sh:140` 把字面值 `"startup|resume"` 钉成断言，修复时需同改 — 建议 matcher 改 `startup|resume|clear|compact`（monitor.py `start` 有 flock 单例保护，重复拉起无副作用），E7 断言改为「含 clear」。
- [P1] `README.md:65` — 「临时停用 → 项目 `.zcode/config.json` 置 `hooks.enabled: false`」无证据支撑且与手册口径相悖：`hooks.enabled` 治理的是**配置文件钩子**（默认关、需 true 才跑），而「When any plugin contributes a hook, the hook runner is enabled automatically」——插件钩子不受该开关管辖的概率高；用户照做后门禁仍在却以为已停用（D.6「文件存在≠功能可用」） — 证据 diagnosing-hooks §1 两段原文 — 建议改为唯一有据路径「Settings → Plugin Management 停用 devflow」（diagnosing-plugins §3），或先实测 `enabled:false` 对插件钩子的效果再保留。
- [P2] `hooks/auto-continue.mjs:17` vs `:118` — 头注释写数据目录缺省 `~/.zcode/auto-continue/`，代码为 `~/.zcode/auto-resume`（且 monitor.py:21-22 / server.mjs 均硬编码 `auto-resume`，代码才是对的） — 建议改注释（C.9 单一真相源）。
- [P2] `hooks/auto-continue.mjs:189` ＆ `:41` — `stopHookActive` 解析后全文未使用（死字段）；`maxRetries: 8` 与手册「`Stop` may request continuation (up to three times)」冲突，第 4 次起宿主侧即不续跑，配置承诺 8 次为虚 — 证据 `grep -n stopHookActive` 仅 14/189 两处；diagnosing-hooks §2 末句 — 建议实测宿主上限后把 `maxRetries` 缺省对齐（≤3）或在注释写明受宿主上限截断。
- [P2] `hooks/auto-continue.mjs:585-613` — 手动 CLI `runCli` 不认 `help/-h/start/stop`（`runCommand` 认），落空后 `main()` 继续读 stdin 进 hook 模式：Bash 工具下静默无输出 rc 0，TTY 下 `readFileSync(0)` 会挂起 — 证据 `node auto-continue.mjs help </dev/null` → stdout 空、rc 0 — 建议 `runCli` default 分支打印 usage 并 `return true`。
- [P2] `README.md:71` ＆ `:62` — ①「新会话应出现 statusMessage『注入会话激活纪律』」把「hook 运行了」当「注入成功」：JSON 校验失败时 statusMessage 照样出现而效果被丢弃（diagnosing-hooks 陷阱 8）；②「工作区级 hooks 首次需信任确认（pending_trust）」与手册「Non-plugin (user or workspace) configuration hooks have no trust gate」冲突，属过期表述 — 建议 ① 保留行为判据（说「加个功能」是否走 ask），删 statusMessage 判据；② 删 pending_trust 句。

## ③ gates 逻辑 ↔ tests/gate-test.sh；eval-run.sh 锚点（E.1 / D.3）

`gates/agent-gate.sh` 5 条分支（7 内置放行 / 自定义角色查白名单 / 无字段阻断 / 有池标记放行 / 缺池标记阻断）中 gate-test.sh 10 例覆盖 4.5 条；**E4 全部 80 条锚点在目标文件中逐条命中，零漂移（无假绿）**；E8 README 锚点全在；E1 三处版本一致。

- [P2] `tests/gate-test.sh:8-22` — 两条真实分支无用例：(a) 自定义白名单角色**无池标记**（gate.sh:23-27 对其同样生效）；(b) `"subagent_type":""` 空值（gate.sh:15 `[ -z "$t" ]`） — 证据实喂 `{"subagent_type":"my-role","prompt":"无池标记"}` rc=2、`{"subagent_type":"","prompt":"[pool:need:1]"}` rc=2，行为正确但无回归保护 — 建议各加一例。
- [P2] `gates/agent-gate.sh:24` — 池标记防呆只匹配前缀 `[pool:need:`，**空 id / 非数字 id 均放行** — 证据 `[pool:need:]` rc=0、`[pool:need:abc]` rc=0 — 「防呆不防恶意」已声明，但空 id 正是防呆对象 — 建议 case 模式改 `*"[pool:need:"[0-9]*` / `*"[pool:bug:"[0-9]*`（POSIX sh 原生支持），gate-test 加 `[pool:need:]` 期望 2。
- [P2] `tests/eval-run.sh:96,99` — 锚点 `skills/ask/SKILL.md|环节跳过权` 重复两次（`sort | uniq -d` 命中）；`:142` 仅 `grep -q additionalContext` 不验 JSON 合法性（本次用 `python3 json.load` 才证明合法）；`:140` 钉 matcher 字面值（见 ② P1） — 建议去重、`:142` 改管道进 `python3 -c json.load`。
- [P2] `tests/pool-mcp-acceptance.py:4,9` — 引用 `docs/specs/2026-10-31-pool-mcp-and-efficiency.md`（该文件实在 `docs/archive/`，`docs/specs/` 不存在）；行 9 写死 DevDesk 专属路径 `tools/.pool-mcp-venv/bin/python`（D.3 机器特定残留，E3 只 grep `/Users/coderstory` 与 `--language cpp` 故漏网）；该脚本 30 条 db 级断言不在发版门（README:77/88 只列 eval-run + gate-test），`exit 2` 缺 SDK 语义合理但无人触发 — 建议改路径为 archive、泛化 venv 提示、E3 增 `pool-mcp-venv|docs/specs` 两个反向锚、README 发版自检补一句「有 mcp SDK 环境时跑 acceptance」。
- [P2] `tests/pool-smoke.py:135-139` — JSON-RPC id 23/24/25 与 :118-:139 前文重复使用（同步逐行客户端下无害，异步化即错） — 建议 id 递增。
- [P2] `docs/eval-log.md` — 工作区有 4 行未提交的本机运行记录（`git diff --stat` 1 file +4，尾两行 `2026-11-01 0.32.0 PASS / 0.33.0 PASS`），eval-run.sh:187 的设计即每跑必写，与「检查点提交」纪律摩擦 — 建议随发版 commit 一并提交，或把台账写入 `evidence/` 外置。

## ④ commands/auto-continue.md（A4 / A1.9 / C.9）

硬断言全过：文件名 `auto-continue` 合规（A4.1）、frontmatter 仅 `description` + `argument-hint` 两键均在 6 键白名单（A4.2）、`$ARGUMENTS` 精确 token（A4.4）、无内联动态 shell（A4.5）、宿主已发现。`AUTOCONTINUE_DIRECT:` 前缀与 `auto-continue.mjs:374 COMMAND_PREFIXES` ��致。

- [P0] `commands/auto-continue.md:6-11` ＋ `hooks/auto-continue.mjs:474-482,453-464` — **`/auto-continue msg <文本>` 把整段兜底文案写进自定义继续消息**：宿主把命令正文全文展开为 prompt，`isCommandPrompt` 截掉前缀后保留了第 8-11 行兜底文字，`runCommand('msg')` 用 `rest.join(' ')` 全收；随后每次 Stop 自动续跑都把「请改用 Bash 工具执行 `node "<devflow 插件 cache 目录>/hooks/auto-continue.mjs" …`」当作继续指令喂给模型——**续跑时模型会被指示去猜路径跑 shell 命令** — 证据（按 .md 原文替换 `$ARGUMENTS` 后喂 UserPromptSubmit）：stdout `{"continue":false,"reason":"自定义继续消息已设置为「注意网络 （若你能看到这段文字，说明 auto-continue 插件的直连钩子未生效。请改用 Bash 工具执行： `node \"<devflow 插件 cache 目录>/hooks/auto-continue.mjs\" msg 注…`，`state.json.customMessage` 长 217 字符；紧接 Stop 样例返回 `{"decision":"block","reason":"注意网络 （若你能看到这段文字…"}` — 建议 `isCommandPrompt` 只取首行（`trimmed.split(/\r?\n/)[0]`）再截前缀；gate-test/E7 加一例「msg 后 customMessage 不含『若你能看到』」。`argument-hint` 公开承诺了 `msg <文本>`，此为已发布功能缺陷。
- [P1] `commands/auto-continue.md:9-10` — 兜底文案让 AI **猜测**插件缓存目录（「通常位于 `~/.zcode/cli/plugins/cache/` 下（devflow 插件的版本目录内）」），违反 A1.9「脚本/文案不得猜 cache 路径」与 Z8「loader 管 token」精神（尺子预判 ◐ 维持；E4 第 76 条锚 `devflow 插件 cache` 还把这句**钉成必须存在**，反向固化了反模式） — 建议改为确定性查找「读 `~/.zcode/cli/plugins/installed_plugins.json` 中 `devflow` 条目的 `installPath`」，并把 E4 该锚改指 `installed_plugins.json`。

## ⑤ monitor/（A6.1-A6.6 / D.6）

产物齐全：`dist/server.mjs`（768,582 B，`node --check` 过）、`dist/panel.html`（680,804 B）、`dist/THIRD_PARTY_NOTICES.txt`、`monitor.py`、`dashboard.html`（7,998 B）。职责分工清晰无冲突：monitor.py = flock 单例守护 + 只读扫宿主 sqlite + `127.0.0.1:8765` 控制 API（`Origin`+`X-ZCode-Control` 双校验）+ 写 `status.json/dashboard.port`；server.mjs = MCP 门面，读 `dashboard.port`/`status.json`、`timeout 5e3` POST 控制接口，**不写 state.json**（auto-continue 独占），无文件级写冲突。`mcpServers.monitor` 无 `timeoutMs`（默认 30000ms）：E7 三行握手冒烟即时返回、768KB 单文件 node 启动毫秒级，**未发现实质风险**，后续若加启动期扫描再补。敏感值零（A6.6）。serverInfo 报 `zcode-auto-resume/0.2.0` 与 ui 资源名已在 README:84-86 如实声明为上游产物不改。

- [P1] `monitor/monitor.py:6,252,284` — 模块顶层 `import fcntl` + `fcntl.flock` + `start_new_session=True` 为 POSIX 专有；Windows 上 `python3 monitor.py start` 在 import 即 `ModuleNotFoundError` → SessionStart process hook **每会话必报错一次**（fail-open 放行，但日志有失败记录、monitor 面在主开发机 Windows 永不存在）；README:66「Windows 未验证」为低估——这是确定性失败，非未验证 — 证据 `grep -n 'import fcntl\|fcntl\.\|start_new_session'` 三处；DevDesk AGENTS.md §三 双平台硬约束、§二主开发在 Windows — 建议 `fcntl` 改延迟导入 + Windows 走 `msvcrt.locking` 或 PID 文件；`start_new_session` 仅 POSIX 传；README 平台句改为「monitor 守护 Windows 不可用（fcntl）」直到修复。
- [P2] `monitor/dist/server.mjs`（`c_="ui://zcode-auto-resume/panel.html"`）＆ `.zcode-plugin/plugin.json` — panel.html 以 MCP `ui://` 资源暴露，但 manifest **无 `ui.surfaces` 声明**（对照 E1 typst-workbench 有），面板是否在宿主可见未验证；同时 monitor.py 又自带 `dashboard.html` 走 :8765 — 同一状态两套 HTML UI — README 未承诺面板故非功能缺口，但 680KB 产物若不可见即每版本死重 — 建议实测 `ui://` 可见性；可见则补 `ui.surfaces`，不可见则从 dist 移除 panel.html 或在 README 写明「仅工具面」。
- [P2] `monitor/monitor.py:133-137,149-151,158-159` — `load_state()` 读的是 auto-continue.mjs 写的同一 `state.json`，但按旧 auto-resume schema（`day` / `sessions[*].attempts`）解读；现 schema 为 `{sessions:{count,repeats},pausedUntil,enabled,customMessage}`，`state.get("day") != day` 恒真 → `attempts_today` / `attempts_today_total` 恒 0，`MAX_ATTEMPTS_PER_DAY` 死常量 — 建议删除该读取与字段，或改读 `sessions[*].count`。
- [P2] `monitor/monitor.py:19-20,45-115` — 直连宿主内部库 `~/.zcode/v2/tasks-index.sqlite` / `~/.zcode/cli/db/db.sqlite` 的表与列（`tasks.cli_last_outcome`、`message.sequence`、`part.data $.state.status`、`session.last_outcome`），无 schema 版本守卫；宿主升级改表即整体 `RuntimeError`/`OperationalError` 进 `monitor.log`，面板显示 error 但无指向 — 建议启动时 `PRAGMA table_info` 自检并在 status.error 写明「宿主 schema 变更」。

---

## 计数

- **P0：1**（④ `/auto-continue msg` 污染自定义继续消息并把「跑 shell」指令注入续跑）
- **P1：4**（② SessionStart 漏 `clear|compact`；② README `hooks.enabled:false` 停用指引无据；④ 兜底文案猜 cache 路径；⑤ monitor.py `fcntl` Windows 确定性失败）
- **P2：13**
- 另：简报「缓存 0.17.0」前提失实（实为 0.33.0、与源码零差异），请主会话更正。
- 未发现项：① 路径/相对/逃逸/版本同步/validate 全过；② 事件名/matcher 大小写/字段混用/超时单位/模板变量/解释器调用全过；③ E4 80 锚点零漂移、E1 版本三处一致；④ 文件名/frontmatter 键/`$ARGUMENTS`/动态 shell 全过；⑤ 产物存在、职责无冲突、`timeoutMs` 风险未发现。

**总判定：需重审**（P0 为已发布命令功能缺陷且副作用进模型续跑指令；P1 四项中 ②-clear/compact 与 ⑤-fcntl 直接影响主开发机实效）。

工具调用：26 次（预算 ≤35，未超）。
