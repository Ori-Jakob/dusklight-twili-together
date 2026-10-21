#pragma once

// The cosmetics mod's colours a player shares (LOOK_MANIFEST), and what the others share.

#include "looks/Choices.hpp"
#include "looks/Slots.hpp"

#include <nlohmann/json.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <string>

namespace twili::looks {

inline constexpr size_t kCosmeticsVarCount = 24;

// Indices into kCosmeticsVars (Inventory.hpp).
enum CosmeticsVarId : uint8_t {
    kHerosTunicCap,
    kHerosTunicTorso,
    kHerosTunicSkirt,
    kZoraArmorCap,
    kZoraArmorHelmet,
    kZoraArmorTorso,
    kZoraArmorScales,
    kZoraArmorFlippers,
    kLanternGlow,
    kWoodenSword,
    kOrdonSwordBlade,
    kOrdonSwordHandle,
    kMasterSwordBlade,
    kMasterSwordHandle,
    kLightSwordGlow,
    kBoomerang,
    kIronBoots,
    kSpinner,
    kMidnaHairBase,
    kMidnaHairTips,
    kMidnaChargeRing,
    kLinkHair,
    kWolfLink,
    kEpona,
};

struct CosmeticsValues {
    // Bit i: var i differs from its default.
    uint32_t mask = 0;
    // 0xRRGGBB, or Midna's hair palette index (1-9).
    std::array<uint32_t, kCosmeticsVarCount> value{};
    std::string version;  // the cosmetics mod's

    bool has(uint8_t var) const { return var < kCosmeticsVarCount && ((mask >> var) & 1) != 0; }
    uint32_t get(uint8_t var) const { return has(var) ? value[var] : 0; }
    bool operator==(const CosmeticsValues&) const = default;
};

// Share My Looks (config share_looks): the only consent, Off by default.
enum class ShareScope : uint8_t { Off, Team, Room };
ShareScope shareScope();

// Session: the handshake's features and disc id, the relay's features, our manifest.
nlohmann::json handshakeFeatures();
std::string gameId();
void onRoster(const nlohmann::json& allClientState);
// The relay passes look entries on (relayFeatures "looks.send").
bool relayPassesLooks();
bool handlePacket(const std::string& type, const nlohmann::json& packet);
// Once per tick: reads ours once a second and sends it when it changed, at most every 2 s.
void tick();
void resetSession();
void shutdown();

// What a player shares, null when nothing (or nothing received yet).
const CosmeticsValues* sharedCosmetics(uint32_t clientId);
// What their puppets of one type wear: null when our settings hide it or we cannot show it.
const CosmeticsValues* shownCosmetics(uint32_t clientId, LookType type);
// Midna's hair on their puppets: theirs, ours for My Own, else null.
const CosmeticsValues* hairCosmetics(uint32_t clientId);
// Whether their puppets of one type keep our cosmetics off their cosmetics textures with pristine
// blockers: not for My Own, and only while something of ours could land there. With
// `allTextures`, whether every other texture of theirs gets one too (our texture packs and mods'
// textures), unless Allow My Texture Packs on Other Players is on.
bool blockOwnLooks(uint32_t clientId, LookType type, bool allTextures = false);
// What we read from the cosmetics mod (nullopt: not active or unreadable) and what we last sent.
const std::optional<CosmeticsValues>& localCosmetics();
bool cosmeticsSent();
const std::optional<CosmeticsValues>& sentCosmetics();
// The `looks` of the manifest we last sent.
const nlohmann::json& sentLooks();

// The sender may use other settings than the ones our recolour knows.
bool cosmeticsApproximate(const CosmeticsValues& values);
// The Looks tab's lines and the cosmetics part of the Players tab's row tag ("" for none).
std::string cosmeticsStatusText();
std::string cosmeticsPlayerTag(uint32_t clientId);

}  // namespace twili::looks
