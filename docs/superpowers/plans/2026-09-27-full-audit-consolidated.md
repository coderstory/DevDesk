# DevDesk 全量审计汇总（六分片 + 专家审计 + 本轮修复对照）

- 日期：2026-09-27 ｜ 基线：master tip `2d2ebfa`（各分片自己的基线见 §1）
- 输入：六份 `2026-09-27-full-audit-*.json`、`2026-09-27-expert-audit.md`、`2026-09-27-HANDOFF.md` §9.4、`git log --oneline --merges e99ee21..HEAD`（39 个 merge）、`2026-09-27-refactor-incremental-design.md`（R0–R9）、`2026-09-27-god-class-refactor-design.md`（R10/R4/R5/R8 增补）
- 口径：**只汇总不改判**。证据一律沿用分片原文的 `file:符号名`（行号会漂移，动手前按符号名重定位）；"已修"以 merge 消息 + 文件级核对为准，个别标「待核」。
- 本文未改代码、未跑构建/测试/应用。

## 1. 概览

| 分片 | 前缀 | 条数 | 高 | 中 | 低 | 分片基线 |
|---|---|---|---|---|---|---|
| core + platform | `CP-` | 12 | 2 | 6 | 4 | `f47766b` |
| claude_proxy | `CPX-` | 15 | 3 | 7 | 5 | `a3d13f4` |
| todo + home + settings | `THS-` | 18 | 0 | 6 | 12 | `f47766b` |
| pet + app + 构建 | `PAB-` | 13 | 1 | 5 | 7 | `7e86964` |
| tests + 基建 | `TIN-` | 13 | 2 | 5 | 6 | `7e86964` |
| markdown | `MD-` | 8 | 1 | 3 | 4 | `ad82fbc` |
| **合计** | | **79** | **9** | **32** | **38** | |

- **去重**：79 → **77 行**。仅两组属"同一根因、同一修法"合并：`THS-03 + PAB-07`（M3 只修了 config.json，其余 JSON 落盘副本仍是截断写）、`PAB-02 + THS-08`（模块清单多源，装配层与 Home 各硬编码一份）。其余"结构同类但位置不同"（如 THS-01 vs 专家 H2、CPX-06 vs H2、THS-09 vs 专家 M4、TIN-12 vs 专家 L2）分片已自陈不是重复，保持独立。
- **kind 分布**：maintainability 33 ｜ architecture 12 ｜ correctness 11 ｜ pattern 9 ｜ oop 5 ｜ testability 5 ｜ deadcode 3 ｜ security 1。
- **lens 分布**：arch 44 ｜ cpp 18 ｜ test 15 ｜ oop 1 ｜ security 1。
- **状态分布**（79 条 findings / 77 行）：已修 **10 条**（9 行）｜部分已修 **3 条**（CPX-02 / TIN-02 / THS-15）｜待排 **66 条**（65 行）｜已排除 **0**。分片项无一条处于"在飞未回单"，三条"部分已修"的剩余半均已在表内点明。

## 2. 总表（按"架构 / 可维护性"影响排序：高 → 中 → 低，同档按影响面）

