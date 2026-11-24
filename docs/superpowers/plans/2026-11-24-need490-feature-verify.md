# need#490 网页终端 F-01～F-28 功能核验（researcher 只读）

- 日期：2026-11-24；基线：dev 0e40903d
- 依据：池 PRD v13（slug `2026-11-23-web-terminal`）+ 实际源码
- 方法：静态走读 + 已有单测/E2E 清单；未跑构建、未真机

## 1. 计数

| 分类 | 数 | 条目 |
|---|---|---|
| 已实现且可验（自动化证据可复跑） | 6 | F-01 F-08 F-10 F-11 F-22 F-25 |
| 已实现但未验（代码在，仅单测/无真链路） | 17 | F-02 F-03 F-05 F-07 F-09 F-12 F-14 F-15 F-16 F-18 F-19 F-20 F-21 F-23 F-24 F-26 F-28 |
| 未实现 / 与 PRD 不符 | 5 | F-04 F-06 F-13 F-17 F-27 |

## 2. 逐条核验

| ID | 结论 | 实现位置 | 可验证方式 / 缺口 |
|---|---|---|---|
| F-01 专区骨架 | 已实现且可验 | `src/module_settings/web_access_card.cpp:200-505`；注册 `settings_module.cpp:140` | probe `tests/probes/web_access_probe.cpp` 四态截图 |
| F-02 开关/端口 | 已实现但未验 | `web_access_card.cpp:230-299`；`settings_module.cpp:1582-1598,1542-1578`；`web_terminal_service.cpp:105-145` | 端口占用文案仅「端口被占用或无法监听」（无端口号/两类区分）；端口不持久化（`setConfiguredPort` 仅内存，`web_terminal_service.cpp:1090-1095`） |
| F-03 地址展示/刷新 | 已实现但未验 | `web_terminal_service.cpp:221`（每快照重取 `localIpv4Addresses`）；`web_access_card.cpp:306-345` | 需真机换网/改端口；复制回调 `copyWebAddress` |
| F-04 二维码/倒计时 | **未实现（P0）** | 编码器 `web/qr/qr_encode.cpp`；绘制 `web_access_card.cpp:350-420`；**快照从不填 qrMatrix** `web_terminal_service.cpp:216-231` | `qrSize/qrMatrix` 全仓仅测试/probe 赋值；界面永远「二维码生成中…」 | 
| F-05 绑定管理/自诊 | 已实现但未验 | `web_access_card.cpp:423-505`；`web_terminal_service.cpp:803-822` | 删除确认/绑定时间/最近登录在；自诊断言「127.0.0.1:port」与实际 0.0.0.0 不符；onlineCount=握手会话数（含未登录） |
| F-06 扫码绑定页 | **不可达（P0）** | 后端 `web_terminal_service.cpp:683-741`；前端 `protocol.js:587-618`；`app.js:355-420` | 逻辑完整（claim 幂等/5 次作废/三态），但桌面无二维码可扫，链路无入口 |
| F-07 TOTP 登录 | 已实现但未验 | `web_terminal_service.cpp:625-677`；TOTP ±1 步 `web_auth.cpp:152-160` | 单测 `webAuthTotpRfc6238/webAuthLoginLockout`；刷新免登录失败（见 F-09） |
| F-08 本机免验 | 已实现且可验 | `web_terminal_service.cpp:462-465` | E2E `handshake+loopback-login` |
| F-09 票据/退出/上限 | 已实现但未验（含缺口） | 签/验/吊销 `web_auth.h:79-123`；四上限 `web_auth.h:127`, `web_bridge.h:20`, `web_crypto_session.h:18`；退出 `web_terminal_service.cpp:677-682` | 单测覆盖 64 上限/限流；**浏览器无 localStorage、服务端未调 `saveToFile/loadFromFile`** → 刷新/重启即重输码，违 P-12/F-09 |
| F-10 渲染/滚动 | 已实现且可验 | `web_grid.cpp`；`render.js:72-233`；滚动恢复 `app.js:180-183` | E2E grid-frame + resync |
| F-11 推送/重连/探活 | 已实现且可验 | `protocol.js:672-900`；`app.js:794-800` | E2E auto-reconnect/resync；补发队列按 256 条非 4 KB（`protocol.js:401`），文案「待发送队列已满」（`app.js:258`） |
| F-12 输入/选择/粘贴 | 已实现但未验 | IME `app.js:94-124`；长按 500ms `app.js:128-155`；粘贴框 `app.js:532-556` | 需真机中文输入法/长按 |
| F-13 tab 列表/切换/改名 | **与 PRD 不符** | 列表/切换 `app.js:431-509`；改名 `app.js:469-502`；服务端 `web_terminal_service.cpp:940-985` | 改名 maxLength=24（PRD 32）；标题仅存服务内存 `tabTitles_`，**桌面端不同步**（bridge 无 rename）；按 index 存，关闭后错位 |
| F-14 新建 tab | 已实现但未验 | `web_session_bridge.cpp:59-66`；`web_terminal_service.cpp:919-938` | 启动目录/失败卡需真机（控制器失败态已接） |
| F-15 关闭 tab | 已实现但未验 | `web_session_bridge.cpp:68-76`；`protocol.js:958-975` | 空态「暂无终端」+新建在 |
| F-16 配置六项 | 已实现但未验 | `app.js:667-752`；`web_terminal_service.cpp:600-624`；`web_session_bridge.cpp:189-212` | 六项齐全，单一写入路径；需真机双端并发 |
| F-17 退出/失败/桌面重启 | **与 PRD 不符（P1）** | 退出卡 `index.html:93-97`；`app.js:203-207,506`；事件 `web_terminal_service.cpp:845-873` | 仅「新建终端」，缺「重新开始」「关闭标签」；失败态复用「终端已退出」无失败文案；`markDesktopRestart` 全仓无调用、前端不读 `emptyReason` → 桌面重启提示未接线 |
| F-18 尺寸协同 | 已实现但未验 | `web_session_bridge.cpp:143-187`；`protocol.js:900-909` | 钳制 20–200/8–200、断订阅恢复桌面尺寸在；需真机 |
| F-19 移动布局/键盘 | 已实现但未验 | `app.css:17-37,254`；`app.js:765-790`；`index.html:8` | dvh/visualViewport/safe-area 齐；需真机 |
| F-20 多设备 | 已实现但未验 | 票据表/会话表多实例 | 无真链路，仅单测；依赖 F-04 才能首绑 |
| F-21 登录失败日志 | 已实现但未验 | `web_auth.h:164`；调用 `web_terminal_service.cpp:316` | 单测覆盖限流，日志内容未断言 |
| F-22 断线两段式 | 已实现且可验 | `protocol.js:848-900`；`app.js:220-238` | E2E 杀进程→自动重连 |
| F-23 启动目录双端 | 已实现但未验 | 桌面 `terminal_page.cpp:1408-1440`；网页 `app.js:749-752`；校验 `web_terminal_service.cpp:175-183` | 单测三态；桌面面板布局需真机 |
| F-24 撤销绑定 | 已实现但未验 | `web_terminal_service.cpp:160-172,725-741`；前端 `protocol.js:772` | revokeAll+广播 revoke；需真机 |
| F-25 握手/加密帧 | 已实现且可验 | `web_crypto_session.cpp`；`crypto.js`；`web_terminal_service.cpp:296-420` | 单测 RFC 向量/篡改/重放 + E2E 实际握手；S-33 性能门禁未测 |
| F-26 凭据保护 | 已实现但未验 | 令牌只在 `#b`（`protocol.js:571-582`）；票据只在密文体 `protocol.js:473-514` | 无 Cookie/Authorization；抓包未验；`/api/status` 明文回在线数/绑定时间（无凭据） |
| F-27 全屏 | **与 PRD 不符（轻）** | `app.js:618-647` | 支持/不支持在；iOS「一次性记住」未实现（每次 toast） |
| F-28 Esc/Tab 键带 | 已实现但未验 | `index.html:106-107`；`app.js:658-659` | 0x1b/0x09 在；焦点保持需真机 |

