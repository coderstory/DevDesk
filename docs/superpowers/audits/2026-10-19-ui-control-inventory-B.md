> **存档说明**（非原文内容）：来源：只读盘点子代理 2026-10-19 产出，原文照录（HTML 转义已还原）。与 `2026-10-19-ui-control-inventory-A.md` 合为全量 UI 控件参考，供 C++ 开发阶段"先看原本 UI 有啥"用（按用户 2026-10-19 澄清"文档与代码无绝对映射"，不倒灌进文档当镜像目标）。

---

# DevDesk 4 页面 UI 控件全量盘点 B（todo / claude_proxy / agent / settings）

仓库：`D:\project\gui\DevDesk\src`。弹窗触发点引 `2026-10-19-dialog-inventory.md` 编号。

## 1. module_todo（Todo 看板）

| # | 类型 | 用途/文案 | 区域 | file:line | id |
|---|---|---|---|---|---|
| T1 | 文本标题 | "Todo 看板" | 页头 | todo_ui_board.cpp:46-53 | todo.h1 |
| T2 | 文本副标题 | "点击卡片查看详情 · Ctrl+↑/↓…" | 页头 | :59-67 | todo.subtitle |
| T3 | 搜索框 input | "搜索任务…" | 页头动作区 | :83-95 | todo.search |
| T4 | 图标 FA 0xF002 | 放大镜 | 搜索框内 | :96-103 | todo.search.icon |
| T5 | 次按钮+图标 0xF071 | "逾期 N"（0 置灰） | 页头动作区 | :109-124 | todo.btn.overdue |
| T6 | 次按钮 | "+ 列" | 页头动作区 | :126-135 | todo.btn.addcol |
| T7 | 主按钮 | "+ 新建任务" | 页头动作区 | :137-146 | todo.btn.add |
| T8 | 错误文本 | "数据加载失败…" | 板面 | :163-171 | todo.error.text |
| T9 | 主按钮 | "重试" | 板面错误态 | :172-182 | todo.error.retry |
| T10 | 空态文本 | "还没有任何列…" | 板面空态 | :192-200 | todo.empty.text |
| T11 | 看板列容器 row | 列横向排列 | 板面 | :216-225 | todo.body |
| T12 | 列 bg rect（拖拽悬停加粗边） | 列底色+边框 | 列 | :264-272 | `<id>.bg` |
| T13 | 列头右键热区 rect | 列菜单触发 | 列头 | :288-297 | `<id>.head` → 右键 #5 |
| T14 | 列色圆点 rect | 列颜色标识 | 列头 | :302-308 | `<id>.dot` |
| T15 | 列标题文本 | "标题 (N)" | 列头 | :310-322 | `<id>.title` |
| T16 | 图标按钮 0xF303 | ✎ 重命名列 | 列头 | :325-334 | `<id>.rename` → 弹窗 #8 |
| T17 | 次按钮 | "+" 加任务到列 | 列头 | :336-345 | `<id>.add` → 弹窗 #6 |
| T18 | 空列提示文本 | "暂无任务"/"归档的任务…" | 列内 | :354-364 | `<id>.empty` |
| T19 | scrollView | 列内卡片滚动 | 列内 | :378-418 | `<id>.scroll` |
| T20 | 拖拽占位条 rect（半透主色） | 插入位指示 | 列内 overlay | :251-262,395-400 | `<id>.dropbar` |
| T21 | 卡片组件 card | 卡片骨架 | 列内 | :437-443 | `<id>.card` |
| T22 | 卡片交互层 rect | hover/active/选中边框/onClick/onContextMenu/onPress/onDrag/onRelease | 卡片 | :455-486 | `<id>.bg` → 弹窗 #11/右键 #4 |
| T23 | 截止 chip | 截止日期（逾期红） | 卡片 chip 行 | :499-509 | `<id>.due` |
| T24 | 归档时间 chip | archivedAt | 卡片 chip 行 | :517-525 | `<id>.archived` |
| T25 | tag chip ×N（最多 2 + "+N"） | 任务标签 | 卡片 chip 行 | :526-545 | `<id>.tag.<i>` / `.tag.more` |
| T26 | 序号徽章 rect+text | 列内序号（1..n，六色哈希环） | 卡片标题行 | :577-597 | `<id>.badge` / `.badge.text` |
| T27 | 卡片标题文本 | 任务标题（居中省略） | 卡片 | :603-616 | `<id>.title` |
| T28 | 完成图标 FA 0xF00C | ✓ 完成角标 | 卡片 | :628-639 | `<id>.done` |
| T29 | 拖拽跟手副本 card+text（z=100，0.85 alpha） | 拖动中跟手 | 页顶层 | :652-708 | todo.dragghost |
| T30 | 弹窗 #6 新建任务 | 选列+任务名 | 全屏 | todo_ui_dialogs.cpp:38-100 | todo.dialog.newtask |
| T30a | 单行 input | "任务标题…" | 弹窗 #6 | :66-79 | todo.dialog.newtask.input |
| T30b | 信息文本 | "将创建在「X」列" | 弹窗 #6 | :61-64 | todo.dialog.newtask.col |
| T30c | 红字错误行 | 校验失败 | 弹窗 #6 | :82-85 | todo.dialog.newtask.error |
| T31 | 弹窗 #7 新建列 | 列名 | 全屏 | :123-183 | todo.dialog.newcol |
| T32 | 弹窗 #8 重命名列 | 列改名 | 全屏 | :262-314 | todo.dialog.rename |
| T33 | 弹窗 #9 删除确认（任务/列双态） | 不可恢复 | 全屏 | :333-383 | todo.dialog.delcol |
| T34 | 弹窗 #10 逾期任务清单（Wide） | 行点击跳详情 | 全屏 | :411-515 | todo.dialog.overdue |
| T35 | 弹窗 #11 任务详情（Wide, 656h, 只读态可切换） | 标题/描述/优先级/截止/标签/完成 | 全屏 | :594-849 | todo.dialog.detail |
| T35a | 单行 input | "任务标题…" | 弹窗 #11 | :638-646 | todo.dialog.detail.input.title |
| T35b | 多行 textarea | "补充说明…" | 弹窗 #11 | :652-664 | todo.dialog.detail.input.desc |
| T35c | 优先级 segmented ×4（None/Low/Medium/High，选中深底+边框） | 优先级 | 弹窗 #11 | :670-693 | todo.dialog.detail.prio.<i> |
| T35d | 次按钮 | "未设置"/"清除" 截止 | 弹窗 #11 | :701-712 | todo.dialog.detail.due.clear |
| T35e | 次按钮+图标 0xF133 | "选择日期…"/已设日期 → datepicker | 弹窗 #11 | :714-726 | todo.dialog.detail.due.pick |
| T35f | tag chip + × 删除按钮 ×N | 标签编辑 | 弹窗 #11 | :733-761 | todo.dialog.detail.tag.<i> + .x |
| T35g | 单行 input（回车追加，Esc 清草稿） | "输入标签，回车添加…" | 弹窗 #11 | :762-773 | todo.dialog.detail.tag.input |
| T35h | 关闭按钮（只读态） | "关闭" | 弹窗 #11 底 | :778-790 | todo.dialog.detail.close |
| T35i | 危险次按钮 | "删除" → 弹窗 #9 | 弹窗 #11 底 | :792-806 | todo.dialog.detail.delete |
| T35j | 次按钮+图标 0xF021/0xF00C | "标记完成"/"取消完成" | 弹窗 #11 底 | :808-830 | todo.dialog.detail.toggle |
| T35k | 主按钮 | "保存" | 弹窗 #11 底 | :832-846 | todo.dialog.detail.save |
| T36 | 日期滚轮选择器 datePicker | 截止日期选择 | 浮层 | :386-408 | todo.datepicker |
| T37 | 右键菜单 #4 卡片 | 编辑/上移/下移/移动到某列×N/删除 | 列表右键 | :186-248 | todo.ctx |
| T38 | 右键菜单 #5 列头 | 重命名列/左移/右移/改色(8 色子菜单)/删除列 | 列头右键 | :206-215 | todo.ctx |
| T39 | toast | 操作反馈 | 顶层 | todo_module.cpp:530-533 | todo.toast |

