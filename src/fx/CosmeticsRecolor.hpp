#pragma once

// A teammate's cosmetics on their puppets (CosmeticsRecolor.cpp): our own recolour of the
// textures the cosmetics mod colours that still match the disc, registered as POINTER
// replacements on each puppet's own copies, and pristine POINTER blockers that keep our
// cosmetics and texture packs off them.

#include "fx/PlayerRecolorMath.hpp"
#include "looks/Cosmetics.hpp"

#include <dolphin/gx.h>

#include <array>
#include <cstdint>
#include <functional>
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
struct CosmeticsTexProbe {
    const char* texture = nullptr;
    uint8_t var = 0;                // looks::CosmeticsVarId
    CosmeticsTexState state = CosmeticsTexState::None;
    int8_t vanilla = -1;            // 1 the disc's bytes, 0 changed or uncheckable, -1 not yet
    uint32_t color = 0;             // Recolor: 0xRRGGBB
    Hsv mean{};                     // Recolor: of the replacement's colours
    int source = -1;                // ModTextureSource the renderer picks, -1 without ModsService
};
struct CosmeticsProbe {
    bool bound = false;
    // The vanilla keys: 0 not read yet, 1 read from the disc, 2 unavailable (every texture trusted)
    uint8_t keys = 0;
    uint16_t recolored = 0, blocked = 0;
    uint8_t hairBase = 0, hairTips = 0;
    std::vector<CosmeticsTexProbe> tex;
};

class PuppetCosmetics {
public:
    using Models = std::array<J3DModelData*, static_cast<size_t>(CosmeticsModel::Count)>;
    // Holds whether our in-place tint has rewritten these texels right now.
    using TintedFn = std::function<bool(const uint8_t* image)>;

    // In createHeap, while every texture is still as loaded.
    void bind(const Models& models, uint32_t clientId);
    // Before the archives holding the textures go away.
    void release();
    // Once per tick with what the owner's puppets wear (null: nothing of theirs), and whether the
    // rest gets pristine blockers.
    void update(const looks::CosmeticsValues* shown, bool block, const TintedFn& tinted);
    void probe(CosmeticsProbe& out) const;
    bool bound() const { return !mEntries.empty(); }

private:
    struct Entry {
        uint8_t target = 0;
        uint8_t var = 0;
        J3DTexture* tex = nullptr;
        uint16_t texNo = 0;
        const uint8_t* image = nullptr;
        uint8_t format = 0;
        uint16_t width = 0, height = 0;
        uint8_t mips = 1;
        bool palette = false;
        uint32_t bytes = 0;
        uint64_t hash = 0;  // at bind
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
