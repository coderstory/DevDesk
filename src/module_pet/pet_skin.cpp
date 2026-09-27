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

#include "core/atomic_write.h"
#include "core/logging.h"
#include "core/paths.h"
// 桥函数契约（discoverSkins 定义在本 TU）：签名漂移由该头末尾 static_assert 拦
#include "module_pet_bridge.h"
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

    // 原子写（tmp → remove → rename，core/atomic_write.h 唯一实现，审计 THS-03
    // 第二批收口）：旧的 binary+trunc 原地写中途失败会留下半截 PNG（帧永远解不
    // 出来，且原帧已被截断覆盖）；改为写全或保留旧帧。字节照旧实现逐字相同
    return core::atomicWriteFile(path, std::string(png.begin(), png.end()),
                                 "皮肤帧")
        .isOk();
}

// ============ 占位皮肤绘制（简单形状 + 1px 覆盖率抗锯齿边缘）============

struct Rgba {
    unsigned char r, g, b, a;
};

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

// 硬边实心矩形（Zzz 字形用；整数坐标全覆盖，足够占位皮肤）
void fillRect(Canvas& c, float x, float y, float w, float h, const Rgba& color) {
    for (int yy = static_cast<int>(y); yy < static_cast<int>(y + h); ++yy) {
        for (int xx = static_cast<int>(x); xx < static_cast<int>(x + w); ++xx) {
            c.blend(xx, yy, color, 1.0f);
        }
    }
}

// "Z" 字形（sleep 帧的 Zzz）：上横杠 + 阶梯斜杠 + 下横杠，占位画法
void drawZ(Canvas& c, float x, float y, float scale, const Rgba& color = kInk) {
    const float t = std::max(1.5f, 2.0f * scale);   // 笔画粗细
    const float w = 10.0f * scale;
    const float h = 10.0f * scale;
    fillRect(c, x, y, w, t, color);                 // 上横
    for (int i = 0; i < 3; ++i) {                   // 斜杠（3 级阶梯近似）
        const float step = (w - t) / 3.0f;
        const float vStep = (h - 2.0f * t) / 3.0f;
        fillRect(c, x + w - t - i * step, y + t + i * vStep, t * 1.5f, t * 1.5f, color);
    }
    fillRect(c, x, y + h - t, w, t, color);         // 下横
}

bool writeFrame(const std::filesystem::path& dir, const char* file, Canvas&& canvas) {
    const std::filesystem::path target = dir / file;
    if (!writePngRgba(target, canvas.w, canvas.h, canvas.px)) {
        LOG_ERROR("桌宠占位皮肤帧写出失败: " + target.string());
        return false;
    }
    return true;
}

// ============ 皮肤目录/pet.json 写出（各内置皮肤生成器共用）============

bool ensureSkinDir(const std::filesystem::path& dir) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        LOG_ERROR("桌宠皮肤目录创建失败: " + dir.string() + " (" + ec.message() + ")");
        return false;
    }
    return true;
}

bool writePetJson(const std::filesystem::path& dir, const char* jsonText) {
    const std::filesystem::path target = dir / "pet.json";
    // 原子写（core/atomic_write.h 唯一实现，审计 THS-03 第二批收口）：旧实现
    // trunc 原地写，中途失败会把皮肤清单截成半截 → loadSkin 解析失败 → 皮肤
    // 整个不可用（帧文件还在）；改后失败时旧 pet.json 原样保留
    if (!core::atomicWriteFile(target, jsonText, "皮肤清单").isOk()) {
        LOG_ERROR("桌宠皮肤 pet.json 写出失败: " + target.string());
        return false;
    }
    return true;
}

// ============ robot（方头机器人：天线 + 屏幕脸，typing 屏幕滚字）============

constexpr Rgba kRobotBody{0x92, 0xA0, 0xB0, 0xFF};   // 石板灰
constexpr Rgba kRobotDark{0x6E, 0x7A, 0x8A, 0xFF};   // 深一档（天线/身体/耳块）
constexpr Rgba kRobotScreen{0x18, 0x20, 0x28, 0xFF}; // 屏幕
constexpr Rgba kRobotLcd{0x62, 0xF0, 0x8C, 0xFF};    // 屏幕绿
constexpr Rgba kRobotAntenna{0xE2, 0x5B, 0x5B, 0xFF};

