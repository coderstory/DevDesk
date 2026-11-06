# 阶段 0 POC 报告：字体子集化管线（need:132）

- 日期：2026-11-06
- 池条目：`[pool:need:132]`（跨平台显示一致 · 字体统一线）
- 执行：ZCode 主会话直做（POC/试错类按 ask §1 归主会话）
- 线索：clue 54
- 环境：macOS arm64 / Python 3.15.5 / fontTools 4.68.0 / pyftsubset（隔离 venv `/tmp/font-poc/venv`，未污染项目）

## 结论先行

1. **子集化管线可行**：Noto Sans CJK SC Regular 16.44MB → **3.22MB**（20.5%），优于技术卡 ≤4MB 目标。
2. **子集化不改度量**（关键前提实证）：全量 **7150 码点** hmtx advance **逐一零差异**，unitsPerEm 1000 一致。→ PRD「统一字体后两端测宽一致」的推理链在此环节成立。
3. **重要发现：覆盖率缺口来自源字体选型，不是子集化**。18 个源码在用码点（U+2705 ✅、U+2714 ✔、U+25B8 ➤ 等 emoji/符号）**Noto Sans CJK 本身就不含**，子集前后都缺。
4. **影响 PRD 定义**：§6.2 缺字回退链不是「极端边界兜底」，而是**日常必现路径**——界面里真在用的符号就必须走 emoji/符号字体。技术卡须把回退链从可选兜底升级为一等公民，并补 emoji 字体选型。

## 四断言明细

| 断言 | 方法 | 结果 |
|---|---|---|
| ① 体积 | `du` on 子集产物 | 3,371,564B = **3.22MB** ≤ 4MB **PASS** |
| ② 度量不变 | 全量 7150 码点 hmtx advance 与源字体逐一对 | **零差异 PASS** |
| ③ unitsPerEm | head 表 | 1000 = 1000 **PASS** |
| ④ 覆盖率 | unicodes.txt 每码点在子集 cmap | 缺失 18 / 7150 **FAIL** → 归因见下 |

## 覆盖率缺口归因（关键）

缺失 18 个码点，逐个回查源字体 cmap：**源字体本身即无**。样例：

| 码点 | 字符 | 源字体 | 用途（源码字面量） |
|---|---|---|---|
| U+2705 | ✅ | 无 | 状态标记 |
| U+2714 | ✔ | 无 | 勾选 |
| U+25B8 | ▸ | 无 | 折叠指示 |
| U+2728 | ✨ | 无 | 装饰 |
| U+2139 | ℹ | 无 | 信息标记 |

（其余为 U+22EE ⋮ / U+25BE ▾ / U+270E ✎ / U+2715 ✕ / U+2717 ✗ 同类）

→ **结论**：这些是 emoji/dingbat 区码位，需要专门的符号字体（如 Noto Emoji / Symbola / 系统 emoji 字体）承接。F-01 若只打包中文字体，界面符号会缺字。

## 字符集构成

| 来源 | 码点数 |
|---|---|
| GB2312 全集（双字节枚举，5 个未指派已跳过） | 6763 |
| 源码字面量非 ASCII（扫 `src/**/*.{cpp,h,mm}`） | 1638 |
| ASCII 可打印 + 全角标点/符号段（U+3000-303F、U+FF00-FF5F） | 补足 |
| **合并去重** | **7150** |

## 复现方式

```sh
mkdir -p /tmp/font-poc && cd /tmp/font-poc
python3 -m venv venv && ./venv/bin/pip install fonttools brotli
curl -sL -o notosans.zip https://github.com/notofonts/noto-cjk/releases/download/Sans2.004/08_NotoSansCJKsc.zip
./venv/bin/python -I poc_subset.py   # 子集产出
./venv/bin/python -I poc_verify.py   # 四断言
```

## 交给技术卡 v2 的待办

1. **补 emoji/符号字体选型**（F-01 扩展）：候选 Noto Emoji（OFL，monochrome 版体积极小）/ Symbola（许可需核）/ 系统 emoji 字体兜底。需评估：是否随包、是否影响两端一致性（系统 emoji 字体两端不同→又一处分叉源）。
2. **回退链定性升级**：PRD §6.2 的「缺字走系统兜底」须改为「符号/emoji 专用字体为主，系统兜底为次」，并明确两端同字体，否则 F-01 验收 1（测宽相等）在这些符号上仍会分叉。
3. **子集管线确定可入库**：`scripts/make_font_subset.py` 可按本 POC 实现，断言②③已实证，断言④改为「子集覆盖率 = 源字体覆盖率」（保证子集没额外丢字），另立 emoji 覆盖断言。

## 未覆盖 / 遗留

- 未验证 JetBrains Mono（F-02）——本 POC 只跑中文字体。
- 未验证 pyftsubset 输出字节级确定性（技术卡 §3 断言④）——本轮未重跑比对。
- 未验证真实渲染观感（hinting/抗锯齿）——需设备上屏截图，属实现期。
- 未验证 Windows 侧同字体加载——本机 macOS，须 CI 或真机。
