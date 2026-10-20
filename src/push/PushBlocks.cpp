// Pushed blocks: a teammate's move replays with the block's own walk, then lands where theirs did.

#include "push/PushState.hpp"

#include "core/Log.hpp"
#include "sync/RemoteApplyGuard.hpp"

#include "JSystem/JParticle/JPAEmitter.h"  // d_a_obj_iceblock.h is not self-contained
#include "d/actor/d_a_obj_iceblock.h"
#include "d/actor/d_a_obj_movebox.h"
#include "d/actor/d_a_player.h"
#include "d/d_com_inf_game.h"
#include "f_op/f_op_actor_mng.h"
#include "Z2AudioLib/Z2SeMgr.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace twili::push {
namespace detail {
namespace {

using Box = daObjMovebox::Act_c;
using Ice = daObjIceBlk_c;

// A teammate's move or rest waits this long for our copy to come to rest.
constexpr auto kQueueTtl = std::chrono::seconds(10);
constexpr size_t kMaxQueue = 4;
// On the spot a move starts from or ends on.
constexpr float kBoxSpot = 5.0f;
constexpr float kIceSpot = 10.0f;
// Half an ice block (d_a_obj_iceblock.cpp checks at 145 from its centre) and its height.
constexpr float kIceHalf = 150.0f;
constexpr float kIceHeight = 300.0f;
// Link's own wall radius, and some.
constexpr float kPlayerRadius = 40.0f;
// l_dir_angle of d_a_obj_iceblock.cpp.
constexpr s16 kIceDirAngle[4] = {0x0000, 0x4000, -0x8000, -0x4000};
constexpr int kDirX[4] = {0, 1, 0, -1};
constexpr int kDirZ[4] = {1, 0, -1, 0};

Box* boxOf(const Record& r) {
    return static_cast<Box*>(r.actor);
}

Ice* iceOf(const Record& r) {
    return static_cast<Ice*>(r.actor);
}

const daObjMovebox::Attr_c& boxAttr(const Box* b) {
    return Box::M_attr[b->mType];
}

// Act_c::mode_wait's spot for grid cell (gx, gz): home, turned by the home angle.
XZ gridXZ(const Box* b, int gx, int gz) {
    const float unit = boxAttr(b).mUnitDistance;
    const float lx = gx * unit;
    const float lz = gz * unit;
    const float c = cM_scos(b->home.angle.y);
    const float s = cM_ssin(b->home.angle.y);
    return {b->home.pos.x + lx * c + lz * s, b->home.pos.z - lx * s + lz * c};
}

void gridOf(const Box* b, const XZ& p, int& gx, int& gz) {
    const float unit = boxAttr(b).mUnitDistance;
    const float dx = p.x - b->home.pos.x;
    const float dz = p.z - b->home.pos.z;
    const float c = cM_scos(b->home.angle.y);
    const float s = cM_ssin(b->home.angle.y);
    gx = static_cast<int>(std::lround((dx * c - dz * s) / unit));
    gz = static_cast<int>(std::lround((dx * s + dz * c) / unit));
}

bool iceLocked(Ice* ice) {
    return ice->getSwbit2() != 0xFF && !fopAcM_isSwitch(ice, ice->getSwbit2());
}

bool walking(const Record& r) {
    if (r.kind == Kind::Movebox) {
        return boxOf(r)->field_0x5ac == 1;
    }
    if (isTurn(r.kind)) {
        return turnRotating(r);
    }
    return iceOf(r)->mMode == Ice::MODE_PROC_WALK_e;
}

float spotTolerance(const Record& r) {
    if (isTurn(r.kind)) {
        return kTurnScale * 0.25f;
    }
    return r.kind == Kind::Movebox ? kBoxSpot : kIceSpot;
}

// A turn object's spots travel as steps.
void putSpot(nlohmann::json& packet, const Record& r, const char* key, const XZ& spot) {
    if (isTurn(r.kind)) {
        packet[std::strcmp(key, "from") == 0 ? "step" : "toStep"] = turnStepOf(spot);
    } else {
        packet[key] = xzJson(spot);
    }
}

// Moves our Link out of a block that just appeared around him.
void pushOutPlayer(const XZ& centre, float half, float bottom, float top, s16 yaw) {
    daPy_py_c* player = daPy_getPlayerActorClass();
    if (player == nullptr) {
        return;
    }
    const cXyz p = player->current.pos;
    if (p.y < bottom - 50.0f || p.y > top) {
        return;
    }
    const float c = cM_scos(yaw);
    const float s = cM_ssin(yaw);
    const float dx = p.x - centre.x;
    const float dz = p.z - centre.z;
    float lx = dx * c - dz * s;
    float lz = dx * s + dz * c;
    const float limit = half + kPlayerRadius;
    if (std::fabs(lx) >= limit || std::fabs(lz) >= limit) {
        return;
    }
    if (limit - std::fabs(lx) < limit - std::fabs(lz)) {
        lx = lx < 0.0f ? -limit : limit;
    } else {
        lz = lz < 0.0f ? -limit : limit;
    }
    const cXyz to(centre.x + lx * c + lz * s, p.y, centre.z - lx * s + lz * c);
    player->setPlayerPosAndAngle(&to, player->shape_angle.y, TRUE);
    TwiliLog.info("[push] moved Link out of a block that landed on him ({:.0f} {:.0f} -> {:.0f} "
                  "{:.0f})",
        p.x, p.z, to.x, to.z);
}

void startBoxWalk(Record& r, int dir, bool pull) {
    Box* b = boxOf(r);
    b->field_0x8e8 = dir;
    b->mPPLabel = pull ? dBgW::PPLABEL_PULL : dBgW::PPLABEL_PUSH;
    b->mode_walk_init();
    const int time = pull ? boxAttr(b).mPullMoveTime : boxAttr(b).mPushMoveTime;
    b->field_0x8f8 = static_cast<s16>(time);
    b->field_0x8e4 = 32768.0f / time;
    b->clr_moment_cnt();
    r.remoteWalk = true;
}

void startIceWalk(Record& r, int dir, uint8_t walkType) {
    Ice* ice = iceOf(r);
    ice->mMoveDir = dir;
    ice->current.angle.y = kIceDirAngle[dir];
    ice->mWalkType = walkType;
    ice->speedF = 0.0f;
    ice->mode_init_walk();
    ice->clrCounter();
    // A push orders a camera event; it belongs to the pusher's game, not ours.
    if (ice->mAction == Ice::ACTION_WAIT_e) {
        ice->setAction(Ice::ACTION_DEAD_e);
        r.actionMuted = true;
    }
    fopAcM_seStart(ice, Z2SE_OBJ_IRONBLOCK_MOVE, 0);
    r.remoteWalk = true;
}

void startReplay(Record& r, const Move& m) {
    if (r.kind == Kind::Movebox) {
        startBoxWalk(r, m.dir, m.pull);
    } else if (isTurn(r.kind)) {
        startTurn(r, m.dir);
    } else {
        startIceWalk(r, m.dir, m.walk);
    }
    ++r.applied;
    TwiliLog.info("[push] {} ({}): client {}'s {} replays, dir {}", kindName(r.kind),
        keyText(r.key), m.client, m.pull ? "pull" : "push", m.dir);
}

void snapTo(Record& r, const XZ& to) {
    ++r.snapped;
    if (isTurn(r.kind)) {
        const int step = turnStep(r);
        const int want = turnStepOf(to);
        // One step away: turn it, with its own sound and rotation.
        if (want == step + 1 || want == step - 1) {
            startTurn(r, want > step ? 0 : 1);
            TwiliLog.info("[push] {} ({}): turns one step to where a teammate left it",
                kindName(r.kind), keyText(r.key));
            return;
        }
        setTurnStep(r, want);
        TwiliLog.info("[push] {} ({}): set to step {}", kindName(r.kind), keyText(r.key), want);
        return;
    }
    if (r.kind == Kind::Movebox) {
        Box* b = boxOf(r);
        int gx, gz;
        gridOf(b, to, gx, gz);
        const int dgx = gx - b->field_0x8dc;
        const int dgz = gz - b->field_0x8e0;
        // One cell away: walk it, so collision, sound and Link's push-out stay the game's.
        for (int dir = 0; dir < 4; ++dir) {
            if (dgx == kDirX[dir] && dgz == kDirZ[dir]) {
                startBoxWalk(r, dir, false);
                TwiliLog.info("[push] movebox ({}): walks one cell to where a teammate left it",
                    keyText(r.key));
                return;
            }
        }
        b->field_0x8dc = gx;
        b->field_0x8e0 = gz;
        // mode_wait moves it there and saves the spot once it stands: sent, so a switch a lost
        // concurrent move of ours saved is put right for the team too.
        b->field_0x902 = 0;
        r.settleGuard = false;
        const XZ spot = gridXZ(b, gx, gz);
        const float half = std::max(boxAttr(b).mUnitDistance, 60.0f);
        pushOutPlayer(spot, half, b->current.pos.y, b->current.pos.y + boxAttr(b).field_0x68,
            b->home.angle.y);
        TwiliLog.info("[push] movebox ({}): snapped to cell {} {} ({:.0f} {:.0f})", keyText(r.key),
            gx, gz, spot.x, spot.z);
        return;
    }
    Ice* ice = iceOf(r);
    ice->current.pos.x = to.x;
    ice->current.pos.z = to.z;
    ice->old.pos.x = to.x;
    ice->old.pos.z = to.z;
    // Saves the spot and locks a solved block, as its own walk's end does.
    ice->mode_init_wait();
    pushOutPlayer(to, kIceHalf, ice->current.pos.y, ice->current.pos.y + kIceHeight, 0);
    TwiliLog.info("[push] iceblock ({}): snapped to {:.0f} {:.0f}", keyText(r.key), to.x, to.z);
}

// mode_wait does not recurse into another box's.
int s_boxMode = -1;
int s_boxGx = 0;
int s_boxGz = 0;
bool s_boxGuard = false;
fopAc_ac_c* s_walkBox = nullptr;
bool s_walkKeep = false;
uint8_t s_iceMode = 0xFF;
fopAc_ac_c* s_iceGuard = nullptr;

}  // namespace

void endWalk(Record& r) {
    if (r.localWalk) {
        r.localWalk = false;
        nlohmann::json packet = {{"type", "PUSH_REST"}};
        putSpot(packet, r, "from", r.localFrom);
        putSpot(packet, r, "pos", blockPos(r));
        if (sendPacket(packet, r)) {
            ++r.sent;
        }
    }
    if (r.remoteWalk) {
        r.remoteWalk = false;
        if (r.kind == Kind::Movebox) {
            r.settleGuard = true;
        }
    }
    if (r.actionMuted) {
        r.actionMuted = false;
        Ice* ice = iceOf(r);
        if (ice->mAction == Ice::ACTION_DEAD_e) {
            ice->setAction(Ice::ACTION_WAIT_e);
        }
    }
}

void sendMove(Record& r, const XZ& from, const XZ* to, int dir, bool pull, int walkType) {
    r.localWalk = true;
    r.localFrom = from;
    setOwnAuthority(r, from);
    nlohmann::json packet = {
        {"type", "PUSH_MOVE"},
        {"dir", dir},
        {"pull", pull},
    };
    putSpot(packet, r, "from", from);
    if (to != nullptr) {
        putSpot(packet, r, "to", *to);
    }
    if (r.kind == Kind::IceBlock) {
        packet["walk"] = walkType;
    }
    if (sendPacket(packet, r)) {
        ++r.sent;
        TwiliLog.info("[push] {} ({}): our {} sent, dir {}", kindName(r.kind), keyText(r.key),
            pull ? "pull" : "push", dir);
    }
}

XZ blockPos(const Record& r) {
    if (isTurn(r.kind)) {
        return turnSpot(turnStep(r));
    }
    if (r.kind == Kind::Movebox) {
        const Box* b = boxOf(r);
        if (b->field_0x5ac == 0) {
            return gridXZ(b, b->field_0x8dc, b->field_0x8e0);
        }
    }
    return {r.actor->current.pos.x, r.actor->current.pos.z};
}

bool blockAtRest(const Record& r) {
    if (r.localWalk || r.remoteWalk) {
        return false;
    }
    if (isTurn(r.kind)) {
        return !turnRotating(r);
    }
    if (r.kind == Kind::Movebox) {
        const Box* b = boxOf(r);
        // Standing on the ground, not afloat or falling.
        return b->field_0x5ac == 0 && (b->mBgc.mState & daObjMovebox::Bgc_c::STATE_1_e) != 0;
    }
    Ice* ice = iceOf(r);
    return ice->mMode == Ice::MODE_PROC_WAIT_e && ice->mAction == Ice::ACTION_WAIT_e &&
           !iceLocked(ice) && (ice->mState & Ice::STATE_2_e) != 0;
}

void queueMove(Record& r, const Move& m) {
    if (r.queue.size() >= kMaxQueue) {
        r.queue.pop_front();
    }
    r.queue.push_back(m);
    if (m.hasTo) {
        r.hasRemoteTarget = true;
        r.remoteTarget = m.to;
    }
}

void queueRest(Record& r, uint32_t, const XZ&, const XZ& pos) {
    r.hasRest = true;
    r.rest = pos;
    r.restAt = Clock::now();
    r.hasRemoteTarget = true;
    r.remoteTarget = pos;
}

void tickBlock(Record& r) {
    // A walk that ended outside its hooks (into water, say) ends here.
    if ((r.localWalk || r.remoteWalk) && !walking(r)) {
        endWalk(r);
    }
    const auto now = Clock::now();
    while (!r.queue.empty() && now - r.queue.front().at > kQueueTtl) {
        TwiliLog.info("[push] {} ({}): a teammate's move expired, our copy never stood still",
            kindName(r.kind), keyText(r.key));
        r.queue.pop_front();
    }
    if (r.hasRest && now - r.restAt > kQueueTtl) {
        r.hasRest = false;
    }
    if (!optionOn()) {
        r.queue.clear();
        r.hasRest = false;
        return;
    }
    if ((r.queue.empty() && !r.hasRest) || !blockAtRest(r)) {
        return;
    }
    const XZ cur = blockPos(r);
    const float tol = spotTolerance(r);
    if (!r.queue.empty()) {
        const Move m = r.queue.front();
        r.queue.pop_front();
        if (nearXZ(cur, m.from, tol)) {
            startReplay(r, m);
        } else if (m.hasTo && !nearXZ(cur, m.to, tol)) {
            snapTo(r, m.to);
        }
        // An ice block's end comes with its rest.
        return;
    }
    r.hasRest = false;
    if (!nearXZ(cur, r.rest, tol)) {
        snapTo(r, r.rest);
    }
}

void pushForTest(Record& r, int dir, bool pull) {
    if (isTurn(r.kind)) {
        pushTurnForTest(r, dir);
    } else if (r.kind == Kind::Movebox) {
        Box* b = boxOf(r);
        for (int i = 0; i < 4; ++i) {
            b->field_0x8f0[i] = i == dir ? 100 : 0;
        }
        b->field_0x8fe = 1;
        b->mPPLabel = static_cast<dBgW::PushPullLabel>(
            (pull ? dBgW::PPLABEL_PULL : dBgW::PPLABEL_PUSH) | dBgW::PPLABEL_4);
    } else if (r.kind == Kind::IceBlock) {
        Ice* ice = iceOf(r);
        for (int i = 0; i < 4; ++i) {
            ice->mCounter[i] = i == dir ? 1 : 0;
        }
        ice->mPPLabel = dBgW::PPLABEL_PUSH;
    }
}

nlohmann::json xzJson(const XZ& p) {
    return {{"x", std::round(p.x * 10.0f) / 10.0f}, {"z", std::round(p.z * 10.0f) / 10.0f}};
}

}  // namespace detail

