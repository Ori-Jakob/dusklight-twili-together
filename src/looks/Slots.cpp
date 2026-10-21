#include "looks/Slots.hpp"

#include <array>
#include <cctype>

namespace twili::looks {
namespace {

using T = LookType;

// Outfit archives: what DummyPlayer loads of each.
constexpr AllowedEntry kKmdl[] = {
    {"al.bmd", T::Link}, {"al_face.bmd", T::Link}, {"al_head.bmd", T::Link},
    {"al_hands.bmd", T::Link}, {"al_bootsh.bmd", T::Link}, {"al_swb.bmd", T::Link},
    {"al_kantera.bmd", T::Link}, {"ef_ktglow.bmd", T::Link}, {"ef_ktglow.btk", T::Link},
};
constexpr AllowedEntry kMmdl[] = {
    {"ml.bmd", T::Link}, {"al_face.bmd", T::Link}, {"ml_head.bmd", T::Link},
    {"al_hands.bmd", T::Link}, {"al_bootsh.bmd", T::Link}, {"al_swb.bmd", T::Link},
    {"al_kantera.bmd", T::Link}, {"ef_ktglow.bmd", T::Link}, {"ef_ktglow.btk", T::Link},
    {"ml_body_power_down.brk", T::Link}, {"ml_body_power_up_a.brk", T::Link},
    {"ml_body_power_up_b.brk", T::Link}, {"ml_head_power_down.brk", T::Link},
    {"ml_head_power_up_a.brk", T::Link}, {"ml_head_power_up_b.brk", T::Link},
};
constexpr AllowedEntry kZmdl[] = {
    {"zl.bmd", T::Link}, {"zl_face.bmd", T::Link}, {"zl_head.bmd", T::Link},
    {"al_hands.bmd", T::Link}, {"al_bootsh.bmd", T::Link}, {"al_swb.bmd", T::Link},
    {"al_kantera.bmd", T::Link}, {"ef_ktglow.bmd", T::Link}, {"ef_ktglow.btk", T::Link},
};
constexpr AllowedEntry kBmdl[] = {
    {"bl.bmd", T::Link}, {"al_face.bmd", T::Link}, {"bl_head.bmd", T::Link},
    {"bl_hands.bmd", T::Link}, {"al_bootsh.bmd", T::Link}, {"al_swb.bmd", T::Link},
    {"al_kantera.bmd", T::Link}, {"ef_ktglow.bmd", T::Link}, {"ef_ktglow.btk", T::Link},
};
// Link's gear in his private Alink copy: swords, sheaths and items in hand (DummyPlayer's
// models and their texture animations; skeletal animations are no look).
constexpr AllowedEntry kAlink[] = {
    {"al_swa.bmd", T::Link}, {"al_poda.bmd", T::Link}, {"al_swa.btk", T::Link},
    {"al_swm.bmd", T::Link}, {"al_podm.bmd", T::Link}, {"al_swm.btk", T::Link},
    {"al_swm.brk", T::Link}, {"al_boom.bmd", T::Link}, {"al_sp.bmd", T::Link},
    {"al_bomb.bmd", T::Link}, {"al_arrow.bmd", T::Link}, {"al_arrowb.bmd", T::Link},
    {"al_pachi_nut.bmd", T::Link}, {"pg.bmd", T::Link}, {"pb.bmd", T::Link},
    {"crod_ball.bmd", T::Link}, {"crod_ball.brk", T::Link}, {"crod_ball.btk", T::Link},
    {"ef_setboom.bmd", T::Link}, {"ef_setboom.btk", T::Link}, {"ef_shippu.bmd", T::Link},
    {"ef_shippu.btk", T::Link}, {"rod_ukia.bmd", T::Link}, {"rod_ukib.bmd", T::Link},
    {"rod_ukic.bmd", T::Link}, {"uki.bmd", T::Link}, {"uki_saki.bmd", T::Link},
    {"sfook.bmd", T::Link}, {"sango_fook.bmd", T::Link}, {"hk.bmd", T::Link},
    {"worm.bmd", T::Link},
};
constexpr AllowedEntry kHyShd[] = {{"al_sha.bmd", T::Link}};
constexpr AllowedEntry kCWShd[] = {{"al_shb.bmd", T::Link}};
constexpr AllowedEntry kSWShd[] = {{"al_shc.bmd", T::Link}};
// The wolf, and Midna riding him.
constexpr AllowedEntry kWmdl[] = {
    {"wl.bmd", T::Wolf}, {"wl_kusari.bmd", T::Wolf},
    {"md.bmd", T::Midna}, {"md_mask.bmd", T::Midna}, {"md_hands.bmd", T::Midna},
    {"md_hair_hand.bmd", T::Midna}, {"md_d_color.brk", T::Midna},
    {"md_mask_d_color.brk", T::Midna}, {"md_hands_d_color.brk", T::Midna},
    {"md_hair_hand_d_color.brk", T::Midna},
};
// Her own form, which most Midna mods change.
constexpr AllowedEntry kMidna[] = {
    {"s_md.bmd", T::Midna}, {"s_md_mask.bmd", T::Midna}, {"s_md_hands.bmd", T::Midna},
    {"s_md_hair_hand.bmd", T::Midna},
};
constexpr AllowedEntry kHorse[] = {
    {"hs.bmd", T::Epona}, {"hs_eye.btp", T::Epona}, {"tazuna.bti", T::Epona},
};

struct ArcDef {
    const char* name;
    const char* path;
    const char* alias;
    LookType type;
    std::span<const AllowedEntry> entries;
};

constexpr std::array<ArcDef, kLookArcCount> kArcs = {{
    {"Kmdl", "/res/Object/Kmdl.arc", "/res/Object/tt/base/Kmdl.arc", T::Link, kKmdl},
    {"Mmdl", "/res/Object/Mmdl.arc", "/res/Object/tt/base/Mmdl.arc", T::Link, kMmdl},
    {"Zmdl", "/res/Object/Zmdl.arc", "/res/Object/tt/base/Zmdl.arc", T::Link, kZmdl},
    {"Bmdl", "/res/Object/Bmdl.arc", "/res/Object/tt/base/Bmdl.arc", T::Link, kBmdl},
    {"Alink", "/res/Object/Alink.arc", "/res/Object/tt/base/Alink.arc", T::Link, kAlink},
    {"HyShd", "/res/Object/HyShd.arc", "/res/Object/tt/base/HyShd.arc", T::Link, kHyShd},
    {"CWShd", "/res/Object/CWShd.arc", "/res/Object/tt/base/CWShd.arc", T::Link, kCWShd},
    {"SWShd", "/res/Object/SWShd.arc", "/res/Object/tt/base/SWShd.arc", T::Link, kSWShd},
    {"Wmdl", "/res/Object/Wmdl.arc", "/res/Object/tt/base/Wmdl.arc", T::Wolf, kWmdl},
    {"Midna", "/res/Object/Midna.arc", "/res/Object/tt/base/Midna.arc", T::Midna, kMidna},
    {"Horse", "/res/Object/Horse.arc", "/res/Object/tt/base/Horse.arc", T::Epona, kHorse},
}};

constexpr const char* kTypeIds[kLookTypeCount] = {"link", "wolf", "midna", "epona"};
constexpr const char* kTypeNames[kLookTypeCount] = {"Link", "Wolf", "Midna", "Epona"};

const ArcDef& def(LookArc arc) {
    return kArcs[static_cast<size_t>(arc)];
}

}  // namespace

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); i++) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i])))
        {
            return false;
        }
    }
    return true;
}

