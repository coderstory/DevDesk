# doc/scripts 脚本索引

本目录存放项目用到的独立 Python 脚本（多为诊断/验证用途）。新脚本必须在此登记。

| 脚本 | 用途 | 前置条件 | 执行方式 |
|------|------|----------|----------|
| `proxy_smoke_test.py` | 直测 Claude 本地代理：向 `127.0.0.1:8888` 发 `/v1/messages`（非流式 + 流式），并可选直连上游做对照，用于定位失败发生在哪一跳 | DevDesk 已启动且代理在跑（Claude 页手动启动，或设置页开「随应用启动代理」） | `python doc/scripts/proxy_smoke_test.py [proxy\|upstream\|all]` |

## proxy_smoke_test.py 说明

- 代理**无鉴权**（2026-09-27 起仅监听 127.0.0.1），代理这一跳不带凭据；真实上游 key 由代理在转发时注入 profile 的 key。
- 直连上游对照需要 token：默认取 `~/.claude/settings.json` 的 `env.ANTHROPIC_AUTH_TOKEN`，可用环境变量覆盖。
- 环境变量：`DEVDESK_PROXY`（默认 `http://127.0.0.1:8888`）、`DEVDESK_UPSTREAM`、`DEVDESK_MODEL`、`DEVDESK_TOKEN`。
- 退出码：0 = 被测项全部非 5xx 且非网络错；1 = 有失败项。
