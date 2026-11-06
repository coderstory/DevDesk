// src/module_wallpaper/platform/importer_stub.cpp
// 「从花見导入」的其它平台 stub：花見（Huajian）是 macOS App，其它平台没有这个
// 来源目录。语义对齐同族 *_stub.cpp：available()=false、run() 直接报 sourceMissing
// （service 的 UI 按钮由 available() 把门，根本不会出现；这里兜链接符号与防御调用）。
#include "module_wallpaper/platform/importer_macos.h"

namespace module::wallpaper::importer {

bool hanamiImportAvailable() {
    return false;
}

std::string defaultHanamiSourceDirectory() {
    return std::string();
}

std::string defaultTombstoneDatabasePath() {
    return std::string();
}

SourceDirectoryStatus probeHanamiSourceDirectory(const std::string&) {
    return {};  // stub 平台无花見来源（available=false 把门），无可读/无权限两态
}

void openFullDiskAccessSettings() {
    // 「完全磁盘访问」面板是 macOS 专属；其它平台 no-op。
}

ImportReport runHanamiImport(const std::string& destinationFolder,
                             const std::function<void(int, int)>& onProgress,
                             const std::atomic<bool>* cancel) {
    (void)destinationFolder;
    (void)onProgress;
    (void)cancel;
    ImportReport report;
    report.sourceMissing = true;
    return report;
}

} // namespace module::wallpaper::importer
