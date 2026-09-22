# W7 稳定化计划（2026-09-22）

> 背景：用户实测后评价当前应用"不可用、demo 都算不上"。四模块粗糙度审计（Home/Todo/Markdown/代理+跨模块）+ 用户实测 5 条 + Windows 字体渲染诊断全部完成，本计划合并其结论。
> 前置：W6（Token 速率卡片 / Todo UX / Markdown Phase1）合并后启动本计划。
> 完成标准：**用户亲手点验通过**，不再只看测试绿。

## 一、用户实测问题（2026-09-22，全部已定位）

| # | 问题 | 根因 | 层 |
|---|------|------|-----|
| 1 | Windows 字体整体偏小 ~25% | EUI-NEO 按 hhea 度量缩放字号，Segoe UI 系数 0.75，15px 正文实际按 11.3px 渲染（`EUI-NEO/core/render/text.cpp:641-646`） | 上游 |
| 2 | Home 设置按钮点击无反应 | 过时 disabled（设置页早已落地），且 EUI-NEO disabled 不自动置灰（`src/main.cpp` 旧注释 + KNOWN_LIMITATIONS） | DevDesk |
| 3 | 侧边栏文字不居中 | W5 新代码漏 `verticalAlign(Center)`，EUI-NEO text 默认顶部对齐，`lineHeight` 不做垂直居中（`src/main.cpp:285-293, 314-321`） | DevDesk |
| 4+5 | 浅色模式下 Todo/Claude 页背景全黑 | 五模块只有设置页铺页面底色（`settings_module.cpp:208`），其余浮在启动时快照的 clearColor 上（`main.cpp:332-336`，EUI-NEO 无运行时改 clearColor API） | DevDesk（+上游候选） |

## 二、P0 — 可用性阻断（本轮必做）

1. **浅色黑底**：Home/Markdown/Todo/Claude 四模块 compose 各铺一行 `pagebg` rect（仿 `settings_module.cpp:208`）。上游候选：EUI-NEO 增加运行时 clearColor API（另立 PR，不阻塞本项）。
2. **字体缩水止血**：DevDesk 默认字号缩放调 ~130%（`theme::setFontScale`，一行配置）。根治走上游 PR（见第四节）。
3. **侧边栏居中**：`main.cpp` 导航项 + brand 文字加 `.verticalAlign(eui::VerticalAlign::Center)`（两行）。
4. **设置按钮**：接 `navigateTo(PageId::Settings)`（一行）；全局 grep 其他 disabled 按钮补 `textDisabled` 色。
5. **mtime epoch 回归**（`markdown_service.cpp:157-159`）：MinGW file_clock epoch 非 1970，写库前经 `system_clock` 基准转换；补断言 `abs(modified_at - time(nullptr)) < 86400` 的测试（当前 137 全绿是假阴性——测试用 SQL 直插合成时间戳绕过了转换链路）。
6. **Todo position=0 数据缺陷**（`todo_module.cpp:890` 等全部移动路径）：编辑保存保留原 position；移动按目标索引计算；补列内滚动（当前第 12 张卡起从界面消失，`todo_module.cpp:463`）。
7. **代理流静默截断 + 5min 总超时**（`streaming_proxy.cpp:248-252`、`proxy_router.cpp:309`）：Error 分支合成 `event: error` SSE 帧或异常关闭连接；总超时改空闲超时（`CURLOPT_LOW_SPEED_LIMIT/TIME`）。

## 三、P1 — 呈现与反馈（"粗糙感"主来源）

1. **错误态统一**：以 Home 三态（空/错误+重试指引/数据）为范本，改造 Todo（`todo_module.cpp:125-139` 静默空态）、Claude（`claude_module.cpp:82-102` 静默吞错）、Markdown。
2. **Markdown 保存安心感链路**（`markdown_module.cpp:609-634`）：保存失败指数退避（当前每秒 60 次重试风暴）+ toast 反馈 + 退出前失败阻断或明示；加保存状态指示（spec §5.6 右下角状态点 + 状态栏）。
3. **Home 数据陈旧**：进页重查（Router 加页面进入通知或 dirty 标记，`router.cpp:38-47`）。
4. **字号缩放断裂收敛**：Home 行高/卡高随 `fontScale()` 派生（`home_module.cpp:56-78`）；各模块 ~40 处字号魔数换 token。
5. **死链接清理**：Claude"查看全部 →"（`claude_module.cpp:647-654`）接 onClick 或删除；代理未运行时副标题显示假端口（`claude_module.cpp:78-80`）。
6. **contentKey 同长度替换 bug**（`markdown_module.cpp:459-460`）：key 改用文本 hash（一行）。
7. **深色主题全页铺底**：与 P0-1 同一改动，验收含深浅两主题四页截图。

## 四、EUI-NEO 上游 PR（并行轨道，不阻塞 W7）

1. **字号 em 语义修复**（根因见第一节 #1）：`loadFontFace` 对 scalable 字体 fontSize 直接作 em 尺寸（CSS 语义），`pixelHeightScale` 恒 1。属框架级语义变更：升版本号 + examples 回归 + 文档（按 EUI-NEO PR 清单）。
2. 候选：运行时 clearColor API；text 在显式 size 容器内默认居中（API 语义讨论）。

## 五、P2 — 功能补完（排 W8，不在本轮）

W6 覆盖：Todo 键盘/右键/列管理/datepicker、Markdown 三态/大纲/同步滚动/状态栏、Token 速率卡片。
W6 未覆盖（排 W8）：Todo 搜索入口/过期视图/tags 编辑入口；Markdown 目录树/标签系统/外部变更感知/FTS 符号清洗；代理日志 token 列 + 流结束回写（与 token 卡统计口径一并做）；错误 JSON 转义；>10MB 请求体 413。

## 六、验收标准

1. 137+W6 新增测试全绿，且新增防假阴性测试（mtime 链路、position 保留）
2. 浅色/深色两主题下四模块 + 侧边栏截图，无黑底、无裁切、文字居中
3. 字号观感恢复设计预期（15px 正文实际 15px）
4. **用户亲手点验清单通过**（终验 agent 产出清单，用户逐项确认）
