# a98e3f91 脱敏层+红队样本 独立审计（阶段 2 关卡）

- 审计者：独立审计子代理（未参与本提交，非实现者会话延续）
- 对象：`a98e3f91` feat(sanitize)，7 新文件 933 行全为新增
- 环境：D:/wt-memcpp @ feat/memcpp；审计过程零代码改动（变异均为内存 monkeypatch，`git status` 复核 tools/ 干净）
- 自检：Read/Bash(grep 代替 Grep)/Write 齐备，`git log -1` = a98e3f91，否则拒单——已满足

## 六维度逐条结论

### 1. 正确性 — 已核，通过
- 六类正则逐类抽核（core.py:55-122）：phone 11 位结构/分隔位约束、id18 不限年段+校验位、id15、email（加号/多级域/全角@）、ipv4 八位组校验+前后界、url_param 大小写不敏感、vendor key（AKIA/AIza/ghp_/github_pat_/sk-/xox）、Bearer 防普通词、kv 密钥含 `-` 前缀（x-api-key）、PEM 块+裸 BEGIN 脱到尾。
- 实测复跑 `eval_redteam.py`：total=110 pos=92 neg=18 leaks=0 misfires=0/18 → PASS，EXIT=0。
- 6 条正例手工走正则路径全真命中（p15/e12/s05/u03/c07/c06），非"样本配合正则"。
- shadow 映射严格 1:1（core.py:35-50），区间映射回原文合法；幂等（占位符不触发二轮命中）。
- 残余注记（不计 finding）：非全角的 Unicode 数字（如阿拉伯-印度数字）不在 shadow 表、`1[3-9]` 字面量不匹配——README 未声称覆盖，边界一致。
- fail-closed 见维度 4/F4。

### 2. 回归面 — 已核，通过
- `git show a98e3f91 --stat`：仅 7 新文件（sanitize/{core,__init__,README,datasets/redteam.json,eval_redteam} + tests/{test_sanitize,test_redteam_eval}），0 删除。
- experiments/ 冻结资产、eval_retrieval.py、freeze_datasets.py：不在提交内，零触碰。
- 既有测试文件（compress/db/embedding/ingest/llm 共 5 文件 63 条）零改动；全 suite 110 条实跑 OK。

### 3. 平台对称性 — 不适用（纯 Python，无平台分支代码）。已核。

### 4. 静默失败面 — 已核，通过
- eval_redteam.py 全文无 try/except、无 except:pass、无 print 后继续；`ensure_gate`（eval_redteam.py:102-117）五项判定（样本量/泄漏/类别未命中/误杀率/幂等）任一不过必 raise；`main` 仅 gate 通过才 print PASS 返回 0。
- GateRaisesTest 用毒化数据实证 raise 路径（test_redteam_eval.py:85-132）。
- sanitize() 对畸形输入实测：空串/10 万字符/全角混合/emoji/含 NUL 字节——无异常、无静默放行（夹带手机号照常命中），无卡死。
- 见 F4：fail-closed 是"传播式"而非显式机制，缺护栏说明。

### 5. 测试有效性（变异视角）— 已核，通过
内存 monkeypatch 五组变异，全部能翻红（证明断言能杀死变异，非假绿）：
- MUT1 手机号 `\d{4}`→`\d{3}`（历史 bug 复现）：22 条正例泄漏 → eval gate raise，phone 单测红。
- MUT2/MUT4 kv lookbehind 禁 `-` 前缀（x-api-key bug 复现）：s19/s22 泄漏 → 红。
- MUT3 id18 年份段限 19/20（历史 bug 复现）：c02/c10 泄漏 → 红。
- MUT5 `_id18_detail` 恒报 checksum_ok：test_sanitize.py:72 `assertEqual(detail,"id18:checksum_bad")` 实测 FAILED → 红。
- 恒真断言排查：无 `assertIsNotNone` 式假断言；GateTest 各断言对 []/0 精确比较；误杀率断言含字段存在性+阈值双检。
- 数据集 5+ 条正例抽样手工核对正则路径：全部真命中（详见维度 1）。

### 6. 文档口径 — 已核，2 条 finding
- sanitize/README.md "不防"清单（中文姓名/绝对路径/语义级/IPv6/银行卡/护照/逐字对抗）与 core.py 实际能力一致：无反向超配（声称不防却在防）、也无超能力声明。
- allowlist 与声明精确一致：13800000000 + 127.0.0.1/0.0.0.0/255.255.255.255（core.py:31-32）。
- F1 根 README 数字过时（属实）、F2 回单"89 条"失实、F3 preview 契约措辞与实现不符。

