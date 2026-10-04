// ProfileRepository：claude_profiles 行 + SharedConfig 片段的唯一持久化入口。
// 不变式：api_key_hint 是 UI / 导出用脱敏形态（前6+***+尾4）；api_key_check 是
// 全量 SHA256 hex（「密钥是否变更」判据）——两者随密钥写入同批更新；is_active
// 全局唯一；SharedConfig 悬空 id 读路径容错（退化为未绑定）。
// 线程约定：无自有锁，随调用方（SqliteDb recursive_mutex）
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
    // **仅内存** 用；loadApiKey() 后才有值。非空 = 本次 update 要覆盖凭据列。
    // 凭据本体在 api_key_encrypted 列，一律经 loadApiKey / TwoPhaseKeyWrite
    // 读写，谁都不该直接读那一列
    std::string apiKeyPlain;
    std::string apiKeyHint;     // 例如 sk-ant-***xyz（UI 显示 / 导出占位用，脱敏）
    std::string apiKeyCheck;    // 全量 SHA256 hex（64 字符；「密钥是否变更」判据）
    std::string model;          // 默认模型（= models_config 的 default 槽位，双向同步）
    std::string modelsConfigJson; // 6 槽位模型配置 JSON 原文（空串 = 未配置）
    std::string extraEnvJson;   // 额外 env 键值 JSON 原文（空串 = 未配置）
    // settings.json **除 env 外全部顶层属性**的原始 JSON（空串 = 未配置）。
    // 仅存原文不拆字段；UI 落库统一 pretty-print，读侧容错见 provider_config
    std::string extraSettingsJson;
    int64_t sharedConfigId = 0; // 绑定共享配置片段 id（0 = 未绑定；0..1、不嵌套）
    std::string authField = "both";  // 认证字段三态（'both'/'bearer'/'api_key'；未知/空串按 both 读）
    // 上游方言覆盖：空串 = 未覆盖 = 按 baseUrl 的 host 推断。自建网关 /
    // 内网镜像 host 推断不出正确方言时用它钉住；未知/空串读侧回落推断、
    // 写侧归一为空串，取值域见 upstream_dialect.h
    std::string dialectOverride;
    // 备用 Profile id（0 = 无）：主 profile 返回可重试错误（401/403/429/5xx/
    // 网络失败）时代理用备用重试一次（不改 is_active、不重写 settings.json）。
    // 写侧校验拒自引用；删除时同事务解绑；复制时一并携带
    int64_t fallbackProfileId = 0;
    bool isActive = false;
    std::time_t createdAt = 0;
    std::time_t updatedAt = 0;
};

// 共享配置片段：只有名字 + 高级属性 JSON 预设，**不含 base_url / 密钥**，
// 故不进路由、不进凭据链；一个 profile 绑定 0..1 个片段，不嵌套
struct SharedConfig {
    int64_t id = 0;
    std::string name;
    std::string extraSettingsJson;   // 高级属性 JSON 预设原文（空串 = 未配置）
    std::time_t createdAt = 0;
    std::time_t updatedAt = 0;
};

// 两段写助手：段一 writeKey() 把凭据三列写成新值（空串明文 = 三列清空），
// 段二 writeDb() 落该行其余元数据；任一段失败 → 按捕获的旧值回写三列，保证
// 密钥与 hint / check 始终对得上（回到「改之前」的状态，不留中间态）。
// 保持两段分离而不并成一条 UPDATE：四种宿主的失败补偿语义与文案都围绕它写。
// 旧值按**存储形态原样字节**回写（不经 protect 重编码），DPAPI 那种「同一明文
// 两次加密结果不同」的平台也能回到逐字节旧状态
class TwoPhaseKeyWrite {
public:
    TwoPhaseKeyWrite(core::data::SqliteDb& db, int64_t profileId,
                     std::string oldStored, std::string oldPlain);

