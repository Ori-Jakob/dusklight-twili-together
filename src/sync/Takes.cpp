// TAKE_ITEM: an item the game takes from us (the monkey's lantern, a trade item handed over, the
// Wooden Sword Talo grabs) is taken from every teammate too. Polled per tick; never in randomizer
// games, shields never tracked.

#include "sync/WorldSyncState.hpp"

#include "core/LocalPlayer.hpp"
#include "core/Log.hpp"
#include "core/SaveGate.hpp"
#include "core/Session.hpp"
#include "sync/RemoteApplyGuard.hpp"
#include "ui/ItemToasts.hpp"

#include "d/actor/d_a_alink.h"
#include "d/d_meter2_info.h"

#include <algorithm>
#include <cstring>

namespace twili::sync {
namespace detail {
namespace {

struct PendingTake {
    int slot = kTakeFirstBit;
    uint8_t itemNo = 0;
    item_toasts::Sender from;
    bool merge = false;
    bool waited = false;
};

struct Takes {
    bool baselineValid = false;
    uint32_t baselineGeneration = 0;
    TakeSnapshot baseline;
    bool itemGetValid = false;
    TakeSnapshot itemGetBefore;
    // Kept across reconnects, dropped with the save.
    std::set<uint8_t> records;
    std::deque<PendingTake> pending;
    bool holdForTest = false;
};

Takes s_takes;

dSv_player_c& player() {
    return dComIfGs_getSaveInfo()->getPlayer();
}

// Raw reads: other mods hook the accessors.
uint8_t rawSlot(int slot) {
    return player().getItem().mItems[slot];
}

bool rawFirstBit(uint8_t itemNo) {
    return ((static_cast<u32>(player().getGetItem().mItemFlags[itemNo >> 5]) >> (itemNo & 31)) &
               1u) != 0;
}

uint8_t rawSword() {
    return player().getPlayerStatusA().getSelectEquip(COLLECT_SWORD);
}

int takeSlotOf(uint8_t itemNo) {
    for (const int slot : kTakeSlots) {
        if (isTakeableSlotItem(slot, itemNo)) {
            return slot;
        }
    }
    return -1;
}

bool baselineUsable() {
    return s_takes.baselineValid && isSaveLoaded() &&
           s_takes.baselineGeneration == saveGeneration();
}

void sendTake(int slot, uint8_t itemNo) {
    nlohmann::json packet = {
        {"type", "TAKE_ITEM"},
        {"itemNo", static_cast<unsigned>(itemNo)},
    };
    if (slot == kTakeSword) {
        packet["equip"] = "sword";
    } else if (slot >= 0) {
        packet["slot"] = slot;
    } else {
        packet["firstBit"] = true;
    }
    stampWorldPacket(packet, true);
    send(std::move(packet));
}

void onLocalTake(int slot, uint8_t itemNo) {
    if (randoGame()) {
        return;
    }
    s_takes.records.insert(itemNo);
    if (!enabled()) {
        TwiliLog.info("[sync] the game took item 0x{:02X}; world sync is off, not shared", itemNo);
        return;
    }
    sendTake(slot, itemNo);
    state().stats.takesSent++;
    TwiliLog.info("[sync] the game took item 0x{:02X} (slot {}): sent TAKE_ITEM", itemNo, slot);
}

// In again without an item get (the game-over lantern, an event's trade swap): no GIVE_ITEM went.
void onLocalReturn(uint8_t itemNo) {
    s_takes.records.erase(itemNo);
    if (randoGame()) {
        return;
    }
    cancelPendingTakes(itemNo);
    if (!enabled()) {
        return;
    }
    sendGiveItem(itemNo, nullptr);
    state().stats.returnsSent++;
    TwiliLog.info("[sync] item 0x{:02X} came back without an item get: sent GIVE_ITEM", itemNo);
}

void noteChanges(const TakeSnapshot& was, const TakeSnapshot& now) {
    for (size_t i = 0; i < std::size(kTakeSlots); i++) {
        if (was.slots[i] == now.slots[i]) {
            continue;
        }
        if (now.slots[i] == dItemNo_NONE_e) {
            if (isTakeableSlotItem(kTakeSlots[i], was.slots[i])) {
                onLocalTake(kTakeSlots[i], was.slots[i]);
            }
        } else if (isSyncedItemNo(now.slots[i])) {
            onLocalReturn(now.slots[i]);
        }
    }
    for (size_t i = 0; i < std::size(kTakeFirstBits); i++) {
        if (was.bits[i] && !now.bits[i]) {
            onLocalTake(kTakeFirstBit, kTakeFirstBits[i]);
        } else if (!was.bits[i] && now.bits[i]) {
            onLocalReturn(kTakeFirstBits[i]);
        }
    }
    // Talo's grab; the collection menu only switches between swords, never to none. Equipping one
    // gains nothing (it was owned), so that sends no return.
    if (was.sword == dItemNo_WOOD_STICK_e && now.sword == dItemNo_NONE_e) {
        onLocalTake(kTakeSword, dItemNo_WOOD_STICK_e);
    }
}

void pollTakes() {
    if (!isSaveLoaded()) {
        s_takes.baselineValid = false;
        return;
    }
    const TakeSnapshot now = takeSnapshot();
    if (baselineUsable()) {
        noteChanges(s_takes.baseline, now);
    }
    s_takes.baseline = now;
    s_takes.baselineValid = true;
    s_takes.baselineGeneration = saveGeneration();
}

// Why a teammate's take of this item waits, or nullptr.
const char* takeBlocker(const PendingTake& take) {
    if (s_takes.holdForTest) {
        return "held";
    }
    const char* block = local::localTeleportBlocker();
    if (block != nullptr && std::strcmp(block, "riding") != 0 && std::strcmp(block, "busy") != 0 &&
        std::strcmp(block, "carrying") != 0)
    {
        return block;
    }
    // A replay's give of it lands before its take.
    const auto& grants = state().pendingGrants;
    if (std::find(grants.begin(), grants.end(), take.itemNo) != grants.end()) {
        return "granting";
    }
    daAlink_c* link = local::liveLink();
    if (link == nullptr) {
        return "loading";
    }
    if (link->mEquipItem == take.itemNo) {
        return "in-hand";
    }
    // 0x103: the sword in hand (drawn or swinging).
    if (take.slot == kTakeSword && link->mEquipItem == 0x103) {
        return "drawn";
    }
    if (take.itemNo == dItemNo_KANTERA_e) {
        switch (link->mProcID) {
        case daAlink_c::PROC_KANDELAAR_SWING:
        case daAlink_c::PROC_KANDELAAR_POUR:
        case daAlink_c::PROC_HORSE_KANDELAAR_POUR:
        case daAlink_c::PROC_CANOE_KANDELAAR_POUR:
            return "in-use";
        default:
            break;
        }
    }
    return nullptr;
}

// The game's own calls for this take; false if we do not hold the item.
bool removeHeld(const PendingTake& take) {
    if (take.slot == kTakeSword) {
        if (rawSword() != take.itemNo) {
            return false;
        }
        // As Talo's cut does: unequipped, still collected (the menu hides it by F_0026).
        dMeter2Info_setSword(dItemNo_NONE_e, false);
        return true;
    }
    if (take.slot == kTakeFirstBit) {
        if (!rawFirstBit(take.itemNo)) {
            return false;
        }
        dComIfGs_offItemFirstBit(take.itemNo);
        return true;
    }
    if (rawSlot(take.slot) != take.itemNo) {
        return false;
    }
    if (take.itemNo == dItemNo_KANTERA_e) {
        // Lit on the belt: it goes out as in water. The oil is kept for a game over, as the monkey does.
        daAlink_c* link = local::liveLink();
        if (link != nullptr && link->checkUseKandelaar(0)) {
            link->offKandelaarModel();
        }
        dMeter2Info_setOilGaugeBackUp(dComIfGs_getOil());
    }
    if (take.slot == SLOT_21) {
        dComIfGs_setWarashibeItem(dItemNo_NONE_e);
    } else {
        dComIfGs_setItem(take.slot, dItemNo_NONE_e);
    }
    return true;
}

void applyTake(const PendingTake& take) {
    State& st = state();
    const TakeSnapshot before = takeSnapshot();
    bool removed = false;
    {
        RemoteApplyGuard guard;
        removed = removeHeld(take);
    }
    shiftTakeBaseline(before);
    // Gone either way (we had it): a teammate's later give brings it back.
    if (removed || (take.slot != kTakeFirstBit && rawFirstBit(take.itemNo))) {
        s_takes.records.insert(take.itemNo);
    }
    const char* how = take.merge ? "world state" : "TAKE_ITEM";
    if (!removed) {
        st.stats.takesSkipped++;
        TwiliLog.info("[sync] {} of client {} takes item 0x{:02X}: we do not hold it", how,
            take.from.clientId, take.itemNo);
        return;
    }
    st.stats.takesApplied++;
    TwiliLog.info("[sync] item 0x{:02X} taken from us ({} of client {})", take.itemNo, how,
        take.from.clientId);
    item_toasts::noteTaken(take.from, take.itemNo);
}

void applyPendingTakes() {
    State& st = state();
    for (auto it = s_takes.pending.begin(); it != s_takes.pending.end();) {
        if (const char* block = takeBlocker(*it)) {
            if (!it->waited) {
                it->waited = true;
                st.stats.takesDeferred++;
                TwiliLog.info("[sync] take of item 0x{:02X} waits ({})", it->itemNo, block);
            }
            ++it;
            continue;
        }
        const PendingTake take = *it;
        it = s_takes.pending.erase(it);
        applyTake(take);
    }
}

bool queueTake(PendingTake take) {
    const bool queued = std::any_of(s_takes.pending.begin(), s_takes.pending.end(),
        [&](const PendingTake& p) { return p.slot == take.slot && p.itemNo == take.itemNo; });
    if (queued) {
        return false;
    }
    s_takes.pending.push_back(std::move(take));
    return true;
}

}  // namespace

bool isTakeableSlotItem(int slot, uint8_t itemNo) {
    switch (slot) {
    case SLOT_1:
        return itemNo == dItemNo_KANTERA_e;
    case SLOT_19:
        return itemNo == dItemNo_RAFRELS_MEMO_e || itemNo == dItemNo_ASHS_SCRIBBLING_e;
    case SLOT_21:
        // The horse call ends the line and is never taken.
        return itemNo == dItemNo_LETTER_e || itemNo == dItemNo_BILL_e ||
               itemNo == dItemNo_WOOD_STATUE_e || itemNo == dItemNo_IRIAS_PENDANT_e;
    case SLOT_22:
        return itemNo == dItemNo_ANCIENT_DOCUMENT_e || itemNo == dItemNo_AIR_LETTER_e ||
               itemNo == dItemNo_ANCIENT_DOCUMENT2_e;
    default:
        return false;
    }
}

bool isTakeableFirstBit(uint8_t itemNo) {
    return std::find(std::begin(kTakeFirstBits), std::end(kTakeFirstBits), itemNo) !=
           std::end(kTakeFirstBits);
}

bool hasTakeRecord(uint8_t itemNo) {
    return s_takes.records.count(itemNo) != 0;
}

bool mayGrantAgain(uint8_t itemNo) {
    if (!hasTakeRecord(itemNo) || !isSaveLoaded()) {
        return false;
    }
    if (isTakeableFirstBit(itemNo)) {
        return !rawFirstBit(itemNo);
    }
    if (itemNo == dItemNo_WOOD_STICK_e) {
        return rawSword() == dItemNo_NONE_e;
    }
    // Into its empty slot only: a trade slot that moved on keeps its newer item.
    const int slot = takeSlotOf(itemNo);
    return slot >= 0 && rawSlot(slot) == dItemNo_NONE_e;
}

TakeSnapshot takeSnapshot() {
    TakeSnapshot snap;
    if (!isSaveLoaded()) {
        return snap;
    }
    for (size_t i = 0; i < std::size(kTakeSlots); i++) {
        snap.slots[i] = rawSlot(kTakeSlots[i]);
    }
    for (size_t i = 0; i < std::size(kTakeFirstBits); i++) {
        snap.bits[i] = rawFirstBit(kTakeFirstBits[i]);
    }
    snap.sword = rawSword();
    return snap;
}

void shiftTakeBaseline(const TakeSnapshot& before) {
    if (!baselineUsable()) {
        return;
    }
    const TakeSnapshot now = takeSnapshot();
    for (size_t i = 0; i < std::size(kTakeSlots); i++) {
        if (now.slots[i] != before.slots[i]) {
            s_takes.baseline.slots[i] = now.slots[i];
        }
    }
    for (size_t i = 0; i < std::size(kTakeFirstBits); i++) {
        if (now.bits[i] != before.bits[i]) {
            s_takes.baseline.bits[i] = now.bits[i];
        }
    }
    if (now.sword != before.sword) {
        s_takes.baseline.sword = now.sword;
    }
}

void queueMergeTakes(const std::vector<SlotTake>& takes, const nlohmann::json& packet) {
    if (takes.empty()) {
        return;
    }
    const item_toasts::Sender from = item_toasts::senderOf(Session::instance(), packet);
    for (const SlotTake& t : takes) {
        if (queueTake({t.slot, t.itemNo, from, true})) {
            state().stats.mergeTakes++;
            TwiliLog.info("[sync] client {}'s world state shows item 0x{:02X} taken", from.clientId,
                t.itemNo);
        }
    }
}

void takesBeforeItemGet() {
    s_takes.itemGetValid = false;
    if (!baselineUsable()) {
        return;
    }
    // A take earlier this frame (an event that removes one item, then hands out the next).
    pollTakes();
    s_takes.itemGetBefore = s_takes.baseline;
    s_takes.itemGetValid = true;
}

void takesAfterItemGet(uint8_t itemNo, bool remoteGrant) {
    s_takes.records.erase(itemNo);
    // Our own get is newer than a teammate's take still waiting; a replayed grant is older.
    if (!remoteGrant) {
        cancelPendingTakes(itemNo);
    }
    if (s_takes.itemGetValid) {
        s_takes.itemGetValid = false;
        shiftTakeBaseline(s_takes.itemGetBefore);
    }
}

void cancelPendingTakes(uint8_t itemNo) {
    const size_t dropped = std::erase_if(
        s_takes.pending, [&](const PendingTake& p) { return p.itemNo == itemNo; });
    if (dropped > 0) {
        TwiliLog.info("[sync] item 0x{:02X} came back before its take applied: take dropped", itemNo);
    }
}

void handleTakeItem(const nlohmann::json& packet) {
    State& st = state();
    if (!acceptsWorldPacket(packet, "TAKE_ITEM")) {
        return;
    }
    st.stats.takesReceived++;
    const uint32_t from = packet.value("clientId", 0u);
    if (randoGame()) {
        TwiliLog.info("[sync] ignored TAKE_ITEM from client {}: randomizer game", from);
        return;
    }
    const int itemNo = packet.value("itemNo", -1);
    const bool firstBit = packet.value("firstBit", false);
    const bool sword = packet.value("equip", std::string{}) == "sword";
    const int slot = sword ? kTakeSword : firstBit ? kTakeFirstBit : packet.value("slot", -1);
    const auto item = static_cast<uint8_t>(itemNo);
    const bool valid = itemNo >= 0 && itemNo <= 0xFF &&
                       (sword      ? item == dItemNo_WOOD_STICK_e :
                           firstBit ? isTakeableFirstBit(item) :
                                      isTakeableSlotItem(slot, item));
    if (!valid) {
        TwiliLog.warn("[sync] ignored TAKE_ITEM from client {}: item {} slot {}", from, itemNo, slot);
        return;
    }
    queueTake({slot, static_cast<uint8_t>(itemNo), item_toasts::senderOf(Session::instance(), packet)});
    TwiliLog.info("[sync] client {} took item 0x{:02X}", from, itemNo);
}

void tickTakes() {
    pollTakes();
    applyPendingTakes();
}

void invalidateTakeBaseline() {
    s_takes.baselineValid = false;
    s_takes.itemGetValid = false;
}

void resetTakes() {
    const bool hold = s_takes.holdForTest;
    s_takes = Takes{};
    s_takes.holdForTest = hold;
}

}  // namespace detail

#if TWILI_ENABLE_AUTOTEST
void holdTakesForTest(bool hold) {
    detail::s_takes.holdForTest = hold;
    TwiliLog.info("[sync] teammates' takes {}", hold ? "held" : "released");
}
#endif

}  // namespace twili::sync
