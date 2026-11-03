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

#include <functional>
#include <string>

#include "module_wallpaper/core/importer.h"

namespace module::wallpaper::importer {

// 花見（Huajian）App 的下载目录（容器内 Documents/Videos）。
// 不存在或读不了时报 sourceMissing，不区分两者。
std::string defaultHanamiSourceDirectory();

// 导入到 destinationFolder。onProgress 可为空；跑完返回报告。
ImportReport runHanamiImport(const std::string& destinationFolder,
                             const std::function<void(int done, int total)>& onProgress);

// 墓碑库的默认落点（DevDesk 数据根下，随应用数据一起备份）。
// 墓碑写入只有一条路径：每次导入开头把 ~/.Trash 顶层的 mp4 幂等回填进库
// （DevDesk 壁纸 UI 无删除入口，删除走 Finder→废纸篓，回填即删除钩子）。
std::string defaultTombstoneDatabasePath();

} // namespace module::wallpaper::importer
