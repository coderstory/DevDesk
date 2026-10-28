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
