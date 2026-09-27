# doc/scripts 脚本索引

本目录存放项目用到的独立 Python 脚本（多为诊断/验证用途）。新脚本必须在此登记。

| 脚本 | 用途 | 前置条件 | 执行方式 |
|------|------|----------|----------|
| `proxy_smoke_test.py` | 直测 Claude 本地代理：向 `127.0.0.1:8888` 发 `/v1/messages`（非流式 + 流式），并可选直连上游做对照，用于定位失败发生在哪一跳 | DevDesk 已启动且代理在跑（Claude 页手动启动，或设置页开「随应用启动代理」） | `python doc/scripts/proxy_smoke_test.py [proxy\|upstream\|all]` |
| `gen_dmg_background.py` | 生成 macOS DMG 的窗口背景图（`installer/macos/background.png` 700×440 与 `background@2x.png` 1400×880），尺寸与 Mac 侧 `create-dmg --window-size` 严格等大；窗口无 Mac，素材在 Windows 侧预生成入库 | Pillow（本机只有 `C:/Program Files/Python311/python.exe` 带 PIL，MSYS2 的 python 没有；缺 Pillow 时脚本会给出中文提示） | `"C:/Program Files/Python311/python.exe" doc/scripts/gen_dmg_background.py` |

## proxy_smoke_test.py 说明

- 代理**无鉴权**（2026-09-27 起仅监听 127.0.0.1），代理这一跳不带凭据；真实上游 key 由代理在转发时注入 profile 的 key。
- 直连上游对照需要 token：默认取 `~/.claude/settings.json` 的 `env.ANTHROPIC_AUTH_TOKEN`，可用环境变量覆盖。
- 环境变量：`DEVDESK_PROXY`（默认 `http://127.0.0.1:8888`）、`DEVDESK_UPSTREAM`、`DEVDESK_MODEL`、`DEVDESK_TOKEN`。
- 退出码：0 = 被测项全部非 5xx 且非网络错；1 = 有失败项。

## gen_dmg_background.py 说明

- 画面按 `docs/superpowers/specs/2026-09-28-installer-and-dmg-design.md` §4.2 固定：暖白 `#FAF9F5` 竖向渐变到 `#F0EEE6`；中间一支 clay `#D97757`、约 60% 不透明度的细线箭头（位置落在 create-dmg 两个图标之间，y 与 `--icon` / `--app-drop-link` 对齐）；箭头下方一行 13px `#6B665E` 中文说明「拖入「应用程序」文件夹完成安装」；顶部留白给 Finder 标题栏；底部极浅色带收边。
- 所有坐标按 700×440 的「逻辑画布」定义，1x / 2x 共用同一份绘制逻辑按比例缩放（改版面只动脚本顶部常量，不需要改两处坐标）。
- 中文字体用 `C:/Windows/Fonts/msyh.ttc`；字体缺失或加载失败时自动退化为**无文字版本**并打印 `[WARN]`，脚本仍可复跑。
- 可重复执行（覆盖已有文件）；结束前用 Pillow 读回两张 PNG 断言尺寸 700×440 / 1400×880，不符即抛错（退出码 1）。
- 产物是二进制素材：改版面重新生成后需显式入库 `git add installer/macos/background.png "installer/macos/background@2x.png"`。
- Mac 侧尚未验证的部分（本机无 Mac）：`background@2x.png` 是否真被 Finder 采用（create-dmg 无对应参数）、窗口内图标与箭头的实际对齐观感。
