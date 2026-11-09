# 2026-11-07 批次 · 老板手测清单

> 批次 = 15 个池号（#158/161/163+177/169/171/178/185/186/187/191/194/212/214/215），HEAD=`0e634533`。
> 机器面已验：doctest 全量 2267/2259 绿、probe 38 条 37 绿（1 条已知红 bug142_click_chain_probe，探针自身 scan 缺陷非产品回归，clue 92）、守卫链 6/6 + 守卫自检 5/5 绿。
> 本清单只列需要真人看的项。每条过完在 `[ ]` 打勾；不符合期望的直接在条目下写一句现象即可。
>
> 前置：先装含 HEAD 的新构建（关闭 app 后把 `build/DevDesk.app` 覆盖 `/Applications/DevDesk.app`，或 `bash scripts/build-test-run.sh --install`；Windows 机按装机口径拷 Debug 产物）。**Dock 图标项（D-1）必须重打包后看，旧安装不算数。**

---

## 一、Markdown 模块

### M-1 编辑器字号步进（need161）
- [ ] 入口：侧栏「Markdown」→ 打开任意 md 文件 → 看编辑器右下状态栏
- 操作：
  1. 点状态栏字号控件（原下拉位置已升级为 A− / A+ 步进对）
  2. 连点 A+ 到顶、连点 A− 到底
  3. 完全退出 DevDesk 再打开，回到 Markdown 页看字号
- 期望：字号随点击连续变大/变小（12–24 范围）；到 12 / 24 边界时对应按钮变禁用态；重启后字号保持上次值。

### M-2 tooltip 悬浮位置（bug163 MD 侧）
- [ ] 入口：Markdown 页，把鼠标悬到工具栏图标 / 目录树节点等带提示的元素上
- 操作：逐个悬浮，停留约 1 秒
- 期望：tooltip 出现在触发元素近旁、不压住编辑区正文、不飘到屏幕别处；移开后消失。

## 二、待办 Todo

### T-1 所有列支持独立折叠（need169）
- [ ] 入口：侧栏「待办」
- 操作：
  1. 点任意一列的列头
  2. 再点同一列头展开
  3. 同时折叠两列（含试折叠「归档」列以外的普通列）
- 期望：点列头该列收起（动画与归档列同款手感）；再点还原、内容无丢失错位；多列可同时处于折叠态；折叠/展开时其余列回流无空洞。

## 三、上线分组 · 任务页

### R-1 tab 紧排 + 计数右缘（池191）
- [ ] 入口：侧栏「上线」分组 → 任务页
- 操作：观察顶部四个 tab 行；依次点四个 tab 切换；对比切换前后计数位置
- 期望：四个 tab 紧凑排左侧；数量计数固定钉在行最右端右对齐，切 tab 时计数**不**跟着选中 tab 移动。

### R-2 新建任务弹层重开即空表单（BUG212）
- [ ] 入口：任务页 → 「新建任务」
- 操作：
  1. 填写表单（含日期）→ 提交建一个任务
  2. 再点「新建任务」打开弹层
- 期望：第二次打开是**空表单**，日期锚点重置为今天，不残留上一次输入。

## 四、定时 Schedule

### S-1 tab 行计数右缘（池171）
- [ ] 入口：侧栏「定时」列表页
- 操作：观察 tab 行；切换各 tab
- 期望：与 R-1 同范式——tab 紧排左对齐，计数钉行右缘、切换不位移；计数字号比 tab 文字小一号（次级色）。

## 五、MCP

### P-1 可用工具弹窗瀑布流（need178）
- [ ] 入口：侧栏「MCP」→ 打开「可用工具」弹窗
- 操作：
  1. 正常宽度看卡片排列
  2. 拖窄窗口再看；拖宽再看
- 期望：工具卡片按瀑布流多列排布（各列顶部对齐、卡片高度不一时间隙被利用）；窗口变窄列数变少、变宽列数变多；无横向溢出、无卡片重叠。

## 六、启动代理 Claude

### C-1 导入 profile 预检 + 状态槽（need185）
- [ ] 入口：启动代理页 → 新建 profile
- 操作：
  1. 填一个**不通的**地址/假凭证 → 点确认
  2. 观察 5 秒内的反馈；再填一个正确可达的配置提交
- 期望：确认后先进入「预检中…」（按钮变文案、表单出现状态槽进度行）；预检失败 → 红字给出明确原因（如认证失败/连不上），**不建库**；预检通过才真正创建。表单不再出现 hint/error 散排的旧布局。

### C-2 tooltip 悬浮位置（bug177 启动代理侧）
- [ ] 入口：启动代理页，悬浮各带提示的控件
- 期望：tooltip 锚在控件近旁，不遮挡表单关键行，与 M-2 同标准。

## 七、记忆

