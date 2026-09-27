# Windows 用户级安装程序 + macOS DMG（设计）

日期：2026-09-28 ｜ 状态：**设计（未实施）** ｜ 关联：`2026-09-22-backlog.md`（EUI-NEO push 决策门控）、`docs/KNOWN_LIMITATIONS.md`

## 1. 需求与决策

| # | 需求（用户原话） |
|---|------------------|
| **R1** | 「调研和开发打包 windows 下可执行的安装程序功能…安装到用户目录内，不需要管理员权限」 |
| **R2** | 「把编译单 exe 的需求取消了，不需要把 dll 都打包到主程序里」 |
| **R3** | 「安装程序尽可能的 UI 现代化，好看一点 不要用默认的丑陋样式」 |
| **R4** | 「macos 也需要生成 dmg 的能力」＋「我没签名 提供一个 xattr 脚本」＋「dmg 好像也可以配置窗口的样式的，搞得现代化大气一点」 |

| # | 决策（本会话自主拍板，待复核） | 理由（证据见 §2） |
|---|--------------------------------|-------------------|
| **D-1** | Windows 安装器用 **Inno Setup 6.7.3**（不是 NSIS） | NSIS 要"现代"只能用许可不明/停更的皮肤插件或自写整窗自绘且仍无深色模式；Inno 6.6+ 的 `WizardStyle=modern dynamic` 自带**跟随系统深色** + 可换配色 + VCL 皮肤，是唯一"免管理员 + 现代 + 深色 + 许可可商用"的路线 |
| **D-2** | **每次安装都是用户级**：`PrivilegesRequired=lowest`（Inno 官方口径：lowest 时**没有任何进程提权**）；安装目录 `{autopf}\DevDesk`（低权限下解析为 `%LOCALAPPDATA%\Programs\DevDesk`）；开始菜单/桌面快捷方式与卸载项全写 HKCU（Inno 自动生成） | R1；与 Tauri/VS Code 同款形态 |
| **D-3** | **不开** `PrivilegesRequiredOverridesAllowed=dialog`（不给"为所有人安装"选项） | 少一个提权入口；真要全员装可另出参数化版本 |
| **D-4** | 外观：`WizardStyle=modern dynamic hidebevels` + `WizardBackColor=#FAF9F5` + `WizardBackColorDynamicDark=#1A1816` + `SetupIconFile`/`UninstallDisplayIcon` 用 DevDesk 图标 | R3 + 与 DevDesk 方案 B 同族（token 见 §2）；`hidebevels` 去掉全部横向分割线 |
| **D-5** | 卸载**不删** `%APPDATA%\DevDesk`（用户数据：DB/笔记/日志/皮肤）；卸载项显示名 `DevDesk`、`NoModify`/`NoRepair` | 数据安全默认；卸载只清程序目录、快捷方式、HKCU 卸载键 |
| **D-6** | **不做**开机自启 / 不做文件关联 / 不自建"应用和功能"以外的注册表项（本期 YAGNI，登记为后续可选项） | 减少安装器职责；文件关联涉及"是否抢默认程序"的产品判断 |
| **D-7** | 版本号**单一源** = `CMakeLists.txt` 的 `project(DevDesk VERSION 0.1.0)`：打包脚本以 configure 期变量/`CMakeCache` 取值注入 `.iss`（`MyAppVersion`），并同步 macOS `Info.plist`（后续把 plist 改为由 CMake 生成） | 现状 `Info.plist` 手写 `0.1.0` 与 CMake 各自独立，无 bump 机制；CPack 的版本是 FreeType 的（陷阱，见 §2） |
| **D-8** | 脚本落点：**两个独立脚本**，不做平台分派 —— `scripts/package-windows.sh`（构建 Release 树 → 暂存 → 调 `ISCC`）与 `scripts/package-macos.sh`（构建 → 打包 `.app` → `create-dmg`）。**不改** `scripts/build-test-run.sh`（它只管构建/测试/启动） | 用户明确"build-test-run.sh 是跑测试的脚本，不能放这里"；分派脚本在无 Mac 的机器上永远是死代码 |
| **D-9** | Windows Release 构建走**独立目录 `build-release/`**（现有脚本固定 Debug；Release 首次构建吃不到 Debug 的 ccache，约 10 分钟冷构建） | 不污染 Debug 树；ccache 哈希含编译选项，`-O2` 与 `-g` 不通 |
| **D-10** | macOS DMG 用 **`create-dmg`**（MIT，封 `hdiutil`），窗口规格写在脚本参数里（唯一真相源）；**背景图在 Windows 侧用 Pillow 预生成入库** | 本机无 Mac，素材必须能在 Windows 生成；实测本机 Python 3.11 + Pillow 12.2.0 可用，`msyh.ttc` 渲染中文无豆腐块 |
| **D-11** | 本期**不做签名/公证**；DMG 内附**去隔离 `.command`**（不碰 `spctl`/Gatekeeper 全局降级） | 用户明确"我没签名" |
| **D-12** | 单 exe 约束按 R2 **降级为"现状/可选优化"**：**本期不改链接方式**（保持静态）。理由：安装包只管装哪些文件，与链接方式无关；改成动态要动 EUI-NEO 的 `-static` 并随包带约 18 个 MSYS2 DLL，收益为负 | R2 的字面要求是"不必把 DLL 塞进主程序"，现状已满足；验收口径的作废位置见 §2 |

