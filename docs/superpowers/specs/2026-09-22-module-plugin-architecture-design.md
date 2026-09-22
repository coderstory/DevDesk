# 模块插拔架构 — 调研与设计

日期：2026-09-22 ｜ 状态：设计定稿（待评审） ｜ 关联 spec：§8（数据层）、§10.4（Claude 代理）、§6.5.2（侧边栏）

## 1. 需求与约束

用户需求（原话转述）：

1. 后续可能开发更多功能模块（当前 4 工具模块 + Home + Settings，未来估 8-10 个）
2. 用户可以**关闭某个模块**
3. 关闭后该模块的功能代码**不在任何场景执行**（不只是隐藏 UI）
4. 关闭后左侧菜单不展示该模块
5. 插件**可能**改成事件驱动模式（"可能"= 未定，非硬性需求）

硬约束（来自项目现状）：

- **单 exe 零外部 DLL**（T3 已达成、W2 验收过的发布形态，spec §16）
- **不抛 C++ 异常**，错误走 `Result<T>`（全局约定）
- `core` 不依赖 platform/EUI-NEO；`module_*` 之间不互相依赖（分层铁律）
- EUI-NEO 给 app target 强加 `-fno-exceptions -fno-rtti`（CMakeLists.txt:68-83）
- 用户不熟悉 C++，方案的确定性 > 架构先进性

**结论先行：推荐方案 A（编译期内置 + 运行时注册门控）。事件总线（方案 B）明确推迟，动态库插件（方案 C）明确不做。** 理由见 §4。

## 2. 现状调研：模块机制

### 2.1 Module 接口与 Router（插拔的底子已经是对的）

- `src/core/module_interface.h:30-62` — `Module` 纯虚接口：`pageId()/displayName()/iconGlyph()/compose()` + 默认空实现的 `onKeyEvent()/onPageEnter()/onStart()/onStop()`。模块是纯被动对象，**没有任何后台线程/定时器**——不注册就不会有任何代码路径碰到它。
- `src/core/router.cpp:10-12` — `registerModule()` 只是 `modules_.push_back()`；`startAll()`（:64-71）只对已注册模块调 `onStart()`；`compose()`（:53-62）只渲染当前页的已注册模块；侧边栏（main.cpp:296）遍历 `router->modules()` 渲染菜单。
- 装配在 `src/main.cpp:125-157` `getRouter()`：5 个 `create_*_module()` 工厂 + `registerModule` + `startAll`。
- 跨模块跳转 payload：`router.h:21` `navigateTo(page, payload)`，单槽一次性消费（`consumePendingPayload`，router.cpp:36-42）。

**关键判断：现有机制下"不注册 = 零执行 + 零菜单项"天然成立。** 模块所有代码入口都是 Router 驱动的（compose/onStart/onKeyEvent/onPageEnter），Router 不持有它就什么都不发生。方案 A 的改动量因此极小。

### 2.2 CMake 组织（加新模块的成本，已核实）

每个模块一个 static lib（如 `src/module_todo/CMakeLists.txt`，共 16 行）。加新模块的全部成本：

1. 新目录 + 16 行 CMakeLists（复制 module_todo 模板）
2. 顶层 `CMakeLists.txt:53-57` 加一行 `add_subdirectory`，`:84-105` 加 link + include 各一行
3. `module_interface.h:11-17` 的 `PageId` 枚举加一项（**枚举是 core 持有的封闭清单**，编译期内置方案下这是特性不是缺陷）
4. `main.cpp` 加工厂 extern（:23-27 处）+ `getRouter()` 里 new/register 两行（:127-141）

线性成本，无重构需求。新模块本身零跨模块依赖即可插。

## 3. 现状耦合清单（插拔的真正障碍，逐个查证）

### 3.1 Home 仪表盘直查其他模块的表（最硬障碍）

`src/module_home/home_service.cpp`：

| 位置 | 查询 | 依赖的表 |
|------|------|----------|
| :194 | 今日笔记 COUNT | `markdown_index` |
| :201 | 待办任务 COUNT | `todo_tasks` |
| :206-208 | 逾期任务 COUNT（口径抄 TodoService::listOverdue） | `todo_tasks` |
| :214 | 今日 API 调用 COUNT | `claude_call_logs` |
| :145 | 最近编辑 5 条 | `markdown_index` |

