// A teammate's flags from our boss arena wait while we fight there: a kill, clear or heart container of
// theirs must not change our fight (doors, Midna's hints, switch-driven scenes, the restart point after
// a game over, our own heart container). Everything else keeps flowing. Applied once the fight ends.

#include "sync/WorldSyncState.hpp"

#include "boss/BossFight.hpp"
#include "core/Log.hpp"
#include "sync/RemoteApplyGuard.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <iterator>
#include <set>
#include <vector>

namespace twili::sync {
namespace detail {
namespace {

// STAGE_BOSS_ENEMY, STAGE_LIFE, STAGE_BOSS_DEMO, OOCCOO_NOTE (set with the boss bit), STAGE_BOSS_ENEMY_2.
constexpr uint8_t kBossBits = 0xF8;
// A teammate cannot grow this without bound.
constexpr size_t kMaxHeld = 4096;

constexpr int kMemorySwitchEnd = dSv_info_c::MEMORY_SWITCH;
constexpr int kDanSwitchEnd = kMemorySwitchEnd + dSv_info_c::DAN_SWITCH;
constexpr int kDanItemEnd = dSv_info_c::MEMORY_ITEM;
constexpr int kMemoryItemEnd = kDanItemEnd + dSv_info_c::DAN_ITEM;

enum Scope : uint32_t { kMemSwitch = 1, kDanSwitch, kMemItem, kDanItem, kTbox };

uint32_t key(Scope scope, int saveTbl, int no) {
    return scope << 24 | static_cast<uint32_t>(saveTbl & 0xFF) << 16 | static_cast<uint32_t>(no & 0xFFFF);
}

struct Held {
    // In arrival order; replayed through the same apply functions.
    std::vector<nlohmann::json> packets;
    // Bits those packets set, taken out of world-state merges meanwhile.
    std::set<uint32_t> bits;
    std::set<uint16_t> events;
    // From merges only: boss bits per stage table, and the highest max life.
    uint8_t dungeonBits[dSv_save_c::STAGE_MAX] = {};
    uint16_t maxLife = 0;
};

Held s_held;

void noteFlagBits(const nlohmann::json& packet) {
    const std::string category = packet.value("category", std::string{});
    const int no = packet.value("flagNo", -1);
    const int tbl = packet.value("saveTblNo", -1);
    if (no < 0 || tbl < 0 || tbl >= dSv_save_c::STAGE_MAX) {
        return;
    }
    if (category == "SWITCH") {
        if (no < kMemorySwitchEnd) {
            s_held.bits.insert(key(kMemSwitch, tbl, no));
        } else if (no < kDanSwitchEnd) {
            s_held.bits.insert(key(kDanSwitch, tbl, no - kMemorySwitchEnd));
        }
    } else if (category == "ITEM") {
        if (no < kDanItemEnd) {
            s_held.bits.insert(key(kDanItem, tbl, no));
        } else if (no < kMemoryItemEnd) {
            s_held.bits.insert(key(kMemItem, tbl, no - kDanItemEnd));
        }
    } else if (category == "TBOX" && no < TBOX_MAX) {
        s_held.bits.insert(key(kTbox, tbl, no));
    }
}

bool held(Scope scope, int tbl, int no) {
    return s_held.bits.count(key(scope, tbl, no)) != 0;
}

void keep(const std::string& type, const nlohmann::json& packet, const char* what) {
    if (s_held.packets.size() >= kMaxHeld) {
        TwiliLog.warn("[sync] boss fight: too many held packets, applying {} now", type);
        return;
    }
    nlohmann::json copy = packet;
    copy["type"] = type;
    s_held.packets.push_back(std::move(copy));
    state().stats.bossHeld++;
    TwiliLog.info("[sync] boss fight in {}: {} from client {} waits ({})", boss::arena().stage, type,
        packet.value("clientId", 0u), what);
}

bool isArenaCheck(const nlohmann::json& packet) {
    const auto check = packet.find("check");
    PlacedCheck c;
    return check != packet.end() && check->is_string() &&
           parsePlacedCheck(check->get_ref<const std::string&>().c_str(), c) &&
           boss::inArena(c.stage.c_str(), boss::arena().room);
}

void applyHeld(const nlohmann::json& packet) {
    const std::string type = packet.value("type", std::string{});
    if (type == "SET_FLAG") {
        applySetFlag(packet);
    } else if (type == "UNSET_FLAG") {
        applyUnsetFlag(packet);
    } else if (type == "SET_EVENT_BIT") {
        applySetEventBit(packet);
    } else if (type == "UNSET_EVENT_BIT") {
        applyUnsetEventBit(packet);
    } else if (type == "UPDATE_DUNGEON_ITEMS") {
        applyUpdateDungeonItems(packet);
    } else if (type == "GIVE_ITEM") {
        applyGiveItem(packet);
    }
}

}  // namespace

bool holdForBossFight(const std::string& type, const nlohmann::json& packet) {
    if (!boss::guarded()) {
        return false;
    }
    if (type == "SET_FLAG" || type == "UNSET_FLAG") {
        if (!boss::fromArena(packet)) {
            return false;
        }
        if (type == "SET_FLAG") {
            noteFlagBits(packet);
        }
        keep(type, packet, fmt::format("{} {}", packet.value("category", std::string("SWITCH")),
                               packet.value("flagNo", -1)).c_str());
        return true;
    }
    if (type == "SET_EVENT_BIT" || type == "UNSET_EVENT_BIT") {
        if (!boss::fromArena(packet)) {
            return false;
        }
        const unsigned no = packet.value("no", 0u);
        if (type == "SET_EVENT_BIT" && no <= 0xFFFF) {
            s_held.events.insert(static_cast<uint16_t>(no));
        }
        keep(type, packet, fmt::format("0x{:04X}", no).c_str());
        return true;
    }
    if (type == "UPDATE_DUNGEON_ITEMS") {
        const unsigned bits = packet.value("dungeonItemBits", 0u) & 0xFF;
        if (packet.value("saveTblNo", -1) != boss::arena().saveTbl || (bits & kBossBits) == 0) {
            return false;
        }
        nlohmann::json boss = packet;
        boss["keyDelta"] = 0;
        boss["dungeonItemBits"] = bits & kBossBits;
        keep(type, boss, fmt::format("boss bits 0x{:02X}", bits & kBossBits).c_str());
        // Keys, map, compass and boss key go on now.
        if (packet.value("keyDelta", 0) != 0 || (bits & ~kBossBits) != 0) {
            nlohmann::json rest = packet;
            rest["dungeonItemBits"] = bits & ~kBossBits;
            applyUpdateDungeonItems(rest);
        }
        return true;
    }
    if (type == "GIVE_ITEM") {
        // The arena's heart container: its grant heals fully, and our own one comes after the fight.
        if (!isArenaCheck(packet)) {
            return false;
        }
        keep(type, packet, packet.value("check", std::string{}).c_str());
        return true;
    }
    return false;
}

void holdMergeForBossFight(dSv_save_c& remote, dSv_save_c& local) {
    if (!boss::guarded()) {
        return;
    }
    const int tbl = boss::arena().saveTbl;
    if (tbl >= 0 && tbl < dSv_save_c::STAGE_MAX) {
        dSv_memBit_c& r = remote.mSave[tbl].getBit();
        const uint8_t bossBits = r.mDungeonItem & kBossBits;
        if (bossBits != 0) {
            s_held.dungeonBits[tbl] |= bossBits;
            r.mDungeonItem &= static_cast<u8>(~kBossBits);
        }
        for (int no = 0; no < kMemorySwitchEnd; no++) {
            if (held(kMemSwitch, tbl, no)) {
                r.mSwitch[no >> 5] = static_cast<u32>(r.mSwitch[no >> 5]) & ~(1u << (no & 31));
            }
        }
        for (int no = 0; no < dSv_info_c::DAN_ITEM; no++) {
            if (held(kMemItem, tbl, no)) {
                r.mItem[0] = static_cast<u32>(r.mItem[0]) & ~(1u << no);
            }
        }
        for (int no = 0; no < TBOX_MAX; no++) {
            if (held(kTbox, tbl, no)) {
                r.mTbox[no >> 5] = static_cast<u32>(r.mTbox[no >> 5]) & ~(1u << (no & 31));
            }
        }
    }
    for (const uint16_t no : s_held.events) {
        remote.mEvent.mEvent[no >> 8] &= static_cast<uint8_t>(~(no & 0xFF));
    }
    // A teammate's heart container would raise our max life before our own one is picked up.
    dSv_player_status_a_c& ra = remote.getPlayer().getPlayerStatusA();
    const u16 ours = local.getPlayer().getPlayerStatusA().getMaxLife();
    if (ra.getMaxLife() > ours) {
        s_held.maxLife = std::max<uint16_t>(s_held.maxLife, ra.getMaxLife());
        ra.setMaxLife(ours);
    }
}

void holdDanForBossFight(dSv_danBit_c& remote) {
    if (!boss::guarded() || remote.mStageNo != boss::arena().saveTbl) {
        return;
    }
    const int tbl = remote.mStageNo;
    for (int no = 0; no < dSv_info_c::DAN_SWITCH; no++) {
        if (held(kDanSwitch, tbl, no)) {
            remote.mSwitch[no >> 5] &= ~(1u << (no & 31));
        }
    }
    for (int no = 0; no < kDanItemEnd; no++) {
        if (held(kDanItem, tbl, no)) {
            remote.mItem[no >> 5] &= ~(1u << (no & 31));
        }
    }
}

void dropBossFightHolds() {
    if (!s_held.packets.empty()) {
        TwiliLog.info("[sync] dropping {} boss-fight packet(s) held for another save",
            s_held.packets.size());
    }
    s_held = Held{};
}

}  // namespace detail

using namespace detail;

void releaseBossFightHolds() {
    Held h = std::move(s_held);
    s_held = Held{};
    if (h.packets.empty() && h.maxLife == 0 &&
        std::all_of(std::begin(h.dungeonBits), std::end(h.dungeonBits), [](uint8_t b) { return b == 0; }))
    {
        return;
    }
    TwiliLog.info("[sync] boss fight over: applying {} held packet(s)", h.packets.size());
    for (const nlohmann::json& packet : h.packets) {
        applyHeld(packet);
        state().stats.bossReleased++;
    }
    dSv_info_c* info = dComIfGs_getSaveInfo();
    RemoteApplyGuard guard;
    for (int tbl = 0; tbl < dSv_save_c::STAGE_MAX; tbl++) {
        if (h.dungeonBits[tbl] == 0) {
            continue;
        }
        const bool live = tbl == currentSaveTblNo();
        dSv_memBit_c& bit =
            live ? info->getMemory().getBit() : info->getSavedata().getSave(tbl).getBit();
        const int keyNum = bit.mKeyNum;
        const uint8_t before = bit.mDungeonItem;
        bit.mDungeonItem |= h.dungeonBits[tbl];
        if (live) {
            shiftDungeonItemBaseline(keyNum, before);
        }
    }
    dSv_player_status_a_c& a = info->getPlayer().getPlayerStatusA();
    if (h.maxLife > a.getMaxLife()) {
        a.setMaxLife(h.maxLife);
    }
}

size_t bossFightHeld() {
    return s_held.packets.size();
}

}  // namespace twili::sync