## 2. module_claude_proxy（Claude 切换器）

| # | 类型 | 用途 | 区域 | file:line | id |
|---|---|---|---|---|---|
| C1 | 页标题 | "Claude 切换器" | 页头 | claude_ui_list.cpp:288-295 | claude.h1 |
| C2 | 状态圆点 rect | 运行中绿/已停灰 | 页头 | :303-307 | claude.status.dot |
| C3 | 状态文本 | "运行中 :8888"/"已停止" | 页头 | :308-316 | claude.status.text |
| C4 | 副标题 | "管理多套 Claude API 配置…" | 页头 | :325-331 | claude.subtitle |
| C5 | 次按钮+图标 0xF04B/0xF04C | "启动代理"/"停止代理" | 页头动作区 | :341-353 | claude.btn.proxy |
| C6 | 引导条图标 0xF05A+文本 | 接管提示（首次） | 引导条 | :363-385 | claude.guide.* |
| C7 | 左栏面板 bg | Profile 列表面板 | 左栏 | :401-408 | claude.profiles.bg |
| C8 | 面板标题 | "Profile 列表" | 左栏 | :414-422 | claude.profiles.title |
| C9 | 错误/空态文本 | "加载失败…"/"暂无 Profile…" | 左栏 | :437-464 | claude.profiles.error/.empty |
| C10 | virtualList | Profile 卡片虚拟滚动 | 左栏 | :486-543 | claude.profiles.scroll |
| C11 | Profile 卡片 rect（选中/激活态） | 点击选中 | 左栏列表 | :150-160 | claude.profile.<i>.bg |
| C12 | 头像圆点 rect（9 色哈希）+首字 | Profile 头像 | 卡片 | :169-189 | .avatar/.avatar.text |
| C13 | 名称文本 | Profile 名称 | 卡片 | :192-200 | .name |
| C14 | ACTIVE 徽章 rect+text | 激活标识 | 卡片 | :202-221 | .active.bg/.active.text |
| C15 | Base URL 文本 | URL | 卡片 | :224-231 | .url |
| C16 | 共享徽章 rect+text | "共享" | 卡片 | :234-252 | .shared.badge/.text |
| C17 | model 文本 | "model: X" | 卡片 | :255-263 | .model |
| C18 | 卡片右键热区 stack | 右键菜单触发 | 卡片 | :513-532 | `<cardId>.wrap` → 弹窗 #16/右键 #6 |
| C19 | 共享配置区块 stack | 钉面板底 | 左栏 | :545-553 | claude.shared.block |
| C20 | 区块标题 | "共享配置片段" | 共享区块 | :612-620 | claude.shared.title |
| C21 | 错误/空态文本 | "加载失败…"/"暂无片段…" | 共享区块 | :635-648 | claude.shared.error/.empty |
| C22 | scrollView | 片段卡片滚动 | 共享区块 | :651-703 | claude.shared.scroll |
| C23 | 片段卡 rect（onClick=编辑） | 片段条目 | 共享区块 | :670-680 | claude.shared.card.<i>.bg |
| C24 | 片段名+JSON 预览文本 ×2 | 片段信息 | 共享区块 | :681-697 | .name/.keys |
| C25 | 次按钮 | "+ 新建片段" → 弹窗 #18 | 共享区块底 | :705-715 | claude.shared.new |
| C26 | 次按钮 | "+ 新建 Profile" → 弹窗 #16 | 左栏底 | :559-570 | claude.profiles.new |
| C27 | 次按钮+图标 0xF019 | "导入当前配置" | 左栏底 | :571-584 | claude.profiles.import |
| C28 | 次按钮 | "从 cc-switch 导入" → 弹窗 #22 | 左栏底 | :585-599 | claude.profiles.import.ccswitch |
| C29 | 右栏详情面板 bg | 详情 | 右栏 | claude_ui_detail.cpp:936-944 | claude.detail.bg |
| C30 | 详情标题 | "当前 Profile 详情" | 右栏 | :956-964 | claude.detail.title |
| C31 | Tab 组件（5 页签：基础信息/模型映射/环境变量/高级配置/最近调用） | 切换详情页 | 右栏 | :973-983 | claude.detail.tabs |
| C32 | 内容框 bg rect | 显式边界 | 右栏 | :994-1000 | claude.detail.contentbox |
| C33-C39 | 基础信息 pane（名称/Base URL/API Key/接管状态/共享配置绑定/覆盖警告） | Tab 0 | :60-173 | claude.field.* |
| C40-C41 | 模型映射 pane：槽位行 ×N（槽位名+值+1M toggleSwitch） | Tab 1 | :206-252 | claude.slot.* / .onem.<i> |
| C42-C44 | 环境变量 pane：env 行 ×N（键+值+来源+官方说明 tooltip）+空态+溢出 | Tab 2 | :296-406 | claude.env.* |
| C45-C49 | 最近调用 pane：表头 ×5+调用行 ×N（时间/方法路径/状态色/耗时/速率）+空态/错误+溢出+"查看全部 →"按钮 → 弹窗 #20 | Tab 4 | :436-647 | claude.calls.* |
| C50-C53 | 高级配置 pane：方言+备用 Profile+高级属性键来源行 ×N+空态/溢出/note | Tab 3 | :674-780 | claude.field.dialect/fallback/advjson.* |
| C54 | 分隔线 rect ×2 | 动作区上下分割 | 右栏底 | :791-796,916-921 | claude.act.divider/claude.divider |
| C55 | 主按钮 | "启用" | 右栏动作区 | :810-826 | claude.act.enable |
| C56 | 次按钮 | "测试连通" | 右栏动作区 | :828-844 | claude.act.test |
| C57 | 次按钮 | "编辑" → 弹窗 #16 | 右栏动作区 | :846-862 | claude.act.edit |
| C58 | 次按钮 | "运行"（触发原生目录选择器） | 右栏动作区 | :864-880 | claude.act.run |
| C59 | 次按钮 | "余量查询"（功能开关开启时）→ 弹窗 #21 | 右栏动作区 | :883-899 | claude.act.quota |
| C60 | 副提示文本 | "以该 Profile 生成临时配置…" | 右栏动作区 | :905-913 | claude.act.run.hint |
| C61 | 弹窗 #16 新建/编辑 Profile（Form, 600h, 4 tabs） | 表单 | 全屏 | claude_ui_form.cpp:1012-1086 | claude.dialog.form |
| C61a | tabs（基础/模型映射/高级 env/高级配置） | 表单页签 | 弹窗 #16 | :1041-1048 | claude.form.tabs |
| C61b | 单行 input ×3（名称/Base URL/API Key） | 基础页 | 弹窗 #16 | :417-451 | claude.dialog.form.name/url/key |
| C61c | dropdown 单选（认证字段 3 选项） | "认证字段" | 弹窗 #16 基础页 | :467-479 | claude.dialog.form.authfield.dd |
| C61d | 图标 0xF05A+提示 | "模型映射、高级 env…" | 弹窗 #16 基础页 | :483-491 | claude.form.basic.hint.* |
| C61e | 红字错误 | 校验失败 | 弹窗 #16 | :493-498 | claude.dialog.form.error |
| C61f | 警告图标 0xF071+文本 | 接管态导入警告 | 弹窗 #16 基础页 | :502-510 | claude.dialog.form.importwarn.* |
| C61g | 表头 ×3（槽位/上游模型名/显示名） | 模型映射页表头 | 弹窗 #16 Tab 1 | :750-761 | claude.form.slots.h.* |
| C61h | 槽位行 ×N（label+input+input） | 模型槽位编辑 | 弹窗 #16 Tab 1 | :763-793 | claude.form.slot.<i>.model/name |
| C61i | 提示 ×2 | 自动派生/留空说明 | 弹窗 #16 Tab 1 | :795-808 | claude.form.slots.hint1/hint2 |
| C61j | env 行 ×5（key input+value input+× 删除 0xF00D） | env 编辑 | 弹窗 #16 Tab 2 | :829-885 | claude.form.env.row.<i>.key/value/del |
| C61k | tooltip（已知 env 键 hover 说明） | 变量名识别 | 弹窗 #16 Tab 2 | :843-850 | claude.form.env.row.<i>.tip |
| C61l | 未知键红边框 rect | 未知变量警告 | 弹窗 #16 Tab 2 | :851-858 | claude.form.env.row.<i>.key.unknown |
| C61m | 次按钮 | "+ 添加变量" | 弹窗 #16 Tab 2 | :887-895 | claude.form.env.add |
| C61n | 溢出提示 | "另有 N 项已保存…" | 弹窗 #16 Tab 2 | :897-904 | claude.form.env.overflow |
| C61o | 分隔标签 | "── 从 cc-switch 粘贴导入 ──" | 弹窗 #16 Tab 2 | :907-911 | claude.form.import.label |
| C61p | 多行 textarea | 粘贴 JSON | 弹窗 #16 Tab 2 | :912-920 | claude.form.import.text |
| C61q | 次按钮 | "解析导入" | 弹窗 #16 Tab 2 | :922-930 | claude.form.import.parse |
| C61r | 主按钮（解析就绪时） | "应用到表单" | 弹窗 #16 Tab 2 | :932-940 | claude.form.import.apply |
| C61s | 解析结果文本 ×N（预览/警告/提示） | 导入预览 | 弹窗 #16 Tab 2 | :944-1006 | claude.form.import.error/preview/note.*/hint |
| C61t | dropdown 单选（方言 3 选项） | "上游方言" | 弹窗 #16 Tab 3 | :547-569 | claude.form.advanced.dialect.dd |
| C61u | dropdown 单选（备用 Profile N+1 项） | "备用 Profile" | 弹窗 #16 Tab 3 | :611-626 | claude.form.basic.fallback.dd |
| C61v | dropdown 单选（共享片段 N+1 项） | "绑定片段" | 弹窗 #16 Tab 3 | :661-677 | claude.form.advanced.shared.dd |
| C61w | 多行 textarea | "高级设置 JSON" | 弹窗 #16 Tab 3 | :690-701 | claude.form.advanced.json |
| C61x | 次按钮 | "格式化" | 弹窗 #16 Tab 3 | :704-712 | claude.form.advanced.json.format |
| C61y | 次按钮 | "全屏编辑" → 弹窗 #23 | 弹窗 #16 Tab 3 | :714-722 | claude.form.advanced.json.fs |
| C61z | 绑定提示 | "已绑定片段…" | 弹窗 #16 Tab 3 | :727-733 | claude.form.advanced.json.boundhint |
| C61aa | 行内 JSON 红字 | 格式化失败 | 弹窗 #16 Tab 3 | :737-741 | claude.form.advanced.json.error |
| C61ab | 取消/保存按钮对 | — | 弹窗 #16 底 | :1071-1084 | — |
| C62 | 全屏覆盖层 #23（z=1050, JSON 编辑） | 全屏 JSON 编辑 | 全屏 | :1089-1185 | claude.jsonfs.* |
| C62a-e | 遮罩+大 textarea+提示+错误+格式化/保存/放弃按钮 | — | 弹窗 #23 | :1107-1182 | claude.jsonfs.* |
| C63 | 弹窗 #17 删除确认（Compact） | "确认删除？" | 全屏 | :1187-1231 | claude.dialog.delete |
| C64 | 弹窗 #18 共享配置片段编辑（Form, 460h, 编辑态多"删除"） | 片段编辑 | 全屏 | :1307-1432 | claude.dialog.shared |
| C64a-f | name input+json textarea+format btn+error+delete btn+cancel/save | — | 弹窗 #18 | :1334-1430 | claude.dialog.shared.* |
| C65 | 弹窗 #19 片段删除确认（Compact） | "确认删除片段？" | 全屏 | :1435-1480 | claude.dialog.shared.delete |
| C66 | 弹窗 #20 调用记录（ExtraWide, 560h, virtualList+分页） | 完整调用记录 | 全屏 | claude_ui_logs.cpp:177-403 | claude.dialog.calls |
| C66a-h | search input+status dropdown+count+loading/empty+virtualList+prev/next 翻页+page 文本+close | — | 弹窗 #20 | :202-401 | claude.dialog.calls.* |
| C67 | 弹窗 #21 余量查询配置（Form, 动态高） | 供应商配置 | 全屏 | claude_ui_quota.cpp:71-285 | claude.dialog.quota |
| C67a-h | provider dropdown+动态字段 input（含 secret 脱敏）+ccswitch load btn+ccswitch note+ccswitch dropdown+test btn+result+cancel/save | — | 弹窗 #21 | :110-282 | claude.dialog.quota.* |
| C68 | 弹窗 #22 cc-switch 导入（Wide, 520h） | 勾选导入 | 全屏 | claude_ui_ccswitch.cpp:245-405 | claude.dialog.ccswitch |
| C68a-h | desc+error/empty+selall+scrollView+候选行（自绘 checkbox+name/url/model）+hit+result+close/import | — | 弹窗 #22 | :263-403 | claude.dialog.ccswitch.* |
| C69 | 右键菜单 #6 Profile（编辑/建副本/复制配置/删除/设为当前） | Profile 操作 | 列表右键 | claude_ui_list.cpp:720-766 | claude.list.ctx |
| C70 | toast | 操作反馈 | 顶层 | claude_module.cpp:504-508 | claude.toast |

