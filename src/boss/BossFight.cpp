// Our own boss or mini-boss fight: found by the live actor in our room, kept until we leave the arena.

#include "boss/BossFight.hpp"

#include "core/LocalPlayer.hpp"
#include "core/Log.hpp"
#include "core/SaveGate.hpp"
#include "core/Session.hpp"
#include "sync/WorldSync.hpp"

#include "SSystem/SComponent/c_counter.h"
#include "d/d_com_inf_game.h"
#include "f_op/f_op_actor_mng.h"
#include "f_pc/f_pc_name.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstring>
#include <iterator>

namespace twili::boss {
namespace {

// The flag that says our own copy is already beaten (its actor then stays away or turns friendly).
enum class Beaten : uint8_t { None, Boss, MiniBoss };

// Rooms that place a boss or mini-boss (from the disc's stage data); room -1: the whole stage.
struct Place {
    const char* stage;
    int8_t room;
    Beaten beaten;
    int group;
};

constexpr int kZantGroup = 1;
constexpr int kGanonGroup = 2;

constexpr Place kPlaces[] = {
    {"D_MN05A", -1, Beaten::Boss, 0},      // Diababa
    {"D_MN05B", -1, Beaten::MiniBoss, 0},  // Ook
    {"D_MN04A", -1, Beaten::Boss, 0},      // Fyrus
    {"D_MN04B", -1, Beaten::MiniBoss, 0},  // Dangoro
    {"D_MN01A", -1, Beaten::Boss, 0},      // Morpheel
    {"D_MN01B", -1, Beaten::MiniBoss, 0},  // Deku Toad
    {"D_MN10A", -1, Beaten::Boss, 0},      // Stallord
    {"D_MN10B", -1, Beaten::MiniBoss, 0},  // Death Sword
    {"D_MN11A", -1, Beaten::Boss, 0},      // Blizzeta
    {"D_MN11B", -1, Beaten::MiniBoss, 0},  // Darkhammer
    {"D_MN06A", -1, Beaten::Boss, 0},      // Armogohma
    {"D_MN06B", -1, Beaten::MiniBoss, 0},  // Darknut
    {"D_MN07A", -1, Beaten::Boss, 0},      // Argorok
    {"D_MN07B", -1, Beaten::MiniBoss, 0},  // Aeralfos
    {"D_MN08B", -1, Beaten::None, 0},      // Phantom Zant
    {"D_MN08C", -1, Beaten::None, 0},
    {"D_MN08D", -1, Beaten::None, kZantGroup},
    {"D_MN09A", -1, Beaten::None, kGanonGroup},
    {"D_MN09B", -1, Beaten::None, kGanonGroup},
    {"D_MN09", 13, Beaten::MiniBoss, 0},  // King Bulblin
    {"D_MN09", 2, Beaten::None, 0},       // Darknuts
    {"D_MN09", 4, Beaten::None, 0},
    {"D_MN09", 12, Beaten::None, 0},
    {"D_MN09", 15, Beaten::None, 0},  // Aeralfos
    {"D_MN07", 15, Beaten::None, 0},
    {"D_SB01", 38, Beaten::None, 0},  // Cave of Ordeals
    {"D_SB01", 47, Beaten::None, 0},
    {"D_SB01", 48, Beaten::None, 0},
    {"F_SP118", 1, Beaten::None, 0},  // King Bulblin in the desert
    {"F_SP118", 2, Beaten::None, 0},
    {"F_SP115", 0, Beaten::None, 0},  // Twilit Bloat
};

// Linked stages without a boss actor of their own: the throne room around Zant's fight, the field
// scenes between Ganon's phases.
constexpr Place kGroupOnly[] = {
    {"D_MN08A", 10, Beaten::None, kZantGroup},
    {"D_MN09C", -1, Beaten::None, kGanonGroup},
};

constexpr int16_t kBosses[] = {
    fpcNm_B_BQ_e, fpcNm_E_FM_e, fpcNm_B_OB_e, fpcNm_B_DS_e, fpcNm_B_YO_e, fpcNm_B_GM_e,
    fpcNm_B_DR_e, fpcNm_B_ZANT_e, fpcNm_E_HZELDA_e, fpcNm_B_MGN_e, fpcNm_B_GND_e,
};
constexpr int16_t kMiniBosses[] = {
    fpcNm_E_MK_e, fpcNm_E_GOB_e, fpcNm_E_DT_e, fpcNm_E_VT_e, fpcNm_E_TH_e, fpcNm_B_TN_e,
    fpcNm_B_GG_e, fpcNm_E_PZ_e, fpcNm_E_RDB_e, fpcNm_E_YMB_e,
};
// Parts that order their own boss scenes.
constexpr int16_t kBossParts[] = {
    fpcNm_B_BH_e, fpcNm_B_OH_e, fpcNm_B_YOI_e, fpcNm_B_DRE_e, fpcNm_B_ZANTZ_e, fpcNm_B_ZANTS_e,
    fpcNm_B_ZANTM_e,
};

// Boss scenes by name: the clear and warp-out of daObjBossWarp_c, and the arrival after it.
constexpr const char* kBossEventNames[] = {
    "BOSSCLEAR01", "BOSSCLEAR02", "BOSSCLEAR03", "BOSSCLEAR05", "BOSSCLEAR06", "BOSSCLEAR07",
    "BOSS_WARPIN", "BOSS_WARPIN_M", "BOSSWARP_START",
};

// Class-b map events of docs/cutscenes.md: boss and mini-boss intros, phase changes and outros.
struct StageEvent {
    const char* stage;
    const char* name;
};
constexpr StageEvent kBossMapEvents[] = {
    {"D_MN04B", "R51_st2"},
    {"D_MN11A", "BOSSCLEAR05"},
    {"D_MN08A", "demo33_01"},
    {"D_MN08A", "demo25_01"},
    {"D_MN09A", "R51_start"},
    {"D_MN09A", "demo27_01"},
    {"D_MN09A", "demo28_01"},
    {"D_MN09A", "demo27_02"},
    {"D_MN09B", "r00_start"},
};

// Ticks between actor scans while we stand where a boss can be.
constexpr uint32_t kScanTicks = 5;

Arena s_arena;
bool s_fighting = false;
bool s_guard = true;

bool sameStage(const char* a, const char* b) {
    return a != nullptr && b != nullptr && std::strncmp(a, b, 8) == 0;
}

template <size_t N>
bool listed(const int16_t (&list)[N], int16_t profile) {
    return std::find(std::begin(list), std::end(list), profile) != std::end(list);
}

const Place* placeAt(const char* stage, int room) {
    for (const Place& p : kPlaces) {
        if (sameStage(p.stage, stage) && (p.room < 0 || p.room == room)) {
            return &p;
        }
    }
    return nullptr;
}

template <size_t N>
bool groupHas(const Place (&list)[N], int group, const char* stage, int room) {
    for (const Place& p : list) {
        if (p.group == group && sameStage(p.stage, stage) && (p.room < 0 || p.room == room)) {
            return true;
        }
    }
    return false;
}

bool inGroup(int group, const char* stage, int room) {
    return group != 0 &&
           (groupHas(kPlaces, group, stage, room) || groupHas(kGroupOnly, group, stage, room));
}

const char* currentStage() {
    const char* s = dComIfGp_getStartStageName();
    return s != nullptr ? s : "";
}

struct Search {
    int room;
    bool anyRoom;
};

bool beatenHere(Beaten beaten) {
    switch (beaten) {
    case Beaten::Boss:
        return dComIfGs_isStageBossEnemy() != FALSE;
    case Beaten::MiniBoss:
        return dComIfGs_isStageMiddleBoss() != FALSE;
    default:
        return false;
    }
}

void* findLiveBoss(void* proc, void* data) {
    auto* actor = static_cast<fopAc_ac_c*>(proc);
    const auto* want = static_cast<const Search*>(data);
    const int16_t profile = fopAcM_GetName(actor);
    if (!isBossProfile(profile)) {
        return nullptr;
    }
    if (!want->anyRoom && fopAcM_GetRoomNo(actor) != want->room) {
        return nullptr;
    }
    return fopAcM_IsExecuting(fopAcM_GetID(actor)) ? actor : nullptr;
}

void begin(const Place& place, fopAc_ac_c* boss, const char* stage, int room) {
    s_arena = Arena{};
    std::strncpy(s_arena.stage, stage, sizeof(s_arena.stage) - 1);
    s_arena.room = static_cast<int8_t>(place.room < 0 ? -1 : room);
    s_arena.saveTbl = Session::instance().currentSaveTblNo();
    s_arena.profile = fopAcM_GetName(boss);
    s_arena.group = place.group;
    s_fighting = true;
    TwiliLog.info("[boss] fight with {} in {} room {} (save table {}){}",
        fopAcM_getProcNameString(boss), stage, room, s_arena.saveTbl,
        s_guard ? "" : " (guard off for the test)");
}

void end(const char* why) {
    TwiliLog.info("[boss] fight in {} over ({})", s_arena.stage, why);
    s_fighting = false;
    // Before the story prompts that waited: what they offer may depend on these flags.
    sync::releaseBossFightHolds();
    s_arena = Arena{};
}

}  // namespace

bool fighting() {
    return s_fighting;
}

bool guarded() {
    return s_fighting && s_guard;
}

const Arena& arena() {
    return s_arena;
}

bool inArena(const char* stage, int room) {
    if (!s_fighting) {
        return false;
    }
    if (sameStage(s_arena.stage, stage) && (s_arena.room < 0 || s_arena.room == room)) {
        return true;
    }
    return inGroup(s_arena.group, stage, room);
}

bool fromArena(const nlohmann::json& packet) {
    const auto stage = packet.find("stageName");
    if (stage == packet.end() || !stage->is_string()) {
        return false;
    }
    return inArena(stage->get_ref<const std::string&>().c_str(), packet.value("roomNo", -1));
}

bool isBossProfile(int16_t profile) {
    return listed(kBosses, profile) || listed(kMiniBosses, profile);
}

bool isBossEvent(int16_t requester, const char* stage, const std::string& name) {
    if (requester >= 0 && (listed(kBosses, requester) || listed(kMiniBosses, requester) ||
                              listed(kBossParts, requester) || requester == fpcNm_Obj_BossWarp_e))
    {
        return true;
    }
    for (const char* n : kBossEventNames) {
        if (name == n) {
            return true;
        }
    }
    for (const StageEvent& e : kBossMapEvents) {
        if (name == e.name && sameStage(e.stage, stage)) {
            return true;
        }
    }
    return false;
}

void tick() {
    if (!isSaveLoaded()) {
        if (s_fighting) {
            end("no save");
        }
        return;
    }
    // Loads keep the state: a game over reloads the arena, a boss sequence crosses stages.
    if (local::liveLink() == nullptr || dComIfGp_isEnableNextStage() ||
        Session::instance().currentSaveTblNo() < 0)
    {
        return;
    }
    const char* stage = currentStage();
    const int room = dComIfGp_roomControl_getStayNo();
    if (s_fighting) {
        if (!inArena(stage, room)) {
            end("left the arena");
        }
        return;
    }
    const Place* place = placeAt(stage, room);
    if (place == nullptr || g_Counter.mCounter0 % kScanTicks != 0 || beatenHere(place->beaten)) {
        return;
    }
    Search search{room, place->room < 0};
    if (auto* boss = static_cast<fopAc_ac_c*>(fopAcM_Search(&findLiveBoss, &search))) {
        begin(*place, boss, stage, room);
    }
}

void shutdown() {
    s_fighting = false;
    s_arena = Arena{};
    s_guard = true;
}

#if TWILI_ENABLE_AUTOTEST
void setGuardForTest(bool on) {
    s_guard = on;
    TwiliLog.info("[boss] guard {} for the test", on ? "on" : "off");
}
#endif

}  // namespace twili::boss