## 2. 现状事实（实测 + 源码核实）

| 事实 | 证据 |
|------|------|
| 仓库**无 `install()`、无 `include(CPack)`**；`build/CPackConfig.cmake` 是 EUI-NEO 自带 FreeType 2.13.3 的副产物（`CPACK_PACKAGE_VERSION` 被写成 `2.13.3`）→ 直接 `cpack` 会产出误名空包 | 调研实测（`git grep -nE 'install\(|CPack'` 零命中；`../EUI-NEO/3rd/freetype/CMakeLists.txt:683 include(CPack)`） |
| 产物依赖：`objdump -p build/DevDesk.exe` = **17 项全系统 DLL、零 MSYS2 DLL**（静态已达标） | 调研实测 |
| 必须随包分发的资源：`build/assets/`（约 **24 MB**：字体 NotoSansSC 17M + 音乐 + shaders + pets；EUI-NEO 侧 `text.cpp`/`image_source.cpp` 按 **exe 旁 `assets/`** 解析）；`fonts/` 是空目录不用打 | 调研实测（`build/assets` 体积、EUI-NEO 读取路径） |
| ⚠️ `build-release/assets` 是 **9-22 的旧资源（5.7 MB）** → 打包**必须取当次构建目录**的 assets，不得引旧目录 | 调研实测 |
| 本机 **NSIS 3.12 已装**（pacman，`/c/msys64/mingw64/bin/makensis.exe`）；**Inno Setup 未装**，且 `winget install --scope user` 因 `github.com` 被公司网拦而失败（`InternetOpenUrl() failed 0x80072efd`）；`files.jrsoftware.org` 只有 `.issig` 签名文件、`jrsoftware.org` 页面只指向 GitHub → **本机今晚无法获取 ISCC** | 本轮实测 |
| 本机 `C:/Program Files/Python311/python.exe` 带 **Pillow 12.2.0**；`magick`/`rsvg-convert`/`inkscape`/`ffmpeg` 全无；MSYS2 python 无 PIL | 调研 + 实测 |
| 仓库**不保留可执行位**（`git ls-files -s scripts/` 全 `100644`）→ DMG 构建必须 `chmod 755` + `[ -x ]` 断言 | 调研实测 |
| 仓库**零 `.icns`**（无 App 图标、无卷图标素材）→ 图标只能在 Mac 上 `iconutil` 生成（列入"必须在 Mac 上做"） | 调研实测 |
| `.app` 链路仍在：`CMakeLists.txt` 的 `if(APPLE)` → `devdesk_app` target → `platform/macos/build-app.sh`（产出 `build/DevDesk.app`，含 `lsregister`）；`platform/macos/Info.plist` 手写版本 `0.1.0` | 调研实测 |
| DevDesk 主题 token（方案 B）：暖白底 `#FAF9F5`、暖墨底 `#1A1816`、主色 Claude Clay `#D97757`、浅色主色文字 `#A64F2E` | `src/core/theme.cpp` |
| `platform/macos/build-app.sh` **不拷 `assets/`**，而 `pet_skin.cpp` 的开发期皮肤根按 cwd 解析 → Finder 启动 .app 时美术皮肤不可见 | 分片 A/C 审计（本 spec 需一并修） |

