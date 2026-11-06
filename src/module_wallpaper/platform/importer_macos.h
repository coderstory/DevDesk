// src/module_wallpaper/platform/importer_macos.h
// 「从花見导入」的 macOS 实现接口（业务代码只调这里的函数，零平台宏）。
//
// 分工：core/importer.h 是纯决策（去重顺序、计数），本层负责真实的文件系统动作——
// 枚举花見容器目录、取文件大小、探针判视频轨、tmp+rename 拷贝、SQLite 墓碑。
//
// 线程契约：runHanamiImport() 是阻塞调用，**必须在后台线程跑**（拷贝几十个
// 上百 MB 的视频是分钟级）；进度经由回调上报，回调在工作线程触发，
// 实现方不得在里面碰 UI。
#pragma once

#include <atomic>
#include <functional>
#include <string>

#include "module_wallpaper/core/importer.h"

namespace module::wallpaper::importer {

// 「从花見导入」是否可用（花見是 macOS App；其它平台 stub 返回 false，
// service 据此决定是否构建导入按钮——业务层不写平台宏，能力查询走这里）。
bool hanamiImportAvailable();

// 花見（Huajian）App 的下载目录（容器内 Documents/Videos）。
// 不存在报 sourceMissing；存在但读不了（TCC）报 sourceNoAccess——两者分 fields。
std::string defaultHanamiSourceDirectory();

// 源目录可达性（bug200 症状①）：「未安装」与「无权限」原来是同一降级，
// 用户看到的都是「找不到花見下载目录」——TCC 拒绝时给的是误导文案。
// 现在区分：目录存在但枚举被拒 = noAccess（service 弹窗分流「未授予完全磁盘访问」）。
struct SourceDirectoryStatus {
    bool readable = false;  // 可枚举
    bool noAccess = false;  // 目录存在但枚举被拒（TCC）
};
SourceDirectoryStatus probeHanamiSourceDirectory(const std::string& directory);

// 打开 macOS「完全磁盘访问」设置面板（无权限弹窗按钮的落点；用户点了才开，
// 禁止自动打开）。旧 URL 串在新 macOS 失效时回退新串；其它平台 stub 为 no-op。
void openFullDiskAccessSettings();

// 导入到 destinationFolder。onProgress 可为空；跑完返回报告。
// cancel 非空且被置真时，在**候选之间**尽早返回（正在拷贝的那一个会拷完）；
// 报告携带已发生部分的计数。
ImportReport runHanamiImport(const std::string& destinationFolder,
                             const std::function<void(int done, int total)>& onProgress,
                             const std::atomic<bool>* cancel = nullptr);

// 墓碑库的默认落点（DevDesk 数据根下，随应用数据一起备份）。
// 墓碑写入只有一条路径：每次导入开头把 ~/.Trash 顶层的 mp4 幂等回填进库
// （DevDesk 壁纸 UI 无删除入口，删除走 Finder→废纸篓，回填即删除钩子）。
std::string defaultTombstoneDatabasePath();

} // namespace module::wallpaper::importer
