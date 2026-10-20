#include "fx/CosmeticsRecolor.hpp"

#include "core/Log.hpp"
#include "core/PrivateAccess.hpp"
#include "fx/PlayerRecolor.hpp"
#include "looks/Archive.hpp"
#include "looks/Inventory.hpp"

#include "JSystem/J3DGraphAnimator/J3DModelData.h"
#include "JSystem/J3DGraphBase/J3DTexture.h"
#include "JSystem/JUtility/JUTNameTab.h"

#include <fmt/format.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <exception>
#include <iterator>
#include <map>
#include <thread>

namespace twili {
namespace {

using looks::CosmeticsVarId;

// What the cosmetics mod colours: one texture of one puppet model per row (a game fact, our table).
struct CosmeticsTarget {
    uint8_t var;
    CosmeticsModel model;
    const char* arc;      // /res/Object/<arc>.arc, for the disc's own key
    const char* bmd;      // the model's file in it
    const char* texture;  // its TEX1 name
};

using M = CosmeticsModel;
constexpr CosmeticsTarget kTargets[] = {
    {looks::kHerosTunicCap, M::KokiriHead, "Kmdl", "al_head.bmd", "al_cap"},
    {looks::kHerosTunicTorso, M::KokiriBody, "Kmdl", "al.bmd", "al_upbody"},
    {looks::kHerosTunicSkirt, M::KokiriBody, "Kmdl", "al.bmd", "al_lowbody"},
    {looks::kZoraArmorCap, M::ZoraHead, "Zmdl", "zl_head.bmd", "zl_cap"},
    {looks::kZoraArmorHelmet, M::ZoraHead, "Zmdl", "zl_head.bmd", "zl_helmet"},
    {looks::kZoraArmorTorso, M::ZoraBody, "Zmdl", "zl.bmd", "zl_armor"},
    {looks::kZoraArmorTorso, M::ZoraBody, "Zmdl", "zl.bmd", "zl_armL"},
    {looks::kZoraArmorScales, M::ZoraBody, "Zmdl", "zl.bmd", "zl_body"},
    {looks::kZoraArmorFlippers, M::ZoraBody, "Zmdl", "zl.bmd", "zl_boots"},
    {looks::kWoodenSword, M::KokiriSword, "Kmdl", "al_swb.bmd", "al_SWB"},
    {looks::kWoodenSword, M::ZoraSword, "Zmdl", "al_swb.bmd", "al_SWB"},
    {looks::kWoodenSword, M::MagicSword, "Mmdl", "al_swb.bmd", "al_SWB"},
    {looks::kWoodenSword, M::CasualSword, "Bmdl", "al_swb.bmd", "al_SWB"},
    {looks::kOrdonSwordBlade, M::SwordA, "Alink", "al_swa.bmd", "al_SWA"},
    {looks::kOrdonSwordHandle, M::SwordA, "Alink", "al_swa.bmd", "al_SWgripA"},
    {looks::kMasterSwordBlade, M::SwordM, "Alink", "al_swm.bmd", "al_SWM"},
    {looks::kMasterSwordHandle, M::SwordM, "Alink", "al_swm.bmd", "al_SWgripM"},
    {looks::kBoomerang, M::Boomerang, "Alink", "al_boom.bmd", "L_al_boom00"},
    {looks::kIronBoots, M::KokiriBoots, "Kmdl", "al_bootsh.bmd", "al_bootsH"},
    {looks::kIronBoots, M::ZoraBoots, "Zmdl", "al_bootsh.bmd", "al_bootsH"},
    {looks::kIronBoots, M::MagicBoots, "Mmdl", "al_bootsh.bmd", "al_bootsH"},
    {looks::kIronBoots, M::CasualBoots, "Bmdl", "al_bootsh.bmd", "al_bootsH"},
    {looks::kSpinner, M::Spinner, "Alink", "al_sp.bmd", "al_SP"},
    {looks::kLinkHair, M::KokiriHead, "Kmdl", "al_head.bmd", "al_hair"},
    {looks::kLinkHair, M::MagicHead, "Mmdl", "ml_head.bmd", "al_hair"},
    {looks::kLinkHair, M::CasualHead, "Bmdl", "bl_head.bmd", "bl_hair"},
    {looks::kWolfLink, M::Wolf, "Wmdl", "wl.bmd", "wl_body"},
    {looks::kWolfLink, M::Wolf, "Wmdl", "wl.bmd", "wl_eye.1"},
    {looks::kWolfLink, M::Wolf, "Wmdl", "wl.bmd", "wl_eye.2"},
    {looks::kWolfLink, M::Wolf, "Wmdl", "wl.bmd", "wl_eye.3"},
    {looks::kWolfLink, M::Wolf, "Wmdl", "wl.bmd", "wl_eye.4"},
    {looks::kWolfLink, M::Wolf, "Wmdl", "wl.bmd", "wl_eye.5"},
    {looks::kEpona, M::Horse, "Horse", "hs.bmd", "hs_body"},
    {looks::kEpona, M::Horse, "Horse", "hs.bmd", "hs_eye.1"},
    {looks::kEpona, M::Horse, "Horse", "hs.bmd", "hs_eye.2"},
    {looks::kEpona, M::Horse, "Horse", "hs.bmd", "hs_eye.3"},
};
constexpr size_t kTargetCount = std::size(kTargets);

// A puppet changes in one tick: aurora rebuilds its texture cache once a frame, however many land.
constexpr int kEntriesPerTick = 64;
constexpr size_t kMaxArchiveBytes = 64u << 20;
constexpr size_t kMaxFileBytes = 16u << 20;

// --- the disc's own texture keys

struct VanillaTex {
    uint16_t index;
    uint8_t format;
    uint16_t width, height;
    uint8_t mips;
    uint64_t hash;
};
struct TargetKeys {
    bool known = false;  // false: its file could not be read, so nothing is recoloured
    std::vector<VanillaTex> tex;
};

enum KeysState : int { kKeysIdle, kKeysRunning, kKeysReady, kKeysUnavailable, kKeysFailed };
std::atomic<int> sKeysState{kKeysIdle};
std::thread sKeysThread;
// Written by the worker before kKeysReady or kKeysFailed.
std::vector<TargetKeys> sKeys;
std::string sKeysSummary;
size_t sKeysFound = 0;
bool sKeysLogged = false;

void buildKeysOrThrow() {
    const auto start = std::chrono::steady_clock::now();
    std::vector<TargetKeys> keys(kTargetCount);
    std::map<std::string, std::vector<uint8_t>> arcs;
    struct Model {
        std::vector<uint8_t> bytes;
        std::optional<std::vector<looks::Tex1Entry>> textures;
    };
    std::map<std::string, Model> models;
    size_t known = 0, found = 0, textures = 0;
    std::string missing;
    for (size_t t = 0; t < kTargetCount; t++) {
        const CosmeticsTarget& target = kTargets[t];
        auto arc = arcs.find(target.arc);
        if (arc == arcs.end()) {
            std::vector<uint8_t> bytes;
            const std::string path = fmt::format("/res/Object/{}.arc", target.arc);
            if (auto raw = looks::readBaseDiscFile(path.c_str())) {
                if (!looks::isYaz0(*raw)) {
                    bytes = std::move(*raw);
                } else if (auto decoded = looks::yaz0Decode(*raw, kMaxArchiveBytes)) {
                    bytes = std::move(*decoded);
                }
            }
            arc = arcs.emplace(target.arc, std::move(bytes)).first;
        }
        const std::string modelKey = fmt::format("{}/{}", target.arc, target.bmd);
        auto model = models.find(modelKey);
        if (model == models.end()) {
            Model m;
            if (auto bmd = looks::rarcRead(arc->second, target.bmd, kMaxFileBytes)) {
                m.bytes = std::move(*bmd);
                m.textures = looks::bmdTextures(m.bytes);
            }
            model = models.emplace(modelKey, std::move(m)).first;
        }
        if (model->second.textures) {
            keys[t].known = true;
            known++;
            const auto& list = *model->second.textures;
            for (size_t i = 0; i < list.size(); i++) {
                const looks::Tex1Entry& x = list[i];
                if (x.name != target.texture || x.palette) {
                    continue;
                }
                const uint64_t hash =
                    looks::hashBytes(model->second.bytes.data() + x.imageOffset, x.imageBytes);
                keys[t].tex.push_back(
                    {static_cast<uint16_t>(i), x.format, x.width, x.height, x.mipCount, hash});
                textures++;
            }
        }
        if (keys[t].tex.empty()) {
            missing += fmt::format(" {}/{}:{}{}", target.arc, target.bmd, target.texture,
                keys[t].known ? "" : " (unreadable)");
        } else {
            found++;
        }
    }
    sKeys = std::move(keys);
    sKeysFound = found;
    sKeysSummary = fmt::format("{} of {} targets found, {} files read, {} textures, {:.0f} ms{}{}",
        found, kTargetCount, known, textures,
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count(),
        missing.empty() ? "" : "; missing:", missing);
    sKeysState.store(kKeysReady, std::memory_order_release);
}

// An exception here would end the game: no keys then, so nothing is recoloured.
void buildKeys() {
    try {
        buildKeysOrThrow();
        return;
    } catch (const std::exception& e) {
        try {
            sKeysSummary = e.what();
        } catch (...) {
        }
    } catch (...) {
    }
    sKeysState.store(kKeysFailed, std::memory_order_release);
}

// 1 the disc's bytes, 0 changed or not checkable, -1 not known yet.
int8_t vanillaMatch(uint8_t target, uint16_t texNo, uint8_t format, uint16_t width,
    uint16_t height, uint8_t mips, uint64_t hash) {
    const int state = sKeysState.load(std::memory_order_acquire);
    if (state == kKeysUnavailable || state == kKeysFailed) {
        return 0;
    }
    if (state != kKeysReady) {
        return -1;
    }
    const TargetKeys& keys = sKeys[target];
    if (!keys.known) {
        return 0;
    }
    for (const VanillaTex& v : keys.tex) {
        if (v.index == texNo && v.format == format && v.width == width && v.height == height &&
            v.mips == mips && v.hash == hash)
        {
            return 1;
        }
    }
    return 0;
}

// --- our recolour

uint16_t be16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] << 8 | p[1]);
}

