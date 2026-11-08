# 池72 / 池73 首页双缺陷修复证据

日期：2026-11-03　仓库 HEAD：e4b5012e（+ 本次工作区改动）　平台：macOS arm64

## 池72 图表区大片空白（有数据不画图）

根因（数据层，不是绘制层）：
- 首页「今日 Token 用量分布」纵轴 = `claude_call_logs` 逐时 `output_tokens` 合计
  （`home_service.cpp:215-266`），速率卡同理（`home_service.cpp:268`）。
- 该表**全仓已无写入方**：`git log -S "INSERT INTO claude_call_logs"` 只命中
  `ef9e9466`（2026-10-26「删除最近调用」）——把它连同 `call_log_store.cpp`、
  `ClaudeService::logCall`、`proxy_router.cpp` 的落库段一起删了。
  `src/` 下现存引用只剩 SELECT / DELETE / 建表。
- 于是真实运行永远 0 行 → `hourlyDataRange` 恒 `hasData=false` → 走空态分支
  （`home_module.cpp:711`），图表区留白。绘制代码本身是好的。

实验（现象 → 实验 → 结论）：
1. 种子库跑 home_probes（`INSERT INTO claude_call_logs` 2 行 400/600 token）→ 图表正常画柱
   → 绘制路径无缺陷，缺陷在「真实数据进不来」。
2. 只读查询本机真实库 `~/DevDesk/devdesk.db`：`SELECT COUNT(*) FROM claude_call_logs` = **0**
   → 与「永远空态」一致。
3. 验证脏写入恢复后测试先红后绿（见下）。

修复（`src/module_claude_proxy/proxy_router.cpp` / `.h`，`claude_service.h`）：
- 最小恢复落库：`insertCallLogRow()`（`INSERT ... RETURNING id`，不用
  `last_insert_rowid()` 以免并发 worker 串号）+ `updateCallLogUsage()`。
- 缓冲路径：三处终态（上游不可达 / 正常转发）各写一行。
- 流式路径：响应头就绪插行 → `forwardStream` 的 `onUsage` 回调流末回填 token 四列 +
  首事件毫秒，并同批回填 `latency_ms`（流式耗时只有流末才有意义）。
- `ClaudeService::db()` 新增只读访问器（ProxyRouter 需同一连接）。
- 未恢复已下线的日志查询/弹窗 UI（那是 ef9e9466 的产品决策）；本修复只恢复**输出端**，
  首页两个消费方（分布图 + 速率卡）即恢复。

防复发断言：
- `tests/test_modules/test_proxy_router.cpp:proxy_router_writes_call_log_row`
  （缓冲路径写终态行：n=1 / status=200 / profile_id / method）
- `tests/test_modules/test_streaming_proxy.cpp:proxy_writes_call_log_usage_for_streaming_path`
  （流式插行 + 流末回填 output_tokens > 0；轮询等 releaser）
- `tests/probes/home_probes.cpp` 组 N1（有数据必真有柱：bar.0/bar.8 高 > 1px，且不走空态分支）

先红后绿证据：把 `insertCallLogRow` 打成 `return 0`（模拟 ef9e9466 之后状态）→
两个单测均 FAIL（n/rows != 1）；恢复后 4/4 绿。

## 池73 切换 Claude Profile 图标绿方块

根因：卡通码点 `0xF46D` 在本仓 FA7 Free-Solid 字体里映射到的是**实心方块轮廓**
（cmap 有映射、charstring 是矩形），不是「缺 cmap」——所以 ink/绿判据都过，看着像
「有图标」，实际是 tofu 块。

实验：
1. `before-1600x1000.png` 图标盒 ASCII 采样 → 实心圆角矩形（见 before 图右下角）。
2. 直接解析 `EUI-NEO/assets/Font Awesome 7 Free-Solid-900.otf` 的 cmap + CFF：
   `U+F46D` → gid 948，charstring 254 字节（实心块）；`U+F007` → gid 431，73 字节。
3. G3 同款 ink/green 判据实测**不区分**：旧码点 ink=96.9%/green=61.3%，
   新码点 ink=99.3%/green=43.9%，两者都「有墨迹且非绿主导」→ 会假绿。
4. 换形状分布判据（满行占比 = 字形包围盒内宽度 > 85% 的行占比）：
   旧码点 65.4%（矩形）、新码点 31.8%（笔画收放）→ 阈值 55% 可区分。

修复：码点换 `0xF007`（fa-user，语义 = 账号/Profile；避开页头刷新按钮的 0xF021）。

防复发断言：`tests/probes/home_probes.cpp` 组 N2（满行占比 ≤ 55% + Icon 字体 + 非空）。
先红后绿证据：把码点改回 `0xF46D` → N2 FAIL（65.4%）；恢复 `0xF007` → PASS（31.8%）。

## 回归

| 项 | 结果 |
|---|---|
| home_probes 1600x1000（组 A-N 全量，含 B7 的 L/M） | exit 0 |
| home_probes 1200x600（最小窗档） | exit 0 |
| home 域单测（`devdesk_tests "home"`） | 26 例 0 失败 |
| proxy 域单测（`devdesk_tests "proxy"`） | 63 例 0 失败 |
| pool72 回归（`devdesk_tests "call_log"`） | 4 例 0 失败 |
| uat62 01-home 四档（1.0/1.15/1.3/1.5）@1000x640 | 四档均正常，图标为 fa-user 字形 |

## 文件

- before-1600x1000.png / before-1200x600.png：修复前（图表有柱=种子态；主缺陷是真实库无写入，见根因）
- after-1600x1000.png / after-1200x600.png：修复后
- revert-check.png：码点回退 0xF46D 时的红态图
- uat62-fs{1.0,1.15,1.3,1.5}/01-home-*.png：四档字号首页
