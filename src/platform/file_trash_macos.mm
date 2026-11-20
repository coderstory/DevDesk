// src/platform/file_trash_macos.mm
// macOS：NSFileManager trashItemAtURL:（10.8+）。系统负责移入 ~/.Trash 并在重名时
// 自动改名，因此**不消费** resultingItemURL 出参（Apple 注释明写该 API 会改名）。
// 本文件单独用 -fobjc-arc 编译（见 platform/CMakeLists.txt），不要手动 retain/release。
#import <Foundation/Foundation.h>

#include "file_trash.h"

#include <string>

namespace platform::file_trash {
namespace {

// UTF-8 → NSString；非法字节返回 nil（不能把乱码交给文件系统 API）
NSString* toNSString(const std::string& value) {
    return [[NSString alloc] initWithBytes:value.data()
                                    length:value.size()
                                  encoding:NSUTF8StringEncoding];
}

// NSError → 五档。只认 NSCocoaErrorDomain 的已知码，其余（含未知 domain）归
// TrashUnavailable/Busy：前者是「系统拒收」，后者是「占用/IO」，两者都不该被误报成
// 权限问题（文案会误导用户去改权限）。
TrashFailure mapNSError(NSError* error) {
    if (error == nil) return TrashFailure::TrashUnavailable;
    if (![error.domain isEqualToString:NSCocoaErrorDomain]) {
        return TrashFailure::Busy;
    }
    switch (error.code) {
        case NSFileNoSuchFileError:      // 4：源已不存在
        case NSFileReadNoSuchFileError:  // 260：同一语义的读侧变体
            return TrashFailure::NotFound;
        case NSFileReadNoPermissionError:   // 257
        case NSFileWriteNoPermissionError:  // 513
            return TrashFailure::PermissionDenied;
        default:
            // 废纸篓禁写 / 卷不支持 / 单文件超限等：系统不给专码，统一按「系统拒收」
            return TrashFailure::TrashUnavailable;
    }
}

}  // namespace

TrashOutcome moveToTrash(const std::string& path) {
    if (path.empty()) return TrashOutcome{TrashFailure::NotFound};

    // autoreleasepool 必须有：本函数在 worker 线程跑，无 AppKit 主 runloop 帮忙排水
    @autoreleasepool {
        NSString* filePath = toNSString(path);
        if (filePath == nil) return TrashOutcome{TrashFailure::NotFound};
        // 相对路径会被解析到 cwd，与本能力「path 为绝对路径」的契约不符 → 按未找到拒绝
        if (![filePath isAbsolutePath]) {
            return TrashOutcome{TrashFailure::NotFound};
        }

        NSError* error = nil;
        const BOOL ok = [[NSFileManager defaultManager]
            trashItemAtURL:[NSURL fileURLWithPath:filePath]
         resultingItemURL:nil
                    error:&error];
        if (ok) return TrashOutcome{};
        // error 的对象属本池：只取映射结果（enum），不出池任何 ObjC 对象
        return TrashOutcome{mapNSError(error)};
    }
}

}  // namespace platform::file_trash