    // 捕获该行当前的凭据（读不到 / 读失败 = 按「没有密钥」= 空串处理）
    static TwoPhaseKeyWrite capture(core::data::SqliteDb& db, int64_t profileId);

    // 段一：把三列写成 newKey（空串 = 清空三列）。
    // 失败 → 尽力回写旧值后返回该错误；调用方不应继续段二。
    core::Result<void> writeKey(const std::string& newKey);

    // 段二：执行 DB 写；失败 → 尽力回写旧值后返回该错误。
    core::Result<void> writeDb(const std::function<core::Result<void>()>& dbWrite);

    // 捕获到的旧明文（供调用方做「密钥是否已回到旧值」的诊断比较）
    const std::string& oldKeyPlain() const { return oldPlain_; }

private:
    // 三列已改且尚未回到旧值 → 回写旧值（失败只记 ERROR，结果由调用方
    // 从列的实际状态判断）
    void compensate();

    core::data::SqliteDb& db_;
    int64_t profileId_;
    std::string oldStored_;     // 旧存储形态原样字节（回写用，不经 protect）
    std::string oldPlain_;      // 旧明文（诊断比较用）
    bool keyTouched_ = false;   // 段一改动过该行且尚未回到旧值
};

class ProfileRepository {
public:
    // db 由调用方确保生命周期；keychainService 只用于启动时的旧 Keychain
    // 残留清理（命名空间由生产 / 测试各自派生，见 cleanupOrphanKeychainEntries）
    explicit ProfileRepository(core::data::SqliteDb& db,
                               std::string keychainService);

    // ---- Profile CRUD ----

    // 列出所有 profile（apiKeyPlain 全部为空字符串）
    core::Result<std::vector<Profile>> listProfiles();

    // 取单个 profile（apiKeyPlain 为空）
    core::Result<Profile> getProfile(int64_t id);

    // 当前激活 profile（无激活 → NoActiveProfile + 可操作指引；该文案是
    // 503 JSON body 的原样透传内容，proxy_router 唯一出口）
    core::Result<Profile> getActiveProfile();

    // 创建并保存到 SQLite（校验 + protect(apiKey) + 一条 INSERT ... RETURNING id
    // 原子取回 rowid）。protect 失败在建行**之前**失败，不留行、不留半截凭据。
    // 不含「首建自动激活」编排（策略尾在门面）；返回的 Profile.isActive 恒
    // false（激活与否由门面回填），时间戳为写侧统一取值的 now。
    // fallbackProfileId / extraSettingsJson 建时即带（同「建时即设」口径），
    // 避免先建后 update 留下未配置的中间态
    core::Result<Profile> createProfile(const std::string& name,
                                        const std::string& baseUrl,
                                        const std::string& apiKey,
                                        const std::string& model,
                                        int64_t sharedConfigId = 0,
                                        const std::string& authField = "both",
                                        const std::string& dialectOverride =
                                            std::string(),
                                        int64_t fallbackProfileId = 0,
                                        const std::string& extraSettingsJson =
                                            std::string());

    // 更新 profile（profile.id 必须已存在；apiKeyPlain 非空则覆盖凭据三列，
    // 空则只更新元数据）。JSON 信任边界校验同 createProfile。oldProfile（可空
    // 出参）：成功时回填更新前的旧行——门面 re-takeover 策略尾据此判断，
    // 本类不做该编排
    core::Result<void> updateProfile(const Profile& profile,
                                     Profile* oldProfile = nullptr);

    // 删除 profile（两段写：段一清凭据三列、段二删 SQLite 行；任一段失败
    // 按旧值回写三列，行保留可重试）。wasActive（可空出参）回填被删行的
    // is_active——门面停代理策略尾据此判断，本类不做该编排
    core::Result<void> deleteProfile(int64_t id, bool* wasActive = nullptr);

