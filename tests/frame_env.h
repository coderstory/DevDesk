// tests/frame_env.h
// 无头帧驱动（TIN-06 收尾波）：FocusEnv / MdLiveEnv / SpikeEnv 三合一。
// 原三份 body 逐字重复（core::dsl::Runtime rt + eui::Ui* ui + frame() 调
// rt.compose(pageId, 1600.0f, 1000.0f)），仅 pageId 不同（"mdfocus" /
// "mdlive" / "mdspike"）；pageId 不是装饰：state / 焦点 / 元素 id 都以
// pageId 起头做命名空间，换名等于换命名空间——合流必须保留调用方原 pageId
// 字面量。header-only（tests/ 已在 target_include_directories，零 CMake 改动）；
// 独立于 test_support.h：dsl_runtime.h 重，收它要扩张共享头 include 面，
// 故三合一放本头（TIN-06 决策 1）。
#pragma once

#include <core/dsl_runtime.h> // core::dsl::Runtime（compose 帧末消费 requestFocus）
#include <eui/dsl.h>          // eui::Ui / eui::Screen

#include <string>
#include <utility>

namespace test_support {

// 无头帧驱动：唯一入口是 Runtime::compose（requestFocus 的帧末消费端在
// Runtime::compose，plain Ui 不消费挂起请求）。pageId 恒定——state/焦点/
// 元素 id 都以 pageId 起头做命名空间，换名等于换命名空间。
struct HeadlessFrame {
    core::dsl::Runtime rt;
    eui::Ui* ui = nullptr;
    std::string pageId;
    explicit HeadlessFrame(std::string pageId) : pageId(std::move(pageId)) {}
    template <typename Fn>
    void frame(Fn&& body) {
        rt.compose(pageId, 1600.0f, 1000.0f,
                   [&](eui::Ui& u, const eui::Screen&) {
                       ui = &u;
                       body(u);
                   });
    }
};

} // namespace test_support