`home_service.h:5` 注释明说"不依赖任何 module_*"——**是代码依赖为零，但语义依赖是硬的**：Todo 关掉后表还在（见 3.5），SQL 不会报错，但数字是死的（没人写入了），显示出来是欺骗用户。

Home 还有导航耦合：5 张统计卡点击跳目标页（home_module.cpp:297/306/311/316/321）、最近编辑行点击带 `open:<路径>` payload 跳 Markdown（:396-398）、快捷按钮带 `action:new_note`/`action:new_task` payload（:457-459/:473-475）。目标模块关掉后这些跳转全部落空。

另有一个**已解决的反向依赖样板**：Token 速率卡读 `core::proxy_stats::snapshot()`（home_module.cpp:278，设计见 token-rate-card-design.md §2.5）——claude 模块关闭时 snapshot 自然是空闲态，零出错。这是"跨模块共享数据走 core 层静态"的正确先例。

### 3.2 Router 跳到未注册页 = 空白页

`router.cpp:60-62`：`compose()` 找不到当前页的模块 → 渲染"无页面"死文案。`navigateTo()` 本身不校验目标页是否注册（:14-34 直接 `current_ = page`）。模块关闭后任何残留的跳转入口（侧边栏外：Home 卡片、`main.cpp:57-68` `dispatchPageForFile` 的文件打开分发）都会把用户送进空白页。

### 3.3 Settings 模块 → module_claude_proxy 的真实代码依赖（唯一一处 module↔module）

`src/module_settings/settings_module.cpp`：

- :27 `#include "claude_service.h"`（注释 :22-24 自己承认"单向依赖 module_claude_proxy……无环"，是对分层铁律的注释豁免）
- :163-183 `doRegenerateToken()`：`ClaudeService::global()` → stopProxy + startProxy 重生成 token
- :448-520 代理卡片 compose 直读 `global()` 的运行状态/port/token
- :73-92 `writeProxyTokenFile` 与 main.cpp:196-215 **整段复制**（两边都是匿名命名空间没法复用）

这是全代码库唯一 module↔module include。claude 模块关闭时 `global()` 返回 null（main 不再创建 service），Settings 代理卡已处理 null（"Claude 服务不可用"），但正确行为应该是**整卡隐藏**而不是显示故障态。

### 3.4 Claude 代理生命周期横跨 main.cpp（启动顺序耦合）

- main.cpp:183-193 `getClaudeService()`：单例创建 + `setGlobal`
- main.cpp:218-242 `startClaudeProxy()`：bind 127.0.0.1:8888 + 写 proxy_token.txt
- main.cpp:398-403 启动顺序硬约定：`startClaudeProxy()` **必须**在 `getRouter()` 之前（W3 修复 50ff663 的成果：模块 onStart 时 `global()` 非 null，启动零 WARN）
- claude_module.cpp:724-743 `onStart` 只是"已在运行"兜底检查
- **30 天日志清理**：main.cpp:102-119，在 `openAppDatabase()` 里按 settings 配置 `pruneCallLogs`。归属是 app 级（不是 claude 模块驱动），claude 模块关闭它照跑——清理的是 DB 行，无害，且 re-enable 时数据还在。**不列为障碍。**
- **markdown_index 维护**：`MarkdownService::rescan`（markdown_service.cpp:34 声明，onStart 调用，markdown_module.cpp:128）。markdown 模块关闭 → 无人 rescan → `markdown_index` 变陈旧。但因为方案 A 会隐藏 Home 的笔记卡（3.1），陈旧数据没有消费方，无害。

### 3.5 DB migration：全局 migrations.h（是助力不是障碍）

`src/core/migrations.h`：7 张表全在 v1（:24-123），v2 播种默认 Todo 列（:128-138）。**模块关闭后表照建、数据照留**——这恰好是插拔想要的语义：

- 关闭 = 不执行代码，**绝不删数据**（删数据对"可插拔"是灾难）
- 重新启用 = 数据原样回来，零迁移成本
- 唯一小瑕疵：v2 的 Todo 默认列播种是模块数据语义混进了全局 migration，但 ON CONFLICT 幂等 + 表存在即无害，不动。

