# 全站弹窗遮罩差异表（bug165，2026-11-08 实查）

用途：后续弹窗迁移批（need156 存量壳→dialog_shell）的输入。遮罩「参数」全站已统一，
差异在「壳实现与范围」；本表按遮罩口径 + 壳状态两维记录。

## 遮罩口径（唯一实现）

- 实现：EUI-NEO `components/dialog.h:19` DialogStyle 内置 backdrop
- 参数：黑 α = 亮 0.28 / 暗 0.46（`tokens.dark` 驱动，无逐弹窗覆写口）
- 范围：全屏（screenW×screenH rect，z 序在面板下）
- 交互：点击遮罩 = 取消路径关闭；吞滚轮（onScroll 空实现）
- 出口：ui_kit `dialog_form.cpp:12`（旧壳）与 `dialog_shell.cpp:81`（新壳）**同一组件同一参数**

## 逐弹窗状态

| 弹窗 | 壳 | 遮罩 | 状态 |
|---|---|---|---|
| MD 新建笔记（md.dialog.new） | **dialog_shell（本批迁入 2c8e1383）** | 组件内置 全屏 0.28/0.46 | 统一 |
| release 新建任务 / 非常规确认 | dialog_shell（36b1f602 试点） | 同上 | 统一 |
| agent 可用工具 / memory 提案全文 | dialog_shell（36b1f602 试点） | 同上 | 统一 |
| MD 新建文件夹 / 重命名 / 删除 | dialog_form 旧壳 | 同上 | 待迁移批（壳结构，非遮罩） |
| todo 新建/详情任务（显式 zIndex） | dialog_form 旧壳 | 同上 | 待迁移批 |
| claude 域弹窗（删除确认 Compact / Profile Form） | dialog_form 旧壳 | 同上 | 待迁移批 |
| schedule 三处 / mcp 详情 / agent_detail / release_detail | 直用 components::dialog 自排 | 同上 | 待迁移批（遮罩已同口径） |
| settings git_error_dialog | 直用 components::dialog | 同上 | 待迁移批 |
| **终端设置面板（need250，terminal_page.cpp:921）** | **自绘 rect** | **黑 α0.35；范围仅页面区含 TAB 栏（非全屏）；点 scrim 不关（纯 hit 隔离，Esc/×/完成 关）** | **口径偏离——need250 设计如此，是否收编待裁决** |
| 原生 message_box | 平台原生 modal | 系统 | 规范域外（设计文档 G 表脚注） |

## 实测（探针 md_scrim_probe，before/after）

- backdrop 实测 rgba(0, 0, 0, 0.46)（暗档 tokens），1200×1000 全屏，前后同值
- before-dialog.png：旧壳 460 宽、无头/底分隔线、左锚半宽按钮对
- after-dialog.png：标准壳 640 宽、头区分隔线 + 底区细线、右锚按钮对
- 遮罩像素不变属预期：迁移统一的是「壳结构 + 遮罩语义出口」，遮罩参数本就同源
