// src/module_wallpaper/core/importer.h
// 「从花見导入」的纯逻辑层：台账（墓碑）、报告结构、去重判定。
// 零文件系统副作用在这里——真正的枚举/取大小/拷贝由 platform 层注入，
// 这样去重矩阵可以完全不碰磁盘地测。
//
// 身份判据 = **文件名 + 字节大小**（不做内容哈希）：
//   * 视频动辄几十上百 MB，内容哈希要把每个文件整读一遍；导入是用户点一次等着的
//     操作，读盘量能省则省。文件名 + 大小足以识别「同一个素材」——花見下载的文件名
//     自带唯一性，同名同大小却内容不同的概率可忽略。
//   * 同目录候选的文件名天然唯一，所以「同轮重复」这条路不存在（不需要额外记集合）。
//
// 口径（照搬已实跑验证的参考实现，仅把身份判据换成名字+大小）：
//   * 墓碑：已删素材的身份签名记库，导入时命中即跳过——否则「删一次、导一次全回来」。
//   * 墓碑读失败一律当「没删过」：坏库不得拦导入。写失败只丢一条墓碑，代价是素材多导一次。
#pragma once

#include <string>

namespace module::wallpaper::importer {

// 一次导入的读数。分项不可合并——合并会让「到底被哪一道吃掉的」无法定位。
struct ImportReport {
    int tombstoneSeededFromTrash = 0;  // 废纸篓回填墓碑数
    int imported = 0;
    int skippedTombstone = 0;  // 墓碑命中（删过的素材）
    int skippedExisting = 0;   // 目标目录已有同名文件
    int rejectedByProbe = 0;   // 探针判定没有视频轨
    int failedCopy = 0;        // 拷不动 / 取不到文件大小
    bool sourceMissing = false;  // 花見目录不存在（未安装或从未下载）
    bool sourceNoAccess = false;  // 花見目录存在但读不了（TCC 未授予完全磁盘访问）
};

// 单条候选的判定结果。纯决策，不做 IO。
enum class CandidateVerdict {
    Import,             // 通过全部检查，应拷贝
    SkipExisting,       // 目标已存在同名
    SkipTombstone,      // 墓碑命中
    FailStat,           // 取不到文件大小（读失败）
    RejectNoVideoTrack  // 探针判定无视频轨
};

// 一个候选的已知事实（由 platform 层填）。
struct CandidateFacts {
    bool targetExists = false;   // 目标目录已有同名文件
    bool signatureOk = false;    // 身份签名是否算出（= 能 stat 到大小）
    std::string signature;       // 身份签名（signatureOk 为真时有效）
    bool inTombstone = false;    // 墓碑库命中
    bool hasVideoTrack = false;  // 探针结论
};

// 纯决策。**顺序即语义**：
//   ① 目标已存在最优先——存在就跳过，连大小都不用取；
//   ② 取不到大小；
//   ③ 墓碑命中；
//   ④ 无视频轨。
inline CandidateVerdict decide(const CandidateFacts& facts) {
    if (facts.targetExists) return CandidateVerdict::SkipExisting;
    if (!facts.signatureOk) return CandidateVerdict::FailStat;
    if (facts.inTombstone) return CandidateVerdict::SkipTombstone;
    if (!facts.hasVideoTrack) return CandidateVerdict::RejectNoVideoTrack;
    return CandidateVerdict::Import;
}

// 把判定结果累加进报告（Import 之外的路径都只是计数）。
inline void recordVerdict(CandidateVerdict verdict, ImportReport& report) {
    switch (verdict) {
        case CandidateVerdict::Import:
            break;  // 由拷贝成功/失败决定 imported / failedCopy
        case CandidateVerdict::SkipExisting:
            ++report.skippedExisting;
            break;
        case CandidateVerdict::SkipTombstone:
            ++report.skippedTombstone;
            break;
        case CandidateVerdict::FailStat:
            ++report.failedCopy;
            break;
        case CandidateVerdict::RejectNoVideoTrack:
            ++report.rejectedByProbe;
            break;
    }
}

// 身份签名 = 文件名 + 分隔符 + 字节大小。
// 分隔符用 \x1f（单元分隔符）：合法文件系统名里不会出现，避免
// 「a|1 大小 2」与「a 大小 1|2」这类拼接歧义。
inline std::string makeSignature(const std::string& fileName, long long byteSize) {
    std::string out = fileName;
    out.push_back('\x1f');
    out += std::to_string(byteSize);
    return out;
}

// 墓碑台账接口。实现见 platform/importer_macos.cpp（SQLite）。
class TombstoneLedger {
public:
    virtual ~TombstoneLedger() = default;

    // 读：任何错误一律当「没删过」（坏库不得拦导入）。
    virtual bool contains(const std::string& signature) = 0;
    // 写：失败返回 false。墓碑缺一条只意味着素材多导一次，不阻断导入。
    virtual bool insert(const std::string& signature, double deletedAtEpoch) = 0;
};

} // namespace module::wallpaper::importer
