// src/module_pet/pet_skin.cpp
// 皮肤解析 + 内置占位皮肤生成。
// PNG 写出用未压缩 deflate（stored blocks）：只依赖 <cstdint> 标准库，
// 不引入 stb_image_write/libpng 链接依赖；128×128 RGBA ≈ 64KB/帧，占位皮肤可接受。
#include "module_pet/pet_skin.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>

#include "core/logging.h"
#include "core/paths.h"
#include "platform/json.h"

namespace module::pet {

using core::ErrorCode;
using core::Result;

// ============ PNG 写出（未压缩 deflate，stored blocks）============

namespace {

std::uint32_t crc32Of(const unsigned char* data, std::size_t count) {
    std::uint32_t crc = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < count; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return crc ^ 0xFFFFFFFFu;
}

std::uint32_t adler32Of(const unsigned char* data, std::size_t count) {
    std::uint32_t a = 1;
    std::uint32_t b = 0;
    for (std::size_t i = 0; i < count; ++i) {
        a = (a + data[i]) % 65521u;
        b = (b + a) % 65521u;
    }
    return (b << 16) | a;
}

void appendBigEndian32(std::vector<unsigned char>& out, std::uint32_t value) {
    out.push_back(static_cast<unsigned char>((value >> 24) & 0xFF));
    out.push_back(static_cast<unsigned char>((value >> 16) & 0xFF));
    out.push_back(static_cast<unsigned char>((value >> 8) & 0xFF));
    out.push_back(static_cast<unsigned char>(value & 0xFF));
}

void appendLittleEndian16(std::vector<unsigned char>& out, std::uint16_t value) {
    out.push_back(static_cast<unsigned char>(value & 0xFF));
    out.push_back(static_cast<unsigned char>((value >> 8) & 0xFF));
}

// chunk = length(4BE) + type(4) + data + crc32(type+data)
void appendPngChunk(std::vector<unsigned char>& out, const char* type,
                    const unsigned char* data, std::size_t count) {
    appendBigEndian32(out, static_cast<std::uint32_t>(count));
    std::vector<unsigned char> body(type, type + 4);
    if (count > 0) {
        body.insert(body.end(), data, data + count);
    }
    out.insert(out.end(), body.begin(), body.end());
    appendBigEndian32(out, crc32Of(body.data(), body.size()));
}

// RGBA → PNG（color type 6，每行 filter byte 0，zlib stored blocks 不压缩）
bool writePngRgba(const std::filesystem::path& path, int width, int height,
                  const std::vector<unsigned char>& rgba) {
    if (width <= 0 || height <= 0 ||
        rgba.size() != static_cast<std::size_t>(width) * height * 4) {
        return false;
    }
    // 扫描线：每行 1 字节 filter (0) + width*4 像素
    std::vector<unsigned char> raw;
    raw.reserve(static_cast<std::size_t>(height) * (1 + width * 4));
    for (int y = 0; y < height; ++y) {
        raw.push_back(0);
        const auto* row = rgba.data() + static_cast<std::size_t>(y) * width * 4;
        raw.insert(raw.end(), row, row + width * 4);
    }

    std::vector<unsigned char> png;
    png.reserve(raw.size() + 128);
    const unsigned char signature[] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    png.insert(png.end(), signature, signature + 8);

    unsigned char ihdr[13] = {};
    ihdr[0] = static_cast<unsigned char>((width >> 24) & 0xFF);
    ihdr[1] = static_cast<unsigned char>((width >> 16) & 0xFF);
    ihdr[2] = static_cast<unsigned char>((width >> 8) & 0xFF);
    ihdr[3] = static_cast<unsigned char>(width & 0xFF);
    ihdr[4] = static_cast<unsigned char>((height >> 24) & 0xFF);
    ihdr[5] = static_cast<unsigned char>((height >> 16) & 0xFF);
    ihdr[6] = static_cast<unsigned char>((height >> 8) & 0xFF);
    ihdr[7] = static_cast<unsigned char>(height & 0xFF);
    ihdr[8] = 8;   // bit depth
    ihdr[9] = 6;   // color type: RGBA
    ihdr[10] = 0;  // compression
    ihdr[11] = 0;  // filter
    ihdr[12] = 0;  // interlace
    appendPngChunk(png, "IHDR", ihdr, sizeof(ihdr));

    // zlib 流：header 0x78 0x01 + stored blocks + adler32
    std::vector<unsigned char> zlibStream;
    zlibStream.push_back(0x78);
    zlibStream.push_back(0x01);
    std::size_t offset = 0;
    while (offset < raw.size()) {
        const std::size_t blockLen = std::min<std::size_t>(raw.size() - offset, 65535);
        const bool finalBlock = (offset + blockLen == raw.size());
        zlibStream.push_back(finalBlock ? 1 : 0);
        appendLittleEndian16(zlibStream, static_cast<std::uint16_t>(blockLen));
        appendLittleEndian16(zlibStream, static_cast<std::uint16_t>(~blockLen & 0xFFFF));
        zlibStream.insert(zlibStream.end(), raw.begin() + offset,
                          raw.begin() + offset + blockLen);
        offset += blockLen;
    }
    appendBigEndian32(zlibStream, adler32Of(raw.data(), raw.size()));
    appendPngChunk(png, "IDAT", zlibStream.data(), zlibStream.size());
    appendPngChunk(png, "IEND", nullptr, 0);

    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        return false;
    }
    file.write(reinterpret_cast<const char*>(png.data()),
               static_cast<std::streamsize>(png.size()));
    file.close();
    return file.good();
}

// ============ 占位皮肤绘制（简单形状 + 1px 覆盖率抗锯齿边缘）============

struct Rgba {
    unsigned char r, g, b, a;
};

constexpr Rgba kClayBody{0xD9, 0x77, 0x57, 0xFF};   // Claude Clay
constexpr Rgba kInk{0x2B, 0x1D, 0x16, 0xFF};        // 深墨

struct Canvas {
    int w;
    int h;
    std::vector<unsigned char> px;

