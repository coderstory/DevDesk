// ProfileRepository：claude_profiles 行 + SharedConfig 片段的唯一持久化入口。
// 不变式：api_key_hint 为脱敏形态（前6+***+尾4）、api_key_check 为全量 SHA256 hex（变更判据），随密钥写入同批更新；is_active 全局唯一；悬空 SharedConfig id 读路径容错
#pragma once

#include <cstdint>
#include <ctime>
#include <functional>
#include <string>
#include <vector>

#include "core/result.h"
#include "core/sqlite_db.h"

namespace module::claude {

// 单条 Claude API 配置
struct Profile {
    int64_t id = 0;
    std::string name;
    std::string baseUrl;        // 例如 https://api.anthropic.com
    // 仅内存：loadApiKey() 后才有值；非空 = 本次 update 覆盖凭据列。凭据本体在 api_key_encrypted 列，一律经 loadApiKey / TwoPhaseKeyWrite 读写
    std::string apiKeyPlain;
    std::string apiKeyHint;     // 例如 sk-ant-***xyz（UI 显示 / 导出占位用，脱敏）
    std::string apiKeyCheck;    // 全量 SHA256 hex（64 字符；「密钥是否变更」判据）
    std::string model;          // 默认模型（= models_config 的 default 槽位，双向同步）
    std::string modelsConfigJson; // 6 槽位模型配置 JSON 原文（空串 = 未配置）
    std::string extraEnvJson;   // 额外 env 键值 JSON 原文（空串 = 未配置）
    int64_t sharedConfigId = 0; // 绑定共享配置片段 id（0 = 未绑定；0..1、不嵌套）
    std::string authField = "both";  // 认证字段三态（'both'/'bearer'/'api_key'；未知/空串按 both 读）
    // 备用 Profile id（0 = 无）：主 profile 返回可重试错误（401/403/429/5xx/网络失败）时用备用重试一次（不动 is_active、不重写 settings.json）；写侧拒自引用，删除同事务解绑
    int64_t fallbackProfileId = 0;
    bool isActive = false;
    std::time_t createdAt = 0;
    std::time_t updatedAt = 0;
};

// 共享配置片段：只有名字 + 高级属性 JSON 预设，不含 base_url / 密钥，不进路由与凭据链；一个 profile 绑 0..1 个，不嵌套
struct SharedConfig {
    int64_t id = 0;
    std::string name;
    std::string extraSettingsJson;   // 高级属性 JSON 预设原文（空串 = 未配置）
    std::time_t createdAt = 0;
    std::time_t updatedAt = 0;
};

// 两段写助手：writeKey() 写凭据三列（空串 = 清空），writeDb() 落其余元数据；任一段失败按旧值存储形态原样字节回写（不经 protect 重编码），不留中间态
class TwoPhaseKeyWrite {
public:
    TwoPhaseKeyWrite(core::data::SqliteDb& db, int64_t profileId,
                     std::string oldStored, std::string oldPlain);

    // 捕获该行当前的凭据（读不到 / 读失败 = 按「没有密钥」= 空串处理）
    static TwoPhaseKeyWrite capture(core::data::SqliteDb& db, int64_t profileId);

    // 段一：把三列写成 newKey（空串 = 清空三列）。失败 → 尽力回写旧值后返回该错误；调用方不应继续段二
    core::Result<void> writeKey(const std::string& newKey);

    // 段二：执行 DB 写；失败 → 尽力回写旧值后返回该错误。
    core::Result<void> writeDb(const std::function<core::Result<void>()>& dbWrite);

    // 捕获到的旧明文（供调用方做「密钥是否已回到旧值」的诊断比较）
    const std::string& oldKeyPlain() const { return oldPlain_; }

private:
    // 三列已改且尚未回到旧值 → 回写旧值（失败只记 ERROR，结果由调用方从列的实际状态判断）
    void compensate();

    core::data::SqliteDb& db_;
    int64_t profileId_;
    std::string oldStored_;     // 旧存储形态原样字节（回写用，不经 protect）
    std::string oldPlain_;      // 旧明文（诊断比较用）
    bool keyTouched_ = false;   // 段一改动过该行且尚未回到旧值
};

class ProfileRepository {
public:
    // db 由调用方确保生命周期；keychainService 仅用于旧凭据回填（backfillCredentialsFromKeychain）
    // 的 Keychain 命名空间（命名空间生产 / 测试各自派生）
    explicit ProfileRepository(core::data::SqliteDb& db,
                               std::string keychainService);