### N-1 触发线基线磁贴含义标注（bug187）
- [ ] 入口：记忆页 → 配置 tab → 「触发线」卡
- 操作：在 100% 与 140% 字号档（设置页字号缩放）各看一次该卡
- 期望：基线磁贴数字（如 5/3/1 或两位数 11）完整显示不被截断、不坠出卡片底部；卡内有标注行说明「记词基线」含义（磁贴是干什么的一眼能懂）。

### N-2 脱敏卡 / 用量卡字号档（bug215）
- [ ] 入口：记忆配置页 → 脱敏卡、用量卡
- 操作：切 140%（及 100% 复位）字号档各看一次
- 期望：两卡内容随字号放大仍完整在卡内，无溢出、无压线（与 N-1 同款修法）。

### N-3 召回评测（need186，可选）
- [ ] 入口：记忆页主控台 → 触发线卡「跑评测」按钮（需记忆引擎 Running）
- 期望：点后后台跑评测，界面出 Recall@5 分数（阈值 85% 红线）；评测数据用后自动清理，不污染正常记忆。

## 八、动态背景 / 壁纸

### W-1 动态背景 20fps 手感 + 三处即停（bug194）
- [ ] 入口：设置 / 壁纸页 → 开启动态背景
- 操作：
  1. 回首页观察背景流动手感（约 30 秒）
  2. 打开设置页停一会 → 活动监视器看 CPU
  3. 最小化窗口 → 再看 CPU
  4. 关闭窗口藏入托盘 → 再看 CPU
- 期望：背景流动顺滑无明显卡顿/拖影（20fps 观感可接受）；**设置页打开、窗口最小化、藏托盘三种任一情况发生时背景动画即停**（CPU 明显回落，空闲接近 0%）。

## 九、系统集成

### D-1 Dock 图标等大（bug158，必须重打包）
- [ ] 前置：`bash scripts/package-macos.sh`（或 `--install`）重新组装安装；仅覆盖 exe 不重建 bundle 看不到新图标
- 操作：装好后看 Dock 中 DevDesk 图标，与旁边其它 app（ Finder / 浏览器等）对比大小；必要时重启 Dock（`killall Dock`）
- 期望：图标视觉尺寸与系统其它 app 等大（HIG 824 网格对齐，无缩水/发虚）；DMG 卷窗口图标同步为新图。

## 十、2026-11-08 新批次补录（HEAD=`aea3945d`）

> 新增批次：#144+168 / #174-176 / #182-184 / #196+218 / #217 / #225 / #229 / 全站列表统一批 1·3·4·5·6·7·markdown 补批。
> #161 字号步进、#169 全列折叠已在上方 M-1 / T-1 在列（HEAD `0e634533` 批次已含），不重复追加。

### X-1 Profiles 卡重设观感（#174/175/176）
- [ ] 入口：侧栏「启动代理 Claude」→ Profiles 列表卡 → 任一 profile 详情（含环境变量卡）
- 操作：亮 / 暗主题各看一遍列表卡与详情环境变量卡
- 期望：新卡设计落地——层级清晰、亮暗两态均无对比度 / 溢出问题（机器面对照：`evidence/profiles-batch/` 8 图）。

### X-2 引擎按钮态与审核卡标注（#182/184）
- [ ] 入口：记忆页 → 引擎卡 → 审核 tab
- 操作：
  1. 找未装好 embedding 的环境（或删引擎组件）看引擎卡按钮态
  2. 引擎未运行时打开审核页
  3. 看一条未提炼的采集提案卡
- 期望：未装好**不再误显「关闭引擎」**，显失败态 + 修复引导（模型下载失败给手动放置路径）；审核页未运行时出降级横幅；未分类片段标「原始片段（未分类）」并给处理建议，不再虚标 fact。

### X-3 splash 1.2s 过渡观感（#196）
- [ ] 入口：完全退出 DevDesk → 重新启动
- 操作：连续重启两三次观察启动序列
- 期望：淡入 → 进度保持 → 交叉淡化 → 主界面，总时长约 1.2s（原 1.93s），序列连贯无黑帧 / 跳变（机器面序列帧：`evidence/sidebar-splash-occlusion/splash-seq-01..09`）。

### X-4 全站玻璃卡与 8px 间距巡览（#217+池224 批 2/4/5/6/7+markdown 补批）
- [ ] 入口：定时 / 启动代理 / Agent / 记忆 / MCP / 首页 / 待办 / 设置 / Markdown 逐页巡览
- 操作：亮暗两态各过一遍，重点看卡面材质与卡间距、页头字重
- 期望：卡面统一玻璃材质通路（无突兀实色卡）；卡垂直间距 8px 一致；页头 / 统计大数 600、佐证 400 两档制，无残留 700 加粗页头；无内容挤压重叠。

### X-5 真锁屏 CPU 安静（#218）
- [ ] 入口：开动态背景 → 锁屏（合盖或 Ctrl+Cmd+Q）
- 操作：锁屏保持 1 分钟 → 解锁 → 活动监视器看 DevDesk CPU，对照解锁后数值
- 期望：锁屏（窗口遮挡态）期间流质泵挂起、CPU 明显回落接近空闲，解锁后恢复。需真人锁屏；机器面已验 occlusion 接线（`evidence/sidebar-splash-occlusion/occlusion-pump-evidence.log`）。