// 屏幕脸内容：方眼/眨眼/警报"!"/关屏（待机灯）/滚字两帧
enum class Screen { Eyes, Blink, Alert, Off, Text0, Text1 };

// 方头 + 侧耳块 + 天线 + 屏幕脸（硬边矩形——机器人理应如此）
Canvas drawRobotFrame(int size, int bob, Screen screen, bool squash) {
    Canvas c(size, size);
    const float s = size / 128.0f;
    const float headY = (36.0f + (squash ? 6.0f : 0.0f) + bob) * s;
    // 天线（杆 + 顶球，随头动）
    c.ellipse(64.0f * s, headY - 16.0f * s, 5.0f * s, 5.0f * s, kRobotAntenna);
    fillRect(c, 62.0f * s, headY - 12.0f * s, 4.0f * s, 12.0f * s, kRobotDark);
    // 头 + 侧耳块
    fillRect(c, 36.0f * s, headY, 56.0f * s, 52.0f * s, kRobotBody);
    fillRect(c, 28.0f * s, headY + 16.0f * s, 8.0f * s, 18.0f * s, kRobotDark);
    fillRect(c, 92.0f * s, headY + 16.0f * s, 8.0f * s, 18.0f * s, kRobotDark);
    // 屏幕（头内深色凹面）
    const float sx = 44.0f * s;
    const float sy = headY + 12.0f * s;
    fillRect(c, sx, sy, 40.0f * s, 28.0f * s, kRobotScreen);
    switch (screen) {
        case Screen::Eyes:  // 两个方瞳
            fillRect(c, 50.0f * s, headY + 20.0f * s, 9.0f * s, 10.0f * s, kRobotLcd);
            fillRect(c, 69.0f * s, headY + 20.0f * s, 9.0f * s, 10.0f * s, kRobotLcd);
            break;
        case Screen::Blink:  // 眨眼 = 瞳孔压扁成线
            fillRect(c, 50.0f * s, headY + 25.0f * s, 9.0f * s, 3.0f * s, kRobotLcd);
            fillRect(c, 69.0f * s, headY + 25.0f * s, 9.0f * s, 3.0f * s, kRobotLcd);
            break;
        case Screen::Alert:  // 被戳：屏幕"!"警报
            fillRect(c, 61.0f * s, headY + 17.0f * s, 6.0f * s, 11.0f * s, kRobotLcd);
            fillRect(c, 61.0f * s, headY + 32.0f * s, 6.0f * s, 4.0f * s, kRobotLcd);
            break;
        case Screen::Off:  // 睡眠：熄屏 + 待机小灯
            fillRect(c, 74.0f * s, headY + 34.0f * s, 4.0f * s, 3.0f * s, kRobotLcd);
            break;
        case Screen::Text0:  // 打字：屏幕滚字（两帧行宽错开 = 滚动感）
            fillRect(c, 48.0f * s, headY + 18.0f * s, 20.0f * s, 3.0f * s, kRobotLcd);
            fillRect(c, 48.0f * s, headY + 25.0f * s, 30.0f * s, 3.0f * s, kRobotLcd);
            fillRect(c, 48.0f * s, headY + 32.0f * s, 14.0f * s, 3.0f * s, kRobotLcd);
            break;
        case Screen::Text1:
            fillRect(c, 48.0f * s, headY + 18.0f * s, 30.0f * s, 3.0f * s, kRobotLcd);
            fillRect(c, 48.0f * s, headY + 25.0f * s, 12.0f * s, 3.0f * s, kRobotLcd);
            fillRect(c, 48.0f * s, headY + 32.0f * s, 24.0f * s, 3.0f * s, kRobotLcd);
            break;
    }
    // 身体（头下一小节）
    fillRect(c, 42.0f * s, (92.0f + bob * 0.5f) * s, 44.0f * s, 20.0f * s, kRobotDark);
    return c;
}