**决策：migration 维持 app 级全局，不按模块拆分。** 拆分（按 enabledModules 选择性迁移）会制造 schema 版本分叉——用户交替开关模块后 DB 状态空间爆炸，收益为零。

### 3.6 耦合清单小结（障碍排序）

| # | 障碍 | 位置 | 硬度 |
|---|------|------|------|
| 1 | Home 直查三模块表 + 卡片/按钮导航 | home_service.cpp:145-231、home_module.cpp:297-488 | **最硬**：需要注册感知的降级渲染，是方案 A 里最大的单点改动 |
| 2 | Claude 代理生命周期 + Settings↔claude 依赖 | main.cpp:183-242/398-403、settings_module.cpp:27/163-183/448-520 | **硬**：main 启动顺序门控 + Settings 卡片隐藏 + （顺手）消掉唯一 module↔module include |
| 3 | Router 空白页 + 文件打开分发 | router.cpp:60-62、main.cpp:57-68 | 中：加 fallback 语义，改动小但必须做，否则用户会看到"无页面" |
| 4 | config 模型 | settings_config.h:36-43 | 低：加一个字段 |
| 5 | migration 全局 | migrations.h | 非障碍，保持现状 |

## 4. 方案对比

### 方案 A：编译期内置 + 运行时注册门控（推荐）

模块仍全部静态编译进 exe（CMake 零改动），`config.json` 新增 `enabledModules`；关闭 = `getRouter()` 不注册进 Router + main 不启动其专属服务（claude 代理）。

- **覆盖需求全部语义**：不注册 → onStart/compose/onKeyEvent/onPageEnter 全部不可达（代码零执行，§2.1 已论证）；`modules()` 不含它 → 侧边栏零渲染。语义是**结构性保证**，不靠各模块自觉检查开关。
- 改动面：main.cpp 装配门控（~20 行）、Router fallback（~15 行）、Home 降级渲染（~60 行）、Settings 门控+新卡片（~100 行）、config 模型（~20 行）。**无 core 架构变更，无新抽象层。**
- 代价：关闭的模块代码仍在 exe 里（体积冗余 ~ 每模块几十 KB，对单 exe 工具无感）；生效需重启（见 §5.4 决策）。

### 方案 B：A + 事件总线（推迟）

把"Router 轮询/直呼"改成订阅事件（页面进入、数据变更、跨模块请求），关闭 = 不订阅。

**事件总线真正解决什么？** 唯一沾边的是 Home 反向依赖（3.1）。但拆开看：

- Home 依赖的是**共享 DB schema 上的 SQL**，不是代码调用——事件总线消不掉这个依赖，只能把"Home 进页查一次"变成"模块写数据时发 DataChanged 事件、Home 订阅刷新"。刷新时机问题已有更便宜的解：`onPageEnter` 重查（router.h:39-41，W7 已做）+ 需要实时时用 onTimer 轮询（Token 速率卡先例）。
- 跨模块跳转已有干净协议（payload 单槽），事件化反而引入异步时序问题。
- 规模判断：4 个模块 + 未来 8-10 个，**进程内、单 UI 线程为主**的桌面应用。事件总线的收益场景（多订阅者、动态拓扑、跨线程广播）一个都不存在。

**做 B 的真实成本**（如果现在做）：事件命名/载荷 schema 设计、订阅生命周期（模块 enable/disable 时退订）、时序与错误传播语义、proxy 网络线程 → UI 线程的边界封送（EventBus 得自带线程安全 + 只允许 UI 线程消费的约定）。这是一整套新基础设施，服务的是一个推测性需求。

**结论：推迟。触发条件（出现任一再做）**：① 出现第二个模块需要订阅同一信号（如 Todo 和 Home 都要实时响应 Markdown 变更）；② 出现真正的跨线程通知需求（当前 proxy_stats 的 mutex+snapshot 拉模式已够用）；③ 模块数 >10 且相互协作多于独立。届时在 core 加最小 EventBus（§5.6 有预留 sketch），接口按届时真实需求定，不预写。

### 方案 C：动态库插件（dlopen/LoadLibrary）——不做

技术上否决的理由，每条独立成立：