| 编号 | 级 | kind/lens | 一句话 | 证据（file:符号名） | 影响（维护成本/演进代价） | 建议改法 | 状态 |
|---|---|---|---|---|---|---|---|
| CPX-01 | 高 | oop/arch | ClaudeService 是 God 类：7 类职责 + 6 组状态 + 33 个公开方法 | `claude_service.h:ClaudeService` | 任一协议改动都在 1409 行类内动手；新能力只能继续往类里堆 | 拆 ProfileRepository / ProxyRuntime / SettingsTakeOver，门面保留签名（R10） | 待排（god-class §2 已设计；Q1–Q3 待拍板） |
| TIN-01 | 高 | architecture/arch | 模块隔离用 grep 源码文本断言，且正向断言把 settings→`claude_service.h` 耦合钉成不变量 | `tests/test_modules/test_settings.cpp:settings_module_isolation_guard` | R0 删该 include 时用例必红被当回归；注释变动即误判 | 静态约束移出单测 → `scripts/check-module-isolation.sh`；删正向断言 | 待排（R0 同批；正向断言现仍在） |
| MD-01 | 高 | correctness/cpp | 实时态活动块锚 `liveCursorByte_` 只初始化不推进，块边界一变就换文本 + 光标归零 | `markdown_module.cpp:drawLiveColumn`、`syncDocumentFromLiveInput` | 实时态已合 master 可进入 → 现网可触发丢字/跑位 | 选活动块前用活动块起点回填锚（setLiveCursor 最小版） | 待排（与 Phase3 M3 同批） |
| CPX-02 | 高 | architecture/arch | 接管/还原返回 void，startProxy/setActiveProfile 丢弃结果，跳过键只进日志 | `claude_service.h:syncTakeOver/syncRestore`、`ClaudeService::startProxy` | 用户只看到"已启动"；全仓唯一不用 `Result` 的写路径 | 两处改 `Result<void>`，warnings 并入 `lastSyncWarnings_` 统一展示 | 部分已修（`71b13a9`/`2f82c7c`：takeOver 已上报；syncRestore 仍 void） |
| TIN-02 | 高 | maintainability/test | worktree 隔离被机器级资源共享击穿：脚本杀全机同名进程 + 临时路径固定名 | `scripts/build-test-run.sh:taskkill -F -IM`、`tests/test_modules/test_app_assembly.cpp:tempNotesDir` | 并发多 worktree 互相 kill/覆写 → 随机红，归因成本高 | 临时名加 pid；taskkill 前按 pidfile 只杀自己 | 部分已修（`51ad034` 经 `27d9a82`：临时路径 pid 唯一化；taskkill 仍存） |
| CP-01 | 高 | architecture/arch | core 在 Router::compose 内调 `eui::Ui` 渲染兜底页，越过 spec §4.2.1「只透传」 | `src/core/router.cpp:Router::compose` | core 持有 UI 内容策略；`grep 'ui\.' src/core` 机械护栏失效 | 空态交 app 层注入回调，core 零 `ui.` 调用 | 已修（`3515e28`/`61ecab6`） |
| CP-02 | 高 | oop/cpp | JsonValue 双份表示，4 个特殊成员不维护镜像 → 左值拷贝/移动静默错值 | `src/platform/json.cpp:JsonValue` 拷贝/移动成员 | `asString/asInt` 静默返回默认值，靠 prvalue 省略侥幸不爆 | 删镜像字段，类型与取值全从 JsonImpl 派生 | 已修（`d443b8c`/`fb92d51`） |
| CPX-03 | 高 | correctness/arch | 接管快照无来源判定：sidecar 丢失后残留托管值被当用户原值永久写回 | `settings_sync.cpp:takeOver/restore` | 清数据/换机后死地址固化，真实上游地址不可恢复 | 快照污染守卫 + 无 sidecar 兜底重建 | 已修（`71b13a9`/`2a5bb77`） |
| PAB-01 | 高 | maintainability/arch | 一体化构建脚本在 worktree 里不可用（不传 EUI_NEO_DIR → configure FATAL） | `scripts/build-test-run.sh:configure 段` | 与"构建必走 worktree"铁律冲突，首建只能手敲 -D（知识易丢） | 三段式探测 EUI-NEO 并透传 `-DEUI_NEO_DIR` | 已修（`27d9a82`/`0efa088`） |
| THS-02 | 中 | maintainability/arch | TodoModule 2474 行 / ~55 成员 / 14 个 draw 方法的 God 类 | `todo_module.cpp:class TodoModule` | 任何 Todo 需求都改同一文件；不变量（detail/confirm 状态）只能靠注释 | 弹窗收成 struct（open+表单+draw+submit 同居），并入 R5 | 待排（god-class §4 已设计） |
| CPX-05 | 中 | maintainability/arch | claude_module 的 compose 单体 863 行，三个详情 pane 用行号级坐标内联 | `claude_module.cpp:void compose` | 字段增删要重算相邻块 y（对偶字面量缺陷的直接来源） | 按既有先例抽 `drawDetail*/drawHeader*`（并入 R4） | 待排 |
| MD-04 | 中 | maintainability/arch | markdown_module.cpp 单文件 3327 行（M1/M2 又 +480） | `markdown_module.cpp:MarkdownModule` | R7 的估算区间被打破，拆分成本升高 | 按 R7 原范围先切 `md_ui_live.cpp` / `md_ui_dialogs.cpp` | 待排（已登记 R7，前提漂移） |
| PAB-02 + THS-08 | 中 | maintainability/arch | 模块接入点分散：装配层公开结构 + 两张无测映射表 + 构建清单（Home 内另有 4 处硬编码） | `src/app_assembly.h:ModuleSet`、`app_assembly.cpp:assembleAndStart`；`home_module.cpp:statCount` | 新增模块/卡片要改 5~7 处，漏改静默（宽度错位、入口消失） | `ModuleSet` → 表驱动 vector + 门控一行表（并入 R8） | 待排（god-class §5.1 已设计） |
| TIN-07 | 中 | testability/test | 进程级全局靠手工登记清理：`resetCaseGlobals` 漏日志级别 / ClaudeService 单例 / proxy_stats / appRouter | `tests/test_main.cpp:resetCaseGlobals` | 一个失败用例让后续全部跑在 Trace 下；跨用例泄漏无告警 | 扩成显式清单 + 函数头「新增全局必须登记」 | 待排 |
| TIN-06 | 中 | pattern/test | 51 个用例文件零共享支撑头：TestEnv 5 份 / TempDir 2 份 / setEnvVar 7 份 | `tests/test_modules/test_app_assembly.cpp:ScopedAppRouter` 等 | 新用例只能靠抄，口径漂移 → TIN-07 与历次 flake | 新增 `tests/test_support.h` 四件套，存量渐进迁移 | 待排 |
| CP-04 | 中 | testability/test | 测试隔离缝是环境变量约定、散落 3 层，无中心入口；`DEVDESK_PETS_DIR` 还参与产品行为 | `src/core/paths.cpp:configFile/markdownDir/petsDir` | 漏设即读写真实 `%APPDATA%`；生产代码长期留 getenv 分支 | 单一 `DEVDESK_DATA_ROOT` 派生全部路径；pet_skin 不看环境变量 | 待排 |
| CP-05 | 中 | maintainability/arch | 托盘「按模块开关收缩」整链已是死代码，谓词表/disabled/测试注释/HANDOFF 仍在描述它 | `src/core/tray_menu_spec.h:isNavigationAction/trayMenuSpec` | 后来者基于错误前提推理；两张 switch 表 + 恒真断言常驻 | 二选一：加回导航项，或删 disabled/谓词/后缀并同步注释与文档 | 待排（需产品拍板） |
| CP-06 | 中 | maintainability/arch | `toHeaderMap` 把大小写不敏感 Headers 降级成大小写敏感 `std::map` | `src/platform/http_server.cpp:toHeaderMap` | 每个消费方各自复刻大小写约定；响应侧 `find("Content-Type")` 靠生产者恰好写对 | 键统一小写 + 两条 header 回调合一 | 待排 |
| CP-07 | 中 | oop/cpp | `SqliteDb`/`Database` 公开默认构造 = 「未 open 但合法」，方法对 `handle_ == nullptr` 无守卫 | `src/core/sqlite_db.h:SqliteDb() = default` | 未打开的对象能传递到首次 exec 才以 MISUSE/静默 0 暴露 | 默认构造私有化，`open()` 唯一入口 | 已修（`5ffa2a2`/`f06da47`） |
| CP-08 | 中 | pattern/arch | `Result` 有 4 个取值口，`value()` 行为随 TU 编译选项变，`unsafeValue()` 完全无守卫 | `src/core/result.h:value/expect/unsafeValue/valueOr` | 失败行为读代码不可判；unsafeValue 在 app 里等于无上下文 terminate | 收敛为 `valueOr` + `value`（统一 LOG_FATAL），删 unsafeValue/expect | 待排（专家 C 类 + M8 已覆盖部分视角） |
| CPX-04 | 中 | correctness/arch | 热切换不清理「上一轮托管、本轮不再生成」的键 → 旧模型名留给新供应商 | `settings_sync.cpp:takeOver`、`managedKeys` | A→B 热切换后客户端用 A 的模型名请求 B，400 直到停止代理 | takeOver 写前先 erase 差集键（集合已在手边） | 已修（`71b13a9`/`a8c950e`：stale 托管键先删） |
| CPX-06 | 中 | maintainability/arch | H2「渲染不 IO」只落了 compose 三处：调用记录弹窗在回调里同步查库（搜索逐键两查） | `claude_module.cpp:reloadCallsPage`、搜索框 onChange | H2 约束靠注释维持；代理忙时开弹窗/打字卡帧 | 复用 future 刷新模式 + 防抖；页面统一用注入的接管路径 | 待排（专家 H2 未覆盖部分） |
| CPX-07 | 中 | architecture/arch | 「Keychain + DB 两段写」四处手写、三种补偿策略，updateProfile 这条最常用路径无补偿 | `claude_service.cpp:updateProfile` | 密钥已换、元数据没换，`api_key_hint` 与实际不一致且无从判断 | 抽最小两段写助手（失败按旧值回写 Keychain），四处复用 | 待排 |
| CPX-08 | 中 | maintainability/arch | 加一个模型槽位/官方 env 键要改 9 处（数据模型 1 + 纯函数 4 + UI 3 + 冲突键 1），无编译期提醒 | `provider_config.h:ModelsConfig`、`provider_config.cpp:kSlots` | 漏一处分别表现为 UI 不显示/导入不识别/接管不写/反向不回填，全静默 | 槽位↔键名↔显示名收敛成一张表，UI 侧遍历派生 | 待排 |
| CPX-09 | 中 | maintainability/arch | 「官方 env 键」两份来源：`env_var_hints.h` 256 条 vs `provider_config.cpp` 只认 18 条 | `env_var_hints.h:kEnvVarHints`、`provider_config.cpp:knownOfficialEnvKeys` | 导入 cc-switch 配置时 240 个官方键被误报「未识别」并画红框，噪音淹没真异常 | 白名单由 hints 派生（`envVarHint(k) != nullptr`），删手写清单 | 待排 |
| CPX-10 | 中 | correctness/arch | proxy_router 两条转发路径各写一套取 profile 逻辑，`resolveProfile` 只被流式路径用 | `proxy_router.cpp:resolveProfile`、缓冲路径内联分支 | 同一 Keychain 读失败在 stream:false/true 下报不同状态码与文案，排障要分叉 | 缓冲路径改调 `resolveProfile`，统一错误码/message | 待排 |
| MD-02 | 中 | maintainability/arch | `drawLiveColumn` 与 `drawEditorColumn` 的列壳逐行复制（提示/chip/状态栏/720 钳制/顶距） | `markdown_module.cpp:drawEditorColumn`、`drawLiveColumn` | 列壳任何调整要改两遍，靠注释「与编辑态同款」口头同步 | 抽 `drawTextColumnChrome(...)` 返回内容区，零行为变化 | 待排 |
| MD-03 | 中 | architecture/arch | `drawBlockFlow` 成「几何表重建 + 逐块渲染 + 解析缓存 + hover + 活动块特判」四合一路径 | `markdown_module.cpp:drawBlockFlow` | 每加视图态/交互就加参数 + isActive 分支；post-build 回改元素带隐式顺序 | 活动块特判收成一个注入点（可选 hook），预览态传空 | 待排 |
| PAB-03 | 中 | maintainability/arch | 托盘动作 → 页面/模块映射存在 3 张平行表，其中 2 张在无测试的 main.cpp | `src/app_assembly.cpp:pageForTrayAction`、`src/main.cpp:executeTrayAction` | 漏改任一处 = 「点了没反应」或置灰与实际相反，难复现 | `executeTrayAction` 改调 `pageForTrayAction` + 两个特例（并入 R8） | 待排 |
| PAB-04 | 中 | testability/test | 启动顺序契约只由注释与语句位置保证，三段 static 初始化 lambda 与 pet 开窗顺序全在无测试的 main.cpp | `src/main.cpp:dslAppConfig::themeRestored/initialWindow` | 顺序错不编译失败、不挂测试，只静默降级（标题栏不跟随主题、日志丢行） | 收进可测 `app_boot.{h,cpp}` + 3 条顺序用例（并入 R8） | 待排（专家 M2 未覆盖部分） |
| PAB-05 | 中 | maintainability/arch | `petCompose` 单函数 176 行，气泡三级内容源调度概率内联在 onTimer 闭包（不可测），注释与代码数学不符 | `src/module_pet/pet_window.cpp:petCompose`、onTimer 内 roll 调度 | 调行为只能改代码 + 肉眼；注释写 60s、代码实为 100s | 调度抽成 `pet_bubble.h` 纯函数（dt+状态+roll → 文案来源/停留） | 待排 |
| PAB-06 | 中 | testability/arch | 「单 exe 零外部 DLL」无自动守护：脚本不查产物 DLL，`DEVDESK_STATIC` 可被单点关掉而无提示 | `scripts/build-test-run.sh:构建段`、`CMakeLists.txt:option(DEVDESK_STATIC)` | 发布承诺（拷贝到无 MSYS2 机器能跑）只在发版前靠人肉 objdump 维持 | 构建后 `objdump -p` 取 DLL 集与白名单求差，非空 exit 1 | 待排 |
| THS-01 | 中 | architecture/arch | settings 渲染路径仍有 4 处文件系统调用：H2 刚立的「compose 不做 IO」在同文件内被自己违反 | `settings_module.cpp:SettingsModule::compose`、`drawProxyCard` | 每帧 stat/getcwd 与目录扫描；H2 硬约束退化为注释 | 状态并入快照刷新（后台读一次 → 成员 bool/list），compose 只读 | 待排（专家 H2 未覆盖部分） |
| THS-03 + PAB-07 | 中 | correctness/cpp | M3 只修了 config.json：todo/md 视图状态、pet 配置与皮肤仍是截断原地写、解析失败静默回默认 | `todo_view_state.cpp:TodoViewStateStore::save`、`pet_config.cpp:PetConfigStore::save` | 写窗口内崩溃 → 皮肤/列滚动位置静默丢；同类修复每次要人工逐文件排查 | 4 份拷贝改调 `core::atomicWriteFile` | 已修（`cf5edf7`/`a9d3116`+`8401b5a`；`63a6d64`/`e1f173b`） |
| THS-04 | 中 | architecture/arch | Home 跨模块直读他人表，「逾期」这一条业务规则在仓内有 3 份各自维护的实现 | `home_service.cpp:HomeService::getStats`、`todo_service.cpp:TodoService::listOverdue` | 口径调整或列改名后首页与 Todo 页给出不同数字，无从判断谁对 | 「逾期」谓词/SQL 片段收进 core，Home 与 Todo 共用 + 跨模块一致性用例 | 待排 |
| THS-05 | 中 | pattern/arch | 「谁在最上层」无单一持有者：9 个 bool + 手写关闭顺序 + 手挑 zIndex（settings 侧 7 个下拉 bool 同构） | `todo_module.cpp:closeTopmostPopup`、弹窗状态成员块 | 新增弹窗要同时改三处且顺序要与 z 序一致，漏改静默降级 | todo 用 `openStack_`，settings 收成一个 `openDropdown_ = -1` | 待排 |
| THS-06 | 中 | correctness/cpp | 4 处次生失败被吞：用户看到「任务已完成」但卡片没移动；逾期列表失败被改写成「逾期（0）」 | `todo_module.cpp:doCompleteTask/doUncompleteTask`、`openOverdueDialog` | 伪成功与伪空都无排障线索 | 「状态迁移 + 列迁移」合成一次可失败操作；查询失败走 `setStatus(错误,true)` | 待排 |
| TIN-03 | 中 | correctness/test | 运行器统计口径三处失真：0 用例命中=退出码 0 且判「全绿」、`[SKIP]` 计入 Passed、filter 参数无防护 | `tests/test_main.cpp:main()`、`scripts/build-test-run.sh:FILTER` | 「未运行测试=未完成」的纪律可被工具静默绕过（假绿） | total==0 返非 0；判绿加 total>0；`[SKIP]` 单列 | 已修（`e7eab8c`/`fc8eda1`） |
| TIN-04 | 中 | maintainability/test | 墙钟红线同类残留两处（去 flake 那批只删了两处绝对阈值） | `tests/test_modules/test_md_block_model.cpp:DOCTEST_CHECK_MESSAGE(warmUs < 8000)`、`test_md_preview_highlight.cpp` | Debug + 并发编译负载下随机翻红，红一次要人肉重跑整轮 | 照该批做法：printf 记实测 + 换非时间断言 | 待排（`warmUs < 8000` 现仍在） |
| TIN-05 | 中 | maintainability/test | 文件头声明覆盖 `trayMenuSpecFiltered`，实际零用例（该函数是原生托盘降级路径） | `tests/test_modules/test_tray_dynamic.cpp:文件头 vs REGISTER_TEST 清单` | 注释让人以为有护栏；模块关掉时菜单项怎么消失零覆盖 | 补 1 条 headless 用例（全 false → 移除项/顺序/分隔线合并） | 待排 |
| CP-03 | 中 | architecture/arch | `src/` 唯一平台宏回流：`http_client.cpp` 用 `#if defined(_WIN32)` 选 CA 策略 | `src/platform/http_client.cpp:applyCaOptions` | 「业务代码零平台宏」的机械验收失效；后续 TLS 差异会照抄就地加 `#if` | 抽 `tls_ca_win.cpp` / `tls_ca_posix.cpp`，CMake 按平台选源 | 已修（`5ffa2a2`/`0b60c49`） |
| CP-09 | 低 | maintainability/cpp | 平台实现族内部逐字重复且缺公共落点：`shellSingleQuote` ×2、UTF-8↔UTF-16 ×2（已漂移）、claudeSettingsFile 环境分支 ×3 | `src/platform/file_open_macos.cpp:shellSingleQuote`、`file_open_posix.cpp:shellSingleQuote` | 修一处 bug 要在 2~3 文件同步；重复已造成实际漂移（file_watcher_win 缺 wlen 校验） | 公共 `file_open_common.cpp` 承载帮助函数；env 分支下沉 `paths.cpp` | 待排 |
| CP-10 | 低 | deadcode/arch | 两处公开 API 无消费者/无效果：`SqliteDb::setSchemaVersion()` 空实现、`ResponseWriter::ended_/end()/ended()` 写而不用 | `src/core/sqlite_db.cpp:setSchemaVersion`、`http_server.h:ResponseWriter` | 「公开且有注释」被当成可用能力，真用一次就掉坑返工 | 删除两者（写版本号若需要，做成 runMigrations 内私有步骤） | 待排 |
| CP-11 | 低 | security/security | keychain(win) `sanitize()` 把非白名单字符统一替换 `_` → 不同 account 可能折叠到同一密钥文件，且密钥文件非原子截断写 | `src/platform/keychain_win.cpp:sanitize`、`keyFilePath` | 安全性靠命名习惯维持；截断写崩溃后 load 报 Internal 无法区分「没配」与「损坏」 | account 文件名可逆编码 + 落盘改 tmp+rename | 待排 |
| CP-12 | 低 | architecture/arch | 平台族形状不一致：`system_theme` 没有 `*_macos` 实现，macOS 上「跟随系统主题」恒解析为浅色 | `src/platform/CMakeLists.txt:APPLE 分支`、`system_theme_posix.cpp:prefersDarkTheme` | 声明支持的平台静默失效，无日志；补 macOS 时要在 CMake 加分支而非加文件 | 补 `system_theme_macos.cpp`；补齐前把文案/注释改成「仅 Windows」 | 待排（需拍板） |
| CPX-11 | 低 | maintainability/arch | 同模块两套同构的 JSON 顶层字段扫描器（约 130 行）+ 4 个重复小工具（trim/toLowerAscii/stripTrailingSlash 各 2 份） | `streaming_proxy.cpp:findTopLevelField`、`upstream_dialect.cpp:findTopLevelStringSpan` | JSON 边界处理是易错面，两处实现不同步只会在某种 payload 下静默分歧 | 抽模块内私有 `json_scan.h` 承载扫描器与小工具 | 待排 |
| CPX-12 | 低 | maintainability/arch | 托管键集合唯一来源是 `buildProxyEnv`（动态），上层覆盖检测另维护一份 8 键硬编码清单 | `settings_sync.cpp:conflictingEnvKeys`、`provider_config.cpp:buildProxyEnv` | 自己写进去的键（`*_MODEL_NAME`、extra_env）反而检测不到，覆盖警告漏报 | 检测键集改为 `buildProxyEnv(空 profile, 端口)` ∪ extra_env 动态派生 | 待排 |
| CPX-13 | 低 | architecture/arch | 上游方言只能按 baseUrl host 后缀推断，Profile 上无覆盖入口 | `upstream_dialect.h:forBaseUrl`、`upstream_dialect.cpp:isOpencodeHost` | 自建网关/内网镜像方言丢失（UA 403 / `[1M]` 400），用户无法配置只能改代码 | Profile 加可空 `dialectOverride`，`forBaseUrl` 加可选覆盖参数 | 待排（需拍板） |
| CPX-14 | 低 | testability/test | keychain 是自由函数、无注入点 → 多步写协议的失败分支无法单测 | `src/platform/keychain.h:storeSecret/loadSecret/deleteSecret` | CPX-07 的三种补偿策略、CPX-02 的失败可见性无法用测试钉住 | 给 keychain 加一层可选注入（函数指针/薄接口），补失败用例 | 待排 |
| CPX-15 | 低 | architecture/arch | `settings_sync` 一个文件承载方向相反的两套协议，并吃 service 层 DB 行模型 Profile | `settings_sync.h:文件头（接管/还原）`、`SettingsEnvRead` | 一条守卫语义要横跨三层才读懂（HANDOFF §9.4.2 的 G8/G9 不可区分即其副产物） | takeOver 入参换小视图结构；头里写明「settings.json 唯一 IO 边界」契约 | 待排 |
| MD-05 | 低 | pattern/oop | 实时态状态位群语义隐式、复位点分散：`liveCursorValid_` 伪三态、`liveFocusPending_` 消费即弃、`liveOffset_` 漏复位 | `markdown_module.cpp:成员区`、`MarkdownModule::setMode` | 新增进入实时态的路径都要手补复位；`liveOffset_` 已漏（换笔记残留滚动位置） | 收 `enterLiveState(reason)` / LiveState.reset()，三条路径统一调 | 待排 |
| MD-06 | 低 | maintainability/cpp | 编辑器 input 的 id 是 13 处散落字面量，而新加的实时 id 反而有常量 | `markdown_module.cpp:"md.editor.input"` 字面量群、`kEditorInputHitId` | 写错一个字符不编译报错、只静默失效（M0 spike C1 的事故形态） | 补 `kEditorInputId` 常量并替换 13 处（测试侧 46 处同批） | 待排 |
| MD-07 | 低 | maintainability/cpp | 同一上游耦合系数 1.2（input 内部行高）有两份定义，两份注释都写「必须与上游同步」 | `markdown_module.cpp:kEditorLineHeightFactor`、`md_live_edit.cpp:kMdLiveLineHeightFactor` | 上游调系数时改一处忘另一处：双栏同步换算或贴合高度偏掉（违反 C4） | 系数提 `md_live_edit.h` 常量，另一处 include 复用 | 待排 |
| MD-08 | 低 | correctness/cpp | 索引写两条路径锁不对称：UI 线程 `write()/create()/remove()` 不持 `rescanMu_`，watcher 线程持锁做同一批索引写 | `markdown_service.cpp:MarkdownService::write`、`onWatchEvent` | 自动保存与 watcher 事件天然并发 → 瞬态索引/FTS 不一致（搜索可见），下轮 rescan 收敛 | 两条调用收进持锁段（或私有方法内自取锁，注意非递归 mutex） | 待排（未实证复现节拍） |
| PAB-08 | 低 | pattern/arch | 内置皮肤位图合成器约 490 行（自研 PNG/zlib 编码器 + 光栅器）只为 2 款占位皮肤，`assets/pets` 现为空 | `src/module_pet/pet_skin.cpp:crc32Of/appendPngChunk/writePngRgba`、`Canvas` | 美术/动作微调要改 C++ 重编译；维护面放大到本模块 2/3 | 帧一次性生成为 `assets/pets/*` 资产随仓库分发，删生成器 | 待排（需拍板） |
| PAB-09 | 低 | maintainability/arch | 「挂机穿透」删除后的陈旧注释与用例名残留（含与断言不符的用例标题） | `pet_window.cpp:文件头注释`、`PetUiState 注释` | 菜单下标算术靠这些顺序注释推理，误导后续调整；用例名与断言不符 | 一次性清理注释/用例名 + `app_sidebar.cpp` 宽度数字 | 待排 |
| PAB-10 | 低 | maintainability/cpp | `-Werror` 只在 Debug 生效，发布档（Release）与默认 configure 都不带 | `cmake/CompilerWarnings.cmake:devdesk_apply_warnings` | 只在 -O2/-O3 出现的告警永不失败；`cmake -B build` 默认就是 Release | 两档都加 `-Werror`（保留现有白名单），或发布前跑一次 Release 构建 | 待排（需拍板） |
| PAB-11 | 低 | maintainability/arch | 构建脚本与 CMake 的 zlib/工具链口径相反（脚本传 DLL 导入库并压掉静态默认） | `scripts/build-test-run.sh:--clean 传 -DZLIB_LIBRARY/-DOPENSSL_ROOT_DIR`、`cmake/Dependencies.cmake` | 无法从任一处判断发布物链的是哪个 zlib；历史 workaround 残留 | 删脚本里的三处 `-D`，只留 PATH；确需覆盖则与 `DEVDESK_STATIC` 同源 | 待排（是否真影响最终 exe 未取证） |
| PAB-12 | 低 | correctness/cpp | 桌宠的「随机」从未播种：全仓无 srand，`std::rand()` 直接使用 | `pet_window.cpp:pickPhrase`、onTimer 内 roll | 每次启动同一短语/同一闲聊序列；`% size` 有低位偏置 | 匿名命名空间放 `std::mt19937& rng()`，改用 uniform 分布 | 待排 |
| PAB-13 | 低 | pattern/arch | 「隐藏桌宠（本次会话）」会话内不可逆：菜单/设置页都没有重新显示入口 | `pet_window.cpp:executePetMenuSelection（Hide 分支）`、`applyPetSkin` | 误点一次要重启应用；换肤只在窗口开着时热切换 | 设置页加「显示桌宠」按钮（或菜单项随状态切换） | 待排 |
| THS-07 | 低 | pattern/cpp | `nowEpochSeconds()` 恒成功却返回 `Result`，迫使 8 个写方法各写 3 行不可达错误分支 | `todo_service.cpp:TodoService::nowEpochSeconds` | 真实失败路径与仪式分支形状相同，审阅无法用形状区分；噪声按调用点线性增长 | 签名改回 `int64_t`，8 处各删 3 行 | 待排 |
| THS-09 | 低 | oop/cpp | 拖拽命中的几何输入 `boardCardW_` 由渲染副作用写入（画卡片时赋值），命中结果依赖「本帧画过什么」 | `todo_module.cpp:drawCard`、`drawBoard`、`hitDropColumn` | 渲染侧演进（自适应卡宽/虚拟化/空列）会静默改变命中判定 | 板面几何收成值对象，由 drawBoard 一次算好只读传递 | 待排（dpi 探针已随 `13e31fb` 收口） |
| THS-10 | 低 | maintainability/arch | `HomeService::countBySql` 两个重载 90% 重复（唯一差别是有无 bind） | `home_service.cpp:countBySql(const char*)`、`countBySql(const char*, int64_t)` | 25 行抄两份，改一处必漏另一处（错误文案开始各自漂移） | 合成一个带 `std::optional<int64_t>` 的重载 | 待排 |
| THS-11 | 低 | deadcode/cpp | 公共头承诺的 `NotFound` 语义未实现，注释承诺的 `changes()` 校验也不存在 | `todo_service.h:renameColumn 文档注释`、`todo_service.cpp:renameColumn` | 调用方会照头文件写依赖 NotFound 的分支；UI 把「改了不存在的 id」当成功 | 二选一：删头里两处承诺，或实现 `SELECT changes()` 判 0 行 | 待排（需拍板） |
| THS-12 | 低 | pattern/cpp | `reorderColumns` 为 schema 里不存在的唯一约束写了两段式防冲突（负位中间态） | `todo_service.cpp:TodoService::reorderColumns`、`migrations.h:todo_columns` | 防御不存在的约束，让低频逻辑背 2n 条语句；注释把不成立的前提写成事实 | 二选一：补 `(position)` 唯一索引，或删负位段并改注释 | 待排（需拍板） |
| THS-13 | 低 | correctness/cpp | 逾期弹窗可显示「逾期 0 天」：入选按秒级、显示按日历日 | `todo_module.cpp:drawOverdueDialog`、`todo_ui_logic.h:ui::overdueDays` | 入口写「逾期 1」点进去看到 0 天；跨午夜更常见 | 显示侧 `max(1, overdueDays)`，或入选条件也改日历日口径 | 待排（需拍板） |
| THS-14 | 低 | maintainability/arch | 「已归档」列语义靠标题字符串识别，与同文件刚确立的「不按标题找」原则冲突 | `todo_module.cpp:drawColumnContent`、`findColumnByKind 注释` | 用户重命名列后空态文案/归档特判静默失效 | 迁移增补 `kind='archive'`（同 v4 手法），UI 改按 kind | 待排（需拍板） |
| THS-15 | 低 | deadcode/arch | 「为什么不把原子写下沉 core」的唯一书面依据引用了不存在的登记项（`KNOWN_LIMITATIONS #16` 实为另一件事） | `settings_config.cpp:atomicWriteConfig 注释`（现版本已随重构消失）、`markdown_service.cpp:atomicWriteFile 注释` | 原语被抄到第 4、5 份时，唯一解释「为何允许复制」的记录指向空处 | 补一条真实条目并让残留注释指向它；已下沉则删注释 | 部分已修（`cf5edf7` 落 `core::atomicWriteFile` 后 config 侧引用消失；`markdown_service.cpp` 仍引用） |
| THS-16 | 低 | correctness/cpp | 「昨天」判定用相邻午夜秒差 /86400，在有夏令时的时区会错一天（春季 23 小时 → 显示「23 小时前」） | `home_service.cpp:formatRelativeTime`、`todo_ui_logic.h:ymdToEpoch` | 只在有 DST 的时区体现，当前开发机与常规测试跑不出来 | 改用日历日序号比较（`days_from_civil` 上收 core 复用） | 待排 |
| THS-17 | 低 | maintainability/arch | Priority 的四个映射函数散在 UI 与服务两文件，且都带 default 掩盖新枚举 | `todo_module.cpp:priorityLabel/priorityColor`、`todo_service.cpp:intToPriority/priorityToInt` | 新增档位时 `-Wswitch` 不报警，静默落成「无」+ 灰 + 0 | 文案/颜色收成按枚举索引的单一映射表，switch 去 default | 待排 |
| THS-18 | 低 | pattern/arch | 「同列内 position 连续」只由 `moveTask` 维护，而 `updateTask` 能同时改 `column_id` 与 `position` | `todo_service.cpp:TodoService::updateTask`、`todo_module.cpp:collectDetailFormTask` | 复用 updateTask 改列会静默留下位置空洞（顺序错乱、追加位置偏大） | updateTask 只改内容字段，改列一律走 moveTask | 待排 |
| TIN-08 | 低 | oop/test | 自制框架宏约束反向塑造用例写法：`REGISTER_TEST` 的 body 是宏实参，裸逗号被切参（18 处规避注释） | `tests/doctest.h:REGISTER_TEST` | 断言体被拆到宏外、期望值表搬到文件作用域；一条用例要在两处跳 | `REGISTER_TEST` 改可变参宏；`CHECK_EQ` 的 auto 改 `const auto&` | 待排 |
| TIN-09 | 低 | maintainability/test | 用例文件归属错位：`test_todo.cpp` 装的是 `core::PageId` 枚举契约（与 todo 无关） | `tests/test_modules/test_todo.cpp:todo_pageid`、`src/core/module_interface.h:PageId` | 找 PageId 契约的人不会翻 test_todo.cpp；grep 模块名找不到它 | 用例挪进 `tests/test_core/`，并在 PageId 处留指针注释 | 待排 |
| TIN-10 | 低 | architecture/test | 测试目标构建接线三处结构债：手工枚举 53 个用例文件、只开两个模块私有头、生产 TU 双份编译且选项不同 | `tests/CMakeLists.txt:add_executable(devdesk_tests ...)` | 新用例文件漏进清单就永不编译、永不运行且无提示 | 顶部加 GLOB 计数自检 FATAL_ERROR；app 侧抽 `devdesk_app_support` 库 | 待排 |
| TIN-11 | 低 | maintainability/test | `platform::http_client` 的测试文件只回显结构体默认值，错误路径/超时/TLS 语义零直测 | `tests/test_platform/test_http_client.cpp:http_build_request` | H1 就落在 http_client.cpp，改动只能靠间接用例碰运气 | 补 2~3 条直测（连不存在端口、小 timeout 打慢上游、readChunk Error 终态） | 待排 |
| TIN-12 | 低 | maintainability/test | 用例总数写死在文档里且与静态登记数不符（静态 561 vs 文档 530） | `CLAUDE.md:「共 530 个用例」`、`tests/:REGISTER_TEST 计数` | 数字既漂移又无法自证；每次加用例都要人肉改文档 | 改为「以运行输出 Total 为准」不写死数字 | 待排（差额未解释） |
| TIN-13 | 低 | maintainability/test | 一致性检查脚本仍是 macOS-only（screencapture / open -a / python3），Windows 主平台不可用 | `scripts/check-consistency.sh:文件头前置条件` | 唯一的「HTML demo ↔ C++ 渲染」视觉闸门在迁移后等于没有 | 文件头写明 macOS-only（或归档 docs/），Windows 走人工截屏 | 待排（已登记） |