    explicit Canvas(int width, int height)
        : w(width), h(height), px(static_cast<std::size_t>(width) * height * 4, 0) {}

    void blend(int x, int y, const Rgba& color, float coverage) {
        if (x < 0 || y < 0 || x >= w || y >= h || coverage <= 0.0f) return;
        if (coverage > 1.0f) coverage = 1.0f;
        const float srcA = color.a / 255.0f * coverage;
        const float dstA = px[(y * w + x) * 4 + 3] / 255.0f;
        const float outA = srcA + dstA * (1.0f - srcA);
        if (outA <= 0.0f) return;
        unsigned char* p = &px[(y * w + x) * 4];
        for (int c = 0; c < 3; ++c) {
            const float src = c == 0 ? color.r : (c == 1 ? color.g : color.b);
            const float dst = p[c];
            p[c] = static_cast<unsigned char>((src * srcA + dst * dstA * (1.0f - srcA)) / outA);
        }
        p[3] = static_cast<unsigned char>(outA * 255.0f);
    }

    // 椭圆（带 ~1.5px 覆盖率边缘——占位皮肤也要有抗锯齿 alpha 边缘，
    // 这是透明窗口 premultiply 路径的真实素材形态）
    void ellipse(float cx, float cy, float rx, float ry, const Rgba& color) {
        for (int y = static_cast<int>(cy - ry - 2); y <= static_cast<int>(cy + ry + 2); ++y) {
            for (int x = static_cast<int>(cx - rx - 2); x <= static_cast<int>(cx + rx + 2); ++x) {
                const float dx = (x + 0.5f - cx) / rx;
                const float dy = (y + 0.5f - cy) / ry;
                const float dist = dx * dx + dy * dy;
                if (dist >= 1.3f) continue;
                const float coverage = (1.0f - std::sqrt(dist)) * 2.0f;
                blend(x, y, color, coverage);
            }
        }
    }

