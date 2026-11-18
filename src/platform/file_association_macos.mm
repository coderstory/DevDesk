// src/platform/file_association_macos.mm
// macOS 记事本多扩展文件关联（F-18 多扩展化，need263 批 4 棒 B；原 F-09 单
// .md 版）。落点名偏差说明：卡面写 file_association_macos.cpp，实为 .mm——
// 12+ 写入路径 NSWorkspace 设默认无 C 等价 API（现行
// toOpenContentTypeOfFileAtURL: completionHandler:；旧 toOpenURL: error 形态
// 已随 27.2 SDK 整体移除），本 TU 必须 ObjC++（repo 先例
// notification_macos.mm）。
// 架构红线：版本→API 路径唯一权威 = file_association_api_map.h（纯逻辑单测
// 钉死），@available 仅编译期可用性守卫，运行期判定收敛本单文件。
// 多扩展写入策略（12+ 批量）：26 项逐个 await 确认框最坏 26×1.5s 挂死 UI →
// 全部先 fire + dispatch_group 聚合，单次 1.5s 总预算等待，随后统一回查判定
//（超时未回调 = 确认框待用户操作，交回查如实呈现）。
// mac 侧扩展豁免（.ts）：系统 UTI = public.mpeg-2-transport-stream（视频，
// 2026-11-18 本机 UTType 实测）——登记会抢视频文件关联，mac 侧写入/聚合查询
// 一律跳过；Windows 注册表 ProgId 路径无 UTI 冲突照常全量。
//   shortcut: 跳过清单仅 .ts 一项, 升级触发=扩展清单新增与系统 UTI 冲突条目时
// clue10 口径（同 win）：写入成功 ≠ 双击行为生效——setAsDefaultFor 返回值 =
// 写后回查判属；macOS 11–15 逐版本定性（扩展 UTI/NSWorkspace 确认框）= UAT 项。
// 注入缝口径（与 win 同款 AssocConfig，mac 侧语义差异）：
//   · config.progId = 查询判属锚（回读 bundle id 比对串）；生产缺省（win 字面量
//     "DevDesk.notepad"）在 mac 无意义 → 解析为运行期本应用 bundle identifier；
//     显式传入非缺省 progId（mac 测试用隔离 bundle id）则按字面量比对。
//   · 写入目标恒为本应用 bundle id + 自身 app URL（mac 设默认的合法对象是真实
//     app，异于 win 自建 ProgId 注册键可任写）。
// mac 无注销 API（LS 默认 handler 只能改设不能删）→ unregisterAll 诚实
// NotImplemented（F-18「关闭=撤销」在 mac 侧不可达，设置页关=停自愈如实呈现）。
#include "file_association.h"

#include "file_association_api_map.h" // 版本→写入 API 路径唯一权威

#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>
// 27.2 SDK 实测（CI run 43889570987）：伞头不引全 UTType.h——类与常量可见、
// +typeWithTag: 方法声明不可见（no known class method 同证）→ 显式引类头/常量头
#import <UniformTypeIdentifiers/UTType.h>
#import <UniformTypeIdentifiers/UTTagClass.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h> // UTType 类反查（11.0+；C API 12.0 起弃用）

#include <CoreServices/CoreServices.h> // UTTypeCreatePreferredIdentifierForTag（<11 保守路径）/ LSSet·LSCopyDefaultRoleHandlerForContentType
#include <dispatch/dispatch.h> // NSWorkspace completionHandler → 同步 Result 桥接（group+超时）
#include <sys/sysctl.h>

#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

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
std::string extensionWithoutDot(const std::string& extWithDot) {
    std::string ext = extWithDot;
    if (!ext.empty() && ext.front() == '.') ext.erase(ext.begin());
    return ext;
}

// mac 侧豁免扩展（见文件头）：.ts 撞 public.mpeg-2-transport-stream，写入与
// 聚合查询都不碰；单扩展查询不豁免（直接问就如实答）
bool macSupportedExtension(const std::string& extWithDot) {
    return extWithDot != ".ts";
}

