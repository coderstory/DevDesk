// tests/test_modules/test_mcp_claude_config.cpp
// 「安装到 Claude Code」（need244）配置读写用例：~/.claude.json 顶层 mcpServers
// 的键级安装/卸载。
// 防的事故：整文件重写丢掉 Claude Code 自身状态（numStartups/projects 等）、
// 重复点安装写出重复条目、卸载误删别的 server、缺文件被自动创建。
#include "doctest.h"

#include "claude_mcp_config.h"

#include "core/fs_text.h"
#include "core/paths.h"  // 审计 P1-2：真文件定位走生产契约（Win 用 USERPROFILE，不裸取 HOME）
#include "platform/json.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

namespace cc = module::mcp::cc;
namespace pj = platform::json;

// 贴近实测形态的最小样本：mcpServers 两个既有 server + 若干顶层自身状态字段
const char* kSampleJson = R"({
  "numStartups": 183,
  "machineID": "abc123",
  "projects": {"/tmp/a": {"allowedTools": ["Read"]}},
  "mcpServers": {
    "codegraph": {"type": "stdio", "command": "codegraph", "args": ["serve"]},
    "rea": {"type": "http", "url": "http://127.0.0.1:7777/mcp"}
  }
})";

// 解析后按值取键（避免大段 string find）。解析失败返回空对象——调用方断言处
// 用 keys().empty() 判红（辅助函数内不能用 REQUIRE：其返回 bool 与对象返回冲突）
pj::JsonObject parseObj(const std::string& text) {
    const auto parsed = pj::parse(text);
    if (parsed.isFail() || !parsed.value().isObject()) return pj::JsonObject();
    return parsed.value().asObject();
}

std::string serversJson(const std::string& text) {
    const auto root = parseObj(text);
    const auto out = pj::stringify(root.get("mcpServers"));
    return out.isOk() ? out.value() : std::string();
}

std::filesystem::path makeTempConfig() {
    const auto dir = test_temp::tempPath("cc_cfg");
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir / ".claude.json";
}

} // namespace

// 安装：新条目形态 + 其余 server 原样 + 其余顶层字段原样
REGISTER_TEST(mcp_cc_install_keeps_other_fields,
              "mcp_cc: 安装只加 mcpServers.devdesk，其余 server 与顶层字段完整保留", {
    bool changed = false;
    const auto out = cc::applyServerEntry(kSampleJson, "http://127.0.0.1:8888/mcp",
                                          &changed);
    DOCTEST_REQUIRE(out.isOk());
    DOCTEST_CHECK(changed);

    const auto root = parseObj(out.value());
    // 顶层自身状态字段全在（整文件重写的丢字段事故）
    DOCTEST_CHECK(root.get("numStartups").asInt() == 183);
    DOCTEST_CHECK_EQ(root.get("machineID").asString(), std::string("abc123"));
    DOCTEST_CHECK(root.get("projects").isObject());

    const auto servers = root.get("mcpServers").asObject();
    DOCTEST_CHECK(servers.keys().size() == 3);
    DOCTEST_CHECK(servers.has("codegraph"));
    DOCTEST_CHECK(servers.has("rea"));
    DOCTEST_REQUIRE(servers.has("devdesk"));
    // 条目形态 = claude mcp add --transport http 实测产物：只 type + url
    const auto entry = servers.get("devdesk").asObject();
    DOCTEST_CHECK_EQ(entry.get("type").asString(), std::string("http"));
    DOCTEST_CHECK_EQ(entry.get("url").asString(),
                     std::string("http://127.0.0.1:8888/mcp"));
    DOCTEST_CHECK(entry.keys().size() == 2);
    // 既有 server 内容不被改写
    DOCTEST_CHECK_EQ(servers.get("codegraph").asObject().get("command").asString(),
                     std::string("codegraph"));
    DOCTEST_CHECK_EQ(servers.get("rea").asObject().get("url").asString(),
                     std::string("http://127.0.0.1:7777/mcp"));
    return true;
})

