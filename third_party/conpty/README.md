# third_party/conpty — ConPTY v2 redist 放包目录（debt246）

本目录存放**随包分发**的 ConPTY v2 redist 二进制（`conpty.dll` + `OpenConsole.exe`，
**必须成对**——只带 dll 不带 OpenConsole.exe 会静默回落系统 inbox conhost，
wtmux 实证坑，加载层按「成对存在」判定可用性）。构建期由根 `CMakeLists.txt`
POST_BUILD 拷到 exe 旁 `conpty\`（仅 Windows）；打包链由
`scripts/package-windows.sh` / `scripts/install-win-debug.sh` 接力；
发布形态守卫（`scripts/build-test-run.sh`）把 `conpty/` 子目录列入白名单
（白名单外 DLL 仍禁止）。

运行时三级查找（`src/platform/terminal/terminal_backend_win.cpp` 的 conptyApi()）：

1. `DEVDESK_CONPTY_DIR` 环境变量（指向含成对文件的目录）
2. exe 旁 `conpty\` 目录（本目录经构建/打包链的落点）
3. 回落系统 kernel32 的 inbox `CreatePseudoConsole`（Win10 1809+，现状行为）

来源写入启动日志一条（`ConPTY: v2 redist x.y.z @ <目录>` /
`ConPTY: inbox ...`），TermPerf 对照与验收据此区分。任一级不可用 = LOG_WARN +
回落 inbox，不崩。

## 获取方式（二选一，均已核证，clue105）

- **主渠道：node-pty 的 pinned 副本**
  <https://github.com/microsoft/node-pty> 树内 `third_party/conpty/<版本>/`
  直接取 `conpty.dll` + `OpenConsole.exe` 两文件（VS Code 同源，pin 见下表）。
- **备渠道：NuGet 包 `Microsoft.Windows.Console.ConPTY`**
  `https://www.nuget.org/api/v2/package/Microsoft.Windows.Console.ConPTY/<版本>`
  下载 `.nupkg`（zip 形态），解包取 `conpty.dll` + `OpenConsole.exe`
  （包内目录布局以实际 nupkg 为准；两文件在包内必成对出现）。

## 版本 pin 策略

钉 node-pty `third_party/conpty/` 当前 pin（研究核证时点为
**1.25.260303002**；WezTerm 参考 pin 1.24.260402001）。升级时：换文件 +
更新下表 sha256 + 本行版本号，跑 `bash scripts/build-test-run.sh --dev
--filter ConPTY`（构建/启动未验前由主会话统一执行）。

## 放包与校验

1. 两个文件放进本目录：`conpty.dll`、`OpenConsole.exe`（名字逐字，大小写敏感面
   以 Windows 实际文件名为准）。
2. 计算并**回填**下表：

```sh
sha256sum conpty.dll OpenConsole.exe
```

| 文件 | 版本 pin | SHA256 |
|---|---|---|
| conpty.dll | 1.25.260303002 | `3319b484b80bb53d1f4d0a9eb0ea60fd0f61da69db7280ca43b84215f19245ff` |
| OpenConsole.exe | 1.25.260303002 | `7f68c840226505004215c0b82d4e502c24b5bc3f4b93c4baaaa19bd679c0def8` |

> 回填记录（2026-11-05，主会话）：两文件于 2026-11-05 从 node-pty `main` 的
> `third_party/conpty/1.25.260303002/win10-x64/` 经 GitHub Blob API 原样取回；
> 下载后以 `git hash-object` 校验内容与上游 blob SHA 完全一致
> （conpty.dll=`3669e4744781ae76166ad68ef0e5b8f129d42e15`、
> OpenConsole.exe=`c514e7ce3f661522d87e1796e66d3466ac8be938`）。

> 资产缺席 = 构建不拷贝、打包出 WARN、运行时回落 inbox——全链不失败；
> 半目录（只放了一个文件）会在加载层被拦并 LOG_WARN，不会静默半绑。

## 许可

Microsoft Windows Terminal / ConPTY 组件，MIT License（microsoft/terminal 仓
LICENSE；随包分发条目已登记 `THIRD-PARTY-NOTICES.md`）。
