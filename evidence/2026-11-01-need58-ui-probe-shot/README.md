# 池58 证据物：ui-probe-shot skill 实操验证（2026-11-01）

skill 产物：`.agents/skills/ui-probe-shot/SKILL.md`（仓库内唯一入库物）。

## 路径 A：整页批拍（skill「路径 A」原文命令实测）

- `pages-640/`：`cd build/tests && ./uat62_page_shots_probe --size 1000x640 --out <本目录>/pages-640`
- 结果：PASS，12 页落图（2000x1280 = Retina 2x），跳过 wallpaper / release_group（平台能力不支持，符合预期）
- 抽查 `01-home-640.png`：图标（Font Awesome）、侧栏选中态、卡片全渲染——非空窗/全黑

## 路径 B：特定 UI 自写探针（skill「路径 B」骨架 + 「布局取证」dump 段逐行实测）

- 临时探针 `tests/probes/ui_shot_poc_probe.cpp`（验证后已删除，不入库）：按 skill 骨架组合 bg+card 两 rect，加 dump 段
- `poc-layout-dump.txt`：frame 输出精确 = 构图值（card=(40,60 320x180)，逻辑 px）
- `poc-800x600.png`：1600x1200（2x），白卡位置/比例与 frame 断言一致（已目验）
- ctest 定向：`ctest -N -R ui_shot_poc_probe` 匹配 1 条 → skill 的 `-R <name>` 写法有效

## 过程记录（非缺陷，备忘）

- GLOB 变更触发的首次 reconfigure 构建出现一次过渡态红（release_list_ui.cpp 报 ui_kit::wtab 不存在），
  同一文件集再次构建即绿、此后稳定——HEAD 无回归，属 CONFIGURE_DEPENDS 重配置过渡竞态。