    // Profile CRUD

    // 列出所有 profile（apiKeyPlain 全部为空字符串）
    core::Result<std::vector<Profile>> listProfiles();

    // 取单个 profile（apiKeyPlain 为空）
    core::Result<Profile> getProfile(int64_t id);

    // 当前激活 profile（无激活 → NoActiveProfile + 可操作指引；该文案是 503 JSON body 的原样透传内容，proxy_router 唯一出口）
    core::Result<Profile> getActiveProfile();

    // 创建并保存（校验 + protect + INSERT ... RETURNING id 原子取回）。protect 失败在建行前失败，不留行；不含「首建自动激活」编排（策略尾在门面）
    core::Result<Profile> createProfile(const std::string& name,
                                        const std::string& baseUrl,
                                        const std::string& apiKey,
                                        const std::string& model,
                                        int64_t sharedConfigId = 0,
                                        const std::string& authField = "both",
                                        int64_t fallbackProfileId = 0);

    // 更新 profile（id 必须已存在；apiKeyPlain 非空则覆盖凭据三列，空则只更新元数据）。
    // oldProfile（可空出参）成功时回填更新前的旧行——门面 re-takeover 据此判断。
    core::Result<void> updateProfile(const Profile& profile,
                                     Profile* oldProfile = nullptr);

    // 删除 profile（两段写：段一清凭据三列、段二删 SQLite 行；任一段失败按旧值
    // 回写三列，行保留可重试）。wasActive（可空出参）回填被删行 is_active。
    core::Result<void> deleteProfile(int64_t id, bool* wasActive = nullptr);

    // 复制 Profile：名称加「副本」后缀（重名递增避让，100 次冲突 → fail）；API key 一并复制（源缺失 → fail）；副本不激活；二段补写失败 best-effort 回滚
    core::Result<Profile> duplicateProfile(int64_t srcId);

    // 迁移旧 Keychain 凭据入库：查 api_key_encrypted 为空且 keychain_account 列存在的 profile，
    // 逐行读旧 keychain → protect → 写回凭据三列；失败不中断启动（LOG_WARN 后继续下一行）。
    // 仅在旧 keychain_account 列存在时执行（新库跳过，零开销）。成功回填的条目一并更新 hint/check。
    core::Result<int> backfillCredentialsFromKeychain();

    // 取明文 API key（仅发请求时调用，用完即丢弃）。列空 → fail(NotFound)；解不开 → fail(原错误码)——「解不开」与「没配过」必须可分辨
    core::Result<std::string> loadApiKey(int64_t profileId);

    // setActiveProfile 的 DB 段：事务内「清零所有 is_active → 置位目标」。
    // 调用方负责前置校验；反向同步（syncCaptureActiveProfile）必须先于本事务发生。
    core::Result<void> activateProfile(int64_t id);

    // 共享配置片段
    // 生命周期语义（策略尾都在门面）：改 → 绑定它的激活 profile 代理在跑则立即 re-takeover；删 → 同事务解绑后再判 re-takeover；解绑后同名键回落 profile 自身值
    core::Result<std::vector<SharedConfig>> listSharedConfigs();
    core::Result<SharedConfig> createSharedConfig(const std::string& name,
                                                  const std::string& extraSettingsJson);
    core::Result<void> updateSharedConfig(const SharedConfig& config);
    core::Result<void> deleteSharedConfig(int64_t id, bool* activeBound = nullptr);

    // 片段高级属性 JSON 原文（0 / 不存在 / 读失败 → 空串 = 未绑定；悬空 id 退化成「未绑定」）。导出链走列表快照 findSharedById
    std::string sharedExtraSettingsJsonFor(int64_t sharedConfigId) const;

private:
    // 从一行 SqlRow 构造 Profile（不含 apiKeyPlain）
    core::Result<Profile> rowToProfile(const core::data::SqlRow& row) const;

    // 从一行 SqlRow 构造 SharedConfig
    core::Result<SharedConfig> rowToSharedConfig(const core::data::SqlRow& row) const;

    // 片段存在性校验（update / delete 前置，缺失 → NotFound）
    core::Result<void> requireSharedConfigExists(int64_t id) const;

    core::data::SqliteDb& db_;
    std::string keychainService_;   // 仅用于旧 Keychain 凭据回填（见构造注释）
};

} // namespace module::claude
