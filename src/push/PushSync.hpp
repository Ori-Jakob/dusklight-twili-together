#pragma once

#include <nlohmann/json_fwd.hpp>

#include <string>

class fopAc_ac_c;

// Block and chain sync (syncPushPull): a teammate's push, pull or chain drag plays here too.
namespace twili::push {

// PUSH_MOVE, PUSH_REST and CHAIN_PULL carry their own version.
inline constexpr int kVersion = 1;

// Hook entry points (hooks_push.cpp). The actor's append is readable until its create returns.
void onActorCreated(fopAc_ac_c* actor);
void onActorDeleted(fopAc_ac_c* actor);
// daObjMovebox::Act_c::mode_wait / mode_walk.
void moveboxWaitPre(fopAc_ac_c* box);
void moveboxWaitPost(fopAc_ac_c* box);
void moveboxWalkPre(fopAc_ac_c* box);
void moveboxWalkPost(fopAc_ac_c* box);
// daObjIceBlk_c::mode_proc_wait / mode_proc_walk.
void iceWaitPre(fopAc_ac_c* block);
void iceWaitPost(fopAc_ac_c* block);
void iceWalkPre(fopAc_ac_c* block);
void iceWalkPost(fopAc_ac_c* block);
// daObjSwChain_c::execute.
void chainExecutePre(fopAc_ac_c* chain);
void chainExecutePost(fopAc_ac_c* chain);

bool handlePacket(const std::string& type, const nlohmann::json& packet);
// Session::update, every tick: replays, snaps and the chain stream.
void tick();
void resetSession();
void shutdown();

}  // namespace twili::push
