#pragma once

// What the player has installed, through the host's optional ModsService.

#include <mods/svc/texture.h>

#if __has_include(<mods/svc/mods.h>)
#include <mods/svc/mods.h>
#define TWILI_HAS_MODS_SERVICE 1
#else
#define TWILI_HAS_MODS_SERVICE 0
#endif

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace twili::looks {

enum class OverrideKind : uint8_t { DiscFile, ArcEntry, Texture };

struct ModEntry {
    std::string id;
    std::string name;
    std::string version;
    std::string author;
    std::string manifest;
    uint32_t loadOrder = 0;
    bool native = false;
    bool script = false;
};

struct OverrideEntry {
    uint64_t id = 0;
    OverrideKind kind = OverrideKind::DiscFile;
    bool runtime = false;
    bool wins = false;
    std::string discPath;
    std::string archivePath;
    std::string aliasOf;
    std::string bundlePath;
    uint64_t size = 0;
    TextureKey textureKey = TEXTURE_KEY_INIT;
    bool textureRaw = false;
    uint32_t textureWidth = 0;
    uint32_t textureHeight = 0;
    uint32_t textureGxFormat = 0;
};

using Sha256 = std::array<uint8_t, 32>;

// False when the host has no ModsService.
bool available();
std::vector<ModEntry> listMods();
// Without `textures`, texture overrides are skipped before anything of them is copied.
std::vector<OverrideEntry> listOverrides(const std::string& modId, bool textures = true);
std::optional<Sha256> hashOverride(const std::string& modId, uint64_t id);
std::optional<std::vector<uint8_t>> readOverride(const std::string& modId, uint64_t id);
std::optional<std::vector<uint8_t>> readBaseDiscFile(const char* discPath);
std::string hex(const Sha256& sha);

// The cosmetics settings a player shares, in the order M1 sends them.
struct CosmeticsVar {
    const char* name;
    bool isInt;
};
extern const std::array<CosmeticsVar, 24> kCosmeticsVars;
inline constexpr const char* kCosmeticsModId = "dev.twilitrealm.cosmetics";

// tt.looks.dump: logs every active mod, its overrides and the shared cosmetics values. With
// `hashes`, hashes every winning override on the game thread (a debugging aid only).
void dump(bool hashes);

}  // namespace twili::looks
