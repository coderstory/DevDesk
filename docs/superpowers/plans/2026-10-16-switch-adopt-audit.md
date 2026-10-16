# 审计 worktree-switch-adopt（merge 70a9331，diff 双亲 0115ddc..70a9331）

1. **坐标等价**：9 处 position/尺寸/属性逐字搬迁；旧 wrapper stack 零 padding（EdgeInsets 默认）、flow 子落原点 → 左上角恒等，无漂移。抽查 github enabled.sw / settings autostart / agent destructive 3 处均过；新 API 双轴全设，未误用「未设轴保 flow」语义（EUI-NEO 8976b1d 实证：position 落开关内部根 stack）。
2. **回归面**：9 个 wrap id 全仓引用=0（剩余 .wrap 均为无关既有容器：tabs/chart/logs/trigger/cron.template）；onChange/checked 原样；test find() 改指现存的开关根 id。合并对 dev 仅带入 7 个在册文件，分支旧基线噪音（quota lifecycle/event_bus/audit docs）正确按 dev 侧消解，无夹带。
3. **测试有效**：断言 y=78/254 由 impl 公式复推（±0.5），x 钉字面 104；变异 +10 读判必红（需真 layout，已调 ui.layout）；wrap 负断言改前非空、判据成立。finding（info）：test_github_sync_card.cpp:216-222 的 y 期望与 impl 公式同构复算，impl 常量同向改则不红，x 字面量部分兜底。
4. **卫生**：无调试残留、注释同步更新；markdown_module.cpp:428 系真 flow row（justify/gap + 流内 112×36），不动正确。

裁决：**放行**
