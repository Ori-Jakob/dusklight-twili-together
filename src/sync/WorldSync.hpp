#pragma once

// Shared progress between teammates: flags, event bits, items, keys and the full save merge.

#include <nlohmann/json_fwd.hpp>

#include <cstddef>
#include <cstdint>
#include <string>

namespace twili {

struct Client;

namespace sync {

// Connected, the room shares world state and the member gate lets us sync.
bool enabled();

// Save layout we announce and require from teammates (a test may force another one).
const std::string& localLayout();
void setLayoutOverrideForTest(const std::string& layout);

// Adds the team, protocol and layout fields every team-scoped packet carries.
void stampPacket(nlohmann::json& packet, bool addToQueue);

// Swap save data with the team as soon as a stage's save table is live.
void requestExchange();
void onStageSaveTableLoaded();
void onStageSaveTableUnloaded();

// True when the packet type belongs to world sync (handled or dropped here).
bool handlePacket(const std::string& type, const nlohmann::json& packet);
// Session::update while connected.
void tick();
// Session::update, last thing: our queued item grants may dispatch after mod_update.
void endOfUpdate();
void resetSession();
void shutdown();

// Hook entry points.
void onSetFlag(const char* category, int flagNo, int roomNo);
void onUnsetFlag(int flagNo, int roomNo);
void onEventBit(uint16_t no, bool set);
void onSaveWritten();
// execItemGet pre/post and the frame's actor pass (closes the grant dispatch window).
void beforeItemGet(uint8_t itemNo);
void afterItemGet(uint8_t itemNo);
void closeGrantWindow();
// daObjDrop_c::dropGet pre/post: a tear pickup, sent as LIGHT_DROP.
void beforeDropGet(void* drop);
void afterDropGet(void* drop);
bool insideDropGet();
#if TWILI_ENABLE_AUTOTEST
void keepTakenTearsForTest(bool keep);
// Teammates' takes wait, as if it were never safe, until released.
void holdTakesForTest(bool hold);
#endif
bool installItemObserver();
void removeItemObserver();
// daTbox_c / daTbox2_c actionOpenWait pre (true: a teammate opened it, skip the original) and
// actionWait post (plays the lid of a chest we opened).
bool remoteOpenTbox(void* tbox);
void animateOpenedTbox(void* tbox);
bool remoteOpenTbox2(void* tbox2);
void animateOpenedTbox2(void* tbox2);
// daAlink_c::makeFairy pre/post: its heal is a bottle's, not a pickup.
void enterBottleFairy();
void leaveBottleFairy();
// daItem_c::itemGetNextExecute / daObjLife_c::initActionOrderGetDemo pre: true if a teammate
// already took this item (it is removed, nothing given; skip the original).
bool blockTakenPickup(void* item);
// daObjLife_Create post, on a finished create: true if a teammate took this heart piece or
// container's check (the create fails, nothing is placed).
bool blockTakenSpawn(void* life);

// GIVE_ITEM's filter: progression items, no consumables or dungeon items.
bool isSyncedItemNo(int itemNo);
// The shields that burn (Ordon and Wooden); personal unless the room shares them.
bool isWoodenShield(int itemNo);

// Tears of light while an area's tears are collected (area -1: none): ours, or what that teammate
// last reported (TEAR_COUNT).
struct TearCount {
    int area = -1;
    int count = 0;
};
TearCount tearCount(uint32_t clientId);

// Story sync: our post-load world state went out (or none is due).
bool ownStateSettled();
// World-state merges applied from this client.
uint32_t mergeCount(uint32_t clientId);

// Our boss fight is over: what a teammate's flags from its arena held back applies now.
void releaseBossFightHolds();
// Packets waiting for our boss fight to end.
size_t bossFightHeld();

// Autotest readback.
struct Stats {
    uint32_t merges = 0;
    uint32_t layoutRefusals = 0;
    uint32_t layoutWarnings = 0;
    uint32_t grantsQueued = 0;
    uint32_t grantsApplied = 0;
    // SHARE_PICKUP: pickups sent, and gains added or skipped here (no container, full, down).
    uint32_t pickupsSent = 0;
    uint32_t pickupsApplied = 0;
    uint32_t pickupsSkipped = 0;
    int32_t rupeesAdded = 0;
    int32_t lifeAdded = 0;
    int32_t arrowsAdded = 0;
    int32_t seedsAdded = 0;
    int32_t bombsAdded = 0;
    uint32_t trapsSent = 0;
    uint32_t trapsReceived = 0;
    uint32_t trapsApplied = 0;
    // Freestanding items a teammate took: removed here, pickups stopped, GIVE_ITEMs dropped, heart
    // pieces and containers never placed.
    uint32_t freestandingRemoved = 0;
    uint32_t freestandingBlocked = 0;
    uint32_t grantsDeduped = 0;
    uint32_t spawnsBlocked = 0;
    // Poes and golden wolves a teammate's flag or check took, removed here.
    uint32_t actorsRemoved = 0;
    // TAKE_ITEM: our takes sent, teammates' received, removed here or already gone, takes a merge
    // found, takes that had to wait, grants of a taken item, and items back without an item get.
    uint32_t takesSent = 0;
    uint32_t takesReceived = 0;
    uint32_t takesApplied = 0;
    uint32_t takesSkipped = 0;
    uint32_t mergeTakes = 0;
    uint32_t takesDeferred = 0;
    uint32_t takeRegrants = 0;
    uint32_t returnsSent = 0;
    // Teammates' packets held while we fought a boss in their arena, and applied after.
    uint32_t bossHeld = 0;
    uint32_t bossReleased = 0;
};
const Stats& stats();
// True once the incompatible-layout toast was shown for this teammate name.
bool layoutWarned(const std::string& name);

}  // namespace sync
}  // namespace twili