## 3. Top 10 优先项

| # | 编号 | 为什么现在做 | 依赖 / 前置 |
|---|---|---|---|
| 1 | **MD-01**（高） | 实时态 M2 已合 master 且可进入 → 现网可触发丢字/跑位；M3 正要动 setLiveCursor，**同批做避免二次返工** | Phase3 M3 spec（`2026-09-27-markdown-phase3-block-render-design.md`）；文件 `markdown_module.cpp` |
| 2 | **CPX-01**（高） | 33 方法 / 7 职责 / 1409 行，是"加能力只能继续堆"的源头；设计（R10）已就绪 | god-class spec Q1–Q3 拍板；profile 反向同步已合（前置满足） |
| 3 | **THS-02 + CPX-05**（中） | 两个最大 UI God 类（2474 / 863 行）；R4/R5 的手法一旦跑通，其余模块可复制 | R1–R3（`ui_kit` 基座）；THS-02 另需 `todo_module.cpp` 单写者 |
| 4 | **TIN-01**（高） | R0 一旦落地删 `#include "claude_service.h"`，这条守卫**必红并被误判为回归** | R0 同批；改 `tests/test_settings.cpp` + 新增 `scripts/check-module-isolation.sh` |
| 5 | **THS-06**（中） | 唯一"用户可见的错"：伪成功（提示已完成但卡片没动）与伪空（逾期 0）；修法局部 | `todo_module.cpp`（与 THS-05/THS-13 同文件 → 排同一条线） |
| 6 | **CPX-09**（中） | 导入 cc-switch 配置的**主路径噪音**（240 个官方键误报），用户直接可见；修法是删一份手写清单 | `provider_config.cpp` 单文件 + `env_var_hints.h` 只读 |
| 7 | **THS-01**（中） | H2 硬约束在同一文件内被自己违反，每帧 stat/getcwd（含目录扫描）；不修则 H2 只剩注释 | 与 H2 第二批同批；`settings_module.cpp` 单写者 |
| 8 | **TIN-02 残 + TIN-07 + CP-04**（高/中） | 三者同一根因面（测试隔离无中心入口 + 进程级全局手工登记），并发 worktree 下的随机红归因成本最高 | 一条测试基建线（`tests/doctest.h`/`test_main.cpp`/`test_support.h` + `paths.cpp`） |
| 9 | **PAB-02 + THS-08**（中） | 模块插拔是产品卖点，接入成本 5~7 处直接决定第 4~7 个模块的演进速度 | R8（`main.cpp` + `app_assembly.*` 单写者）；需 `ModuleSet` 类型变更同步改测试 |
| 10 | **TIN-10**（低但便宜） | 用例文件漏进 CMakeLists = 永不编译且零提示（用例数 481→514→530 多次漂移说明手工同步会掉队） | `tests/CMakeLists.txt` 单文件；可即刻做 |