    // 硬边三角形（耳朵；被身体椭圆的抗锯齿边缘盖住底边）
    void triangle(float x1, float y1, float x2, float y2, float x3, float y3,
                  const Rgba& color) {
        const float minX = std::min({x1, x2, x3});
        const float maxX = std::max({x1, x2, x3});
        const float minY = std::min({y1, y2, y3});
        const float maxY = std::max({y1, y2, y3});
        const auto sign = [](float ax, float ay, float bx, float by, float px, float py) {
            return (px - bx) * (ay - by) - (ax - bx) * (py - by);
        };
        for (int y = static_cast<int>(minY); y <= static_cast<int>(maxY); ++y) {
            for (int x = static_cast<int>(minX); x <= static_cast<int>(maxX); ++x) {
                const float d1 = sign(x1, y1, x2, y2, x + 0.5f, y + 0.5f);
                const float d2 = sign(x2, y2, x3, y3, x + 0.5f, y + 0.5f);
                const float d3 = sign(x3, y3, x1, y1, x + 0.5f, y + 0.5f);
                const bool neg = (d1 < 0) || (d2 < 0) || (d3 < 0);
                const bool pos = (d1 > 0) || (d2 > 0) || (d3 > 0);
                if (neg && pos) continue;
                blend(x, y, color, 1.0f);
            }
        }
    }
};

enum class Eyes { Open, Closed, Wide };
enum class Mouth { Smile, Open };

// 画一只"黏土猫"帧：身体椭圆 + 三角耳 + 眼睛/嘴表情
Canvas drawCatFrame(int size, int bob, Eyes eyes, Mouth mouth, bool squash) {
    Canvas c(size, size);
    const float s = size / 128.0f;  // 参数按 128 设计，按比例缩放
    const float bodyY = (80.0f + bob) * s;
    c.ellipse(64.0f * s, bodyY, 44.0f * s, (squash ? 28.0f : 34.0f) * s, kClayBody);
    // 耳朵（squash 时贴着压扁的身体）
    const float earBase = (squash ? 60.0f : 52.0f + bob) * s;
    c.triangle(30.0f * s, earBase, 40.0f * s, (14.0f + bob) * s, 56.0f * s, earBase, kClayBody);
    c.triangle(98.0f * s, earBase, 88.0f * s, (14.0f + bob) * s, 72.0f * s, earBase, kClayBody);
    // 眼睛
    const float eyeY = (72.0f + bob) * s;
    if (eyes == Eyes::Open) {
        c.ellipse(46.0f * s, eyeY, 6.0f * s, 7.0f * s, kInk);
        c.ellipse(82.0f * s, eyeY, 6.0f * s, 7.0f * s, kInk);
    } else if (eyes == Eyes::Closed) {
        c.ellipse(46.0f * s, eyeY, 6.0f * s, 1.5f * s, kInk);
        c.ellipse(82.0f * s, eyeY, 6.0f * s, 1.5f * s, kInk);
    } else {  // Wide（被戳）
        c.ellipse(46.0f * s, eyeY, 8.0f * s, 9.0f * s, kInk);
        c.ellipse(82.0f * s, eyeY, 8.0f * s, 9.0f * s, kInk);
    }
    // 嘴
    if (mouth == Mouth::Smile) {
        c.ellipse(64.0f * s, (88.0f + bob) * s, 8.0f * s, 3.0f * s, kInk);
    } else {
        c.ellipse(64.0f * s, (90.0f + bob) * s, 7.0f * s, 6.0f * s, kInk);
    }
    return c;
}

bool writeFrame(const std::filesystem::path& dir, const char* file, Canvas&& canvas) {
    const std::filesystem::path target = dir / file;
    if (!writePngRgba(target, canvas.w, canvas.h, canvas.px)) {
        LOG_ERROR("桌宠占位皮肤帧写出失败: " + target.string());
        return false;
    }
    return true;
}

// idle 6 帧（frameRate 4 → 1.5s 周期，眨眼 0.25s）：静-轻浮-静-静-闭眼-静
// poked 2 帧（0.5s）：瞪眼张嘴 → 压扁闭眼
bool generatePlaceholderSkin(const std::filesystem::path& dir) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        LOG_ERROR("桌宠皮肤目录创建失败: " + dir.string() + " (" + ec.message() + ")");
        return false;
    }
    struct FrameSpec {
        const char* file;
        int bob;
        Eyes eyes;
        Mouth mouth;
        bool squash;
    };
    const FrameSpec frames[] = {
        {"f0.png", 0, Eyes::Open, Mouth::Smile, false},
        {"f1.png", -3, Eyes::Open, Mouth::Smile, false},
        {"f2.png", 0, Eyes::Open, Mouth::Smile, false},
        {"f3.png", 0, Eyes::Open, Mouth::Smile, false},
        {"f4.png", 0, Eyes::Closed, Mouth::Smile, false},
        {"f5.png", 0, Eyes::Open, Mouth::Smile, false},
        {"p0.png", 0, Eyes::Wide, Mouth::Open, false},
        {"p1.png", 0, Eyes::Closed, Mouth::Open, true},
    };
    for (const FrameSpec& spec : frames) {
        if (!writeFrame(dir, spec.file, drawCatFrame(128, spec.bob, spec.eyes, spec.mouth, spec.squash))) {
            return false;
        }
    }
    constexpr const char* kPetJson =
        "{\n"
        "  \"name\": \"clay-cat\",\n"
        "  \"frameSize\": [128, 128],\n"
        "  \"frameRate\": 4,\n"
        "  \"license\": \"CC0（程序生成占位皮肤，可整体替换为美术资源）\",\n"
        "  \"source\": \"DevDesk 内置生成\",\n"
        "  \"states\": {\n"
        "    \"idle\": [\"f0.png\", \"f1.png\", \"f2.png\", \"f3.png\", \"f4.png\", \"f5.png\"],\n"
        "    \"poked\": [\"p0.png\", \"p1.png\"]\n"
        "  }\n"
        "}\n";
    std::ofstream out(dir / "pet.json", std::ios::trunc);
    if (!out) {
        LOG_ERROR("桌宠皮肤 pet.json 写出失败: " + (dir / "pet.json").string());
        return false;
    }
    out << kPetJson;
    out.close();
    return out.good();
}

} // namespace