## 3. module_agent（智能体）

| # | 类型 | 用途 | 区域 | file:line | id |
|---|---|---|---|---|---|
| A1 | 页标题 | "智能体" | 页头 | agent_module.cpp:1578-1585 | agent.title |
| A2 | 主按钮（store 未就绪 disabled） | "+ 新建任务" → 弹窗 #1 | 页头 | :1586-1592 | agent.page.newTask |
| A3 | 页内错误文本 | "LLM 配置不可用…" | 页头下 | :1597-1605 | agent.page.error |
| A4 | 引导链接文本（onClick） | "去 Claude 页 →" | 页头下 | :1607-1617 | agent.page.error.hint |
| A5 | 可用工具区（可折叠 header onClick 切换） | 工具列表 | 工具区 | :1621-1654 | agent.tools.header |
| A6 | header label+展开提示 | "可用工具（N）"+"展开 ▼"/"收起 ▲" | 工具区头 | :1637-1652 | agent.tools.header.label/hint |
| A7 | scrollView+工具卡 ×N（bg+name+desc+params） | 工具卡列表 | 工具区 | :1655-1739 | agent.tools.scroll/card.<i>.* |
| A8 | 任务列表 scrollView | 任务行 | 内容区 | :1745-1750 | agent.tasklist |
| A9 | 空态/错误文本 | "暂无任务…"/"数据库未就绪" | 任务列表 | :1752-1770 | agent.empty/error |
| A10 | 任务行 stack（bg 命中=点行开详情） | 行容器 | 任务列表 | :1816-1938 | agent.task.<id>/.bg → 弹窗 #3 |
| A11 | 任务名文本（disabled 置灰） | 任务名 | 任务行 | :1834-1843 | agent.task.<id>.title |
| A12 | 配置异常标记 | "配置异常" | 任务行 | :1846-1854 | agent.task.<id>.warn |
| A13 | 限次徽章 | "一次性"/"限N次" | 任务行 | :1856-1864 | agent.task.<id>.limit |
| A14 | 已停用徽章 | "已停用" | 任务行 | :1866-1874 | agent.task.<id>.off |
| A15 | 状态圆点 rect | 状态色 | 任务行 | :1875-1879 | agent.task.<id>.status.dot |
| A16 | 状态文本 | 状态中文 | 任务行 | :1880-1886 | agent.task.<id>.status |
| A17 | actionChip "详情" | 打开详情 → 弹窗 #3 | 任务行 | :1887-1891 | agent.task.<id>.detail |
| A18 | actionChip "编辑" | 打开编辑 → 弹窗 #1 | 任务行 | :1892-1896 | agent.task.<id>.edit |
| A19 | actionChip "禁用"/"启用" | 切换启用 | 任务行 | :1897-1902 | agent.task.<id>.toggle |
| A20 | actionChip "删除"（运行中 disabled） | 删除确认 → 弹窗 #5 | 任务行 | :1903-1909 | agent.task.<id>.delete |
| A21 | actionChip "入队"（仅 resident） | 手动入队 | 任务行 | :1910-1923 | agent.task.<id>.enqueue |
| A22 | 主按钮（运行中/loop 未装 disabled） | "启动"/"运行中…" | 任务行 | :1924-1934 | agent.task.<id>.run |
| A23 | 弹窗 #1 任务表单（Form, 500h, 新建/编辑共用） | 任务编辑 | 全屏 | :1963-2346 | agent.dialog.task |
| A23a | 关闭文本按钮 ✕ | 关闭 | 弹窗 #1 | :2004-2014 | agent.dialog.task.close |
| A23b | actionChip 主按钮 "AI 生成草稿" → 弹窗 #2 | 草稿入口 | 弹窗 #1 标题行 | :2019-2027 | agent.dialog.task.aichip |
| A23c | 多行 textarea | "描述这个任务要做什么…" | 弹窗 #1 | :2038-2049 | agent.dialog.task.goal |
| A23d | dropdown 单选（触发方式：手动/定时/事件/闹钟/常驻） | 触发方式 | 弹窗 #1 | :2063-2081 | agent.dialog.task.trigger |
| A23e | 事件开关 ×3（toggleSwitch，仅 event 分支） | 事件勾选 | 弹窗 #1 | :2091-2102 | agent.dialog.task.event.<i> |
| A23f | 单行 input+dropdown 模板（仅 schedule 分支） | "cron 表达式"+"选择模板…" | 弹窗 #1 | :2113-2151 | agent.dialog.task.cron/.cron.template |
| A23g | cron 预览/错误文本 | "下次触发：…" | 弹窗 #1 | :2154-2159 | agent.dialog.task.cron.preview |
| A23h | 单行 input（仅 once 分支） | "YYYY-MM-DD HH:MM" | 弹窗 #1 | :2170-2179 | agent.dialog.task.once |
| A23i | 闹钟预览/错误文本 | "将在 …" | 弹窗 #1 | :2180-2185 | agent.dialog.task.once.preview |
| A23j | 触发提示文本 | 各类型说明 | 弹窗 #1 | :2190-2194 | agent.dialog.task.trigger.hint |
| A23k | dropdown 单选（模型槽位） | "模型槽位" | 弹窗 #1 | :2205-2218 | agent.dialog.task.slot |
| A23l | 单行 input（数字解析） | "迭代上限" | 弹窗 #1 | :2223-2231 | agent.dialog.task.iter |
| A23m | toggleSwitch | "允许破坏性操作" | 弹窗 #1 | :2245-2253 | agent.dialog.task.destructive |
| A23n | toggleSwitch（仅 schedule/event） | "只跑一次"/"最多 N 次" | 弹窗 #1 | :2255-2272 | agent.dialog.task.limit |
| A23o | 提示文本 | 迭代/破坏性说明 | 弹窗 #1 | :2277-2282 | agent.dialog.task.meta.hint |
| A23p | 多行 textarea | "记录任务执行上下文…"（执行记忆） | 弹窗 #1 | :2289-2300 | agent.dialog.task.note |
| A23q | toggleSwitch | "结束后自动记录回复" | 弹窗 #1 | :2305-2313 | agent.dialog.task.autonote |
| A23r | 错误文本 | 校验失败 | 弹窗 #1 | :2316-2319 | agent.dialog.task.error |
| A23s | 取消/保存按钮对 | — | 弹窗 #1 底 | :2324-2344 | agent.dialog.task.cancel/save |
| A24 | 子弹窗 #2 AI 草稿（Standard, 320h, z=1100） | AI 生成草稿 | 叠层 | :2356-2453 | agent.dialog.task.draft |
| A24a-f | close+caption+textarea+generate btn+dismiss+notice/warnings/error | — | 弹窗 #2 | :2382-2451 | agent.dialog.task.draft.* |
| A25 | 弹窗 #3 任务详情（Form, 660h，宽屏分栏/窄屏 segmented） | 任务详情 | 全屏 | agent_detail_dialog.cpp:58-1344 | agent.dialog.detail |
| A25a | close ✕ | 关闭 | 弹窗 #3 | :112-122 | agent.dialog.detail.close |
| A25b | actionChip "停止 · 当前轮结束后生效"（仅运行中） | 停止 run | 弹窗 #3 标题行 | :127-142 | agent.dialog.detail.stop |
| A25c | 摘要三卡 ×3（bg+label+value+sub） | 最近一次/下一次/近 7 天 | 弹窗 #3 | :212-292 | agent.dialog.detail.summary.* |
| A25d | 配置头 stack（折叠头） | 任务配置 | 弹窗 #3 | :317-369 | agent.dialog.detail.config.header.* |
| A25e | segmented（窄屏：执行记录/运行详情 2 tab） | 窄屏导航 | 弹窗 #3 窄屏 | :1141-1149 | agent.dialog.detail.nav |
| A25f | 执行记录 scrollView（按天分组头+行 ×N） | 时间线 | 弹窗 #3 左/Tab 0 | :389-630 | agent.dialog.detail.runlist.* |
| A25g | run 行（bg 命中+选中底+左缘 2px+圆点+时间+状态+耗时+[错误+引导]） | run 行 | 时间线 | :490-612 | agent.dialog.detail.runlist.<i>.* |
| A25h | 20 条上限提示 | "仅显示最近 20 次" | 时间线底 | :617-627 | agent.dialog.detail.runlist.note |
| A25i | 运行详情块（状态 pill+元信息 6 行+计划对照 plan/actual/verdict+[错误块]） | run 详情 | 弹窗 #3 右/Tab 1 | :753-953 | agent.dialog.detail.detail.* |
| A25j | 消息区标题行（消息(N)+复制回复 chip+展开/收起 chip） | 消息头 | 弹窗 #3 右/Tab 1 | :681-745 | agent.dialog.detail.msgs.* |
| A25k | 消息 scrollView（展开态） | 消息列表 | 弹窗 #3 右/Tab 1 | :1020-1052,1119-1124 | agent.dialog.detail.msgscroll/detail.scroll |
| A25l | 消息行（badge+time+toggle+copy chips+收起 preview+展开 blocks ×N：text/toolUse/toolResult） | 消息内容 | 消息列表 | agent_module.cpp:1381-1528 | agent.dialog.detail.msg.<i>.*/blk.<j>.* |
| A25m | 空态/错误/折叠提示 ×N | "选择一次执行…"/"消息读取失败…"/"展开查看完整链路" | 弹窗 #3 右/Tab 1 | :961-1018,1083-1117 | agent.dialog.detail.detail.empty/msgs.error/msgs.hint/empty |
| A25n | 配置覆盖浮层 #4（scrim z=19+popover z=20，scrollView+字段行 ×N：目标/触发/定时/模型/迭代/破坏性/次数上限/配置异常/执行记忆） | 任务配置浮层 | 弹窗 #3 内 | :1158-1342 | agent.dialog.detail.config.scrim/popover+body.<field>.* |
| A26 | 弹窗 #5 删除确认（Compact, 190h, z=1100） | 删除任务 | 全屏叠层 | agent_module.cpp:2484-2533 | agent.dialog.delete |
| A26a | 取消/确认删除（Danger） | — | 弹窗 #5 底 | :2518-2531 | — |
| A27 | toast | "已复制" | 顶层 | :1959 | agent.detail.toast |