// idle：方眼静-轻浮-静-静-眨眼-静；poked：屏幕"!"警报 → 压扁
// sleep：熄屏 + 屏幕绿 Zzz；typing：屏幕滚字两帧交替
bool generateRobotSkin(const std::filesystem::path& dir) {
    if (!ensureSkinDir(dir)) {
        return false;
    }
    struct Spec {
        const char* file;
        int bob;
        Screen screen;
        bool squash;
    };
    const Spec frames[] = {
        {"f0.png", 0, Screen::Eyes, false},   {"f1.png", -3, Screen::Eyes, false},
        {"f2.png", 0, Screen::Eyes, false},   {"f3.png", 0, Screen::Eyes, false},
        {"f4.png", 0, Screen::Blink, false},  {"f5.png", 0, Screen::Eyes, false},
        {"p0.png", 0, Screen::Alert, false},  {"p1.png", 2, Screen::Alert, true},
    };
    for (const Spec& spec : frames) {
        if (!writeFrame(dir, spec.file,
                        drawRobotFrame(128, spec.bob, spec.screen, spec.squash))) {
            return false;
        }
    }
    for (int i = 0; i < 3; ++i) {
        Canvas c = drawRobotFrame(128, 0, Screen::Off, false);
        drawZ(c, 96.0f, 34.0f - static_cast<float>(i) * 4.0f, 0.7f + 0.2f * i, kRobotLcd);
        if (!writeFrame(dir, ("s" + std::to_string(i) + ".png").c_str(), std::move(c))) {
            return false;
        }
    }
    {
        Canvas t0 = drawRobotFrame(128, -2, Screen::Text0, false);
        Canvas t1 = drawRobotFrame(128, 1, Screen::Text1, false);
        if (!writeFrame(dir, "t0.png", std::move(t0)) ||
            !writeFrame(dir, "t1.png", std::move(t1))) {
            return false;
        }
    }
    constexpr const char* kPetJson =
        "{\n"
        "  \"name\": \"robot\",\n"
        "  \"frameSize\": [128, 128],\n"
        "  \"frameRate\": 4,\n"
        "  \"license\": \"CC0（程序生成皮肤，可整体替换为美术资源）\",\n"
        "  \"source\": \"DevDesk 内置生成\",\n"
        "  \"phrases\": [\"滴——系统正常\", \"电量 87%\", \"扫描中…\", \"待机模式已就绪\", \"执行摸鱼协议\"],\n"
        "  \"states\": {\n"
        "    \"idle\": [\"f0.png\", \"f1.png\", \"f2.png\", \"f3.png\", \"f4.png\", \"f5.png\"],\n"
        "    \"poked\": [\"p0.png\", \"p1.png\"],\n"
        "    \"sleep\": [\"s0.png\", \"s1.png\", \"s2.png\"],\n"
        "    \"typing\": [\"t0.png\", \"t1.png\"]\n"
        "  }\n"
        "}\n";
    return writePetJson(dir, kPetJson);
}

// ============ ghost（小幽灵：半透明淡紫，波纹下摆飘动，sleep 渐隐）============

constexpr Rgba kGhostBody{0xE8, 0xE4, 0xF8, 0xD8};  // 半透明淡紫

// 拱顶 + 正弦波纹下摆（phase 驱动飘动）；alphaScale < 1 时整体渐隐（sleep）
Canvas drawGhostFrame(int size, int bob, Eyes eyes, Mouth mouth, float phase,
                      float alphaScale = 1.0f) {
    Canvas c(size, size);
    const float s = size / 128.0f;
    Rgba body = kGhostBody;
    body.a = static_cast<unsigned char>(body.a * alphaScale);
    // 拱顶 + 下摆：逐列竖条（28..99），顶 = 半圆拱，底 = 正弦波纹
    const float hemBase = (98.0f + bob) * s;
    for (int gx = 28; gx < 100; ++gx) {
        const float dx = (gx + 0.5f - 64.0f) / 36.0f;
        if (dx * dx > 1.0f) continue;
        const float domeTop = (42.0f + bob - 36.0f * std::sqrt(1.0f - dx * dx)) * s;
        const float hem = 6.0f * std::sin(gx * 0.3f + phase) * s;
        fillRect(c, gx * s, domeTop, s, hemBase + hem - domeTop, body);
    }
    // 眼睛 + 嘴
    const float eyeY = (62.0f + bob) * s;
    if (eyes == Eyes::Open) {
        c.ellipse(54.0f * s, eyeY, 5.0f * s, 6.0f * s, kInk);
        c.ellipse(74.0f * s, eyeY, 5.0f * s, 6.0f * s, kInk);
    } else if (eyes == Eyes::Closed) {
        c.ellipse(54.0f * s, eyeY, 5.0f * s, 1.5f * s, kInk);
        c.ellipse(74.0f * s, eyeY, 5.0f * s, 1.5f * s, kInk);
    } else {
        c.ellipse(54.0f * s, eyeY, 7.0f * s, 8.0f * s, kInk);
        c.ellipse(74.0f * s, eyeY, 7.0f * s, 8.0f * s, kInk);
    }
    if (mouth == Mouth::Smile) {
        c.ellipse(64.0f * s, (76.0f + bob) * s, 7.0f * s, 2.5f * s, kInk);
    } else {
        c.ellipse(64.0f * s, (78.0f + bob) * s, 5.0f * s, 5.0f * s, kInk);  // O 嘴
    }
    return c;
}