1. **与发布形态正面冲突**：单 exe 零 DLL 是 T3 达成、W2 验收的硬目标（spec §16 P14）；插件目录 = 回到多文件分发 + 插件版本管理。
2. **C++ ABI 地狱**：`Module` 接口全裸 STL（`std::string`、`eui::Ui&`），跨 .so/.dll 边界要求宿主与所有插件用同一编译器同一标准库版本 ABI——MinGW GCC 升级即全员重编，且 `-fno-exceptions -fno-rtti`（EUI-NEO 强加给 app target）下插件里异常/RTTI 语义还要单独约定。
3. **EUI-NEO 集成断裂**：模块直接用 EUI-NEO DSL 和字体/图标资源，插件需要宿主导出整套 EUI-NEO 符号或静态链接两份（两份静态 = 两份全局状态，theme/proxy_stats 直接错乱）。
4. **无需求支撑**：用户场景是"自己开发更多模块"，不是"第三方开发者生态"。编译期内置下加模块的成本已经是线性的（§2.2）。

除非将来明确要做第三方插件市场（需要独立 C ABI + 版本化接口 + 沙箱），否则不重开此案。

### 推荐

**方案 A。** 用户要的是"关闭 = 确定性地不执行、不显示、不占端口"，A 用最小改动给出结构性保证；B 是为未来可能的事件化预留的过度设计；C 与产品形态冲突。B 的事件总线等真需求出现再加，加的时候也不推翻 A（注册门控与事件订阅是正交的两层）。

## 5. 推荐方案（A）详细设计

### 5.1 数据模型

`config.json`（`core::paths::configFile()`）新增字段：

```jsonc
{
  "darkTheme": true,
  "fontScalePercent": 130,
  "logRetentionDays": 30,
  "enabledModules": ["home", "markdown", "todo", "claude_proxy"]  // 新增
}
```

- **缺省 = 全启用**（字段不存在/解析失败 → 默认列表）。老 config.json 零迁移直接兼容，SettingsStore 现有"损坏回退默认"策略（settings_config.h:51-52）自动覆盖。
- 模块 ID 字符串与 `pageIdToString` 对齐：`"home" | "markdown" | "todo" | "claude_proxy" | "settings"`（全小写）。未知 ID 忽略 + LOG_WARN（防手改 config 打死应用）。
- `SettingsConfig`（settings_config.h:36）加 `std::vector<std::string> enabledModules`，`sanitized()` 负责过滤未知项。
- **`home` 与 `settings` 不可关闭**（`kAlwaysOnModules`）：Home 是默认落地页（router.cpp:49 `current_ = PageId::Home`），Settings 是开关 UI 自身的宿主。允许关它们会引入"落地页不存在"和"关掉唯一入口"两个自锁问题，收益为零。三个工具模块（markdown/todo/claude_proxy）可关。（是否放开 Home 见 §9 开放问题。）

### 5.2 生命周期与装配（main.cpp）

```cpp
// getRouter() 内（伪码，改动集中在 main.cpp:125-157）
const auto enabled = /* SettingsStore 加载 + kAlwaysOnModules 补齐 */;
if (isEnabled("markdown")) { router.registerModule(markdown.get()); }
if (isEnabled("todo"))     { router.registerModule(todo.get()); }
// home / settings 恒注册
router.startAll();   // 只有已注册模块收到 onStart —— 零执行保证在这成立
```

- **Claude 代理门控**：main.cpp:402 `startClaudeProxy()` 加 `if (isEnabled("claude_proxy"))`。claude 模块关闭 = 代理不 bind 8888、`proxy_token.txt` 不写、`ClaudeService::global()` 保持 null。语义决策：**代理随模块走**（模块关闭 = 该功能整体下线，含后台服务）——"代码不在任何场景执行"的字面要求也包含代理线程。模块关闭期间 `claude_call_logs` 自然无新行，清理任务（app 级）照常处理旧行。
- **启动顺序不变**：`startClaudeProxy()` 仍在 `getRouter()` 之前（claude 启用时），W3 修复的顺序约定原样保留。
- `dispatchPageForFile`（main.cpp:57-68）：目标模块未注册 → 回退 Home + LOG_WARN，不再送进空白页。

### 5.3 Router 加固（core/router.cpp，~15 行）

```cpp
bool Router::isRegistered(PageId page) const;          // Home 降级渲染要查
```

