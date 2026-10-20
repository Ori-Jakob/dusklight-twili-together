// Steps for the look sharing host pieces (ModsService, base aliases, entry overlay churn);
// reference in the runner README.

#include "autotest/AutoTestSteps.hpp"

#include "actors/PrivateArchives.hpp"
#include "core/Log.hpp"
#include "looks/Inventory.hpp"

#include <mods/svc/host.h>
#include <mods/svc/overlay.h>
#include <mods/svc/texture.h>

#include "JSystem/JKernel/JKRArchive.h"
#include "d/d_com_inf_game.h"
#include "res/Object/AlAnm.h"

#include <dolphin/dvd.h>
#include <dolphin/gx.h>
#include <fmt/format.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace twili::autotest {
namespace {

#if TWILI_HAS_MODS_SERVICE

bool iequals(const std::string& a, const std::string& b) {
    return std::ranges::equal(a, b, [](char x, char y) {
        return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
    });
}

std::vector<uint8_t> bytesOf(const std::string& text) {
    return {text.begin(), text.end()};
}

struct Problems {
    std::vector<std::string> list;
    void expect(bool ok, std::string what) {
        if (!ok) {
            list.push_back(std::move(what));
        }
    }
};

const looks::OverrideEntry* findPath(const std::vector<looks::OverrideEntry>& list, const std::string& path) {
    const auto it = std::ranges::find_if(list, [&](const auto& o) {
        return o.kind != looks::OverrideKind::Texture && iequals(o.discPath, path);
    });
    return it != list.end() ? &*it : nullptr;
}

std::string shaOf(const std::string& modId, const looks::OverrideEntry& o) {
    const auto sha = looks::hashOverride(modId, o.id);
    return sha ? looks::hex(*sha) : "none";
}

uint8_t* align32(uint8_t* data) {
    return reinterpret_cast<uint8_t*>((reinterpret_cast<uintptr_t>(data) + 31) & ~uintptr_t{31});
}

// Finds file `index` (or the first file for kAnyFile, setting index) and its in-archive path,
// "<root>/<dirs>/<name>", as entry overlays use it.
constexpr uint32_t kAnyFile = UINT32_MAX;
bool arcEntryPath(const JKRArchive& arc, uint32_t dirIndex, const std::string& prefix,
    uint32_t& index, std::string& out) {
    const auto& dir = arc.mNodes[dirIndex];
    for (uint32_t i = 0; i < dir.num_entries; i++) {
        const auto& entry = arc.mFiles[dir.first_file_index + i];
        const std::string name = &arc.mStringTable[entry.getNameOffset()];
        if (name == "." || name == "..") {
            continue;
        }
        if (entry.isDirectory()) {
            if (arcEntryPath(arc, entry.data_offset, prefix + name + "/", index, out)) {
                return true;
            }
        } else if (index == kAnyFile || entry.index == index) {
            index = entry.index;
            out = prefix + name;
            return true;
        }
    }
    return false;
}

std::string arcRoot(const JKRArchive& arc) {
    return std::string(&arc.mStringTable[arc.mNodes[0].name_offset]) + "/";
}

// A copy-style read of one archive file, as Link's and the puppets' loads do.
std::vector<uint8_t> readArcFile(JKRArchive& arc, uint32_t index) {
    constexpr uint32_t kMaxSize = 1u << 20;
    const auto storage = std::make_unique<uint8_t[]>(kMaxSize + 32);
    uint8_t* buffer = align32(storage.get());
    const uint32_t size = std::min(arc.readIdxResource(buffer, kMaxSize, index), kMaxSize);
    return {buffer, buffer + size};
}

// XXH64 (seed 0) of 64 bytes of 0x3C, the content key Aurora computes for sSourceTexels.
constexpr uint64_t kSourceTexelsHash = 0xF2FA84DAA598697Aull;

// 4x4 RGBA8: a runtime POINTER replacement keys on sTexels and a SOURCE one on the content of
// sSourceTexels; nothing keys on sOtherTexels.
alignas(32) uint8_t sTexels[64];
alignas(32) uint8_t sOtherTexels[64];
alignas(32) uint8_t sSourceTexels[64];

struct InventoryRun {
    std::vector<OverlayHandle> overlays;
    TextureReplacementHandle texture = 0;
    TextureReplacementHandle sourceTexture = 0;
    uint64_t watch = 0;
    int watchCalls = 0;
    std::vector<uint8_t> vanilla;
    std::vector<uint8_t> runtimeBytes;
    std::string discOverlayFile;
    std::vector<uint8_t> discOverlayVanilla;
    std::vector<uint8_t> discOverlayBytes;
    Problems problems;
    int phase = 0;
    // The alias and its base file, mounted the way puppets mount theirs.
    std::string mountPaths[2];
    PrivateArchives archives;
    uint32_t entryIndex = kAnyFile;
    std::vector<uint8_t> baseEntry;
    std::vector<uint8_t> entryOverlayBytes;
    int overlayTick = 0;
};
InventoryRun sInventory;

void cleanupInventory() {
    for (const OverlayHandle handle : sInventory.overlays) {
        if (handle != 0) {
            svc_overlay->remove(mod_ctx, handle);
        }
    }
    for (const TextureReplacementHandle handle : {sInventory.texture, sInventory.sourceTexture}) {
        if (handle != 0) {
            svc_texture->unregister(mod_ctx, handle);
        }
    }
    if (sInventory.watch != 0) {
        svc_mods->unwatch(mod_ctx, sInventory.watch);
    }
    sInventory.archives.release();
    sInventory = {};
}

// Registers what the check then expects to see: a runtime buffer, a base alias, an overlay over
// a disc file and runtime POINTER and SOURCE textures.
bool beginInventory(StepContext& ctx) {
    const auto& p = ctx.step;
    if (!looks::available() || svc_overlay == nullptr || svc_texture == nullptr) {
        ctx.fail("checkLooksInventory: the host lacks ModsService, OverlayService or TextureService");
        return false;
    }
    sInventory = {};
    const std::string baseFile = p.value("baseFile", std::string("/res/Object/Mmdl.arc"));
    sInventory.vanilla = looks::readBaseDiscFile(baseFile.c_str()).value_or(std::vector<uint8_t>{});
    if (sInventory.vanilla.size() < 4 ||
        (std::memcmp(sInventory.vanilla.data(), "RARC", 4) != 0 &&
            std::memcmp(sInventory.vanilla.data(), "Yaz0", 4) != 0))
    {
        ctx.fail(fmt::format("checkLooksInventory: no archive read from {}", baseFile));
        return false;
    }
    sInventory.discOverlayFile = p.value("discOverlayFile", std::string("/res/Object/E_fz.arc"));
    sInventory.discOverlayVanilla =
        looks::readBaseDiscFile(sInventory.discOverlayFile.c_str()).value_or(std::vector<uint8_t>{});
    if (sInventory.discOverlayVanilla.empty()) {
        ctx.fail(fmt::format("checkLooksInventory: nothing read from {}", sInventory.discOverlayFile));
        return false;
    }
    svc_mods->watch(
        mod_ctx, [](ModContext*, uint64_t, void*) { ++sInventory.watchCalls; }, nullptr,
        &sInventory.watch);

    OverlayHandle handle = 0;
    sInventory.runtimeBytes = bytesOf(p.value("runtimeBytes", std::string("twili runtime overlay\n")));
    svc_overlay->add_buffer(mod_ctx, p.value("runtimePath", std::string("/tt_autotest/runtime.bin")).c_str(),
        sInventory.runtimeBytes.data(), sInventory.runtimeBytes.size(), &handle);
    sInventory.overlays.push_back(handle);

#if OVERLAY_SERVICE_MINOR >= 1
    if (SERVICE_HAS(svc_overlay, OverlayService, add_base_alias)) {
        handle = 0;
        svc_overlay->add_base_alias(mod_ctx,
            p.value("aliasPath", std::string("/tt_autotest/alias/Mmdl.arc")).c_str(), baseFile.c_str(),
            &handle);
        sInventory.overlays.push_back(handle);
    }
#endif

    // Never loaded in Link's house; removed when the step ends.
    sInventory.discOverlayBytes = bytesOf("twili disc overlay\n");
    handle = 0;
    svc_overlay->add_buffer(mod_ctx, sInventory.discOverlayFile.c_str(),
        sInventory.discOverlayBytes.data(), sInventory.discOverlayBytes.size(), &handle);
    sInventory.overlays.push_back(handle);

    std::memset(sTexels, 0x5A, sizeof(sTexels));
    std::memset(sOtherTexels, 0xA5, sizeof(sOtherTexels));
    TextureKey key = TEXTURE_KEY_INIT;
    key.kind = TEXTURE_KEY_POINTER;
    key.pointer = sTexels;
    TextureData data = TEXTURE_DATA_INIT;
    data.data = sTexels;
    data.size = sizeof(sTexels);
    data.width = 4;
    data.height = 4;
    data.gx_format = GX_TF_RGBA8;
    svc_texture->register_data(mod_ctx, &key, &data, &sInventory.texture);

    std::memset(sSourceTexels, 0x3C, sizeof(sSourceTexels));
    TextureKey sourceKey = TEXTURE_KEY_INIT;
    sourceKey.kind = TEXTURE_KEY_SOURCE;
    sourceKey.texture_hash = kSourceTexelsHash;
    sourceKey.width = 4;
    sourceKey.height = 4;
    sourceKey.gx_format = GX_TF_RGBA8;
    svc_texture->register_data(mod_ctx, &sourceKey, &data, &sInventory.sourceTexture);
    return true;
}

void checkFixtures(const nlohmann::json& p, const std::vector<looks::ModEntry>& mods, Problems& out) {
    const std::string idA = p.value("fixtureA", std::string());
    const std::string idB = p.value("fixtureB", std::string());
    const auto findMod = [&](const std::string& id) {
        const auto it = std::ranges::find_if(mods, [&](const auto& m) { return m.id == id; });
        return it != mods.end() ? &*it : nullptr;
    };
    const auto* modA = findMod(idA);
    const auto* modB = findMod(idB);
    out.expect(modA != nullptr && modB != nullptr, fmt::format("fixtures {} / {} not active", idA, idB));
    out.expect(findMod(looks::kCosmeticsModId) != nullptr, "the cosmetics mod is not active");
    if (modA == nullptr || modB == nullptr) {
        return;
    }
    const std::string marker = p.value("manifestMarker", std::string());
    out.expect(!marker.empty() && modA->manifest.find(marker) != std::string::npos,
        fmt::format("{} mod.json lacks '{}': {}", idA, marker, modA->manifest));
    out.expect(!modA->native && !modA->script, fmt::format("{} is not asset-only", idA));

    const auto listA = looks::listOverrides(idA);
    const auto listB = looks::listOverrides(idB);
    const bool aWins = modA->loadOrder > modB->loadOrder;
    const std::string sharedPath = p.value("sharedPath", std::string());
    const auto* sharedA = findPath(listA, sharedPath);
    const auto* sharedB = findPath(listB, sharedPath);
    out.expect(sharedA != nullptr && sharedB != nullptr, fmt::format("{} not listed for both fixtures", sharedPath));
    if (sharedA != nullptr && sharedB != nullptr) {
        out.expect(sharedA->wins == aWins && sharedB->wins == !aWins && !sharedA->runtime,
            fmt::format("{}: wins A={} B={}, load order A={} B={}", sharedPath, sharedA->wins,
                sharedB->wins, modA->loadOrder, modB->loadOrder));
    }

    const std::string aOnlyPath = p.value("aOnlyPath", std::string());
    if (const auto* aOnly = findPath(listA, aOnlyPath); aOnly == nullptr) {
        out.expect(false, fmt::format("{} not listed", aOnlyPath));
    } else {
        const auto bytes = looks::readOverride(idA, aOnly->id);
        out.expect(aOnly->kind == looks::OverrideKind::DiscFile && aOnly->wins &&
                       aOnly->bundlePath == "overlay" + aOnlyPath,
            fmt::format("{}: kind {} wins {} bundle '{}'", aOnlyPath, static_cast<int>(aOnly->kind),
                aOnly->wins, aOnly->bundlePath));
        out.expect(aOnly->size == p.value("aOnlySize", 0ull) && bytes && bytes->size() == aOnly->size,
            fmt::format("{}: size {} read {}", aOnlyPath, aOnly->size, bytes ? bytes->size() : 0));
        const std::string sha = shaOf(idA, *aOnly);
        out.expect(sha == p.value("aOnlySha", std::string()),
            fmt::format("{}: sha256 {} want {}", aOnlyPath, sha, p.value("aOnlySha", std::string())));
        out.expect(shaOf(idA, *aOnly) == sha, fmt::format("{}: hash changed on a second request", aOnlyPath));
    }

    const std::string entryPath = p.value("arcEntryPath", std::string());
    if (const auto* entry = findPath(listA, entryPath); entry == nullptr) {
        out.expect(false, fmt::format("{} not listed", entryPath));
    } else {
        out.expect(entry->kind == looks::OverrideKind::ArcEntry &&
                       iequals(entry->archivePath, p.value("arcEntryArchive", std::string())),
            fmt::format("{}: kind {} archive '{}'", entryPath, static_cast<int>(entry->kind),
                entry->archivePath));
    }

    const std::string textureFile = p.value("textureBundlePath", std::string());
    const auto texture = std::ranges::find_if(listA, [&](const auto& o) {
        return o.kind == looks::OverrideKind::Texture && o.bundlePath == textureFile;
    });
    if (texture == listA.end()) {
        out.expect(false, fmt::format("{} not listed", textureFile));
    } else {
        const TextureKey& key = texture->textureKey;
        const uint64_t wantHash = std::stoull(p.value("textureHash", std::string("0")), nullptr, 16);
        out.expect(!texture->runtime && texture->wins && !texture->textureRaw &&
                       key.kind == TEXTURE_KEY_SOURCE && key.texture_hash == wantHash &&
                       key.width == p.value("textureWidth", 0u) &&
                       key.height == p.value("textureHeight", 0u) &&
                       key.gx_format == p.value("textureFormat", 0u),
            fmt::format("{}: runtime {} wins {} kind {} hash {:016x} {}x{} fmt {}", textureFile,
                texture->runtime, texture->wins, static_cast<int>(key.kind), key.texture_hash,
                key.width, key.height, key.gx_format));
    }
}

// The alias as the game opens it, through the file table: the disc file's size and bytes.
void checkAliasFile(const std::string& aliasPath, Problems& out) {
    DVDFileInfo info{};
    if (!DVDOpen(aliasPath.c_str(), &info)) {
        out.expect(false, fmt::format("DVDOpen {} failed", aliasPath));
        return;
    }
    const uint32_t length = info.length;
    const uint32_t readSize = (length + 31) & ~31u;
    std::vector<uint8_t> storage(readSize + 32);
    uint8_t* buffer = align32(storage.data());
    const s32 read = readSize != 0 ? DVDReadPrio(&info, buffer, static_cast<s32>(readSize), 0, 2) : 0;
    DVDClose(&info);
    out.expect(length == sInventory.vanilla.size() && read == static_cast<s32>(length) &&
                   std::memcmp(buffer, sInventory.vanilla.data(), length) == 0,
        fmt::format("DVDOpen {}: {} B, read {}, the disc file has {} B", aliasPath, length, read,
            sInventory.vanilla.size()));
}

void checkRuntime(const nlohmann::json& p, Problems& out) {
    const std::string self = svc_host->mod_id(mod_ctx);
    const auto list = looks::listOverrides(self);
    const std::string baseFile = p.value("baseFile", std::string("/res/Object/Mmdl.arc"));

    const std::string runtimePath = p.value("runtimePath", std::string("/tt_autotest/runtime.bin"));
    if (const auto* o = findPath(list, runtimePath); o == nullptr) {
        out.expect(false, fmt::format("{} not listed for {}", runtimePath, self));
    } else {
        const auto bytes = looks::readOverride(self, o->id);
        out.expect(o->runtime && o->wins && o->kind == looks::OverrideKind::DiscFile &&
                       bytes == sInventory.runtimeBytes,
            fmt::format("{}: runtime {} wins {} read {} B", runtimePath, o->runtime, o->wins,
                bytes ? bytes->size() : 0));
        const std::string sha = shaOf(self, *o);
        out.expect(sha == p.value("runtimeSha", std::string()), fmt::format("{}: sha256 {}", runtimePath, sha));
    }

    const std::string aliasPath = p.value("aliasPath", std::string("/tt_autotest/alias/Mmdl.arc"));
    if (const auto* o = findPath(list, aliasPath); o == nullptr) {
        out.expect(false, fmt::format("{} not listed (no add_base_alias?)", aliasPath));
    } else {
        const auto bytes = looks::readOverride(self, o->id);
        out.expect(o->runtime && iequals(o->aliasOf, baseFile) && o->size == sInventory.vanilla.size() &&
                       bytes == sInventory.vanilla,
            fmt::format("{}: alias of '{}' size {} read {} B, the disc file has {} B", aliasPath,
                o->aliasOf, o->size, bytes ? bytes->size() : 0, sInventory.vanilla.size()));
    }

    checkAliasFile(aliasPath, out);

    const std::string& discFile = sInventory.discOverlayFile;
    if (const auto* o = findPath(list, discFile); o == nullptr) {
        out.expect(false, fmt::format("{} overlay not listed", discFile));
    } else {
        out.expect(o->runtime && o->wins && o->size == sInventory.discOverlayBytes.size(),
            fmt::format("{} overlay: wins {} size {}", discFile, o->wins, o->size));
    }
    const auto underOverlay = looks::readBaseDiscFile(discFile.c_str());
    out.expect(underOverlay == sInventory.discOverlayVanilla,
        fmt::format("{} read {} B under an overlay, want the disc's {} B", discFile,
            underOverlay ? underOverlay->size() : 0, sInventory.discOverlayVanilla.size()));

    const auto runtimeTexture = [&](int keyKind) {
        return std::ranges::find_if(list, [&](const auto& o) {
            return o.kind == looks::OverrideKind::Texture && o.runtime && o.textureKey.kind == keyKind;
        });
    };
    if (const auto source = runtimeTexture(TEXTURE_KEY_SOURCE); source == list.end()) {
        out.expect(false, "runtime SOURCE texture not listed");
    } else {
        out.expect(source->wins && source->textureKey.texture_hash == kSourceTexelsHash &&
                       source->textureKey.width == 4 && source->textureKey.height == 4 &&
                       source->textureKey.gx_format == GX_TF_RGBA8,
            fmt::format("runtime SOURCE texture: wins {} hash {:016x} {}x{} fmt {}", source->wins,
                source->textureKey.texture_hash, source->textureKey.width, source->textureKey.height,
                source->textureKey.gx_format));
    }
    const auto texture = runtimeTexture(TEXTURE_KEY_POINTER);
    if (texture == list.end()) {
        out.expect(false, "runtime POINTER texture not listed");
    } else {
        out.expect(texture->wins && texture->textureRaw && texture->textureKey.kind == TEXTURE_KEY_POINTER &&
                       texture->textureKey.pointer == sTexels && texture->size == sizeof(sTexels) &&
                       texture->textureWidth == 4 && texture->textureHeight == 4 &&
                       texture->textureGxFormat == GX_TF_RGBA8,
            fmt::format("runtime texture: wins {} raw {} kind {} size {} {}x{} fmt {}", texture->wins,
                texture->textureRaw, static_cast<int>(texture->textureKey.kind), texture->size,
                texture->textureWidth, texture->textureHeight, texture->textureGxFormat));
    }

    GXTexObj mine;
    GXTexObj byContent;
    GXTexObj other;
    GXInitTexObj(&mine, sTexels, 4, 4, GX_TF_RGBA8, GX_CLAMP, GX_CLAMP, GX_FALSE);
    GXInitTexObj(&byContent, sSourceTexels, 4, 4, GX_TF_RGBA8, GX_CLAMP, GX_CLAMP, GX_FALSE);
    GXInitTexObj(&other, sOtherTexels, 4, 4, GX_TF_RGBA8, GX_CLAMP, GX_CLAMP, GX_FALSE);
    ModTextureSource mineSource = MOD_TEXTURE_SOURCE_NONE;
    ModTextureSource contentSource = MOD_TEXTURE_SOURCE_NONE;
    ModTextureSource otherSource = MOD_TEXTURE_SOURCE_POINTER;
    svc_mods->debug_texture_source(mod_ctx, &mine, nullptr, &mineSource);
    svc_mods->debug_texture_source(mod_ctx, &byContent, nullptr, &contentSource);
    svc_mods->debug_texture_source(mod_ctx, &other, nullptr, &otherSource);
    out.expect(mineSource == MOD_TEXTURE_SOURCE_POINTER && contentSource == MOD_TEXTURE_SOURCE_SOURCE &&
                   otherSource == MOD_TEXTURE_SOURCE_NONE,
        fmt::format("debug_texture_source: pointer {} content {} other {}", static_cast<int>(mineSource),
            static_cast<int>(contentSource), static_cast<int>(otherSource)));
}

void checkVars(const nlohmann::json& p, Problems& out) {
    const char* cosmetics = looks::kCosmeticsModId;
    for (const auto& [name, want] : p.value("strings", nlohmann::json::object()).items()) {
        char value[64] = {};
        size_t length = 0;
        const ModResult result =
            svc_mods->get_mod_var_string(mod_ctx, cosmetics, name.c_str(), value, sizeof(value), &length);
        out.expect(result == MOD_OK && value == want.get<std::string>() && length == std::strlen(value),
            fmt::format("{}: result {} value '{}' want {}", name, static_cast<int>(result), value, want.dump()));
        size_t queried = 0;
        out.expect(svc_mods->get_mod_var_string(mod_ctx, cosmetics, name.c_str(), nullptr, 0, &queried) ==
                           MOD_OK &&
                       queried == length,
            fmt::format("{}: length query {}", name, queried));
        int64_t asInt = 0;
        out.expect(svc_mods->get_mod_var_int(mod_ctx, cosmetics, name.c_str(), &asInt) == MOD_INVALID_ARGUMENT,
            fmt::format("{}: read as an int", name));
    }
    for (const auto& [name, want] : p.value("ints", nlohmann::json::object()).items()) {
        int64_t value = -1;
        const ModResult result = svc_mods->get_mod_var_int(mod_ctx, cosmetics, name.c_str(), &value);
        out.expect(result == MOD_OK && value == want.get<int64_t>(),
            fmt::format("{}: result {} value {} want {}", name, static_cast<int>(result), value, want.dump()));
    }
    for (const auto& refused : p.value("refused", nlohmann::json::array())) {
        const std::string modId = refused.value("mod", std::string());
        const std::string name = refused.value("name", std::string());
        char value[64] = {};
        int64_t number = 0;
        const ModResult asString =
            svc_mods->get_mod_var_string(mod_ctx, modId.c_str(), name.c_str(), value, sizeof(value), nullptr);
        const ModResult asInt = svc_mods->get_mod_var_int(mod_ctx, modId.c_str(), name.c_str(), &number);
        out.expect(asString == MOD_UNAVAILABLE && asInt == MOD_UNAVAILABLE,
            fmt::format("{} {}: not refused ({} / {})", modId, name, static_cast<int>(asString),
                static_cast<int>(asInt)));
    }
}

// Once the alias and its base file are mounted: both read the same, then an entry overlay goes
// under the alias's own directory. False when the step should finish.
bool overlayAliasEntry(const nlohmann::json& p, int tick) {
    auto& run = sInventory;
    JKRArchive* alias = run.archives.get(0);
    JKRArchive* base = run.archives.get(1);
    if (alias == nullptr || base == nullptr) {
        run.problems.expect(false, fmt::format("mounting {} / {} failed", run.mountPaths[0], run.mountPaths[1]));
        return false;
    }
    std::string entryPath;
    if (!arcEntryPath(*alias, 0, arcRoot(*alias), run.entryIndex, entryPath)) {
        run.problems.expect(false, fmt::format("{} has no files", run.mountPaths[0]));
        return false;
    }
    const auto aliasEntry = readArcFile(*alias, run.entryIndex);
    run.baseEntry = readArcFile(*base, run.entryIndex);
    run.problems.expect(!run.baseEntry.empty() && aliasEntry == run.baseEntry,
        fmt::format("{} file {}: the alias read {} B, the base file {} B", run.mountPaths[0], entryPath,
            aliasEntry.size(), run.baseEntry.size()));

    std::string dir = run.mountPaths[0];
    dir.resize(dir.size() - 4);
    const std::string discPath = dir + "/" + entryPath;
    run.entryOverlayBytes = bytesOf(p.value("aliasEntryBytes", std::string("twili alias entry overlay\n")));
    OverlayHandle handle = 0;
    if (svc_overlay->add_buffer(mod_ctx, discPath.c_str(), run.entryOverlayBytes.data(),
            run.entryOverlayBytes.size(), &handle) != MOD_OK)
    {
        run.problems.expect(false, "add_buffer failed for " + discPath);
        return false;
    }
    run.overlays.push_back(handle);
    run.overlayTick = tick;
    TwiliLog.info("[autotest] looks inventory: entry overlay {} under the alias", discPath);
    return true;
}

void checkAliasEntry() {
    auto& run = sInventory;
    const auto aliasEntry = readArcFile(*run.archives.get(0), run.entryIndex);
    const auto baseEntry = readArcFile(*run.archives.get(1), run.entryIndex);
    run.problems.expect(aliasEntry == run.entryOverlayBytes,
        fmt::format("the alias read {} B for file {}, want its entry overlay's {} B", aliasEntry.size(),
            run.entryIndex, run.entryOverlayBytes.size()));
    run.problems.expect(baseEntry == run.baseEntry,
        fmt::format("the base file read {} B for file {} under the alias's entry overlay, want {} B",
            baseEntry.size(), run.entryIndex, run.baseEntry.size()));
}

bool checkLooksInventory(StepContext& ctx) {
    if (!ctx.begun) {
        return !beginInventory(ctx);
    }
    auto& run = sInventory;
    const auto& p = ctx.step;
    if (run.phase == 0) {
        // One frame applies the overlays, the next sees the watch callback.
        if (ctx.ticks < 2) {
            return false;
        }
        looks::dump(false);
        checkFixtures(p, looks::listMods(), run.problems);
        checkRuntime(p, run.problems);
        checkVars(p.value("cosmetics", nlohmann::json::object()), run.problems);
        run.problems.expect(run.watchCalls > 0, "the watch callback never ran");

        ModOverlayStats stats = MOD_OVERLAY_STATS_INIT;
        run.problems.expect(svc_mods->debug_overlay_stats(mod_ctx, &stats) == MOD_OK && stats.overlay_syncs > 0,
            fmt::format("debug_overlay_stats: {} syncs", stats.overlay_syncs));
        run.mountPaths[0] = p.value("aliasPath", std::string("/tt_autotest/alias/Mmdl.arc"));
        run.mountPaths[1] = p.value("baseFile", std::string("/res/Object/Mmdl.arc"));
        run.phase = 1;
        return false;
    }
    if (run.phase == 1) {
        const char* const paths[2] = {run.mountPaths[0].c_str(), run.mountPaths[1].c_str()};
        const auto state = run.archives.update(&run, paths, 2, p.value("mountHeapSize", 0x400000u));
        if (run.archives.failed()) {
            run.problems.expect(false, "no heap for the alias mount");
        } else if (state != PrivateArchives::State::Ready) {
            return false;
        } else if (overlayAliasEntry(p, ctx.ticks)) {
            run.phase = 2;
            return false;
        }
    } else if (ctx.ticks <= run.overlayTick) {
        // The entry overlay reaches the file table at the end of the frame.
        return false;
    } else {
        checkAliasEntry();
    }

    Problems problems = std::move(run.problems);
    const uint64_t before = svc_mods->generation(mod_ctx);
    cleanupInventory();
    problems.expect(svc_mods->generation(mod_ctx) != before, "removing overrides kept the generation");

    TwiliLog.info("[autotest] looks inventory: {} problem(s)", problems.list.size());
    if (!problems.list.empty()) {
        std::string all;
        for (const auto& problem : problems.list) {
            TwiliLog.warn("[autotest] looks inventory: {}", problem);
            all += (all.empty() ? "" : "; ") + problem;
        }
        ctx.fail("checkLooksInventory: " + all);
    }
    return true;
}

struct ChurnRun {
    JKRArchive* arc = nullptr;
    uint32_t index = 0;
    std::unique_ptr<uint8_t[]> storage;
    uint8_t* buffer = nullptr;
    uint32_t bufferSize = 0;
    std::vector<uint8_t> vanilla;
    OverlayHandle entryOverlay = 0;
    OverlayHandle churn = 0;
    int cycle = 0;
    bool measuring = false;
    ModOverlayStats base = MOD_OVERLAY_STATS_INIT;
};
ChurnRun sChurn;

void endChurn() {
    if (sChurn.churn != 0) {
        svc_overlay->remove(mod_ctx, sChurn.churn);
    }
    if (sChurn.entryOverlay != 0) {
        svc_overlay->remove(mod_ctx, sChurn.entryOverlay);
    }
    sChurn = {};
}

// A copy-style read, as Link's animation loads do; true when it came from our padded overlay and
// a second read in the same frame reused the archive's copy.
bool readThroughOverlay(StepContext& ctx) {
    const uint32_t size = sChurn.arc->readIdxResource(sChurn.buffer, sChurn.bufferSize, sChurn.index);
    const uint32_t want = static_cast<uint32_t>(sChurn.vanilla.size()) + 32;
    if (size != want || std::memcmp(sChurn.buffer, sChurn.vanilla.data(), sChurn.vanilla.size()) != 0) {
        ctx.fail(fmt::format("overlayChurnAlloc: read {} B at cycle {}, want the overlay's {} B", size,
            sChurn.cycle, want));
        endChurn();
        return false;
    }
    ModOverlayStats first = MOD_OVERLAY_STATS_INIT;
    ModOverlayStats second = MOD_OVERLAY_STATS_INIT;
    svc_mods->debug_overlay_stats(mod_ctx, &first);
    const uint32_t again = sChurn.arc->readIdxResource(sChurn.buffer, sChurn.bufferSize, sChurn.index);
    svc_mods->debug_overlay_stats(mod_ctx, &second);
    if (again != want || second.total_entry_buffers != first.total_entry_buffers) {
        ctx.fail(fmt::format("overlayChurnAlloc: a second read at cycle {} got {} B and made {} copies",
            sChurn.cycle, again, second.total_entry_buffers - first.total_entry_buffers));
        endChurn();
        return false;
    }
    return true;
}

bool beginChurn(StepContext& ctx) {
    if (!looks::available() || svc_overlay == nullptr) {
        ctx.fail("overlayChurnAlloc: the host lacks ModsService or OverlayService");
        return false;
    }
    sChurn = {};
    sChurn.arc = dComIfGp_getAnmArchive();
    sChurn.index = ctx.step.value("index", static_cast<uint32_t>(dRes_INDEX_ALANM_BCK_WAITCANOEFISHL_e));
    if (sChurn.arc == nullptr || !sChurn.arc->isFileEntry(sChurn.index)) {
        ctx.fail(fmt::format("overlayChurnAlloc: AlAnm has no file {:#x}", sChurn.index));
        return false;
    }
    std::string entryPath;
    if (!arcEntryPath(*sChurn.arc, 0, arcRoot(*sChurn.arc), sChurn.index, entryPath)) {
        ctx.fail(fmt::format("overlayChurnAlloc: no path for AlAnm file {:#x}", sChurn.index));
        return false;
    }

    sChurn.bufferSize = 1u << 20;
    sChurn.storage = std::make_unique<uint8_t[]>(sChurn.bufferSize + 32);
    sChurn.buffer = align32(sChurn.storage.get());
    const uint32_t size = sChurn.arc->readIdxResource(sChurn.buffer, sChurn.bufferSize, sChurn.index);
    if (size == 0 || size + 32 > sChurn.bufferSize) {
        ctx.fail(fmt::format("overlayChurnAlloc: AlAnm file {:#x} read {} B", sChurn.index, size));
        return false;
    }
    sChurn.vanilla.assign(sChurn.buffer, sChurn.buffer + size);

    // The vanilla clip padded by 32 zero bytes, so reads show they came from the overlay.
    std::vector<uint8_t> padded = sChurn.vanilla;
    padded.resize(padded.size() + 32, 0);
    const std::string discPath = "/res/Object/AlAnm/" + entryPath;
    if (svc_overlay->add_buffer(mod_ctx, discPath.c_str(), padded.data(), padded.size(),
            &sChurn.entryOverlay) != MOD_OK)
    {
        ctx.fail("overlayChurnAlloc: add_buffer failed for " + discPath);
        return false;
    }
    TwiliLog.info("[autotest] overlay churn: entry overlay {} ({} B)", discPath, padded.size());
    return true;
}

bool overlayChurnAlloc(StepContext& ctx) {
    if (!ctx.begun) {
        return !beginChurn(ctx);
    }
    const int cycles = std::max(1, ctx.step.value("cycles", 50));
    if (!sChurn.measuring) {
        auto* entry = sChurn.arc->mFiles + sChurn.index;
        if (sChurn.arc->getFileSize(entry) != sChurn.vanilla.size() + 32) {
            ctx.fail("overlayChurnAlloc: the AlAnm entry overlay is not visible");
            endChurn();
            return true;
        }
        if (!readThroughOverlay(ctx)) {
            return true;
        }
        svc_mods->debug_overlay_stats(mod_ctx, &sChurn.base);
        sChurn.measuring = true;
    } else if (!readThroughOverlay(ctx)) {
        return true;
    }

    // Each add or remove rebuilds the overlay set and bumps every archive's generation.
    if (sChurn.churn == 0 && sChurn.cycle < cycles) {
        static const uint8_t kChurnBytes[16] = {'t', 'w', 'i', 'l', 'i'};
        svc_overlay->add_buffer(mod_ctx, "/tt_autotest/churn.bin", kChurnBytes, sizeof(kChurnBytes),
            &sChurn.churn);
        return false;
    }
    if (sChurn.churn != 0) {
        svc_overlay->remove(mod_ctx, sChurn.churn);
        sChurn.churn = 0;
        ++sChurn.cycle;
        return false;
    }

    // Each rebuild replaces the entry's copy, so only the one overlaid entry stays in memory.
    ModOverlayStats end = MOD_OVERLAY_STATS_INIT;
    svc_mods->debug_overlay_stats(mod_ctx, &end);
    const int64_t maxLiveGrowth = ctx.step.value("maxLiveGrowth", int64_t{0});
    const int64_t liveGrowth = static_cast<int64_t>(end.live_entry_buffers) -
                               static_cast<int64_t>(sChurn.base.live_entry_buffers);
    const uint64_t syncs = end.overlay_syncs - sChurn.base.overlay_syncs;
    TwiliLog.info("[autotest] overlay churn: {} cycles, {} overlay syncs, entry buffers live {} -> {}, "
                  "made {} -> {}",
        cycles, syncs, sChurn.base.live_entry_buffers, end.live_entry_buffers,
        sChurn.base.total_entry_buffers, end.total_entry_buffers);
    if (syncs < static_cast<uint64_t>(cycles) * 2) {
        ctx.fail(fmt::format("overlayChurnAlloc: only {} overlay syncs for {} cycles", syncs, cycles));
    } else if (liveGrowth > maxLiveGrowth) {
        ctx.fail(fmt::format("overlayChurnAlloc: live entry overlay copies grew by {}", liveGrowth));
    }
    endChurn();
    return true;
}

#endif

std::optional<bool> looksSteps(const std::string& op, StepContext& ctx) {
    if (op == "looksDump") {
        looks::dump(ctx.step.value("hashes", false));
        return true;
    }
    if (op == "checkLooksInventory" || op == "overlayChurnAlloc") {
#if TWILI_HAS_MODS_SERVICE
        return op == "checkLooksInventory" ? checkLooksInventory(ctx) : overlayChurnAlloc(ctx);
#else
        ctx.fail(op + ": built against an SDK without ModsService");
        return true;
#endif
    }
    return std::nullopt;
}

const bool sRegistered = registerSteps(&looksSteps);

}  // namespace
}  // namespace twili::autotest
