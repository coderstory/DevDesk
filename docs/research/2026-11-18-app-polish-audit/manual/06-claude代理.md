# 06 Claude 代理（Claude 配置页）

> 手册走查底稿。以源码实况为准（核对基准日 2026-11-18）；与 docs/USER_GUIDE.md 冲突处已标注「差异」。

## 一句话

管理多套 Claude API 配置（Profile）+ 本地带鉴权流式代理；启动代理即自动接管 Claude Code 的 settings.json，停止即按快照还原。

## 入口与前置（模块开关/门控）

- 侧边栏「Claude 配置」（模块显示名，src/module_claude_proxy/claude_module.cpp:41）。受设置页模块开关「Claude 配置切换器」控制。
- 会话内刚开启模块时服务可能未装配：toast「Claude 服务未装配 / 本会话内开启的模块需重启后才能提供代理服务」（claude_module.cpp:107-108）。
- 代理**默认不启动**（onStart 只记日志不拉起，claude_module.cpp:200）；启动方式：本页按钮或设置页「随应用启动代理」。

## 主流程步骤（编号：操作→预期可见结果）

1. 进入页面→左栏「Profile 列表」、右上角状态点（绿点「运行中 :<端口>」/灰点「已停止」，副标题「本地代理运行中，管理多套 Claude API 配置」或不含「本地代理运行中」，claude_ui_list.cpp:414-415、506-507）。代理未启动时页面有引导条：「启动代理将接管 Claude Code 配置（…指向本地代理），停止代理时自动还原」（claude_ui_list.cpp:536-538）。
2. 「+ 新建 Profile」→ 弹「新建 Profile」弹窗（三页签「基础信息 / 模型配置 / 环境变量」；提交钮文案：首建为「创建」且提交后先做导入预检——「正在验证可用性（连通性/凭证，最长 5 秒）…通过后自动创建」，预检期间变「预检中…」，claude_ui_form.cpp:315、518-521）。通过→自动建库；当前无激活 Profile 时自动激活（toast「已创建并启用」）。
3. 「导入当前配置」→ 读 ~/.claude/settings.json 解析托管键预填新建表单（表单错误红字：「服务未就绪（ClaudeService 未注册）」等，claude_module.cpp:633）。
4. 选中 Profile → 右栏「当前 Profile 详情」，页签「基础信息 / 模型配置 / 环境变量」（claude_ui_detail.cpp:694）：名称/模型/Base URL/API Key 脱敏（前 6+***+后 4，带 [本机库]）；接管状态行「● Claude Code 已接管 <路径>」或「○ Claude Code 未接管（启动代理后自动接管，停止时还原）」（claude_ui_detail.cpp:221-222）；接管警告行「（共 N 条，详见设置页）」（claude_ui_detail.cpp:230-232）。
5. 「测试连通」→ 后台 POST `<Base URL>/v1/messages` 1-token ping（**15 秒超时**）。结果 toast/标题：「连通成功 / 上游返回 HTTP 2xx，API 可用」；失败「连通异常」+分类提示——401/403「（API Key 被上游拒绝）」、404「（接口不存在或模型不可用，检查 Base URL 与模型配置）」、其余「（检查 API Key 或 Base URL）」（claude_module.cpp:577-588）。**差异**：USER_GUIDE §3.4 写「对 /v1/models 发 GET」已过期——探测已改 POST /v1/messages（旧探测把 bailian 等网关误报「连通异常」，claude_service.cpp:248-252）。
6. 「启用 / 设为当前」→ 弹「切换 Profile」确认弹窗（「切换到"<名称>"？代理配置将立即生效」，按钮「确认切换」/「取消」，claude_ui_list.cpp:745-757）；确认后 toast「已启用 / 该 Profile 已设为激活（ACTIVE）」（claude_module.cpp:378）。代理运行中会按新 Profile 重新接管 settings.json。
7. 「启动代理」→ 先做启动前置校验（2 项：激活 Profile 存在（零网络）；对激活 Profile 发连通探测，**2 秒超时**，非 2xx 算不过，claude_service.cpp:344、144）。期间 toast「正在校验 / 检测激活 Profile 与上游连通性（最长 N 秒）…」（claude_module.cpp:350-353）。通过→bind + 接管 + toast「代理已启动 / 监听 http://127.0.0.1:<端口>」（claude_module.cpp:615-616）；校验/启动失败→「启动代理失败」（claude_module.cpp:611）。
8. 代理运行期间照常用 Claude Code（被接管键直连本地代理；Bearer 鉴权，Token 见设置页）。详情底部最近调用表 +「查看全部 →」开「调用记录」弹窗（搜索、状态码过滤、分页每页 20 条）。
9. 「停止代理」→ 停止监听并按快照还原 settings.json，toast「代理已停止 / 本地代理已停止监听」（claude_module.cpp:343）。
10. 「运行」（详情区按钮）→ 选工作目录后**以该 Profile 直连打开一个 Claude Code 会话**（临时配置、不改全局设置）：toast「已启动 / 已用该 Profile 直连打开 Claude Code 会话（临时配置会话结束后自动删除，未改动任何全局配置）。」（claude_module.cpp:449-471，详情按钮文案与说明 claude_ui_detail.cpp:599、625、636）。
11. Profile 卡右键菜单：`编辑 / 建副本 / 复制配置 / 删除 / 设为当前`（claude_ui_list.cpp:785）。删除→「确认删除？」弹窗「将删除 Profile「<名称>」及其保存的 API Key，不可恢复。」；删的是激活 Profile 时同时停代理并还原配置。
12. 「模型配置」页签：槽位/上游模型名/显示名；1M 上下文勾选即时生效，toast「已更新 / 「<槽位>」的 1M 上下文标记已开启/关闭」（claude_module.cpp:418-420）。快捷键 Ctrl+N 打开新建表单（claude_module.cpp:244）。