using namespace detail;

void moveboxWaitPre(fopAc_ac_c* actor) {
    Record* r = recordOf(actor);
    if (r == nullptr) {
        return;
    }
    const Box* b = boxOf(*r);
    s_boxMode = b->field_0x5ac;
    s_boxGx = b->field_0x8dc;
    s_boxGz = b->field_0x8e0;
    // path_save after a teammate's move: their game saves it, ours stays quiet.
    s_boxGuard = r->settleGuard;
    if (s_boxGuard) {
        sync::RemoteApplyGuard::enter();
    }
}

void moveboxWaitPost(fopAc_ac_c* actor) {
    if (s_boxGuard) {
        sync::RemoteApplyGuard::leave();
        s_boxGuard = false;
    }
    Record* r = recordOf(actor);
    const int before = s_boxMode;
    s_boxMode = -1;
    if (r == nullptr) {
        return;
    }
    Box* b = boxOf(*r);
    if (r->settleGuard && (b->field_0x902 != 0 || b->field_0x5ac != 0)) {
        r->settleGuard = false;
    }
    // Our Link's push: replays start outside mode_wait.
    if (before == 0 && b->field_0x5ac == 1 && !r->remoteWalk && !r->localWalk) {
        const int dir = b->field_0x8e8;
        if (dir < 0 || dir > 3) {
            return;
        }
        const XZ from = gridXZ(b, s_boxGx, s_boxGz);
        const XZ to = gridXZ(b, s_boxGx + kDirX[dir], s_boxGz + kDirZ[dir]);
        const bool pull = (b->mPPLabel & dBgW::PPLABEL_PULL) != 0;
        sendMove(*r, from, &to, dir, pull, 0);
    }
}

