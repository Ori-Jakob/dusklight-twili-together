#pragma once

// Boss and mini-boss fights: each player fights their own, and nothing from a teammate interrupts one.

#include <nlohmann/json_fwd.hpp>

#include <cstdint>
#include <string>

namespace twili::boss {

// Where our fight runs: the boss's stage (room -1: all of it) and its stage save table.
struct Arena {
    char stage[8] = {};
    int8_t room = -1;
    int saveTbl = -1;
    int16_t profile = -1;
    // Linked stages of one boss sequence (Zant, Ganon): 0 for none.
    int group = 0;
};

// From the first tick we stand in the room of a live boss or mini-boss of ours until we stand,
// settled, outside its arena. A game over that reloads the arena keeps it.
bool fighting();
// A fight runs and its guard is on: teammates' arena flags wait, prompts wait, no pull-in.
bool guarded();
const Arena& arena();
// The stage (and room) of a packet's stageName and roomNo is our arena's.
bool fromArena(const nlohmann::json& packet);
bool inArena(const char* stage, int room);

// A boss or mini-boss actor.
bool isBossProfile(int16_t profile);
// A boss cutscene: ordered by a boss, a mini-boss or the boss warp, or a class-b map event of
// docs/cutscenes.md. Never pulled in, so a finished fight never interrupts one under way.
bool isBossEvent(int16_t requester, const char* stage, const std::string& name);

// Every Session::update, connected or not, before story::tick.
void tick();
void shutdown();

#if TWILI_ENABLE_AUTOTEST
// Off: fights are still seen, nothing is held or deferred.
void setGuardForTest(bool on);
#endif

}  // namespace twili::boss