## 弹窗·菜单·浮层清单

| 名称 | 触发 | 内容 | 关闭方式 |
|---|---|---|---|
| 新建/编辑 Profile | 「+ 新建 Profile」/右键「编辑」/Ctrl+N | 三页签表单；底部「创建/保存/预检中…」+「取消」 | 提交或「取消」 |
| 确认删除？ | 右键「删除」 | 「将删除 Profile「<名称>」及其保存的 API Key，不可恢复。」按钮「确认删除/取消」 | 两按钮 |
| 切换 Profile | 「设为当前」/「启用」 | 「切换到"<名称>"？代理配置将立即生效」按钮「确认切换/取消」 | 两按钮 |
| 调用记录 | 详情「查看全部 →」 | 搜索框「搜索路径 / 方法…」+状态码过滤+分页 | 弹窗关闭钮 |
| 引导条 | 代理未启动时页面常驻 | 「启动代理将接管 Claude Code 配置…停止代理时自动还原」 | 启动代理后消失 |
| Profile 右键菜单 | 列表卡右键 | 编辑/建副本/复制配置/删除/设为当前 | 点击项或移开 |

## 空态·错误态

- 列表空：「暂无 Profile，点右上角 + 新建」（claude_ui_list.cpp:636）；列表读失败：「Profile 列表加载失败：<原因>」（claude_ui_list.cpp:626）。
- 服务未注册：「服务未就绪（ClaudeService 未注册）」（toast/表单红字，claude_module.cpp:336、633）。
- 无激活 Profile：详情名称显示「(未启用)」、Key「(无激活 profile)」（claude_module.cpp:128-133）。
- 未选中 Profile 点操作：toast「测试连通失败 / 编辑失败 / 运行失败——请先新建或选择一个 Profile」（claude_ui_detail.cpp:571-608）。

## 设置项联动

- 设置页「Claude 代理」卡（仅 Claude 模块开启时显示）：「随应用启动代理」开关（下次启动生效）；「代理端口」改后回车或点「应用」，**运行中自动重启（Token 不变）**（settings_ui_cards.cpp:638-688）；认证 Token 只读脱敏+眼睛显形+「重新生成」；「Claude Code 接管」胶囊「已接管/未接管」+接管文件路径；「同步警告」区透出接管/还原失败警告（settings_ui_cards.cpp:734）。
- 端口非法（越界）自动回落默认 8888 并 LOG_WARN（proxy_runtime.cpp:112-115）。

## 异常与边界场景表

