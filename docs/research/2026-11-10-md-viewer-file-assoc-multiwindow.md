# 只读 MD 查看器：平台文件关联 + EUI-NEO 多窗口承载预览能力调研（need 1 前置）

- 日期：2026-11-10 ｜ 类型：调研（L 级需求前置） ｜ 范围：外部不确定点钉死，不出 PRD/技术方案
- 工具调用 41 次（预算 40，超 1 次如实标注）；墙钟内完成

## 结论先行

1. **Windows 双击 .md 不会拉起第二进程**：单实例守卫已存在（`src/main.cpp:628-629` + `src/platform/single_instance_win.cpp:63-95` 命名 mutex `Local\DevDesk.SingleInstance.Mutex`）。但第二实例的文件路径**不会传给第一实例**（`notifyExistingInstance` 只 `SetEvent` 唤起主窗，`single_instance_win.cpp:97-110`；监听线程只调 `app::requestShow()`，`:33-57`）——运行中双击 = 窗口弹起但文件丢失，这是全需求最大的现有缺口。
2. **macOS「已运行实例接收文件」链路已全通，零缺口**：Finder 双击/「打开方式」→ kAEOpenDocuments Apple Event → `pollOpenFiles` 每帧派发（EUI-NEO `core/platform/platform.h:117-141`、`platform.cpp:900-927`、`core/app/glfw_app_main.cpp:830`）→ DevDesk 已接线 `.onOpenFile`（`src/main.cpp:705`）→ `handleAppleEventOpenFile`（`src/main.cpp:308-317`）。macOS .app 单实例由系统保证（`src/platform/single_instance.h:10`）。
3. **EUI-NEO 多窗口能力确认可用且与源码一致**：`app::openWindow` 为请求队列模式（`include/eui/dsl_app.h:366-367` 声明、`include/eui/detail/dsl_app_impl.h:170-202` 实现、`:212-216` 由主循环 `createRequestedWindows` 消费建窗，`glfw_app_main.cpp:874/:1026/:1035`，销毁 `:1036`）；每窗口独立 `compose = void(Ui&, Screen&)`（`include/eui/app.h:16`）+ 独立 keyHandler/生命周期钩子（`dsl_app.h:331-360`）。滚动容器官方组件 `components/scrollview.h:16-40`（`offset()`/`bind(Signal<float>&)` 支持程序化滚动，即滚动条 + 同步滚动）。**未发现需上游补 PR 的能力缺口**。
4. **渲染复用编辑器预览可行**：`src/module_markdown/md_module_preview.cpp`（1108 行）全部为 `MarkdownModule` 成员函数，签名 `(Ui&, Screen&, Tokens, componentTheme)`，**全文件零 Router/PageId 引用**（grep 证实）——耦合对象是模块内部状态（buffer/outline/搜索命中），不是 Router 页面上下文。抽到独立窗口 = 把一个 MarkdownModule 形态的实例交给第二窗口 compose，无渲染管线自研。
5. **UTI 是 macOS 侧唯一硬缺口**：`platform/macos/Info.plist:106` 有 `UTExportedTypeDeclarations` 键但全文件零 `UTTypeIdentifier`（grep 证实）= 无有效 UTI 声明；`LSMinimumSystemVersion=11.0`（`:31-32`）在 macOS 11–15 上，若用户机器无其他 app 声明 `net.daringfireball.markdown`，.md 落动态 UTI（`dyn.*`），按内容类型设默认会设不上。**必须补 `UTImportedTypeDeclarations`**。

## 对比表（按问题面）

