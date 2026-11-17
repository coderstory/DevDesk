// tests/test_modules/test_mcp_zcode_config.cpp
// 「安装到 ZCode」（need279）配置读写用例：~/.zcode/cli/config.json 顶层
// mcp.servers 的键级安装/卸载（镜像 test_mcp_claude_config 模板）。
// 防的事故：整文件重写丢掉 ZCode 自身状态（plugins/hooks/skills/未知键）、
// 重复点安装写出重复条目、同名异址条目被覆盖、卸载误删别的 server、
// 缺文件被自动创建。
#include "doctest.h"

#include "zcode_mcp_config.h"

#include "core/fs_text.h"
#include "core/paths.h"  // 真文件定位走生产契约（Win 用 USERPROFILE）
#include "platform/json.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

namespace zc = module::mcp::zc;
namespace pj = platform::json;

// 贴近实测形态的最小样本（线索 179：顶层 plugins/hooks/skills/mcp）：
// mcp.servers 两个既有 server + 3 个未知顶层键 + 自身状态字段
const char* kSampleJson = R"({
  "plugins": {"installed": {"demo": "1.0"}},
  "hooks": {"SessionStart": []},
  "skills": ["a", "b"],
  "unknownA": 7,
  "unknownB": "keep-me",
  "unknownC": {"nested": [1, 2, 3]},
  "mcp": {
    "servers": {
      "codegraph": {"args": ["serve"], "command": "codegraph", "type": "stdio"},
      "mt": {"enabled": true, "type": "http", "url": "http://127.0.0.1:9200/mcp"}
    }
  }
})";

// 解析后按值取键（解析失败返回空对象——调用方断言处用 keys().empty() 判红）
pj::JsonObject parseObj(const std::string& text) {
    const auto parsed = pj::parse(text);
    if (parsed.isFail() || !parsed.value().isObject()) return pj::JsonObject();
    return parsed.value().asObject();
}

std::string serversJson(const std::string& text) {
    const auto root = parseObj(text);
    const auto servers = root.get("mcp").asObject().get("servers");
    const auto out = pj::stringify(servers);
    return out.isOk() ? out.value() : std::string();
}

std::filesystem::path makeTempConfig() {
    const auto dir = test_temp::tempPath("zc_cfg");
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir / "cli" / "config.json";
}

bool writeSample(const std::filesystem::path& file) {
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    std::ofstream f(file, std::ios::binary | std::ios::trunc);
    if (!static_cast<bool>(f)) return false;  // 断言宏只能在返回 bool 的函数体用
    f << kSampleJson;
    return true;
}

} // namespace

// F-01：路径解析形状（家目录/.zcode/cli/config.json；Win=USERPROFILE 本机即证）
REGISTER_TEST(zc_path_shape,
              "zc: zcodeConfigFile() 解析为 <home>/.zcode/cli/config.json 形状", {
    const auto p = core::paths::zcodeConfigFile();
    DOCTEST_CHECK_EQ(p.filename().string(), std::string("config.json"));
    DOCTEST_CHECK_EQ(p.parent_path().filename().string(), std::string("cli"));
    DOCTEST_CHECK_EQ(p.parent_path().parent_path().filename().string(),
                     std::string(".zcode"));
    return true;
})