void putBe16(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v >> 8);
    p[1] = static_cast<uint8_t>(v & 0xFF);
}

Rgb rgbOf(uint32_t color) {
    return {static_cast<float>((color >> 16) & 0xFF) / 255.0f,
            static_cast<float>((color >> 8) & 0xFF) / 255.0f,
            static_cast<float>(color & 0xFF) / 255.0f};
}

float luma(const Rgb& c) {
    return 0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b;
}

// RGB5A3: RGB555 with bit 15 set, else ARGB3444.
Rgb from5A3(uint16_t v, float& alpha) {
    if (v & 0x8000) {
        alpha = 1.0f;
        return {static_cast<float>((v >> 10) & 31) / 31.0f, static_cast<float>((v >> 5) & 31) / 31.0f,
                static_cast<float>(v & 31) / 31.0f};
    }
    alpha = static_cast<float>((v >> 12) & 7) / 7.0f;
    return {static_cast<float>((v >> 8) & 15) / 15.0f, static_cast<float>((v >> 4) & 15) / 15.0f,
            static_cast<float>(v & 15) / 15.0f};
}

uint16_t to5A3(const Rgb& c, float alpha) {
    const int a = quantize(alpha, 7);
    if (a == 7) {
        return static_cast<uint16_t>(0x8000 | quantize(c.r, 31) << 10 | quantize(c.g, 31) << 5 |
                                     quantize(c.b, 31));
    }
    return static_cast<uint16_t>(a << 12 | quantize(c.r, 15) << 8 | quantize(c.g, 15) << 4 |
                                 quantize(c.b, 15));
}

