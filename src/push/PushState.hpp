#pragma once

// Internal to push/*.cpp and the autotest.

#include "push/PushSync.hpp"

#include "SSystem/SComponent/c_xyz.h"
#include "f_op/f_op_actor.h"
#include "f_pc/f_pc_base.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <deque>
#include <map>
#include <string>

namespace twili::push::detail {

using Clock = std::chrono::steady_clock;

enum class Kind : uint8_t { None, Movebox, IceBlock, SwChain, SwTurn, Lv6SwTurn, PushDoor };

const char* kindName(Kind kind);
Kind kindFromName(const std::string& name);

// What identifies a placed object on every client: its spawn data, read at create.
struct Key {
    int16_t procName = -1;
    int8_t roomNo = -1;
    uint32_t params = 0;
    uint16_t setId = 0;
    float home[3] = {};
    int16_t angle[3] = {};
};

bool sameKey(const Key& a, const Key& b);
std::string keyText(const Key& key);

struct XZ {
    float x = 0.0f;
    float z = 0.0f;
};

bool nearXZ(const XZ& a, const XZ& b, float tolerance);
nlohmann::json xzJson(const XZ& p);

// A teammate's block move: `to` is known up front for grid blocks only.
struct Move {
    uint32_t client = 0;
    XZ from;
    XZ to;
    bool hasTo = false;
    uint8_t dir = 0;
    bool pull = false;
    // daObjIceBlk_c::mWalkType: 1 a push, 2 an iron ball hit.
    uint8_t walk = 1;
    Clock::time_point at{};
};

// A teammate's chain, as last streamed.
struct ChainStream {
    uint32_t client = 0;
    bool held = false;
    // Held by the clawshot rather than the hand.
    bool hook = false;
    uint8_t out = 0;
    uint8_t cap = 0;
    cXyz end{};
    // The chain end in the puller's frame (from its feet, turned by its facing).
    bool hasRel = false;
    cXyz rel{};
    Clock::time_point at{};
};

struct Record {
    fopAc_ac_c* actor = nullptr;  // valid until onActorDeleted
    Kind kind = Kind::None;
    Key key;
    // Another object here has the same key: never synced.
    bool dup = false;

    // Blocks.
    bool localWalk = false;
    bool remoteWalk = false;
    // Our copy settles a teammate's move: its switch saves stay quiet.
    bool settleGuard = false;
    XZ localFrom;
    // The move this block follows; a concurrent one (same start) from a lower client id wins.
    bool authValid = false;
    uint32_t authClient = 0;
    XZ authFrom;
    Clock::time_point authAt{};
    std::deque<Move> queue;
    bool hasRest = false;
    XZ rest;
    Clock::time_point restAt{};
    // daObjIceBlk_c: its push event is held back while a teammate's push replays.
    bool actionMuted = false;

    // Chains.
    bool hasStream = false;
    ChainStream stream;
    bool driving = false;
    // Moved by a teammate since our own Link last held it: its switch writes are undone.
    bool touched = false;
    bool hookBitOurs = false;
    bool sentHeld = false;
    uint8_t sentOut = 0;
    int sendTicks = 0;

    // Readback (autotest and logs).
    uint32_t sent = 0;
    uint32_t applied = 0;
    uint32_t snapped = 0;
    uint32_t rejected = 0;
    uint32_t blockedSwitchWrites = 0;
    uint32_t drivenTicks = 0;
    bool hasRemoteTarget = false;
    XZ remoteTarget;
};

std::map<fpc_ProcID, Record>& records();
Record* recordOf(const fopAc_ac_c* actor);

// Connected, the room shares world state and blocks, and we may sync.
bool optionOn();
uint32_t selfId();
// Accepts a move or rest from `client` that started at `from` unless a concurrent one (same
// start) from a lower client id is already followed.
bool takeAuthority(Record& r, uint32_t client, const XZ& from);
void setOwnAuthority(Record& r, const XZ& from);

// Sends to teammates in our stage and layer; false (nothing sent) when none is there.
bool sendPacket(nlohmann::json packet, const Record& r);
nlohmann::json keyJson(const Key& key);

// PushBlocks.cpp
XZ blockPos(const Record& r);
// Our Link's move: sends it and follows it (`to` for grid blocks and turns only).
void sendMove(Record& r, const XZ& from, const XZ* to, int dir, bool pull, int walkType);
// The walk (or turn) stopped: a local one sends its rest.
void endWalk(Record& r);
bool blockAtRest(const Record& r);
void tickBlock(Record& r);
void queueMove(Record& r, const Move& m);
void queueRest(Record& r, uint32_t client, const XZ& from, const XZ& pos);
// Autotest: what a push from our Link does, before the block's next wait.
void pushForTest(Record& r, int dir, bool pull);

// PushTurns.cpp: a turn object's spot is its step, x = step * kTurnScale.
inline constexpr float kTurnScale = 100.0f;
bool isTurn(Kind kind);
int turnStep(const Record& r);
int turnDir(const Record& r);
bool turnRotating(const Record& r);
XZ turnSpot(int step);
int turnStepOf(const XZ& spot);
void startTurn(Record& r, int dir);
void setTurnStep(Record& r, int step);
void pushTurnForTest(Record& r, int dir);

// PushChains.cpp
void tickChain(Record& r);
void applyStream(Record& r, const ChainStream& s);
// Chain state for the autotest.
int chainOut(const Record& r);
bool chainHeldLocally(const Record& r);
cXyz chainEnd(const Record& r);

}  // namespace twili::push::detail