> 备选（同档但让位）：`CP-06`（HTTP 头大小写）、`PAB-06`（零外部 DLL 无守护，5 行脚本）、`MD-08`（索引写锁不对称，未实证节拍）。

## 4. 建议的三波修复计划

**硬约束自查**：每一波内按"线"划分，**线内串行、线间文件不重叠**；跨波同文件允许（波按序推进）。

### 波 1：干净收口（无设计前置，可即刻并行）

| 线 | findings | 涉及文件 |
|---|---|---|
| 1a 托盘线 | CP-05、TIN-05 | `src/core/tray_menu_spec.h`、`src/main.cpp`、`tests/test_modules/test_tray_dynamic.cpp`、`HANDOFF.md` |
| 1b platform 拆分线 | CP-09、CP-12 | `src/platform/{file_open_common.cpp(新), file_open_*.cpp, file_watcher_win.cpp, system_theme_*.cpp}`、`src/platform/CMakeLists.txt`、`src/core/paths.cpp` |
| 1c http/db 线 | CP-06、CP-10 | `src/platform/http_server.cpp`、`src/core/sqlite_db.{h,cpp}` |
| 1d proxy 内部收口线 | CPX-09、CPX-11、CPX-12 | `src/module_claude_proxy/{provider_config.cpp, settings_sync.cpp, streaming_proxy.cpp, upstream_dialect.cpp, json_scan.h(新), CMakeLists.txt}` |
| 1e todo 服务线 | THS-07、THS-11、THS-12、THS-14、THS-17、THS-18 | `src/module_todo/{todo_service.{h,cpp}, todo_module.cpp, todo_ui_logic.h}`、`src/core/migrations.h` |
| 1f home 线 | THS-10、THS-16 | `src/module_home/home_service.cpp` |
| 1g pet 线 | PAB-09、PAB-12、PAB-13 | `src/module_pet/pet_window.cpp`、`src/module_settings/settings_module.cpp`、`src/app_sidebar.cpp`、`tests/test_modules/test_pet.cpp` |
| 1h markdown 收口线 | MD-06、MD-07、THS-15 | `src/module_markdown/{markdown_module.cpp, md_live_edit.{h,cpp}, markdown_service.cpp}`、`docs/KNOWN_LIMITATIONS.md`、`tests/test_modules/test_markdown.cpp` |
| 1i 构建脚本线 | PAB-06、PAB-11 | `scripts/build-test-run.sh` |
| 1j 测试基建线 1 | TIN-04、TIN-09、TIN-11、TIN-12、TIN-13 | `tests/test_modules/test_md_block_model.cpp`、`test_md_preview_highlight.cpp`、`tests/test_todo.cpp`→`tests/test_core/`、`tests/test_platform/test_http_client.cpp`、`CLAUDE.md`、`scripts/check-consistency.sh` |