struct MeanRgb {
    double r = 0, g = 0, b = 0;
    uint32_t n = 0;
    void add(const Rgb& c) {
        r += c.r;
        g += c.g;
        b += c.b;
        n++;
    }
    Hsv hsv() const {
        return n == 0 ? Hsv{} :
                        toHsv({static_cast<float>(r / n), static_cast<float>(g / n),
                            static_cast<float>(b / n)});
    }
};

bool recolorable(uint8_t format) {
    switch (format) {
    case GX_TF_I8:
    case GX_TF_IA8:
    case GX_TF_RGB565:
    case GX_TF_RGB5A3:
    case GX_TF_RGBA8:
    case GX_TF_CMPR:
        return true;
    default:
        return false;
    }
}

// The texels in `color`; I8 and IA8 come out as RGB5A3 (same size or larger, their alpha kept).
bool recolorTexels(uint8_t format, uint16_t width, uint16_t height, uint8_t mips,
    const uint8_t* src, uint32_t bytes, uint32_t color, std::vector<uint8_t>& out,
    uint32_t& outFormat, Hsv& mean) {
    const Rgb target = rgbOf(color);
    MeanRgb acc;
    outFormat = format;
    switch (format) {
    case GX_TF_CMPR:
        out.assign(src, src + bytes);
        for (uint32_t i = 0; i + 8 <= bytes; i += 8) {
            const uint16_t c0 = be16(src + i), c1 = be16(src + i + 2);
            const Rgb n0 = cosmeticsRgb(from565(c0), target), n1 = cosmeticsRgb(from565(c1), target);
            acc.add(n0);
            acc.add(n1);
            storeCmprBlock(out.data() + i, to565(n0), to565(n1), src + i + 4, c0 > c1);
        }
        break;
    case GX_TF_RGB565:
        out.assign(src, src + bytes);
        for (uint32_t i = 0; i + 2 <= bytes; i += 2) {
            const Rgb n = cosmeticsRgb(from565(be16(src + i)), target);
            acc.add(n);
            putBe16(out.data() + i, to565(n));
        }
        break;
    case GX_TF_RGB5A3:
        out.assign(src, src + bytes);
        for (uint32_t i = 0; i + 2 <= bytes; i += 2) {
            float alpha = 1.0f;
            const Rgb n = cosmeticsRgb(from5A3(be16(src + i), alpha), target);
            if (alpha >= 0.5f) acc.add(n);
            putBe16(out.data() + i, to5A3(n, alpha));
        }
        break;
    case GX_TF_RGBA8:
        out.assign(src, src + bytes);
        // 4x4 tiles of 64 bytes: 16 AR pairs, then 16 GB pairs.
        for (uint32_t t = 0; t + 64 <= bytes; t += 64) {
            for (uint32_t p = 0; p < 16; p++) {
                uint8_t* ar = out.data() + t + p * 2;
                uint8_t* gb = out.data() + t + 32 + p * 2;
                const Rgb n = cosmeticsRgb({ar[1] / 255.0f, gb[0] / 255.0f, gb[1] / 255.0f}, target);
                if (ar[0] >= 0x80) acc.add(n);
                ar[1] = static_cast<uint8_t>(quantize(n.r, 255));
                gb[0] = static_cast<uint8_t>(quantize(n.g, 255));
                gb[1] = static_cast<uint8_t>(quantize(n.b, 255));
            }
        }
        break;
    case GX_TF_IA8:
        // The same 4x4 tiles of 16 bits: alpha then intensity.
        out.assign(src, src + bytes);
        outFormat = GX_TF_RGB5A3;
        for (uint32_t i = 0; i + 2 <= bytes; i += 2) {
            const float alpha = src[i] / 255.0f, l = src[i + 1] / 255.0f;
            const Rgb n = cosmeticsRgb({l, l, l}, target);
            if (src[i] >= 0x80) acc.add(n);
            putBe16(out.data() + i, to5A3(n, alpha));
        }
        break;
    case GX_TF_I8: {
        // 8x4 tiles of bytes into 4x4 tiles of RGB5A3; GX samples I8 as (I, I, I, I).
        outFormat = GX_TF_RGB5A3;
        out.assign(looks::gxImageBytes(GX_TF_RGB5A3, width, height, mips), 0);
        uint32_t inAt = 0, outAt = 0, w = width, h = height;
        for (uint8_t level = 0; level < mips; level++) {
            const uint32_t inTilesX = (w + 7) / 8, outTilesX = (w + 3) / 4;
            const uint32_t inBytes = inTilesX * ((h + 3) / 4) * 32;
            const uint32_t outBytes = outTilesX * ((h + 3) / 4) * 32;
            if (inAt + inBytes > bytes || outAt + outBytes > out.size()) {
                return false;
            }
            for (uint32_t y = 0; y < h; y++) {
                for (uint32_t x = 0; x < w; x++) {
                    const uint8_t i8 =
                        src[inAt + ((y / 4) * inTilesX + x / 8) * 32 + (y % 4) * 8 + x % 8];
                    const float l = i8 / 255.0f;
                    const Rgb n = cosmeticsRgb({l, l, l}, target);
                    if (i8 >= 0x80) acc.add(n);
                    putBe16(out.data() + outAt + ((y / 4) * outTilesX + x / 4) * 32 +
                                ((y % 4) * 4 + x % 4) * 2,
                        to5A3(n, l));
                }
            }
            inAt += inBytes;
            outAt += outBytes;
            w = std::max(1u, w >> 1);
            h = std::max(1u, h >> 1);
        }
        break;
    }
    default:
        return false;
    }
    mean = acc.hsv();
    return true;
}