// 幂等：已存在 devdesk 再安装 = 原样返回（不重复、不改写）
REGISTER_TEST(mcp_cc_install_idempotent,
              "mcp_cc: 已装 devdesk 时重复安装 → changed=false 且文本一字节不变", {
    bool first = false;
    const auto once = cc::applyServerEntry(kSampleJson, "http://127.0.0.1:8888/mcp",
                                           &first);
    DOCTEST_REQUIRE(once.isOk());
    DOCTEST_REQUIRE(first);

    bool second = false;
    const auto twice = cc::applyServerEntry(once.value(), "http://127.0.0.1:9999/mcp",
                                            &second);
    DOCTEST_REQUIRE(twice.isOk());
    DOCTEST_CHECK_FALSE(second);                       // 幂等判据命中
    DOCTEST_CHECK_EQ(twice.value(), once.value());     // 一字不动（端口不覆盖）
    const auto servers = parseObj(twice.value()).get("mcpServers").asObject();
    DOCTEST_CHECK(servers.keys().size() == 3);
    return true;
})

// 卸载：只摘 devdesk，其余 server 与其他顶层字段完整
REGISTER_TEST(mcp_cc_remove_only_target_key,
              "mcp_cc: 卸载只删 mcpServers.devdesk，其余条目与顶层字段保留", {
    bool installed = false;
    const auto withDevdesk = cc::applyServerEntry(
        kSampleJson, "http://127.0.0.1:8888/mcp", &installed);
    DOCTEST_REQUIRE(withDevdesk.isOk());

    bool removed = false;
    const auto out = cc::removeServerEntry(withDevdesk.value(), &removed);
    DOCTEST_REQUIRE(out.isOk());
    DOCTEST_CHECK(removed);

    const auto root = parseObj(out.value());
    DOCTEST_CHECK(root.get("numStartups").asInt() == 183);
    DOCTEST_CHECK_EQ(root.get("machineID").asString(), std::string("abc123"));
    const auto servers = root.get("mcpServers").asObject();
    DOCTEST_CHECK_FALSE(servers.has("devdesk"));
    DOCTEST_CHECK(servers.has("codegraph"));
    DOCTEST_CHECK(servers.has("rea"));
    DOCTEST_CHECK(servers.keys().size() == 2);
    // 回到安装前原样（往返守恒）
    DOCTEST_CHECK_EQ(serversJson(out.value()), serversJson(kSampleJson));
    return true;
})

// 卸载幂等 + 无 mcpServers 容器时不凭空造键
REGISTER_TEST(mcp_cc_remove_noop_and_no_container,
              "mcp_cc: 本就没装 → 卸载 changed=false；无 mcpServers 键不凭空创建", {
    bool removed = false;
    const auto out = cc::removeServerEntry(kSampleJson, &removed);
    DOCTEST_REQUIRE(out.isOk());
    DOCTEST_CHECK_FALSE(removed);
    DOCTEST_CHECK_EQ(out.value(), std::string(kSampleJson));

    const std::string noServers = R"({"numStartups": 1})";
    bool r2 = false;
    const auto out2 = cc::removeServerEntry(noServers, &r2);
    DOCTEST_REQUIRE(out2.isOk());
    DOCTEST_CHECK_FALSE(r2);
    DOCTEST_CHECK_FALSE(parseObj(out2.value()).has("mcpServers"));
    return true;
})

// 非法输入不静默吞：解析失败必须 fail（否则会拿空对象覆盖 105KB 真配置）
REGISTER_TEST(mcp_cc_invalid_json_fails,
              "mcp_cc: 非法 JSON / 顶层非对象 → 安装与卸载都 fail", {
    bool changed = true;
    const auto bad = cc::applyServerEntry("{not json", "http://x/mcp", &changed);
    DOCTEST_CHECK(bad.isFail());
    DOCTEST_CHECK_FALSE(changed);

    bool changed2 = true;
    const auto arr = cc::applyServerEntry("[1,2]", "http://x/mcp", &changed2);
    DOCTEST_CHECK(arr.isFail());
    DOCTEST_CHECK_FALSE(changed2);

    bool changed3 = true;
    const auto badRemove = cc::removeServerEntry("{not json", &changed3);
    DOCTEST_CHECK(badRemove.isFail());
    return true;
})

// 文件级：不存在 → fail 且绝不创建（禁自动创建 ~/.claude.json）
REGISTER_TEST(mcp_cc_missing_file_not_created,
              "mcp_cc: ~/.claude.json 缺失时安装 fail 且文件仍不存在", {
    const auto file = makeTempConfig();
    DOCTEST_CHECK_FALSE(std::filesystem::exists(file));
    const auto r = cc::installServerFile(file, "http://127.0.0.1:8888/mcp");
    DOCTEST_CHECK(r.isFail());
    DOCTEST_CHECK_FALSE(std::filesystem::exists(file));
    // 备份也不该凭空出现
    DOCTEST_CHECK_FALSE(std::filesystem::exists(cc::backupFile(file)));
    return true;
})