// 扩展名 → LS 侧实际生效 UTI：运行期按 tag 反查，不硬编码 UTI 串（声明了
// UTImportedTypeDeclarations 的机器得声明 UTI，未声明机器得 dyn.*——写/查
// 同法反查，两侧对称）。12+ 走 UTType 类（旧 C API 自 12.0 起标记弃用，
// -Werror 下会红）；11 保留 C API（应用下限 11.0，该类尚不可用），弃用告警
// 按分支局部豁免。
core::Result<std::string> contentTypeForExtension(const std::string& extNoDot) {
    NSString* const extNs = [NSString stringWithUTF8String:extNoDot.c_str()];
    if (extNs == nil) {
        return core::Result<std::string>::fail(core::ErrorCode::Internal,
                                               "扩展名转 NSString 失败");
    }
    if (@available(macOS 11.0, *)) {
        UTType* const type = [UTType typeWithFilenameExtension:extNs];
        if (type == nil) {
            return core::Result<std::string>::fail(
                core::ErrorCode::Internal, "扩展名未解析出 UTI (" + extNoDot + ")");
        }
        const std::string out = cfToStdString((__bridge CFStringRef)type.identifier);
        if (out.empty()) {
            return core::Result<std::string>::fail(
                core::ErrorCode::Internal, "UTI 转 string 失败 (" + extNoDot + ")");
        }
        return core::Result<std::string>::ok(out);
    }
    // <11 路径（UTType 类不可用）：旧 C API 是唯一选择。UTTypeCreatePreferredIdentifierForTag
    // 12.0 起标记弃用，但弃用≠移除——27.2 SDK 仍声明、符号仍可链（deprecated API
    // 的兼容承诺）。CI 实证：clang 的 -Wdeprecated-declarations 不随 @available
    // 分支自动消，须 pragma 显式包裹（push/pop 成对）。11+ 已走 UTType 类
    //（typeWithFilenameExtension:，本文件 Mac 侧真编译收口）；此分支仅 <11 运行期可达。
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    CFStringRef const extCf = cfStringFromStdString(extNoDot);
    if (extCf == nullptr) {
        return core::Result<std::string>::fail(core::ErrorCode::Internal,
                                               "扩展名转 CFString 失败");
    }
    std::string out;
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

// 查询单扩展当前生效默认：LSCopyDefaultRoleHandlerForContentType 按内容类型查，
// 与写入侧同源（12+ NSWorkspace toOpenContentTypeOfFileAtURL / 11 LSSet 均
// 按 contentType 写）。need331（2026-11-18 darwin27 本机实测）：旧路径
// LSCopyDefaultApplicationURLForURL 对不存在的探针文件返回 nil（旧「文件无需
// 存在、按扩展名判归属」行为已消失；文件真实存在才答得出），OS 弹窗写入的
// UTI 级 handler 查不到 → 回读永假 = 设置页永显「未关联」。UTI 级查询同源
// 可靠（实测 ROLE net.daringfireball.markdown → 本应用）。
// fail = 查询失败/无 handler（调用方落 Unknown，绝不映射 Others）
core::Result<std::string> queryCurrentDefaultBundleId(const std::string& extWithDot) {
    const std::string ext = extensionWithoutDot(extWithDot);
    if (ext.empty()) {
        return core::Result<std::string>::fail(core::ErrorCode::Internal,
                                               "扩展名为空，无法查询");
    }
    const core::Result<std::string> uti = contentTypeForExtension(ext);
    if (uti.isFail()) {
        return uti;
    }
    CFStringRef const utiCf = cfStringFromStdString(uti.value());
    CFStringRef const handler =
        utiCf != nullptr
            ? LSCopyDefaultRoleHandlerForContentType(utiCf, kLSRolesAll)
            : nullptr;
    if (utiCf != nullptr) CFRelease(utiCf);
    if (handler == nullptr) {
        return core::Result<std::string>::fail(
            core::ErrorCode::Internal,
            "LaunchServices 查询默认应用失败（无生效 handler）: " + extWithDot);
    }
    const std::string id = cfToStdString(handler);
    CFRelease(handler);
    if (id.empty()) {
        return core::Result<std::string>::fail(
            core::ErrorCode::Internal,
            "默认应用 bundle identifier 为空: " + extWithDot);
    }
    return core::Result<std::string>::ok(id);
}

// 写后刷新（win SHChangeNotify must-call 的 mac 同位步骤）：mac 无关联变更广播
// API，LS handler 变更即时落 LS 偏好域，显式 synchronize 促跨进程可见；
// 实效定性（图标/Finder 即时性）= UAT 项。整批写入恰刷一次（spy 断言）。
void launchServicesRefreshed() {
    if (g_notifySink != nullptr) {
        g_notifySink();
        return;
    }
    CFPreferencesAppSynchronize(CFSTR("com.apple.LaunchServices"));
}

} // namespace

