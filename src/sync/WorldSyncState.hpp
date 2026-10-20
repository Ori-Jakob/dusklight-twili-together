#pragma once

// Internal to sync/*.cpp.

#include "sync/WorldSync.hpp"

#include "d/d_com_inf_game.h"
#include "d/d_item_data.h"
#include "d/d_save.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <deque>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace twili::sync::detail {

using Clock = std::chrono::steady_clock;

// Memory switches and event bits cleared this session: merges never OR them back.
struct ClearedFlags {
    uint32_t switches[dSv_save_c::STAGE_MAX][4] = {};
    uint8_t eventBits[MAX_EVENTS] = {};
};

struct State {
    // Per session.
    bool exchangePending = false;
    bool catchUpRequested = false;
    Clock::time_point ownStatePublishAt{};
    ClearedFlags cleared;
    // Save tables whose key count the next merge may take from the snapshot.
    uint32_t keyCountAdoptable = 0;
    bool dungeonBaselineValid = false;
    int dungeonBaselineSaveTbl = -1;
    int dungeonBaselineKeyNum = 0;
    uint8_t dungeonBaselineItems = 0;
    std::set<uint32_t> versionMismatchLogged;
    std::set<uint32_t> layoutWarnedIds;
    std::map<uint32_t, uint32_t> merges;

    // Kept across reconnects, dropped with the save: the catch-up replays the whole queue.
    std::string queueEpoch;
    std::set<uint64_t> queueApplied;
    uint32_t saveGeneration = 0;

    // Our grants waiting in ItemService's queue, oldest first.
    std::deque<uint8_t> pendingGrants;
    bool grantWindow = false;

    std::set<std::string> layoutWarnedNames;
    std::string layoutOverride;
    Stats stats;
};

State& state();

int currentSaveTblNo();
// Sender known, same team and layout, member gate open, not a replay we already applied.
bool acceptsWorldPacket(const nlohmann::json& packet, const char* what);
void stampWorldPacket(nlohmann::json& packet, bool addToQueue);
void send(nlohmann::json packet);
// Our save changed since the last check: forget what applied to the old one.
void checkSaveGeneration();

// Flags.cpp
// The live copy while that stage is loaded here, else its save slot; the dungeon bits of ours.
dSv_memBit_c* memBitForSaveTbl(int saveTblNo);
dSv_danBit_c* danBitForSaveTbl(int saveTblNo);
void handleSetFlag(const nlohmann::json& packet);
void handleUnsetFlag(const nlohmann::json& packet);
void handleSetEventBit(const nlohmann::json& packet);
void handleUnsetEventBit(const nlohmann::json& packet);
// The handlers past their accept and boss-fight checks (a held packet replays through these).
void applySetFlag(const nlohmann::json& packet);
void applyUnsetFlag(const nlohmann::json& packet);
void applySetEventBit(const nlohmann::json& packet);
void applyUnsetEventBit(const nlohmann::json& packet);
void noteMemorySwitch(int saveTblNo, int switchNo, bool set);
void noteEventBit(uint16_t no, bool set);

// WorldState.cpp
void sendRequestWorldState();
void sendUpdateWorldState(uint32_t targetClientId = 0);
void handleRequestWorldState(const nlohmann::json& packet);
void handleUpdateWorldState(const nlohmann::json& packet);

// LightDrops.cpp
void handleLightDrop(const nlohmann::json& packet);
void handleTearCount(const nlohmann::json& packet);
void tickTearCount();
void resetTearCounts();

// Chests.cpp: a teammate's TBOX bit landed in that stage table (live: just now, from SET_FLAG).
void onRemoteTbox(int saveTblNo, int tboxNo, bool live);
void clearOpeningChests();
// Our chest with that number is opening for us (its event commanded or its item demo running).
bool chestOpeningForUs(int tboxNo);

// Checks that stand for one thing in the world: GIVE_ITEM's `check`, granted once per team.
// Reward: an NPC's or minigame's heart piece, taken once its event bit is set.
enum class CheckKind : uint8_t { None, Freestanding, Boss, Poe, GoldenWolf, Fishing, Chest, Reward };
struct PlacedCheck {
    CheckKind kind = CheckKind::None;
    std::string stage;
    // Item bit, switch, event bit or chest number.
    int no = -1;
};
bool parsePlacedCheck(const char* check, PlacedCheck& out);
// Any other named give (no shop, no unparsed placed name): each player gets it once.
bool isNamedCheck(const char* check);

// Freestanding.cpp: item bits in dSv_info_c numbering (room only for zone bits).
bool itemBitSet(int saveTblNo, int bit, int roomNo);
// A teammate set this item bit here (it was not set): its item vanishes if it lies here.
void onRemoteItemBit(int saveTblNo, int bit, int roomNo);
// A teammate set this switch, TBOX bit or event bit (it was not set); `live`: just now, here.
void onRemoteSwitch(int saveTblNo, int no, int roomNo, bool live);
void onRemoteTboxBit(int saveTblNo, int no);
void onRemoteEventBit(uint16_t no, bool live);
// A teammate's GIVE_ITEM for `check` (placed or named): false if that check already reached us.
bool acceptTeamCheck(const std::string& check, int saveTblNo);
void noteOwnCheck(const char* check);
// A teammate's grant for this check already reached us.
bool teamTookCheck(const char* check);
void tickFreestanding();
void clearFreestandingStage();
void resetFreestanding();