// A1/F-03：安装写入三字段条目 + 其余 server/顶层键原样
REGISTER_TEST(zc_install_writes_http_entry,
              "zc: 安装只加 mcp.servers.devdesk（type/url/enabled），其余完整保留", {
    bool changed = false;
    const auto out = zc::applyServerEntry(kSampleJson, "http://127.0.0.1:8888/mcp",
                                          &changed);
    DOCTEST_REQUIRE(out.isOk());
    DOCTEST_CHECK(changed);

    const auto root = parseObj(out.value());
    // 顶层自身状态字段与未知键全在（整文件重写的丢字段事故）
    DOCTEST_CHECK(root.get("unknownA").asInt() == 7);
    DOCTEST_CHECK_EQ(root.get("unknownB").asString(), std::string("keep-me"));
    DOCTEST_CHECK(root.get("plugins").isObject());
    DOCTEST_CHECK(root.get("hooks").isObject());
    DOCTEST_CHECK(root.get("skills").isArray());

    const auto servers = root.get("mcp").asObject().get("servers").asObject();
    DOCTEST_CHECK(servers.keys().size() == 3);
    DOCTEST_CHECK(servers.has("codegraph"));
    DOCTEST_CHECK(servers.has("mt"));
    DOCTEST_REQUIRE(servers.has("devdesk"));
    // 条目形态（PRD §8 裁决）：type + url + 显式 enabled=true
    const auto entry = servers.get("devdesk").asObject();
    DOCTEST_CHECK_EQ(entry.get("type").asString(), std::string("http"));
    DOCTEST_CHECK_EQ(entry.get("url").asString(),
                     std::string("http://127.0.0.1:8888/mcp"));
    DOCTEST_CHECK(entry.get("enabled").asBool());
    DOCTEST_CHECK(entry.keys().size() == 3);
    // 既有 server 内容不被改写
    DOCTEST_CHECK_EQ(servers.get("codegraph").asObject().get("command").asString(),
                     std::string("codegraph"));
    DOCTEST_CHECK_EQ(servers.get("mt").asObject().get("url").asString(),
                     std::string("http://127.0.0.1:9200/mcp"));
    return true;
})

// F-02：mcp / mcp.servers 层级缺失 → 补建（文件已存在由文件级编排把守）
REGISTER_TEST(zc_install_creates_missing_hierarchy,
              "zc: mcp 或 mcp.servers 缺失 → 安装补建层级，其余键保留", {
    bool changed = false;
    const auto out = zc::applyServerEntry(R"({"plugins": {}})",
                                          "http://127.0.0.1:8888/mcp", &changed);
    DOCTEST_REQUIRE(out.isOk());
    DOCTEST_CHECK(changed);
    const auto root = parseObj(out.value());
    DOCTEST_CHECK(root.get("plugins").isObject());
    const auto mcp = root.get("mcp").asObject();
    DOCTEST_CHECK(mcp.get("servers").asObject().has("devdesk"));

    bool changed2 = false;
    const auto out2 = zc::applyServerEntry(R"({"mcp": {"other": 1}})",
                                           "http://127.0.0.1:8888/mcp", &changed2);
    DOCTEST_REQUIRE(out2.isOk());
    const auto mcp2 = parseObj(out2.value()).get("mcp").asObject();
    DOCTEST_CHECK(mcp2.get("other").asInt() == 1);
    DOCTEST_CHECK(mcp2.get("servers").asObject().has("devdesk"));
    return true;
})

// A3：同址重复安装 = 幂等 no-op
REGISTER_TEST(zc_install_idempotent_same_url,
              "zc: 已装同址 devdesk 时重复安装 → changed=false 且文本一字节不变", {
    bool first = false;
    const auto once = zc::applyServerEntry(kSampleJson, "http://127.0.0.1:8888/mcp",
                                           &first);
    DOCTEST_REQUIRE(once.isOk());
    DOCTEST_REQUIRE(first);

    bool second = false;
    const auto twice = zc::applyServerEntry(once.value(),
                                            "http://127.0.0.1:8888/mcp", &second);
    DOCTEST_REQUIRE(twice.isOk());
    DOCTEST_CHECK_FALSE(second);
    DOCTEST_CHECK_EQ(twice.value(), once.value());
    const auto servers =
        parseObj(twice.value()).get("mcp").asObject().get("servers").asObject();
    DOCTEST_CHECK(servers.keys().size() == 3);
    return true;
})