### X-6 新装机 llama 零下载（#225）
- [ ] 入口：DMG 新装 → 记忆页 → 引擎启用向导
- 操作：用打包产物（见 X-7）全新装机走引擎向导
- 期望：llama-server 从包内 `Contents/MacOS/assets/llama/bin` 命中，**不需要** brew 安装或运行时额外下载；模型权重首次仍需下载（属预期）。（机器面：`evidence/llama-bundle/` 三件套）

### X-7 DMG 转正打包（#225 配套，老板终端执行）
- [ ] 入口：终端（老板本人）
- 操作：`bash scripts/package-macos.sh` 跑一次打包，完成后开 DMG 看 app
- 期望：脚本 exit 0 无红；DMG 内 app 含 `Contents/MacOS/assets/llama/bin`；装机后可复验 X-6。

### X-8 MCP 三列格阵位置确认（#151）
- [ ] 入口：侧栏「MCP」→ 工具格阵页
- 操作：正常宽度与拖宽窗口各看一次
- 期望：三列格阵位置符合预期（批 7 网格行间 24→8 / 行内 16→8），间距收紧后三列仍在位，无重叠 / 溢出 / 错行。

## 无手测项条目（机器面已覆盖）

| 池号 | 内容 | 为什么不用手测 |
|---|---|---|
| bug214 | agent_ui OS 系统通知用例 MinGW 线程可靠性 skip 守卫（30bf7e84） | 纯测试代码，Windows CI 面；macOS 本机该用例本就是 skip 形态。CI 红记录在位：`evidence/agent-ui-notification-ci/` |

---

## 附：机器面证据索引（evidence/，已 git-ignore）

| 池号 | 证据目录 |
|---|---|
| 158 | `evidence/dock-icon/`（compare-before/after.png + measurements.txt + icns 组） |
| 161 | `evidence/md-font-entry/`（before-dropdown / after-stepper.png） |
| 163+177 | `evidence/tooltip-anchor/`（md-after / claude-after.png） |
| 169 | `evidence/todo-collapse/`（before + 两折叠态，1600×1000） |
| 171+191 | `evidence/tab-batch/{before,after}/`（全站 17 页对照） |
| 178 | `evidence/tools-dialog-masonry/`（宽窄 × before/after 4 图） |
| 185 | `evidence/import-precheck/`（form-before/after/after-precheck/after-fail.png） |
| 186 | `evidence/recall-cards/`（before/after 对照，映射按目录名+clue 135 推定） |
| 187 | `evidence/memory-cards-scale/`（before/after 各 11 图，含 140% 档；clue 134 红绿记录） |
| 194 | `evidence/fluid-bg-fps/`（base/after4 fps 日志 + README + 提取） |
| 212 | `evidence/create-task-reset/`（README + before-red / after-green.txt） |
| 214 | `evidence/agent-ui-notification-ci/`（两轮 CI 红记录 + timeline） |
| 215 | `evidence/memory-cards-scale/after/memory-config*.png`（clue 134 同族修法，commit 0e634533） |
| 本轮 QA | `evidence/2026-11-07-qa216/`（fullrun-mac.log / guardchain-6guards.log / probe-ctest-LastTest.log / probe-bug142-knownfail-shot.png） |
| #144+168 | `evidence/sidebar-toggle-batch/`（四档窗口 toggle 截图） |
| 池224/need219 批1 | `evidence/unify-batch1/`（ui_kit 核对结论 md，零改动批） |
| 批3 | `evidence/unify-batch3/`（settings before/after × 亮暗） |
| 批4 | `evidence/unify-batch4/`（schedule+claude 12 图） |
| 批5 | `evidence/unify-batch5/`（agent 12 图） |
| 批6 | `evidence/unify-batch6/`（memory 亮暗 / fs140 前后 6 组） |
| 批7 | `evidence/unify-batch7/`（mcp/home/todo 22 图） |
| markdown 补批 | `evidence/unify-md/`（21 文件含 raw-after） |
| #196+218 | `evidence/sidebar-splash-occlusion/`（splash 9 帧 + occlusion-pump-evidence.log） |
| #217 | `evidence/fluid-lowres/`（before-sharp/after-lowres）+ `evidence/fluid-bg-fps/`（fps 日志扩容） |
| #225 | `evidence/llama-bundle/`（bundle-list / otool-closure / isolation-run） |
| #182+184 | `evidence/memory-engine-batch/`（12 图：向导 / 引擎卡 / 审核卡） |
| #229 | `evidence/mcp-stale-test/`（red-before-fix + 3 green） |
| #174-176 | `evidence/profiles-batch/`（list / detail-env 亮暗前后 8 图） |