// --- Midna's hair

// d_a_midna.cpp's statics: palette index 0.
constexpr GXColorS10 kHairNormal = {0x50, 0x00, 0x00, 0x00};
constexpr GXColor kHairNormalK = {0xB4, 0x87, 0x00, 0x00};
constexpr GXColor kHairNormalK2 = {0x00, 0xC3, 0xC3, 0x00};
constexpr GXColorS10 kHairBig = {0xFF, 0x64, 0x78, 0x00};
constexpr GXColor kHairBigK = {0x1E, 0x00, 0x00, 0x00};
constexpr GXColor kHairLightNormalK = {0xFF, 0xDC, 0x00, 0x00};
constexpr GXColor kHairLightNormalK2 = {0x00, 0xC3, 0xEB, 0x00};
constexpr GXColorS10 kHairLightBig = {0xFF, 0x78, 0x00, 0x00};
constexpr GXColor kHairLightBigK2 = {0xAA, 0xFF, 0xC3, 0x00};

// The cosmetics mod's hair options 1-9 (Pink, Red, Yellow, Green, Blue, Purple, Brown, White,
// Black), made by moving the game's own colours: the base turns about its gold, the tips about
// their cyan, so each keeps its glow from dark to light.
enum class HairShift : uint8_t { Hue, Brown, White, Black };
struct HairOption {
    HairShift shift;
    float hue;
};
constexpr HairOption kHairOptions[9] = {
    {HairShift::Hue, 320.0f}, {HairShift::Hue, 0.0f},   {HairShift::Hue, 55.0f},
    {HairShift::Hue, 125.0f}, {HairShift::Hue, 220.0f}, {HairShift::Hue, 275.0f},
    {HairShift::Brown, 25.0f}, {HairShift::White, 0.0f}, {HairShift::Black, 0.0f},
};
constexpr float kHairBaseHue = 45.0f;
constexpr float kHairTipsHue = 180.0f;

Rgb shiftHair(const Rgb& c, uint32_t index, float refHue) {
    const HairOption& option = kHairOptions[index - 1];
    Hsv h = toHsv(c);
    const float turned = std::fmod(h.h + option.hue - refHue + 720.0f, 360.0f);
    switch (option.shift) {
    case HairShift::Hue:
        h.h = turned;
        break;
    case HairShift::Brown:
        h.h = turned;
        h.s *= 0.8f;
        h.v *= 0.45f;
        break;
    case HairShift::White:
        h.s *= 0.1f;
        h.v = 0.6f + 0.4f * h.v;
        break;
    case HairShift::Black:
        h.s *= 0.4f;
        h.v *= 0.15f;
        break;
    }
    return fromHsv(h);
}

