// src/platform/file_association_macos.mm
// macOS .md 文件关联（F-09，方案卡 §二.F-09，双路径=预置 A D-6）。
// 落点名偏差说明：卡面写 file_association_macos.cpp，实为 .mm——12+ 写入路径
// NSWorkspace 设默认无 C 等价 API（现行 toOpenContentTypeOfFileAtURL:
// completionHandler:；旧 toOpenURL: error 形态已随 27.2 SDK 整体移除），本 TU
// 必须 ObjC++（repo 先例 notification_macos.mm；.h 接口与注入缝零改动）。
// 架构红线：版本→API 路径唯一权威 = file_association_api_map.h（纯逻辑单测钉死），
// @available 仅编译期可用性守卫，运行期判定收敛本单文件（平台宏不出 *_macos）。
// clue10 口径（同 win）：写入成功 ≠ 双击行为生效——setAsDefaultForMd 返回值 =
// 写后回查判属；macOS 11–15 逐版本定性（.md 编号内建与否/NSWorkspace 确认框）
// = UAT 项，预置 A 先行零返工。
// 注入缝口径（与 U6 同款 AssocConfig，mac 侧语义差异）：
//   · config.progId = 查询判属锚（回读 bundle id 比对串）；生产缺省（win 字面量
//     "DevDesk.md"）在 mac 无意义 → 解析为运行期本应用 bundle identifier；
//     显式传入非缺省 progId（mac 测试用隔离 bundle id）则按字面量比对。
//   · 写入目标恒为本应用 bundle id + 自身 app URL（mac 设默认的合法对象是真实
//     app，异于 win 自建 ProgId 注册键可任写）。
// mac 无注销 API（LS 默认 handler 只能改设不能删）→ removeRegistrationForTest
// 诚实 NotImplemented（PRD D-4 生产不注销，清理缝 mac 面无消费场景）。
#include "file_association.h"

#include "file_association_api_map.h" // 版本→写入 API 路径唯一权威

#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>
// 27.2 SDK 实测（CI run 43889570987）：伞头不引全 UTType.h——类与常量可见、
// +typeWithTag: 方法声明不可见（no known class method 同证）→ 显式引类头/常量头
#import <UniformTypeIdentifiers/UTType.h>
#import <UniformTypeIdentifiers/UTTagClass.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h> // UTType 类反查（11.0+；C API 12.0 起弃用）

#include <CoreServices/CoreServices.h> // UTTypeCreatePreferredIdentifierForTag（<12 保守路径）/ LSCopyDefaultApplicationURLForURL / LSSetDefaultRoleHandlerForContentType
#include <dispatch/dispatch.h> // NSWorkspace completionHandler → 同步 Result 桥接（信号量+超时）
#include <sys/sysctl.h>

#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>