`navigateTo()` 开头：目标页未注册 → `LOG_WARN("跳转到未注册页面 X，回退 Home")` + 改跳 Home（Home 恒注册，无递归风险）。`compose()` 的"无页面"兜底（router.cpp:60-62）保留作最后防线，正常路径不再触达。**不做运行时 unregister**（见 5.4）。

### 5.4 设置 UI 与生效时机

Settings 页新增"模块管理"卡片（放"外观"和"目录"卡之间，复用 cardFrame）：

- 三个工具模块各一行：名称 + 描述 + switch 开关（EUI-NEO switch 组件已在能力清单）
- 拨动开关 → 写 config.json → 状态栏提示"已保存，重启 DevDesk 后生效"

**生效时机决策：重启生效（一期）。** 理由：运行时动态反注册需要处理"当前正停在被关模块页"的迁移、模块内 service/service_ 状态的 stop-再-start 语义、onStart 里已消费的一次性资源——而"关闭模块"是低频操作，重启成本可忽略。**重启生效让"零执行"语义绝对干净**：进程从启动就没注册过它，不存在任何半死状态。运行时开关列入开放问题（§9），有真实使用反馈再说。

Settings 页现有卡片的门控：claude 模块关闭 → 隐藏"Claude 代理"卡（整卡不渲染，而非显示"服务不可用"故障态）；markdown 关闭 → 隐藏"笔记目录"卡。

### 5.5 Home 仪表盘降级（最大的单模块改动）

原则：**按注册状态隐藏，不显示陈旧数据、不显示故障态。**

- `HomeModule::compose` 开头查一次 `router->isRegistered(...)`（经 `appRouter()`，模块已有该句柄模式）
- 5 张统计卡 → 只渲染已启用目标模块的卡（Token 速率卡随 claude_proxy，笔记卡随 markdown，待办/逾期卡随 todo）。`statW = (contentW - gap*(n-1)) / n` 现算，n=1..5。核心 2 张（Token + API 调用）都关时该行整行隐藏。
- 快捷按钮组（新建笔记/新建任务/切换 Profile）：目标模块关闭 → 该按钮隐藏，宽度按剩余数量均分；全关 → 整行隐藏。
- 最近编辑列表：markdown 关闭 → 整卡隐藏。
- 空态兜底：三模块全关时 Home 显示欢迎区 + 一句"所有工具模块已关闭，可在设置中开启"。
- **不改 HomeService 的 SQL**——表恒存在（§3.5），隐藏发生在渲染层，数据层零改动。这是最小 diff 且避免了给 HomeService 加模块感知。

### 5.6 事件总线预留（不做，只记录设计约束）

若将来触发 §4 方案 B 的条件，最小实现约束如下（届时再展开成正式设计）：

- 位置 core 层（如 `src/core/event_bus.h`），零依赖 platform/EUI-NEO，同 `proxy_stats` 先例
- 不抛异常：订阅回调签名 `void(const Event&)`，回调内错误只能记日志，**无错误传播链**（fire-and-forget）
- 线程边界：发布允许任意线程，**回调统一在 UI 线程执行**（内部队列 + 主循环泵，或一期简化为"仅允许 UI 线程发布"断言）——proxy 网络线程的事件（如流结束）经队列过界
- 事件 = `{ const char* name; std::string_view payloadJson; }`，载荷用 platform::json 序列化，避免 std::any/继承体系
- 注册门控天然兼容：模块不注册就不订阅，两机制正交

### 5.7 迁移策略

- **config.json**：新字段可选，缺省全启用，无迁移（§5.1）
- **DB**：零改动。表是 app 级 schema，关闭模块不 drop、不 truncate；重启用数据完整（§3.5 决策）
- **代码**：纯增量，无既有行为变化（enabledModules 全启用时与现状二进制等价——验收要显式验证这一点）

## 6. 对现有模块的改造清单