// 审计 P1-5：mcpServers 键存在但非对象 → 安装与卸载都 fail 拒写（防静默吞原值）；
// 显式 null 与缺失等同 → 放行新建
REGISTER_TEST(mcp_cc_servers_non_object_rejected,
              "mcp_cc: mcpServers 非对象（数组/字符串）→ 安装/卸载 fail；null 放行新建", {
    bool changed = true;
    const auto arr = cc::applyServerEntry(R"({"mcpServers": [1,2]})", "http://x/mcp",
                                          &changed);
    DOCTEST_CHECK(arr.isFail());
    DOCTEST_CHECK_FALSE(changed);

    bool changed2 = true;
    const auto str = cc::applyServerEntry(R"({"mcpServers": "oops"})", "http://x/mcp",
                                          &changed2);
    DOCTEST_CHECK(str.isFail());
    DOCTEST_CHECK_FALSE(changed2);

    bool changed3 = true;
    const auto rem = cc::removeServerEntry(R"({"mcpServers": [1,2]})", &changed3);
    DOCTEST_CHECK(rem.isFail());

    // null =「无容器」语义：放行（安装新建对象、卸载幂等）
    bool changed4 = true;
    const auto nullInstall = cc::applyServerEntry(R"({"mcpServers": null})", "http://x/mcp",
                                                  &changed4);
    DOCTEST_REQUIRE(nullInstall.isOk());
    DOCTEST_CHECK(changed4);
    DOCTEST_CHECK(parseObj(nullInstall.value()).get("mcpServers").asObject().has("devdesk"));

    bool changed5 = false;
    const auto nullRemove = cc::removeServerEntry(R"({"mcpServers": null})", &changed5);
    DOCTEST_REQUIRE(nullRemove.isOk());
    DOCTEST_CHECK_FALSE(changed5);
    return true;
})

// 审计 P1-3：写失败面——路径存在但不是文件（目录冒充 .claude.json）→ 读取分支
// fail 且目标不被破坏。UI 侧红字 hint 由 hint 槽透传该 errorMessage（服务未运行时
// 按钮置灰不可达，真机验收看红字；此处钉错误来源文案非空即可）。
REGISTER_TEST(mcp_cc_dir_as_file_fails_clean,
              "mcp_cc: .claude.json 是目录 → 安装 fail（读失败分支）且目录原样保留", {
    const auto dir = test_temp::tempPath("cc_cfg_dirfile");
    std::error_code ec;
    std::filesystem::create_directories(dir / ".claude.json", ec);
    DOCTEST_REQUIRE(std::filesystem::is_directory(dir / ".claude.json"));

    const auto install = cc::installServerFile(dir / ".claude.json", "http://127.0.0.1:8888/mcp");
    DOCTEST_CHECK(install.isFail());
    DOCTEST_CHECK_FALSE(install.errorMessage().empty());
    // 目标没被删/没被换成文件
    DOCTEST_CHECK(std::filesystem::is_directory(dir / ".claude.json"));

    const auto remove = cc::removeServerFile(dir / ".claude.json");
    DOCTEST_CHECK(remove.isFail());

    std::filesystem::remove_all(dir, ec);
    return true;
})