## 3. 用户会立刻撞上的缺失

**P0**
1. 桌面二维码永远「二维码生成中…」：`WebAccessSnapshot.qrMatrix/qrSize` 无生产赋值（`web_terminal_service.cpp:216-231`），绑定令牌无 URL 出口 → 手机无法扫码绑定，局域网鉴权全链路不可用。
2. 会话票据不落地：浏览器无 localStorage，服务端 `TicketStore::saveToFile/loadFromFile` 无调用 → 每次刷新/重开都要重输 TOTP；桌面重启后全部票据失效（PRD「7 天刷新免登录」「重启后票据仍有效」均不成立）。

**P1**
3. 桌面重启提示未接线：`markDesktopRestart` 无调用，`app.js` 不读 `emptyReason`（仅 demo mock 有文案）。
4. tab 重命名桌面端不同步且限 24 字符（PRD 32）。
5. 退出卡缺「重新开始 / 关闭标签」；失败态误显示「终端已退出（退出码）」。
6. 断线补发队列按 256 条（非 4 KB），超限文案与 PRD 不一致。
7. 自诊「监听端口：127.0.0.1」与实际 0.0.0.0 不符；在线数含未登录握手会话。

**P2**
8. 自定义端口不持久化，重启回 7878。
9. 生产静态资源内含 `?demo=1` MockClient 与演示数据（`app.js:11-13,80`；`protocol.js` MockClient）。