const char* lookTypeId(LookType type) {
    return kTypeIds[static_cast<size_t>(type)];
}

const char* lookTypeName(LookType type) {
    return kTypeNames[static_cast<size_t>(type)];
}

std::optional<LookType> parseLookType(std::string_view id) {
    for (size_t i = 0; i < kLookTypeCount; i++) {
        if (id == kTypeIds[i]) {
            return static_cast<LookType>(i);
        }
    }
    return std::nullopt;
}

const char* arcName(LookArc arc) {
    return def(arc).name;
}

const char* arcDefaultPath(LookArc arc) {
    return def(arc).path;
}

const char* arcAliasPath(LookArc arc) {
    return def(arc).alias;
}

std::optional<LookArc> arcByName(std::string_view name) {
    for (size_t i = 0; i < kLookArcCount; i++) {
        if (iequals(name, kArcs[i].name)) {
            return static_cast<LookArc>(i);
        }
    }
    return std::nullopt;
}

std::optional<LookArc> arcByPath(std::string_view discPath) {
    for (size_t i = 0; i < kLookArcCount; i++) {
        if (iequals(discPath, kArcs[i].path)) {
            return static_cast<LookArc>(i);
        }
    }
    return std::nullopt;
}

LookType arcType(LookArc arc) {
    return def(arc).type;
}

std::optional<LookType> entryType(LookArc arc, std::string_view name) {
    for (const AllowedEntry& entry : def(arc).entries) {
        if (iequals(name, entry.name)) {
            return entry.type;
        }
    }
    return std::nullopt;
}

std::span<const AllowedEntry> allowedEntries(LookArc arc) {
    return def(arc).entries;
}

}  // namespace twili::looks
