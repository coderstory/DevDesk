// src/platform/file_trash_posix.cpp
// 其它 POSIX 兜底：不做直删（误删不可回收 = 数据丢失），诚实返回 TrashUnavailable，
// 由调用方降级成「当前平台不支持删除」的提示文案。
#include "file_trash.h"

namespace platform::file_trash {

TrashOutcome moveToTrash(const std::string& path) {
    (void)path;
    return TrashOutcome{TrashFailure::TrashUnavailable};
}

}  // namespace platform::file_trash