## 3. Windows：Inno Setup 用户级安装器

### 3.1 `.iss` 关键指令（`installer/windows/devdesk.iss`）

```
[Setup]
AppId={{8B2C…}}                       ; 固定 GUID（升级/卸载识别）
AppName=DevDesk
AppVersion={#MyAppVersion}            ; 由打包脚本 -D 注入（D-7）
AppPublisher=DevDesk
DefaultDirName={autopf}\DevDesk       ; 低权限 → %LOCALAPPDATA%\Programs\DevDesk
DisableProgramGroupPage=yes
PrivilegesRequired=lowest             ; 全程不提权（D-2）
OutputDir=out
OutputBaseFilename=DevDesk-{#MyAppVersion}-setup
SetupIconFile=..\..\assets\icon.ico   ; 如实体路径以实现为准
UninstallDisplayIcon={app}\DevDesk.exe
WizardStyle=modern dynamic hidebevels  ; D-4
WizardBackColor=#FAF9F5
WizardBackColorDynamicDark=#1A1816
Compression=lzma2
SolidCompression=yes

[Files]
Source: "<ReleaseTree>\DevDesk.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "<ReleaseTree>\assets\*";   DestDir: "{app}\assets"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{userprograms}\DevDesk"; Filename: "{app}\DevDesk.exe"
Name: "{userdesktop}\DevDesk";  Filename: "{app}\DevDesk.exe"

[Run]
Filename: "{app}\DevDesk.exe"; Description: "启动 DevDesk"; Flags: nowait postinstall skipifsilent

[UninstallDelete]
Type: filesandordirs; Name: "{app}"
```
（`{userprograms}`/`{userdesktop}` 是 per-user 常量；**不写** `{commonprograms}`/HKLM 相关项 —— 任何 HKLM 写入都会触发 UAC。）

### 3.2 打包流程（`scripts/package-windows.sh`）

1. 工具链 PATH（照 `build-test-run.sh` 的三段式）+ 定位 EUI-NEO（**复用其逻辑**：`EUI_NEO_DIR` → 同级 `../EUI-NEO` → git 主工作树同级）。
2. `cmake -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release -DEUI_NEO_DIR=… -DOPENSSL_ROOT_DIR=C:/msys64/mingw64 -DZLIB_LIBRARY=… -DZLIB_INCLUDE_DIR=…`（首次）+ `cmake --build build-release`。
3. 版本号取值：从 `build-release/CMakeCache.txt` 或 `cmake -LA` 读 `CMAKE_PROJECT_VERSION`（= `project(... VERSION)`），传给 ISCC：`ISCC /DMyAppVersion=<v> installer/windows/devdesk.iss`。
4. ISCC 定位：三段式探测（`ISCC` 环境变量 → `%LOCALAPPDATA%\Programs\Inno Setup 6\ISCC.exe` → `C:\Program Files (x86)\Inno Setup 6\ISCC.exe`）→ 全无则**清晰报错**并打印安装命令（见 §6）。
5. 产物：`installer/windows/out/DevDesk-<v>-setup.exe`；打印体积。
6. `.gitignore` 增补：`installer/**/out/`。

### 3.3 验收（工具到位后可跑，**本轮阻塞**）

静默安装到临时目录 → 断言：`DevDesk.exe` 与 `assets/` 就位、`{userprograms}\DevDesk.lnk` 存在、HKCU 卸载项存在、**无 UAC 提示**（`RequestExecutionLevel=user` 等价物：`PrivilegesRequired=lowest`）→ 启动 → `objdump` 复核 `assets` 可读 → 卸载 → 断言程序目录与快捷方式消失、**`%APPDATA%\DevDesk` 保留**。

## 4. macOS：DMG

### 4.1 窗口规格（`scripts/package-macos.sh` 内，create-dmg 参数即规格）

```
--volname "DevDesk"
--window-pos 200 120
--window-size 700 440        # 背景图必须严格等于此尺寸
--icon-size 120
--icon "DevDesk.app" 190 210
--app-drop-link 510 210
--hide-extension "DevDesk.app"
--background "installer/macos/background.png"
--no-internet-enable
```