// idle：波纹相位逐帧推进（下摆飘动）+ 眨眼；poked：瞪眼 O 嘴 → 下坠闭眼
// sleep：整体逐帧渐隐 + Zzz；typing：上下漂浮（bob -4/+2）张嘴
bool generateGhostSkin(const std::filesystem::path& dir) {
    if (!ensureSkinDir(dir)) {
        return false;
    }
    struct Spec {
        const char* file;
        int bob;
        Eyes eyes;
        Mouth mouth;
        float phase;
    };
    const Spec frames[] = {
        {"f0.png", 0, Eyes::Open, Mouth::Smile, 0.0f},
        {"f1.png", -3, Eyes::Open, Mouth::Smile, 1.05f},
        {"f2.png", 0, Eyes::Open, Mouth::Smile, 2.09f},
        {"f3.png", 0, Eyes::Open, Mouth::Smile, 3.14f},
        {"f4.png", 0, Eyes::Closed, Mouth::Smile, 4.19f},
        {"f5.png", 0, Eyes::Open, Mouth::Smile, 5.24f},
        {"p0.png", 0, Eyes::Wide, Mouth::Open, 0.0f},
        {"p1.png", 4, Eyes::Closed, Mouth::Open, 1.05f},
    };
    for (const Spec& spec : frames) {
        if (!writeFrame(dir, spec.file,
                        drawGhostFrame(128, spec.bob, spec.eyes, spec.mouth, spec.phase))) {
            return false;
        }
    }
    for (int i = 0; i < 3; ++i) {
        Canvas c = drawGhostFrame(128, 2, Eyes::Closed, Mouth::Smile,
                                  1.05f * i, 1.0f - 0.22f * i);  // 逐帧渐隐
        drawZ(c, 90.0f, 40.0f - static_cast<float>(i) * 4.0f, 0.8f + 0.25f * i);
        drawZ(c, 104.0f, 26.0f - static_cast<float>(i) * 3.0f, 0.5f + 0.2f * i);
        if (!writeFrame(dir, ("s" + std::to_string(i) + ".png").c_str(), std::move(c))) {
            return false;
        }
    }
    {
        Canvas t0 = drawGhostFrame(128, -4, Eyes::Open, Mouth::Open, 0.0f);
        Canvas t1 = drawGhostFrame(128, 2, Eyes::Open, Mouth::Open, 1.57f);
        if (!writeFrame(dir, "t0.png", std::move(t0)) ||
            !writeFrame(dir, "t1.png", std::move(t1))) {
            return false;
        }
    }
    constexpr const char* kPetJson =
        "{\n"
        "  \"name\": \"ghost\",\n"
        "  \"frameSize\": [128, 128],\n"
        "  \"frameRate\": 4,\n"
        "  \"license\": \"CC0（程序生成皮肤，可整体替换为美术资源）\",\n"
        "  \"source\": \"DevDesk 内置生成\",\n"
        "  \"phrases\": [\"呼~…\", \"别怕，我很友好\", \"漂浮中…\", \"呜咿—\", \"今晚月色不错\"],\n"
        "  \"states\": {\n"
        "    \"idle\": [\"f0.png\", \"f1.png\", \"f2.png\", \"f3.png\", \"f4.png\", \"f5.png\"],\n"
        "    \"poked\": [\"p0.png\", \"p1.png\"],\n"
        "    \"sleep\": [\"s0.png\", \"s1.png\", \"s2.png\"],\n"
        "    \"typing\": [\"t0.png\", \"t1.png\"]\n"
        "  }\n"
        "}\n";
    return writePetJson(dir, kPetJson);
}

} // namespace