| 场景 | 预期用户所见 | 现状判定 | 代码锚点 |
|---|---|---|---|
| 测试连通超时（15s） | 「连通异常」类提示（网络层失败归入「连不上——<原因>」分类文案） | 已处理 | src/module_claude_proxy/claude_service.cpp:236、238；src/module_claude_proxy/claude_module.cpp:581-588 |
| 启动前置校验探测非 2xx | 「启动代理失败」（校验 2/2 不过，非 2xx 同样算不过） | 已处理 | src/module_claude_proxy/claude_service.cpp:344-352、144 |
| 流式运行中上游断流/超时 | Claude Code 侧收到合成 `event: error` SSE 帧（可感知截断，不静默当正常结束）；DevDesk 页面无专门提示 | 半处理（客户端可感知，但上游读为阻塞 readChunk、未见空闲超时机制，长挂流只能靠对端断开） | src/module_claude_proxy/streaming_proxy.cpp:255-264；src/module_claude_proxy/streaming_proxy.h:94 |
| 流式/非流式转发上游返回非 200 | 未知（转发对非 200 的页面/调用日志形态未核） | 未知 | src/module_claude_proxy/proxy_router.cpp（转发主路径，未核状态码透传与用户可见文案） |
| 端口被占用 | toast「启动代理失败」（bind 失败→AlreadyExists，含端口值） | 已处理 | src/module_claude_proxy/proxy_runtime.cpp:46-51；src/module_claude_proxy/claude_module.cpp:611 |
| 启动代理/接管 settings.json 时 Claude Code 正在跑 | Claude Code 官方支持运行中会话热重载配置（USER_GUIDE §4.2）；DevDesk 侧不检测 Claude Code 进程，写完即生效 | 半处理（依赖官方热重载行为，DevDesk 无提示/无冲突检测） | src/module_claude_proxy/settings_takeover.cpp:46-69 |
| 停止代理还原 settings.json 失败 | 还原失败并入警告：详情警告行「（共 N 条，详见设置页）」+设置页「同步警告」区 | 已处理 | src/module_claude_proxy/settings_takeover.cpp:93-96；src/module_claude_proxy/claude_ui_detail.cpp:230-232 |
| 异常退出遗留接管快照 | 下次启动补还原（先于任何 startProxy）；**补还原失败仅 LOG_WARN，无 UI 提示** | 半处理（成功路径已处理，失败路径用户不可见） | src/module_claude_proxy/settings_takeover.cpp:31-39 |
| Profile API Key 无效 | 「连通异常 +（API Key 被上游拒绝）」（401/403 分类）；无 Key 时发未授权请求以 401 验证 | 已处理 | src/module_claude_proxy/claude_module.cpp:584；src/module_claude_proxy/claude_service.cpp:254-259 |
| 代理运行中改端口/改激活 Profile | 改端口自动重启代理（Token 不变）；切换激活 Profile 同步重写接管键 | 已处理 | src/module_settings/settings_ui_cards.cpp:667-688；src/module_claude_proxy/claude_service.cpp:124-127 |
| 流式调用进行中改配置/停代理 | 未知（进行中请求的处理未核） | 未知 | src/module_claude_proxy/streaming_proxy.cpp（收尾/取消路径，未核配置热切换） |
| 无激活 Profile 时客户端调用 | 503 + no_active_profile（中文指引去 Claude 页） | 已处理 | src/module_claude_proxy/claude_service.cpp:347-352；docs/USER_GUIDE.md §7 |
| env_var_hints.h 中的 API_TIMEOUT_MS / CLAUDE_STREAM_IDLE_TIMEOUT_MS | 只是透传给 Claude Code 的环境变量说明文案，非 DevDesk 自身处理 | 已处理（口径已澄清） | src/module_claude_proxy/env_var_hints.h |

## 演练卡壳点

1. 走查「测试连通」时勿按 USER_GUIDE §3.4 期待 /v1/models 探测——实际 POST /v1/messages（15s）。
2. 「运行」按钮会弹**系统目录选择对话框**（platform::folderpicker）——独立系统弹窗不在 devdesk_ui_driver 射程内，驱动脚本走查需人工介入（claude_module.cpp:449）。
3. 首建 Profile 走「预检中…（最长 5 秒）」异步链，且「字段被改→以当前值重新预检」存在收敛循环；走查时在预检中修改字段验证重检行为（claude_module.cpp:670-675）。
4. 新建表单提交按钮随状态变文案（创建/保存/预检中…），断言脚本需按态匹配（claude_ui_form.cpp:518-521）。
5. 引导条文案含实际接管路径拼接（claude_ui_list.cpp:536-538），不同机器路径不同，断言用 contains。

## 2026-11-18 打磨后状态

> 对账口径：本章正文 = 批次前基线（commit 64378979）；本节为打磨专项增量，依据池台账与 `git log 63900b3e..HEAD`，零发明。

### 行为变化（变化前 → 后）

1. **流式空闲看门狗**：上游读为阻塞 readChunk、长挂流只能靠对端断开 → 空闲看门狗判死，默认 300s、env 可调（need#290，`92ca67ae`）。异常表「流式断流/超时=半处理」行升级「已处理」。
2. **凭据通道**：claude profile 旧凭据 keychain 回退读移除，凭据统一落 `app_secrets` 专表（keychain 全禁，老板令 2026-11-18；need#304 测试门控 + #305 一期运行时切换，待验收）。
3. **USER_GUIDE 漂移已修**：need#299 全量重写 USER_GUIDE（漂移×8 全修）——本章卡壳点 1 所指「/v1/models 过期探测」口径已在 USER_GUIDE 更新为 POST /v1/messages（现文 §148/§295），手册与指南已一致。

### 卡壳点对账

1. 卡壳点 1（勿按 USER_GUIDE 旧口径期待 /v1/models）：**已消解**——need#299 重写后指南与实现一致。
2. 卡壳点 2（「运行」按钮系统目录选择框在驱动射程外）：仍在。
3. 卡壳点 3（首建预检收敛循环）：仍在。
4. 卡壳点 4（提交按钮随态变文案）：仍在。
5. 卡壳点 5（引导条路径拼接断言用 contains）：仍在。