// ============ 解析（纯函数）============

namespace {

// 提取字符串数组（成员必须全是非空字符串；否则失败）
Result<std::vector<std::string>> parseFrameArray(const platform::json::JsonValue& value) {
    if (!value.isArray()) {
        return Result<std::vector<std::string>>::fail(ErrorCode::ValidationError,
                                                      "states.* 必须是字符串数组");
    }
    std::vector<std::string> frames;
    for (int i = 0; i < value.size(); ++i) {
        if (!value[i].isString() || value[i].asString().empty()) {
            return Result<std::vector<std::string>>::fail(
                ErrorCode::ValidationError, "帧名必须是非空字符串");
        }
        frames.push_back(value[i].asString());
    }
    if (frames.empty()) {
        return Result<std::vector<std::string>>::fail(ErrorCode::ValidationError,
                                                      "帧数组不能为空");
    }
    return Result<std::vector<std::string>>::ok(std::move(frames));
}

} // namespace

Result<PetSkin> parsePetSkinJson(const std::string& jsonText) {
    auto parsed = platform::json::parse(jsonText);
    if (parsed.isFail() || !parsed.unsafeValue().isObject()) {
        return Result<PetSkin>::fail(ErrorCode::JsonParseError,
                                     "pet.json 必须是 JSON object");
    }
    const platform::json::JsonObject obj = parsed.unsafeValue().asObject();

    PetSkin skin;
    if (obj.has("name") && obj.get("name").isString()) {
        skin.name = obj.get("name").asString();
    }
    if (obj.has("frameRate") && obj.get("frameRate").isNumber()) {
        const double rate = obj.get("frameRate").asDouble();
        if (rate >= 1.0 && rate <= 30.0) skin.frameRate = rate;
    }
    if (obj.has("frameSize") && obj.get("frameSize").isArray() &&
        obj.get("frameSize").size() == 2 &&
        obj.get("frameSize")[0].isNumber() && obj.get("frameSize")[1].isNumber()) {
        const int w = static_cast<int>(obj.get("frameSize")[0].asInt());
        const int h = static_cast<int>(obj.get("frameSize")[1].asInt());
        if (w >= 16 && w <= 512 && h >= 16 && h <= 512) {
            skin.frameWidth = w;
            skin.frameHeight = h;
        }
    }
    if (obj.has("license") && obj.get("license").isString()) {
        skin.license = obj.get("license").asString();
    }
    if (obj.has("source") && obj.get("source").isString()) {
        skin.source = obj.get("source").asString();
    }
    if (!obj.has("states") || !obj.get("states").isObject()) {
        return Result<PetSkin>::fail(ErrorCode::ValidationError,
                                     "pet.json 缺少 states 对象");
    }
    const platform::json::JsonObject states = obj.get("states").asObject();
    if (!states.has("idle")) {
        return Result<PetSkin>::fail(ErrorCode::ValidationError,
                                     "pet.json 缺少 states.idle");
    }
    auto idle = parseFrameArray(states.get("idle"));
    if (idle.isFail()) {
        return Result<PetSkin>::fail(idle.errorCode(), "states.idle: " + idle.errorMessage());
    }
    skin.idleFrames = std::move(idle.unsafeValue());
    // poked 可选；MVP 之外的状态名静默忽略（前向兼容，M2 多状态机直接吃）
    if (states.has("poked")) {
        auto poked = parseFrameArray(states.get("poked"));
        if (poked.isOk()) {
            skin.pokedFrames = std::move(poked.unsafeValue());
        }
    }
    return Result<PetSkin>::ok(std::move(skin));
}

