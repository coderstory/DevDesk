# 小抄：UI 走查方法学（driver 交互链与截图判读）

> 用途：用 devdesk_ui_driver / uat 探针做 UI 走查、截图判读、写断言脚本前翻此页；违背=假绿灯 / 空壳图。
> 惯例：一事一行，带日期与出处；与既有规则重复不记。

- 2026-11-18（need284 打磨审计 REPORT.md §4 D4）：driver 截图必须在交互步骤后显式 `wait` ≥800ms 再拍，否则拍的是空壳（动画 / 弹层未收场）——driver 动作前不做自动等待。
- 2026-11-18（need294 批次 C 收口 closure-qa-c.md §四）：ui_driver 原语清单（click/type/key/scroll/drag/wait/assert/screenshot/dump/quit，`tests/ui_driver/driver_steps.h`）**无右键原语**——右键菜单 / 右键系功能的走查用单元测试替代驱动点击，不要硬凑 click 假绿。
- 2026-11-18（need294 批次 C 收口 closure-qa-c.md §四）：逻辑树断言≠视觉渲染——`dump` / `assert` 只证元素在树里，像素是否真画出来要靠截图判读（实例：dark 档 settings tab1/tab3「白底空帧」为探针静态 init 竞态，逻辑断言全绿但图不可判）。
- 2026-11-18（need284 manual 07 智能体 / 10 MCP 卡壳点）：异步链（轮询刷新、后台线程试跑、预检中重检）断言「结果出现」前必须显式 `wait` ms 留余量，勿假设同步返回；收尾后再叠 D4 的 ≥800ms 截图等待。
