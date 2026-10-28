; DevDesk Windows 用户级安装脚本（Inno Setup 6.7.3）
; 设计依据：docs/superpowers/specs/2026-09-28-installer-and-dmg-design.md §3.1
;
; 本文件不含任何仓库相对路径：{#ExeDir} / {#AssetsDir} / {#MyAppVersion} 三个
; ISPP 定义全部由 scripts/package-windows.sh 用 ISCC /D... 命令行注入
; （不存在 <ReleaseTree> 之类的自定义 token）。手工验证时的调用形态：
;   ISCC /DExeDir=<构建目录绝对路径> /DAssetsDir=<构建目录>\assets ^
;        /DMyAppVersion=0.1.0 installer\windows\devdesk.iss
;
; 全程用户级安装（PrivilegesRequired=lowest，没有任何进程提权）：
; 安装目录落 %LOCALAPPDATA%\Programs\DevDesk，快捷方式与卸载项全写 HKCU。

[Setup]
; AppId 生成一次后固化（升级识别 / 卸载识别用；后续任何改动都会让升级装成"另一款软件"）：
;   python -c "import uuid;print('{{'+str(uuid.uuid4()).upper()+'}')"
; Inno 语法：{{ 表示一个字面 {，故下面这行的最终 AppId 值为 {7A4378A7-8C89-4CC5-9D58-268C59268240}
; （花括号转义形态在 M3 首次 ISCC 实编译时校验）
AppId={{7A4378A7-8C89-4CC5-9D58-268C59268240}
AppName=DevDesk
; 版本号单一源 = CMakeLists.txt 的 project(DevDesk VERSION ...)，由打包脚本从 CMakeCache 取值注入
AppVersion={#MyAppVersion}
AppPublisher=DevDesk
; 低权限安装 → %LOCALAPPDATA%\Programs\DevDesk（Win7+ 上 {autopf} 的用户级形态）
DefaultDirName={autopf}\DevDesk
DisableProgramGroupPage=yes
; 全程不提权；也不给"为所有人安装"的入口（不开 PrivilegesRequiredOverridesAllowed）
PrivilegesRequired=lowest
OutputDir=out
OutputBaseFilename=DevDesk-{#MyAppVersion}-setup
; 图标真身 = DevDesk 仓库 assets/icon.ico（2026-10-20 C 方案终端窗 >_；经 package-windows.sh
; 的 AssetsDir 暂存链同树拷入构建目录的 assets/，同名字义下覆盖 EUI-NEO 的同名旧图）
SetupIconFile={#AssetsDir}\icon.ico
UninstallDisplayIcon={app}\DevDesk.exe
; 卸载项显示名（"应用和功能"里看到的名字）
UninstallDisplayName=DevDesk
; 外观：跟随系统深色的 modern 向导 + DevDesk 暖白/暖墨底色 + 去分割线
WizardStyle=modern dynamic hidebevels
WizardBackColor=#FAF9F5
WizardBackColorDynamicDark=#1A1816
Compression=lzma2
SolidCompression=yes

[Languages]
; 默认语言 = 简体中文：chinesesimp 排在最前，Inno 以首个语言条目为默认语言（缺省不经
; 首屏语言下拉提问，直接以该语言显示向导），契合中文用户环境，也与 [Run] 段既有中文
; 文案（"启动 DevDesk"）一致；english 排在后面，供需要英文界面的用户在首屏下拉切换。
; 中文 ISL 自托管理由：官方编译器自带的 Languages 目录在本机 Inno 6.7.3 安装里缺失
; 全部中文文件（仅 29 个语言文件、无 ChineseSimplified.isl），若用 compiler: 前缀引用
; 会直接编不过。故把官方 issrc 仓库 Files/Languages/ChineseSimplified.isl（Inno 6.5.0+
; 简体中文消息，与 6.7.3 兼容）随仓库放在 Languages/ 下，经 {#SourcePath}（ISPP 内置，
; = 本 .iss 所在目录）引用——任何机器的 ISCC 都能编译，不依赖 Inno 安装目录的完整语言包。
; english 用 compiler:Default.isl：本机 Inno 安装同样缺 English.isl，而 Default.isl 即
; 官方英文基准（含 [LangOptions] LanguageName=English / LanguageID=$0409，与 English.isl
; 等价），无需再自托管一份。官方默认中文文案已覆盖全部向导按钮与提示（"下一步"/"安装"/
; "完成"/"浏览" 等均现代友好），故不做 [CustomMessages] 覆盖。
Name: "chinesesimp"; MessagesFile: "{#SourcePath}Languages\ChineseSimplified.isl"
Name: "english";     MessagesFile: "compiler:Default.isl"

[Files]
Source: "{#ExeDir}\DevDesk.exe";  DestDir: "{app}"; Flags: ignoreversion
; THIRD-PARTY-NOTICES.md 随包分发（mpv GPLv2+ 合规：GPL 分发须随附许可告示）。
; 源路径走 {#SourcePath}（= 本 .iss 所在目录，含尾部反斜杠，与上面 Languages 条目同款），
; ..\.. 回到仓库根——文件随仓库走，任何机器的 ISCC 都能就地取材，不新增 /D 注入。
Source: "{#SourcePath}..\..\THIRD-PARTY-NOTICES.md"; DestDir: "{app}"; Flags: ignoreversion
; Excludes：排除 17MB 死重量 NotoSansSC-VF.ttf —— 该字体是变量字体，FreeType 加载不稳、
; 当前未启用（见 DevDesk/src/main.cpp:487 注释），运行时字体走 EUI-NEO
; core/render/text.cpp:607-625 的 fallback 链（系统 CJK 字体微软雅黑 msyh 优先，
; 打包 JingNanJunJunTi 作兜底），排除不影响任何界面渲染。Excludes 是 [Files]
; 的合法参数（Inno 6.7），支持通配符，多个模式用分号分隔。
Source: "{#AssetsDir}\*";         DestDir: "{app}\assets"; Flags: ignoreversion recursesubdirs createallsubdirs; Excludes: "NotoSansSC-VF.ttf"

[Icons]
; WorkingDir 必设：EUI-NEO 的 image_source.cpp 查找顺序是 cwd/assets 优先于 exeDir/assets，
; 不设工作目录时从快捷方式启动可能找不到 assets
Name: "{userprograms}\DevDesk"; Filename: "{app}\DevDesk.exe"; WorkingDir: "{app}"
Name: "{userdesktop}\DevDesk";  Filename: "{app}\DevDesk.exe"; WorkingDir: "{app}"

[Run]
Filename: "{app}\DevDesk.exe"; Description: "启动 DevDesk"; WorkingDir: "{app}"; Flags: nowait postinstall skipifsilent

; 已删 / 未写的项（都有理由，勿"顺手补上"）：
; - 不写 NoModify / NoRepair：它们不是 Inno 指令；官方 Setup.Install.pas 在无
;   AppModifyPath 时默认就会写这两个值。
; - 不写 [UninstallDelete] Type: filesandordirs; Name: "{app}"：官方帮助明确劝阻用
;   通配删 {app}；[Files] 装进去的文件 Inno 本来就会自动删。
; - 不写 {commonprograms} / 任何 HKLM 项：任何 HKLM 写入都会触发 UAC。