core::Result<AssocState> isDefaultFor(const std::string& extension,
                                      const AssocConfig& config) {
    const std::string anchor = resolveAnchor(config);
    if (anchor.empty()) {
        // 判属锚缺失（非 bundle 进程）= 无法判定 → Unknown（≠Others，语义见 .h）
        return core::Result<AssocState>::ok(AssocState::Unknown);
    }
    const core::Result<std::string> current = queryCurrentDefaultBundleId(extension);
    return core::Result<AssocState>::ok(
        classifyState(current.isOk(), current.valueOr(""), anchor));
}

core::Result<AssocState> overallState(const AssocConfig& config) {
    const std::string anchor = resolveAnchor(config);
    if (anchor.empty()) {
        return core::Result<AssocState>::ok(AssocState::Unknown);
    }
    std::vector<AssocState> states;
    states.reserve(config.extensions.size());
    for (const std::string& ext : config.extensions) {
        if (!macSupportedExtension(ext)) continue;  // .ts 豁免（文件头）
        const core::Result<std::string> current = queryCurrentDefaultBundleId(ext);
        states.push_back(classifyState(current.isOk(), current.valueOr(""), anchor));
    }
    return core::Result<AssocState>::ok(aggregateAssocStates(states));
}

core::Result<bool> setAsDefaultFor(const AssocConfig& config) {
    const core::Result<std::pair<std::string, CFURLRef>> self = selfIdentity();
    if (self.isFail()) {
        return core::Result<bool>::fail(self.errorCode(), self.errorMessage());
    }
    const std::string anchor = resolveAnchor(config);
    if (anchor.empty()) {
        CFRelease(self.value().second);
        return core::Result<bool>::fail(core::ErrorCode::Internal,
                                        "非 bundle 进程，无法设默认");
    }

    // 逐扩展解析 UTI（跳过 mac 豁免项；UTI 反查失败如实 fail——不伪装部分成功）
    struct ExtUti { std::string ext; std::string uti; };
    std::vector<ExtUti> targets;
    for (const std::string& ext : config.extensions) {
        if (!macSupportedExtension(ext)) continue;
        const core::Result<std::string> uti =
            contentTypeForExtension(extensionWithoutDot(ext));
        if (uti.isFail()) {
            CFRelease(self.value().second);
            return core::Result<bool>::fail(uti.errorCode(), uti.errorMessage());
        }
        targets.push_back({ext, uti.value()});
    }
    if (targets.empty()) {
        CFRelease(self.value().second);
        return core::Result<bool>::fail(core::ErrorCode::Internal,
                                        "mac 侧无可登记扩展（全部豁免）");
    }

    // 写入（双路径，分支唯一权威 = api_map；@available 仅编译期守卫）
    const mac_api::WriteApi api = mac_api::writeApiFor(macOSMajorVersion());
    core::Result<void> written = core::Result<void>::fail(core::ErrorCode::Internal,
                                                          "写入路径未执行");
    if (api == mac_api::WriteApi::NsWorkspaceSetDefault) {
        if (@available(macOS 12.0, *)) {
            NSURL* const appNs = (__bridge NSURL*)self.value().second;
            // 12+ 唯一公开 API 是 async 形态（27.2 SDK 实测：旧 toOpenURL:
            // error 出参/completionHandler 两形态已整体移除——docs 404 + 编译器
            // no visible @interface 同证；现行 = toOpenContentTypeOfFileAtURL:，
            // 按探针文件 URL 的 content type 设默认，与旧 toOpenURL: 同语义）。
            // 多扩展批量：全部先 fire + group 聚合，单次总预算等待——逐项
            // await 确认框最坏 N×超时挂死 UI。需要用户确认的 UTI 完成回调可能
            // 很久才来：超时未回调 = 确认框待用户操作，交写后回查如实判定
            //（本文件既有契约：写入成功 ≠ 行为生效，一切以查询回读为准）。
            __block NSError* firstErr = nil;
            dispatch_group_t const group = dispatch_group_create();
            for (const ExtUti& t : targets) {
                CFURLRef const docUrl = probeFileUrl(extensionWithoutDot(t.ext));
                NSURL* const docNs = (__bridge NSURL*)docUrl;
                dispatch_group_enter(group);
                [[NSWorkspace sharedWorkspace]
                    setDefaultApplicationAtURL:appNs
                      toOpenContentTypeOfFileAtURL:docNs
                            completionHandler:^(NSError* error) {
                                if (error != nil && firstErr == nil) firstErr = error;
                                dispatch_group_leave(group);
                            }];
                CFRelease(docUrl);
            }
            const long waited =
                dispatch_group_wait(group,
                                    dispatch_time(DISPATCH_TIME_NOW,
                                                  1500 * NSEC_PER_MSEC));
            if (firstErr != nil) {
                written = core::Result<void>::fail(
                    core::ErrorCode::Internal,
                    std::string("NSWorkspace setDefaultApplicationAtURL 失败: ") +
                        (firstErr.localizedDescription.UTF8String != nullptr
                             ? firstErr.localizedDescription.UTF8String
                             : "未知错误"));
            } else if (waited != 0) {
                // 超时（确认框待用户操作）：已受理，交回查判定
                written = core::Result<void>::ok();
            } else {
                written = core::Result<void>::ok();
            }
        } else {
            // 不可达（api_map 12+ 才落此分支）；防御性诚实失败
            written = core::Result<void>::fail(
                core::ErrorCode::Internal,
                "版本判定与系统可用性不一致（api_map=12+，@available<12）");
        }
    } else {
        // 11 路径：LSSet 为同步逐项（无确认框，不挂 UI），任一失败如实报
        bool anyFailed = false;
        for (const ExtUti& t : targets) {
            CFStringRef const utiCf = cfStringFromStdString(t.uti);
            CFStringRef const bundleIdCf = cfStringFromStdString(self.value().first);
            const OSStatus st = LSSetDefaultRoleHandlerForContentType(
                utiCf, kLSRolesAll, bundleIdCf);
            if (utiCf != nullptr) CFRelease(utiCf);
            if (bundleIdCf != nullptr) CFRelease(bundleIdCf);
            if (st != noErr) {
                written = core::Result<void>::fail(
                    core::ErrorCode::Internal,
                    "LSSetDefaultRoleHandlerForContentType 失败 (" + t.ext +
                        " OSStatus=" + std::to_string(static_cast<long>(st)) + ")");
                anyFailed = true;
                break;
            }
        }
        if (!anyFailed) written = core::Result<void>::ok();
    }
    CFRelease(self.value().second);
    if (written.isFail()) {
        return core::Result<bool>::fail(written.errorCode(), written.errorMessage());
    }

    // 写后必刷（win SHChangeNotify 同位口径，整批恰一次，spy 缝在此断言）
    launchServicesRefreshed();
    // 写后回查：bool = 聚合归本应用。回查落 Unknown/Others → ok(false)——
    // 写入成功 ≠ 行为生效（clue10/F-07 判定规则），呈现与自愈以查询回读为准
    return core::Result<bool>::ok(
        overallState(config).valueOr(AssocState::Unknown) == AssocState::Ours);
}

ShellNotifySink* setShellNotifySink(ShellNotifySink* sink) {
    ShellNotifySink* const old = g_notifySink;
    g_notifySink = sink;
    return old;
}

core::Result<void> unregisterAll(const AssocConfig&) {
    return core::Result<void>::fail(
        core::ErrorCode::NotImplemented,
        "macOS 无注销默认 handler 的公开 API（LS handler 只能改设不能删；"
        "F-18「关闭=撤销」在 mac 不可达，关闭语义=停止自愈，如实呈现）");
}

core::Result<void> launchAdvancedAssociationUI(const std::string&) {
    return core::Result<void>::fail(
        core::ErrorCode::NotImplemented,
        "macOS 无按文件类型的系统默认页程序化直达（PRD D-5：官方唯一批量入口 = "
        "Finder 显示简介→打开方式→全部更改，设置页文字指引兜底）");
}

core::Result<void> removeRegistrationForTest(const AssocConfig&) {
    return core::Result<void>::fail(
        core::ErrorCode::NotImplemented,
        "macOS 无注销默认 handler 的公开 API（LS handler 只能改设不能删；"
        "mac 注入缝链路测试如立项须先调研清除路径）");
}

} // namespace platform::file_association
