# F-07 前置验证 POC：Win10 下静默写 per-user .md 关联的生效性实测

- 日期：2026-11-10 ｜ need 1 ｜ F-07 前置验证 ｜ 执行：qa-tester
- 环境：Windows 10 22H2（10.0.19045.4894），实机验证；POC 占位程序 = notepad.exe，全程 HKCU、无提权
- **三态结论：部分生效**（注册表写入层生效；用户行为面 / 生效关联不生效）

## 一句话结论

静默写 `HKCU\Software\Classes\.md` 默认值 + 自建 ProgId，在 Win10 22H2 上注册表数据可持久化、可回读、可幂等复写，但 ShellExecute（=双击等价路径）两次实测均拉起系统「选择打开方式」对话框（OpenWith.exe），notepad 零次被拉起——**写入不足以静默改变生效关联**；与 clue4 官方标注（default-programs 静默机制 Win10 起对 UserChoice 场景不适用）实机吻合。D-5 若要真正接管 .md 默认，需引导用户显式选择（LaunchAdvancedAssociationUI / openwith 流程）。

## 步骤 1：现状记录（「被占」场景如实记录）

- `FileExts\.md\UserChoice`：**不存在**（无 UserChoice，天然非「已钉默认」场景）
- 「被占」表现 = 其他程序的残留登记（非默认占用）：

```
FileExts\.md\OpenWithList    a=WorkBuddy.WorkBuddy  MRUList=a
FileExts\.md\OpenWithProgids WorkBuddy.md  REG_NONE
HKCU\Software\Classes\.md\OpenWithProgids  WorkBuddy.md / QuarkMDMD（均 REG_SZ 空值）
HKCU\Software\Classes\.md    键存在、无默认值
HKCR\.md 合并视图默认值 = (数值未设置)
```

即：基线状态 .md 在注册表层无任何生效默认 → 双击本就走 OpenWith 选择器。

## 步骤 2：备份

`%TEMP%\f07-poc-backup\`（仓库外）：
- `fileexts-md.reg`（794B，FileExts\.md 整键）— export 成功
- `classes-md.reg`（356B，HKCU\Software\Classes\.md）— export 成功

## 步骤 3-4：写入与回读（注册表层生效的证据）

```
reg add DevDeskPOC.md (默认)="DevDesk POC Markdown Handler"         → 操作成功完成
reg add ...\shell\open\command = "notepad.exe \"%1\""               → 操作成功完成
reg add HKCU\Software\Classes\.md /ve /d "DevDeskPOC.md"            → 操作成功完成
回读：DevDeskPOC.md 三级结构齐全；HKCU\Software\Classes\.md (默认)=DevDeskPOC.md
HKCR\.md 合并视图 reg query 回读 = DevDeskPOC.md（写入在合并视图可见）
```

## 步骤 5：行为实测（行为面不生效的证据）

`cmd /c start` 拉起 `%TEMP%\f07-poc-test.md`（与双击同走 ShellExecute），tasklist 前后差集抓新增进程：

```
第 1 次：NEW processes = [OpenWith.exe]     notepad NOT in process list
第 2 次（幂等复写后复测）：NEW = [OpenWith.exe]  notepad NOT in process list
辅助回读：cmd assoc .md  → 没有为扩展名 .md 找到文件关联
          cmd ftype DevDeskPOC.md → 没有找到文件类型（与 reg query HKCR 可见性分歧，如实记录）
```

两次实测稳定复现：系统不采纳 HKCU per-user 默认值，宁可弹选择器。notepad 拉起次数 = 0。

## 步骤 6：幂等验证

重复执行步骤 3 全部三条 reg add → 全部「操作成功完成」，回读值与首次一致 → **幂等成立**（静默重写可安全重复执行，不会累积副作用）。

## 步骤 7：恢复声明（对照步骤 1 逐键核验）

| 键 | 步骤 1 基线 | 恢复后 | 判定 |
|---|---|---|---|
| FileExts\.md\UserChoice | 不存在 | 不存在（7k 错误=找不到） | 一致 |
| FileExts\.md\OpenWithList | a=WorkBuddy.WorkBuddy, MRUList=a | 同左（7j） | 一致 |
| FileExts\.md\OpenWithProgids | 仅 WorkBuddy.md | 仅 WorkBuddy.md（7j） | 一致 |
| HKCU\Software\Classes\.md | 有 OpenWithProgids、无默认值 | 同左（7e） | 一致 |
| HKCU\Software\Classes\DevDeskPOC.md | 不存在 | 不存在（7d 找不到） | 一致 |
| HKCR\.md 合并默认 | 未设置 | 未设置（7g） | 一致 |

附加处置：POC 探测期间 shell 在 FileExts OpenWithProgids 侧记了 `DevDeskPOC.md REG_NONE` 副产物（基线无），已 `reg delete /v DevDeskPOC.md` 清除并复验；测试文件 f07-poc-test.md 已删；备份目录保留于 `%TEMP%\f07-poc-backup\`。

**残留声明（唯一）**：POC 拉起的系统对话框进程 OpenWith.exe（PID 8468）在对话框关闭后仍以无窗口形态驻留（MainWindowTitle 为空，CloseMainWindow 不可达）；受「禁杀系统进程」约束未强杀，进程无可见窗口、无输入占用，预期由 shell 自行回收。**除该无窗口进程外恢复完整。**

## 三态判定依据

- **生效**（写入+行为均改）＝未达成：notepad 零次拉起
- **部分生效**＝达成：写入持久化 ✓ 回读一致 ✓ HKCR 合并视图可见 ✓ 幂等 ✓；但生效关联/行为面 ✗
- **不生效**（连注册表数据都落不了盘）＝未发生：写入层证据完整

判定：**部分生效——注册表归属数据可静默落盘且幂等，但 Win10 22H2 关联解析不采纳 per-user .md 默认值（无 UserChoice 时宁可弹选择器），用户可见默认未变。**

## 对 D-5 终裁的建议

1. F-07「静默重写关联」在本版本面不可依赖纯注册表写入改变生效默认；DevDesk 写入 ProgId（让自身出现在 openwith 候选）可行，接管默认需用户显式选择。
2. 建议实现口径：写 HKCU ProgId + 关联元数据（可静默、幂等，本 POC 已证）+ 需要接管时调 `openwith`/LaunchAdvancedAssociationUI 引导（与 clue4 官方标注一致）。
3. 设置页开关的「生效/未生效」状态展示应以 UserChoice/QueryCurrentDefault 为准（注册表写入≠生效，本 POC 为反例实证）。
4. 未测面（明账）：①存在 UserChoice 且被其他程序占用时，静默写 HKCU 默认是否同样被屏蔽（本机无 UserChoice，未构造）；②SHChangeNotify 刷新效果（POC 判定不依赖，未测）；③QueryCurrentDefault API 实值回读（用注册表等价回读替代，PRD 允许）。