### 4.2 背景素材（Windows 侧预生成，入库）

- 生成器：`doc/scripts/gen_dmg_background.py`（仓库约定：`doc/scripts/` + `index.md` 登记）；用 Pillow 渲染 **700×440** 与 **1400×880**（`background.png` / `background@2x.png`）。
- 画面：暖白 `#FAF9F5` 竖向渐到 `#F0EEE6`、一支克制的 clay 箭头（`#D97757`）、箭头下一行 13px `#6B665E` 中文说明（"拖入「应用程序」文件夹完成安装"）、顶部留白给标题栏。
- 脚本必须可在本机复跑（`C:/Program Files/Python311/python.exe`），并在无 Pillow 时报清晰错误。

### 4.3 交付物（`installer/macos/`）

| 文件 | 说明 |
|------|------|
| `build-dmg.sh`（或由 `scripts/package-macos.sh` 统管） | 构建 → `platform/macos/build-app.sh` → 补拷 `assets/` → `chmod 755` 断言 → `create-dmg`（参数见 4.1） |
| `首次运行（去隔离）.command` | 去隔离脚本（正文见下），`chmod 755` |
| `安装说明.txt` | 三步：拖进「应用程序」→ 双击去隔离脚本 → 若仍被拦：**系统设置 → 隐私与安全性 → 仍要打开** |
| `background.png` / `background@2x.png` | 4.2 生成的素材 |

`.command` 正文要点（完整正文在实施时落盘）：目标优先 `/Applications/DevDesk.app`、否则回退到 `$(dirname "$0")/DevDesk.app`；`xattr -dr com.apple.quarantine "$TARGET"`；失败给中文提示；末尾 `read` 不自动关窗；`set -u`；**不写**任何 Gatekeeper 全局降级。

### 4.4 必须在 Mac 上做的事（本机无 Mac，不可验证）

1. `iconutil` 生成 `DevDesk.icns`（App 图标）与卷图标 `.VolumeIcon.icns`（仓库现零 `.icns`）。
2. `create-dmg` 实跑（含 AppleScript 美化可能需要"终端控制访达"授权）。
3. `background@2x.png` 是否被 Finder 采用（create-dmg 无对应参数）。
4. `open`/`.command` 的 0755 与 LaunchServices 默认处理器行为。
5. DMG 内 `assets/` 与皮肤根（`pet_skin.cpp` 的 cwd 依赖）修复后的真机表现。

## 5. 文件改动清单

| 文件 | 动作 |
|------|------|
| `installer/windows/devdesk.iss` | 新增（§3.1） |
| `scripts/package-windows.sh` | 新增（§3.2；不改 `build-test-run.sh`） |
| `scripts/package-macos.sh` | 新增（§4.1/4.3） |
| `installer/macos/首次运行（去隔离）.command` | 新增（§4.3） |
| `installer/macos/安装说明.txt` | 新增 |
| `installer/macos/background.png` / `background@2x.png` | 新增（生成后入库） |
| `doc/scripts/gen_dmg_background.py` + `doc/scripts/index.md` | 新增脚本 + 登记 |
| `platform/macos/build-app.sh` | 补：拷 `assets/`（含 pets）到 bundle 内，并按 exe 相对路径放置 |
| `src/module_pet/pet_skin.cpp` | 皮肤根改为**可执行文件所在目录**（消除 cwd 依赖；与 `pet_skin.h` 注释对齐） |
| `platform/macos/Info.plist` | 版本号改为由 CMake 生成/注入（D-7；可与 §6 的文档同步一起做） |
| `CMakeLists.txt` | 需要时为 macOS 侧导出 `DEVDESK_VERSION` / 生成 plist |
| `.gitignore` | 增补 `installer/**/out/`、`*.dmg` |
| `CHANGELOG.md` / `README.md` / `docs/KNOWN_LIMITATIONS.md` | 发布形态口径改写（单 exe 降级为"现状"；新增"安装包/ DMG"入口）——**注意避让另一会话的波 1 文档同步线，落地前先对账** |

## 6. 工具获取（当前唯一阻塞）

