# 批1/7 ui_kit 核对结论表（need224 全站列表统一）

日期：2026-11-07｜角色：developer｜规范：docs/design/2026-11-07-列表统一规范.md｜clue 155

无视觉 diff（唯一改动 = 删除零引用死常量），证据 = 本核对结论表。

## 逐组件核对结论

| 组件 | 默认值现状 | 规范对照 | 结论 |
|---|---|---|---|
| glass.h | glassFill 0.95暗/0.94亮、glassPanelFill 0.78/0.80、border 0.12/0.60 | §5.1 表逐值一致 | 合规 |
| materials.h | itemSurface=glassFill、containerSurface=glassPanelFill | §5.1 语义层一致 | 合规 |
| card.h/card.cpp | cardSurface=glassFill；cardPanel→cardPanelColored(glassFill)；borderDefault 1px + panelShadow | §5.1/§5.2-1/2 | 合规 |
| wtab.cpp:62 | fontWeight(active?600:400) | §2.1 条件式两态正范式 | 合规 |
| wtabRow 计数（wtab.cpp:111/115） | fontSizeSmall/textSecondary 无 fontWeight=400 | 兼容声明 clue 118 范式不动 | 合规 |
| wtab 字号 15/14（kWtabFontSize/Small） | 池167 放大档 | 非列表文字角色（控制件），A 段角色表未覆盖 | 不冲突，豁免归类 |
| form_row.cpp:25/28 | label 14 scaled/textPrimary 无 fontWeight=400 | §6-A diff 无此条目；clue 155 诊断实测值即当前态 | 合规 |
| dialog_form.cpp:41/58 | 标题 bold 一律 700 | §2.3 豁免清单：附录批次随 #156 单独过会，不在本条目强推 | 豁免（延后） |
| splash.cpp:211 | fontWeight(700) | §2.3 豁免（品牌页） | 豁免 |
| empty_state.cpp:80/86 | 主句 14/textSecondary/400 | 描述性提示非列表行标题，判据不触发 600 | 合规 |
| selection_bar.cpp:26 | 选中胶囊 surface 白底 | 状态胶囊底不属卡面材质（§5.2-4 同族） | 合规 |
| button/wswitch/slider/toast/light_toast/columns/grid | 全 token 取色、无 fontWeight 硬编码 | 无冲突 | 合规 |
| metrics.h kCardGlassAlpha=0.72 | **零代码引用死常量**（全仓 grep 仅注释引用，注释值亦过期：0.92/0.72 vs 实际 glassFill 0.94/0.95） | §5.1 显式否决「第二套半透常量」 | **冲突→已删**（本批唯一改动） |

## §5.2-3 todo cardFaceColor 基色核对（规范批1唯一指定核对项）

todo_ui_board.cpp:65-76：`mixColor(ui_kit::cardSurface(t), *src, kTodoTintAlpha)`——混合基色已是 cardSurface(=glassFill) 而非裸 surface，语义色 Low/Medium/High=success/warning/error。**合规，无需改**（C9「待核」核清，实际改动归批7 不需做）。附注：:73 注释仍写「基底 surface 叠加 kCardGlassAlpha」为过期描述，批7 动该文件时顺手刷新。

## 波及面自查

- 删除 kCardGlassAlpha 前全仓 grep（src/+tests/）：零代码引用，全部命中为注释（settings_ui_cards.cpp ×6、github_sync_card.cpp:170、home_module.cpp:651、agent_detail_dialog.cpp:359、todo_ui_board.cpp:73/737、tests 注释 ×3）。删除后注释悬空但编译无关，各文件随批2-7 触达时刷新。
- 测试断言同源核对：test_settings.cpp:4003-4007 / test_github_sync_card.cpp:150 / test_theme_hierarchy.cpp 均断言 `cardSurface(t)` 函数返回值（追踪 glassFill 单一来源），非引用常量，不受删除影响。

## 改动

- src/ui_kit/metrics.h：删除 kCardGlassAlpha 常量及注释（-4 行）
- 验证：bash scripts/build-test-run.sh --dev --filter "file:test_theme_hierarchy"（编译全量完整性 + 玻璃材质契约断言）→ 构建成功，Total: 4 Passed: 4 Failed: 0（2026-11-07）
