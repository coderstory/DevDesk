# DevDesk 模块开关即时生效（Live Module Toggle）设计

> 日期：2026-10-01 · 状态：待用户评审 · 来源：用户实测发现「模块开关必须重启才生效」（agent M1 验收中暴露），立项修复

## 0. 背景与目标

现状（方案 A §5.4 既定设计，settings_module.cpp:559 注释明说）：模块开关**只写 config.json**，注册门控在下次启动的 main.cpp 装配处生效；唯一例外是桌宠（PET-CLOSE 即时窗口同步）。

用户预期：开关拨动，侧边栏/导航**当帧**反映，无需重启。

目标：markdown / todo / claude_proxy / agent 四个 Router 页面模块的开关即时生效。home/settings 恒开（无开关）；桌宠维持既有即时窗口同步不变。

## 1. 路线定案：A「注册表活门控」（用户已确认）

保持 R8 门控语义（**注册清单 = 门控**，buildGatedEntries 仍是唯一门控口径），把门控从「启动一次性」改为「**可重入**」：

- 模块对象全程存活（main.cpp 装配段持有的 unique_ptr 不动、零重建、零工厂改动）
- toggle 关 = `onStop()` → `Router::unregisterModule(module)`
- toggle 开 = `Router::registerModule(module)` → `onStart()`
- 侧边栏零改动：compose 遍历 `router->modules()`（app_sidebar.cpp:183），注册变化下一帧自然反映
- 路线 B（常注册 + 可见性过滤）被否：推翻 R8-C2 装配契约、门控口径分裂两处

## 2. 组件与数据流

```
设置页开关 onClick
  → settings_module::applyModuleEnabled(moduleId, on)     [改]
      写 cfg_ + persist()（不变）
      桌宠 → applyPetModuleVisibility（不变）
      其余 → module_toggle_bridge::onModuleToggled(moduleId, on)   [新，替代「重启生效」提示]

module_toggle_bridge（中立层桥头，模式照 module_pet_bridge.h）
  声明：void onModuleToggled(const std::string& moduleId, bool on);
        void setModuleToggleHandler(Handler);   // app 层启动时注入
  实现：main.cpp 装配段注入 → app_assembly::dispatchModuleToggle

app_assembly::dispatchModuleToggle(moduleId, on)          [新]
  1. 查 kModuleRegistry 得 PageId + 模块对象指针（main.cpp 已持有的表）
  2. on：registerModule → module->onStart()
     off：module->onStop() → unregisterModule
  3. off 且 router->currentPage() == 该页 → navigateTo(Home) + toast「模块已关闭」
```

**为什么走桥**：依赖规则禁止 module_settings 依赖 app 层；`module_pet_bridge.h` 已确立「中立层桥头 + app 层注入实现」的先例，本次照抄该模式（桥头位置与 pet 桥同层）。

## 3. Router 扩展（src/core/router.h/.cpp）

- 新增 `void unregisterModule(Module* m)`：从 modules_ 移除；**未注册时调用 = 幂等 no-op**（防重复 off）
- `registerModule` 重复注册同一模块 = 幂等 no-op（现状若是 fail/log 则改幂等，防重复 on）
- `navigateTo(PageId)` 增加守卫：目标页未注册 → 拒绝导航（返回 false 或静默；由调用方 toast）
- 注册清单变化后下一帧 compose 自然反映（Router 无需主动通知 UI——UI 每帧重建元素树）

## 4. 模块 onStart/onStop 重入幂等审计（本项目的核心风险面）

每个页面模块的 onStart/onStop 必须支持 `onStart → onStop → onStart → onStop` 循环 + 意外双重 onStart。已知与待审：

| 模块 | 已知问题 | 处置 |
|---|---|---|
| todo | `attachTools` 守卫 `if (!service_ \|\| todoTools_) return;`——onStop 后 todoTools_ 仍非空 → 二次 onStart **不重注册工具** | onStart 改为按注册表实况重挂（find 失败才 register），或 onStop 置空 todoTools_ |
| markdown | 同构风险（markdown_tools 挂载守卫） | 同上口径，实现期核实 |
| claude_proxy | onStart 的 service 接线（代理本体由 main 启动，W3）；重复 onStart 的幂等性 | 实现期核实，原则同上 |
| agent | onStart：sweep → 构造 store_/llm_/loop_ → 注册 app.stats；onStop：cancel→join→clear→unregister。重入：成员 unique_ptr 重构造安全；**双重 onStart 会重复注册 app.stats**（ToolRegistry 同名注册行为需核实：报错即需守卫） | onStart 先查注册表实况；重入用例覆盖 |

**统一验收**：每模块一条重入循环用例（onStart→onStop→onStart，断言工具注册表状态、服务可用、无重复注册），进各自测试文件。

## 5. 交互决策

- D-1 关闭正在浏览的页面 → `navigateTo(Home)` + toast「模块已关闭」（toast 单例，既有）
- D-2 agent 关闭走既有 onStop（取消并 join 运行中 run）——即时生效是安全增益，时序已由 T12 用例钉死
- D-3 托盘导航（dispatchTrayNavigation）同门控口径：未注册页不可达（该函数遍历 kModuleRegistry 派生，随注册状态自然一致——实现期核实）
- D-4 重复 toggle 同值（连点）→ applyModuleEnabled 已短路（状态没变不写盘），桥不触发
- D-5 桌宠路径完全不动（含其「重启生效」注释口径的更新——pet 注释补一句「其余模块自 live-toggle 起即时生效」）

## 6. 非目标

- 不做模块对象的热销毁/重建（对象常驻，main.cpp 装配段不动）
- 不改 buildGatedEntries 本身与启动期门控（启动仍按 config 注册，两个路径共用同一 `moduleEnabled` 谓词）
- 不做 home/settings 的开关化
- 不做「关闭模块时保留其数据/后台任务」的复杂语义——off 即 onStop

## 7. 测试要求

1. **Router 单测**：register/unregister 幂等、navigateTo 未注册页拒绝、unregister 当前页的回调路径
2. **重入循环**：四模块各一条（§4 统一验收）
3. **桥接线**：settings toggle → 桥 → dispatchModuleToggle 的调用断言（照 pet 桥的接线断言模式）
4. **侧边栏实时反映**：无头（真 app::compose + 桩 Router 模式，test_app_sidebar 先例）——toggle 后 compose，条目出现/消失
5. **probe**：真窗口里设置开关拨动 → 侧边栏条目当帧出现/消失（agent_probes 或 settings_cards_probes 扩展）
6. **回归**：启动期门控契约测试（buildGatedEntries 系列）必须零语义变化
7. **静默失败面**：桥未注入（handler null）时 toggle 仍写盘成功、LOG_WARN 一条、不崩（启动早期拨开关的窗口期）

## 8. 风险

- 重入审计漏模块 → 二次开启后工具/服务缺失（静默失败）。缓解：§4 统一验收用例是硬关卡
- 桥的注入时机早于用户操作（main.cpp 装配段注入，先于 Router 展示）→ 无窗口期风险，测试钉
- todo 的 UAF 修复（onStop 不 reset service_）与重入的交互：onStop→onStart→onStop 的第二次 onStop 在 service_ 存活下 detachTools——工具指针生命周期已由 T8 用例覆盖，实现期复核
