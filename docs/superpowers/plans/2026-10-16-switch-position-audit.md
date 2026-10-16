# 审计：feat/switch-position（8976b1d）— SwitchBuilder x/y/position

独立只读审计，worktree 隔离，变异后已还原（worktree 已清理，主仓状态未动）。

1. **先例一致性（过）**：components/switch.h:40-42 的 x/y/position 与 button.h:45-47、input.h:48-50 逐字同构（含 position() 委托写法）；hasX_/hasY_ 条件落根 stack 节点与 button.h:138-143、card.h:85-89、input.h:193-197 同构；float 坐标一致。无偏差。
2. **文档对齐（过）**：docs/组件.md:93 公共属性标准坐标类已列 position(x,y)；Switch 条目（:22）是单行表、无独立能力表，无需同步。
3. **测试真实性（过）**：tests/unit/switch_builder_position.cpp 4 断言组 = flow 默认原点 / position(140,36) 生效 / 只 x 动 x 留 y / 只 y 动 y 留 x。变异抽验：删 switch.h 中 hasX_/hasY_ 应用块 → 重编译 → 红在「position not applied」（exit 4）→ 还原 → git status 干净。测试真咬实现。
4. **消费方回归（过）**：diff 仅增 4 成员 + build() 重缩进，既有字段默认值零改动（grep 证实）；gallery / promo / sidebar_starter 三消费方独立构建 exit=0。
5. **构建卫生（过）**：GLOB CONFIGURE_DEPENDS 自动发现，无需动 CMake；example 目标 switch_position 编译通过；无调试残留（仅 tray_bridge.c 既有 deprecation 警告，与本次无关）。

无 finding。

裁决：**放行**