// 文件级：安装写盘 + 备份原内容 + 状态读取；再卸载恢复原样
REGISTER_TEST(mcp_cc_file_roundtrip_with_backup,
              "mcp_cc: 文件安装→状态已装→卸载→内容恢复原状，且留 .bak-devdesk 备份", {
    const auto file = makeTempConfig();
    {
        std::error_code ec;
        std::filesystem::create_directories(file.parent_path(), ec);
        std::ofstream f(file, std::ios::binary | std::ios::trunc);
        DOCTEST_REQUIRE(static_cast<bool>(f));
        f << kSampleJson;
    }

    const cc::ConfigState before = cc::readConfigState(file);
    DOCTEST_CHECK(before.filePresent);
    DOCTEST_CHECK_FALSE(before.installed);
    DOCTEST_CHECK(before.error.empty());

    const auto install = cc::installServerFile(file, "http://127.0.0.1:8888/mcp");
    DOCTEST_REQUIRE(install.isOk());
    const cc::ConfigState afterInstall = cc::readConfigState(file);
    DOCTEST_CHECK(afterInstall.installed);
    // 写盘后的 mcpServers 与纯函数结果一致（文件级只是薄编排）
    bool expectChanged = false;
    const auto expected = cc::applyServerEntry(kSampleJson, "http://127.0.0.1:8888/mcp",
                                               &expectChanged);
    DOCTEST_REQUIRE(expected.isOk());
    DOCTEST_CHECK_EQ(serversJson(core::readTextFile(file)),
                     serversJson(expected.value()));
    // 备份 = 安装前的原内容
    DOCTEST_CHECK_EQ(core::readTextFile(cc::backupFile(file)), std::string(kSampleJson));

    const auto remove = cc::removeServerFile(file);
    DOCTEST_REQUIRE(remove.isOk());
    const cc::ConfigState afterRemove = cc::readConfigState(file);
    DOCTEST_CHECK_FALSE(afterRemove.installed);
    // 语义恢复原状（键序由封装按 std::map 排序，比对结构化等价）
    DOCTEST_CHECK_EQ(serversJson(core::readTextFile(file)), serversJson(kSampleJson));
    const auto root = parseObj(core::readTextFile(file));
    DOCTEST_CHECK(root.get("numStartups").asInt() == 183);

    std::error_code ec;
    std::filesystem::remove_all(file.parent_path(), ec);
    return true;
})

// 文件级幂等：重复安装不改写文件（mtime/内容都不动）
REGISTER_TEST(mcp_cc_file_install_twice_no_rewrite,
              "mcp_cc: 文件级重复安装第二次不重写（内容与备份均不变）", {
    const auto file = makeTempConfig();
    {
        std::error_code ec;
        std::filesystem::create_directories(file.parent_path(), ec);
        std::ofstream f(file, std::ios::binary | std::ios::trunc);
        DOCTEST_REQUIRE(static_cast<bool>(f));
        f << kSampleJson;
    }
    DOCTEST_REQUIRE(cc::installServerFile(file, "http://127.0.0.1:8888/mcp").isOk());
    const std::string afterFirst = core::readTextFile(file);
    DOCTEST_REQUIRE(cc::installServerFile(file, "http://127.0.0.1:9999/mcp").isOk());
    DOCTEST_CHECK_EQ(core::readTextFile(file), afterFirst);  // 端口不覆盖
    // 第二次无改动 → 备份仍是第一次的原内容，未被第二次覆盖
    DOCTEST_CHECK_EQ(core::readTextFile(cc::backupFile(file)), std::string(kSampleJson));

    std::error_code ec;
    std::filesystem::remove_all(file.parent_path(), ec);
    return true;
})

