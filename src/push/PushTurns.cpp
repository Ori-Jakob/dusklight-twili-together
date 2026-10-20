// Turn switches and push doors: a teammate's turn plays here with the object's own rotation and
// ends on the same step. Their spot is the step, kept as x = step * kTurnScale.

#include "push/PushState.hpp"

#include "core/Log.hpp"
#include "sync/RemoteApplyGuard.hpp"

#include "d/actor/d_a_obj_lv6swturn.h"
#include "d/actor/d_a_obj_pdoor.h"
#include "d/actor/d_a_obj_swturn.h"
#include "d/actor/d_a_player.h"
#include "d/d_com_inf_game.h"

#include <cmath>

namespace twili::push {
namespace detail {
namespace {

using SwTurn = daObjSwTurn_c;
using Lv6Turn = daObjLv6SwTurn_c;
using PushDoor = daObjPDoor_c;

// d_a_obj_lv6swturn.cpp's mode enum.
constexpr u8 kLv6ModeRotate = 1;

}  // namespace

bool isTurn(Kind kind) {
    return kind == Kind::SwTurn || kind == Kind::Lv6SwTurn || kind == Kind::PushDoor;
}

int turnStep(const Record& r) {
    switch (r.kind) {
    case Kind::SwTurn: return static_cast<const SwTurn*>(r.actor)->field_0x5c4;
    case Kind::Lv6SwTurn: return static_cast<const Lv6Turn*>(r.actor)->unk5BC;
    case Kind::PushDoor: return static_cast<const PushDoor*>(r.actor)->mDoorDirection;
    default: return 0;
    }
}

// 0 turns toward the next step up, 1 toward the next step down.
int turnDir(const Record& r) {
    switch (r.kind) {
    case Kind::SwTurn: return static_cast<const SwTurn*>(r.actor)->field_0x5c0;
    case Kind::Lv6SwTurn: return static_cast<const Lv6Turn*>(r.actor)->unk5B8;
    case Kind::PushDoor: return static_cast<const PushDoor*>(r.actor)->field_0x5b0;
    default: return -1;
    }
}

bool turnRotating(const Record& r) {
    switch (r.kind) {
    case Kind::SwTurn: return static_cast<const SwTurn*>(r.actor)->mMode == SwTurn::MODE_ROTATE;
    case Kind::Lv6SwTurn: return static_cast<const Lv6Turn*>(r.actor)->mMode == kLv6ModeRotate;
    case Kind::PushDoor: return static_cast<const PushDoor*>(r.actor)->mMode == PushDoor::MODE_ROTATE;
    default: return false;
    }
}

XZ turnSpot(int step) {
    return {static_cast<float>(step) * kTurnScale, 0.0f};
}

int turnStepOf(const XZ& spot) {
    return static_cast<int>(std::lround(spot.x / kTurnScale));
}

// The rotation from the current step, as the object's own wait mode starts it (the switch it
// sets on the way reaches us from the turner's game).
void startTurn(Record& r, int dir) {
    switch (r.kind) {
    case Kind::SwTurn: {
        auto* t = static_cast<SwTurn*>(r.actor);
        t->field_0x5c0 = dir;
        t->field_0x5ba = 0;
        t->init_modeRotate();
        break;
    }
    case Kind::Lv6SwTurn: {
        auto* t = static_cast<Lv6Turn*>(r.actor);
        t->unk5B8 = dir;
        t->unk5B2 = 0;
        t->shape_angle.y = t->current.angle.y + t->unk5BC * 0x4000;
        t->init_modeRotate();
        break;
    }
    case Kind::PushDoor: {
        auto* t = static_cast<PushDoor*>(r.actor);
        t->field_0x5b0 = dir;
        t->field_0x5ba = 0;
        t->init_modeRotate();
        break;
    }
    default:
        return;
    }
    r.remoteWalk = true;
}

// Their wait modes set the angle from the step every tick.
void setTurnStep(Record& r, int step) {
    switch (r.kind) {
    case Kind::SwTurn: {
        auto* t = static_cast<SwTurn*>(r.actor);
        t->field_0x5c4 = static_cast<s16>(step);
        t->field_0x5ba = 0;
        break;
    }
    case Kind::Lv6SwTurn: {
        auto* t = static_cast<Lv6Turn*>(r.actor);
        t->unk5BC = static_cast<s8>(step);
        t->unk5B2 = 0;
        break;
    }
    case Kind::PushDoor: {
        auto* t = static_cast<PushDoor*>(r.actor);
        t->mDoorDirection = static_cast<s8>(step);
        t->field_0x5ba = 0;
        break;
    }
    default:
        break;
    }
}

// What our Link's push does to its counters (each one's charge time, without the first-push wait).
void pushTurnForTest(Record& r, int dir) {
    constexpr u8 kCharge = 0x23;
    dir &= 1;
    switch (r.kind) {
    case Kind::SwTurn: {
        auto* t = static_cast<SwTurn*>(r.actor);
        for (int i = 0; i < 2; ++i) {
            t->field_0x5b0[i] = i == dir ? kCharge : 0;
        }
        t->field_0x5af = 1;
        t->mPushPullLabel = dBgW_Base::PPLABEL_PUSH;
        break;
    }
    case Kind::Lv6SwTurn: {
        auto* t = static_cast<Lv6Turn*>(r.actor);
        for (int i = 0; i < 2; ++i) {
            t->unk5AE[i] = i == dir ? kCharge : 0;
        }
        t->unk5AD = 1;
        t->unk5C4 = dBgW_Base::PPLABEL_PUSH;
        break;
    }
    case Kind::PushDoor: {
        auto* t = static_cast<PushDoor*>(r.actor);
        for (int i = 0; i < 2; ++i) {
            t->field_0x5b6[i] = i == dir ? kCharge : 0;
        }
        t->field_0x5b5 = 1;
        t->mPushPullLabel = dBgW_Base::PPLABEL_PUSH;
        break;
    }
    default:
        break;
    }
}

}  // namespace detail

using namespace detail;

namespace {

int s_turnMode = -1;
fopAc_ac_c* s_rotateActor = nullptr;
bool s_rotateKeep = false;
bool s_rotateGuard = false;

}  // namespace

void turnWaitPre(fopAc_ac_c* actor) {
    Record* r = recordOf(actor);
    s_turnMode = r != nullptr ? (turnRotating(*r) ? 1 : 0) : -1;
}

void turnWaitPost(fopAc_ac_c* actor) {
    const int before = s_turnMode;
    s_turnMode = -1;
    Record* r = recordOf(actor);
    // Our Link's push: replays start outside the wait mode.
    if (r == nullptr || before != 0 || !turnRotating(*r) || r->remoteWalk || r->localWalk) {
        return;
    }
    const int dir = turnDir(*r);
    if (dir != 0 && dir != 1) {
        return;
    }
    const int step = turnStep(*r);
    const XZ from = turnSpot(step);
    const XZ to = turnSpot(dir == 0 ? step + 1 : step - 1);
    sendMove(*r, from, &to, dir, false, 0);
}

void turnRotatePre(fopAc_ac_c* actor) {
    Record* r = recordOf(actor);
    if (r == nullptr || !r->remoteWalk) {
        return;
    }
    s_rotateActor = actor;
    const daPy_py_c* player = static_cast<const daPy_py_c*>(dComIfGp_getPlayer(0));
    s_rotateKeep = player != nullptr && player->checkNoResetFlg0(daPy_py_c::FLG0_PUSH_PULL_KEEP);
    // The turn's end saves the step in switches: the turner's game does, ours stays quiet.
    sync::RemoteApplyGuard::enter();
    s_rotateGuard = true;
}

void turnRotatePost(fopAc_ac_c* actor) {
    if (s_rotateActor == actor) {
        s_rotateActor = nullptr;
        if (s_rotateGuard) {
            sync::RemoteApplyGuard::leave();
            s_rotateGuard = false;
        }
        // The turn's end lets go of a push our Link never made on it.
        daPy_py_c* player = static_cast<daPy_py_c*>(dComIfGp_getPlayer(0));
        if (s_rotateKeep && player != nullptr) {
            player->onPushPullKeep();
        }
    }
    Record* r = recordOf(actor);
    if (r != nullptr && !turnRotating(*r) && (r->localWalk || r->remoteWalk)) {
        endWalk(*r);
    }
}

}  // namespace twili::push