## Findings（按 severity 降序）

- **F1 [中] tools/memory-lab/README.md:72** — "单元测试：21 条全过"过时：实测全 suite 110 条（sanitize 前 63 + 新增 47）。问题：根 README 是 memory-lab 总口径，B3 自报未更新，本提交也未更新。影响：后续关卡/新 agent 按此估回归基线规模偏差近 5 倍，测试计数成为连环失真源头。建议：改为"110 条全过（含 sanitize 47 条+红队评测 110 样本）"或去数字化表述改为指向 unittest discover。
  - 处置（阶段 3，已修）：README.md 已实测节改为当前事实——全 suite 113 条全过（既有 63 + 脱敏层 50）+ 红队评测 110 样本泄漏 0/误杀 0；全文核查无其他过时计数。口径说明：审计时点 110=63+47；本阶段修 F3/F4 新增 3 条测试，真实数更新为 113=63+50，README 直接写最新事实，不在文档里留双口径。
- **F2 [中] 回单声称（对应 tests/test_sanitize.py + tests/test_redteam_eval.py）** — "89 条新单测全过"失实：实测新文件单测 35+12=47 条。89 = 全 suite 110 − 过时 README 的 21，把存量 63 条误当 21 条。影响：验收数字虚高、算式建立在 F1 的过时口径上，误导关卡决策。建议：回单口径更正为"新增 47 / 全 suite 110"；测试本体全部实跑通过，非质量问题。
  - 处置（阶段 3，口径更正，不重写历史）：仓库全量 grep 无"89 条"表述（仅历史回单失实，sanitize/README.md 与代码注释均干净）——历史 commit message 按 fix-forward 不重写，以本处置栏与最新回单口径为准：审计时点新增 47（35+12）/全 suite 110；含本阶段 3 条护栏后为新增 50/全 suite 113。
- **F3 [低] tools/memory-lab/sanitize/core.py:127,150-153（对照 README.md:13、__init__.py:7）** — `Hit.preview` 契约"绝不携带原始敏感值原文/不含原文"与实现不符：`_masked_preview` 对 >6 字符值保留首尾各 2 字符（如 "hu****e2"，7 字符密钥泄 4 字符，占比 57%）。影响：hits/preview 一旦进日志或调试输出，短密钥半数以上字符可见；契约措辞会让后来者放心外发 hits。建议：≤10 字符值改全掩码，或文档改口"掩码保留首尾各 2 字符，hits 禁止随外发文本出域"。
  - 处置（阶段 3，已修）：`_masked_preview` 改为等长全遮蔽 `"*" * len(value)`（沿用实现里 ≤6 分支既有风格；长度信息本就由 `Hit.start/end` 契约承载，不新增泄露面），core.py 内加反重构警告禁止改回部分掩码；sanitize/README.md:13 同步措辞"全遮蔽，绝不含原文"；新增 `test_preview_fully_masked` 钉住（含防空转假绿前置断言）。核查 Hit/SanitizeResult 其余出口字段：`detail` 恒为类别标签、`text` 为脱敏输出，均不带原文，无需改。
- **F4 [低] tools/memory-lab/sanitize/core.py:241-247（对照 README.md:31）** — "fail-closed"实为"传播式"（无 try/except，检测异常沿 sanitize() 上抛、调用方发送中断）+ "多脱敏"语义，无显式护栏。影响：未来若有人在调用点包一层兜底 except 吞异常，即静默变 fail-open，现无测试或注释拦截。建议：__init__.py 契约注释明写"禁止在 sanitize 外吞异常"，并补一条"检测器异常必须上抛"的护栏测试。
  - 处置（阶段 3，已修）：新增 `FailClosedGuardTest` 三断言护栏——检测器抛 RuntimeError 必上抛；畸形候选元组过短在 sort key 索引处抛 IndexError、过长在 scan 解包处抛 ValueError（两个崩溃点都钉住，均为内存 monkeypatch 注入）；`sanitize()` docstring 明写"传播式 fail-closed、禁止调用点包兜底 except 吞异常"。契约注释落在 core.py（__init__.py 不在阶段 3 允许改动清单内，且 re-export 模块非契约唯一载体）。

## 总裁决

**有 finding 需修（非阻塞代码面）**：正确性/回归面/静默失败面/测试有效性全通过，实测复现泄漏 0、误杀 0；4 条 finding 均为口径与护栏层（F1/F2 必修——文档与回单数字，F3/F4 建议修）。核心代码与测试资产质量过关，可放行进入下一阶段，修 F1/F2 后关闭本关卡。
