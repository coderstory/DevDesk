// src/module_markdown/markdown_module_test_seam.h
// Markdown 模块无头测试缝（唯一消费方 tests/test_modules/test_markdown.cpp）。
//
// 为什么需要它：磁盘轮询周期（默认 5 秒）是**墙钟**判据。用例里「本窗口内不发生
// 轮询」这类假设在负载下会破 —— >5 秒卡顿让轮询闯进窗口，rescan 会把手工插入的
// 索引行剪掉 / 改写列表缓存，造成假红（机理的确定性复现见
// md_module_poll_seam_is_live）。缝把周期变成可注入量，用例两个方向都能写**确定性**
// 判据：调到 1 小时（窗口内必不轮询）/ 调到 1ms（下一帧必轮询）。
//
// 形态：模块实例指针桥 + 自由函数（MarkdownModule 不暴露头文件，实现里用
// static_cast 桥接，与 settings 的 github_sync_test_seam.h 同思路）。**不设环境变量
// 门禁**：本缝只改轮询节流值，无 I/O、无凭据、无远端副作用，误用无害。
#pragma once

#include <cstdint>

namespace core {
class Module;
}

namespace module::markdown {

// 设定磁盘轮询节流周期（毫秒）。ms <= 0 或 mod 为空 → 忽略（保持现值）。
// 仅接受 create_markdown_module() 建出的实例（其他 core::Module 不是本类）。
void mdSetPollIntervalMsForTest(::core::Module* mod, int64_t ms);

} // namespace module::markdown