// TakenActors.cpp: Poes and golden wolves a teammate's flag or check took.
// A Poe with this soul switch dies for us right now (its soul is ours).
bool poeDyingForUs(int bitSw);
void removeTakenPoes(int bitSw, int roomNo);
void removeTakenGoldenWolves(uint16_t eventBit);
void tickTakenActors();
void clearTakenActors();

// The randomizer's trap item (dItemNo_Randomizer_FOOLISH_ITEM_e, an unused id in vanilla).
inline constexpr uint8_t kFoolishItem = 0x13;
// Our game is a randomizer game (item ids then mean the randomizer's items).
bool randoGame();

// Pickups.cpp: a consumable or trap we got from the game, sent as SHARE_PICKUP at the tick.
void notePickup(uint8_t itemNo, const char* checkName);
void flushPickups();
void handleSharePickup(const nlohmann::json& packet);
// The give observer's view of our own trap grant.
bool isTrapGrant(const char* checkName);

// GiveItem.cpp
// `check`: the give's check name, sent along for placed checks (parsePlacedCheck).
void sendGiveItem(uint8_t itemNo, const char* check);
void handleGiveItem(const nlohmann::json& packet);

// DungeonItems.cpp
void tickDungeonItemTracking();
void shiftDungeonItemBaseline(int keyNumBefore, uint8_t dungeonItemsBefore);
void handleUpdateDungeonItems(const nlohmann::json& packet);
void applyUpdateDungeonItems(const nlohmann::json& packet);

// CollectRewards.cpp: Fused Shadows and Mirror Shards, set by the boss warp's scene, not an item get.
void tickCollectRewards();
// A remote apply changed them: not ours to send.
void shiftCollectBaseline(uint8_t crystalBefore, uint8_t mirrorBefore);
void invalidateCollectBaseline();
void handleCollectReward(const nlohmann::json& packet);

// BossHold.cpp: while our boss fight runs, a teammate's flags from its arena wait (true: held).
bool holdForBossFight(const std::string& type, const nlohmann::json& packet);
// Takes out of a merge what we hold: the arena's boss bits and the switches and event bits held.
void holdMergeForBossFight(dSv_save_c& remote);
void holdDanForBossFight(dSv_danBit_c& remote);
void dropBossFightHolds();

// Takes.cpp: items the game takes away (the monkey's lantern, trade items handed over), taken from
// teammates too as TAKE_ITEM. Not in randomizer games, never shields.
inline constexpr int kTakeSlots[] = {SLOT_1, SLOT_19, SLOT_21, SLOT_22};
// The pumpkin and cheese have no slot: their first-get bit is the item.
inline constexpr uint8_t kTakeFirstBits[] = {dItemNo_TOMATO_PUREE_e, dItemNo_TASTE_e};
// Take "slots" that are not item slots: a first-get bit, and the equipped sword (Talo takes the
// Wooden Sword).
inline constexpr int kTakeFirstBit = -1;
inline constexpr int kTakeSword = -2;
// The items the game takes from this slot (the lantern, Auru's memo, the trade line, the Sky Book).
bool isTakeableSlotItem(int slot, uint8_t itemNo);
bool isTakeableFirstBit(uint8_t itemNo);
// Our own take of this item, or a teammate's we applied.
bool hasTakeRecord(uint8_t itemNo);
// Taken from us and its slot (or bit) still empty: a teammate's GIVE_ITEM grants it again.
bool mayGrantAgain(uint8_t itemNo);
// What our tracked slots and bits held; remote applies shift the take baseline by their changes.
struct TakeSnapshot {
    uint8_t slots[std::size(kTakeSlots)] = {};
    bool bits[std::size(kTakeFirstBits)] = {};
    uint8_t sword = dItemNo_NONE_e;
};
TakeSnapshot takeSnapshot();
void shiftTakeBaseline(const TakeSnapshot& before);
// A merge found this item taken from the sender (its slot empty, its first-get bit set).
struct SlotTake {
    int slot = kTakeFirstBit;
    uint8_t itemNo = 0;
};
void queueMergeTakes(const std::vector<SlotTake>& takes, const nlohmann::json& packet);
// execItemGet's outermost pre/post: takes earlier this frame are ours, the get's changes are not.
void takesBeforeItemGet();
void takesAfterItemGet(uint8_t itemNo, bool remoteGrant);
// A teammate's GIVE_ITEM of this item came after its take: the take waiting here is void.
void cancelPendingTakes(uint8_t itemNo);
void handleTakeItem(const nlohmann::json& packet);
void tickTakes();
void invalidateTakeBaseline();
void resetTakes();

}  // namespace twili::sync::detail