void moveboxWalkPre(fopAc_ac_c* actor) {
    Record* r = recordOf(actor);
    if (r == nullptr || !r->remoteWalk) {
        return;
    }
    const daPy_py_c* player = static_cast<const daPy_py_c*>(dComIfGp_getPlayer(0));
    s_walkKeep = player != nullptr && player->checkNoResetFlg0(daPy_py_c::FLG0_PUSH_PULL_KEEP);
    s_walkBox = actor;
}

void moveboxWalkPost(fopAc_ac_c* actor) {
    if (s_walkBox == actor) {
        s_walkBox = nullptr;
        // The walk's end lets go of a push our Link never made on this block.
        daPy_py_c* player = static_cast<daPy_py_c*>(dComIfGp_getPlayer(0));
        if (s_walkKeep && player != nullptr) {
            player->onPushPullKeep();
        }
    }
    Record* r = recordOf(actor);
    if (r != nullptr && boxOf(*r)->field_0x5ac != 1 && (r->localWalk || r->remoteWalk)) {
        endWalk(*r);
    }
}

void iceWaitPre(fopAc_ac_c* actor) {
    Record* r = recordOf(actor);
    s_iceMode = r != nullptr ? iceOf(*r)->mMode : 0xFF;
}

void iceWaitPost(fopAc_ac_c* actor) {
    Record* r = recordOf(actor);
    const uint8_t before = s_iceMode;
    s_iceMode = 0xFF;
    if (r == nullptr) {
        return;
    }
    Ice* ice = iceOf(*r);
    if (before == Ice::MODE_PROC_WAIT_e && ice->mMode == Ice::MODE_PROC_WALK_e && !r->remoteWalk &&
        !r->localWalk && ice->mMoveDir >= 0 && ice->mMoveDir <= 3)
    {
        const XZ from{ice->mWalkInitPos.x, ice->mWalkInitPos.z};
        sendMove(*r, from, nullptr, ice->mMoveDir, false, ice->mWalkType);
    }
}

void iceWalkPre(fopAc_ac_c* actor) {
    Record* r = recordOf(actor);
    // The walk's end saves the spot in switches: their game does, ours stays quiet.
    if (r != nullptr && r->remoteWalk) {
        sync::RemoteApplyGuard::enter();
        s_iceGuard = actor;
    }
}

void iceWalkPost(fopAc_ac_c* actor) {
    if (s_iceGuard == actor) {
        sync::RemoteApplyGuard::leave();
        s_iceGuard = nullptr;
    }
    Record* r = recordOf(actor);
    if (r != nullptr && iceOf(*r)->mMode != Ice::MODE_PROC_WALK_e &&
        (r->localWalk || r->remoteWalk))
    {
        endWalk(*r);
    }
}

}  // namespace twili::push