// A4/D2：同名异址（或缺 url）= 冲突 fail 不覆盖，中文引导
REGISTER_TEST(zc_install_conflict_different_url,
              "zc: 同名异址条目 → 安装 fail（不覆盖）中文引导；缺 url 同判冲突", {
    // 预置异址 devdesk 条目（样本里原没有 devdesk 键）
    const std::string presetConflict = R"({
      "plugins": {"installed": {"demo": "1.0"}},
      "mcp": {"servers": {
        "codegraph": {"args": ["serve"], "command": "codegraph", "type": "stdio"},
        "devdesk": {"enabled": true, "type": "http", "url": "http://127.0.0.1:7777/mcp"}
      }}
    })";
    bool changed = true;
    const auto conflict = zc::applyServerEntry(presetConflict,
                                               "http://127.0.0.1:8888/mcp",
                                               &changed);
    DOCTEST_REQUIRE(conflict.isFail());
    DOCTEST_CHECK_FALSE(changed);
    // 中文引导：原因（地址不同）+ 动作（先移除再安装）；原条目未被改写
    DOCTEST_CHECK(conflict.errorMessage().find("地址不同") != std::string::npos);
    DOCTEST_CHECK(conflict.errorMessage().find("移除") != std::string::npos);
    const auto preset = parseObj(presetConflict);  // fail 不产生新文本，原样可再解析
    DOCTEST_CHECK_EQ(preset.get("mcp").asObject().get("servers")
                         .asObject().get("devdesk").asObject().get("url").asString(),
                     std::string("http://127.0.0.1:7777/mcp"));

    bool changed2 = true;
    const auto noUrl = zc::applyServerEntry(
        R"({"mcp": {"servers": {"devdesk": {"type": "http"}}}})",
        "http://127.0.0.1:8888/mcp", &changed2);
    DOCTEST_CHECK(noUrl.isFail());
    DOCTEST_CHECK_FALSE(changed2);
    return true;
})

// A2/F-04：卸载只摘 devdesk，其余 server 与顶层键完整（往返守恒）
REGISTER_TEST(zc_remove_only_target_key,
              "zc: 卸载只删 mcp.servers.devdesk，其余条目与顶层键保留", {
    bool installed = false;
    const auto withDevdesk = zc::applyServerEntry(
        kSampleJson, "http://127.0.0.1:8888/mcp", &installed);
    DOCTEST_REQUIRE(withDevdesk.isOk());

    bool removed = false;
    const auto out = zc::removeServerEntry(withDevdesk.value(), &removed);
    DOCTEST_REQUIRE(out.isOk());
    DOCTEST_CHECK(removed);

    const auto root = parseObj(out.value());
    DOCTEST_CHECK(root.get("unknownA").asInt() == 7);
    DOCTEST_CHECK(root.get("plugins").isObject());
    DOCTEST_CHECK(root.get("skills").isArray());
    const auto servers = root.get("mcp").asObject().get("servers").asObject();
    DOCTEST_CHECK_FALSE(servers.has("devdesk"));
    DOCTEST_CHECK(servers.has("codegraph"));
    DOCTEST_CHECK(servers.has("mt"));
    DOCTEST_CHECK(servers.keys().size() == 2);
    // 往返守恒（键序由封装按 std::map 排序，比对结构化等价）
    DOCTEST_CHECK_EQ(serversJson(out.value()), serversJson(kSampleJson));
    return true;
})

// F-04：卸载幂等 + 不凭空造键
REGISTER_TEST(zc_remove_noop_and_no_container,
              "zc: 本就没装 → 卸载 changed=false；mcp/servers 缺失或 null 不凭空创建", {
    bool removed = false;
    const auto out = zc::removeServerEntry(kSampleJson, &removed);
    DOCTEST_REQUIRE(out.isOk());
    DOCTEST_CHECK_FALSE(removed);
    DOCTEST_CHECK_EQ(out.value(), std::string(kSampleJson));

    bool r2 = false;
    const auto noMcp = zc::removeServerEntry(R"({"plugins": 1})", &r2);
    DOCTEST_REQUIRE(noMcp.isOk());
    DOCTEST_CHECK_FALSE(r2);
    DOCTEST_CHECK_FALSE(parseObj(noMcp.value()).has("mcp"));

    bool r3 = false;
    const auto nullServers = zc::removeServerEntry(R"({"mcp": {"servers": null}})", &r3);
    DOCTEST_REQUIRE(nullServers.isOk());
    DOCTEST_CHECK_FALSE(r3);
    return true;
})