### 波 2：结构性收口（各需小设计 / 行为等价性自证）

| 线 | findings | 涉及文件 |
|---|---|---|
| 2a markdown 交互线 | MD-01、MD-02、MD-03、MD-05 | `src/module_markdown/markdown_module.cpp`（与 Phase3 M3 同批） |
| 2b markdown 索引线 | MD-08 | `src/module_markdown/markdown_service.cpp` |
| 2c claude 服务线 | CPX-02 残、CPX-07、CPX-14、CPX-15 | `src/module_claude_proxy/{claude_service.{h,cpp}, settings_sync.{h,cpp}}`、`src/platform/keychain.h`、`tests/test_modules/test_claude_service.cpp` |
| 2d claude UI/配置线 | CPX-06、CPX-08、CPX-13 | `src/module_claude_proxy/{claude_module.cpp, provider_config.{h,cpp}}` |
| 2e 转发路径线 | CPX-10 | `src/module_claude_proxy/proxy_router.cpp` |
| 2f todo/home 服务线 | THS-04、THS-06 | `src/core/migrations.h`、`src/module_home/home_service.cpp`、`src/module_todo/{todo_service.cpp, todo_module.cpp}` |
| 2g settings 线 | THS-01、THS-05（settings 侧） | `src/module_settings/settings_module.cpp` |
| 2h 测试基建线 2 | TIN-01、TIN-02 残、TIN-06、TIN-07、CP-04 | `tests/{doctest.h, test_main.cpp, test_support.h(新), CMakeLists.txt}`、`tests/test_modules/test_settings.cpp`、`scripts/check-module-isolation.sh(新)`、`src/core/paths.cpp`、三模块 getenv 分支 |
| 2i 构建告警线 | PAB-10 | `cmake/CompilerWarnings.cmake`（含一次 Release 档构建） |
| 2j pet 线 2 | PAB-05、PAB-08 | `src/module_pet/{pet_window.cpp, pet_skin.cpp, CMakeLists.txt}`、`assets/pets/` |

