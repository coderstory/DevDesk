# B3 智能体列表重设计 证据（池 66，2026-11-02/03）

仓库：/Users/coderstory/dev/DevDesk（macOS arm64，dev 分支，基线 HEAD=162629cc）
构建：bash scripts/build-test-run.sh --dev（增量，唯一入口）
证据目录：evidence/2026-11-02-B3/

## 断言结果（agent_e2e_probe，G 组）

### 默认窗（1200×1000，网格 3 列）
```
G1 card 1 366.5x120 aspect=3.05417     G1 PASS 每卡 2.5–4.5:1（3.5 目标）
G1 card 2 366.5x120 aspect=3.05417
G1 card 3 366.5x120 aspect=3.05417
G1 card 4 366.5x120 aspect=3.05417
G2 card 1 y=110 titleBaseline=144 actionsRight=394.5 wantRight=394.5
G2 card 2 y=110 titleBaseline=144 actionsRight=769 wantRight=769
G2 card 3 y=110 titleBaseline=144 actionsRight=1143.5 wantRight=1143.5
G2 card 4 y=238 titleBaseline=272 actionsRight=394.5 wantRight=394.5
G2 PASS 行内标题基线同 y（2 行）/ 操作组右缘=卡右−14
G3 glyph U+f05a .detail  ink=30.4209% green=0%
G3 glyph U+f044 .edit    ink=24.4898% green=0%
G3 glyph U+f011 .toggle  ink=21.875%  green=0%
G3 glyph U+f03a .enqueue ink=21.588%  green=0%
G3 glyph U+f2ed .delete  ink=27.3278% green=0%
G3 PASS 5 码点字形存在且非绿方块
G4 normal    actions.opacity=0 bg.radius=12 bg.scale=1
G4 hover     actions.opacity=1 bg.radius=16 bg.scale=1.02
G4 out       actions.opacity=0 bg.radius=12 bg.scale=1
G4 on-button actions.opacity=1 bg.radius=16      ← 指针从卡身移到按钮，卡态不丢
G4 PASS hover 驱动操作组显隐 + 卡形变/复位
G4 pixel actionsInk=6347 cornerInk=177 actionsBack=0 cornerBack=0
G4 pixel PASS 操作组淡入/卡角形变/移出逐像素复位
```

### 窄窗（700×900，单列退化）
```
G1 card 1..4 615.5x120 aspect=5.129
G1 PASS（单列退化态：窄窗内容宽不足以排 2 列，按方案退 1 列，宽高比区间不适用）
G2 PASS 行内标题基线同 y（4 行）/ 操作组右缘=卡右−14
```

## 截图
- agent-list.png           智能体列表页（3 列 × 120 高三行卡 + 常驻「启动」）
- agent-list-hover.png     同机位 hover 态（首卡操作组五个图标钮浮现 + 卡角变大）
- uat62/06-agent-1000.png  uat62 回归：06-agent 页（1600×1000，空态）
- uat62/                   1600×1000 全 12 页回归图（PASS，跳过 wallpaper / release_group = 平台不支持）
- probe.log                agent_e2e_probe 完整输出

## 环境性失败（与本次改动无关，只报不修）
- F1–F5 五流程全部失败于同一上游错误：
  `上游 HTTP 400 {"error":{"code":"InvalidSubscription","message":"Your account
  (2123272471) does not have a valid CodingPlan subscription..."}}`
  → LLM 腿订阅过期，run 全部 failed。G1–G4 不依赖 LLM，已全绿。
  探针退出码非 0 仅源于此（前置缺失 = 失败，不静默跳过）。
- 首次运行 F1 报「实测 2 条 run」：跨本地 00:00 触发 daily 当天重发（scheduler.cpp
  ranOnLocalDay 按本地日期比较），属既有跨零点行为，非本次改动引入。

## 测试
- devdesk_tests --filter agent：144/144 PASS
- devdesk_tests --filter file:test_agent_ui：79/79 PASS
- devdesk_tests --filter file:test_agent_detail_layout：3/3 PASS
- devdesk_tests --filter mcp：171/171 PASS（grid.h 共享化后 MCP 页行为不变）
- check-module-isolation.sh：exit 0
- 未跑全量 doctest / 探针全家桶 / 守卫链（研发档纪律，收官由 qa）
