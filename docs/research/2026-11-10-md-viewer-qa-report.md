# 只读 MD 查看器 QA 收口报告（need#1 · wt/mdv-int @ 6c30ea90）

- 日期：2026-11-10 ｜ 类型：审计（QA 收口档）｜ 池条目：need id=1 ｜ 执行：qa-tester
- 对象：worktree D:/project/gui/wt-mdv-int 分支 wt/mdv-int HEAD=6c30ea90（实现+评审修复全在树）

## 总判定：半成品（代码可合入不变；验收闭环欠 4 块，见「未验证项清单」+ BUG-1）

## 一、全量回归（收口档唯一全量执行档，实跑）

- 构建：scripts/build-test-run.sh --no-run → 构建成功（版本 bump 0.6.2→0.6.3），发布形态单 exe 零外部 DLL
- 单测：**Total 1523 / Passed 1514 / Failed 0**（exit 0，脚本判「测试全绿」；Total-Passed 差 9 为计数口径非失败，无失败行）
- 探针：ctest -L probe **18 项 17 绿**；唯一失败=agent_e2e_probe，原因=生产库无激活 profile 凭据（LLM 腿搬运失败，五流程计「未跑」）→ **未验证-环境**，与既有定性一致，不算回归
- 上轮基线复现成立：1523 全绿 + 探针全绿（除环境前置）仍在本轮 exit 0 输出中

## 二、真机实操（win 本机，隔离 DEVDESK_DATA_ROOT 临时根，未碰用户数据）

| 实操 | 结果 | 证据 |
|---|---|---|
| argv 启动打开 ASCII 路径模板（qa_tpl.md=MD测试模板.md 同内容副本） | 独立查看器窗渲染成功：标题=文件名、标题层级/引用/行内格式/列表全出、滚动条在 | f03_viewer_template_ascii.png |
| 滚轮滚动到代码块段 | 有序/任务清单（勾选框）/cpp 代码块灰底块渲染 | f03_viewer_code_table.png |
| 滚轮滚动到表格段 | 表格表头+4 数据行+嵌套引用渲染 | f03_viewer_table.png |
| 第二实例带文件启动（F-01 进程内送达链） | 第二实例自退（进程数保持 1），路径送达首实例并开第二查看器窗 | 日志「单实例：第二实例送达 1 个文件路径」+ f04_two_windows.png |
| 双窗并存 | qa_b.md 与 qa_tpl.md 两查看器窗并排渲染、标题互异 | f04_two_windows.png |
| Ctrl+F 唤起搜索条 | 搜索条出现（查找输入框+计数位+×）；**文本注入未落**（合成输入中文/ASCII 均不入框，IME/合成输入 harness 侧限制）→ 真机键盘输入归 UAT | f05_search_highlight.png |
| argv 打开**中文路径**（D:\Data\Desktop\MD测试模板.md，文件存在可读） | **BUG-1：错误占位「无法打开：无读取权限或打开失败」**；标题正确显示中文文件名（路径已正确送达）；同内容 ASCII 副本正常 → 非内容问题，指向非 ASCII 路径的文件打开层缺陷 | bug1_zhpath_error.png + f03_viewer_template_ascii.png 对照 |
| 单实例送达中文路径复测 | **不定论**：首实例日志零送达行（时序撞 splash 启动窗口，观测不构成缺陷证据也不构成通过） | bug2_zhpath_singleinstance.png（仅主窗） |
| 探针截图（settings_md_assoc_probe --screenshot） | F-10 卡四态几何+截图判据全过 | f10_assoc_off_never/on_ours/on_others/off_ever.png |
| 探针截图（md_viewer_probe --screenshot） | 探针全断言绿；尾帧=B7 错误占位窗（缺文件） | f03_viewer_probe_tail.png |

截图目录：D:/project/gui/wt-mdv-int/docs/screenshots/md-viewer/（14 张，均可打开非空；每图验收点见文件名+上表）

## 三、假绿抽查（变异验证，改期望→红→还原→绿，工作区零残留）

| 断言 | 变异 | 红证据 | 还原后 |
|---|---|---|---|
| F-05 计数 test_markdown.cpp:8897 | 期望命中 3→4 | FAIL `ms.size() != size_t(4)`，Total 1/Failed 1 | Total 1/Passed 1 |
| F-10 状态字 test_md_assoc_logic.cpp:39 | 关+曾开期望 MaintenanceStopped→None | FAIL（同文件 :39），md_assoc 4 项 2 红 | Total 4/Passed 4 |
| F-11 关态决策 test_md_assoc_logic.cpp:63 | 关×Others 期望 Skip→Repair | FAIL（同文件 :63） | 同上 4/4 绿 |

三条均红过=非恒真。F-11「关态零查询」本体在 main.cpp 三目短路（clue devdesk:f10-u11-impl-facts ⑥），决策矩阵变异为等价论证。

## 四、发布检查·交付前三问

