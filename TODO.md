# DevDesk 未完成任务清单

> 2026-09-28 会话截止  
> 已交付 20+ commit（详见 `git log --oneline`）

## 未完成项

### 1. 桌宠最大高度设置
- **需求**：设置页加"最大高度"控件（默认 288px, 范围 [48,512]），皮肤超过等比缩放
- **实时生效**：改设置后当前宠物立即 resize
- **窗最小尺寸**：宠物窗最小 48×48
- **涉及文件**：`settings_config.h/cpp`、`settings_ui_cards.cpp`、`pet_window.cpp`、`pet_config.h/cpp`
- **状态**：代理派发后取消（a6e3434efb40b413f）

### 2. 桌宠气泡样式
- ✅ **已完成**（cbe7245）：底色半透 alpha 0.55 + 文本自适应宽 max 280px + 居中
- **待定**：是否需要换行支持（当前单行）

### 3. Todo 卡片拖拽跟手动画
- **需求**：拖动卡片时，卡片跟随光标移动（目前只变半透明 0.45，不跟手）
- **关键点**：`todo_ui_board.cpp` 的 `drawCard` 加 `.translate(offsetX, offsetY)`，偏移从 `BoardPlate.dragOffset` 取
- **状态**：未开始

### 4. Probes 扩展
- **已交付**：7 probe exe 全编译 + 全 PASS + harness 补键盘/右键/滚轮/拖拽/文本
- **待补**：
  - 多窗 stub（pet 行为档 P3）
  - `confirmDialogProbe` 共享骨架（spec 规划未实现）
  - Cl-13/Cl-14 探针断言组未全量实现
  - `--size` 尺寸矩阵多跑不支持（只支持单尺寸）

### 5. Rules 商标统一
- 缺圆角 item 已补（6f2b5b0：6 模块 9 处）
- 深色/浅色主题下各模块 hover 色验证——建议 probes P2 批覆盖

### 6. 安装程序
- ✅ 63MB setup.exe 已放 `D:\data\desktop\DevDesk-0.1.4-setup.exe`
- 含 9 皮肤 + about 漂浮圆环 + 全部修复
- package-windows.sh 已加 ccache launcher

### 7. About 页面效果
- ✅ 漂浮圆环（7addc2b：7 环 Spring 漂移）
- ✅ 2 卡左右并排（de20482）
- ✅ 删数据卡

### 8. 测试速度
- ✅ soak 60s 加 `DEVDESK_SOAK=1` 门控（b5d8525：3min→1m46s）
- ✅ 批量输出替代 per-case endl flush（b3bb24d）
- ⚠️ Top-3 慢测试：soak 60s (75s)、streaming 15s 超时（15s）、markdown 5000 行（5s）

### 9. EUI-NEO 上游
- ✅ `5b0cd34` styledmenu dismiss rect + requestDismiss 按值参数防 UAF
- 待 push（81 commits ahead of origin）

---

## 已知 Bug

| 问题 | 位置 |
|---|---|
| todo 卡片拖拽无跟手动画 | `todo_ui_board.cpp:drawCard` |
| md tabs 切换过渡已删（1cdd36f） | 原为整页 fade 闪烁 |
| pet 右键菜单缺 dismiss rect 已修（5b0cd34） | EUI-NEO styledmenu |
| 主窗最小尺寸已调 1330×845（65c1543） | main.cpp |

---

## 分支/合并状态

- 全在 `master`（ddf3425 起 22+ commit）
- 无残留 worktree
- 0 未提交改动
- EUI-NEO 1 个 commit 待 push