uint8_t channel(float v) {
    return static_cast<uint8_t>(quantize(v, 255));
}

GXColor hairColor(const GXColor& c, uint32_t index, float refHue) {
    if (index == 0) {
        return c;
    }
    const Rgb n = shiftHair({c.r / 255.0f, c.g / 255.0f, c.b / 255.0f}, index, refHue);
    return {channel(n.r), channel(n.g), channel(n.b), c.a};
}

GXColorS10 hairColor(const GXColorS10& c, uint32_t index, float refHue) {
    if (index == 0) {
        return c;
    }
    const auto unit = [](s16 v) { return std::clamp(v / 255.0f, 0.0f, 1.0f); };
    const Rgb n = shiftHair({unit(c.r), unit(c.g), unit(c.b)}, index, refHue);
    return {static_cast<s16>(channel(n.r)), static_cast<s16>(channel(n.g)),
            static_cast<s16>(channel(n.b)), c.a};
}

std::string probeName(const CosmeticsTexState s) {
    return s == CosmeticsTexState::Recolor ? "recolour" : s == CosmeticsTexState::Block ? "block" : "none";
}

}  // namespace

// The W3C compositing "overlay" of the colour over the texel's luma (Rec. 709 weights).
Rgb cosmeticsRgb(const Rgb& texel, const Rgb& color) {
    const float l = std::clamp(luma(texel), 0.0f, 1.0f);
    const auto overlay = [l](float c) {
        return l <= 0.5f ? 2.0f * l * c : 1.0f - 2.0f * (1.0f - l) * (1.0f - c);
    };
    return {overlay(color.r), overlay(color.g), overlay(color.b)};
}

void warmUpCosmeticsKeys() {
    const int state = sKeysState.load(std::memory_order_acquire);
    if (state == kKeysReady && !sKeysLogged) {
        sKeysLogged = true;
        TwiliLog.info("[cosmetics] vanilla texture keys: {}", sKeysSummary);
    }
    if (state == kKeysFailed && !sKeysLogged) {
        sKeysLogged = true;
        TwiliLog.warn("[cosmetics] reading the disc's textures failed ({}): teammates' cosmetics "
                      "are not shown", sKeysSummary.empty() ? "unknown error" : sKeysSummary);
    }
    if (state != kKeysIdle) {
        return;
    }
    if (!looks::available()) {
        sKeysState.store(kKeysUnavailable, std::memory_order_release);
        TwiliLog.info("[cosmetics] no ModsService: teammates' cosmetics are not shown, as nothing "
                      "can be checked against the disc");
        return;
    }
    sKeysState.store(kKeysRunning, std::memory_order_release);
    sKeysThread = std::thread(buildKeys);
}

void shutdownCosmeticsKeys() {
    if (sKeysThread.joinable()) {
        sKeysThread.join();
    }
}

bool cosmeticsReplaceSet(const looks::CosmeticsValues* shown, RecolorSetId set) {
    if (shown == nullptr) {
        return false;
    }
    switch (set) {
    case RecolorSetId::Kokiri:
        return shown->has(looks::kHerosTunicCap) || shown->has(looks::kHerosTunicTorso) ||
               shown->has(looks::kHerosTunicSkirt);
    case RecolorSetId::Zora:
        return shown->has(looks::kZoraArmorCap) || shown->has(looks::kZoraArmorHelmet) ||
               shown->has(looks::kZoraArmorTorso) || shown->has(looks::kZoraArmorScales) ||
               shown->has(looks::kZoraArmorFlippers);
    case RecolorSetId::Wolf:
        return shown->has(looks::kWolfLink);
    default:
        return false;
    }
}

bool cosmeticsReplaceHorse(const looks::CosmeticsValues* shown) {
    return shown != nullptr && shown->has(looks::kEpona);
}

MidnaHairColours midnaHairColours(uint32_t base, uint32_t tips) {
    base = base <= 9 ? base : 0;
    tips = tips <= 9 ? tips : 0;
    return {
        .normal = hairColor(kHairNormal, base, kHairBaseHue),
        .big = hairColor(kHairBig, base, kHairBaseHue),
        .lightBig = hairColor(kHairLightBig, base, kHairBaseHue),
        .normalK = hairColor(kHairNormalK, base, kHairBaseHue),
        .bigK = hairColor(kHairBigK, base, kHairBaseHue),
        .lightNormalK = hairColor(kHairLightNormalK, base, kHairBaseHue),
        .normalK2 = hairColor(kHairNormalK2, tips, kHairTipsHue),
        .lightNormalK2 = hairColor(kHairLightNormalK2, tips, kHairTipsHue),
        .lightBigK2 = hairColor(kHairLightBigK2, tips, kHairTipsHue),
    };
}