| 问题面 | 结论 | 分级 | 一手依据 |
|---|---|---|---|
| A1 Win per-user 关联注册 | HKCU\Software\Classes\ProgId + shell\open\command（官方样例结构：(Default)+DefaultIcon+shell\open\command） | 已确认 | [Default Programs - MS Learn](https://learn.microsoft.com/en-us/windows/win32/shell/default-programs)（精读） |
| A2 Win 查询归属 | `IApplicationAssociationRegistration::QueryCurrentDefault`（单次调用查默认归属） | 已确认 | 同上页 + API 语义（精读） |
| A3 Win 注销 | 无专用 API = 删 HKCU 相关键；恢复他 app 为默认只能由用户操作 | 已确认（无 API）/待验证（删键后 Shell 缓存时序） | default-programs 页无 unregister API（精读） |
| A4 Explorer 即时感知 | `SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, NULL, NULL)`，官方 Remarks 原文 "must call"（刷新图标/缩略图缓存） | 已确认 | [SHChangeNotify - MS Learn](https://learn.microsoft.com/en-us/windows/win32/api/shlobj_core/nf-shlobj_core-shchangenotify)（精读） |
| A5 argv 传递 | command 串 `"exe" "%1"` → argv[1]；现有 `openFileFromArgv` 用 GetCommandLineW+CommandLineToArgvW 且只取 argv[0..1]（`file_open_win.cpp:50-63`）已兼容 | 已确认 | MS Learn + `src/platform/file_open_win.cpp:50-63` |
| A6 运行中双击 | 不拉第二进程（mutex 挡）；路径丢失（见结论 1） | 已确认 | `single_instance_win.cpp` 全文实读 |
| A7 Win10+ 静默设默认 | default-programs 机制页首标注 "does not apply for Windows 10"；UserChoice 受保护；`SetAppAsDefaultAll`/`QueryAppIsDefault` Win8 起弃用；推荐 UI = `IApplicationAssociationRegistrationUI::LaunchAdvancedAssociationUI`。**"开关开但被他 app 占用 → 静默重新关联"在 Win10/11 的实际生效性未验证**；自愈仅重写自己 ProgId/无 UserChoice 场景可静默 | 部分待验证 | default-programs 页（精读）+ [Windows 10 FTA 变更公告](https://blogs.windows.com/windowsexperience/2015/05/20/announcing-windows-10-insider-preview-build-10122-for-pcs/)（扫描） |
| B1 mac 设默认 API | `LSSetDefaultRoleHandlerForContentType`：macOS 10.4–12.0，**12.0 起弃用**；替代 = `NSWorkspace setDefaultApplication(at:toOpen:)`（macOS 12+）。DevDesk 下限 11.0 → 需双路径或抬下限 | 已确认 | [LSSetDefaultRoleHandlerForContentType - Apple](https://developer.apple.com/documentation/coreservices/1444955-lssetdefaultrolehandlerforconten)（扫描，官方页） |
| B2 mac 查询默认 | `LSCopyDefaultRoleHandlerForContentType`（同批弃用）→ `LSCopyDefaultApplicationForURL` 替代 | 已确认（API 存在与弃用批次）/待验证（替代签名） | 同批 Apple 文档（扫描） |
| B3 UTI 映射 | `net.daringfireball.markdown`：Apple 官方 `UTType.markdown` 存在（conforms utf8PlainText）；历史非系统声明、需 app 自声明，无声明机器落动态 UTI；较新 macOS（26+）系统内建（二手，待实测各版本） | 已确认（机制）/待验证（11–15 各版本系统是否内建） | [UTType.markdown - Apple](https://developer.apple.com/documentation/uniformtypeidentifiers/uttype-swift.struct/markdown)（官方页）+ [Gruber 官方 UTI 定义](https://daringfireball.net/linked/2011/08/05/markdown-uti) + [Stack Overflow 34927527](https://stackoverflow.com/questions/34927527/how-to-get-around-when-another-app-incorrectly-defines-a-uti)（扫描） |
| B4 Info.plist 前提 | `Info.plist:39-65` CFBundleDocumentTypes（Markdown/JSON/JS，role=Editor，rank=Alternate）；`:106` UTExportedTypeDeclarations 空壳（零 UTTypeIdentifier）→ 需补 UTImportedTypeDeclarations（identifier=net.daringfireball.markdown + extensions md/markdown + conforms public.plain-text） | 已确认 | `platform/macos/Info.plist` 实读 + grep |
| B5 DMG 非沙箱 | 非沙箱 app 可调 LaunchServices 设默认；无 App Store 审核限制 | 已确认（无限制性文档）/待验证（12+ NSWorkspace 是否弹确认 UI） | B1/B2 Apple 页无沙箱限制条款（扫描） |
| B6 运行中复用实例 | 见结论 2，链路已全通 | 已确认 | EUI-NEO/DevDesk file:line（上） |
| C 多窗口承载 | 见结论 3、4 | 已确认 | EUI-NEO file:line（上） |
| D 差距清单 | 见下节 | 已确认 | DevDesk file:line（下） |

## 现状 vs 缺口清单（供 PRD 直接引用）

已具备（实读锚点）：
- argv 文件捕获：`src/platform/file_open_win.cpp:50-63`（GetCommandLineW+UTF-16→UTF-8）、`file_open_macos.cpp:18-28`（_NSGetArgc/_NSGetArgv）→ `app_assembly.h:55-65` 待打开队列（`pendingOpenFilePath`/`captureOpenFileFromArgv`）
- 分发：`app_assembly.h:85-90` `dispatchPageForFile`/`dispatchFileOpen`（当前按扩展名路由进 Router 内嵌页，`app_assembly.cpp:183`；md 走编辑器页）——无独立窗口分支
- macOS 运行中接收：`main.cpp:705,308-317`（Apple Event → dispatchFileOpen）
- Windows 单实例：`main.cpp:628-629` + `single_instance_win.cpp`（mutex+event+监听线程）
- 预览渲染素材：`md_module_preview.cpp`（预览列/块流/大纲/同步滚动/undo-redo，成员函数零 Router 耦合）、`md_highlight.cpp`（222 行）、`md_keyword_highlight.cpp`（527 行，多语言关键字高亮）；搜索命中渲染签名见 `markdown_module_internal.h:486-489`（ui/k/hitId 回调形态）
- 滚动条：DevDesk `components::scrollView`（`md_module_preview.cpp:81-88` 实用例）→ EUI-NEO `components/scrollview.h:16-40`（step/gap/scrollbarWidth/offset/bind）

缺口（PRD 需覆盖）：
1. Windows 第二实例文件路径传递：扩展 `notifyExistingInstance`/事件管道带路径，监听线程收信号后写 `pendingOpenFilePath()`（macOS 同通道可对称）——改动全在 DevDesk 侧，非上游缺口
2. dispatchFileOpen 增「独立查看器窗口」目标（现仅 Router 内嵌页）
3. Info.plist 补 `UTImportedTypeDeclarations`（net.daringfireball.markdown）
4. mac 设默认双 API 路径（11.0 兼容 LSSet… + 12+ NSWorkspace）或抬 LSMinimumSystemVersion 至 12.0（产品决策，禁本报告代决）
5. Windows 关联写入模块（HKCU ProgId + .md 指向 + SHChangeNotify + QueryCurrentDefault 查询）——installer（devdesk.iss）现零关联注册，自愈逻辑要求运行时写 HKCU
6. Win10+ 「被他 app 占默认」场景的静默自愈生效性需 POC 实测（建议 `poc_` 用例：写 HKCU→QueryCurrentDefault→SHChangeNotify→AssocQueryString 复核）

## 三问门

- 候选「复用 EUI-NEO openWindow + MarkdownModule 预览 + 平台关联 API」：能否落地=能（上述 file:line 与官方 API 全齐）；100% 覆盖=是（对照表全覆盖，唯 A7/B5 两点待验证已列 POC 项）；生产级参考=是（本仓源码 file:line + MS Learn/Apple 官方页）→ **进推荐**。
- 淘汰项：自研渲染管线（用户已拍板禁议）；写 UserChoice 哈希绕过（undocumented、被杀软/更新破坏风险，弃）。

## 遗留（未闭环 + 差什么）

1. A7：Win10/11 被占默认时静默重关联的实际行为——差一次实机 POC（写注册表后 QueryCurrentDefault 回读 + Explorer 实测双击）。差证据：UserChoice 存在时 HKCU\Software\Classes\.md 写入是否影响 ShellExecute 解析顺序。
2. B3/B5：macOS 11–15 上 `net.daringfireball.markdown` 系统内建与否逐版本实测；NSWorkspace 设默认是否弹系统确认框。差证据：一台 macOS 11/12+ 实机 UTType 查询 + 设默认实跑。
3. C 补充：glfw 主循环中逐窗口 compose 的调用点未逐行核（已证：请求消费/建窗/销毁/多窗动画管理 `glfw_app_main.cpp:1031-1046`；窗口级 compose 签名与钩子 `app.h:16`/`dsl_app.h:331-360`）——对 PRD 无阻塞，属实现期细节。
4. 窗口内搜索高亮的搜索框 UI 复用形态（编辑器页的搜索条是否模块态）未展开——属 PRD 后的技术方案粒度。

## 来源

精读：MS Learn default-programs（全文）、MS Learn SHChangeNotify（全文）、DevDesk/EUI-NEO 上述全部 file:line 段（±60 行纪律）。
扫描：Apple LSSetDefaultRoleHandlerForContentType 页、Apple UTType.markdown 页、Daring Fireball UTI、SO 34927527、Windows 10 FTA 变更博客、CommonMark UTI 讨论、Warp issue #13268、danstutorials（macOS 27 内建说法，二手单源）。

### 新查证事实清单（已回填线索表）

- clue id=4：devdesk:win-file-association-mechanism
- clue id=5：devdesk:macos-uti-markdown
- clue id=6：devdesk:eui-neo-multiwindow-openfile
