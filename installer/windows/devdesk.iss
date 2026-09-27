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
; 图标属 EUI-NEO assets（即构建目录的 assets/），由 /DAssetsDir 注入
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

[Files]
Source: "{#ExeDir}\DevDesk.exe";  DestDir: "{app}"; Flags: ignoreversion
Source: "{#AssetsDir}\*";         DestDir: "{app}\assets"; Flags: ignoreversion recursesubdirs createallsubdirs

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