void PuppetCosmetics::bind(const Models& models, uint32_t clientId) {
    release();
    mClientId = clientId;
    for (size_t t = 0; t < kTargetCount; t++) {
        const CosmeticsTarget& target = kTargets[t];
        J3DModelData* data = models[static_cast<size_t>(target.model)];
        J3DTexture* tex = data != nullptr ? data->getTexture() : nullptr;
        JUTNameTab* names = data != nullptr ? data->getTextureName() : nullptr;
        if (tex == nullptr || names == nullptr) {
            continue;
        }
        for (u16 i = 0; i < tex->getNum(); i++) {
            const char* name = names->getName(i);
            const uint8_t* image = tex->getImgDataPtr(i);
            if (name == nullptr || std::strcmp(name, target.texture) != 0 || image == nullptr ||
                std::any_of(mEntries.begin(), mEntries.end(),
                    [image](const Entry& e) { return e.image == image; }))
            {
                continue;
            }
            const ResTIMG* timg = tex->getResTIMG(i);
            Entry e;
            e.target = static_cast<uint8_t>(t);
            e.var = target.var;
            e.tex = tex;
            e.texNo = i;
            e.image = image;
            e.format = timg->format;
            e.width = timg->width;
            e.height = timg->height;
            e.mips = std::max<uint8_t>(timg->mipmapCount, 1);
            e.palette = timg->indexTexture != 0;
            e.bytes = looks::gxImageBytes(e.format, e.width, e.height, e.mips);
            e.hash = e.bytes != 0 ? looks::hashBytes(image, e.bytes) : 0;
            mEntries.push_back(e);
        }
    }
}

void PuppetCosmetics::release() {
    for (Entry& e : mEntries) {
        if (e.handle != 0 && svc_texture != nullptr) {
            svc_texture->unregister(mod_ctx, e.handle);
        }
    }
    // Puppets never run their destructors: give the storage back here.
    std::vector<Entry>().swap(mEntries);
    mLogged = ~0u;
}

bool PuppetCosmetics::registerEntry(Entry& e, CosmeticsTexState state, uint32_t color) {
    TextureReplacementHandle handle = 0;
    Hsv mean{};
    if (state != CosmeticsTexState::None) {
        std::vector<uint8_t> recolored;
        uint32_t format = e.format;
        const uint8_t* data = e.image;
        size_t size = e.bytes;
        if (state == CosmeticsTexState::Recolor) {
            if (!recolorTexels(e.format, e.width, e.height, e.mips, e.image, e.bytes, color,
                    recolored, format, mean))
            {
                return false;
            }
            data = recolored.data();
            size = recolored.size();
        }
        TextureKey key = TEXTURE_KEY_INIT;
        key.kind = TEXTURE_KEY_POINTER;
        key.pointer = e.image;
        key.width = e.width;
        key.height = e.height;
        key.gx_format = e.format;
        TextureData texture = TEXTURE_DATA_INIT;
        texture.data = data;
        texture.size = size;
        texture.width = e.width;
        texture.height = e.height;
        texture.mip_count = e.mips;
        texture.gx_format = format;
        if (svc_texture->register_data(mod_ctx, &key, &texture, &handle) != MOD_OK) {
            return false;
        }
    }
    if (e.handle != 0) {
        svc_texture->unregister(mod_ctx, e.handle);
    }
    e.handle = handle;
    e.state = state;
    e.color = color;
    e.mean = mean;
    return true;
}

void PuppetCosmetics::update(
    const looks::CosmeticsValues* shown, bool block, const TintedFn& tinted) {
    if (mEntries.empty() || svc_texture == nullptr) {
        return;
    }
    int budget = kEntriesPerTick;
    bool changed = false;
    for (Entry& e : mEntries) {
        if (e.vanilla < 0) {
            e.vanilla = vanillaMatch(e.target, e.texNo, e.format, e.width, e.height, e.mips, e.hash);
        }
        CosmeticsTexState want = block ? CosmeticsTexState::Block : CosmeticsTexState::None;
        uint32_t color = 0;
        if (shown != nullptr && shown->has(e.var) && e.vanilla == 1 && recolorable(e.format)) {
            want = CosmeticsTexState::Recolor;
            color = shown->value[e.var];
        }
        // A pointer replacement would hide our tint, and the tinted bytes match no one's key.
        if (e.bytes == 0 || e.palette || (tinted && tinted(e.image))) {
            want = CosmeticsTexState::None;
        }
        if (want == e.state && (want != CosmeticsTexState::Recolor || color == e.color)) {
            continue;
        }
        if (budget-- <= 0) {
            break;
        }
        if (!registerEntry(e, want, color)) {
            TwiliLog.warn("[cosmetics {}] {} {}: registering failed, left as it is", mClientId,
                kTargets[e.target].texture, probeName(want));
            e.bytes = 0;
        }
        changed = true;
    }
    if (!changed) {
        return;
    }
    uint32_t recolored = 0, blocked = 0;
    for (const Entry& e : mEntries) {
        recolored += e.state == CosmeticsTexState::Recolor ? 1 : 0;
        blocked += e.state == CosmeticsTexState::Block ? 1 : 0;
    }
    if (const uint32_t summary = recolored << 16 | blocked; summary != mLogged) {
        mLogged = summary;
        TwiliLog.info("[cosmetics {}] {} of {} textures recoloured, {} blocked", mClientId,
            recolored, mEntries.size(), blocked);
    }
}