// A5/F-02：非法 JSON / 顶层非对象 / mcp 非对象 / servers 非对象 → 中文 fail
REGISTER_TEST(zc_invalid_shapes_fail,
              "zc: 非法 JSON/顶层非对象/mcp/servers 非对象 → 安装与卸载都 fail", {
    bool changed = true;
    const auto bad = zc::applyServerEntry("{not json", "http://x/mcp", &changed);
    DOCTEST_CHECK(bad.isFail());
    DOCTEST_CHECK_FALSE(changed);

    bool changed2 = true;
    const auto arr = zc::applyServerEntry("[1,2]", "http://x/mcp", &changed2);
    DOCTEST_CHECK(arr.isFail());
    DOCTEST_CHECK_FALSE(changed2);

    bool changed3 = true;
    const auto mcpArr =
        zc::applyServerEntry(R"({"mcp": [1]})", "http://x/mcp", &changed3);
    DOCTEST_CHECK(mcpArr.isFail());
    DOCTEST_CHECK(mcpArr.errorMessage().find("拒绝写入") != std::string::npos);

    bool changed4 = true;
    const auto serversStr = zc::applyServerEntry(R"({"mcp": {"servers": "oops"}})",
                                                 "http://x/mcp", &changed4);
    DOCTEST_CHECK(serversStr.isFail());
    DOCTEST_CHECK_FALSE(changed4);

    bool changed5 = true;
    const auto badRemove = zc::removeServerEntry(R"({"mcp": {"servers": [1]}})", &changed5);
    DOCTEST_CHECK(badRemove.isFail());
    DOCTEST_CHECK(badRemove.errorMessage().find("拒绝写入") != std::string::npos);
    return true;
})

// A5：文件缺失 → fail 且绝不创建（禁自动建 ~/.zcode/cli/config.json）
REGISTER_TEST(zc_missing_file_not_created,
              "zc: 配置文件缺失时安装/卸载 fail 且文件与备份仍不存在", {
    const auto file = makeTempConfig();
    std::error_code ec;
    std::filesystem::remove_all(file.parent_path(), ec);
    DOCTEST_CHECK_FALSE(std::filesystem::exists(file));
    const auto r = zc::installServerFile(file, "http://127.0.0.1:8888/mcp");
    DOCTEST_CHECK(r.isFail());
    DOCTEST_CHECK_FALSE(std::filesystem::exists(file));
    const auto r2 = zc::removeServerFile(file);
    DOCTEST_CHECK(r2.isFail());
    DOCTEST_CHECK_FALSE(std::filesystem::exists(zc::backupFile(file)));
    return true;
})

// A5/写失败面：目录冒充 config.json → 读失败分支 fail 且目录原样
REGISTER_TEST(zc_dir_as_file_fails_clean,
              "zc: config.json 是目录 → 安装/卸载 fail（读失败分支）且目录原样保留", {
    const auto dir = test_temp::tempPath("zc_cfg_dirfile");
    std::error_code ec;
    std::filesystem::create_directories(dir / "config.json", ec);
    DOCTEST_REQUIRE(std::filesystem::is_directory(dir / "config.json"));

    const auto install =
        zc::installServerFile(dir / "config.json", "http://127.0.0.1:8888/mcp");
    DOCTEST_CHECK(install.isFail());
    DOCTEST_CHECK_FALSE(install.errorMessage().empty());
    DOCTEST_CHECK(std::filesystem::is_directory(dir / "config.json"));

    const auto remove = zc::removeServerFile(dir / "config.json");
    DOCTEST_CHECK(remove.isFail());

    std::filesystem::remove_all(dir, ec);
    return true;
})

// A7：文件级安装→状态已装（含 installedUrl）→备份==原内容→卸载恢复原状
REGISTER_TEST(zc_file_roundtrip_with_backup,
              "zc: 文件安装→已装+URL 记录→卸载→内容恢复原状，留 .bak-devdesk 备份", {
    const auto file = makeTempConfig();
    DOCTEST_REQUIRE(writeSample(file));

    const zc::ConfigState before = zc::readConfigState(file);
    DOCTEST_CHECK(before.filePresent);
    DOCTEST_CHECK_FALSE(before.installed);
    DOCTEST_CHECK(before.error.empty());

    const auto install = zc::installServerFile(file, "http://127.0.0.1:8888/mcp");
    DOCTEST_REQUIRE(install.isOk());
    const zc::ConfigState afterInstall = zc::readConfigState(file);
    DOCTEST_CHECK(afterInstall.installed);
    DOCTEST_CHECK_EQ(afterInstall.installedUrl, std::string("http://127.0.0.1:8888/mcp"));
    // 备份 = 安装前的原内容
    DOCTEST_CHECK_EQ(core::readTextFile(zc::backupFile(file)), std::string(kSampleJson));

    const auto remove = zc::removeServerFile(file);
    DOCTEST_REQUIRE(remove.isOk());
    const zc::ConfigState afterRemove = zc::readConfigState(file);
    DOCTEST_CHECK_FALSE(afterRemove.installed);
    // 语义恢复原状（比对结构化等价）
    DOCTEST_CHECK_EQ(serversJson(core::readTextFile(file)), serversJson(kSampleJson));
    DOCTEST_CHECK(parseObj(core::readTextFile(file)).get("unknownA").asInt() == 7);

    std::error_code ec;
    std::filesystem::remove_all(file.parent_path(), ec);
    return true;
})