    // 复制 Profile：名称自动加「副本」后缀（重名递增避让，100 次仍冲突 →
    // fail 不建行），API key 一并复制（源 key 缺失 → fail，文案带恢复提示；
    // 读失败原样透传错误码——「读不了」≠「没有」，不误导用户去重填），
    // **副本不激活**。二段补写失败 → best-effort 删刚建的副本回滚（回滚失败
    // 则文案注明不完整副本 id）。shared_config_id / dialect_override /
    // fallback_profile_id / extra_settings_json 一并复制——属于配置，二段
    // 门槛不成立时也不得静默丢字段
    core::Result<Profile> duplicateProfile(int64_t srcId);

    // 启动时旧 Keychain 残留的一次性清理：凭据已迁进本库，本 service 命名
    // 空间下剩余的 claude:* 条目全是没人管的旧钥匙 → 一次性清掉（白名单前缀
    // 之外的 account 一律保留、不计数）。返回清理数量。
    // 三道护栏（少一条都会误删用户真实密钥，事故各出过一次）：
    //   ① 当前数据根零 profile → 整体跳过（service 命名空间的隔离级别弱于
    //      DEVDESK_DATA_ROOT，空根清残留会连用户真实密钥一起清光）
    //   ② 白名单前缀 claude:
    //   ③ api_key_encrypted 列必须可读，且至少一行真的存着凭据——只看
    //      「有没有 profile」会被旧形态的库骗过（旧库同样有行，SELECT 不引用
    //      新列照样成功）
    core::Result<int> cleanupOrphanKeychainEntries();

    // 取该 profile 的明文 API key（仅发请求时调用，用完即丢弃）。
    // 列空 = 没有密钥 → fail(NotFound)；存储形态解不开 → fail(原错误码)——
    // 「解不开」与「没配过」必须可分辨，不静默回落空串
    core::Result<std::string> loadApiKey(int64_t profileId);

    // setActiveProfile 的 DB 段：事务内「清零所有 is_active → 置位目标」
    //（is_active 全局唯一）。调用方（门面）负责前置校验；捕获编排
    //（syncCaptureActiveProfile）必须先于本事务发生，否则会把刚写进去的
    // 托管值当用户值捕获
    core::Result<void> activateProfile(int64_t id);

    // ---- 共享配置片段 ----
    // 生命周期语义（改动当场生效，避免「改了片段不生效」的头号 bug；策略尾
    // 都在门面）：改 → 绑定它的激活 profile 代理在跑则立即 re-takeover；
    // 删 → 同事务解绑绑定它的 profile（不留悬空 id）后再判 re-takeover；
    // 解绑 → 同款当场生效，同名键回落 profile 自身值（片段值从不写进
    // profile 行）
    core::Result<std::vector<SharedConfig>> listSharedConfigs();
    core::Result<SharedConfig> createSharedConfig(const std::string& name,
                                                  const std::string& extraSettingsJson);
    core::Result<void> updateSharedConfig(const SharedConfig& config);
    core::Result<void> deleteSharedConfig(int64_t id, bool* activeBound = nullptr);

    // 片段高级属性 JSON 原文（0 / 片段不存在 / 读失败 → 空串 = 未绑定；读路径
    // 容错，悬空 id 退化成「未绑定」而不是让接管失败）。导出链不经此访问器，
    // 走列表快照 cachedSharedConfigs_ 的 findSharedById
    std::string sharedExtraSettingsJsonFor(int64_t sharedConfigId) const;

private:
    // 从一行 SqlRow 构造 Profile（不含 apiKeyPlain）
    core::Result<Profile> rowToProfile(const core::data::SqlRow& row) const;

    // 从一行 SqlRow 构造 SharedConfig
    core::Result<SharedConfig> rowToSharedConfig(const core::data::SqlRow& row) const;

    // 片段存在性校验（update / delete 前置，缺失 → NotFound）
    core::Result<void> requireSharedConfigExists(int64_t id) const;

    core::data::SqliteDb& db_;
    std::string keychainService_;   // 仅用于旧 Keychain 残留清理（见类头不变式）
};

} // namespace module::claude