### 波 3：重构总表（按各自 spec，不另开设计）

| 线 | findings | 归并目标 |
|---|---|---|
| R0 跨模块耦合消解 | （TIN-01 已在 2h；R0 自身为前置） | R spec §5.2 R0 |
| R1 → R2 → R3（`ui_kit` 基座 / 对话框脚手架 / 数值收口） | — | R spec §4.1、§5.2 |
| R4 `claude_module` 拆分 ‖ R5 `todo_module` 拆分 | CPX-05、CPX-06、THS-02、THS-05（todo 侧）、THS-09、THS-17/18 追加项 | god-class §3、§4 |
| R6 `settings_module` 拆分 | THS-01/THS-05 若未在波 2 完 | R spec §5.2 R6 |
| R7 `markdown_module` 拆分 | MD-02、MD-03、MD-04、MD-06、MD-07 | R spec §4.2 表 + god-class §5「到此为止」 |
| R8 装配层收口 | PAB-02、PAB-03、PAB-04、THS-08、CP-05（托盘表） | god-class §5.1 |
| R10 ClaudeService 拆分 | CPX-01（+CPX-07/CPX-14 的收尾） | god-class §2 |
| 收尾独立线 | CP-08（`Result` 取值口收敛，跨模块机械替换） | 无 spec，需自写 grep 断言 + 一致性用例 |