## 4. module_settings（设置页）

| # | 类型 | 用途 | 区域 | file:line | id |
|---|---|---|---|---|---|
| S1 | 页标题 | "设置" | 页头 | settings_module.cpp:1221-1228 | settings.h1 |
| S2 | 副标题（动态状态/错误） | "所有变更即时保存…" | 页头 | :1229-1239 | settings.subtitle |
| S3 | Tab（4 页签：通用/模块/日志/同步） | 顶部 tab | 页头下 | :1015-1033 | settings.tabs |
| S4 | 卡片 scrollView | 卡区滚动 | 内容区 | :1040-1058 | settings.cards.scroll |
| S5 | 外观卡 stack | 卡片容器 | 通用 tab | settings_ui_cards.cpp:99-277 | settings.card.appearance |
| S5a-b | 标题+描述 | "外观"+"主题与窗口底色即时生效…" | 外观卡 | :103-105 | settings.card.appearance.title/desc |
| S5c | dropdown 单选（主题：自动/浅色/深色） | "主题" | 外观卡 | :119-131 | settings.card.appearance.theme.dd |
| S5d | dropdown 单选（字号缩放 5 档） | "字号缩放（正文基准 15px）" | 外观卡 | :150-163 | settings.card.appearance.font.dd |
| S5e | dropdown 单选（窗口效果 3 档） | "窗口效果" | 外观卡 | :179-193 | settings.card.appearance.effect.dd |
| S5f | 降级警告图标 0xF071+文本 | 不支持时提示 | 外观卡 | :200-219 | settings.card.appearance.effect.hint.* |
| S5g | 主题色选择按钮 ×4（pill 圆点+名称） | Claude 橙/雾紫/玫瑰粉/QQ 蓝 | 外观卡 | :248-274 | settings.card.appearance.accent.sw.<i>/name.<i> |
| S6 | 编辑器字号卡 stack | 卡片 | 通用 tab | :292-336 | settings.card.editor |
| S6a | dropdown 单选（编辑器字号 5 档, openUp） | 编辑器字号 | 编辑器字号卡 | :305-323 | settings.card.editor.dd |
| S6b | 提示文本 | "Markdown 编辑器正文字号…" | 编辑器字号卡 | :326-333 | settings.card.editor.hint |
| S7 | 模块卡 stack | 卡片 | 模块 tab | :364-411 | settings.card.modules |
| S7a | 模块行 ×4（name+desc+toggleSwitch） | Markdown/Todo/Claude/智能体 开关 | 模块卡 | :377-408 | settings.card.modules.name.<i>/rowdesc.<i>/sw.<i> |
| S8 | 余量卡 stack（48h） | 卡片 | 模块 tab | :852-881 | settings.card.quota |
| S8a | label+desc+toggleSwitch | 供应商余量总开关 | 余量卡 | :855-878 | settings.card.quota.label/desc/sw |
| S9 | 笔记目录卡（仅 mdOn 显示） | 卡片 | 模块 tab | :424-500 | settings.card.directory |
| S9a | 路径框 rect+值文本 | "Markdown 笔记目录（只读）" | 目录卡 | :434-448 | settings.card.directory.path.bg/value |
| S9b | 次按钮+图标 0xF07C | "打开目录" | 目录卡 | :449-462 | settings.card.directory.open |
| S9c | 提示文本 | "修改笔记目录属于后续功能…" | 目录卡 | :464-472 | settings.card.directory.hint |
| S9d | 数据根忽略规则 label+框+值 | 防凭据误提交 | 目录卡 | :475-497 | settings.card.directory.gitignore.* |
| S10 | Claude 代理卡（仅 claudeOn 显示） | 卡片 | 模块 tab | :518-645 | settings.card.proxy |
| S10a | 标题+描述（运行态动态） | "Claude 代理" | 代理卡 | :525,539-543 | settings.card.proxy.title/desc |
| S10b | toggleSwitch | "随应用启动代理" | 代理卡右上 | :529-537 | settings.card.proxy.autostart |
| S10c | 端口 label+input+主按钮"应用"+hint | "代理端口" | 代理卡 | :549-590 | settings.card.proxy.port.* |
| S10d | 接管 label+值 | "Claude Code 接管" 状态 | 代理卡 | :594-614 | settings.card.proxy.takeover.* |
| S10e | 警告图标 0xF071+文本 ×≤2 | 上层覆盖警告 | 代理卡 | :622-641 | settings.card.proxy.takeover.warn.<i>.* |
| S11 | 日志设置卡 stack | 卡片 | 日志 tab | :663-762 | settings.card.log |
| S11a | dropdown 单选（日志保留 6 档, openUp） | "日志保留" | 日志卡 | :687-707 | settings.card.log.dd |
| S11b | dropdown 单选（日志级别 4 档, openUp） | "日志级别" | 日志卡 | :733-751 | settings.card.log.level.dd |
| S11c | 提示 ×2 | 保留/级别说明 | 日志卡 | :708-715,752-759 | settings.card.log.hint/level.hint |
| S12 | 日志查看卡（FillRemainder 高） | 卡片 | 日志 tab | :776-836 | settings.tab.log |
| S12a | 标题+次按钮+图标 0xF021 "刷新" | "运行日志（末尾 500 行）" | 日志查看卡 | :780-795 | settings.tab.log.title/refresh |
| S12b | scrollView+行文本 ×N（末 500 行+空态） | 日志内容 | 日志查看卡 | :797-834 | settings.tab.log.scroll/line.<i>/empty |
| S13 | GitHub 同步卡（动态高，未启用只画标题+描述+开关） | 卡片 | 同步 tab | github_sync_card.cpp:75-376 | settings.card.github_sync |
| S13a | 标题+描述（披露实际载荷） | "GitHub 同步" | 同步卡 | :89-110 | settings.card.github_sync.title/desc |
| S13b | label+toggleSwitch | "启用" | 同步卡 | :117-131 | settings.card.github_sync.enabled.label/sw |
| S13c | label+单行 input ×3 | URL/分支/Token | 同步卡 | :140-196 | settings.card.github_sync.url/branch/token.* |
| S13d | label+toggleSwitch | "自动推送" | 同步卡 | :201-215 | settings.card.github_sync.autopush.* |
| S13e | 主按钮+次按钮 ×2（运行中全 disabled） | "立即同步"/"测试连接"/"拉取" | 同步卡 | :221-256 | settings.card.github_sync.btn.push/test/pull |
| S13f | 进度文本（运行中） | "① 准备中…" | 同步卡 | :261-269 | settings.card.github_sync.progress |
| S13g | 错误文本+次按钮"详情" | 错误摘要+入口 | 同步卡 | :273-295 | settings.card.github_sync.error/btn.detail → 弹窗 S14 |
| S13h | 状态文本（error 空时） | 状态文案 | 同步卡 | :300-309 | settings.card.github_sync.status |
| S13i | 提示文本 ×≤5 | 分支提示/public 警告/git 不可用/未同步/删除跳过 | 同步卡 | :313-375 | settings.card.github_sync.branchhint/publicwarn/localgit/unsynced/deleteskip |
| S14 | 弹窗 GitHub 同步失败/冲突详情（ExtraWide, 560h, 变体 3：Error/Conflict409/AuthFailed） | 错误详情 | 全屏 | git_error_dialog.cpp:89-308 | settings.github.err |
| S14a-h | 标题（动态）+摘要+hint+scrollView（409 覆盖清单+请求日志行）+详情+主按钮（动态文案+409 二次确认武装）+复制详情+关闭 | — | 弹窗 S14 | :114-306 | settings.github.err.* |
| S15 | toast | 操作反馈 | 顶层 | settings_module.cpp:512-517 | settings.toast |