// 真文件实证（need244）：把用户真实 ~/.claude.json（105KB / 51 个顶层键，含
// Claude Code 自身状态）整份复制到临时目录后走安装→卸载，断言「顶层键集合
// 与各值语义全等」。不在真文件上做写操作（见 manifest：GUI 点击链未验证，
// 真文件零改动即天然满足「测完恢复原状」）。
REGISTER_TEST(mcp_cc_real_file_preserves_all_keys,
              "mcp_cc: 真实 ~/.claude.json 副本安装→卸载后顶层键与值全等（保留自身状态）", {
    // 审计 P1-2：定位走生产契约 claudeConfigFile()（含 DEVDESK_CLAUDE_CONFIG_DIR
    // 覆盖与 Win USERPROFILE），不裸取 HOME——否则 Windows 必红且平台面假绿
    const std::filesystem::path real = core::paths::claudeConfigFile();
    std::error_code ec;
    if (!std::filesystem::exists(real, ec)) {
        DOCTEST_SKIP("本机无 ~/.claude.json");
    }
    const std::string original = core::readTextFile(real);
    // 样本过小（<1000 字节 / ≤10 顶层键）不具代表性 → 跳过而非硬红（审计 P2-15）
    if (original.size() <= 1000) {
        DOCTEST_SKIP("真配置过小，样本不具代表性");
    }
    const auto before = parseObj(original);
    if (before.keys().size() <= 10) {
        DOCTEST_SKIP("真配置顶层键过少，样本不具代表性");
    }

    bool changed = false;
    const auto installed = cc::applyServerEntry(original, "http://127.0.0.1:8888/mcp",
                                                &changed);
    DOCTEST_REQUIRE(installed.isOk());
    DOCTEST_CHECK(changed);
    DOCTEST_CHECK(installed.value().size() > original.size() * 9 / 10);  // 没被截断

    bool removed = false;
    const auto restored = cc::removeServerEntry(installed.value(), &removed);
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

// 真机 E2E（need244，默认 SKIP）：DEVDESK_CC_REAL_E2E=1 时对**用户真实**
// ~/.claude.json 走一遍生产写入路径（installServerFile → removeServerFile），
// 打印前后片段供人工核对；结束前断言内容语义回到原状。
// 为什么默认跳过：CI / 无 Claude Code 的机器不该被用户态文件影响；真机验收由
// 本用例显式驱动（口径同 memory_install_probe 的显式驱动）。
REGISTER_TEST(mcp_cc_real_file_e2e_gated,
              "mcp_cc: [DEVDESK_CC_REAL_E2E=1] 真实 ~/.claude.json 安装→卸载，打印前后片段", {
    const char* gate = std::getenv("DEVDESK_CC_REAL_E2E");
    if (gate == nullptr || std::string(gate) != "1") {
        DOCTEST_SKIP("真机 E2E 需显式 DEVDESK_CC_REAL_E2E=1 驱动");
    }
    const std::filesystem::path real = core::paths::claudeConfigFile();  // 审计 P1-2
    DOCTEST_REQUIRE(std::filesystem::exists(real));
    const std::string original = core::readTextFile(real);
    const auto before = parseObj(original);
    std::printf("[e2e] before: bytes=%zu topKeys=%zu mcpServers=%s\n",
                original.size(), before.keys().size(),
                serversJson(original).c_str());

    const auto install = cc::installServerFile(real, "http://127.0.0.1:8888/mcp");
    DOCTEST_REQUIRE_MESSAGE(install.isOk(), install.errorMessage());
    // 审计 P1-1：装上真文件后任何断言红都不能把 devdesk 条目留在用户配置里——
    // RAII 兜底卸载（幂等：测试体正常卸载后再跑一遍 = changed=false 无操作）
    struct RealFileRestoreGuard {
        const std::filesystem::path& file;
        ~RealFileRestoreGuard() {
            std::error_code ec;
            if (!std::filesystem::exists(file, ec) || ec) return;
            const auto r = cc::removeServerFile(file);
            if (r.isFail()) {
                std::fprintf(stderr, "[e2e] 兜底卸载失败（手工检查 mcpServers.devdesk）: %s\n",
                             r.errorMessage().c_str());
            }
        }
    } restoreGuard{real};
    const std::string afterInstall = core::readTextFile(real);
    const auto installed = parseObj(afterInstall);
    std::printf("[e2e] after install: bytes=%zu topKeys=%zu mcpServers=%s\n",
                afterInstall.size(), installed.keys().size(),
                serversJson(afterInstall).c_str());
    DOCTEST_CHECK(installed.keys().size() == before.keys().size());
    DOCTEST_CHECK(installed.get("mcpServers").asObject().has("devdesk"));
    DOCTEST_CHECK(std::filesystem::exists(cc::backupFile(real)));

    const auto remove = cc::removeServerFile(real);
    DOCTEST_REQUIRE_MESSAGE(remove.isOk(), remove.errorMessage());
    const std::string afterRemove = core::readTextFile(real);
    const auto restored = parseObj(afterRemove);
    std::printf("[e2e] after remove: bytes=%zu topKeys=%zu mcpServers=%s\n",
                afterRemove.size(), restored.keys().size(),
                serversJson(afterRemove).c_str());
    DOCTEST_CHECK_FALSE(restored.get("mcpServers").asObject().has("devdesk"));
    DOCTEST_CHECK_EQ(restored.keys().size(), before.keys().size());
    for (const std::string& key : before.keys()) {
        const auto a = pj::stringify(before.get(key));
        const auto b = pj::stringify(restored.get(key));
        DOCTEST_CHECK_MESSAGE(a.isOk() && b.isOk() && a.value() == b.value(),
                              "真文件顶层键值被改写: " + key);
    }
    return true;
})
