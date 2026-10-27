# sanitize（外发脱敏层）

云端抽取外发前的最后清洗层，实现 feasibility 报告 §5 风险 8 的前置条件：
「外发前必须脱敏，红队样本泄漏 0」。这是任何云端外发路径的准入门槛。

## API

```python
from sanitize import sanitize

result = sanitize("任意待外发文本")
result.text    # 脱敏后文本，敏感区间替换为 [REDACTED:<category>]
result.hits    # 命中清单：category / start / end / detail / preview（掩码，不含原文）
```

一个函数跑通。`scan(text)` 只返回命中清单不改文本。幂等：对输出再跑一遍不变。

## 覆盖类别（首批）

| category | 形态 | 变体 |
|---|---|---|
| `phone` | 中国手机号 | +86 / 86- 前缀、连字符/空格分组、全角数字 |
| `email` | 邮箱 | 加号 tag、全角 @、多级域 |
| `id_card` | 18/15 位身份证 | 空格分段、全角；校验位 awareness：非法校验位仍命中，`detail` 标 `checksum_bad` |
| `ipv4` | IPv4 | URL 内嵌、带端口 |
| `url_param` | URL 敏感查询参数 | token / access_token / sign / key / password 等，忽略大小写 |
| `secret` | 密钥形态 | Bearer/Basic、AWS AK（AKIA/ASIA/AIDA）、Google AIza、GitHub（ghp_/github_pat_）、OpenAI sk-、Slack xox-、JWT、PEM 私钥块、`api_key=`/`password=`/`secret=` 等键值 |

设计取舍（契约）：

- **泄漏 0 优先于误杀（fail-closed）**：拿不准的形态一律脱敏。例如
  `password: required` 里的 `required` 也会被切掉——这是特性不是缺陷。
- 全角字符经等长 shadow 归一化后检测，命中区间直接映射回原文，其余字节不动。
- allowlist 只豁免公认占位/回环值：`13800000000`、`127.0.0.1`、`0.0.0.0`、
  `255.255.255.255`。新增豁免项必须同步补进 `datasets/redteam.json` 并写明理由。
- 互不重叠原则：同一区间只报最高优先级命中（私钥块 > 键值 > Bearer > 厂商密钥 > JWT
  > 邮箱 > URL 参数 > 身份证 > 手机号 > IPv4）。

## 防什么 / 不防什么

**防**：上述六类形态的确定性泄漏（正则可表达、含全角/分隔符/拼写变体）。

**不防**（已知边界，红队样本 n18 亦声明）：

- **中文姓名**：中文语境下重名/常用词误杀率高，留待后续（白名单+人工抽检方案）。
- **绝对路径 / 私密片段**：feasibility §8.1 提到，但路径在代码会话里出现频率极高，
  需要白名单机制配合，不在本批；接入云端抽取前必须补。
- **语义级敏感**（"我住在 XX 路 18 号"这类无定型形态）：正则不可达，需模型侧策略。
- **IPv6**、银行卡号、护照号：首批不做。
- 形态被刻意破坏的对抗样本（`1 3 8-1 2 3 4`逐字插入）：会漏，属对抗攻防范畴。

## 红队评测

```bash
python sanitize/eval_redteam.py
```

`datasets/redteam.json`：110 条（92 正例含变体 + 18 形似负例）。判定（任何一条不满足
即 raise AssertionError，进程非零退出）：

1. 正例泄漏 = 0（敏感子串残留，或期望类别全部未命中，都算泄漏）；
2. 负例误杀率 ≤ 0.05，实际数字随输出打印；
3. 样本 ≥ 100、脱敏幂等。

风险 8 的证伪门槛由此脚本承载：「100 条红队样本经脱敏层后泄漏 >0 条 → 禁止启用云端抽取」。
