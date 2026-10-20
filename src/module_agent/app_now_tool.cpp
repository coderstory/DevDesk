// AppNowTool 实现：core::time::nowUnix + localTime（Windows=localtime_s / POSIX=localtime_r，
// 全仓跨平台线程安全口径，execute 在 AgentLoop 工作线程单线程执行、工具自身无共享可变状态）。
//
// ISO 偏移量取法：不依赖平台时区数据库（MinGW 下 chrono::current_zone 不可靠），
// 用「同一 time_t 的本地墙钟与 UTC 墙钟各自折算成自 1970-01-01 的秒数再相减」纯算术求出。
#include "app_now_tool.h"

#include "core/time.h"
#include "platform/json.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <string>
#include <utility>

namespace module::agent {

namespace {

using core::Result;
using core::ToolArgs;
using core::ToolResult;
namespace pj = platform::json;

// tm 折算成「把该墙钟读数当成 UTC」的秒数：日期部分用 core::time 自带的
// daysFromCivil 纯算术（闰年/跨年安全；直接用 tm_yday 有 1900 基年坑）
int64_t tmAsUtcSeconds(const std::tm& t) {
    const int64_t days = core::time::daysFromCivil(
        t.tm_year + 1900, static_cast<unsigned>(t.tm_mon) + 1,
        static_cast<unsigned>(t.tm_mday));
    return days * 86400 + t.tm_hour * 3600 + t.tm_min * 60 + t.tm_sec;
}

const char* weekdayCn(int wday) {
    static const char* kNames[7] = {"星期日", "星期一", "星期二", "星期三",
                                    "星期四", "星期五", "星期六"};
    return (wday >= 0 && wday <= 6) ? kNames[wday] : "";
}

class AppNowTool : public core::ITool {
public:
    std::string name() const override { return "app.now"; }

    std::string schemaJson() const override {
        return R"({"name":"app.now","description":"获取当前本地时间：ISO 8601 时间戳、日期、时刻、中文星期、epoch 毫秒。无参数","input_schema":{"type":"object","properties":{}}})";
    }

    Result<ToolResult> execute(const ToolArgs& args) override {
        (void)args;  // 无参数

        // 秒与毫秒同取 system_clock 源：nowUnix 即 time(nullptr)，毫秒同刻补足
        const std::time_t nowSec = core::time::nowUnix();
        const int64_t epochMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count();

        std::tm local{};
        core::time::localTime(nowSec, &local);
        std::tm utc{};
        core::time::utcTime(nowSec, &utc);

        // 缓冲区留足余量防 snprintf 截断告警（-Werror 下 format-truncation 是真错误）
        char date[16];
        std::snprintf(date, sizeof(date), "%04d-%02d-%02d",
                      local.tm_year + 1900, local.tm_mon + 1, local.tm_mday);
        char timeOfDay[16];
        std::snprintf(timeOfDay, sizeof(timeOfDay), "%02d:%02d:%02d",
                      local.tm_hour, local.tm_min, local.tm_sec);

        // 时区偏移（秒）＝ 本地墙钟折算秒 − UTC 墙钟折算秒；符号位单独拼
        const int64_t offsetSec = tmAsUtcSeconds(local) - tmAsUtcSeconds(utc);
        const int64_t absOffset = offsetSec < 0 ? -offsetSec : offsetSec;
        char offset[16];
        std::snprintf(offset, sizeof(offset), "%c%02d:%02d",
                      offsetSec < 0 ? '-' : '+',
                      static_cast<int>(absOffset / 3600),
                      static_cast<int>(absOffset % 3600 / 60));

        char iso[48];
        std::snprintf(iso, sizeof(iso), "%sT%s%s", date, timeOfDay, offset);

        pj::JsonObject o;
        o.set("iso", pj::makeString(iso));
        o.set("date", pj::makeString(date));
        o.set("time", pj::makeString(timeOfDay));
        o.set("weekday", pj::makeString(weekdayCn(local.tm_wday)));
        o.set("epoch_ms", pj::makeInt(epochMs));
        auto text = pj::stringify(pj::JsonValue(std::move(o)));
        if (text.isFail()) {
            return Result<ToolResult>::fail(
                core::ErrorCode::Internal,
                "时间结果序列化失败: " + text.errorMessage());
        }
        return Result<ToolResult>::ok(ToolResult{false, text.value()});
    }
};

} // namespace

std::shared_ptr<core::ITool> makeAppNowTool() {
    return std::make_shared<AppNowTool>();
}

} // namespace module::agent