Result<PetSkin> loadSkin(const std::filesystem::path& dir) {
    const std::filesystem::path jsonPath = dir / "pet.json";
    std::error_code ec;
    if (!std::filesystem::exists(jsonPath, ec) || ec) {
        return Result<PetSkin>::fail(ErrorCode::FileNotFound,
                                     "皮肤不存在: " + jsonPath.string());
    }
    std::ifstream file(jsonPath);
    if (!file) {
        return Result<PetSkin>::fail(ErrorCode::FileReadFailed,
                                     "皮肤读取失败: " + jsonPath.string());
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    file.close();
    auto skin = parsePetSkinJson(buffer.str());
    if (skin.isFail()) {
        return skin;
    }
    skin.unsafeValue().dir = dir;
    if (skin.unsafeValue().name.empty()) {
        skin.unsafeValue().name = dir.filename().string();
    }
    return skin;
}

Result<PetSkin> ensureDefaultSkin(const std::filesystem::path& dir) {
    // 已存在且可解析 → 直接用（用户可能整体替换过帧图）
    if (auto existing = loadSkin(dir); existing.isOk()) {
        return existing;
    }
    if (!generatePlaceholderSkin(dir)) {
        return Result<PetSkin>::fail(ErrorCode::FileWriteFailed,
                                     "占位皮肤生成失败: " + dir.string());
    }
    return loadSkin(dir);
}

std::vector<std::filesystem::path> skinRootCandidates() {
    // 生成/默认皮肤落在 petsDir()（appData，恒可写）；cwd/assets/pets 是
    // 开发期放皮肤包的只读候选（构建目录旁边）
    std::vector<std::filesystem::path> roots;
    roots.push_back(core::paths::petsDir());
    std::error_code ec;
    const std::filesystem::path devRoot = std::filesystem::current_path(ec) / "assets" / "pets";
    if (!ec) {
        roots.push_back(devRoot);
    }
    return roots;
}

std::vector<std::string> discoverSkins() {
    std::vector<std::string> names;
    std::error_code ec;
    for (const std::filesystem::path& root : skinRootCandidates()) {
        if (!std::filesystem::exists(root, ec) || ec) {
            continue;
        }
        for (const auto& entry : std::filesystem::directory_iterator(root, ec)) {
            if (ec || !entry.is_directory(ec)) continue;
            const std::string name = entry.path().filename().string();
            if (std::filesystem::exists(entry.path() / "pet.json", ec) && !ec &&
                std::find(names.begin(), names.end(), name) == names.end()) {
                names.push_back(name);
            }
        }
    }
    std::sort(names.begin(), names.end());
    return names;
}

} // namespace module::pet
