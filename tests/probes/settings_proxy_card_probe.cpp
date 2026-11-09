// settings_proxy_card_probe.cpp — need155 代理卡重设计证据探针（三卡高态 + 亮暗 + 挤压档）
//
// 纯证据物（无断言，判据归 doctest 用例与看图会话）：把 settings 通用 tab 用
// 参数注入的代理运行面（运行/接管/警告 0-2 条）构图一帧落 PNG。
//
// 用法：
//   settings_proxy_card_probe --theme light --warns 2 --size 1000x640
//       --out evidence/unify-155/after --tag after-light-w2
//   --theme light|dark（缺省 light）  --warns 0|1|2（缺省 0）
//   --stopped（未运行/未接管态；缺省运行+已接管常态）
//   --size WxH（缺省 1000x640 最小窗；624x640 = 卡宽 560 挤压档）
//   --out 输出目录（缺省 evidence/unify-155）  --tag 文件名（缺省 shot）
//
// 链接面：else 分支最小链接（settings 模块 + 桥缝），同 settings_dropdown_z_probe。
#include "probes/probe_harness.h"
#include <eui/detail/dsl_app_impl.h>  // app:: 层内联实现自给（链接需要）
#include "test_support.h"             // TestEnv（appDatabase 注册，配置写临时目录）
#include "app_registry.h"             // create_settings_module 工厂声明
#include "core/claude_proxy_service.h"
#include "core/module_interface.h"
#include "core/service_locator.h"     // ScopedService（假运行面注册）
#include "core/theme.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

// dsl_app_impl.h 引用的两个符号：探针不建应用级 UI，compose 空实现仅满足链接
//（同 settings_dropdown_z_probe 的自给口径）。
namespace app {
const app::DslAppConfig& dslAppConfig() {
    static const DslAppConfig config =
        DslAppConfig{}.title("devdesk_settings_proxy_card_probe");
    return config;
}
void compose(eui::Ui&, const eui::Screen&) {}
}  // namespace app

namespace {

// 参数注入的假运行面：只服务构图证据，语义与 IClaudeProxyService 注释一致。
class FakeProxyService final : public ::core::IClaudeProxyService {
public:
    bool running_ = true;
    int port_ = 8888;
    bool managed_ = true;
    std::vector<std::string> warns_;
    std::string path_ = "/Users/demo/.claude/settings.json";

    bool isProxyRunning() const override { return running_; }
    int proxyPort() const override { return port_; }
    void setConfiguredProxyPort(int port) override { port_ = port; }
    void stopProxy() override { running_ = false; }
    ::core::Result<int> startProxy(const std::string&, int port) override {
        running_ = true;
        port_ = port;
        return ::core::Result<int>::ok(port);
    }
    bool isClaudeCodeManaged() const override { return managed_; }
    std::vector<std::string> lastSyncWarnings() const override {
        return warns_;
    }
    std::string settingsFilePath() const override { return path_; }
};

}  // namespace

int main(int argc, char* argv[]) {
    std::string theme = "light";
    int warns = 0;
    bool stopped = false;
    int w = 1000;
    int h = 640;
    std::string out = "evidence/unify-155";
    std::string tag = "shot";
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](std::string& v) {
            if (i + 1 < argc) v = argv[++i];
        };
        if (a == "--theme") {
            next(theme);
        } else if (a == "--warns") {
            if (i + 1 < argc) warns = std::atoi(argv[++i]);
        } else if (a == "--stopped") {
            stopped = true;
        } else if (a == "--size") {
            if (i + 1 < argc) std::sscanf(argv[++i], "%dx%d", &w, &h);
        } else if (a == "--out") {
            next(out);
        } else if (a == "--tag") {
            next(tag);
        }
    }

    if (!probe::initializeWindowBackend()) return 1;
    auto window = probe::makeWindow("settings_proxy_card_probe", w, h);
    if (!window) {
        probe::terminateWindowBackend();
        return 2;
    }

    test_support::TestEnv env("settings_proxy_probe", "proxycard", true);

    FakeProxyService fake;
    fake.running_ = !stopped;
    fake.managed_ = !stopped;
    if (warns >= 1) {
        fake.warns_.push_back(
            "接管失败：settings.json 解析错误（行 1）——已还原原配置");
    }
    if (warns >= 2) {
        fake.warns_.push_back(
            "同步未完成：本地分支领先 origin/dev 2 个提交（示例警告 B）");
    }
    ::core::ScopedService<::core::IClaudeProxyService> reg{&fake};

    std::unique_ptr<::core::Module> mod(create_settings_module());
    mod->onStart();
    mod->onPageEnter();  // 刷新接管状态快照（compose 只读快照）
    // 主题在 onStart 之后置档：settings onStart 会按配置覆回默认档（uat62 已知坑）。
    if (theme == "dark") ::core::theme::setDark();
    else ::core::theme::setLight();

    ::core::dsl::Runtime runtime;
    runtime.initialize(window);
    // 帧泵两遍（memory_probe 口径）：首遍建树，次遍才出图，单遍截到空帧。
    for (int f = 0; f < 2; ++f) {
        probe::composeModuleFrame(
            runtime, window, "settings", static_cast<float>(w),
            static_cast<float>(h),
            [&](eui::Ui& u, const eui::Screen& screen) {
                mod->compose(u, screen);
            });
    }
    probe::FrameCapture capture(window);
    std::filesystem::create_directories(out);
    const std::string path = out + "/" + tag + ".png";
    const bool ok = capture.ok() && capture.capturePng(runtime, path);
    mod->onStop();
    probe::terminateWindowBackend();
    if (!ok) {
        std::cerr << "settings_proxy_card_probe: 落图失败 "
                  << (capture.ok() ? std::string("capturePng") : capture.error())
                  << "\n";
        return 3;
    }
    std::cout << "settings_proxy_card_probe: " << path << "\n";
    return 0;
}