## 覆盖度声明

**搜索范围**：实读 module_todo/（todo_module/ui_board/ui_dialogs 全量）、module_claude_proxy/（claude_module+ui_list/ui_detail/ui_form/ui_ccswitch/ui_logs/ui_quota 全量）、module_agent/（agent_module 2604 行全+agent_detail_dialog+agent_ui_shared）、module_settings/（settings_module+settings_ui_cards+github_sync_card+git_error_dialog 全量）。

**控件关键词 grep 全集核验**：`components::(button|input|dropdown|tabs|switch|toggleSwitch|scrollView|virtualList|card|tooltip|contextMenu|datePicker|segmented)`——4 模块内零 checkbox/lineChart/barChart/pieChart/progressBar/slider/list/dataTable 命中（项目级图表/进度条在 home/markdown）。`ui.rect`/`ui.text` 作自绘控件（chip/徽章/状态点/分隔线/命中面/头像/状态 pill/序号徽章）逐条计入。`actionChip` 是 module_agent 私有自绘控件（两层 stack：bg 命中+文本穿透）。`toggleSwitch` 在本项目等价 switch（无 checkbox，因 -Werror 告警改用 switch，见 agent_module.cpp:2232 注释）。

**弹窗触发点编号**：引 dialog-inventory #1–#23；module_settings 的 GitHub Sync 错误详情弹窗（settings.github.err）是审计后的新增模态（变体 3 态+409 二次确认武装），属第 25 个模态（splash #24 不在页面范围）。

**可能遗漏**：① todo_ui_common.h/logic.h 中的纯几何/文本 helper（drawChip/chipWidth/priorityCardBg）未单列为控件（渲染 helper 非独立可点元素）；② claude_ui_state.h 等状态头不含 UI 元素未读全；③ 侧栏 app_sidebar.cpp 与托盘 main.cpp 不在 4 页范围；④ 跨页 toast app.notice 不在页面范围；⑤ Home 余量卡触发 Claude 页余量弹窗的跳转负载（quota_home_bridge.h）是路由负载非本页控件；⑥ agent 详情元信息 6 行（started/ended/duration/iterations/input/output）合并计入 A25i；⑦ agent 详情宽屏/窄屏分支差异已合并。
