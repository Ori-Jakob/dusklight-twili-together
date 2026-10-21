#pragma once

// A teammate's cosmetics on their puppets (CosmeticsRecolor.cpp): our own recolour of the
// textures the cosmetics mod colours that still match the disc, registered as POINTER
// replacements on each puppet's own copies, and pristine POINTER blockers that keep our
// cosmetics, texture packs and mods' textures off them (the cosmetics textures, and every other
// private texture of the puppet that a content key of ours matches).

#include "fx/PlayerRecolorMath.hpp"
#include "looks/Cosmetics.hpp"
#include "looks/Slots.hpp"

#include <dolphin/gx.h>

#include <array>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

class J3DModelData;
class J3DTexture;

namespace twili {

enum class RecolorSetId : uint8_t;

// The puppet models whose textures a cosmetics colour targets.
enum class CosmeticsModel : uint8_t {
    KokiriBody,
    KokiriHead,
    KokiriSword,
    KokiriBoots,
    ZoraBody,
    ZoraHead,
    ZoraSword,
    ZoraBoots,
    MagicHead,
    MagicSword,
    MagicBoots,
    CasualHead,
    CasualSword,
    CasualBoots,
    SwordA,
    SwordM,
    Boomerang,
    Spinner,
    Wolf,
    Horse,
    Count,
};

enum class CosmeticsTexState : uint8_t { None, Recolor, Block };

// What the autotest reads.
inline constexpr uint8_t kNoCosmeticsVar = 0xFF;
struct CosmeticsTexProbe {
    const char* texture = nullptr;
    uint8_t var = 0;                // looks::CosmeticsVarId, kNoCosmeticsVar for the other textures
    looks::LookType type = looks::LookType::Link;
    CosmeticsTexState state = CosmeticsTexState::None;
    int8_t vanilla = -1;            // 1 the disc's bytes, 0 changed or uncheckable, -1 not yet
    uint32_t color = 0;             // Recolor: 0xRRGGBB
    Hsv mean{};                     // Recolor: of the replacement's colours
    int source = -1;                // ModTextureSource the renderer picks, -1 without ModsService
};
struct CosmeticsProbe {
    bool bound = false;
    // The vanilla keys: 0 not read yet, 1 read from the disc, 2 no ModsService, 3 reading failed
    // (2 and 3: nothing is recoloured)
    uint8_t keys = 0;
    // Targets whose texture the disc's model has, of all targets.
    uint16_t keysFound = 0, keysTotal = 0;
    uint16_t recolored = 0, blocked = 0;
    uint8_t hairBase = 0, hairTips = 0;
    std::vector<CosmeticsTexProbe> tex;
};

// Per look type of one puppet: the cosmetics its owner's puppets wear (null: nothing of theirs),
// and bits per type for pristine blockers on its cosmetics textures and on those of its others
// that a content key of ours matches (looks::ownTextureMatches).
struct PuppetLookState {
    std::array<const looks::CosmeticsValues*, looks::kLookTypeCount> shown{};
    uint8_t block = 0;
    uint8_t blockAll = 0;
};

// A model of the puppet's private archives, for the blockers on all its textures.
struct PrivateModel {
    J3DModelData* data = nullptr;
    looks::LookType type = looks::LookType::Link;
};

class PuppetCosmetics {
public:
    using Models = std::array<J3DModelData*, static_cast<size_t>(CosmeticsModel::Count)>;
    // Holds whether our in-place tint has rewritten these texels right now.
    using TintedFn = std::function<bool(const uint8_t* image)>;

    // In createHeap, while every texture is still as loaded.
    void bind(const Models& models, uint32_t clientId, std::span<const PrivateModel> others = {});
    // Before the archives holding the textures go away.
    void release();
    // Once per tick.
    void update(const PuppetLookState& state, const TintedFn& tinted);
    void probe(CosmeticsProbe& out) const;
    bool bound() const { return !mEntries.empty(); }

private:
    struct Entry {
        uint8_t target = 0;  // kNoCosmeticsVar for the other textures
        uint8_t var = 0;
        looks::LookType type = looks::LookType::Link;
        const char* name = nullptr;
        J3DTexture* tex = nullptr;
        uint16_t texNo = 0;
        const uint8_t* image = nullptr;
        uint8_t format = 0;
        uint16_t width = 0, height = 0;
        uint8_t mips = 1;
        bool palette = false;
        uint32_t bytes = 0;
        uint64_t hash = 0;  // at bind
        uint64_t key = 0;   // the other textures: XXH64 of the base mip at bind
        uint32_t keyRevision = ~0u;
        bool matched = false;
        int8_t vanilla = -1;
        uint64_t handle = 0;
        CosmeticsTexState state = CosmeticsTexState::None;
        uint32_t color = 0;
        Hsv mean{};
    };
    bool registerEntry(Entry& e, CosmeticsTexState state, uint32_t color);

    std::vector<Entry> mEntries;
    uint32_t mClientId = 0;
    uint32_t mLogged = ~0u;
};

// The tint's recolour sets a teammate's cosmetics replace: Kokiri, Zora and wolf, for a colour
// set in them. Magic Armor and the Ordon clothes keep the tint.
bool cosmeticsReplaceSet(const looks::CosmeticsValues* shown, RecolorSetId set);
bool cosmeticsReplaceHorse(const looks::CosmeticsValues* shown);

// Midna's hair TEV colours for palette indices (0: the game's own).
struct MidnaHairColours {
    GXColorS10 normal, big, lightBig;
    GXColor normalK, bigK, lightNormalK, normalK2, lightNormalK2, lightBigK2;
};
MidnaHairColours midnaHairColours(uint32_t base, uint32_t tips);

// Our recolour of one texel colour for a cosmetics colour: its luma, overlaid with the colour.
Rgb cosmeticsRgb(const Rgb& texel, const Rgb& color);

// The disc's own texture keys, read on a worker thread once connected.
void warmUpCosmeticsKeys();
void shutdownCosmeticsKeys();

// The recolour, palette and readers' unit checks (autotest cosmeticsSelfTest).
std::string cosmeticsSelfTest();

}  // namespace twili
