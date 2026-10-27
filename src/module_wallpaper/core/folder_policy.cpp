#include "module_wallpaper/core/folder_policy.h"

#include <cctype>
#include <cstddef>
#include <filesystem>
#include <system_error>

namespace module::wallpaper::folder_policy {

namespace {

std::string trim(const std::string& value) {
    std::size_t begin = 0;
    std::size_t end = value.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(value[begin])) != 0) ++begin;
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) --end;
    return value.substr(begin, end - begin);
}

} // namespace

bool isFolderConfigured(const std::string& folderPath) {
    return !trim(folderPath).empty();
}

std::string normalizeFolderPath(const std::string& rawPath) {
    const std::string trimmed = trim(rawPath);
    if (trimmed.empty()) return {};
    std::filesystem::path path(trimmed);
    std::error_code ec;
    const std::filesystem::path absolute = std::filesystem::absolute(path, ec);
    if (!ec) path = absolute;
    path = path.lexically_normal();
    std::string text = path.make_preferred().string();
    // 去掉末尾分隔符；盘符根（"D:\"）与 POSIX 根（"/"）必须保留，否则变成空串。
    const std::string root = path.root_path().make_preferred().string();
    while (text.size() > root.size() &&
           (text.back() == '/' || text.back() == '\\')) {
        text.pop_back();
    }
    return text;
}

bool isAcceptableFolder(const std::string& normalizedPath) {
    if (normalizedPath.empty()) return false;
    std::error_code ec;
    const std::filesystem::file_status status =
        std::filesystem::status(std::filesystem::path(normalizedPath), ec);
    if (ec) return false;
    return std::filesystem::is_directory(status);
}

bool shouldRequestFolderSelection(const std::string& folderPath, Availability availability) {
    if (!isFolderConfigured(folderPath)) return true;
    return availability != Availability::Playing;
}

} // namespace module::wallpaper::folder_policy