// ============ 解析（纯函数）============

namespace {

// 审计 [28]：帧名会直接拼进皮肤目录（PetSkin::framePath = dir / 帧名），恶意
// 皮肤包可用 "../.." 或绝对路径加载皮肤目录外的任意 PNG（有限任意文件读）。
// 只放行相对文件名：'/'、'\\'（兼收 Unix/Windows 绝对路径与 UNC）、':'（盘符
// 前缀 "C:"、ADS 一律拒）任一出现即拒；".." 子串即拒（穿越片段；单个 '.' 是
// PNG 扩展名必须放行）；空串由上游非空校验兜底
bool isSafeFrameName(const std::string& name) {
    if (name.find_first_of("/\\:") != std::string::npos) {
        return false;
    }
    return name.find("..") == std::string::npos;
}

// 提取字符串数组（成员必须全是非空字符串；否则失败）。asFrameNames=true 时
// 帧名过 isSafeFrameName（states.idle/poked/sleep/typing 用）；false 供
// phrases 等纯文本数组复用——短语是展示文本不是路径，不收字符集限制
Result<std::vector<std::string>> parseFrameArray(const platform::json::JsonValue& value,
                                                 bool asFrameNames) {
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
        const std::string& name = value[i].asString();
        if (asFrameNames && !isSafeFrameName(name)) {
            return Result<std::vector<std::string>>::fail(
                ErrorCode::ValidationError,
                "帧名只许相对文件名（含 / \\ : 或 .. 被拒）: " + name);
        }
        frames.push_back(name);
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
    if (parsed.isFail() || !parsed.value().isObject()) {
        return Result<PetSkin>::fail(ErrorCode::JsonParseError,
                                     "pet.json 必须是 JSON object");
    }
    const platform::json::JsonObject obj = parsed.value().asObject();

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
    auto idle = parseFrameArray(states.get("idle"), /*asFrameNames=*/true);
    if (idle.isFail()) {
        return Result<PetSkin>::fail(idle.errorCode(), "states.idle: " + idle.errorMessage());
    }
    skin.idleFrames = std::move(idle.value());
    // 可选状态/字段（M2）：sleep/typing 帧缺失或非法一律静默忽略——
    // 状态机各自回落 idle，老皮肤包零迁移（poked 同款容错）
    if (states.has("poked")) {
        auto poked = parseFrameArray(states.get("poked"), /*asFrameNames=*/true);
        if (poked.isOk()) {
            skin.pokedFrames = std::move(poked.value());
        }
    }
    if (states.has("sleep")) {
        auto sleep = parseFrameArray(states.get("sleep"), /*asFrameNames=*/true);
        if (sleep.isOk()) {
            skin.sleepFrames = std::move(sleep.value());
        }
    }
    if (states.has("typing")) {
        auto typing = parseFrameArray(states.get("typing"), /*asFrameNames=*/true);
        if (typing.isOk()) {
            skin.typingFrames = std::move(typing.value());
        }
    }
    if (obj.has("phrases")) {
        auto phrases = parseFrameArray(obj.get("phrases"), /*asFrameNames=*/false);
        if (phrases.isOk()) {
            skin.phrases = std::move(phrases.value());
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
    skin.value().dir = dir;
    if (skin.value().name.empty()) {
        skin.value().name = dir.filename().string();
    }
    return skin;
}

namespace {

// 内置皮肤注册表：名字 + 生成器（源标记统一 "DevDesk 内置生成"）
struct BuiltinSkinEntry {
    const char* name;
    bool (*generate)(const std::filesystem::path&);
};

constexpr BuiltinSkinEntry kBuiltinSkins[] = {
    {"robot", generateRobotSkin},
    {"ghost", generateGhostSkin},
};

// ensureDefaultSkin 主体提炼，多皮肤共用：目录已存在且可解析时直接加载；
// 但内置皮肤缺 M2 帧时重新生成升级（用户替换过的皮肤不动）
Result<PetSkin> ensureGeneratedSkin(const std::filesystem::path& dir,
                                    bool (*generate)(const std::filesystem::path&)) {
    if (auto existing = loadSkin(dir); existing.isOk()) {
        const PetSkin& s = existing.value();
        const bool userReplaced = s.source != "DevDesk 内置生成";
        const bool hasM2 =
            !s.sleepFrames.empty() && !s.typingFrames.empty() && !s.phrases.empty();
        if (userReplaced || hasM2) {
            return existing;
        }
        LOG_INFO("桌宠内置皮肤为旧版（缺 M2 帧），重新生成升级: " + dir.string());
    }
    if (!generate(dir)) {
        return Result<PetSkin>::fail(ErrorCode::FileWriteFailed,
                                     "内置皮肤生成失败: " + dir.string());
    }
    return loadSkin(dir);
}

} // namespace

Result<PetSkin> ensureDefaultSkin(const std::filesystem::path& dir) {
    // 默认皮肤 = 内置 robot（kDefaultSkinName）；原 clay-cat 占位生成器已删，
    // 兜底语义不变：目录缺皮肤/缺 M2 帧时重新生成 robot
    return ensureGeneratedSkin(dir, generateRobotSkin);
}

const std::vector<std::string>& builtinSkinNames() {
    static const std::vector<std::string> kNames = [] {
        std::vector<std::string> names;
        for (const BuiltinSkinEntry& e : kBuiltinSkins) {
            names.push_back(e.name);
        }
        return names;
    }();
    return kNames;
}

std::vector<std::string> ensureBuiltinSkins(const std::filesystem::path& root) {
    std::vector<std::string> ok;
    for (const BuiltinSkinEntry& e : kBuiltinSkins) {
        if (ensureGeneratedSkin(root / e.name, e.generate).isOk()) {
            ok.push_back(e.name);
        }
    }
    return ok;
}

std::vector<std::filesystem::path> skinRootCandidates() {
    // 皮肤根**纯派生**（审计 CP-04）：数据根 → 安装目录，两条都在此列齐，
    // 这里不读任何环境变量。旧实现在 DEVDESK_PETS_DIR 已设时直接丢掉安装
    // 目录候选——环境变量因此参与了**产品行为**（隔离环境下"看不见"随包
    // 皮肤），生产代码长期留着 getenv 分支；测试隔离改由数据根统一承担
    // （DEVDESK_DATA_ROOT / DEVDESK_PETS_DIR 只搬 petsDir()，不再改候选形态）。
    //
    //   1) petsDir()（appData，恒可写）：生成/默认皮肤落这里，也是换肤写盘处
    //   2) exe 旁 assets/pets：随包分发的只读美术皮肤（安装包/DMG 里与可执行
    //      文件同级；开发期 = 构建目录，由顶层 CMake POST_BUILD 拷贝）
    std::vector<std::filesystem::path> roots;
    roots.push_back(core::paths::petsDir());
    // exe 目录解析失败时 executableDir() 返回空 path，此时必须跳过该候选：
    // 空路径与 "assets/pets" 拼接会退化成相对 cwd 的路径
    const std::filesystem::path exeDir = core::paths::executableDir();
    if (!exeDir.empty()) {
        roots.push_back(exeDir / "assets" / "pets");
    }
    return roots;
}

std::vector<std::string> discoverSkins() {
    // 皮肤名合法字符与 PetConfigStore::sanitized 一致（会拼回路径）
    static const std::string kValid =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_";
    std::vector<std::string> names;
    for (const std::filesystem::path& root : skinRootCandidates()) {
        std::error_code ec;
        if (!std::filesystem::exists(root, ec) || ec) continue;
        for (std::filesystem::directory_iterator it(root, ec), end; !ec && it != end;
             it.increment(ec)) {
            if (!it->is_directory(ec) || ec) continue;
            const std::string name = it->path().filename().string();
            if (name.empty() || name.find_first_not_of(kValid) != std::string::npos) {
                continue;  // 非法名不进清单（路径安全）
            }
            std::error_code ec2;
            if (!std::filesystem::exists(it->path() / "pet.json", ec2) || ec2) continue;
            if (std::find(names.begin(), names.end(), name) == names.end()) {
                names.push_back(name);
            }
        }
    }
    return names;
}

} // namespace module::pet