> `R9 module_pet` 已按用户决策放弃（HANDOFF §4），pet 项改走 1g / 2j 两条独立线。

## 5. 与既有重构路线的归并

- **应并入 R0–R9 / R10（不另立）**：CPX-01→R10；CPX-05→R4；THS-02/THS-05(todo)/THS-09→R5；PAB-02/PAB-03/PAB-04→R8；THS-08→R8；MD-02/MD-03/MD-04/MD-06/MD-07→R7；CP-05→R8（托盘表合一）。
- **应并入 Phase3 spec（不属 R 线）**：MD-01→M3（setLiveCursor）、MD-05→M3/M6、MD-08→spec §4.7 相邻条目。
- **必须另开（现无归属）**：`CP-04`（测试隔离中心入口）、`CP-06`/`CP-09`/`CP-10`/`CP-11`/`CP-12`（core/platform 小收口）、`CPX-08`/`CPX-09`/`CPX-12`/`CPX-13`（provider 配置表驱动，建议并入 `2026-09-27-shared-config-and-failover-design.md` 的后续批）、`CPX-10`/`CPX-11`（claude_proxy 内部去重）、`CPX-07`/`CPX-14`/`CPX-15`（写协议与注入缝，R10 P-A2 前置）、`THS-04`/`THS-06`/`THS-11`~`THS-18`（todo/home 服务层小线）、`PAB-05`/`PAB-08`（pet，R9 已放弃）、全部 `TIN-*`（测试基建不在 R 线内）、`CP-08`（Result 取值口收敛）。
- **文档同步义务**：凡落地 1a（CP-05）、1i（PAB-06/11）、2h（TIN-01/02）者，必须同批改 `HANDOFF.md`/`CLAUDE.md`（这三处的旧描述已被本轮审计证伪）。