void PuppetCosmetics::probe(CosmeticsProbe& out) const {
    out = {};
    out.bound = bound();
    const int state = sKeysState.load(std::memory_order_acquire);
    out.keys = state == kKeysReady ? 1 : state == kKeysUnavailable ? 2 : 0;
    for (const Entry& e : mEntries) {
        CosmeticsTexProbe t;
        t.texture = kTargets[e.target].texture;
        t.var = e.var;
        t.state = e.state;
        t.vanilla = e.vanilla;
        t.color = e.color;
        t.mean = e.mean;
#if TWILI_HAS_MODS_SERVICE
        if (svc_mods != nullptr) {
            ModTextureSource source = MOD_TEXTURE_SOURCE_NONE;
            const TGXTexObj* obj = &(e.tex->*privateMember(detail::J3DTextureTexObj{}))[e.texNo];
            if (svc_mods->debug_texture_source(mod_ctx, static_cast<const GXTexObj*>(obj), nullptr,
                    &source) == MOD_OK)
            {
                t.source = static_cast<int>(source);
            }
        }
#endif
        out.recolored += e.state == CosmeticsTexState::Recolor ? 1 : 0;
        out.blocked += e.state == CosmeticsTexState::Block ? 1 : 0;
        out.tex.push_back(t);
    }
}

