// Freestanding items a teammate took (field items with a save bit, heart pieces and containers, the
// Ordon Sword, golden wolf items, the fishing hole's heart piece): they vanish here too, and one whose
// check a teammate took is never placed. Each placed check (these, chests, Poe souls, NPC heart
// pieces) and each other named give reaches each of us once.

#include "sync/WorldSyncState.hpp"

#include "core/Log.hpp"
#include "story/StoryTypes.hpp"

#include "d/actor/d_a_mg_rod.h"
#include "d/actor/d_a_obj_item.h"
#include "d/actor/d_a_obj_life_container.h"
#include "d/actor/d_a_obj_sword.h"
#include "f_op/f_op_actor_mng.h"
#include "f_pc/f_pc_name.h"

#include <fmt/format.h>
#include <mods/items.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <vector>

namespace twili::sync {
namespace detail {
namespace {

constexpr int kNoBit = 0xFF;
constexpr int kMemoryItemBase = dSv_info_c::MEMORY_ITEM;
constexpr int kZoneItemBase = kMemoryItemBase + dSv_info_c::DAN_ITEM;
constexpr int kOneZoneItemBase = kZoneItemBase + dSv_info_c::ZONE_ITEM;
constexpr int kItemBitEnd = kOneZoneItemBase + dSv_info_c::ONEZONE_ITEM;
constexpr int kDanSwitchBase = dSv_info_c::MEMORY_SWITCH;
constexpr int kZoneSwitchBase = kDanSwitchBase + dSv_info_c::DAN_SWITCH;
// fopAcM_createItemForBoss gives boss heart containers this memory bit.
constexpr int kBossItemBit = 0x9F;
// The fishing hole's heart piece (its htPiece and the rod's fopAcM_onItem use this memory bit).
constexpr const char* kFishingStage = "F_SP127";
constexpr int kFishingHeartBit = 0x80;
// The Goron's heart piece after the springwater rush: a life container with this Hyrule Field bit.
constexpr const char* kSpringwaterStage = "F_SP121";
constexpr int kSpringwaterHeartBit = 0x80;

// NPC and minigame heart pieces, by the event bit that records them (their giver checks it).
struct RewardCheck {
    const char* name;
    uint16_t flag;
};
constexpr RewardCheck kRewardChecks[] = {
    {ITEM_CHECK_GOATS_REWARD, dSv_event_flag_c::F_0539},
    {ITEM_CHECK_ARCHERY_REWARD, dSv_event_flag_c::M_042},
    {ITEM_CHECK_PRAYER_REWARD, dSv_event_flag_c::F_0294},
    {ITEM_CHECK_PLUMM_REWARD, dSv_event_flag_c::F_286},
    {ITEM_CHECK_SNOWBOARD_REWARD, dSv_event_flag_c::F_0483},
};

// Names that parsePlacedCheck turns down under these are shared by many things; shops sell again.
constexpr const char* kNotNamedPrefixes[] = {ITEM_CHECK_FREESTANDING_PREFIX, ITEM_CHECK_BOSS_PREFIX,
    ITEM_CHECK_POE_PREFIX, ITEM_CHECK_GOLDEN_WOLF_PREFIX, ITEM_CHECK_CHEST_PREFIX,
    ITEM_CHECK_SHOP_PREFIX};
// Ticks a busy item stays on the retry list (a boomerang or clawshot pull is shorter).
constexpr int kPendingTicks = 30 * 30;

enum class Kind { None, Item, Life, Sword };
enum class Use { Idle, Carried, Ours, Gone };

// Item bits, switches and TBOX bits a teammate set (not already set), by scopeKey; event bits.
std::set<uint32_t> s_remoteBits;
std::set<uint32_t> s_remoteSwitches;
std::set<uint32_t> s_remoteTboxes;
std::set<uint16_t> s_remoteEvents;
// Checks (checkKey) a teammate's GIVE_ITEM granted us, and checks we took ourselves.
std::set<std::string> s_teamChecks;
std::set<std::string> s_ownChecks;

struct Pending {
    fpc_ProcID id;
    int ticksLeft;
};
std::vector<Pending> s_pending;

bool isZoneBit(int bit) {
    return bit >= kZoneItemBase;
}

uint32_t scopeKey(int saveTbl, int bit, int room) {
    if (isZoneBit(bit)) {
        return 3u << 24 | static_cast<uint32_t>(room & 0xFF) << 8 | static_cast<uint32_t>(bit);
    }
    const uint32_t scope = bit < kMemoryItemBase ? 2u : 1u;
    return scope << 24 | static_cast<uint32_t>(saveTbl & 0xFF) << 16 | static_cast<uint32_t>(bit);
}

uint32_t switchKey(int saveTbl, int no, int room) {
    if (no >= kZoneSwitchBase) {
        return 3u << 24 | static_cast<uint32_t>(room & 0xFF) << 8 | static_cast<uint32_t>(no);
    }
    const uint32_t scope = no < kDanSwitchBase ? 1u : 2u;
    return scope << 24 | static_cast<uint32_t>(saveTbl & 0xFF) << 16 | static_cast<uint32_t>(no);
}

uint32_t tboxKey(int saveTbl, int no) {
    return static_cast<uint32_t>(saveTbl & 0xFF) << 8 | static_cast<uint32_t>(no & 0xFF);
}

bool inStage(const char* stage) {
    const char* ours = dComIfGp_getStartStageName();
    return ours != nullptr && stage != nullptr && stage[0] != '\0' &&
           std::strncmp(ours, stage, 8) == 0;
}

bool inFishingHole() {
    return inStage(kFishingStage);
}

dmg_rod_class* fishingRod() {
    return reinterpret_cast<dmg_rod_class*>(fopAcM_SearchByName(fpcNm_MG_ROD_e));
}

Kind kindOf(fopAc_ac_c* actor) {
    switch (fopAcM_GetName(actor)) {
    case fpcNm_ITEM_e: return Kind::Item;
    case fpcNm_Obj_LifeContainer_e: return Kind::Life;
    case fpcNm_Obj_Sword_e: return Kind::Sword;
    default: return Kind::None;
    }
}

// One still being created is left alone: its create checks the bit again.
Kind liveKind(fopAc_ac_c* actor) {
    const Kind kind = kindOf(actor);
    return kind != Kind::None && fopAcM_IsExecuting(fopAcM_GetID(actor)) ? kind : Kind::None;
}

// A golden wolf's item (the randomizer's) goes by its event bit; -1 for any other actor.
int goldenWolfFlag(fopAc_ac_c* actor, Kind kind) {
    if (kind != Kind::Life || !static_cast<daObjLife_c*>(actor)->mGoldenWolfItem) {
        return -1;
    }
    return static_cast<u16>(static_cast<daObjLife_c*>(actor)->field_0x938);
}

int bitOf(fopAc_ac_c* actor, Kind kind) {
    switch (kind) {
    case Kind::Item: return static_cast<int>(daItem_prm::getItemBitNo(static_cast<daItem_c*>(actor)));
    case Kind::Life:
        return goldenWolfFlag(actor, kind) >= 0 ? kNoBit :
                                                  static_cast<daObjLife_c*>(actor)->getSaveBitNo();
    case Kind::Sword: return static_cast<daObjSword_c*>(actor)->getItemBit();
    default: return kNoBit;
    }
}

// The fishing hole's heart piece, hooked on our lure (the rod holds it in WAIT_2).
bool onOurLure(fopAc_ac_c* actor, Kind kind) {
    return kind == Kind::Life && inFishingHole() &&
           static_cast<daObjLife_c*>(actor)->chkStatus(daObjLife_c::STATUS_WAIT_2_e);
}

Use useOf(fopAc_ac_c* actor, Kind kind) {
    const bool pulled = fopAcM_checkHookCarryNow(actor) != 0;
    switch (kind) {
    case Kind::Item: {
        auto* item = static_cast<daItem_c*>(actor);
        if (item->checkFlag(daItem_c::FLAG_DELETE_ITEM_e)) {
            return Use::Gone;
        }
        if (item->checkFlag(daItem_c::FLAG_INIT_GET_ITEM_e)) {
            return Use::Ours;
        }
        switch (item->getStatus()) {
        case daItem_c::STATUS_NORMAL_e:
        case daItem_c::STATUS_SW_ON_WAIT_e:
        case daItem_c::STATUS_BOOM_HIT_WAIT_e:
            return pulled ? Use::Carried : Use::Idle;
        case daItem_c::STATUS_ENEMY_CARRY_e:
        case daItem_c::STATUS_BOOMERANG_CARRY_e:
            return Use::Carried;
        default:
            return Use::Ours;
        }
    }
    case Kind::Life: {
        auto* life = static_cast<daObjLife_c*>(actor);
        if (life->chkStatus(daObjLife_c::STATUS_WAIT_e) ||
            life->chkStatus(daObjLife_c::STATUS_SW_ON_WAIT_e))
        {
            return pulled ? Use::Carried : Use::Idle;
        }
        // Reeled in, the rod's heart scene gives it; still on the line it can go.
        if (onOurLure(actor, kind)) {
            const dmg_rod_class* rod = fishingRod();
            return rod != nullptr && rod->field_0x10a9 == 2 ? Use::Ours : Use::Idle;
        }
        // WAIT_2 is held by another actor.
        if (life->chkStatus(daObjLife_c::STATUS_BOOMERANG_CARRY_e) ||
            life->chkStatus(daObjLife_c::STATUS_WAIT_2_e))
        {
            return Use::Carried;
        }
        return Use::Ours;
    }
    case Kind::Sword: {
        auto* sword = static_cast<daObjSword_c*>(actor);
        if (sword->mStatus != 0) {
            return Use::Ours;
        }
        return fopAcM_checkCarryNow(sword) ? Use::Carried : Use::Idle;
    }
    default:
        return Use::Gone;
    }
}

void removeActor(fopAc_ac_c* actor, Kind kind) {
    if (kind == Kind::Item) {
        static_cast<daItem_c*>(actor)->setFlag(daItem_c::FLAG_DELETE_ITEM_e);
    }
    // The line goes slack: no heart scene (its message would grant the randomizer's item).
    if (onOurLure(actor, kind)) {
        if (dmg_rod_class* rod = fishingRod()) {
            rod->field_0x10a9 = 0;
        }
    }
    fopAcM_delete(actor);
}

void addPending(fpc_ProcID id) {
    for (const Pending& p : s_pending) {
        if (p.id == id) {
            return;
        }
    }
    s_pending.push_back({id, kPendingTicks});
}

// What a removal or search matches: an item bit (room only for zone bits) or a golden wolf flag.
struct Match {
    int bit = kNoBit;
    int room = -1;  // -1: any room
    int wolfFlag = -1;
};

bool matches(fopAc_ac_c* actor, Kind kind, const Match& match) {
    if (match.wolfFlag >= 0) {
        return goldenWolfFlag(actor, kind) == match.wolfFlag;
    }
    return bitOf(actor, kind) == match.bit &&
           (match.room < 0 || fopAcM_GetHomeRoomNo(actor) == match.room);
}

void* removeMatching(void* proc, void* data) {
    auto* actor = static_cast<fopAc_ac_c*>(proc);
    const Kind kind = liveKind(actor);
    const auto* match = static_cast<const Match*>(data);
    if (kind == Kind::None || !matches(actor, kind, *match)) {
        return nullptr;
    }
    switch (useOf(actor, kind)) {
    case Use::Idle:
        TwiliLog.info("[sync] item {} (bit {}, wolf flag {}) taken by a teammate: removed",
            fopAcM_GetParam(actor) & 0xFF, match->bit, match->wolfFlag);
        removeActor(actor, kind);
        state().stats.freestandingRemoved++;
        break;
    case Use::Carried:
        addPending(fopAcM_GetID(actor));
        break;
    default:
        break;
    }
    return nullptr;
}

void removeTaken(const Match& match) {
    Match copy = match;
    fopAcM_Search(&removeMatching, &copy);
}

struct Busy {
    Match match;
    bool ours = false;
};

void* findOurs(void* proc, void* data) {
    auto* actor = static_cast<fopAc_ac_c*>(proc);
    const Kind kind = liveKind(actor);
    auto* busy = static_cast<Busy*>(data);
    if (kind != Kind::None && matches(actor, kind, busy->match) && useOf(actor, kind) == Use::Ours) {
        busy->ours = true;
        return actor;
    }
    return nullptr;
}

bool oursNow(const Match& match) {
    Busy busy{match};
    fopAcM_Search(&findOurs, &busy);
    return busy.ours;
}

// "<prefix><stage>:<no>"; no stage for an empty-stage prefix ("golden_wolf:<flag>").
bool parseStageNo(const char* check, const char* prefix, bool withStage, std::string& stage,
    int& no) {
    const size_t len = std::strlen(prefix);
    if (std::strncmp(check, prefix, len) != 0) {
        return false;
    }
    const char* number = check + len;
    if (withStage) {
        const char* colon = std::strchr(number, ':');
        if (colon == nullptr || colon == number) {
            return false;
        }
        stage.assign(number, colon);
        number = colon + 1;
    }
    char* end = nullptr;
    const long value = std::strtol(number, &end, 10);
    if (end == number || *end != '\0' || value < 0 || value > 0xFFFF) {
        return false;
    }
    no = static_cast<int>(value);
    return true;
}

// One name per check: the boss bit's freestanding name is its boss check, and the fishing hole's
// heart piece is the rod's check whichever way it was taken.
std::string checkKey(const PlacedCheck& c) {
    switch (c.kind) {
    case CheckKind::Freestanding:
        if (c.no == kBossItemBit) {
            return ITEM_CHECK_BOSS_PREFIX + c.stage;
        }
        if (c.no == kFishingHeartBit && c.stage == kFishingStage) {
            return ITEM_CHECK_FISHING_HEART_PIECE;
        }
        return fmt::format("{}{}:{}", ITEM_CHECK_FREESTANDING_PREFIX, c.stage, c.no);
    case CheckKind::Boss: return ITEM_CHECK_BOSS_PREFIX + c.stage;
    case CheckKind::Fishing: return ITEM_CHECK_FISHING_HEART_PIECE;
    case CheckKind::Poe: return fmt::format("{}{}:{}", ITEM_CHECK_POE_PREFIX, c.stage, c.no);
    case CheckKind::GoldenWolf: return fmt::format("{}{}", ITEM_CHECK_GOLDEN_WOLF_PREFIX, c.no);
    case CheckKind::Chest: return fmt::format("{}{}:{}", ITEM_CHECK_CHEST_PREFIX, c.stage, c.no);
    case CheckKind::Reward:
        for (const RewardCheck& r : kRewardChecks) {
            if (r.flag == c.no) {
                return r.name;
            }
        }
        return {};
    default: return {};
    }
}

// The check a freestanding actor stands for.
PlacedCheck checkOfActor(fopAc_ac_c* actor, Kind kind) {
    PlacedCheck c;
    if (const int flag = goldenWolfFlag(actor, kind); flag >= 0) {
        c.kind = CheckKind::GoldenWolf;
        c.no = flag;
        return c;
    }
    const char* stage = dComIfGp_getStartStageName();
    c.kind = CheckKind::Freestanding;
    c.stage = stage != nullptr ? stage : "";
    c.no = bitOf(actor, kind);
    return c;
}

bool rawSwitch(int saveTbl, int no) {
    if (no < kDanSwitchBase) {
        const dSv_memBit_c* mem = memBitForSaveTbl(saveTbl);
        return mem != nullptr && ((static_cast<u32>(mem->mSwitch[no >> 5]) >> (no & 31)) & 1u) != 0;
    }
    if (no < kZoneSwitchBase) {
        const dSv_danBit_c* dan = danBitForSaveTbl(saveTbl);
        const int i = no - kDanSwitchBase;
        return dan != nullptr && ((dan->mSwitch[i >> 5] >> (i & 31)) & 1u) != 0;
    }
    return false;
}

bool rawTbox(int saveTbl, int no) {
    const dSv_memBit_c* mem = memBitForSaveTbl(saveTbl);
    return mem != nullptr && no >= 0 && no < TBOX_MAX &&
           ((static_cast<u32>(mem->mTbox[no >> 5]) >> (no & 31)) & 1u) != 0;
}

bool rawEvent(int no) {
    return (dComIfGs_getSaveInfo()->getEvent().mEvent[(no >> 8) & 0xFF] & static_cast<u8>(no)) != 0;
}

int saveTblOf(const PlacedCheck& c, int packetSaveTbl) {
    if (c.kind == CheckKind::Fishing || (c.kind != CheckKind::GoldenWolf && packetSaveTbl < 0)) {
        return story::stageSaveTbl(c.stage.c_str());
    }
    return packetSaveTbl;
}

// Its flag is set and no teammate set it: we took it (maybe offline or before a restart).
bool tookBefore(const PlacedCheck& c, int saveTbl) {
    switch (c.kind) {
    case CheckKind::Freestanding:
    case CheckKind::Boss:
    case CheckKind::Fishing:
        return itemBitSet(saveTbl, c.no, -1) && s_remoteBits.count(scopeKey(saveTbl, c.no, -1)) == 0;
    case CheckKind::Poe:
        return rawSwitch(saveTbl, c.no) && s_remoteSwitches.count(switchKey(saveTbl, c.no, -1)) == 0;
    case CheckKind::GoldenWolf:
    case CheckKind::Reward:
        return rawEvent(c.no) && s_remoteEvents.count(static_cast<uint16_t>(c.no)) == 0;
    case CheckKind::Chest:
        return rawTbox(saveTbl, c.no) && s_remoteTboxes.count(tboxKey(saveTbl, c.no)) == 0;
    default:
        return false;
    }
}

// We are taking it right now: our pickup, Poe kill or chest open will give it.
bool takingNow(const PlacedCheck& c) {
    switch (c.kind) {
    case CheckKind::Freestanding:
    case CheckKind::Boss:
    case CheckKind::Fishing:
        return inStage(c.stage.c_str()) && oursNow(Match{c.no});
    case CheckKind::Poe:
        return inStage(c.stage.c_str()) && poeDyingForUs(c.no);
    case CheckKind::GoldenWolf:
        return oursNow(Match{kNoBit, -1, c.no});
    case CheckKind::Chest:
        return inStage(c.stage.c_str()) && chestOpeningForUs(c.no);
    default:
        return false;
    }
}

void removeTakenBy(const PlacedCheck& c) {
    switch (c.kind) {
    case CheckKind::Freestanding:
    case CheckKind::Boss:
    case CheckKind::Fishing:
        if (inStage(c.stage.c_str())) {
            removeTaken(Match{c.no});
        }
        break;
    case CheckKind::Poe:
        if (inStage(c.stage.c_str())) {
            removeTakenPoes(c.no, -1);
        }
        break;
    case CheckKind::GoldenWolf:
        removeTaken(Match{kNoBit, -1, c.no});
        removeTakenGoldenWolves(static_cast<uint16_t>(c.no));
        break;
    default:
        // A chest opens through its TBOX bit.
        break;
    }
}

}  // namespace

bool parsePlacedCheck(const char* check, PlacedCheck& out) {
    out = {};
    if (check == nullptr) {
        return false;
    }
    std::string stage;
    int no = -1;
    if (parseStageNo(check, ITEM_CHECK_FREESTANDING_PREFIX, true, stage, no)) {
        out.kind = no < kZoneItemBase ? CheckKind::Freestanding : CheckKind::None;
    } else if (std::strncmp(check, ITEM_CHECK_BOSS_PREFIX, sizeof(ITEM_CHECK_BOSS_PREFIX) - 1) == 0) {
        stage = check + sizeof(ITEM_CHECK_BOSS_PREFIX) - 1;
        no = kBossItemBit;
        out.kind = CheckKind::Boss;
    } else if (parseStageNo(check, ITEM_CHECK_POE_PREFIX, true, stage, no)) {
        // Without a switch every such Poe shares the name.
        out.kind = no < kNoBit ? CheckKind::Poe : CheckKind::None;
    } else if (parseStageNo(check, ITEM_CHECK_GOLDEN_WOLF_PREFIX, false, stage, no)) {
        out.kind = no != 0xFFFF ? CheckKind::GoldenWolf : CheckKind::None;
    } else if (std::strcmp(check, ITEM_CHECK_FISHING_HEART_PIECE) == 0) {
        stage = kFishingStage;
        no = kFishingHeartBit;
        out.kind = CheckKind::Fishing;
    } else if (parseStageNo(check, ITEM_CHECK_CHEST_PREFIX, true, stage, no)) {
        out.kind = no < TBOX_MAX ? CheckKind::Chest : CheckKind::None;
    } else if (std::strcmp(check, ITEM_CHECK_GORON_SPRINGWATER_RUSH) == 0) {
        stage = kSpringwaterStage;
        no = kSpringwaterHeartBit;
        out.kind = CheckKind::Freestanding;
    } else {
        for (const RewardCheck& r : kRewardChecks) {
            if (std::strcmp(check, r.name) == 0) {
                no = r.flag;
                out.kind = CheckKind::Reward;
            }
        }
    }
    const bool stageless = out.kind == CheckKind::GoldenWolf || out.kind == CheckKind::Reward;
    if (out.kind == CheckKind::None || (!stageless && stage.empty())) {
        out = {};
        return false;
    }
    out.stage = std::move(stage);
    out.no = no;
    return true;
}

bool isNamedCheck(const char* check) {
    PlacedCheck placed;
    if (check == nullptr || check[0] == '\0' || parsePlacedCheck(check, placed)) {
        return false;
    }
    for (const char* prefix : kNotNamedPrefixes) {
        if (std::strncmp(check, prefix, std::strlen(prefix)) == 0) {
            return false;
        }
    }
    return true;
}

bool itemBitSet(int saveTbl, int bit, int room) {
    if (bit < 0 || bit >= kItemBitEnd) {
        return false;
    }
    if (bit < kMemoryItemBase) {
        const dSv_danBit_c* dan = danBitForSaveTbl(saveTbl);
        return dan != nullptr && ((dan->mItem[bit >> 5] >> (bit & 31)) & 1u) != 0;
    }
    if (bit < kZoneItemBase) {
        const dSv_memBit_c* mem = memBitForSaveTbl(saveTbl);
        const int no = bit - kMemoryItemBase;
        return mem != nullptr && ((static_cast<u32>(mem->mItem[0]) >> no) & 1u) != 0;
    }
    if (room < 0 || room >= 64) {
        return false;
    }
    const int zoneNo = dComIfGp_roomControl_getZoneNo(room);
    if (zoneNo < 0 || zoneNo >= dSv_info_c::ZONE_MAX) {
        return false;
    }
    const dSv_zoneBit_c& zone = dComIfGs_getSaveInfo()->getZone(zoneNo).getBit();
    return bit < kOneZoneItemBase ? zone.isItem(bit - kZoneItemBase) != FALSE :
                                    zone.isOneItem(bit - kOneZoneItemBase) != FALSE;
}

void onRemoteItemBit(int saveTbl, int bit, int room) {
    if (bit < 0 || bit >= kItemBitEnd) {
        return;
    }
    s_remoteBits.insert(scopeKey(saveTbl, bit, room));
    // Dungeon and zone bits only land while we are there; a memory bit may be another stage's.
    if (bit < kMemoryItemBase || isZoneBit(bit) || saveTbl == currentSaveTblNo()) {
        removeTaken(Match{bit, isZoneBit(bit) ? room : -1});
    }
}

void onRemoteSwitch(int saveTbl, int no, int room, bool live) {
    if (no < 0 || no >= kZoneSwitchBase + dSv_info_c::ZONE_SWITCH + dSv_info_c::ONEZONE_SWITCH) {
        return;
    }
    s_remoteSwitches.insert(switchKey(saveTbl, no, room));
    if (!live) {
        return;
    }
    // Our dying Poe's soul comes after it is gone: that soul is ours, the teammate's grant is not.
    const char* stage = dComIfGp_getStartStageName();
    if (stage != nullptr && no < kNoBit && poeDyingForUs(no)) {
        s_ownChecks.insert(checkKey(PlacedCheck{CheckKind::Poe, stage, no}));
    }
    removeTakenPoes(no, no >= kZoneSwitchBase ? room : -1);
}

void onRemoteTboxBit(int saveTbl, int no) {
    if (no >= 0 && no < TBOX_MAX) {
        s_remoteTboxes.insert(tboxKey(saveTbl, no));
    }
}

void onRemoteEventBit(uint16_t no, bool live) {
    s_remoteEvents.insert(no);
    if (live) {
        removeTaken(Match{kNoBit, -1, no});
        removeTakenGoldenWolves(no);
    }
}

bool acceptTeamCheck(const std::string& check, int packetSaveTbl) {
    PlacedCheck c;
    const bool placed = parsePlacedCheck(check.c_str(), c);
    if (!placed && !isNamedCheck(check.c_str())) {
        return true;
    }
    State& st = state();
    const std::string key = placed ? checkKey(c) : check;
    if (s_teamChecks.count(key) != 0 || s_ownChecks.count(key) != 0) {
        st.stats.grantsDeduped++;
        return false;
    }
    if (placed && (takingNow(c) || tookBefore(c, saveTblOf(c, packetSaveTbl)))) {
        s_ownChecks.insert(key);
        st.stats.grantsDeduped++;
        return false;
    }
    s_teamChecks.insert(key);
    if (placed) {
        removeTakenBy(c);
    }
    return true;
}

void noteOwnCheck(const char* check) {
    PlacedCheck c;
    if (parsePlacedCheck(check, c)) {
        s_ownChecks.insert(checkKey(c));
    } else if (isNamedCheck(check)) {
        s_ownChecks.insert(check);
    }
}

bool teamTookCheck(const char* check) {
    PlacedCheck c;
    if (parsePlacedCheck(check, c)) {
        return s_teamChecks.count(checkKey(c)) != 0;
    }
    return isNamedCheck(check) && s_teamChecks.count(check) != 0;
}

void tickFreestanding() {
    tickTakenActors();
    if (s_pending.empty()) {
        return;
    }
    for (size_t i = 0; i < s_pending.size();) {
        Pending& p = s_pending[i];
        fopAc_ac_c* actor = fopAcM_SearchByID(p.id);
        const Kind kind = actor != nullptr ? liveKind(actor) : Kind::None;
        const Use use = kind != Kind::None ? useOf(actor, kind) : Use::Gone;
        if (use == Use::Carried && --p.ticksLeft > 0) {
            i++;
            continue;
        }
        if (use == Use::Idle) {
            TwiliLog.info("[sync] item {} taken by a teammate: removed once let go",
                fopAcM_GetParam(actor) & 0xFF);
            removeActor(actor, kind);
            state().stats.freestandingRemoved++;
        }
        s_pending[i] = s_pending.back();
        s_pending.pop_back();
    }
}

void clearFreestandingStage() {
    s_pending.clear();
    std::erase_if(s_remoteBits, [](uint32_t key) { return (key >> 24) == 3u; });
    std::erase_if(s_remoteSwitches, [](uint32_t key) { return (key >> 24) == 3u; });
    clearTakenActors();
}

void resetFreestanding() {
    s_pending.clear();
    s_remoteBits.clear();
    s_remoteSwitches.clear();
    s_remoteTboxes.clear();
    s_remoteEvents.clear();
    s_teamChecks.clear();
    s_ownChecks.clear();
    clearTakenActors();
}

}  // namespace detail

using namespace detail;

bool blockTakenPickup(void* item) {
    auto* actor = static_cast<fopAc_ac_c*>(item);
    const Kind kind = liveKind(actor);
    if (kind == Kind::None) {
        return false;
    }
    const PlacedCheck check = checkOfActor(actor, kind);
    const bool wolf = check.kind == CheckKind::GoldenWolf;
    if (!wolf && check.no == kNoBit) {
        return false;
    }
    const int saveTbl = currentSaveTblNo();
    const int room = fopAcM_GetHomeRoomNo(actor);
    const std::string key = checkKey(check);
    const bool flagByTeammate =
        wolf ? rawEvent(check.no) && s_remoteEvents.count(static_cast<uint16_t>(check.no)) != 0 :
               itemBitSet(saveTbl, check.no, room) &&
                   s_remoteBits.count(scopeKey(saveTbl, check.no, room)) != 0;
    const bool placed = wolf || !isZoneBit(check.no);
    const bool taken = flagByTeammate || (placed && s_teamChecks.count(key) != 0);
    if (!taken) {
        if (placed) {
            s_ownChecks.insert(key);
        }
        return false;
    }
    TwiliLog.info("[sync] item {} ({}) was taken by a teammate: not given again",
        fopAcM_GetParam(actor) & 0xFF, key);
    state().stats.freestandingBlocked++;
    removeActor(actor, kind);
    return true;
}

bool blockTakenSpawn(void* life) {
    auto* actor = static_cast<fopAc_ac_c*>(life);
    if (kindOf(actor) != Kind::Life) {
        return false;
    }
    // Its own create refuses one whose flag reached us; this one only its check did.
    const PlacedCheck check = checkOfActor(actor, Kind::Life);
    if (check.kind != CheckKind::GoldenWolf && (check.no == kNoBit || isZoneBit(check.no))) {
        return false;
    }
    const std::string key = checkKey(check);
    if (s_teamChecks.count(key) == 0) {
        return false;
    }
    TwiliLog.info("[sync] item {} ({}) was taken by a teammate: not placed",
        fopAcM_GetParam(actor) & 0xFF, key);
    state().stats.spawnsBlocked++;
    return true;
}

}  // namespace twili::sync