本机 `ISCC.exe` 缺失且**今晚无法获取**（`github.com` 被公司网拦）。解除方式（任一）：
1. 在网络可达 GitHub 的机器上装 `winget install --id JRSoftware.InnoSetup --scope user`，或
2. 手工下载官方安装包（`innosetup-6.7.3.exe`，官方页面 `https://jrsoftware.org/isdl.php`）后在本机 `/CURRENTUSER /VERYSILENT` 安装，或
3. 提供安装包文件由本机安装。

装好后：`scripts/package-windows.sh` 可直接跑通（`ISCC` 三段式探测会自动找到）。

## 7. 不做（YAGNI）

1. 不做签名/公证（D-11）；不碰 Gatekeeper 全局降级。
2. 不做文件关联、开机自启、便携模式（D-6，登记为后续）。
3. 不做 MSIX/WiX/CPack 路线（CPack 的 FreeType 陷阱见 §2）。
4. 不做"一次构建双平台产物"（DMG 只能在 Mac 上构建，D-8/§4.4）。
5. **不改链接方式**（D-12）；不动 `build-test-run.sh`。

## 8. 测试与验收

| 层 | 内容 |
|----|------|
| 静态 | `.iss` 语法由 ISCC 编译即验；脚本 `bash -n` 语法检查；背景素材尺寸断言（700×440 / 1400×880） |
| Windows 安装器（工具到位后） | §3.3 的静默装/断言/卸载全链；**特别断言无 UAC**、`%APPDATA%\DevDesk` 卸载后仍在 |
| macOS | 本机**不可验证**；静态评审 + Mac 上按 §4.4 清单逐条过 |

## 9. 拆任务（每项 ≤0.5 天；涉及构建/测试的子任务 worktree 隔离）

| 里程碑 | 内容 | 阻塞 | 验收 |
|--------|------|------|------|
| **M1** 素材与生成器 | `doc/scripts/gen_dmg_background.py` + 生成两张背景图入库 + 尺寸断言 | — | 本机可复跑、产物尺寸正确 |
| **M2** 脚本骨架 | `scripts/package-windows.sh` + `scripts/package-macos.sh` + `installer/**` 全部文本文件（.iss/.command/说明） + `bash -n` | `ISCC` 缺失（M3 阻塞） | 脚本可跑；Windows 脚本在缺 ISCC 时给清晰报错 + 安装命令 |
| **M3** Windows 安装器验收 | ISCC 编译 + 静默装/断言/卸载 | **需用户提供 ISCC** | §8 全绿 |
| **M4** macOS 侧修复 | `build-app.sh` 补 assets、`pet_skin` 皮肤根改 exe 目录、Info.plist 版本注入 | 无 Mac（不可验证） | 静态评审通过 |

## 10. 已接受的边界

1. DMG 只能在 Mac 上构建；macOS 分支本机不可验证（§4.4 清单）。
2. Windows 安装器本轮**无法端到端验证**（ISCC 缺失，§6）。
3. 未签名 App：用户需右键打开 / 系统设置"仍要打开" / 去隔离脚本三条路径之一；企业 MDM 管控机器可能另有拦截。
4. 发布形态口径变更：单 exe 零外部 DLL 从"硬需求"降级为"现状/可选优化"——**需同步改写文档（§5 最后一行，避让在飞文档线）**。

## 11. 待用户复核（自主拍板的默认值）

| # | 决策 | 备选 |
|---|------|------|
| D-1 | Inno Setup 6.7.3 | 7.1.0（新风格 `stellar` + 64 位安装器）；NSIS（需放弃"现代"） |
| D-2/D-3 | 全用户级、不给"所有人安装"选项 | 开 `PrivilegesRequiredOverridesAllowed=dialog` |
| D-4 | 暖白/暖墨双色 + 无分割线 + clay 图标 | 再投入做品牌背景位图 |
| D-5 | 卸载不删用户数据 | 提供"同时删除我的数据"勾选项 |
| D-6 | 不做文件关联 | 做（DevDesk 已支持 argv 打开文件） |
| D-11 | 不签名、给去隔离脚本 | 买 Apple Developer（99 USD/年）后做签名+公证 |
| D-12 | 不改链接方式、把约束降级 | 真改成动态链接（需动 EUI-NEO + 随包 18 个 DLL） |
| — | Inno 商业许可**不购买** | 自愿购买（永久 + 2 年更新） |