namespace platform::file_association {
namespace {

// spy 缝存储：null = 生产真刷新（与 _win 同款语义，单测装 spy 用例退出前恢复）
ShellNotifySink* g_notifySink = nullptr;

std::string cfToStdString(CFStringRef s) {
    if (s == nullptr) return {};
    const CFIndex maxLen = CFStringGetMaximumSizeForEncoding(
        CFStringGetLength(s), kCFStringEncodingUTF8) + 1;
    std::string buf(static_cast<size_t>(maxLen), '\0');
    if (!CFStringGetCString(s, buf.data(), maxLen, kCFStringEncodingUTF8)) return {};
    buf.resize(std::strlen(buf.c_str()));
    return buf;
}

CFStringRef cfStringFromStdString(const std::string& s) {
    return CFStringCreateWithCString(kCFAllocatorDefault, s.c_str(),
                                     kCFStringEncodingUTF8);
}

// 运行期 macOS 主版本（kern.osproductversion，如 "12.6"→12）；失败回 0 =
// api_map 落 LS 保守路径（<12 分支，LSSet API 全版本可用）
int macOSMajorVersion() {
    char buf[32] = {};
    size_t size = sizeof(buf);
    if (::sysctlbyname("kern.osproductversion", buf, &size, nullptr, 0) != 0) {
        return 0;
    }
    return std::atoi(buf);
}

// ".md" → "md"（UTTypeCreatePreferredIdentifierForTag 要不带点扩展名）
std::string extensionWithoutDot(const AssocConfig& config) {
    std::string ext = config.extension;
    if (!ext.empty() && ext.front() == '.') ext.erase(ext.begin());
    return ext;
}

// 扩展名 → LS 侧实际生效 UTI：运行期按 tag 反查，不硬编码 UTI 串（声明了
// UTImportedTypeDeclarations 的机器得 net.daringfireball.markdown，未声明机器
// 得 dyn.*——写/查同法反查，两侧对称）
core::Result<std::string> contentTypeForExtension(const std::string& extNoDot) {
    CFStringRef const extCf = cfStringFromStdString(extNoDot);
    if (extCf == nullptr) {
        return core::Result<std::string>::fail(core::ErrorCode::Internal,
                                               "扩展名转 CFString 失败");
    }
    std::string out;
    // UTTypeCreatePreferredIdentifierForTag：macOS 12.0 起弃用，但弃用≠移除——
    // 27.2 SDK 仍声明、符号仍可链（deprecated API 的兼容承诺）。CI 实证：clang 的
    // -Wdeprecated-declarations 不随 @available 分支自动消，须 pragma 显式包裹
    //（push/pop 成对）。现代 UTType 类迁移两轮尝试（typeWithTag/typesWithTag
    // 两种拼法）均被 CI 证伪「no known class method」——Swift/ObjC 命名面差异
    // 无法在无 Mac 编译环境可靠分辨，留给 Mac 侧真编译会话收口（线索 187）。
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    CFStringRef const uti = UTTypeCreatePreferredIdentifierForTag(
        kUTTagClassFilenameExtension, extCf, nullptr);
#pragma clang diagnostic pop
    if (uti == nullptr) {
        CFRelease(extCf);
        return core::Result<std::string>::fail(
            core::ErrorCode::Internal,
            "扩展名未解析出 UTI (" + extNoDot + ")");
    }
    out = cfToStdString(uti);
    CFRelease(uti);
    CFRelease(extCf);
    if (out.empty()) {
        return core::Result<std::string>::fail(core::ErrorCode::Internal,
                                               "UTI 转 string 失败 (" + extNoDot + ")");
    }
    return core::Result<std::string>::ok(out);
}

// 判属锚：缺省生产配置（win 字面量）→ 运行期本应用 bundle id；显式注入 → 按字面量
std::string resolveAnchor(const AssocConfig& config) {
    if (config.progId == productionConfig().progId) {
        CFBundleRef const main = CFBundleGetMainBundle();
        if (main == nullptr) return {};
        return cfToStdString(CFBundleGetIdentifier(main));
    }
    return config.progId;
}

// 本应用自身 bundle id + app URL（写入目标）；非 bundle 进程 = fail（诚实，
// 不伪装成功）
core::Result<std::pair<std::string, CFURLRef>> selfIdentity() {
    CFBundleRef const main = CFBundleGetMainBundle();
    if (main == nullptr) {
        return core::Result<std::pair<std::string, CFURLRef>>::fail(
            core::ErrorCode::Internal,
            "非 bundle 进程，无法设默认（DevDesk 须以 .app 形态运行）");
    }
    std::string const bundleId = cfToStdString(CFBundleGetIdentifier(main));
    if (bundleId.empty()) {
        return core::Result<std::pair<std::string, CFURLRef>>::fail(
            core::ErrorCode::Internal, "本应用 bundle identifier 为空，无法设默认");
    }
    CFURLRef const appUrl = CFBundleCopyBundleURL(main); // caller release
    return core::Result<std::pair<std::string, CFURLRef>>::ok({bundleId, appUrl});
}

// 探针文档 URL（文件无需存在：LS 按 URL 扩展名/UTI 判归属）——查询与 12+ 写入共用
CFURLRef probeFileUrl(const std::string& extNoDot) {
    CFStringRef const path = cfStringFromStdString(
        std::string("/private/tmp/DevDeskAssocProbe.") + extNoDot);
    CFURLRef const url = CFURLCreateWithFileSystemPath(
        kCFAllocatorDefault, path, kCFURLPOSIXPathStyle, false);
    CFRelease(path);
    return url; // caller release
}

// app URL → bundle id（查询回读侧）
core::Result<std::string> bundleIdForAppUrl(CFURLRef appUrl) {
    CFBundleRef const bundle = CFBundleCreate(kCFAllocatorDefault, appUrl);
    if (bundle == nullptr) {
        return core::Result<std::string>::fail(core::ErrorCode::Internal,
                                               "默认应用 URL 非 bundle，无法判属");
    }
    const std::string id = cfToStdString(CFBundleGetIdentifier(bundle));
    CFRelease(bundle);
    if (id.empty()) {
        return core::Result<std::string>::fail(core::ErrorCode::Internal,
                                               "默认应用 bundle identifier 为空");
    }
    return core::Result<std::string>::ok(id);
}

// 查询当前 .md 生效默认：LSCopyDefaultApplicationURLForURL（10.10+，12 上不弃用，
// 与 NSWorkspace URLForApplicationToOpenURL 同结果语义——卡 F-09「12+ 新旧均可」）。
// fail = 查询失败/无归属（调用方落 Unknown，绝不映射 Others）
core::Result<std::string> queryCurrentDefaultBundleId(const AssocConfig& config) {
    const std::string ext = extensionWithoutDot(config);
    if (ext.empty()) {
        return core::Result<std::string>::fail(core::ErrorCode::Internal,
                                               "扩展名为空，无法查询");
    }
    CFURLRef const docUrl = probeFileUrl(ext);
    CFErrorRef err = nullptr;
    CFURLRef const appUrl = LSCopyDefaultApplicationURLForURL(docUrl, kLSRolesAll, &err);
    CFRelease(docUrl);
    if (appUrl == nullptr) {
        std::string msg = "LaunchServices 查询 .md 默认应用失败（无生效 handler）";
        if (err != nullptr) {
            CFStringRef const desc = CFErrorCopyFailureReason(err);
            if (desc != nullptr) {
                msg += ": " + cfToStdString(desc);
                CFRelease(desc);
            }
            CFRelease(err);
        }
        return core::Result<std::string>::fail(core::ErrorCode::Internal, msg);
    }
    const core::Result<std::string> out = bundleIdForAppUrl(appUrl);
    CFRelease(appUrl);
    return out;
}

// 写后刷新（win SHChangeNotify must-call 的 mac 同位步骤）：mac 无关联变更广播
// API，LS handler 变更即时落 LS 偏好域，显式 synchronize 促跨进程可见；
// 实效定性（图标/Finder 即时性）= UAT 项
void launchServicesRefreshed() {
    if (g_notifySink != nullptr) {
        g_notifySink();
        return;
    }
    CFPreferencesAppSynchronize(CFSTR("com.apple.LaunchServices"));
}

} // namespace

core::Result<AssocState> isDefaultForMd(const AssocConfig& config) {
    const std::string anchor = resolveAnchor(config);
    if (anchor.empty()) {
        // 判属锚缺失（非 bundle 进程）= 无法判定 → Unknown（≠Others，语义见 .h）
        return core::Result<AssocState>::ok(AssocState::Unknown);
    }
    const core::Result<std::string> current = queryCurrentDefaultBundleId(config);
    return core::Result<AssocState>::ok(
        classifyState(current.isOk(), current.valueOr(""), anchor));
}

core::Result<bool> setAsDefaultForMd(const AssocConfig& config) {
    const std::string ext = extensionWithoutDot(config);
    if (ext.empty()) {
        return core::Result<bool>::fail(core::ErrorCode::Internal, "扩展名为空，无法设默认");
    }
    const core::Result<std::pair<std::string, CFURLRef>> self = selfIdentity();
    if (self.isFail()) {
        return core::Result<bool>::fail(self.errorCode(), self.errorMessage());
    }
    const core::Result<std::string> uti = contentTypeForExtension(ext);
    if (uti.isFail()) {
        CFRelease(self.value().second);
        return core::Result<bool>::fail(uti.errorCode(), uti.errorMessage());
    }

    // 写入（双路径，分支唯一权威 = api_map；@available 仅编译期守卫）
    const mac_api::WriteApi api = mac_api::writeApiFor(macOSMajorVersion());
    core::Result<void> written = core::Result<void>::fail(core::ErrorCode::Internal,
                                                          "写入路径未执行");
    if (api == mac_api::WriteApi::NsWorkspaceSetDefault) {
        if (@available(macOS 12.0, *)) {
            CFURLRef const docUrl = probeFileUrl(ext);
            NSURL* const docNs = (__bridge NSURL*)docUrl;
            NSURL* const appNs = (__bridge NSURL*)self.value().second;
            // 27.2 SDK：旧 toOpenURL:（error 出参/completionHandler 两形态）已整体
            // 移除（docs 404 + 编译器 no visible @interface 同证）；现行 =
            // toOpenContentTypeOfFileAtURL:（12.0+，按探针文件 URL 的 content
            // type 设默认，与旧 toOpenURL: 同语义）。回调投递队列官方未承诺 →
            // 信号量桥接 + 10s 超时兜底（最坏 bounded 失败不挂死，同步 Result
            // 契约不变）
            dispatch_semaphore_t const done = dispatch_semaphore_create(0);
            __block NSError* nsErr = nil;
            [[NSWorkspace sharedWorkspace]
                    setDefaultApplicationAtURL:appNs
                  toOpenContentTypeOfFileAtURL:docNs
                             completionHandler:^(NSError* err) {
                                 nsErr = err;
                                 dispatch_semaphore_signal(done);
                             }];
            const long waitSt = dispatch_semaphore_wait(
                done, dispatch_time(DISPATCH_TIME_NOW, 10LL * NSEC_PER_SEC));
            CFRelease(docUrl);
            if (waitSt == 0 && nsErr == nil) {
                written = core::Result<void>::ok();
            } else if (waitSt != 0) {
                written = core::Result<void>::fail(
                    core::ErrorCode::Internal,
                    "NSWorkspace 设默认回调超时（10s，投递队列未承诺）");
            } else {
                written = core::Result<void>::fail(
                    core::ErrorCode::Internal,
                    std::string("NSWorkspace setDefaultApplicationAtURL 失败: ") +
                        (nsErr.localizedDescription.UTF8String != nullptr
                             ? nsErr.localizedDescription.UTF8String
                             : "未知错误"));
            }
        } else {
            // 不可达（api_map 12+ 才落此分支）；防御性诚实失败
            written = core::Result<void>::fail(
                core::ErrorCode::Internal,
                "版本判定与系统可用性不一致（api_map=12+，@available<12）");
        }
    } else {
        CFStringRef const utiCf = cfStringFromStdString(uti.value());
        CFStringRef const bundleIdCf = cfStringFromStdString(self.value().first);
        const OSStatus st = LSSetDefaultRoleHandlerForContentType(
            utiCf, kLSRolesAll, bundleIdCf);
        if (utiCf != nullptr) CFRelease(utiCf);
        if (bundleIdCf != nullptr) CFRelease(bundleIdCf);
        if (st == noErr) {
            written = core::Result<void>::ok();
        } else {
            written = core::Result<void>::fail(
                core::ErrorCode::Internal,
                "LSSetDefaultRoleHandlerForContentType 失败 (OSStatus=" +
                    std::to_string(static_cast<long>(st)) + ")");
        }
    }
    CFRelease(self.value().second);
    if (written.isFail()) {
        return core::Result<bool>::fail(written.errorCode(), written.errorMessage());
    }

    // 写后必刷（win SHChangeNotify 同位口径，spy 缝在此断言）
    launchServicesRefreshed();
    // 写后回查：bool = 归本应用。回查落 Unknown/Others → ok(false)——写入成功
    // ≠ 行为生效（clue10/F-07 判定规则），呈现与自愈以本查询回读为准
    const core::Result<AssocState> state = isDefaultForMd(config);
    return core::Result<bool>::ok(state.isOk() &&
                                  state.value() == AssocState::Ours);
}

ShellNotifySink* setShellNotifySink(ShellNotifySink* sink) {
    ShellNotifySink* const old = g_notifySink;
    g_notifySink = sink;
    return old;
}

core::Result<void> removeRegistrationForTest(const AssocConfig&) {
    return core::Result<void>::fail(
        core::ErrorCode::NotImplemented,
        "macOS 无注销默认 handler 的公开 API（LS handler 只能改设不能删；"
        "PRD D-4 生产不注销，mac 注入缝链路测试如立项须先调研清除路径）");
}

} // namespace platform::file_association
