# doc/scripts 脚本索引

本目录存放项目用到的独立 Python 脚本（多为诊断/验证用途）。新脚本必须在此登记。

| 脚本 | 用途 | 前置条件 | 执行方式 |
|------|------|----------|----------|
| `proxy_smoke_test.py` | 直测 Claude 本地代理：向 `127.0.0.1:8888` 发 `/v1/messages`（非流式 + 流式），并可选直连上游做对照，用于定位失败发生在哪一跳 | DevDesk 已启动且代理在跑（Claude 页手动启动，或设置页开「随应用启动代理」） | `python doc/scripts/proxy_smoke_test.py [proxy\|upstream\|all]` |
| `gen_dmg_background.py` | 生成 macOS DMG 的窗口背景图（`installer/macos/background.png` 700×440 与 `background@2x.png` 1400×880），尺寸与 Mac 侧 `create-dmg --window-size` 严格等大；窗口无 Mac，素材在 Windows 侧预生成入库 | Pillow（本机只有 `C:/Program Files/Python311/python.exe` 带 PIL，MSYS2 的 python 没有；缺 Pillow 时脚本会给出中文提示） | `"C:/Program Files/Python311/python.exe" doc/scripts/gen_dmg_background.py` |
| `gen_app_icns.sh` | 在 macOS 上用 `iconutil` 把源 PNG（`installer/macos/icon-1024.png`，1024×1024）转成 App 图标 `DevDesk.icns` 与 DMG 卷图标 `.VolumeIcon.icns`（installer 设计 spec §4.4 第 1 项），内部先补画布留白、再用 `sips` 按 Apple iconset 标准缩放 | **只能在 macOS 上运行**（`iconutil`/`sips` 是 macOS 专有工具，Windows/Linux 上运行会清晰报错）；源图 `installer/macos/icon-1024.png` 需先在 Mac 上准备好，缺失时脚本报错退出；补留白需 python3 + Pillow | `bash doc/scripts/gen_app_icns.sh` |
| `pad_icon_canvas.py` | 把图标源图等比缩到最长边 82% 并居中放进 1024×1024 **透明**画布，供 `gen_app_icns.sh` 与 `platform/macos/build-app.sh` 共用（同一份几何，两处不重复实现）。修「Dock 里图标比其他应用大」——Dock 用同一格子绘制所有图标，苹果自家图标内容只占约 80%，满幅内容就显得大一圈 | Pillow（缺了给中文提示、退出码 2，不静默产出满幅图标） | `python3 doc/scripts/pad_icon_canvas.py <源图> <输出图> [--ratio 0.82]` |

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

## gen_app_icns.sh 说明

- **只能在 macOS 上运行**：`iconutil` / `sips` 是 macOS 专有工具（系统自带），在 Windows / Linux 上运行会打印清晰报错并以非零码退出；脚本本身在 Windows 侧入库，仅作为"上 Mac 后一键生成"的载体（installer 设计 spec §4.4 第 1 项——仓库零 `.icns`，图标只能在 Mac 上 `iconutil` 生成）。
- 前置：`installer/macos/icon-1024.png`（1024×1024 源图）需先在 Mac 上准备好；源图缺失时脚本报错退出，不生成任何产物（不假装成功）。
- 内部按 Apple iconset 标准生成 10 张 PNG（16 / 32 / 128 / 256 / 512 及各自 @2x = 32 / 64 / 256 / 512 / 1024）；**10 档全部派生自一张补过留白的 1024 母图**（先用 `pad_icon_canvas.py` 把源图缩到最长边 82% 居中放进透明画布，再 `sips -z` 向下缩放），源图任一边小于 1024 时打印 `[WARN]`。
- **画布留白**（2026-10-02 加）：Dock 用同一个小格子绘制所有应用图标，苹果自家图标的内容只占格子约 80%、四周留白，源图满幅（alpha bbox = 整张画布）时在同一个格子里就显得比别的应用大。修法是补留白，**不动任何尺寸参数**；脚本在打包前会断言 1024 档四边确有留白，没有就报错退出（不留白 = 改动没生效）。
- **不另加圆角**：现用源图本身已是圆角方形（实测超椭圆 n≈3.4、左上角半径约 130/360 = 画布宽的 36%，四角本就透明），再叠一层圆角只会切掉已有圆角边或完全不起作用；理由写在 `pad_icon_canvas.py` 头部。
- 产物 `installer/macos/DevDesk.icns` 与 `installer/macos/.VolumeIcon.icns`（两份内容来自同一 iconset，卷图标在 DMG 根目录命名为 `.VolumeIcon.icns` 即被 Finder 采用）生成后需显式入库。
- 源图 `installer/macos/icon-1024.png` 被 `installer/macos/.gitignore` 忽略（大二进制源素材不追踪；`.icns` 是交付物不忽略）。
- Mac 侧尚未验证的部分（本机无 Mac）：`iconutil` / `sips` 实跑、`.VolumeIcon.icns` 在两个产出路径下被 Finder 真实采用。