## 4. PRD 有、实现没有
- 二维码矩阵生成与绑定 URL（F-04）
- 票据浏览器持久化 + 服务端票据文件持久化（F-09/P-12/P-29）
- 桌面重启全屏提示（P-29/F-17）
- tab 改名桌面同步与 32 字符上限（F-13/P-33）
- 退出卡「重新开始/关闭标签」与失败卡文案（F-17）
- 4 KB 补发队列与指定文案（P-28/F-11）
- iOS 全屏一次性记住（F-27）

## 5. 实现有、PRD 没写
- 生产包内 `?demo=1` MockClient / `?view=` 演示态（截图走查用）
- 明文 `/api/status`（在线数/绑定态/时间，无凭据）
- 设置页「打开日志目录」按钮
- Debug 环境变量 `DEVDESK_WEB_AUTOSTART/PORT/TEST_HOOKS`（release 失效）
- Host/Origin 白名单校验（安全加固，PRD 未列）

## 6. 真机 UAT 清单（手机路径与预期）

1. 桌面开服务 → 设置页应出现可扫二维码（**当前会停在「二维码生成中…」= 阻塞**）。
2. 手机扫 `http://<局域网IP>:7878/#b=…` → 握手成功后显示绑定页；确认页有截图风险文案。
3. 点「添加到验证器」→ 系统验证器识别 `otpauth://totp/DevDesk?...`；返回输入 6 位码 → 进入终端。
4. 刷新页面 → 预期免登录（当前会回登录页 = 缺陷）。
5. 第二台手机打开地址 → 输同一 TOTP 六位码进入；两端同屏、任一端输入均生效。
6. 桌面新建/切换/关闭 tab → 手机 1 秒内跟随；关到最后一个显示「暂无终端」+「新建终端」。
7. 手机双击/长按 tab 改名 → 各端标题一致、桌面 tab 同步（当前桌面不同步）。
8. 手机点「粘贴」→ 弹手动框，三行文本按原文插入、不被逐行执行。
9. 终端跑 `exit` → 覆盖卡显示退出码与「重新开始 / 关闭标签」（当前只有「新建终端」）。
10. 手机断网再恢复 → 顶部细条、恢复后按序补发、滚动位置保持。
11. 桌面关服务 → 约 3 秒全屏「桌面端已关闭网页服务」+「重新连接」；重开服务自动恢复。
12. 桌面重启应用并重开服务 → 预期「桌面端已重启，会话已全部结束」+「新建终端」（当前显示「暂无终端」）。
13. 桌面删除绑定 → 所有手机 5 秒内全屏「绑定已撤销」；旧票据失效。
14. 手机在 claude code 中按「Esc」中断、「Tab」补全，软键盘与焦点保持。
15. 手机竖屏/横屏/弹键盘 → 无遮挡、画布可横滚、顶底栏固定。
16. 配置页改字号/配色/默认 shell/启动目录 → 桌面画面 1 秒内同步；非法目录保存被拦且保留原值。

## 7. 工具调用自报
本轮 27 次（预算 50）。未跑构建/未跑测试/未真机；所有「已实现且可验」指仓内已有可复跑用例或 E2E 步骤，不等于本轮实跑通过。