## 6. 需要用户拍板的清单（8 条）

| # | 决策 | 选项 | 推荐 |
|---|---|---|---|
| 1 | CP-05 托盘「按模块收缩」死代码 | ① 加回导航项复活谓词链 ② 删 disabled/谓词/后缀 + 改注释与文档 | **② 删**（托盘只管开/关，删完护栏与文档才有真值） |
| 2 | THS-12 `reorderColumns` 负位两段式 | ① 补 `(position)` 唯一索引保住前提 ② 删负位段、改注释 | **② 删**（当前无唯一约束，防御的是不存在的约束） |
| 3 | THS-14 归档列语义 | ① 迁移补 `kind='archive'` ② 放弃归档语义（删特判与文案） | **① 补 kind**（同 v4 手法，成本一档迁移） |
| 4 | THS-11 rename/update 的 NotFound 承诺 | ① 删头文件承诺（最小） ② 实现 `SELECT changes()` 校验 | **② 实现**（合约兑现，成本低） |
| 5 | THS-13 逾期天数口径 | ① 显示 `max(1, overdueDays)` ② 入选也改日历日 | **① max(1)**（最小改动、不动入选语义） |
| 6 | PAB-08 内置皮肤生成器（约 490 行） | ① 帧一次性生成为 `assets/pets/*` 资产，删编码器/光栅器 ② 保留 | **① 转资产**（美术可改、模块瘦身；需一次冒烟） |
| 7 | PAB-10 `-Werror` 覆盖面 | ① Debug+Release 两档都开 ② 只在发布脚本额外跑一次 Release 构建 | **① 两档都开**（保留现有 `-Wno-error=` 白名单） |
| 8 | macOS 侧能力/工具声明不一致（CP-12 + TIN-13） | ① 补 `system_theme_macos.cpp` + Windows 化 check-consistency ② 明确标注「仅 Windows 支持」 | **② 标注**（补齐不在当前 Windows 周期；先把静默承诺去掉） |

> 另两条可直接按 finding 推荐执行、无需拍板但需排期：CPX-13（Profile 加 `dialectOverride`，UI 暂不暴露）、PAB-11（删脚本三处 `-D`）。

## 7. 存疑 / 未取证项归拢

| # | 来源 | 存疑点 | 现状 |
|---|---|---|---|
| 1 | CP 片 open_questions ① | `FileWatcher::stop()` 在回调线程内调用会走 `detach()` 分支后立即 CloseHandle，分离的 worker 可能仍在用这两个句柄 | 当前 markdown_service 的回调不 stop → 非现网缺陷；未列 finding |
| 2 | CP 片 open_questions ② | `Server::start()` 依赖 `running_`（普通 bool）+ `listenStarted`（atomic）双标志；同实例 stop→start 未验证 | 唯一调用方每次新建 Server，未观察到此路径；未列缺陷 |
| 3 | CP 片 open_questions ③ | `json.cpp` 的 2 次深拷贝开销（每次写 settings.json 都走） | KB 级不可测；导入路径已设 1MB 上限 |
| 4 | PAB-11 自陈 | 脚本 `-D` 与 CMake 默认相反是否真影响最终 exe（`png_static` 是否进 eui_neo 链接线未确认） | 配置一致性问题成立，产物影响**未取证** |
| 5 | MD-08 自陈 | 索引写锁不对称的并发不一致：**未实证复现节拍**，仅静态可证「写点无锁」 | 未复现 |
| 6 | TIN-12 自陈 | 静态 `REGISTER_TEST` 561 与文档/实测 530 的**差额未解释** | 待一次运行输出对账（本次禁跑） |
| 7 | 专家审计 V2–V5 / V7 / V8 | dpi 三档实拖 / compose IO 竞争耗时 / 各对话框 Esc 清单 / 三模块全关版式 / `applyPetSkin` 时序 / `estimateTextWidth` 偏差 | 均未取证；其中 V2 相关 dpi 已随 `13e31fb`（经 `5ffa2a2` 合入）收口 |
| 8 | HANDOFF §9.4.2 | `readSettingsEnvForSync` 复用既有 IO 助手后，「存在但读不出」（G8 应中止）与「0 字节空文件」（G9 应 no-op）**不可区分** | 已登记，未修 |
| 9 | HANDOFF §9.4.3 | `SqliteDb::operator=(SqliteDb&&)` 仍用 `sqlite3_close`、`Statement::columnCount()` 无锁 | 已登记，按范围纪律未动（CP-07/L5 之外） |
| 10 | HANDOFF §9.4.1 | C1：`requestFocus` 发基 id 而非 `<id>.hit` | **状态更新：已修**（`ec578c6`，经 `f47766b` 合入；正是 MD-06「id 字面量无常量」的后果之一） |
| 11 | 在飞实况（本轮核） | worktree `4e74966`（摘 EUI-NEO `-Os` workaround + json 序列化快照用例）**未合 master**；`13e31fb`/`459a5be`（todo dpi 改吃 `contentScale`）已随 `5ffa2a2` 合入 | 前者在飞，后者已落地 |
| 12 | 冲突核对 | 六片之间**未发现对同一处的相反结论**；仅有两条"口径张力"：MD-04（R7 估算区间被 +480 行打破）vs god-class §5「markdown_module.cpp 不加码」——后者是范围限制，不改前者的前提漂移判定；THS-12 注释声称的唯一约束 vs schema 现状（`migrations.h` 无 `(position)` 唯一索引）——已在表内标「待拍板」 | 无需仲裁，按 finding 建议执行 |