1. **存量数据升级路径**：安全。mdAssocEnabled/mdAssocEverEnabled 结构体默认 false（settings_config.h:93/96），解析仅在有键时覆写（settings_config.cpp:441/447）→ 存量库无键=开关默认关=自愈不跑（决策矩阵关态 Skip + main.cpp 三目短路零查询）→ 升级零行为变化。证据：md_assoc_store_roundtrip（含 D-4 关+曾开过跨重启）变异还原后 PASS。
2. **真实外网/网关**：不适用。本需求全链路本地（注册表/本地文件渲染/单实例管道），无网络调用面；agent_e2e 的 LLM 腿属环境前置非本需求面。
3. **目标平台真机**：win 本机已实测=构建/全量单测/探针 17/真机 argv 双路径/双窗/渲染全谱/搜索条唤起/F-01 进程内送达；win 未实测=资源管理器真双击（F-01 端到端）、真注册表写入+回读开关联动（F-06/F-10/F-11，测试红线禁触开发机登记，用例均注入缝）、多文件同送。mac=全部未编译未实测（.mm/Info.plist/api_map 为 CI/mac 机兜底）。

## 五、BUG 清单（挂池跟踪，不自行修）

- **BUG-1（P1，新缺陷，未修）**：中文路径 .md 文件查看器打开失败。现象：argv 路由下 D:\Data\Desktop\MD测试模板.md（存在可读）→ 查看器显「无法打开：无读取权限或打开失败」，标题正确（路径送达无碍）；同内容 ASCII 路径副本渲染正常。关联 F-02/流程 B5/B7，命中核心场景（中文文件名），候选根因=UTF-8 路径进 ANSI 文件 API。修复路由→bug-fixer；修后必回归：中文路径 argv+单实例两路各一次+「还原验红」。证据 bug1_zhpath_error.png。
- 观测记录（不计缺陷）：单实例中文路径送达复测不定论（时序撞 splash）；搜索条真机文本注入未落（合成输入限制，非应用缺陷——Ctrl+F 键事件已到达）。

## 六、F-ID 核销矩阵

| F-ID | 结论 | 依据/欠账 |
|---|---|---|
| F-01 | 半 | 进程内送达链真机证据 ✓（第二实例自退+首实例开窗）；资源管理器真双击未验；中文送达不定论 |
| F-02 | 半 | 独立查看器窗真机 ✓（ASCII 路径）；BUG-1 中文路径打不开；应用内点笔记回归靠既有用例绿 |
| F-03 | 完成（win 面） | 渲染全谱截图+滚动条+只读无编辑入口探针断言 ✓ |
| F-04 | 完成（win 面） | 双窗并存截图+900×640/标题=文件名探针断言 ✓；关窗互不影响靠探针级+未真机关窗 |
| F-05 | 半 | 状态机/高亮/跳转/回绕单测+探针绿 ✓；真机键盘输入归 UAT（harness 打不通） |
| F-06 | 半 | 写入/查询/幂等注入缝用例绿；真注册表未验（红线） |
| F-07 | 完成 | POC 三态结论入台账+线索表（clue devdesk:f07-userchoice-poc：部分生效） |
| F-08 | 未验证 | 代码+XML 良构 ✓，未编译未真机 |
| F-09 | 未验证 | 实现+api_map 单测绿（win 可编译面）；mac 运行期未编译未实测 |
| F-10 | 半 | 四态卡探针（几何+截图）+矩阵/持久化单测绿 ✓；真注册表开关联动未验 |
| F-11 | 半 | 决策矩阵+自愈块+关态短路 ✓；真机重启自愈未验 |
| 多文件送达 | 未验证 | 真机多文件同送未测（D-1 逐文件开窗探针级 ✓） |

## 七、未验证项清单（交付汇报「未验证」栏）

1. mac 编译/实测全域（.mm/Info.plist/api_map CI 兜底）——F-08 干净机器 UTI、F-09 macOS 11–15 逐版本
2. F-01 资源管理器真双击（5 秒内开窗+进程数 1 端到端）
3. F-06/F-10/F-11 注册表真机关联链（真写 HKCU→QueryCurrentDefault 回读→开关回显）
4. D-5/D-6 终裁（挂用户拍板）
5. agent_e2e_probe 五流程（环境前置：生产库无激活 profile）
6. F-05 真机键盘输入全流程（中文+ASCII 合成输入均未落；真键盘 UAT）
7. 单实例中文路径送达（复测不定论，待 BUG-1 修后并验）
8. 多文件同送（3 个 .md 依次 3 窗）

## 八、建议

1. BUG-1 交 bug-fixer：非 ASCII 路径文件打开层（候选 std::ifstream/fopen ANSI 面），修后按第六节 F-02/F-03 回归。
2. 真机双击/注册表链/多文件同送：需人工 UAT 半小时档（关联生效后 Explorer 真双击一次即可同时覆盖 F-01/F-02/F-11 端到端）。
3. mac 面攒 CI/mac 机一并验（F-08/F-09 + D-6 回填），不阻塞 win 合入。