| 文件 | 改动 | 规模 |
|------|------|------|
| `src/module_settings/settings_config.h/.cpp` | `enabledModules` 字段 + sanitized 过滤 + load/save 往返 | ~20 行 |
| `src/core/router.h/.cpp` | `isRegistered()` + `navigateTo` 未注册回退 Home | ~15 行 |
| `src/main.cpp` | getRouter 装配门控、startClaudeProxy 门控、dispatchPageForFile 回退 | ~25 行 |
| `src/module_home/home_module.cpp` | 卡片/按钮/最近编辑按 isRegistered 隐藏 + 动态宽度 | ~60 行 |
| `src/module_settings/settings_module.cpp` | "模块管理"卡片 + 代理/目录卡隐藏 | ~100 行 |
| `src/module_claude_proxy/*` | **零改动**（onStart 已容忍 global()==null，claude_module.cpp:730） | 0 |
| `src/module_markdown/*`、`src/module_todo/*` | **零改动**（被动模块，不注册即死） | 0 |
| `src/core/migrations.h` | **零改动**（§3.5 决策） | 0 |
| 顶层 `CMakeLists.txt` | **零改动**（编译期全量内置） | 0 |

顺手修（同 PR，非必须）：`writeProxyTokenFile` 的 main.cpp/settings_module.cpp 双份复制（§3.3）可收敛为 claude_service 内静态或 platform 函数——若 claude 模块可关后 Settings 不再引用它，此重复自然消失一半，届时一并清。

## 7. 分阶段实施与验收

### Phase 1：核心语义（config + 门控 + Router 加固）

1. settings_config 加字段（含单测：缺省/损坏/未知 ID 过滤往返）
2. Router `isRegistered` + navigateTo 回退（单测：跳未注册页回 Home、payload 不丢失语义）
3. main.cpp 三处门控

**验收**：`enabledModules: ["home","settings"]` 时——启动日志只有 2 个"启动模块"行；侧边栏 2 项；`netstat` 无 8888 监听；日志无任何 todo/markdown/claude SQL 痕迹；日志无"WARN"（启动顺序不回退）；全启用时与现状行为逐项一致。

### Phase 2：Home 降级 + Settings 模块管理卡

4. Home 按注册隐藏（单测注入 Router 验证卡片开关逻辑，UI 截屏验证布局）
5. Settings 模块管理卡 + 重启提示（截屏）

**验收**：关 todo → Home 无待办/逾期卡、无"新建任务"按钮，其余卡宽度自适应无布局破碎；关 claude → Settings 无代理卡；截屏按 spec §16.2 流程归档。

### Phase 3（推迟，触发条件见 §4）

运行时开关、事件总线——各自单独立项设计，本期不做。

**回归门槛**：137/137 全绿（现有套件），新增用例计入；P0-P8 历史行为不回退。

## 8. 风险

| 风险 | 缓解 |
|------|------|
| 手改 config 写错模块名 → 模块静默消失 | load 时未知 ID 记 WARN；Settings 卡片展示的是受控开关不是自由文本 |
| 用户关闭 claude 后以为代理还开着（外部脚本连 8888 失败） | 关闭开关的确认提示写明"代理将停止，重启后 8888 不再监听"；proxy_token.txt 在模块关闭启动时删除或加注释行 |
| Home 布局在 n=1/2 张卡时没测过 | Phase 2 验收显式截屏 n=0..4 全组合（三模块开关 8 种里抽边界 4 种） |
| 未来模块作者忘记走 enabledModules 约定 | main.cpp 装配处注释写明"新模块必须经 isEnabled() 门控"；spec §16 追加一条模块开发检查项 |

## 9. 开放问题

1. **Home 是否可关？** 本设计定为不可关（默认落地页）。若将来模块多了想让 Home 也变成可选仪表盘，需要先定"无 Home 时落地到哪"（第一个启用的模块？），届时再议。
2. **运行时开关值得吗？** 重启生效一期够用；若用户反馈"切换模块要重启很烦"，做动态 register/unregister + 当前页迁移。技术上 Router 侧可行（modules_ 是 vector，模块对象生命周期在 getRouter 的 static unique_ptr 里），难点只在模块自身状态语义。
3. **代理是否应该独立于 claude 模块？** spec §10.4 称代理"全局唯一"。当前语义是"代理随模块开关"。若将来出现"关 UI 页面但保留代理服务"的需求（headless 模式），把 startClaudeProxy 拆成独立 config 项即可，架构不用动。
4. **事件总线的第一个真实订阅者会是谁？** 留观。候选：Todo 截止提醒跨页通知、Markdown 保存后 Home 即时刷新（当前靠进页重查已够）。