std::string cosmeticsSelfTest() {
    // Grey is the overlay's identity on luma; any colour keeps its hue.
    for (const float l : {0.0f, 0.2f, 0.5f, 0.8f, 1.0f}) {
        const Rgb n = cosmeticsRgb({l, l, l}, {0.5f, 0.5f, 0.5f});
        if (std::fabs(n.r - l) > 1e-4f || std::fabs(n.g - l) > 1e-4f || std::fabs(n.b - l) > 1e-4f) {
            return fmt::format("cosmeticsRgb: grey moved luma {} to {}", l, n.r);
        }
    }
    for (const uint32_t color : {0xE53935u, 0x1E88E5u, 0x43A047u}) {
        const Hsv want = toHsv(rgbOf(color));
        for (const float l : {0.25f, 0.6f}) {
            const Hsv got = toHsv(cosmeticsRgb({l, l, l}, rgbOf(color)));
            if (hueDist(got.h, want.h) > 2.0f) {
                return fmt::format("cosmeticsRgb: #{:06X} at luma {} gave hue {:.1f}", color, l, got.h);
            }
        }
    }

    // A CMPR block keeps its mode; RGB5A3 keeps its alpha.
    const uint8_t four[8] = {0x84, 0x10, 0x21, 0x04, 0x1B, 0x1B, 0x1B, 0x1B};
    const uint8_t three[8] = {0x21, 0x04, 0x84, 0x10, 0xFF, 0x1B, 0x1B, 0x1B};
    for (const uint8_t* block : {four, three}) {
        std::vector<uint8_t> out;
        uint32_t format = 0;
        Hsv mean{};
        recolorTexels(GX_TF_CMPR, 4, 4, 1, block, 8, 0xE53935, out, format, mean);
        const bool wasFour = be16(block) > be16(block + 2);
        if (out.size() != 8 || (be16(out.data()) > be16(out.data() + 2)) != wasFour) {
            return "recolor CMPR: a block changed its mode";
        }
        if (!wasFour && out[4] != 0xFF) {
            return "recolor CMPR: a three-colour block lost its transparent texels";
        }
    }
    const uint8_t translucent[2] = {0x3A, 0x85};
    std::vector<uint8_t> out;
    uint32_t format = 0;
    Hsv mean{};
    recolorTexels(GX_TF_RGB5A3, 1, 1, 1, translucent, 2, 0x1E88E5, out, format, mean);
    if (out.size() != 2 || (out[0] & 0xF0) != (translucent[0] & 0xF0)) {
        return "recolor RGB5A3: alpha changed";
    }
    // I8 becomes RGB5A3 of the same tile area, keeping I as alpha.
    std::vector<uint8_t> i8(32, 0xB0);
    recolorTexels(GX_TF_I8, 8, 4, 1, i8.data(), 32, 0x43A047, out, format, mean);
    if (format != GX_TF_RGB5A3 || out.size() != 64 || (be16(out.data()) & 0x8000) ||
        ((be16(out.data()) >> 12) & 7) != 5 || hueDist(mean.h, toHsv(rgbOf(0x43A047)).h) > 3.0f)
    {
        return "recolor I8: not RGB5A3 in the colour with I as alpha";
    }

    if (looks::gxImageBytes(GX_TF_CMPR, 8, 8, 1) != 32 || looks::gxImageBytes(GX_TF_CMPR, 16, 16, 2) != 160 ||
        looks::gxImageBytes(GX_TF_RGB5A3, 4, 4, 1) != 32 || looks::gxImageBytes(GX_TF_I8, 9, 4, 1) != 64 ||
        looks::gxImageBytes(0x7, 4, 4, 1) != 0)
    {
        return "gxImageBytes: wrong size";
    }

    // Yaz0: three literals, then five bytes from three back.
    const uint8_t yaz0[] = {'Y', 'a', 'z', '0', 0, 0, 0, 8, 0, 0, 0, 0, 0, 0, 0, 0,
                            0xE0, 'A', 'B', 'C', 0x30, 0x02};
    const auto decoded = looks::yaz0Decode(yaz0, 64);
    if (!decoded || std::string(decoded->begin(), decoded->end()) != "ABCABCAB") {
        return "yaz0Decode: wrong output";
    }
    if (looks::yaz0Decode(std::span(yaz0, sizeof(yaz0) - 1), 64) || looks::yaz0Decode(yaz0, 4)) {
        return "yaz0Decode: accepted a cut stream or one over the limit";
    }
    const uint8_t farBack[] = {'Y', 'a', 'z', '0', 0, 0, 0, 4, 0, 0, 0, 0, 0, 0, 0, 0, 0x80, 'A', 0x10, 0x05};
    if (looks::yaz0Decode(farBack, 64)) {
        return "yaz0Decode: accepted a reference before the start";
    }

    // A one-texture BMD and a one-file RARC.
    std::vector<uint8_t> bmd(0xA0, 0);
    auto put32 = [](std::vector<uint8_t>& b, size_t at, uint32_t v) {
        b[at] = static_cast<uint8_t>(v >> 24);
        b[at + 1] = static_cast<uint8_t>(v >> 16);
        b[at + 2] = static_cast<uint8_t>(v >> 8);
        b[at + 3] = static_cast<uint8_t>(v);
    };
    auto put16 = [](std::vector<uint8_t>& b, size_t at, uint16_t v) {
        b[at] = static_cast<uint8_t>(v >> 8);
        b[at + 1] = static_cast<uint8_t>(v);
    };
    std::memcpy(bmd.data(), "J3D2bmd3", 8);
    put32(bmd, 0x08, 0xA0);
    put32(bmd, 0x0C, 1);
    std::memcpy(bmd.data() + 0x20, "TEX1", 4);
    put32(bmd, 0x24, 0x80);
    put16(bmd, 0x28, 1);
    put32(bmd, 0x2C, 0x20);  // headers at 0x40
    put32(bmd, 0x30, 0x40);  // names at 0x60
    bmd[0x40] = GX_TF_CMPR;
    put16(bmd, 0x42, 8);
    put16(bmd, 0x44, 8);
    bmd[0x58] = 1;
    put32(bmd, 0x5C, 0x40);  // image at 0x80
    put16(bmd, 0x60, 1);
    put16(bmd, 0x66, 8);
    std::memcpy(bmd.data() + 0x68, "tex", 4);
    const auto textures = looks::bmdTextures(bmd);
    if (!textures || textures->size() != 1 || (*textures)[0].name != "tex" ||
        (*textures)[0].imageOffset != 0x80 || (*textures)[0].imageBytes != 32)
    {
        return "bmdTextures: wrong entry";
    }
    if (looks::bmdTextures(std::span(bmd.data(), 0x90))) {
        return "bmdTextures: accepted a cut file";
    }
    std::vector<uint8_t> arc(0x84, 0);
    std::memcpy(arc.data(), "RARC", 4);
    put32(arc, 0x04, 0x84);
    put32(arc, 0x08, 0x20);
    put32(arc, 0x0C, 0x60);  // data at 0x80
    put32(arc, 0x20, 1);
    put32(arc, 0x24, 0x20);
    put32(arc, 0x28, 1);
    put32(arc, 0x2C, 0x30);  // entries at 0x50
    put32(arc, 0x30, 0x10);
    put32(arc, 0x34, 0x44);  // strings at 0x64
    put32(arc, 0x54, 0x01000000);
    put32(arc, 0x58, 0);
    put32(arc, 0x5C, 4);
    std::memcpy(arc.data() + 0x64, "a.bmd", 6);
    std::memcpy(arc.data() + 0x80, "DATA", 4);
    const auto file = looks::rarcRead(arc, "A.BMD", 16);
    if (!file || std::string(file->begin(), file->end()) != "DATA") {
        return "rarcRead: wrong file";
    }
    put32(arc, 0x5C, 8);
    if (looks::rarcFiles(arc)) {
        return "rarcFiles: accepted a file past the end";
    }

    const MidnaHairColours vanilla = midnaHairColours(0, 0);
    if (vanilla.normal.r != kHairNormal.r || vanilla.lightBigK2.g != kHairLightBigK2.g ||
        vanilla.normalK.r != kHairNormalK.r)
    {
        return "midnaHairColours: index 0 is not the game's";
    }
    const MidnaHairColours blue = midnaHairColours(5, 2);
    if (hueDist(toHsv({blue.normalK.r / 255.0f, blue.normalK.g / 255.0f, blue.normalK.b / 255.0f}).h,
            220.0f) > 3.0f ||
        hueDist(toHsv({blue.normalK2.r / 255.0f, blue.normalK2.g / 255.0f, blue.normalK2.b / 255.0f}).h,
            0.0f) > 3.0f)
    {
        return "midnaHairColours: blue base or red tips not in their hue";
    }
    return {};
}

}  // namespace twili
