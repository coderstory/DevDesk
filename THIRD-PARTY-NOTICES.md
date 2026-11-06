# 第三方组件告示（THIRD-PARTY NOTICES）

本仓库打包分发的第三方组件及其许可、来源。凡随包分发的二进制必须在此登记一条；
新增条目沿用下面的字段格式。

## mpv（Windows 播放后端）

- 用途：壁纸模块视频/图片播放后端（`assets/mpv/mpv.exe`，随安装包分发）
- 构建来源：shinchiro 构建（mpv-winbuild-cmake）
  - 构建仓库：<https://github.com/shinchiro/mpv-winbuild-cmake>
- 上游项目主页：<https://mpv.io>
- 上游源码：<https://github.com/mpv-player/mpv>
- 许可证：GPLv2+（GNU General Public License 第 2 版或更高，见
  <https://www.gnu.org/licenses/old-licenses/gpl-2.0.html>）
- 打包入口：`scripts/package-windows.sh`（`MPV_BIN` 拷入 `assets/mpv/mpv.exe`；
  运行时解析见 `src/module_wallpaper/platform/mpv_controller.h` 的
  `resolveMpvExecutable`）

## libvterm（Windows 终端 VT 解析）

- 用途：终端模块 Windows 后端的 VT220/xterm/ECMA-48 序列解析（vendored 静态链入
  主程序，无独立文件分发）
- 上游源码：<https://github.com/neovim/libvterm>（nvim 分支，锁 commit `934bc2f`；
  上游 leonerd v0.3.3 系）
- 许可证：MIT
- 引入方式：git submodule `third_party/libvterm` + 仓内 CMake 胶水
  （根 `CMakeLists.txt`；submodule 零写入）

## ConPTY v2 redist（Windows 终端伪控制台宿主）

- 用途：终端模块 Windows 后端的 pinned 伪控制台（`conpty.dll` + `OpenConsole.exe`
  成对随包，落安装目录 `conpty\`；运行时三级查找，不可用回落系统 inbox ConPTY）
- 上游源码：<https://github.com/microsoft/terminal>（ConPTY 组件）
- 版本 pin：随 node-pty 的 pinned 副本（研究核证时点 1.25.260303002）；确切版本与
  SHA256 以 `third_party/conpty/README.md` 回填表为准
- 获取渠道：node-pty 树内 `third_party/conpty/`（主）/ NuGet 包
  `Microsoft.Windows.Console.ConPTY`（备）
- 许可证：MIT
- 引入方式：手工放包 `third_party/conpty/`（版本 pin 靠仓内二进制钉死，构建/打包/
  装机链拷至 exe 旁 `conpty\`；发布形态守卫白名单覆盖）

## Noto Sans CJK SC（子集随包，UI 正文字体）

- 用途：跨平台 UI 正文字体统一（need:132，`assets/DevDeskSansSC-Regular.otf` +
  `assets/DevDeskSansSC-Bold.otf`，family 改名 `DevDesk Sans SC`）
- 上游项目：<https://github.com/notofonts/noto-cjk>（release Sans2.004，
  `08_NotoSansCJKsc.zip`）
- 许可证：SIL Open Font License 1.1，全文随包 `assets/OFL-NotoSansCJK.txt`
- 修改声明：子集化（GB2312 全集+源码码点，7150 码点）+ 改名避开 OFL Reserved
  Font Name，详见随包 `assets/FONT-MODIFICATIONS.txt` 与
  `scripts/make_font_subset.py`

## JetBrains Mono（原文件随包，等宽字体）

- 用途：终端 / 代码块等宽字体统一（need:132，`assets/JetBrainsMono-Regular.ttf`，
  原文件捆绑未修改）
- 上游项目：<https://github.com/JetBrains/JetBrainsMono>（v2.304）
- 许可证：SIL Open Font License 1.1，全文随包 `assets/OFL-JetBrainsMono.txt`
- 修改声明：无修改（Original Version），说明见随包 `assets/FONT-MODIFICATIONS.txt`