// A3：文件级重复安装——同址 no-op 零重写零新备份；异址冲突 fail 零写入
REGISTER_TEST(zc_file_install_twice_no_rewrite,
              "zc: 文件级重复安装同址不改写；异址冲突 fail 文件与备份均不变", {
    const auto file = makeTempConfig();
    DOCTEST_REQUIRE(writeSample(file));
    DOCTEST_REQUIRE(zc::installServerFile(file, "http://127.0.0.1:8888/mcp").isOk());
    const std::string afterFirst = core::readTextFile(file);

    // 同址：no-op，一字节不动，备份仍是第一次的原内容
    DOCTEST_REQUIRE(zc::installServerFile(file, "http://127.0.0.1:8888/mcp").isOk());
    DOCTEST_CHECK_EQ(core::readTextFile(file), afterFirst);
    DOCTEST_CHECK_EQ(core::readTextFile(zc::backupFile(file)), std::string(kSampleJson));

    // 异址：冲突 fail，文件与备份均不变
    const auto conflict = zc::installServerFile(file, "http://127.0.0.1:9999/mcp");
    DOCTEST_CHECK(conflict.isFail());
    DOCTEST_CHECK_EQ(core::readTextFile(file), afterFirst);
    DOCTEST_CHECK_EQ(core::readTextFile(zc::backupFile(file)), std::string(kSampleJson));

    std::error_code ec;
    std::filesystem::remove_all(file.parent_path(), ec);
    return true;
})

// A6：真实 ~/.zcode/cli/config.json 副本（只读，不写真文件）安装→卸载后
// 顶层键集合与各值语义全等，mcp.servers 仅 devdesk 一键差异
REGISTER_TEST(zc_real_file_preserves_all_keys,
              "zc: 真实 ~/.zcode/cli/config.json 副本安装→卸载后顶层键与值全等", {
    const std::filesystem::path real = core::paths::zcodeConfigFile();
    std::error_code ec;
    if (!std::filesystem::exists(real, ec)) {
        DOCTEST_SKIP("本机无 ~/.zcode/cli/config.json");
    }
    const std::string original = core::readTextFile(real);
    if (original.size() <= 100) {
        DOCTEST_SKIP("真配置过小，样本不具代表性");
    }
    const auto before = parseObj(original);
    if (before.keys().size() < 3) {
        DOCTEST_SKIP("真配置顶层键过少，样本不具代表性");
    }

    bool changed = false;
    const auto installed =
        zc::applyServerEntry(original, "http://127.0.0.1:8888/mcp", &changed);
    DOCTEST_REQUIRE(installed.isOk());
    DOCTEST_CHECK(changed);
    DOCTEST_CHECK(installed.value().size() > original.size() * 9 / 10);  // 没被截断

    bool removed = false;
    const auto restored = zc::removeServerEntry(installed.value(), &removed);
    DOCTEST_REQUIRE(restored.isOk());
    DOCTEST_CHECK(removed);

    const auto after = parseObj(restored.value());
    DOCTEST_CHECK_EQ(after.keys().size(), before.keys().size());
    for (const std::string& key : before.keys()) {
        DOCTEST_CHECK_MESSAGE(after.has(key), "卸载后丢顶层键: " + key);
        if (!after.has(key)) continue;
        // 值语义等价（键序由封装排序，不比对字节）
        const auto a = pj::stringify(before.get(key));
        const auto b = pj::stringify(after.get(key));
        DOCTEST_CHECK_MESSAGE(a.isOk() && b.isOk() && a.value() == b.value(),
                              "顶层键值被改写: " + key);
    }
    return true;
})
